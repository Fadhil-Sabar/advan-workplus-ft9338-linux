# Porting a Windows-Hello-only fingerprint reader to Linux

This project got a "Windows Hello only" USB fingerprint reader working on Linux
by running the vendor's own Windows matching engine natively — no Wine, no
reimplementation of the matcher. This document explains the method so you can do
the same for a *different* reader.

It is not a magic button. Expect real reverse-engineering. But the approach is
repeatable, and much of the hard part (the in-process Windows-DLL loader) is
reusable as-is from this repo.

---

## 0. Is your reader a candidate?

Windows fingerprint readers come in two shapes:

- **Match-on-chip (MOC).** The sensor matches internally; the host just ferries
  encrypted messages. These usually already have (or need) a protocol-style
  libfprint driver — look at `synaptics`, `goodixmoc`, `elanmoc`, `focaltech_moc`.
  *This guide is not for those.*
- **Match-on-host.** The sensor is basically a camera. It ships raw images to the
  PC, and a **vendor usermode "engine adapter" DLL** does the matching. Windows'
  Biometric Framework (WBF / "WinBio") loads that DLL. **This guide is for these.**

Quick tells you have a match-on-host reader:
- Its Windows driver package includes a DLL with `EngineAdapter` in the name, or
  a DLL that exports `WbioQueryEngineInterface`.
- The sensor is a cheap optical/capacitive module with no security silicon.

### Why the vendor engine, and not generic matching?

libfprint's default path (`FpImageDevice`) matches with **NBIS / bozorth3** —
*minutiae* matching. It extracts the little ridge features (endings,
bifurcations) into a point map and scores two prints by how well those maps
align. It's the industry-standard method, but it assumes a reasonably large,
detailed image. On a **tiny sensor** (this project's was 96×96) there simply
aren't enough clean minutiae to extract reliably, so matching becomes flaky —
that's the usual reason a small-sensor reader "doesn't really work" under
libfprint.

Rolling your own matcher is tempting but rarely pays off. A pixel-correlation
approach — e.g. averaging several frames into a template and comparing with
**NCC** (normalized cross-correlation) — sidesteps minutiae, but it's brittle to
finger placement, rotation, and pressure. We tried both NBIS *and* a homegrown
NCC matcher first; neither held up.

The underlying reason: a match-on-host reader was never meant to be matched by a
generic algorithm. The vendor ships a matcher **tuned for that specific little
sensor** and runs it in their DLL. So when the built-in and DIY routes fail,
reusing the vendor's engine is usually the only thing that gives reliable
matching — which is what the rest of this guide is about.

---

## 1. Get the Windows driver

Find your device's USB `VID:PID` (`lsusb`). Get its **signed Windows driver** from
the [Microsoft Update Catalog](https://www.catalog.update.microsoft.com) — search
the vendor name or the hardware ID (`USB\VID_xxxx&PID_xxxx`). Download the `.cab`
and unpack it (`cabextract`).

You are looking for:
- the **engine adapter DLL** (the matcher), and
- the **sensor/transport driver** (usually a UMDF `.dll`) — you don't run this,
  but it documents the USB protocol.

Read the **INF** file: it lists the sensor adapter, engine adapter, and storage
adapter. Microsoft ships inbox sensor + storage adapters; the vendor ships the
engine adapter (and often the transport driver). The engine adapter is your prize.

---

## 2. Vet the engine DLL

Before committing, check it's loadable in a plain loader:

```
objdump -x EngineAdapter.dll | grep 'DLL Name'      # what it imports
objdump -x EngineAdapter.dll | grep -i wbio         # exports WbioQueryEngineInterface?
strings EngineAdapter.dll | grep -i enclave         # VBS enclave?
```

Good signs:
- Exports `WbioQueryEngineInterface`.
- **No VBS enclave** — if it imports `CreateEnclave`/`CallEnclave` or loads a
  separate enclave module, the real matcher runs in a secure enclave you can't
  host, and this approach won't work. This is the one true dealbreaker.

The import list tells you which path you're on. **`kernel32`-only** (like this
repo's DLL) is the easy no-crypto case. If it also imports **`bcrypt`** (usually
alongside `advapi32`/`setupapi`), the engine runs an SDCP secure channel — still
workable, but you take the crypto path in **§3b** with `src/crypto_shims.c`.

---

## 3. Load the DLL in-process (reusable)

A **PE loader** does by hand what the Windows loader normally does when you launch
an `.exe` or load a `.dll`: it parses the file's **PE** (Portable Executable)
structure, copies its sections into memory at the right addresses, fills in the
functions it imports from other libraries, and jumps to its entry point — so a
Windows binary can run even though the OS underneath isn't Windows. A general PE
loader is a big job; here we only need to load one well-behaved DLL, so it stays
small.

`src/ft_engine.c` in this repo is a ~450-line loader that runs a `kernel32`-only
PE in-process on Linux. It is largely reader-agnostic — the reusable core is:

- map the PE at its preferred base;
- resolve its `kernel32` imports to small shim functions (heap, TLS, locale,
  critical sections, logging — mostly C-runtime boilerplate);
- install a minimal fake Windows **TEB** in the `%gs` segment (Windows uses `%gs`
  for thread-local state on x64; glibc uses `%fs`, so `%gs` is free);
- run the DLL's `DllMain` to initialise its static CRT;
- provide the shims the CRT actually calls.

**What the TEB / `%gs` step means.** On 64-bit Windows every thread has a *Thread
Environment Block* — a per-thread struct holding its stack bounds, thread-local
storage (TLS) pointer, PEB pointer, last-error value, and so on. Windows code
reads it through the `%gs` segment register (e.g. `gs:[0x58]` is "my TLS
pointer"), and the C runtime does this constantly. Linux has no TEB and uses the
*other* segment register, `%fs`, for its own thread-local storage — which leaves
`%gs` free. So the loader allocates a small block of memory, fills in just the
handful of fields the DLL actually reads, and points `%gs` at it with
`arch_prctl(ARCH_SET_GS, …)`. It's a hand-built stub, not the real ~1800-byte OS
structure, but enough that the DLL can't tell the difference for the parts it
touches. Because `%gs` is per-thread, you must re-arm it on every entry into the
engine (see the next point).

Two properties worth keeping:
- **W^X-safe loading.** Prepare the resolved image in a buffer, write it to an
  anonymous file (`memfd`), then map each section from that file: code
  read-execute, data read-write, never both. File-backed executable mappings are
  allowed under `MemoryDenyWriteExecute`, so the engine runs inside `fprintd`
  without weakening its systemd hardening.
- **Re-arm `%gs` on every entry**, and treat the engine as **process-global,
  loaded once** — never tear it down on device close. (`fprintd` re-opens the
  device between operations, sometimes on another thread; both facts bit us as
  crashes — see the git history.)

Start from this file and change only the parts that are vendor-specific (the
device geometry and, if your DLL imports functions ours didn't, a few more shims).

---

## 3b. If your engine ships crypto (an SDCP secure channel)

Some Windows Hello engines don't just import `kernel32` — they run Microsoft's
**Secure Device Connection Protocol (SDCP)**: at `Attach` they ECDH-handshake with
the sensor firmware, verify a device certificate chain against a Microsoft root,
and **encrypt each sample** between sensor and engine. Synaptics, Goodix, ELAN,
and EgisTec parts do this. A no-crypto engine like the FT9201's is the lucky
minority — so this repo also carries the crypto path, in **`src/crypto_shims.c`**
(an optional module, not built for the FT9201).

**The framing that makes it tractable: you own the process.** SDCP's crypto gates
exist to stop an *external* attacker tampering with the host engine. When your
loader *is* the host, every gate is neutralizable in-process. A crypto sensor
differs from a no-crypto one only by (a) a bcrypt-shim group and (b) an `Attach`
handshake that expects device I/O — both defeatable without a real device, **or**
replaceable with a real handshake against genuine silicon if you want attestation
to be true rather than bypassed.

**When the offline bypass is valid — verify by disassembly first.** It works only
if the engine is **match-on-host** and its extractor consumes a **plaintext** image
with no per-sample MAC/signature gating extraction. Trace the sample buffer out of
`AcceptSampleData`: if it flows through a `BCryptDecrypt` then straight into feature
extraction, a substituted plaintext sample is not cryptographically rejected and
the bypass is sound. If the extractor consumes session-key material *as data* (not
just as a checkable gate), the bypass fails and you must do the real handshake.

**The two hooks that carry the load** (in `crypto_shims.c`):
- `BCryptDecrypt` → **passthrough** — defeats the per-sample seal; the extractor
  gets the plaintext image.
- `BCryptVerifySignature` → **success** — defeats the `Attach`-time device
  cert-chain verify against the Microsoft root.

The remaining bcrypt shims just return success with **deterministic**, well-formed
outputs so the engine's init state machine proceeds — determinism matters so enroll
and verify see the same derived key material.

**Getting `Attach` to build a context with no device** (observed on one crypto
vendor; a template, not gospel):
- `pipeline[0]` must be a **nonzero device handle** — seed any nonzero value.
- `Attach` drives `DeviceIoControl` expecting **well-formed shapes**, not valid
  content (the verify is hooked). Seen: a cert read (~1580B), a nonce (~512B), a
  challenge (208/208). Return buffers of the requested length; content is irrelevant.
- The cert "parser" may not be crypto — one vendor's `StrStrIW`-searched the blob
  for the sensor **model string**. Embed the expected model string (UTF-16) in the
  cert-shaped buffer and the parser builds a valid descriptor. Check your engine's
  strings for the model tokens it looks for.
- If a real-silicon challenge-response gate remains and you've proven the session
  is unused by the matcher, force `Attach`'s success branch with a couple of 2-byte
  patches to the scratch image before it's mapped (preferred base, RVA == file
  offset, no relocation).

To wire it up: `#include "crypto_shims.c"` inside `ft_engine.c`, call
`register_crypto_shims()` from `register_shims()`, and bump `shims[128]` to `256`
(crypto sensors also pull in ~18 more `kernel32` plus `advapi32`/`setupapi`/etc.
imports). If you instead have the physical sensor and want real attestation, do the
genuine ECDH + cert verify + challenge-response rather than these bypasses.

---

## 4. Recover the WinBio engine ABI

The engine exposes a **`WINBIO_ENGINE_INTERFACE`** (documented in Microsoft's
`winbio_adapter.h`): `Attach`, `QueryPreferredFormat`, `AcceptSampleData`,
`CreateEnrollment` / `UpdateEnrollment` / `CommitEnrollment`, `VerifyFeatureSet`,
etc. You call these to enroll and verify.

You still have to recover, by disassembly, the concrete details the headers don't
pin down for *your* DLL:

- the **`WINBIO_PIPELINE`** layout it reads (where it keeps the engine context and
  the adapter callback tables — offsets differ per DLL);
- the exact **BIR / sample format** `AcceptSampleData` expects (image header
  fields: width, height, depth, and where the pixels start);
- the **storage-record** layout (`GetCurrentRecord` fields the matcher consumes).

`docs/how-it-works.md` and this repo's git history show the specific offsets we
recovered; yours will differ but the *shape* is the same. Tip: if the vendor also
ships a **Linux** blob (some do, as a "TOD" `.so`), it is usually unstripped and
disassembles far more cleanly than the Windows WDF driver — it was our Rosetta
Stone for both the ABI and the hardware init.

**Two facts that de-risk the next port** — both confirmed *identical* across two
unrelated vendors, so they are WINBIO-standard, not vendor-specific:
- The engine vtable slots at `iface+32+N*8` — **Attach=0, AcceptSampleData=8,
  VerifyFeatureSet=10, CreateEnrollment=12, UpdateEnrollment=13,
  GetEnrollmentStatus=14, CommitEnrollment=17** — and the storage-adapter slots held
  the same on both. Start from these; don't rediscover them from scratch.
- `build_bir` in this repo emits a **single** format pair, which lenient engines
  (FocalTech) accept. Stricter engines want the **full** WINBIO_BIR envelope: two
  format pairs (`hdr+0x28/0x2a` **and** `hdr+0x2c/0x2e`) plus a version-block offset
  at `+0x14`, or you get `E_INVALIDARG`. Emit the full envelope and it works on both.

---

## 5. Implement a storage adapter

The engine does not hold templates itself — it fetches and stores them through a
**storage-adapter callback vtable** referenced from the pipeline. For verify it
enumerates stored records; for enroll-commit it hands you a template blob. Back
this with a tiny in-memory store fed by your driver's template (a libfprint
`FpPrint`). See `ft_engine.c`'s storage stub.

---

## 6. Get the hardware talking (the device-specific part)

Everything above is about the matcher. You still need raw images out of the
sensor, which is entirely device-specific:

- USB init / power-up sequence;
- firmware upload, **if** the sensor's MCU has no resident firmware — and note
  that uploading firmware is not enough, you often must run a specific
  register-config sequence to *start* it (that stalled us for a while);
- image capture (finger detection, the bulk read).

Sources, best first:
1. a vendor **Linux** blob's disassembly (clean, symbol-rich);
2. an existing open reference driver for the same chip family;
3. the Windows **UMDF transport driver** (harder — WDF indirection);
4. a **USB capture** of the Windows driver working (USBPcap in a VM), as a last
   resort but ground truth.

---

## 7. Wire it into libfprint

Write an `FpDevice` driver (`src/ft9201.c` is a worked example):

- capture a frame;
- **match the engine's expected geometry** — feed it exactly the pixel dimensions
  it wants. Crop rather than non-uniformly scale; distorting ridge spacing wrecks
  matching (this was the difference between flaky and reliable for us);
- on enroll, feed frames to the engine and store the committed template blob in
  the `FpPrint`; on verify, load the blob and ask the engine to match.

Then distribute it as an **out-of-tree driver** (libfprint has no plugin ABI, so
you graft your driver onto a pinned libfprint at build time — not a fork), fetch
the proprietary blobs from public sources at build time, and install side-by-side
so the distro's libfprint is untouched. See `scripts/` and the main README.

---

## Gotchas we hit (so you don't)

- **The matcher is match-on-host; you must use the vendor DLL.** Don't try to
  reimplement it — that's the vendor's tuned IP.
- **Firmware: upload *and start* it.** Missing the post-upload config writes left
  the MCU dead with the firmware loaded but not running.
- **Feed the engine the size it's *tuned* for, not the sensor's native size.**
  The engine's alg config carries the dimensions it was designed around. Ours
  wants 64×80 even though the sensor is 96×96 — and while the DLL will *accept*
  the larger image (feature extraction succeeds), *matching* collapses (genuine
  fingers stop matching). Bigger is not better; reduce to the configured size.
- **Reduce by cropping, not uneven scaling.** Non-uniform scaling (e.g. 96→64
  wide, 96→80 tall) distorts ridge spacing and wrecks match reliability; a
  center-crop preserves geometry.
- **Engine state is global and `%gs`-based.** Load it once, re-arm `%gs` per call,
  and don't clear it on device close — or `fprintd` will segfault on the second
  operation.
- **Run W^X-safe** (file-backed executable pages) so you never have to disable
  `MemoryDenyWriteExecute` on `fprintd`.
- **Heap shims must survive a double-free — but don't "fix" it by leaking.** Routing
  `HeapFree`→`free` and `HeapReAlloc`→`realloc` (what the FT9201 loader here does) is
  correct and flat: FocalTech's CRT never double-frees. But some crypto engines' static
  CRTs realloc-then-free the same block, which aborts glibc. The tempting fix — a
  **leak-only** allocator (`HeapFree` = no-op) — works, but we *measured it leaking
  ~2.7 MB per verify*, which OOMs any daemon fast. Use a **tracked-idempotent**
  allocator instead: free for real (flat memory) but ignore double or foreign frees.
  Apply it to `Heap*` **and** `Local*`. (FT9201 needs none of this — real free is
  correct for it; this is only for a double-freeing crypto CRT.)
- **Drain large frame reads fully.** If a frame's length is encoded in the read
  command, read until the short packet. A fixed/short read leaves multi-KB residue in
  the pipe that wedges the *next* command — which looks exactly like a false "read
  budget" and invites needless retry/recycle logic. If a capture loop recycles on
  timeout, check it isn't papering over an undrained response.
- **Redistribution.** You cannot ship the vendor DLL or firmware. Fetch/extract
  them from existing public sources at build time.

---

## What's reusable from this repo

- `src/ft_engine.c` — the in-process, W^X-safe PE loader + `kernel32` shims +
  WinBio bridge + storage stub. The bulk of it is not FT9201-specific.
- `src/crypto_shims.c` — optional module extending the loader to **SDCP-crypto**
  sensors (the bcrypt-shim group + the own-the-process bypass). See §3b.
- `scripts/` — the fetch/extract/build/install pattern (out-of-tree driver on
  pinned libfprint, side-by-side install, no hardening disabled).

The FT9201-specific work lives in `src/ft9201.c` (USB + capture) and the exact
ABI offsets. Swap those for your device and the rest largely carries over.

---

## Resources & prior art (for the crypto path)

External tooling the no-crypto FT9201 route never needed, and starting points for
the next SDCP-crypto sensor:

- **Keystone: [`uunicorn/synaWudfBioUsb-sandbox`](https://github.com/uunicorn/synaWudfBioUsb-sandbox)**
  and its Wine fork [`uunicorn/wine`](https://github.com/uunicorn/wine) (the
  validity-sensor hacking branch). It loads a vendor UMDF2 biometric driver under a
  patched Wine and traces it with `WINEDEBUG` — how you lift the vendor command
  protocol with no Windows box. Originally for Synaptics/Validity sensors but
  vendor-agnostic enough to host other UMDF drivers with adaptation. libfprint
  maintainer **Marco Trevisan (3v1n0)** forked it and pointed us here.
- **usbmon** (`/sys/kernel/debug/usb/usbmon/`) for passive wire ground-truth —
  cross-check the harness traces, and trust it over language-binding reads on IN
  transfers (a bindings bug returned zeros).
- **WBF / WBDI ABI** — `WbioQueryEngineInterface` → the `WINBIO_ENGINE_INTERFACE`
  vtable (the matcher API §4 drives) and `WINBIO_BIR` (the sample envelope). Plus
  **UMDF2/WDF** background, **WDK headers** via NuGet
  (`microsoft.windows.sdk.cpp`, `microsoft.windows.wdk.x64`), and **CNG/BCrypt** —
  the crypto surface §3b shims.
- **SDCP** (Microsoft's Secure Device Connection Protocol) + **Microsoft Root CA
  2010** — the attestation model and the root the device cert chains to (what the
  `BCryptVerifySignature` hook skips).
- **RE tooling:** `capstone` + `pefile` (pip) — locate functions by string xref,
  trace decrypt→extract, prove no per-sample MAC gates extraction. Targeted enough
  that Ghidra isn't needed; `objdump`/`nm` for quick import/export views.
- **Prior art:**
  [`championswimmer/libfprint-eh577`](https://github.com/championswimmer/libfprint-eh577)
  — read it for the device protocol, not its recovery logic (its "read budget" was
  the undrained-frame misread; see the Gotcha above).
- **Why the vendor-matcher route exists at all:** 3v1n0's push to improve
  libfprint's *own* matching upstream is the flip side — generic minutiae matching
  hits a false-accept wall on tiny sensors, which is what drove both drivers to the
  vendor engine.

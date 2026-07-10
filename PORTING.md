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

If libfprint's built-in NBIS matcher does a bad job (common on small sensors),
reusing the vendor engine is usually the only way to get good matching.

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

Good signs (this repo's DLL had all of them):
- Imports **only `kernel32`** (plus maybe a couple of well-known DLLs).
- Exports `WbioQueryEngineInterface`.
- **No VBS enclave** — if it imports `CreateEnclave`/`CallEnclave` or loads a
  separate enclave module, the real matcher runs in a secure enclave you can't
  host, and this approach won't work.

---

## 3. Load the DLL in-process (reusable)

`src/ft_engine.c` in this repo is a ~450-line loader that runs a `kernel32`-only
PE in-process on Linux. It is largely reader-agnostic — the reusable core is:

- map the PE at its preferred base;
- resolve its `kernel32` imports to small shim functions (heap, TLS, locale,
  critical sections, logging — mostly C-runtime boilerplate);
- install a minimal fake Windows **TEB** in the `%gs` segment (Windows uses `%gs`
  for thread-local state on x64; glibc uses `%fs`, so `%gs` is free);
- run the DLL's `DllMain` to initialise its static CRT;
- provide the shims the CRT actually calls.

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
- **Redistribution.** You cannot ship the vendor DLL or firmware. Fetch/extract
  them from existing public sources at build time.

---

## What's reusable from this repo

- `src/ft_engine.c` — the in-process, W^X-safe PE loader + `kernel32` shims +
  WinBio bridge + storage stub. The bulk of it is not FT9201-specific.
- `scripts/` — the fetch/extract/build/install pattern (out-of-tree driver on
  pinned libfprint, side-by-side install, no hardening disabled).

The FT9201-specific work lives in `src/ft9201.c` (USB + capture) and the exact
ABI offsets. Swap those for your device and the rest largely carries over.

# How it works

The FT9201 is a 96×96 optical fingerprint sensor. That is a very small image,
and libfprint's built-in minutiae matcher (NBIS/bozorth3) does not discriminate
well on it. Rather than write a new matcher, this driver reuses **FocalTech's own
matching engine** — the same code Windows Hello uses — and runs it on Linux.

There are three moving parts:

```
  ┌─────────────┐   96×96 frames   ┌────────────┐   64×80 BIR   ┌──────────────────────┐
  │  ft9201.c   │ ───────────────► │ ft_engine  │ ────────────► │ ftWbioEngineAdapter  │
  │ (USB + SSM) │                  │ (PE loader)│               │  .dll  (the matcher) │
  └─────────────┘                  └────────────┘               └──────────────────────┘
    libfprint driver               in-process loader             FocalTech proprietary
```

## 1. The driver — `ft9201.c`

A normal libfprint `FpDevice` driver. It handles the USB side:

- **Bring-up.** The sensor's MCU (an 8051 core) has no resident firmware, so the
  driver uploads a firmware blob over a bulk endpoint. Getting the MCU to *run*
  that firmware needs a specific post-upload register sequence
  (`InitMcuConfig` + `SwitchNextSensorWorkMode`, recovered from FocalTech's own
  Linux driver): write AFE registers `0x01`, `0x41`, `0x30`, `0x54`, power-on
  registers `0x1f`/`0x1e`, then reset. Only after that does the MCU report ready
  (`0xa5 0x5a` on status register `0x20`).
- **Capture.** Polls for a finger, reads a 96×96 frame over a bulk transfer.
- **Enroll / verify.** Instead of libfprint's matcher, it hands each frame to the
  engine and asks it to build a template (enroll) or match one (verify). The
  resulting opaque template is stored in the libfprint `FpPrint`.

## 2. The loader — `ft_engine.c`

`ftWbioEngineAdapter.dll` is a self-contained x86-64 matcher: it imports only
`kernel32`, has no VBS-enclave dependency, and exposes a single
`WbioQueryEngineInterface` export returning a Windows Biometric Framework
(WBF/WinBio) v2 engine interface. `ft_engine.c` is a ~450-line loader that runs
it in-process, no Wine:

- Maps the PE at its preferred base and resolves its ~90 `kernel32` imports to
  small in-process shim functions (heap, TLS, locale, sync, logging — mostly
  C-runtime boilerplate).
- Installs a minimal fake Windows **TEB** in the `%gs` segment (Windows uses
  `%gs` for thread-local state on x64; glibc uses `%fs`, so `%gs` is free).
- Runs the DLL's `DllMain` to initialise its static C runtime, then drives the
  WinBio interface: `Attach` → `AcceptSampleData` → `CreateEnrollment` /
  `UpdateEnrollment` / `CommitEnrollment`, or `VerifyFeatureSet`.
- Provides a tiny in-memory **storage adapter** (the WinBio engine fetches and
  stores templates through a callback vtable), backed by whatever template
  libfprint hands it.

### W^X-safe by design

The loader never creates memory that is writable *and* executable. It prepares
the fully-resolved image in a scratch buffer, writes it to an anonymous file
(`memfd`), then maps each section from that file: code **read+execute**, data
**read+write**, never both. Executable file-backed mappings are permitted under
`MemoryDenyWriteExecute`, so the engine runs under `fprintd`'s default systemd
hardening — no security settings need to be relaxed.

## 3. Geometry

The engine expects a 64×80 image. The sensor delivers 96×96, so each frame is
**center-cropped** to 64×80 (not scaled — scaling 96→64 horizontally and 96→80
vertically distorts ridge spacing unevenly and hurts matching). The crop keeps
the central fingerprint region at native resolution.

## Firmware and DLL provenance

Both proprietary pieces come from existing public sources and are fetched, not
committed (see the README):

- `ftWbioEngineAdapter.dll` — FocalTech's signed Windows driver (Microsoft
  Update Catalog).
- MCU firmware — symbol `FOCALFP_9348_FW_APP` inside FocalTech's public Linux
  libfprint blob, which was also the reference for the boot sequence above.

The USB protocol was originally reverse-engineered by
[banianitc/ft9201-fingerprint-driver](https://github.com/banianitc/ft9201-fingerprint-driver).

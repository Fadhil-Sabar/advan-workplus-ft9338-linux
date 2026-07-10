# ftWbioEngineAdapter.dll — recovered ABI (2026-07-09)

All facts below are extracted from the binary (objdump disassembly + probe under Wine),
not guessed. File: `ftWbioEngineAdapter.dll`, PE32+ x86-64, ImageBase 0x180000000.

## Load / feasibility (de-risked)
- Imports **only KERNEL32.dll** (89 functions, all CRT/heap/thread boilerplate).
  No `winbio*.dll`, no crypto, no user32/advapi.
- **No VBS enclave.** The `enclave::` strings are a C++ namespace, not a trustlet:
  the DLL imports none of `CreateEnclave`/`LoadEnclaveData`/`CallEnclave` and loads
  no enclave module. The entire matcher (symbols `FtEnrollByTemplate`, `FtGetTemplate`,
  `FtFeatureCmp`, `FtGetMfsFeatures`, ...) runs in-process in this DLL.
- Single export: `WbioQueryEngineInterface(void **ppInterface)` → returns
  `WINBIO_ENGINE_INTERFACE*`. Verified under Wine: hr=0, **Version 2.0**, Type 2
  (engine), Size 216 = 23 populated function pointers. v2 = Windows 8 WBDI; no v3
  Windows-Hello extensions to emulate. AdapterId {BE076D74-0D8E-4C39-AB47-8C97743BE90C}.

## Engine interface vtable (offset 32 in the interface struct, 8 bytes each)
Order per winbio_adapter.h; all 23 non-null:
 0 Attach            1 Detach             2 ClearContext      3 QueryPreferredFormat
 4 QueryIndexVectorSize 5 QueryHashAlgorithms 6 SetHashAlgorithm 7 QuerySampleHint
 8 AcceptSampleData  9 ExportEngineData   10 VerifyFeatureSet  11 IdentifyFeatureSet
 12 CreateEnrollment 13 UpdateEnrollment  14 GetEnrollmentStatus 15 GetEnrollmentHash
 16 CheckForDuplicate 17 CommitEnrollment 18 DiscardEnrollment 19 ControlUnit
 20 ControlUnitPrivileged 21 NotifyPowerChange 22 Reserved_1

## WINBIO_PIPELINE offsets USED by this engine (empirical — vendor's own layout)
- **+0x08**: a HANDLE-like field; Attach checks `== INVALID_HANDLE_VALUE (-1)`.
- **+0x38**: **EngineContext** pointer.
  - Attach: if `*(pipeline+0x38) != NULL` → returns WINBIO_E_INVALID_DEVICE_STATE
    (0x8009800f). Else Attach allocates the context itself and stores it here.
  - The allocated EngineContext begins with signature dword **0x46311100**; other
    methods validate it via `cmpl $0x46311100,(ctx)` before use (see QuerySampleHint).
  => A harness passes a **zeroed pipeline**; Attach bootstraps its own context. We do
     not need to model the context internals, only give it a stable pipeline buffer.

## QueryPreferredFormat(pipeline, PWINBIO_REGISTERED_FORMAT std, PWINBIO_UUID vendor)
Writes fixed values (RVA 0x327e / 0x329c):
- **StandardFormat = 0x0401001B**  → `{ USHORT Owner = 0x001B (27); USHORT Type = 0x0401 (1025); }`
- **VendorFormat GUID = {7A23C065-7884-45A4-9F01-CBA7FBCB0179}** (copied from .rdata@0x83208)
This is the BIR data format the engine expects for AcceptSampleData / feature sets.

## QuerySampleHint(pipeline, out size)
Reads `ctx = *(pipeline+0x38)`, requires `*(uint32*)ctx == 0x46311100`, else E_INVALIDARG
(0x80070057). So the engine must be Attached first; sample hint comes from engine state.

## Return codes seen (sanity anchors)
0x80004003 E_POINTER · 0x80070057 E_INVALIDARG · 0x8009800f WINBIO_E_INVALID_DEVICE_STATE

## Sensor facts (reconcile with driver)
- Corpus captures in ~/.fp-corpus are **192×192** 8-bit PGM (P5), NOT the 96×96 the
  README claims. Reconcile: either the sensor was reconfigured or the README is stale.
- ftUsbWbioDriver.dll logs `sensor width = %d, height = %d` at capture time (transport
  DLL; we replace it with ft9201.c but it's the reference for capture geometry/IOCTL).

## AcceptSampleData(pipeline, PWINBIO_BIR sample, SIZE_T size, BYTE purpose, PWINBIO_REJECT_DETAIL* reject)
5 args (purpose in r9b, reject on stack). All ptr args non-null; size & purpose non-zero.
EngineContext = pipeline+0x38 (sig 0x46311100). Sets *reject = 0.
BIR is a standard **WINBIO_BIR**: three {ULONG Size; ULONG Offset;} blocks then formats:
  +0x00 HeaderBlock.Size   +0x04 HeaderBlock.Offset
  +0x08 StandardBlock.Size +0x0c StandardBlock.Offset
  +0x10 VendorBlock.Size   +0x14 VendorBlock.Offset
- HeaderBlock data (at BIR + HeaderBlock.Offset): WORD@+0x28 must == **0x001B**, WORD@+0x2a
  must == **0x0401** (the format owner/type from QueryPreferredFormat). Else 0x8009800c.
- StandardBlock data (at BIR + StandardBlock.Offset) — image record header, all little-endian:
  - WORD @+0x1c = HorizontalImageResolution
  - WORD @+0x1e = VerticalImageResolution
  - BYTE @+0x22 = pixel depth, **MUST == 8** ("Unsupported pixel depth %d" → 0x8009800c)
  - BYTE @+0x23 = compression algo, **MUST == 0** ("Unsupported image compression algorithm")
  - WORD @+0x2c = HorizontalLineLength (image **width**)
  - WORD @+0x2e = VerticalLineLength   (image **height**)
  - BYTE @+0x34 = impressionType
  - **+0x38 = start of raw 8-bit pixels**, width*height bytes (192×192 = 36864 for our corpus)
  Debug print @0x842e0 confirms field meanings.
- Internally: 0x66c0 = "set_current_image" (rejects on width/height mismatch vs alg config at
  EngineContext+0x24/+0x28, or poor quality → 0x80098008 WINBIO_E_BAD_CAPTURE, *reject=7);
  0x6400 = extract features (outputs quality/area bytes). So supplied width/height MUST match
  the sensor size the engine was configured with.
NOTE: fields read as native LITTLE-endian, not ANSI-381 big-endian — build LE u16s.

## Enrollment chain — engine accumulates internally, persists via STORAGE callback
- CreateEnrollment(pipeline): sig-check; acquires global lock (`WaitForSingleObject(g,INFINITE)`
  via IAT @0x60058, global handle @0xf6838); resets EngineContext+0x6 = 0 (sample count) and
  the internal template accumulator (0x60d0). Starts a fresh enroll.
- UpdateEnrollment(pipeline, PWINBIO_REJECT_DETAIL reject): requires active enroll (0x6390 !=0,
  else 0x8009800f). Calls 0x60f0 = "add current feature set to enroll template", logs
  "enroll return: %d". On poor merge sets *reject=2 and EngineContext+0x8=2. Repeat per sample.
- CommitEnrollment(pipeline, ...): sig-check; reads accumulated template; calls the **storage
  adapter** (pipeline+0x28 vtable) to persist the new record.

## VerifyFeatureSet — THE match call (9 params)
(pipeline, PWINBIO_IDENTITY id, BYTE subFactor, PBOOLEAN match, PULONG payloadSize,
 PUCHAR* payload, PULONG hashSize, PUCHAR* hash, PWINBIO_REJECT_DETAIL reject) — all non-null.
Zeroes every out; *match=FALSE. Guards against verify-during-enroll (0x80098007).
Acquires global lock, then **iterates the storage adapter** at pipeline+0x28:
  vtable+0x78 = FirstRecord/QueryBySubject,  vtable+0x90 = NextRecord,  vtable+0xa0 = GetRecord.
  (0x8009801f = NO_RESULTS, 0x80098020 = NO_MORE_RECORDS end the loop.)
For each stored template it runs the internal matcher (FtFeatureCmp / bz_match, threshold =
alg config "enroll_score_threshold"/"verify_quality_score", internal score in global
curMatchScore @~0xf7854). On a hit: copies payload+hash to the out params and sets **\*match=1**.
The WBDI contract exposes only the boolean match — the raw score stays internal (but is
readable at the curMatchScore global for harness instrumentation).

## ARCHITECTURE CONCLUSION (drives the whole integration)
The engine is NOT standalone: enroll persistence and verify candidate-fetch both go through a
**storage-adapter vtable at pipeline+0x28** (record iteration + add). CreateEnrollment also
touches a second table at pipeline+0x20 (methods +0x80/+0xb0). So the harness/bridge MUST
implement a minimal in-memory WINBIO storage adapter. This is the clean seam for libfprint:
our driver holds templates in libfprint's DB and exposes them to the engine as storage records.
  Enroll:  Attach → (AcceptSampleData → UpdateEnrollment) ×N → CommitEnrollment(→ our storage.Add)
  Verify:  Attach → AcceptSampleData → VerifyFeatureSet(→ our storage.First/Next/Get) → bool

## RUNTIME RESULTS — engine driven live on Linux under Wine (2026-07-09)
Harness: enroll-harness.go (Go, GOOS=windows) → build-harness.sh. Synthetic zeroed
pipeline (+0x08 = INVALID_HANDLE_VALUE, +0x38 = NULL), engine interface from the DLL.
- **Attach → S_OK.** EngineContext allocated, signature 0x46311100 confirmed live.
- QueryPreferredFormat → S_OK, std=0x0401001B (owner 0x1B, type 0x401) — matches disasm.
- QuerySampleHint → 7 (samples needed for enrollment).
- **Config geometry came back 0×0**: focal_CreateFocalConfigInfoInstance derives width/height
  from chip_type, which comes from HARDWARE (sensor adapter). No sensor in the synthetic
  pipeline → chip_type 0 → 0×0. The engine imports only kernel32 (no advapi32) so config is
  NOT registry-based. We supply geometry by poking EngineContext+0x24/+0x28 post-Attach.
- **GEOMETRY IS 64×80.** With geometry poked to 64×80 and a corpus PGM downscaled to 64×80,
  **AcceptSampleData → S_OK, reject 0 — real feature extraction succeeded.** At 192×192 the
  engine CRASHES (process dies mid-call). 64×80 is the matcher's native input; the disasm's
  hardcoded cmp 0x40/0x50 was correct. (192×192 corpus captures are the wrong geometry.)
- **Enroll path runs on real prints:** CreateEnrollment S_OK; per sample AcceptSampleData S_OK
  + UpdateEnrollment. Samples accumulate returning WINBIO_I_MORE_DATA (0x00090001); after
  ~7 the update returns S_OK; feeding repeats then yields WINBIO_E_BAD_CAPTURE reject=2 —
  the engine's duplicate-area detection working. GetEnrollmentHash = E_NOTIMPL (unused).
  => The algorithm genuinely analyzes fingerprint content and tracks coverage.

### BLOCKER for the match go/no-go: need real captures
Completing enrollment (Status → S_OK) needs ~7 DISTINCT touches per finger covering new
finger area. The corpus has only 3 per finger, captured at the wrong 192×192 geometry and
downscaled (square→4:5 distorts). So the genuine-vs-impostor VerifyFeatureSet test can't run
on current data. Need: recapture ~8-10 distinct touches per finger. Open hardware question —
what does the sensor actually report at reg 0x14/0x15? If native is 64×80, ft9201.c capturing
192×192 is itself wrong; if native is larger, downscale-to-64×80 (as the harness does) is the
right pre-matcher step but should preserve the true aspect ratio.

## NATIVE LOADER — engine runs with NO WINE (2026-07-09)
loader.c (build-loader.sh): pure C, ~380 lines, gcc, no deps. Maps the PE at its
preferred base 0x180000000 (MAP_FIXED_NOREPLACE), copies sections, resolves the 87
kernel32 imports to in-process MS-ABI shims, sets up TLS + a fake x64 TEB installed
in %gs (glibc uses %fs on x86-64, so %gs is free), runs the static-CRT DllMain, then
calls the exports. **Confirmed: WbioQueryEngineInterface, Attach, AcceptSampleData all
return S_OK natively.** This proves the shipping path — the libfprint driver embeds this
loader, no Wine runtime. Every engine call/callback crosses the ABI via __attribute__((ms_abi)).
Shims route OutputDebugStringA/WriteFile/WriteConsoleW to stderr, so we get the engine's
own [Engine] telemetry live.

### Engine internal telemetry (revealed by native logging) — algorithm v1.3.3
Full alg config the engine loads (compiled defaults; these are the tuning knobs):
  chip_type=3 (=FT9361; the 93a9 maps to the FT9361 profile, native 64×80),
  width=64 height=80, algorithm_mode=1, enroll_duplicate_area_check_enable=1,
  algorithm_max_templates=32, enroll_max_templates=18 (max_tpl=18 sub_tpl=32),
  enroll_score_threshold=100, image_quality_score=30, verify_quality_score=30,
  valid_area_scale=0.75, verify_level=15, update_level=16, dirty_finger_enable=1.
Per-sample AcceptSampleData now prints REAL metrics, e.g. on a corpus print downscaled
to 64×80: **quality:72 (thr 30 → pass), area:100, humility:58**. purpose arg: 1 =
WINBIO_PURPOSE_VERIFY (use the enroll purpose value for enrollment samples).
=> We can read per-sample quality/area directly — instrumentation for the go/no-go is free.
NB: "Sensor Size 0×0" is the sensor-adapter query (no HW in harness); alg-config width/height
are 64×80 as compiled. EngineContext+0x24/+0x28 still come from the (absent) sensor, so we
poke them 64×80 post-Attach — unchanged.

## STORAGE-ADAPTER STUB wired + validated (2026-07-09)
loader.c now builds the pipeline+0x28 storage vtable (0x20 header + slots) and wires
pipeline+0x20 (secondary) and +0x28. Verified offsets/signatures from disassembly:
  +0x68 AddRecord(pipe, rec, subFactor)
  +0x78 QueryBySubject(pipe, identity*, subFactor)
  +0x90 FirstRecord(pipe)   +0x98 NextRecord(pipe)   +0xa0 GetCurrentRecord(pipe, rec*)
WINBIO_STORAGE_RECORD fields the engine touches: +0x00 Identity*, +0x20 TemplateBlob,
+0x28 TemplateBlobSize (matcher consumes +0x20/+0x28). Identity compare in Verify reads
Identity+0x04 (type/value) then memcmp Identity+0x08 (GUID/SID body).
TEST (empty store): VerifyFeatureSet → engine called our **QueryBySubject** (logged),
got NO_RESULTS (0x8009801f), returned hr=0x80098005 match=0 cleanly, no crash. Engine
error log leaked its source: PBEngineAdapter.cpp:1635. => storage ABI seam PROVEN.

## STATUS: all software pieces built + validated; only real capture data remains
Loader(no Wine) ✓  Attach ✓  feature-extraction-on-real-prints ✓  enroll-accumulate ✓
storage-vtable+callbacks ✓  Verify-drives-storage ✓. The genuine-vs-impostor match test
just needs: enroll enough DISTINCT captures (engine wants up to 18 sub-templates) →
CommitEnrollment (fires AddRecord, stored in our stub) → VerifyFeatureSet genuine vs
impostor. Then read match bool + curMatchScore (global ~0x1800f7854) for separation.

## END-TO-END MATCH TEST — offline corpus only (2026-07-09)
loader.c now runs full enroll->commit->verify natively. IMPORTANT: this used the STORED
~/.fp-corpus files (captured 2026-04-22), NOT live hardware — no sensor touched.
Method per finger A: enroll with A/1 + A/2 (cycled; commit succeeds despite MORE_DATA
status), hold out A/3, then verify A/3 (genuine) + all other fingers' 1/2/3 (impostors).
CommitEnrollment fires AddRecord with a ~209KB template (up to 18 sub-tpls); our storage
stub returns it on verify. Results (4 enrolled fingers: right-index, left-thumb,
right-middle, left-index): genuine held-out sample MATCHED every time (4/4);
impostors 0/108 falsely accepted (all correctly rejected). Engine logs "Match!"/"No Match".

WHAT THIS PROVES: the whole software stack works natively end-to-end, and the matcher
cleanly SEPARATES different fingers even on downscaled 192->64x80 distorted images.
WHAT IT DOES NOT PROVE: robustness to real-world variation. All samples per finger are
from ONE April session (A/3 ~ A/1 in placement — quality 72 vs 69, so similar not
identical), so the genuine side is an easy test. Impostor rejection (0/108) is the more
robust signal. Real go/no-go for FRR still needs a live session with deliberate
repositioning/pressure/rotation across independent presentations.
(Bug note: the "score" printed = template-size global 0xf7854, not curMatchScore; the
match booleans are the real WBDI output. Find the true score global next.)

## Still to do (next session)
1. Map the exact vtable layouts at pipeline+0x20 and +0x28 (which WINBIO_STORAGE_INTERFACE /
   sensor-adapter slots correspond to +0x78/+0x90/+0xa0/+0x80/+0xb0) — disassemble the callers
   fully + cross-ref the public WINBIO_STORAGE_INTERFACE ordering.
2. Build the harness: zeroed pipeline + our engine interface + a tiny in-memory storage stub;
   Attach; feed a corpus PGM as a BIR; enroll 2-3 prints per finger; VerifyFeatureSet genuine
   vs impostor across ~/.fp-corpus. Record boolean match + curMatchScore separation.
   That separation is the go/no-go for the whole revival.

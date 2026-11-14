# Phase-0 virtual-camera transport selection

This evidence-only spike evaluates replacement architectures after the B–D
handoff failures. It does not contain product code and must not merge into
`main`. Executable evidence belongs on `spikes`; only the measured verdict and
accepted product constraints return to `main`.

## Fixed product gate

- Windows 11 build 22000 or newer.
- One standard virtual-camera device and one active consumer.
- NV12 1920×1080 at 30fps.
- At least 29.5 delivered fps.
- Producer-to-client frame-age p95 no more than 33.3ms.
- Zero malformed, torn, or marker-error frames.

A reduced format, driver, proprietary runtime, or external bridge is a product
fallback, not a pass against this gate.

## Staged candidates

The retained probe tree is organized by architecture rather than vendor:

1. `capabilities/`: adapter visibility and D3D12 cross-adapter heap/resource
   support (G1–G3).
2. `mf-shared-buffer/`: shared-memory-backed custom Media Foundation buffers and
   source-owned slot leasing (M1/M2).
3. `activation-topology/`: in-process, local-server, and surrogate COM source
   activation (A0–A2).
4. `codec-bridge/`: direct compressed subtype negotiation and internal
   encode/decode bridges (M4/M5/M7/M8).
5. `source-local/`: minimal allocator-owned source-local rendering (S1).
6. `compatibility/`: diagnostic and real-application gates for survivors.

A candidate advances only after its preceding kill gate passes. Unknown results
are `not-tested`; they are never inferred as passes.

## Current capability probe

`transport_cross_adapter_probe` enumerates DXGI adapters in the interactive
session, resolves the Frame Server allocator LUID recorded by Spike D, queries
D3D12 cross-node support, and—when both adapters are visible—tests:

- a shared cross-adapter buffer with a shared cross-adapter fence and validated
  producer-GPU → consumer-GPU → readback copy;
- an R8 row-major cross-adapter texture with validated copy;
- NV12 row-major cross-adapter texture creation and import capability.

The default consumer LUID is the measured Spike D allocator LUID
`00000000:000178d8`. Override it only when a fresh source-side trace records a
different value.

## Build

```powershell
powershell -NoProfile -ExecutionPolicy Bypass `
  -File scripts/build-transport-selection.ps1
```

## Run the non-elevated capability stage

```powershell
powershell -NoProfile -ExecutionPolicy Bypass `
  -File scripts/run-transport-selection.ps1 `
  -Stage CrossAdapterCapability
```

Outputs remain under `build/transport-selection-results/`. Build products and
raw machine logs are not retained as evidence until reviewed for machine
identifiers and converted into the strict final summary.

## M1/M2 shared-buffer result

The M1 first-frame gate passed with a media-source sample backed directly by a
shared ring slot. The buffer implements `IMFMediaBuffer` and `IMF2DBuffer2`;
the producer/source lease protocol prevents slot reuse until Frame Server
releases the sample.

Run the two M2 ownership modes from elevated PowerShell:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass `
  -File scripts/run-shared-buffer-handoff.ps1 `
  -Mode ProducerOwned -Frames 1000 -MinimumFps 25 -MaximumP95Ms 33.3

powershell -NoProfile -ExecutionPolicy Bypass `
  -File scripts/run-shared-buffer-handoff.ps1 `
  -Mode SourceOwned -Frames 1000 -MinimumFps 25 -MaximumP95Ms 33.3
```

Both modes delivered 1,000 valid frames without a full-frame CPU copy before
Frame Server. Producer-owned measured 28.798fps and 33.569ms p95;
source-owned measured 28.819fps and 33.428ms p95. Both are below the unchanged
29.5fps gate and above the strict 33.3ms limit, so M2 is `NO-GO`. Recovery,
application, and soak checks were not run after those kill gates. The strict result and
zero-copy counters are retained in
`results/shared-buffer-handoff.json`.

## A1/A2 activation-topology result

The A1 local-server capability gate registered and started a COM local server,
but virtual-camera activation failed before the first frame with
`REGDB_E_CLASSNOTREG` (`0x80040154`). The A2 surrogate gate delivered two
valid frames, but source instrumentation identified the activation image as
`C:\WINDOWS\System32\svchost.exe`, not `dllhost.exe`: the `DllSurrogate`
registration was ignored and the existing in-process Frame Server topology was
unchanged. Neither candidate advances to the 1,000-frame gate.

Reproduce the two elevated kill gates with:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass `
  -File scripts/run-shared-buffer-handoff.ps1 `
  -ActivationTopology LocalServer -Frames 2

powershell -NoProfile -ExecutionPolicy Bypass `
  -File scripts/run-shared-buffer-handoff.ps1 `
  -ActivationTopology Surrogate -Frames 2
```

The strict result is retained in `results/activation-topology.json`.

## M7/M8 compressed-subtype result

The M7 H.264 elementary-stream and M8 MJPEG source types both failed before
the first frame. `IMFActivate::ActivateObject` returned
`MF_E_INVALIDMEDIATYPE` (`0xC00D36B4`) for each source, so Frame Server did not
expose an accepted decoded NV12 mode. The Annex-B H.264 and JPEG fixtures were
generated from the same deterministic NV12 marker frame with the external
FFmpeg 9.0.2 probe tool; FFmpeg is not a SparkFace runtime dependency.

Reproduce the elevated kill gates with:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass `
  -File scripts/run-shared-buffer-handoff.ps1 `
  -CompressedMode H264 -Frames 1 -MaximumP95Ms 1000000000000

powershell -NoProfile -ExecutionPolicy Bypass `
  -File scripts/run-shared-buffer-handoff.ps1 `
  -CompressedMode Mjpeg -Frames 1 -MaximumP95Ms 1000000000000
```

The strict result is retained in `results/compressed-subtypes.json`.

## Final decision

No tested Wave-1 candidate passed the unchanged 1080p30 transport gates. The
closest result, M2 source-owned slots, missed both throughput and latency:
28.819fps and 33.428ms p95 against 29.5fps minimum and 33.3ms maximum. No
transport primary, technical fallback, or product fallback is selected.
Recovery, real-application, soak, and integrated face/beauty checks were not
run because no candidate survived its preceding hard gate. The v1
implementation plan remains blocked. See `results/summary.json`.

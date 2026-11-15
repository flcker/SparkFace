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

## Reduced-mode product characterization

After the unchanged 1080p30 gate rejected every Wave-1 candidate, the closest
M2 source-owned design is characterized separately at four explicit product
fallback modes. These results do not retroactively pass the 1080p30 gate.

| Mode | Minimum fps | Measured fps | Maximum p95 | Measured p95 | Skipped | Outcome |
|---|---:|---:|---:|---:|---:|---|
| NV12 1280×720 at 30fps | 29.5 | 28.853 | 33.3ms | 34.106ms | 37 | `NO-GO` |
| NV12 1920×1080 at 24fps | 23.5 | 23.664 | 41.667ms | 35.686ms | 13 | `NO-GO` |
| NV12 1920×1080 at 15fps | 14.5 | 14.992 | 66.667ms | 35.547ms | 0 | `PASS` |
| NV12 1280×720 at 24fps | 23.5 | 23.654 | 41.667ms | 34.576ms | 13 | `NO-GO` |

Each mode restarts Frame Server, then must deliver 1,000 frames with zero
skipped, duplicate, malformed, or torn markers and no full-frame CPU copy
before Frame Server. Run all four gates from elevated PowerShell:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass `
  -File scripts/run-reduced-mode-characterization.ps1
```

The runner retains per-mode raw logs under
`build/transport-selection-results/reduced-modes/` and emits a reviewable JSON
result at `build/transport-selection-results/reduced-modes.json`. The reviewed
strict result is retained in `results/reduced-modes.json`.

Only 1080p15 passed all automated gates: 1,000 valid frames, 14.992fps,
35.547ms p95, zero sequence or marker errors, and no full-frame CPU copy before
Frame Server. It is a product-fallback candidate, not an accepted product
constraint. Recovery, real-application, soak, and integrated checks remain
unrun until that reduced frame-rate constraint is explicitly accepted.

## Request-credit pacing characterization

The request-credit variant removes the producer's independent timer. Each
Media Foundation `RequestSample` call publishes one credit with its entry QPC;
the producer emits the corresponding frame only after that credit arrives.
Stop and flush explicitly cancel any final unused credit, and the strict result
requires every published shared buffer to be released.

Frame Server and the interactive producer run in different Windows sessions.
The probe therefore uses a globally named auto-reset event with a null DACL.
That permissive event and the public mapped-file path are evidence-only choices,
not an acceptable product transport or security design.

Run all four 1,000-frame gates from elevated PowerShell:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass `
  -File scripts/run-request-paced-characterization.ps1
```

The runner retains per-mode logs under
`build/transport-selection-results/request-paced/` and emits the strict result
at `build/transport-selection-results/request-paced.json`. Recovery,
real-application, and soak gates run only if a mode passes this automated gate.

All four runs delivered 1,000 valid frames with zero skipped, duplicate, or
marker-error frames and zero full-frame CPU copies before Frame Server. Each
run recorded 1,001 requests: 1,000 publications plus one explicit shutdown
cancellation, with all 1,000 shared buffers released and no slot pressure.

| Mode | Minimum fps | Measured fps | Maximum p95 | Measured p95 | Skipped | Outcome |
|---|---:|---:|---:|---:|---:|---|
| NV12 1920×1080 at 30fps | 29.5 | 30.209 | 33.3ms | 48.327ms | 0 | `NO-GO` |
| NV12 1280×720 at 30fps | 29.5 | 30.235 | 33.3ms | 47.909ms | 0 | `NO-GO` |
| NV12 1920×1080 at 24fps | 23.5 | 30.243 | 41.667ms | 48.619ms | 0 | `NO-GO` |
| NV12 1280×720 at 24fps | 23.5 | 30.233 | 41.667ms | 47.968ms | 0 | `NO-GO` |

Request pacing removed the earlier throughput and sequence-skip failures, but
every mode failed its unchanged frame-age p95 gate. Frame Server continued to
issue about 30 requests per second for the advertised 24fps modes; the strict
result retains that observed contract behavior rather than relabeling it as
24fps pacing. No mode advances to recovery, application, or soak testing. The
reviewed result is retained in `results/request-paced.json`.

## S1 source-local render

S1 removes the app-to-source frame transport. The retained Windows-Camera
patch connects `IMFVideoSampleAllocatorEx` to a source-local D3D11 device on
the Intel adapter. For each `RequestSample`, the media source renders a
deterministic moving BGRA marker and converts it with the D3D11 video processor
directly into the allocator-owned NV12 texture. No full-frame CPU copy occurs
before Frame Server.

Run the clean build, temporary registration, 1,000-frame consumer gate, trace
validation, and registration cleanup from elevated PowerShell:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass `
  -File scripts/run-source-local-render.ps1
```

`-KeepInstalled` retains the probe registration only for manual recovery and
application checks. The default path always restores the prior registration
and removes the probe flag in `finally`. The source trace records the hosting
process/session, allocator adapter LUID, and one sparse GPU-conversion
checkpoint per 100 frames; the runner rejects missing or stale checkpoints.

The final strict run passed all automated gates:

| Frames | Measured fps | Render-to-client p50 | p95 | p99 | Maximum | Sequence/marker errors | Pre-Frame-Server CPU copies | Outcome |
|---:|---:|---:|---:|---:|---:|---:|---:|---|
| 1,000 | 30.271 | 9.525ms | 12.415ms | 13.694ms | 17.947ms | 0 | 0 | `PASS` |

The reviewed measurement and conditional-check inventory are retained in
`results/source-local-render.json`.

## S1 H9 productization gate

The source-local productization runner uses protocol v2: a versioned shared
control block owned by an interactive-session producer and read by the
LocalService media source. It carries the exact physical-camera symbolic link,
validates the fixed source asset before use, rejects protocol or asset
mismatches to effect value zero, detects producer process exit immediately,
and treats a live producer heartbeat as stale after five seconds. The probe
publishes that heartbeat on an above-normal-priority worker. Writes are
restricted to ordinary interactive users while restricted and low-integrity
tokens are denied.

Run the elevated H9 gate with:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass `
  -File scripts/run-source-local-productization.ps1 `
  -Frames 120 -MinimumFps 29.5 -MaximumP95Ms 33.3
```

| Scenario | Frames | Measured fps | Render-to-client p95 | Result |
|---|---:|---:|---:|---|
| Normal v2 control | 120 | 30.295 | 11.249ms | `PASS` |
| Protocol mismatch fail-safe | 120 | 30.335 | 10.803ms | `PASS` |
| Applied-to-producer-dead transition | 120 | 30.303 | 12.452ms | `PASS` |
| Producer recovery | 120 | 30.330 | 12.017ms | `PASS` |
| Frame Server restart | 120 | 30.447 | 12.212ms | `PASS` |
| Side-by-side v2 activation | 120 | 30.330 | 12.025ms | `PASS` |

All scenarios had zero sequence, duplicate, or marker errors and zero
pre-Frame-Server full-frame CPU copies. The ACL probe denied restricted and
low-integrity writers while allowing a non-administrator interactive writer.
The runner restored the retained COM registration and removed mode files,
control files, and staged DLLs. H9 is `PASS`; the strict 1,000-frame S1
regression above passed after the productization changes. Detailed evidence is
retained in `results/source-local-productization.json`.


The tested conditional subset also passed:

- Frame Server restart, a 30-second closed interval, and client reopen.
- OBS Studio 33.0.0 preview, a 1,362-frame 1920×1080 recording at 30fps,
  reconnect, and a 10-minute soak.
- Tencent Meeting 3.47.2.405 preview, reconnect, and a 10-minute soak.

Zoom, Microsoft Teams, Discord, and DingTalk were not installed on the
measurement machine. Sleep/resume, privacy-toggle, logoff/login, and integrated
face/beauty were not tested. H9 product control, fail-safe security, restart,
side-by-side update, and cleanup gates passed. S1 therefore remains an
automated-pass candidate rather than a selected transport.

## S1 H8A physical-camera capture

H8A upgrades the source-local probe from a deterministic shader to the single
expected `Integrated Camera`. Protocol v2 publishes its exact Media Foundation
symbolic link. The LocalService media source excludes the SparkFace virtual
camera, opens the selected device on the allocator D3D11 adapter, requires
GPU-backed BGRA samples, overlays the retained marker, and converts directly
into allocator-owned NV12. The final probe uses one bounded MTA capture thread
so physical reads never depend on the media-source delivery work queue.

Reproduce the elevated 1,000-frame kill gate with:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass `
  -File scripts/run-source-local-physical-camera.ps1 `
  -Frames 1000 -MinimumFps 29.5 -MaximumP95Ms 33.3
```

The retained RCA runner separately proves standalone capture, capture-only,
copy-only, and full delivery in that order:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass `
  -File scripts/run-source-local-physical-camera-rca.ps1 `
  -Frames 1000 -MinimumFps 29.5 -MaximumP95Ms 33.3
```

H8A remains `NO-GO`, but the retained reason is corrected. Standalone capture
delivered 1,000 contiguous frames at 29.525fps without device removal. The
staged RCA proved stable capture and GPU-copy lifetimes; it also found that a
transient heartbeat-stale control frame was tearing down and reopening the
camera. Retaining the sample until GPU copy completion and allowing one stale
heartbeat frame removed the historical device-removal, service-exception, and
watchdog failures.

The final integrated run delivered 1,000 contiguous, physically sourced
frames with zero sequence, marker, physical-content, or pre-Frame-Server CPU
copy errors. It reached 29.327fps and 50.167ms capture-to-client p95, missing
both the 29.5fps minimum and 33.3ms p95 maximum. The source scene had only a
3–8 luma range before conversion, so the physical-content gate now rejects
only a zero-range frame instead of misclassifying valid low-contrast input.
H8A therefore does not advance to client-reopen, application, or soak checks.
The runner restores the previous COM registration in `finally` and removes
all staged artifacts. Detailed evidence is retained in
`results/source-local-physical-camera.json`.

## Final decision

No tested Wave-1 app-to-source transport passed the unchanged 1080p30 gates.
Request-credit pacing restored throughput and sequence correctness but failed
the 33.3ms p95 latency gate. S1 deterministic source-local rendering passed the
automated 1080p30 gate, H9 protocol-v2 productization, and the available
recovery, OBS, and Tencent Meeting subset. Corrected H8A evidence proves that
Frame Server-hosted physical capture is functionally stable, but its integrated
29.327fps and 50.167ms p95 still fail the contract. The previously tested
source-adapter handoff failed at 46.001ms p95, so neither source-local nor
app-to-source integration provides a technical fallback.

Reduced-mode characterization separately retains M2 source-owned NV12
1920×1080 at 15fps as an unaccepted product-fallback candidate. No transport
primary or technical fallback is selected, and the v1 implementation plan
remains blocked. See `results/summary.json`,
`results/source-local-render.json`, `results/source-local-productization.json`,
`results/source-local-physical-camera.json`, `results/source-adapter-handoff.json`,
`results/reduced-modes.json`, and `results/request-paced.json`.

# Phase-0 Spike C face-landmark probe

This retained evidence probe selects a commercially usable face-landmark model
and measures ONNX Runtime DirectML performance on the Intel Iris Xe reference
machine. It is not product code and must not be merged into `main`.

## Scope

- Preserve the original 478-point and 468-point landmark-only comparison.
- Retest the lighter 468-point model with its symbolic batch dimension fixed to
  one, as recommended for DirectML execution.
- Measure a complete static-image face stage on every frame: RGB detector
  preprocessing, BlazeFace inference, SSD decode and weighted NMS, roll-aligned
  ROI preprocessing, Face Mesh inference, and landmark mapping.
- Run DirectML on adapter 0 with ONNX Runtime CPU EP fallback disabled.
- Confirm landmark placement on a pinned CC0 real-face crop.

Temporal tracking, camera capture, and the v1 render pipeline remain outside
this probe. The complete-stage gate deliberately runs detection on every frame;
it does not rely on a tracker or reduced detector cadence.

## Pinned environment

- Windows 11 build 22000 or newer
- MSVC x64 Release build
- ONNX Runtime DirectML 1.24.4
- DirectML device 0
- Intel Iris Xe reference GPU, driver `32.0.101.7092`

Model, detector, test-image, license, normalization, tensor, and acquisition
details are in [`models/models.json`](models/models.json) and
[`models/README.md`](models/README.md). External binaries are downloaded under
ignored `build/face-models/` and are not committed.

## Reproduce

From the repository root in Windows PowerShell 5.1 or PowerShell 7:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\spikes\face\fetch-models.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\build-phase0-face.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\run-phase0-face.ps1
```

The runner performs 100 warm-up and 1,000 measured frames for both the fixed-N
landmark-only gate and the complete face-stage gate. Logs and overlays are
written under ignored `build/face-results/`.

## Gates

The selected path must satisfy all of these conditions:

1. Detector and landmark sessions use `DmlExecutionProvider` on device 0 with
   `session.disable_cpu_ep_fallback=1`.
2. Detector `batch` and landmark `N` symbolic dimensions are fixed to one.
3. 100 warm-up frames complete before exactly 1,000 measured frames.
4. All detector and landmark outputs are finite; detector and landmark-presence
   scores are at least 0.5.
5. Complete per-frame detector-to-landmark p95 is at most 8.0 ms on Iris Xe.
6. The generated real-face overlay visibly aligns eyes, nose, mouth, and face
   contour.

## Measurement discipline

Close GPU-heavy applications and run the two modes serially. Preserve the
complete `key=value` output. Do not compare debug builds, CPU EP runs, remote
sessions, or concurrent model runs with the final gate.

## Verdict

**GO: select MediaPipe Face Mesh 468 plus the MediaPipe BlazeFace short-range
detector for v1.** Both sessions remained DirectML-only. The complete pipeline
ran detection on every measured frame and passed the unchanged 8.0 ms p95 gate.

| Measurement | Warm-up / measured | p50 | p95 | p99 | Max | Frames >8 ms | Gate |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | --- |
| 468-point landmark only, original dynamic batch | 10 / 100 | 3.2740 ms | 9.8847 ms | 11.9494 ms | 12.1189 ms | not recorded | Fail |
| 468-point landmark only, fixed `N=1` | 100 / 1,000 | 1.7951 ms | 2.2347 ms | 2.4852 ms | 3.0989 ms | 0 | Pass |
| BlazeFace → ROI → 468-point Face Mesh, fixed batch | 100 / 1,000 | 4.9066 ms | 5.4926 ms | 5.8469 ms | 6.2241 ms | 0 | Pass |

The quality-first 478-point candidate remains rejected at its measured p95 of
15.5720 ms. The complete-stage overlay passed visual review with eyes, nose,
mouth, and contour aligned. Full structured evidence is in
[`results/summary.json`](results/summary.json).

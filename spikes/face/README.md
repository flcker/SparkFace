# Phase-0 Spike C face-landmark probe

This retained evidence probe selects a commercially usable face-landmark model
and measures ONNX Runtime DirectML inference on the Intel Iris Xe reference
machine. It is not product code and must not be merged into `main`.

## Scope

- Compare the pinned 478-point quality candidate with the pinned 468-point
  lower-compute fallback.
- Run DirectML on adapter 0 with ONNX Runtime CPU EP fallback disabled.
- Measure inference only. PPM loading, face-ROI resize/normalization, landmark
  decode, and overlay writing happen outside the measured loop.
- Confirm landmark placement on a pinned CC0 real-face crop.

The probe does not implement face detection, temporal tracking, camera capture,
or the v1 render pipeline. Both candidates require a face ROI supplied by a
separate detector.

## Pinned environment

- Windows 11 build 22000 or newer
- MSVC x64 Release build
- ONNX Runtime DirectML 1.24.4
- DirectML device 0
- Intel Iris Xe reference GPU for the final verdict

Model, test-image, license, normalization, tensor, and acquisition details are
in [`models/models.json`](models/models.json) and
[`models/README.md`](models/README.md). Models and images are downloaded under
ignored `build/face-models/`; no external binary is committed.

## Reproduce

From the repository root in Windows PowerShell 5.1 or PowerShell 7:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\spikes\face\fetch-models.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\build-phase0-face.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\run-phase0-face.ps1
```

`fetch-models.ps1` verifies every source SHA-256 before use and writes a
deterministic cropped P6 PPM for visual validation. `run-phase0-face.ps1` runs
candidates serially and stores logs and overlays under ignored
`build/face-results/`.

## Gates

Each candidate must satisfy all of these conditions:

1. Session creation succeeds with `DmlExecutionProvider` on device 0 and
   `session.disable_cpu_ep_fallback=1`.
2. Ten warm-up inferences complete before at least 100 measured inferences.
3. All landmark outputs are finite and the measured-frame count is exact.
4. Inference p95 is at most 8.0 ms on the Iris Xe reference machine.
5. The generated real-face overlay places the mesh on the face with eyes,
   nose, mouth, and contour visibly aligned.

The primary is selected only if it passes every gate. If the primary fails but
the fallback passes, the fallback becomes the selected v1 candidate. If both
fail, Spike C is NO-GO and no model is selected.

## Measurement discipline

Close GPU-heavy applications and run candidates one at a time. Preserve the
complete `key=value` output. Do not compare debug builds, CPU EP runs, remote
sessions, or concurrent model runs with the final gate. Record the GPU and
driver observed on the measurement machine in `results/summary.json`.

## Verdict

**NO-GO; no model selected.** Measured on Intel Iris Xe driver
`32.0.101.7092` with ONNX Runtime DirectML 1.24.4, device 0, and CPU EP
fallback disabled. Both overlays passed the pinned real-face quality review,
but both candidates failed the 8.0 ms p95 gate.

| Candidate | Role | Landmarks | p50 | p95 | p99 | Peak working set | Quality | Latency gate |
| --- | --- | ---: | ---: | ---: | ---: | ---: | --- | --- |
| MediaPipe Face Landmarker | Primary | 478 | 11.5629 ms | 15.5720 ms | 16.6337 ms | 207,331,328 B | Pass | Fail |
| MediaPipe Face Mesh | Fallback | 468 | 3.2740 ms | 9.8847 ms | 11.9494 ms | 182,472,704 B | Pass | Fail |

Each result used 10 warm-up and 100 measured inference-only frames. The
fallback's median fits the nominal budget, but its p95 does not; selecting it
would weaken the stated gate after measurement. Full structured evidence is in
[`results/summary.json`](results/summary.json).

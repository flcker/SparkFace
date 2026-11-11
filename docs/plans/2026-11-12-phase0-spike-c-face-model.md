# Spike C — DirectML face-model selection

## Verdict

**NO-GO; no face-landmark model selected.** Both tested models produced sensible
landmarks on the pinned real-face crop, but both exceeded the `8.0ms` inference
p95 gate on the Intel Iris Xe reference GPU. The lighter model's passing median
does not override its failed tail-latency gate.

This result adds a second independent blocker to the v1 build plan. Resolving
the failed B–B4 handoff architecture alone is insufficient: a replacement face
model or an explicitly revised face-ML budget also requires validation.

## Baseline

| Item | Tested value |
| --- | --- |
| Date | 2026-11-12 |
| GPU | Intel Iris Xe, driver `32.0.101.7092` |
| Runtime | ONNX Runtime DirectML `1.24.4` |
| Provider | `DmlExecutionProvider`, device 0 |
| CPU EP fallback | Disabled with `session.disable_cpu_ep_fallback=1` |
| Build | MSVC x64 Release, C++17 |
| Timing scope | `Ort::Session::Run` only; preprocessing and overlay generation excluded |
| Samples | 10 warm-up frames, then 100 measured frames per candidate |
| Gate | Finite outputs, exact frame count, p95 `<=8.0ms`, sensible real-face overlay |

Executable evidence is retained on `spikes` under `spikes/face/` by
[evidence PR #9](https://github.com/flcker/SparkFace/pull/9). ONNX models,
test-image binaries, generated overlays, logs, and build products remain out of
Git; acquisition metadata pins URLs, asset IDs, byte sizes, SHA-256 values, and
license evidence.

## Candidates and provenance

Both candidates come from
`yakhyo/mediapipe-face-mesh-onnx@3bdf1e0d100a4c709766c051a0c9e9cdab1fed62`.
The pinned source identifies the shipped ONNX model weights and MediaPipe mesh
topology as Apache-2.0. The evidence manifest retains that statement, the exact
Apache-2.0 text, and the weight lineage from Google's MediaPipe assets. Models
with code-only permissive licenses or non-commercial pretrained weights were
rejected.

The quality-first candidate is the 478-point MediaPipe Face Landmarker at
`256x256`; it adds ten iris points. The lighter fallback candidate is the
468-point MediaPipe Face Mesh model at `192x192`.

## Measured results

| Candidate | Role | Input | p50 | p95 | p99 | Maximum | Peak working set | Quality | Latency |
| --- | --- | --- | ---: | ---: | ---: | ---: | ---: | --- | --- |
| MediaPipe Face Landmarker, 478 points | Primary | RGB `256x256` | `11.5629ms` | `15.5720ms` | `16.6337ms` | `17.1283ms` | `207,331,328` B | Pass | Fail |
| MediaPipe Face Mesh, 468 points | Fallback | RGB `192x192` | `3.2740ms` | `9.8847ms` | `11.9494ms` | `12.1189ms` | `182,472,704` B | Pass | Fail |

Every measured output was finite, and both candidates completed exactly 100
frames. The primary missed the p95 gate by `7.5720ms`; the fallback missed it by
`1.8847ms`.

## Quality check

The acquisition script verifies a CC0-1.0 Metropolitan Museum portrait and
crops the documented `480x480` square face ROI before creating a P6 RGB input.
Both generated overlays aligned the eyes, nose, mouth, and face contour. The
478-point overlay also localized both irises. The visual gate therefore passed
for both candidates and did not affect the NO-GO decision.

## Decision

Do not select either candidate for `core/face/` and do not weaken the p95 gate
after observing the result. A future proposal must change and validate at least
one failed constraint: model architecture, execution path, hardware/performance
promise, or the accepted face-ML budget. Until then, the detailed v1 build plan
remains blocked alongside the B–B4 transport failures.

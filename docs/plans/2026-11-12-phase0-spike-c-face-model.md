# Spike C — DirectML face-model selection

## Verdict

**GO: select the 468-point MediaPipe Face Mesh model with the MediaPipe
BlazeFace short-range detector for v1.** Fixing both symbolic batch dimensions
to one removed the original DirectML tail-latency failure. The complete
per-frame detector-to-landmark stage measured `5.4926ms` p95 against the
unchanged `8.0ms` gate on the Intel Iris Xe reference GPU.

The quality-first 478-point Face Landmarker remains rejected at its original
`15.5720ms` p95. Spike C no longer blocks the v1 build plan; unresolved
transport NO-GOs remain independent blockers.

## Environment and gate

| Item | Tested value |
| --- | --- |
| Retest date | 2026-11-15 |
| GPU | Intel Iris Xe, driver `32.0.101.7092` |
| Runtime | ONNX Runtime DirectML `1.24.4` |
| Provider | `DmlExecutionProvider`, device 0 |
| CPU EP fallback | Disabled with `session.disable_cpu_ep_fallback=1` |
| Build | MSVC x64 Release, C++17 |
| Samples | 100 warm-up frames, then 1,000 measured frames per retest mode |
| Gate | Finite outputs, exact frame count, complete-stage p95 `<=8.0ms`, sensible real-face overlay |

Executable retest evidence is retained on `spikes` under `spikes/face/` by
[evidence PR #19](https://github.com/flcker/SparkFace/pull/19). The original
NO-GO evidence remains in [evidence PR #9](https://github.com/flcker/SparkFace/pull/9).
ONNX models, test-image binaries, generated overlays, logs, and build products
remain out of Git; acquisition metadata pins URLs, asset IDs, byte sizes,
SHA-256 values, and license evidence.

## Models and provenance

The selected detector and landmark model come from
`yakhyo/mediapipe-face-mesh-onnx@3bdf1e0d100a4c709766c051a0c9e9cdab1fed62`:

- MediaPipe BlazeFace short-range detector, RGB `128x128`, Apache-2.0.
- MediaPipe Face Mesh, 468 points, RGB `192x192`, Apache-2.0.

The evidence manifest retains explicit model-weight license evidence, exact
release asset IDs, hashes, tensor profiles, and the weight lineage from
Google's MediaPipe assets. Models with code-only permissive licenses or
non-commercial pretrained weights remain rejected.

## Measured results

The original 468-point run used the graph's symbolic batch dimension and failed
at `9.8847ms` p95 across 100 measured landmark-only calls. The fixed-batch
retest produced:

| Measurement | Timed scope | p50 | p95 | p99 | Maximum | Frames >8 ms | Gate |
| --- | --- | ---: | ---: | ---: | ---: | ---: | --- |
| 468-point Face Mesh, fixed `N=1` | `Ort::Session::Run` landmark inference only | `1.7951ms` | `2.2347ms` | `2.4852ms` | `3.0989ms` | 0 | Pass |
| BlazeFace → ROI → Face Mesh, fixed batch | RGB detector preprocessing, detector inference, SSD decode and weighted NMS, roll-aligned ROI preprocessing, landmark inference, full-image mapping | `4.9066ms` | `5.4926ms` | `5.8469ms` | `6.2241ms` | 0 | Pass |

Both modes completed exactly 1,000 measured frames with finite outputs. The
complete stage ran face detection on every frame; it did not rely on temporal
tracking or a reduced detector cadence.

## Quality check

The acquisition script verifies a CC0-1.0 Metropolitan Museum portrait and
crops the documented `480x480` square fixture. The complete detector-to-mesh
overlay visibly aligned eyes, nose, mouth, and face contour. Detector confidence
was `0.908732`; landmark presence was `1.0`.

## Decision

Use BlazeFace short-range plus the 468-point Face Mesh model as the v1
`core/face/` baseline. Production integration must preserve fixed batch size
one, DirectML device selection, disabled CPU EP fallback, pinned normalization
and ROI geometry, and the unchanged 8 ms face-stage p95 gate. This probe selects
the model path; it does not validate camera capture, temporal tracking, beauty
quality, or the unresolved virtual-camera transport architecture.

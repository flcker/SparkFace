# Spike C face-landmark candidates

This directory retains **metadata and license evidence only**. Models and image
files are acquired into ignored `build/face-models/`; nothing binary belongs
under `spikes/`.

```powershell
.\spikes\face\fetch-models.ps1
```

The script is idempotent: a present file is reused only after SHA-256
verification. A mismatched existing file fails with its expected and actual
digests; use `-Force` only to replace it. Release downloads use the pinned
asset names and record the GitHub asset IDs; SHA-256 enforces exact content.
The repository source, README, and license are pinned to commit
`3bdf1e0d100a4c709766c051a0c9e9cdab1fed62`.

`models.json` is strict JSON and is the machine-readable contract for the
harness. Consume `profile.input_index`, `profile.input.width`,
`profile.input.height`, lowercase `profile.input.layout` and
`profile.input.channel_order`, three-element `profile.input.scale` and
`profile.input.bias`, `profile.landmark_count`, `profile.dimensions`,
`profile.coordinate_mode`, and `profile.landmark_output_index` directly. Do
not parse the descriptive fields to configure inference.

## Candidates

| Role | Model | Input | Landmark output | Rationale |
| --- | --- | --- | --- | --- |
| Primary | `face_landmarker_Nx3x256x256.onnx` | `NCHW` RGB, `256x256`, value / 255 | `landmarks`: 478 x 3 crop-pixel coordinates; `score`: raw presence logit | Quality-first dense mesh, including ten iris points. The pinned source reports 113 MMac. |
| Fallback | `face_mesh_Nx3x192x192.onnx` | `NCHW` RGB, `192x192`, value / 255 | `landmarks`: 468 x 3 crop-pixel coordinates; `score`: raw presence logit | Lighter dense mesh. The pinned source reports 35 MMac. |

Both candidates are landmarks-only. The caller supplies one roll-aligned square
face ROI, using a detector-box long side expanded by 1.5x, then resizes it to
the candidate input. The output x/y values are in that resized crop's pixels;
z uses the same scale. Apply sigmoid to `score` before a presence threshold.
No performance conclusion is recorded here: DirectML/Iris Xe timing belongs to
the serialized Spike C measurement.

## Weight license, not merely code license

The candidate release is from
[`yakhyo/mediapipe-face-mesh-onnx`](https://github.com/yakhyo/mediapipe-face-mesh-onnx)
at the pinned commit above. Its pinned README identifies the listed ONNX files
as the shipped models and states in **License and attribution** that the model
and mesh topology are Apache-2.0. It also documents the model-weight lineage
from Google's MediaPipe assets. Its exact root
[LICENSE](https://raw.githubusercontent.com/yakhyo/mediapipe-face-mesh-onnx/3bdf1e0d100a4c709766c051a0c9e9cdab1fed62/LICENSE)
is retained as
[`licenses/yakhyo-mediapipe-face-mesh-onnx-Apache-2.0.txt`](licenses/yakhyo-mediapipe-face-mesh-onnx-Apache-2.0.txt).

This is explicit weight evidence for these two ONNX assets, not an inference
from a permissive converter alone. The manifest also records the immutable
source URLs, GitHub asset IDs, source commit, byte sizes, and SHA-256 values.
InsightFace Landmark106 and arbitrary conversion/mirror assets are rejected:
a permissive code repository does **not** make a pretrained weight commercially
usable, and the relevant InsightFace weights are non-commercial.

## Overlay still

The script downloads the CC0-1.0 Metropolitan Museum portrait
`Elderly Man; Full Face` from its
[pinned Wikimedia Commons revision](https://commons.wikimedia.org/w/index.php?title=File:-Elderly_Man%3B_Full_Face-_MET_37.14.52.jpg&oldid=824076779).
The source JPEG's SHA-256 is recorded in `models.json`. The 602x768 portrait is
**not** fed directly to the quality gate: acquisition deterministically crops
`crop_xywh: [61, 60, 480, 480]` before writing
`elderly-man-full-face.ppm` (P6 RGB). This square preserves the face and upper
head (approximately source x=170..420, y=130..440) with contextual margin,
which matches the landmark models' face-ROI requirement.

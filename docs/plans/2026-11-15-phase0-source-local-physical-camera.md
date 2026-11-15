# H8A — source-local physical-camera capture verdict

## Verdict

**NO-GO: reject the measured Frame Server-hosted physical-camera path on
throughput and latency.** The corrected source sustained 1,000 contiguous
physical-camera frames through allocator-owned GPU conversion with zero
sequence, marker, physical-content, or pre-Frame-Server CPU-copy errors. It
reached only 29.327fps and 50.167ms capture-to-client p95 against the unchanged
29.5fps and 33.3ms gates.

Executable evidence is retained on `spikes` under
`spikes/transport-selection/`. [Evidence PR #22](https://github.com/flcker/SparkFace/pull/22)
contains the original probe and [RCA evidence PR #24](https://github.com/flcker/SparkFace/pull/24)
corrects its causal diagnosis. Probe and build changes remain evidence-only and
do not enter `main`.

## Unchanged gate

The path had to expose the existing standard Windows 11 virtual camera and
produce NV12 1920×1080 at 30fps for one active consumer with:

- at least 29.5 delivered fps;
- capture-to-client p95 no more than 33.3ms;
- zero malformed, skipped, duplicate, reordered, or content-invalid frames;
- zero full-frame CPU copies before Frame Server;
- client-reopen and Frame Server restart passes before application and soak
  testing.

No CPU fallback, reduced mode, external bridge, or different product contract
was accepted for this gate.

## Corrected boundary

H9 control protocol v2 publishes the exact Media Foundation physical-camera
symbolic link from the interactive session. The LocalService media source
excludes the SparkFace virtual camera, creates the camera Source Reader on the
allocator D3D11 adapter, requires GPU-backed BGRA samples, overlays the retained
marker, and converts directly into allocator-owned NV12.

The RCA separated standalone capture, capture-only, GPU-copy-only, and full
media-source stages. It found that the original probe released each Media
Foundation sample before the asynchronous D3D11 copy completed. Retaining the
sample and texture until a copy-completion query succeeds removed the historical
device-removal, service-exception, and watchdog symptoms. The probe also
retained capture across one transient live-producer heartbeat lapse, published
heartbeats on an above-normal-priority thread, and corrected a physical-content
test that rejected the measured low-contrast camera scene.

These were probe lifetime, scheduling, and validation defects. They were not
evidence of a fundamental recursive physical-camera limitation.

## Measured result

| Run | Observed result | Gate |
| --- | --- | --- |
| Standalone physical-camera capture | 1,000 frames at 29.525fps; zero skipped sequences; device-removal reason `S_OK`; sampled luma range 3–11 | Pass |
| Frame Server capture-only stage | 1,000 frames at 29.334fps; 52.279ms p95; zero skipped or duplicate sequences | Diagnostic pass |
| Frame Server GPU-copy-only stage | 1,000 frames at 29.338fps; 52.260ms p95; zero skipped or duplicate sequences | Diagnostic pass |
| Frame Server full diagnostic stage | 1,000 frames at 29.356fps; 50.662ms p95; zero skipped, duplicate, or physical-content errors | Diagnostic pass |
| H8A final strict gate | 1,000 frames at 29.327fps; 50.167ms p95; zero skipped, duplicate, marker, physical-content, or pre-Frame-Server CPU-copy errors | Fail: fps and p95 |
| H9 protocol-v2 regression, six 120-frame scenarios | 30.295–30.447fps; 10.803–12.452ms p95; zero sequence, duplicate, marker, or pre-Frame-Server copy errors | Pass |

The final strict run completed instead of stalling. Its correctness and
GPU-resident pre-Frame-Server path passed; its throughput and latency did not.
Client reopen, Frame Server restart, real applications, soak, face ML, and
beauty integration were not run after those two kill gates failed.

## Decision

S1 deterministic source-local rendering remains evidence that an
allocator-owned shader can meet the transport-only budget. H8A now proves that
physical-camera integration is functional, while also proving that this path
does not meet the product's 1080p30 performance contract. The external
source-adapter handoff is not a fallback: its retained result also exceeded the
33.3ms latency gate at 46.001ms p95.

Therefore:

- **Transport primary:** none.
- **Technical fallback:** none.
- **Product fallback:** none accepted. M2 source-owned NV12 1920×1080 at 15fps
  remains only an unaccepted candidate.
- **Product readiness:** blocked.

The detailed v1 build plan must not be written until an explicit product or
architecture decision changes a failed constraint and the revised path retains
new end-to-end evidence.

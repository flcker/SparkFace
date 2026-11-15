# H8A — source-local physical-camera capture verdict

## Verdict

**NO-GO: reject physical-camera capture from the Frame Server-hosted custom
media source.** The source initialized the exact selected camera and completed
an initial allocator-owned GPU conversion, but it could not sustain recursive
camera reads in the Frame Server process.

Executable evidence is retained on `spikes` under
`spikes/transport-selection/` by
[evidence PR #22](https://github.com/flcker/SparkFace/pull/22). The probe and
build changes remain evidence-only and do not enter `main`.

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

## Tested boundary

H9 control protocol v2 publishes the exact Media Foundation physical-camera
symbolic link from the interactive session. The LocalService media source
excludes the SparkFace virtual camera, creates the camera Source Reader on the
allocator D3D11 adapter, requires GPU-backed BGRA samples, overlays the retained
marker, and converts directly into allocator-owned NV12.

The final probe used one bounded MTA capture thread. This removed the initial
work-queue dependency between media-source delivery and asynchronous camera
callbacks. A single in-flight sample prevented unbounded buffering and added no
per-frame thread or heap allocation. CPU-backed samples and adapter mismatch
remained hard failures.

## Measured result

| Run | Observed result | Gate |
| --- | --- | --- |
| H9 protocol-v2 regression, six 120-frame scenarios | 30.195–30.374fps; 10.337–12.341ms p95; zero sequence, marker, or pre-Frame-Server copy errors | Pass |
| H8A final 120-frame kill gate | Camera initialization `S_OK`; protocol v2 and first GPU conversion observed; then `DXGI_ERROR_DEVICE_REMOVED` (`0x887A0005`), repeated `WAIT_TIMEOUT` (`0x80070102`), and the 45-second watchdog | Fail |
| H8A alternate strict reruns | Consumer received Frame Server service exception `0x80070428` | Fail |
| H8A two-frame diagnostic | 2/2 frames at 2.391fps; 17.643ms p95; 2 physical-content errors; zero marker or sequence errors; zero pre-Frame-Server full-frame CPU copies | Fail |

The different terminal symptoms share the same boundary: capture starts inside
the Frame Server host but does not remain a valid 1080p30 physical-camera
stream. The first conversion is not a first-frame architecture pass because the
next reads either stall, remove the D3D device, or fail through the service.

Client reopen, Frame Server restart, real applications, soak, face ML, and
beauty integration were not run after the kill gate failed.

## Decision

S1 deterministic source-local rendering remains useful evidence that an
allocator-owned shader can meet the transport-only budget. H8A proves that it
does not provide a viable physical-camera input path. The external
source-adapter handoff is not a fallback: its retained 1,000-frame result was
30.093fps but 46.001ms producer-to-client p95 against the 33.3ms gate.

Therefore:

- **Transport primary:** none.
- **Technical fallback:** none.
- **Product fallback:** none accepted. M2 source-owned NV12 1920×1080 at 15fps
  remains only an unaccepted candidate.
- **Product readiness:** blocked.

The detailed v1 build plan must not be written until an explicit product or
architecture decision changes a failed constraint and the revised path retains
new end-to-end evidence.

# Phase-0 virtual-camera transport selection verdict

**Date:** 2026-11-14  
**Verdict:** **NO-GO — no transport selected**  
**Evidence:** [PR #13](https://github.com/flcker/SparkFace/pull/13), [PR #18](https://github.com/flcker/SparkFace/pull/18), and [PR #22](https://github.com/flcker/SparkFace/pull/22), retained on the `spikes` branch under `spikes/transport-selection/`

## Unchanged product gate

A candidate had to remain a standard Windows 11 virtual camera and deliver NV12
1920×1080 at 30fps to one active consumer with all of the following:

- at least 29.5 delivered fps;
- producer-to-client frame-age p95 no more than 33.3ms;
- zero malformed, torn, duplicate, or reordered frames;
- successful recovery, required-application compatibility, and soak gates after
  the automated gate.

No reduced resolution, lower frame rate, driver, proprietary runtime, or
external bridge was accepted by this verdict.

## Measured candidate results

| Candidate | Observed result | Hard gate |
| --- | --- | --- |
| G1 source-LUID D3D11 handoff | 1,000 frames, 30.093fps, 46.001ms p95 | H4 latency failed |
| G2 cross-adapter D3D12 texture | `E_INVALIDARG`; cross-node sharing tier 0 | Capability failed |
| G3 cross-adapter D3D12 buffer | `E_INVALIDARG`; cross-node sharing tier 0 | Capability failed |
| M1 shared `IMFMediaBuffer` | First-frame pass; zero pre-Frame-Server full-frame CPU copies | Advanced to M2 |
| M2 producer-owned shared slots | 1,000 frames, 28.798fps, 33.569ms p95 | H3 throughput and H4 latency failed |
| M2 source-owned shared slots | 1,000 frames, 28.819fps, 33.428ms p95 | H3 throughput and H4 latency failed |
| A1 COM local server | `REGDB_E_CLASSNOTREG` (`0x80040154`), zero frames | H1 first frame failed |
| A2 COM surrogate | Source still loaded in Frame Server `svchost.exe`, not `dllhost.exe` | H0 topology failed |
| M7 direct H.264 subtype | `MF_E_INVALIDMEDIATYPE` (`0xC00D36B4`), zero frames | H1 first frame failed |
| M8 direct MJPEG subtype | `MF_E_INVALIDMEDIATYPE` (`0xC00D36B4`), zero frames | H1 first frame failed |

M2 proved that Frame Server can consume a leased shared-memory slot through a
custom `IMFMediaBuffer`/`IMF2DBuffer2` without requesting a contiguous-copy
path. It still missed both unchanged product gates. The 0.128ms latency miss of
the source-owned mode is a hard-gate failure, not rounding headroom.

## Source-local follow-up

S1 deterministic source-local rendering later passed 1,000 frames at
30.271fps and 12.415ms render-to-client p95, and H9 protocol-v2 productization
passed its security, restart, side-by-side deployment, and cleanup gates.

H8A RCA corrected the original failure diagnosis. After retaining each Media
Foundation sample and texture through D3D11 copy completion and fixing probe
heartbeat and low-contrast validation defects, the Frame Server-hosted source
sustained 1,000 contiguous physical-camera frames with zero sequence, marker,
physical-content, or pre-Frame-Server CPU-copy errors. It still reached only
29.327fps and 50.167ms capture-to-client p95 against the unchanged 29.5fps and
33.3ms gates. Full detail: [H8A source-local physical-camera capture](2026-11-15-phase0-source-local-physical-camera.md).

## Selection

- **Transport primary:** none.
- **Technical fallback:** none.
- **Product fallback:** none accepted.
- **Product readiness:** blocked.

Recovery, real-application, and soak checks were not run for H8A because the
final automated run failed both performance gates. Spike C independently
selected its face-model path, but no viable camera transport remains for
integration.

## Consequence

The detailed v1 implementation plan remains blocked. Proceeding requires an
explicit product or architecture decision that changes a failed constraint,
followed by retained end-to-end evidence for the revised path. Examples include
an accepted reduced mode, a driver or proprietary SDK, or an external OBS
bridge; none is authorized or selected by this verdict.

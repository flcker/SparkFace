# Phase-0 virtual-camera transport selection verdict

**Date:** 2026-11-14  
**Verdict:** **NO-GO — no transport selected**  
**Evidence:** [PR #13](https://github.com/flcker/SparkFace/pull/13), retained on the `spikes` branch under `spikes/transport-selection/`

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

## Selection

- **Transport primary:** none.
- **Technical fallback:** none.
- **Product fallback:** none accepted.
- **Product readiness:** blocked.

Recovery, real-application, soak, and integrated face/beauty checks were not run
because no candidate survived its preceding hard gate. Spike C also remains
NO-GO without a face model meeting its independent DirectML latency gate.

## Consequence

The detailed v1 implementation plan remains blocked. Proceeding requires an
explicit product or architecture decision that changes a failed constraint,
followed by retained evidence for the revised candidate. Examples include an
accepted reduced mode, a driver or proprietary SDK, an external OBS bridge, or
a larger source-local redesign; none is authorized or selected by this verdict.

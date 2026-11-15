# Virtual-camera transport architecture selection plan

> Status: executed Phase-0 recovery plan, retained as the architecture and gate
> rationale behind the transport evidence. This document is not a current
> selection verdict; see the [canonical Wave-1 verdict](2026-11-14-phase0-transport-selection-verdict.md).

## 1. Goal and decision boundary

Select one primary Windows 11 virtual-camera transport architecture, one
technical fallback with a meaningfully different failure mode, and—only with an
explicit product decision—one degraded product fallback. Selection follows
measured end-to-end evidence, not API availability or component microbenchmarks.

The current product contract remains unchanged during evaluation:

- Windows 11 build 22000 or newer;
- one standard virtual-camera device and one active consumer at a time;
- default 1920×1080 NV12 at 30fps;
- at least 29.5 delivered fps;
- producer-to-client frame-age p95 no more than 33.3ms on Intel Iris Xe;
- no malformed or torn frames;
- Apache-2.0-compatible open build, dynamically linked Qt, and isolated optional
  proprietary plugins;
- user-mode `MFCreateVirtualCamera` and no kernel driver unless a later product
  decision explicitly changes that constraint.

This plan enumerates architecture *classes*. Individual commercial SDKs map to
one of those classes and do not become distinct designs merely because their
vendor or API surface differs.

## 2. Evidence baseline

The replacement must improve on measured failures rather than rerun them under
a new name.

| Evidence | Result that constrains this plan |
| --- | --- |
| Spike A/A2 | `MFCreateVirtualCamera`, NV12 1080p30, OBS and Tencent Meeting work separately; v1 accepts one active consumer |
| Spike B | Copy-free D3D11/D3D12/bgfx/DirectML chain failed; D3D12→D3D11 shared BGRA8/RGBA8/R8 worked, but NV12 and the required reverse direction did not |
| Spike B2 | CPU fallback correct; producer-to-client p95 `78.099ms` |
| Spike B3 | Better pacing mode still measured `57.635ms` p95 |
| Spike B4 | Async source delivered `26.644fps`, p95 `48.758ms`; failed both gates |
| Spike C | Fixed-batch BlazeFace → ROI → 468-point Face Mesh passed at `5.4926ms` p95 against the `8.0ms` gate; the original symbolic-batch failure remains retained evidence |
| Spike D | D3D11 named texture delivered 0/1,000 frames: producer LUID `00000000:0000ffd8`, Frame Server allocator LUID `00000000:000178d8` |

### 2.1 Execution outcome

Wave 1 executed the user-mode external-producer, GPU cross-adapter, activation-topology, compressed-subtype, shared-buffer, and reduced-mode probes. None passed the unchanged complete 1080p30 selection gates, so evidence PR [#13](https://github.com/flcker/SparkFace/pull/13) retained a no-selection verdict.

The later S1 deterministic source-local candidate passed its 1,000-frame automated gate at `30.271fps` and `12.415ms` render-to-client p95 with zero sequence, marker, or pre-Frame-Server full-frame-copy errors. H9 protocol-v2 productization and the available OBS and Tencent Meeting subset also passed in evidence PR [#18](https://github.com/flcker/SparkFace/pull/18).

H8A then exercised the required physical-camera boundary in evidence PR [#22](https://github.com/flcker/SparkFace/pull/22). Camera initialization and one allocator-owned GPU conversion succeeded, but sustained reads hit `DXGI_ERROR_DEVICE_REMOVED` plus timeouts, the 45-second watchdog, or Frame Server service exception `0x80070428`. S1 is therefore NO-GO, not an incomplete candidate. Spike C independently selected fixed-batch BlazeFace plus 468-point Face Mesh in evidence PR [#19](https://github.com/flcker/SparkFace/pull/19), but no viable camera path remains for the integrated workload.

A renamed handle, different namespace, keyed mutex/fence swap, or duplicated
D3D11 handle does not change Spike D's adapter mismatch and is not a new
candidate.

## 3. Architecture inventory

### 3.1 Family M — `MFCreateVirtualCamera` with an external producer

These retain the current desktop engine and C ABI boundary. The custom media
source remains a narrow transport consumer.

| ID | Architecture | Frame path | Initial status |
| --- | --- | --- | --- |
| M0 | Allocator sample + CPU ring copy | app GPU readback → NV12 ring → source memcpy → allocator sample | Measured baseline; NO-GO at 1080p30 |
| M1 | Shared-memory-backed custom media buffer | app GPU readback → leased NV12 slot → custom `IMFMediaBuffer`/`IMF2DBuffer` sample | Test first; smallest architectural change |
| M2 | Source-owned system-memory pool | source publishes reusable slots; app fills a leased slot; source queues the same storage | Test only if source/app can enforce lifetime without hidden copies |
| M3 | Uncompressed alternate formats | YUY2 or BGRA ring instead of NV12 | Conditional; larger bandwidth or conversion cost, retained for compatibility evidence |
| M4 | Hardware-codec bridge | app low-latency encode → bounded bitstream IPC → source hardware decode → allocator sample | Conditional; avoids full-frame IPC but adds codec latency and quality risk |
| M5 | MJPEG bridge | app MJPEG encode → IPC → source decode | Conditional; compatibility-oriented, likely CPU/latency expensive |
| M6 | Broker/service transport | app → Windows service → source | Structural option only; an extra process does not remove a copy by itself |
| M7 | Compressed camera subtype | app encoder → bounded bitstream IPC → source queues H.264/HEVC directly | Product-fallback capability probe; violates the default NV12 contract unless Frame Server supplies an accepted decoded mode |
| M8 | Direct MJPEG camera subtype | app MJPEG encoder → source queues MJPEG directly | Product-fallback capability probe; violates the default NV12 contract unless consumers negotiate an explicitly accepted mode |

`M1` and `M2` require a real COM buffer implementation, explicit slot leasing,
and proof of when the final consumer releases each sample. `IMF2DBuffer::Lock`
may create internal contiguous copies for non-contiguous storage; probes must
exercise `Lock2D` and record whether Frame Server requests a copying path.

### 3.2 Family G — GPU transport across the Frame Server boundary

| ID | Architecture | Frame path | Initial status |
| --- | --- | --- | --- |
| G0 | D3D11 named NT texture | app D3D11 texture → source opens by name | Closed by Spike D unless both LUIDs change and are proven equal |
| G1 | Source-LUID device selection | source publishes allocator LUID; app attempts to enumerate/create on that adapter | Capability test; reject if adapter is unavailable to the interactive session |
| G2 | D3D12 cross-adapter texture | producer adapter → shared cross-adapter row-major texture → source adapter | Test; system-memory-backed and copy-only restrictions apply |
| G3 | D3D12 cross-adapter buffer | producer writes a shared buffer → source GPU converts/copies into local NV12 texture | Test if G2 rejects NV12 or required texture layout |
| G4 | D3D12 cross-adapter BGRA/R8 | share a supported intermediate format → source-local NV12 conversion | Test after G2/G3 capability proof; conversion is part of the measured path |
| G5 | D3D11-on-12 source bridge | source opens D3D12 cross-adapter resource, then copies through D3D11-on-12/local allocator device | Conditional; no claim until adapter and API interop are proven |
| G6 | Common software/WARP device | both processes use a software adapter | Static reject for primary GPU path; may only serve diagnostic isolation |

D3D12 cross-adapter sharing is materially different from failed D3D11 sharing.
Microsoft documents `D3D12_HEAP_FLAG_SHARED_CROSS_ADAPTER`, row-major/copy-only
restrictions, system-memory residency, `ALLOW_CROSS_ADAPTER`, shared fences, and
no sharing across D3D API versions. The probe must therefore include source-side
D3D12 work and a local copy/conversion into the Media Foundation allocator; it
must not assume a D3D12 handle can be opened directly as D3D11.

### 3.3 Family S — move work into the Frame Server media source

| ID | Architecture | Frame path | Initial status |
| --- | --- | --- | --- |
| S0 | Final compositor in source | app sends source frame + compact face/effect metadata; source performs final render into allocator texture | Conditional; source-frame transport still needs M or G family |
| S1 | Full engine in source | source captures camera, runs ML/beauty, writes allocator-owned texture; desktop app sends controls only | Test with a minimal render first; largest user-mode redesign |
| S2 | Split ML/control | source owns capture/render; app or service supplies landmarks and control state | Conditional; timing and trust boundary are complex |
| S3 | Source-local CPU engine | source captures/processes in system memory and queues local samples | Diagnostic fallback; likely high CPU at 1080p30 |

Frame Server Custom Media Sources run as Local Service in Session 0 and cannot
interact with the user desktop. Model acquisition, settings, logging, crash
isolation, GPU selection, file permissions, update behavior, and telemetry must
all be tested in that security context. Heavy work must be deferred from COM
construction to source start.

### 3.4 Family A — change custom-source activation topology

| ID | Architecture | Process boundary | Initial status |
| --- | --- | --- | --- |
| A0 | In-process COM DLL | Frame Server loads source DLL in its current host | Measured baseline |
| A1 | Out-of-process COM local server | Frame Server activates a marshaled `IMFMediaSource` hosted by a dedicated executable | Capability probe; only viable if activation and MF interfaces marshal correctly |
| A2 | COM surrogate | Frame Server activates the source DLL through a configured surrogate | Capability probe; only useful if host identity, adapter, or isolation materially changes |
| A3 | Service-hosted source object | Dedicated Windows service owns source/engine and exposes the COM object | Conditional; adds service deployment and does not guarantee Frame Server acceptance |

These are activation experiments, not assumed-supported product designs. Record
the actual executable, account, session, integrity level, adapter LUID, COM
marshaling behavior, crash containment, and whether samples still cross another
process boundary. Reject any topology that requires unsupported registration or
cannot return one valid sample through `MFCreateVirtualCamera`.

### 3.5 Family E — replace or wrap the standard-camera endpoint

| ID | Architecture | Standard camera | Initial status |
| --- | --- | --- | --- |
| E0 | Current `MFCreateVirtualCamera` software-camera source | Yes | Retained endpoint for M/G/S candidates |
| E1 | Frame Server Custom Media Source with UMDF stub package | Yes | Deployment alternative; same Session 0 source host, not a transport performance fix |
| E2 | AVStream miniport/kernel camera driver | Yes | Enumerate and estimate only; violates current no-kernel-driver constraint |
| E3 | DirectShow source filter | Legacy/consumer-dependent | Static downgrade: Microsoft classifies DirectShow as legacy and recommends Media Foundation for new Windows 10/11 code |
| E4 | Commercial virtual-camera driver/SDK | Vendor-dependent | Map to E1/E2/E3; require license, redistribution, signing, support, and data-path disclosure |
| E5 | Per-application plugin/injection | No single system camera | Product-scope alternative, not a transparent fallback |

Changing from direct `MFCreateVirtualCamera` registration to a UMDF stub can
change installation, trust, and PnP behavior, but it does not by itself move the
custom source out of Frame Server or repair adapter/latency failures.

### 3.6 Family B — operational bridges and non-camera outputs

| ID | Architecture | Downstream use | Initial status |
| --- | --- | --- | --- |
| B0 | OBS Virtual Camera bridge | SparkFace → OBS source/plugin → OBS camera | Operational fallback only; requires OBS running |
| B1 | NDI | NDI-aware applications or OBS bridge | Existing v4 optional proprietary-plugin direction; not a standard camera |
| B2 | Spout/shared-texture plugin | Same-session GPU-aware applications or OBS | Windows-only and consumer/plugin-dependent; not a standard camera |
| B3 | File/network loopback | Application-specific ingest | Diagnostic/integration path, not a system camera |
| B4 | Screen/window capture | Consumer captures SparkFace preview | Manual workaround, not a camera transport |

Operational bridges may be documented as user workarounds, but cannot be named
the v1 technical fallback without an explicit decision that SparkFace may
require another application.

### 3.7 Explicitly excluded non-solutions

The following do not define a new frame transport and receive no separate
performance probe:

- `Local\\` versus `Global\\` object names;
- named handle versus duplicated handle when adapter LUIDs still differ;
- keyed mutex versus fence without a new resource-sharing mechanism;
- named pipe versus file mapping for metadata only;
- registrar, CLSID, INF, or friendly-name changes without a new data path;
- a different UI toolkit;
- Windows Graphics Capture, which captures windows/displays but does not expose
  a standard virtual camera;
- DMFT/MFT/camera-effect insertion, which can transform frames but still needs
  a source and an app-to-source transport;
- vendor SDK wrappers that ultimately use an already listed endpoint family.

## 4. Hard gates

A candidate cannot be rescued by weighted scoring after failing a hard gate.

| Gate | Requirement | Failure action |
| --- | --- | --- |
| H0 Contract | Exact architecture, process/session boundary, copy points, lifetime, privileges, and license are documented | Do not build until specified |
| H1 First frame | One valid 1920×1080 NV12 frame reaches the MF diagnostic client with matching marker and sequence | Stop candidate |
| H2 Correctness | 1,000 frames; strict sequence accounting; zero torn/malformed/marker-error frames | Stop candidate |
| H3 Throughput | At least 29.5 delivered fps | Stop 1080p30 candidate |
| H4 Latency | Producer-to-client frame-age p95 no more than 33.3ms | Stop 1080p30 candidate |
| H5 Standard camera | Enumerates and renders in required target applications, one at a time | Cannot be primary standard-camera option |
| H6 Recovery | Producer loss/restart, 30-second stall, client reopen, sleep/resume, privacy denial/re-enable | Stop candidate on hang, removal, or required reinstall |
| H7 Sustained run | Two 10-minute application soaks without malformed frames, black-frame persistence, device removal, or hang | Stop candidate |
| H8 Product integration | Minimal face ML + representative beauty pass still meets end-to-end H2–H7 | No v1 selection |
| H9 Security/deployment | No undeclared elevation, writable protected path, secret leakage, unsupported signing, or incompatible license | Stop or require explicit product change |

The current single-active-consumer contract remains in force. Multi-consumer
behavior is recorded but is not a v1 gate.

## 5. Staged test program

### Stage 0 — static capability and policy elimination

For every candidate, record:

- whether it remains a standard Windows camera;
- host identity, Windows session, integrity level, and adapter LUID;
- required Windows version and graphics feature queries;
- supported resource dimensions, layouts, formats, heap flags, and sync objects;
- all full-frame copies and conversions;
- administrator, service, driver, signing, WHQL, reboot, and installer needs;
- third-party runtime and licensing obligations;
- whether the current C ABI and UI-agnostic engine boundary survive.

Static elimination uses `disqualified` only for a documented product or API
conflict. An unknown capability is `not-tested`, never `fail` or `pass`.

### Stage 1 — one-frame kill probes

Run in this order because each later item costs more:

1. M1 custom `IMFMediaBuffer`/`IMF2DBuffer` acceptance and release callback.
2. M2 source-owned slot lifetime and cross-session mapping.
3. A1/A2 activation, actual host identity, interface marshaling, and one frame.
4. G1 source-adapter enumeration from the interactive process.
5. G2/G3 cross-adapter heap, resource, shared fence, and one known-pattern copy.
6. G4/G5 source-local conversion/copy into an allocator-owned NV12 sample.
7. M7/M8 direct compressed-subtype negotiation and one queued bitstream frame.
8. S1 minimal source-local shader into an allocator-owned sample.
9. M4 hardware encode/decode first frame only if uncompressed/GPU candidates
   have failed or lack budget margin.

Each probe records the exact HRESULT, adapter LUIDs, format, resource flags,
copy path, QPC boundaries, and cleanup. Failure to deliver one validated frame
ends that candidate.

### Stage 2 — 1,000-frame automated gate

Only Stage-1 survivors run 1,000 delivered frames after warm-up. Record:

- requested, produced, source-queued, and client-delivered frames;
- delivered fps and inter-frame p50/p95/p99/max;
- producer-to-source, source-to-client, and producer-to-client age using the
  same QPC domain;
- skipped, duplicate, reordered, torn, stale, and marker-error counts;
- per-stage p50/p95/p99/max, without summing independent percentiles into an
  invented end-to-end percentile;
- process CPU, GPU engine utilization, working set, committed GPU/system memory,
  queue depth, slot pressure, and full-frame copy count;
- output subtype, dimensions, stride, and whether hidden contiguous-buffer
  conversion was observed.

The source must expose a safe timestamp boundary. If a boundary cannot be
measured, report `n/a`; do not rename a nearby event to the missing stage.

### Stage 3 — resolution and frame-rate characterization

Run only after a candidate fails H3 or H4 but passes correctness:

1. 1920×1080 at 30fps remains the product gate.
2. 1280×720 at 30fps characterizes a possible product fallback.
3. 1920×1080 at 24fps and 15fps characterize frame-rate sensitivity.
4. 1280×720 at 24fps characterizes the final reduced mode.

A reduced mode does not turn the 1080p30 result into GO. It produces a separate
product-change option requiring explicit approval.

### Stage 4 — recovery and lifecycle

For each automated-gate survivor:

- kill and restart the producer without closing the client;
- stop frames for 30 seconds, then resume;
- close/reopen the client repeatedly;
- restart the source/host where safely supported;
- sleep/resume Windows;
- deny/re-enable camera privacy access;
- log off/on when the architecture spans user and Session 0;
- verify installation persistence and clean removal;
- verify every failed run restores COM registration and leaves no probe process,
  public mapping, stale device, service, or driver package.

### Stage 5 — application compatibility

Test applications separately under the accepted single-consumer contract:

| Application | Required observation |
| --- | --- |
| OBS Studio | Enumerates, selects exact mode, moving marker renders, recording has expected dimensions/fps |
| Zoom | Enumerates, preview moves, reconnect succeeds |
| Microsoft Teams | Enumerates, preview moves, reconnect succeeds |
| Discord | Enumerates and renders moving video |
| DingTalk | Enumerates and renders moving video |
| Tencent Meeting | Enumerates, exact mode where observable, preview moves, reconnect succeeds |

Unavailable software, account login, or blocked automation is `not-tested` and
prevents a blanket compatibility claim. Diagnostic MF/DirectShow clients do not
substitute for a required real application.

### Stage 6 — sustained and integrated gates

Only candidates passing Stages 1–5 run:

- one 10-minute OBS soak;
- one 10-minute Tencent Meeting soak;
- the minimal integrated face/beauty workload;
- the same 1,000-frame, recovery, and soak gates with that workload.

Transport-only headroom is not a product pass. Spike C supplies a selected
fixed-batch model path, but H8A rejected the prerequisite source-local
physical-camera path before face ML or representative beauty could be integrated.

## 6. Efficient execution order

### Wave 1 — preserve the current user-mode boundary

- A1/A2 activation-topology capability;
- M7/M8 direct compressed-output capability;
- M1 shared-memory-backed media buffer;
- M2 source-owned slots;
- G1 source-LUID enumeration;
- G2/G3 D3D12 cross-adapter sharing.

### Wave 2 — conversions and source-local rendering

- G4/G5 local conversion and D3D11-on-12 bridge;
- S1 minimal source-local render;
- S0 only if its input path is a Wave-1 survivor.

### Wave 3 — bandwidth-reduction transports

- M4 low-latency hardware codec;
- M5 MJPEG only for compatibility comparison.

### Wave 4 — product-boundary alternatives

- E1 packaging path if it provides a non-performance reason to change install;
- E2/E3/E4 and B0–B4 remain documented options until the user explicitly
  approves their driver, legacy, proprietary, or external-application tradeoff.

A failed wave does not authorize silently lowering resolution or advancing to a
more invasive architecture. The next wave begins only when its result could
still satisfy the unchanged contract or inform a stated product decision.

## 7. Decision algorithm

### 7.1 Eligibility

A primary or technical-fallback transport must pass H0–H7 and H9 at 1080p30.
H8 is the separate product-readiness gate. Spike C now satisfies its independent
model-selection gate, but only an integrated physical-camera, face-ML, and
representative beauty run can satisfy H8. A score cannot override a hard-gate
failure.

The executed H8A result leaves no eligible source-local candidate. Combined
with G1's `46.001ms` p95 failure, no primary or technical fallback survives.

### 7.2 Ranking eligible candidates

Rank survivors lexicographically, not by an opaque weighted total:

1. preserves the accepted standard-camera and 1080p30 contract;
2. larger measured p95 margin below 33.3ms;
3. fewer full-frame copies and conversions;
4. lower privileges and no driver/signing requirement;
5. smaller Session 0 and kernel attack surface;
6. preserves the C ABI and engine/media-source separation;
7. lower dependency, licensing, installation, and maintenance burden;
8. better recovery and application coverage.

Every comparison table must show raw measurements alongside the rank.

### 7.3 Required decision outputs

The verdict must name exactly:

- **Transport primary:** highest-ranked candidate passing H0–H7 and H9.
- **Technical fallback:** another transport-gate-pass candidate with a
  meaningfully different transport/failure mode. A slower mode of the same
  broken transport is not independent.
- **Product-readiness status:** whether the selected primary also passes H8; if
  not, the transport is selected but v1 implementation planning remains blocked.
- **Product fallback:** best measured reduced-resolution/frame-rate, OBS bridge,
  proprietary SDK, or driver option. It remains unaccepted until the user
  approves the corresponding product constraint change.
- **Rejected options:** exact hard gate and evidence for each rejection.
- **No-selection outcome:** if no candidate passes H0–H7 and H9, retain the
  transport NO-GO. In all cases, do not start the v1 build plan until H8 passes.

No fallback may be labeled automatic unless installation, runtime detection,
switching, recovery, and user-visible behavior were themselves tested.

## 8. Retained evidence

Execution retained executable evidence on the long-lived `spikes` branch under
one direct directory:

```text
spikes/transport-selection/
├── README.md
├── spike.json
├── CMakeLists.txt
├── capabilities/
├── mf-shared-buffer/
├── d3d12-cross-adapter/
├── activation-topology/
├── source-local/
├── codec-bridge/
├── compatibility/
├── media-source/
└── results/summary.json
```

Build products, downloaded SDKs, models, DLLs, drivers, recordings, and files
over 10 MiB remain outside Git. Pinned upstream URLs, versions, hashes, licenses,
and exact acquisition/build/run/cleanup commands belong in the manifest.

The evidence and decision history is split deliberately:

1. evidence PR [#13](https://github.com/flcker/SparkFace/pull/13) retained the initial Wave-1 measurements and no-selection result on `spikes`;
2. evidence PR [#18](https://github.com/flcker/SparkFace/pull/18) retained the S1 automated and H9 productization results on `spikes`;
3. the verdict-only main document records the canonical Wave-1 decision without merging probe or product source.

Future H5, H6, or H8 evidence must use the same evidence/verdict branch separation.

## 9. Original planned verification commands

The approved plan proposed these stable entry points; the retained evidence README records the commands actually exercised:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass `
  -File scripts/build-transport-selection.ps1

# Elevated only while temporarily installing/restoring the camera source.
powershell -NoProfile -ExecutionPolicy Bypass `
  -File scripts/run-transport-selection.ps1

powershell -NoProfile -ExecutionPolicy Bypass `
  -File scripts/check-spike-evidence.ps1 -BaseRef origin/spikes
```

Every run script must restore prior registration in `finally`, enforce a finite
watchdog, and fail if cleanup is incomplete.

## 10. Primary references

- [`MFCreateVirtualCamera`](https://learn.microsoft.com/en-us/windows/win32/api/mfvirtualcamera/nf-mfvirtualcamera-mfcreatevirtualcamera): Windows build 22000+, currently supports the software-camera-source type, with explicit lifetime/access behavior.
- [Frame Server Custom Media Source](https://learn.microsoft.com/en-us/windows-hardware/drivers/stream/frame-server-custom-media-source): Custom Media Source versus AVStream, Local Service/Session 0 security context, PnP/stub-driver packaging, and source lifecycle guidance.
- [`IMFMediaBuffer`](https://learn.microsoft.com/en-us/windows/win32/api/mfobjects/nn-mfobjects-imfmediabuffer) and [`IMF2DBuffer`](https://learn.microsoft.com/en-us/windows/win32/api/mfobjects/nn-mfobjects-imf2dbuffer): buffer contracts, stride, contiguous-copy behavior, and `Lock2D` guidance.
- [D3D12 shared heaps](https://learn.microsoft.com/en-us/windows/win32/direct3d12/shared-heaps): cross-process and cross-adapter heap flags, row-major/copy restrictions, system-memory residency, barriers, and cross-adapter fences.
- [DirectShow](https://learn.microsoft.com/en-us/windows/win32/directshow/directshow): Microsoft classifies DirectShow as legacy and recommends Media Foundation-based APIs for new Windows 10/11 code.
- [OBS Virtual Camera](https://obsproject.com/kb/virtual-camera-guide): operational bridge behavior and external OBS dependency.

## 11. Plan review checklist

Before any probe implementation begins, review this document for:

- complete architecture-class coverage and explicit non-solutions;
- consistency with B2/B3/B4/C/D measurements;
- an early kill gate for every expensive candidate;
- no inference presented as measurement;
- no hidden acceptance of lower resolution, lower fps, a driver, proprietary
  runtime, or external application;
- end-to-end—not summed component—latency criteria;
- deterministic cleanup and registration restoration;
- evidence/verdict branch separation;
- a no-selection result when no candidate passes.

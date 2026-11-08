# SparkFace Phase-0 Spike A2 — Frame Server Multi-Consumer Recovery Plan

**Status:** Proposed corrective spike after Spike A **NO-GO**  
**Date:** 2026-11-06  
**Spec:** [v1 MVP design](../specs/2026-08-15-v1-mvp-design.md)  
**Predecessor:** [Phase-0 spikes — Spike A verdict](2026-08-15-phase0-spikes.md#verdict-log)

## Goal

Determine whether the Windows 11 `MFCreateVirtualCamera` architecture can satisfy SparkFace's existing v1 contract when the custom Media Foundation source fully implements the Windows Camera Frame Server sharing contract.

This is a corrective feasibility spike, not product implementation. It must either:

1. prove simultaneous OBS + meeting-app delivery with measured latency and stable lifecycle, changing Spike A to **GO**; or
2. produce a precise, reproducible blocker and preserve Spike A as **NO-GO**.

Spike B, Spike C, and the detailed v1 build plan remain blocked until A2 reaches GO. A GO-WITH-CAVEAT verdict is allowed only after an explicit product decision changes the v1 multi-consumer contract.

## Why A2 Exists

Spike A proved the following on the Iris Xe reference workstation:

| Area | Observed result |
|---|---|
| Single-consumer OBS | 1920×1080 NV12 at 30fps; 360-frame recording; zero OBS render/output skips |
| Single-consumer Tencent Meeting | Device selected and generated frames rendered |
| Ten-minute run | 18,000 samples at 30.00fps; three duplicate samples |
| Producer-to-consumer latency | 57.99ms average; 65.14ms p95 |
| Idle behavior | Black frame within 66.32ms; producer recovery without reopening |
| Lifecycle | App restart, reboot persistence, and removal passed |
| Multi-consumer | Failed in both activation orders |

The two failure modes were:

- OBS first, Tencent Meeting second: Tencent Meeting reported a camera-capture data exception.
- Tencent Meeting first, OBS second: OBS received idle frames instead of the active producer frames.

The Windows API exposes `MF_DEVICESTREAM_FRAMESERVER_SHARED`, whose documented purpose is to explicitly mark a stream as shared by Frame Server. The official Windows-Camera sample also records critical fixes around source attributes, stream state, and Media Foundation work queues. Spike A reduced that implementation before proving those contracts independently. A2 therefore tests the smallest evidence-backed correction before reconsidering the architecture.

## Primary Hypotheses

1. **Shared-stream metadata:** the stream must expose `MF_DEVICESTREAM_FRAMESERVER_SHARED=TRUE` in the attribute store consumed by Frame Server.
2. **Media-source contract fidelity:** source attributes, stream state transitions, events, and work-queue ownership must match the official sample's corrected behavior.
3. **Broadcast IPC semantics:** source instances must observe the same latest complete frame without a destructive global read cursor or one-reader notification primitive.
4. **Queueing:** the measured ~58–65ms latency contains avoidable polling or one-to-two-frame queueing rather than unavoidable Frame Server cost.

These are hypotheses, not accepted root causes. Instrumentation must identify which one is true.

## Non-Goals

- No production engine, C ABI, Qt UI, installer, shader, face model, or GPU-sharing implementation.
- No kernel AVStream fallback.
- No Windows 10 or macOS path.
- No retries, forced camera reopen, or client-specific special cases that mask the source contract failure.
- No reuse of A2 probe code in the product. Keep only the measured verdict and durable architecture decision.

## Reference Baseline

Use the Microsoft Windows-Camera repository at the same pinned revision used by Spike A:

```text
microsoft/Windows-Camera@626f8b19c5f367602f2e89c6b314573d3776c9df
```

Relevant primary references:

- [Windows-Camera VirtualCamera sample](https://github.com/microsoft/Windows-Camera/tree/626f8b19c5f367602f2e89c6b314573d3776c9df/Samples/VirtualCamera)
- [Frame Server custom media source requirements](https://learn.microsoft.com/windows-hardware/drivers/stream/frame-server-custom-media-source)
- [`MF_DEVICESTREAM_FRAMESERVER_SHARED`](https://learn.microsoft.com/windows/win32/medfound/mf-devicestream-frameserver-shared)
- [`MF_DEVSOURCE_ATTRIBUTE_FRAMESERVER_SHARE_MODE`](https://learn.microsoft.com/windows/win32/medfound/mf-devsource-attribute-frameserver-share-mode)

Preserve upstream license and attribution for copied or derived code.

## Probe Layout

Create the probe only on `feat/phase0-virtualcam-multiconsumer`:

```text
spikes/virtualcam-a2/
├── CMakeLists.txt
├── media-source/       # official SimpleMediaSource baseline + minimal IPC adapter
├── producer/           # deterministic moving NV12 pattern + shared ring writer
├── consumer/           # MF diagnostic consumer with sequence/latency checks
├── registrar/          # install/start/stop/remove/list only
└── shared/              # bounded broadcast ring and trace schema
```

Build output, upstream checkout, traces, recordings, screenshots, and generated reports stay under ignored `build/` or OS temporary directories.

## Required Contracts

### Frame Server source and stream

- `GetSourceAttributes` returns the live source attribute store expected by upstream components; it must not return a detached copy.
- The video stream exposes `MF_DEVICESTREAM_FRAMESERVER_SHARED=TRUE` before Frame Server consumes its descriptor/attributes.
- `Start`, `Stop`, `Pause`, `SetStreamState`, `RequestSample`, and shutdown event ordering follows the official sample.
- Each stream owns the required Media Foundation work queue; callbacks do not block that queue waiting for producer frames.
- Media-type changes are serialized and logged. One client must not silently change the active type underneath another client.
- NV12 1920×1080 at 30/1 is the mandatory test type. YUY2 may remain advertised for legacy compatibility but is not a substitute for the NV12 acceptance path.

### Broadcast shared-memory ring

- Fixed-size preallocated slots; no per-frame heap allocation.
- Producer publishes payload, timestamp, and monotonically increasing sequence only after the slot is complete.
- Every source instance keeps its own last-seen sequence. Reading a frame never advances a global consumer cursor.
- Notifications are hints, not ownership tokens. A notification consumed by one source instance must not prevent another instance from seeing the published sequence.
- A slow consumer reads the latest complete slot instead of building an unbounded queue.
- No fresh frame for 100ms produces black NV12 at the negotiated cadence.

### Trace schema

Every source instance records:

```text
process_id, thread_id, source_instance_id, stream_instance_id,
event, media_type, producer_sequence, source_sequence,
producer_qpc, source_acquire_qpc, sample_enqueue_qpc, hresult
```

Required events include activation, source/stream construction, media-type selection, Start/Stop/Pause/Shutdown, RequestSample, producer timeout, black-frame transition, producer recovery, and object destruction.

## Measurement Model

Do not report one opaque latency number. Use synchronized QPC timestamps and report:

| Stage | Measurement | A2 gate |
|---|---|---:|
| Producer publish → source acquire | Shared-ring notification/read cost | p95 ≤1ms |
| Source acquire → sample enqueue | Media-source sample construction/queue cost | p95 ≤2ms |
| Internal virtual-camera handoff | Sum of the two stages above | p95 ≤3ms |
| Sample enqueue → diagnostic consumer read | Frame Server/client scheduling | Report separately; no hidden inclusion in the 0–3ms internal budget |

Also report achieved fps, duplicate sequences, skipped producer sequences, feeder CPU, Frame Server CPU, source instance count, and active media type for every run.

## Execution Tasks

### Task 0 — Establish an exact official baseline

- [ ] Create `feat/phase0-virtualcam-multiconsumer` from the pushed Spike A verdict.
- [ ] Fetch the pinned Windows-Camera revision into ignored `build/reference/`.
- [ ] Build and run the official SimpleMediaSource contract tests that apply to the selected source.
- [ ] Record the exact test list and baseline results before changing source behavior.
- [ ] Copy only the files required for SimpleMediaSource activation, one stream, and registrar lifecycle, preserving attribution.

**Gate:** the unmodified baseline source passes its applicable contract tests. Failure blocks modification; diagnose the environment first.

### Task 1 — Add source/stream contract tracing

- [ ] Add stable per-instance IDs and the trace schema above.
- [ ] Trace source and stream attributes at construction and activation.
- [ ] Trace every state transition and Media Foundation event.
- [ ] Trace media-type negotiation without changing behavior.
- [ ] Run one diagnostic MF consumer and confirm a complete activation-to-shutdown trace.

**Gate:** a trace can distinguish one shared source from multiple independently activated source instances and can identify the active media type for each consumer.

### Task 2 — Prove Frame Server shared-stream behavior

- [ ] Run the official baseline without `MF_DEVICESTREAM_FRAMESERVER_SHARED`; capture source-instance and client behavior.
- [ ] Set `MF_DEVICESTREAM_FRAMESERVER_SHARED=TRUE` in the stream attribute store exposed to Frame Server.
- [ ] Repeat with two diagnostic MF consumers in both activation orders.
- [ ] Confirm both consumers receive changing producer sequences concurrently.
- [ ] Confirm closing either consumer does not stop or change media type for the other.

**Gate:** two diagnostic consumers receive the same published sequence progression without sharing violations, black-only output, or source restart.

### Task 3 — Implement broadcast IPC and idle recovery

- [ ] Replace only the official synthetic-frame generator with the bounded broadcast ring adapter.
- [ ] Keep a reader-local last-seen sequence in every source instance.
- [ ] Generate a deterministic moving NV12 pattern with a visible sequence/time marker.
- [ ] Verify two diagnostic consumers receive changing frames; compare their sequence ranges.
- [ ] Stop the producer while both remain open; verify black frames begin within 100ms.
- [ ] Restart the producer; verify both recover without reopening.

**Gate:** neither consumer starves the other, and both idle/recovery transitions pass.

### Task 4 — Remove avoidable queueing and measure latency

- [ ] Instrument all four QPC timestamps defined in the measurement model.
- [ ] Eliminate polling waits and destructive auto-reset notification behavior from the hot path.
- [ ] Ensure the source always takes the latest complete slot.
- [ ] Run 10 minutes at 1920×1080 NV12 30fps with two diagnostic consumers.
- [ ] Report p50/p95/p99 per stage, duplicate sequences, skipped sequences, fps, feeder CPU, and Frame Server CPU.

**Gate:** internal handoff p95 ≤3ms, sustained output is 30fps within measurement precision, and there is no unbounded latency growth.

### Task 5 — Revalidate real applications

Run every row; do not substitute enumeration for rendered changing frames.

| Case | First consumer | Second consumer | Required evidence |
|---|---|---|---|
| A | OBS Studio | Tencent Meeting | Both show the moving pattern; OBS records 1080p30; Tencent preview changes |
| B | Tencent Meeting | OBS Studio | Same requirements in reverse activation order |
| C | OBS Studio | Tencent Meeting reopened | OBS remains live while Tencent exits and rejoins |
| D | Tencent Meeting | OBS Studio reopened | Tencent remains live while OBS exits and rejoins |

For OBS, record stats and inspect the output with `ffprobe` plus sampled frame hashes. For Tencent Meeting, capture the preview surface while the deterministic marker changes; retain visual evidence and trace correlation.

- [ ] Run Cases A–D.
- [ ] Repeat each activation order three times.
- [ ] Run a 10-minute simultaneous OBS + Tencent Meeting soak.
- [ ] During the soak, stop and restart the producer once.
- [ ] Record client errors, dropped frames, media-type changes, and source-instance lifecycle.

**Gate:** both applications continuously receive valid changing frames in every case; zero sharing violations or camera-capture exceptions.

### Task 6 — Revalidate lifecycle and cleanup

- [ ] Exercise install/start/stop/shutdown/remove.
- [ ] Restart both applications and reopen the device.
- [ ] Reboot once if registration, lifetime, or activation behavior changed from Spike A.
- [ ] Remove the camera before unregistering the COM source.
- [ ] Confirm no SparkFace PnP camera, COM registration, process, startup task, or RunOnce entry remains.
- [ ] Record privilege boundaries. Treat HKLM COM registration and installer elevation as packaging requirements, not runtime elevation.

**Gate:** normal camera use is non-elevated and cleanup leaves no registered device or COM class.

### Task 7 — Record verdict and discard the probe

Append the A2 result to the original Phase-0 verdict log with:

```text
Spike A2: <GO|NO-GO> — shared_attribute=<yes/no>, source_instances=<n>,
apps=<versions>, activation_orders=<pass/fail>, 1080p30=<yes/no>,
internal_handoff_p95_ms=<x>, frame_server_delivery_p95_ms=<x>,
duplicates=<n>, skipped=<n>, idle_black_ms=<x>, recovery=<yes/no>,
restart=<yes/no>, reboot=<yes/no/not-needed>, clean_remove=<yes/no>,
admin_install=<yes/no>, admin_runtime=<yes/no>, upstream=<commit>
```

- [ ] Delete `spikes/virtualcam-a2/` and ignored probe artifacts.
- [ ] Retain only plan/verdict documentation and any explicitly approved durable spec change.
- [ ] Commit the verdict on the feature branch.
- [ ] Push only after separate user approval.

## Verdict Rules

### GO

All of the following are mandatory:

- OBS and Tencent Meeting both receive valid changing frames simultaneously in both activation orders.
- Consumer exit/rejoin does not interrupt the remaining consumer.
- Ten-minute two-consumer soak passes at NV12 1920×1080/30fps.
- Internal handoff p95 is ≤3ms.
- Idle black begins within 100ms and both consumers recover without reopening.
- Lifecycle, privilege, reboot/persistence when applicable, and clean removal pass.

A GO replaces the Spike A NO-GO verdict with linked A2 evidence, then unblocks Spike B.

### NO-GO

Any mandatory GO criterion fails after the official source contract and broadcast IPC semantics are confirmed. Record the exact failing layer and stop Phase-0 again.

### GO-WITH-CAVEAT

A2 cannot choose this outcome by itself. If multi-consumer still fails, GO-WITH-CAVEAT requires explicit user approval to change the v1 contract, followed by updates to:

- the v1 MVP specification;
- README product claims;
- compatibility matrix and tests;
- the original Phase-0 verdict log.

Without that product decision, multi-consumer failure remains NO-GO.

## Stop Conditions

Stop and report before continuing when any of these occurs:

- the official baseline fails its own applicable source contract tests;
- the documented shared-stream attribute is not visible to Frame Server;
- either real application still receives idle-only frames or reports a capture exception;
- internal handoff p95 remains above 3ms after queueing is removed;
- cleanup leaves an orphaned PnP device or COM registration.

Do not proceed to Spike B/C or write the v1 implementation plan while a stop condition is active.

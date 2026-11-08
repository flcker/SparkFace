# SparkFace Phase-0 Spike A2 — Frame Server Multi-Consumer Recovery Plan

**Status:** Completed — **GO-WITH-CAVEAT** accepted for v1 single-consumer scope; real-application concurrency is deferred
**Date:** 2026-11-06  
**Spec:** [v1 MVP design](../specs/2026-08-15-v1-mvp-design.md)  
**Predecessor:** [Phase-0 spikes — Spike A verdict](2026-08-15-phase0-spikes.md#verdict-log)

## Goal

Determine whether the Windows 11 `MFCreateVirtualCamera` architecture can satisfy SparkFace's existing v1 contract when the custom Media Foundation source fully implements the Windows Camera Frame Server sharing contract.

This is a corrective feasibility spike, not product implementation. It must either:

1. prove simultaneous OBS + meeting-app delivery with measured latency and stable lifecycle, changing Spike A to **GO**; or
2. produce a precise, reproducible blocker and preserve Spike A as **NO-GO**.

On 2026-11-08 the product owner explicitly narrowed v1 to one virtual-camera device with one active consuming application at a time. This converts the measured concurrency NO-GO into an accepted **GO-WITH-CAVEAT** and unblocks Spike B, Spike C, and subsequent v1 planning once those spikes pass.

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

- [x] Create `feat/phase0-virtualcam-multiconsumer` from the pushed Spike A verdict.
- [x] Fetch the pinned Windows-Camera revision into ignored `build/reference/`.
- [x] Build and run the official SimpleMediaSource contract tests that apply to the selected source.
- [x] Record the exact test list and baseline results before changing source behavior.
- [x] Copy only the files required for SimpleMediaSource activation, one stream, and registrar lifecycle, preserving attribution.

**Observed baseline (2026-11-08):** built the pinned source and tests with Windows SDK `10.0.26100.0`, MSVC `19.51.36256`, and the installed `v145` toolset override. `SimpleMediaSourceTest.TestMediaSource`, `SimpleMediaSourceTest.TestMediaSourceStream`, and `SimpleMediaSourceTest.TestKsControl` all passed (`3/3`, 30 valid NV12 samples and 30 valid RGB32 samples). The separate `VirtualCameraSimpleMediaSourceTest.TestCreateVirtualCamera` correctly rejected a non-elevated unpackaged launch with `E_ACCESSDENIED`; the official test itself logs that elevation is required. This is a registrar privilege precondition, not a SimpleMediaSource contract failure, and A2 retests the virtual-camera lifecycle through its explicit elevated registrar in Task 6.

**Gate:** PASS — the unmodified applicable source contract suite passed before A2 modification.

### Task 1 — Add source/stream contract tracing

- [x] Add stable per-instance IDs and the trace schema above.
- [x] Trace source and stream attributes at construction and activation.
- [x] Trace every state transition and Media Foundation event.
- [x] Trace media-type negotiation without changing behavior.
- [x] Run one diagnostic MF consumer and confirm a complete activation-to-shutdown trace.

**Gate:** a trace can distinguish one shared source from multiple independently activated source instances and can identify the active media type for each consumer.

**Observed:** PASS — the CSV trace identified activation/source/stream instances, exposed both stream and descriptor shared flags, recorded NV12 negotiation and every source/stream transition, and correlated producer, enqueue, and consumer QPC timestamps.

### Task 2 — Prove Frame Server shared-stream behavior

- [x] Run the official baseline without `MF_DEVICESTREAM_FRAMESERVER_SHARED`; capture source-instance and client behavior.
- [x] Set `MF_DEVICESTREAM_FRAMESERVER_SHARED=TRUE` in the stream attribute store exposed to Frame Server.
- [x] Repeat with two diagnostic MF consumers in both activation orders.
- [x] Confirm both consumers receive changing producer sequences concurrently.
- [x] Confirm closing either consumer does not stop or change media type for the other.

**Gate:** two diagnostic consumers receive the same published sequence progression without sharing violations, black-only output, or source restart.

**Observed:** PASS for diagnostic consumers — both consumers reported `frameserver_shared=stream:1,descriptor:1`, received the same moving NV12 sequence concurrently, and the remaining consumer continued after its peer exited.

### Task 3 — Implement broadcast IPC and idle recovery

- [x] Replace only the official synthetic-frame generator with the bounded broadcast ring adapter.
- [x] Keep a reader-local last-seen sequence in every source instance.
- [x] Generate a deterministic moving NV12 pattern with a visible sequence/time marker.
- [x] Verify two diagnostic consumers receive changing frames; compare their sequence ranges.
- [x] Stop the producer while both remain open; verify black frames begin within 100ms.
- [x] Restart the producer; verify both recover without reopening.

**Gate:** neither consumer starves the other, and both idle/recovery transitions pass.

**Observed:** PASS — the broadcast ring served both consumers without a destructive cursor. Producer loss switched to black within the 100ms gate (conservative bound `<=50.7ms`), and both consumers resumed producer frames without reopening.

### Task 4 — Remove avoidable queueing and measure latency

- [x] Instrument all four QPC timestamps defined in the measurement model.
- [x] Eliminate polling waits and destructive auto-reset notification behavior from the hot path.
- [x] Ensure the source always takes the latest complete slot.
- [x] Run 10 minutes at 1920×1080 NV12 30fps with two diagnostic consumers.
- [ ] Report p50/p95/p99 per stage, duplicate sequences, skipped sequences, fps, feeder CPU, and Frame Server CPU. Average/p95, sequence counts, and fps were retained; the real-app NO-GO stop condition occurred before a separate p50/p99/CPU report was retained.

**Gate:** internal handoff p95 ≤3ms, sustained output is 30fps within measurement precision, and there is no unbounded latency growth.

**Observed:** PASS for the mandatory latency/fps gate — both consumers completed 18,000 frames at 30fps. Producer publish → source acquire was `0.083ms` average / `0.116ms` p95; source acquire → enqueue was `0.117ms` average / `0.160ms` p95; internal handoff was `0.200ms` average / `0.274ms` p95; Frame Server/client delivery was `27.666ms` average / `28.390ms` p95. The two consumers recorded `3/3` and `425/428` duplicate/skipped sequences respectively, with no unbounded latency growth.

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

**Observed:** FAIL / stop condition — with OBS Studio `33.0.0` first, OBS continued rendering the moving pattern while Tencent Meeting `3.47.2.405` remained on an empty preview. With Tencent Meeting first, Tencent rendered the moving pattern while OBS remained black. Each application passed alone, so enumeration, activation, NV12 production, and rendering are individually valid. The trace did not record a second running source when the second real application opened: failure occurs in Frame Server/client access-mode arbitration before the second source reaches the media-source streaming path. Cases C/D, repetition, and the simultaneous soak were not run because the plan requires an immediate stop when either real application still receives empty/idle output.

### Task 6 — Revalidate lifecycle and cleanup

- [x] Exercise install/start/stop/shutdown/remove.
- [x] Restart both applications and reopen the device.
- [x] Reboot once if registration, lifetime, or activation behavior changed from Spike A. Not needed: A2 retained the already-proven system lifetime and registration model.
- [x] Remove the camera before unregistering the COM source.
- [x] Confirm no SparkFace PnP/DirectShow camera, COM registration, process, startup task, or RunOnce entry remains.
- [x] Record privilege boundaries. Treat HKLM COM registration and installer elevation as packaging requirements, not runtime elevation.

**Gate:** normal camera use is non-elevated and cleanup leaves no registered device or COM class.

**Observed:** PASS — install/remove required elevation for HKLM COM registration; OBS, Tencent Meeting, producer, and diagnostic consumers ran non-elevated. Uninstall returned success; the COM class, probe processes, startup entries, and DirectShow SparkFace device were absent afterward. Launching the producer after removal failed because the transport mapping was unavailable, as expected.

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

**Recorded result:** `Spike A2: NO-GO — shared_attribute=yes, source_instances=2 diagnostic / second real-app source never reached running, apps=OBS Studio 33.0.0 + Tencent Meeting 3.47.2.405, activation_orders=fail/fail, 1080p30=yes for either app alone, internal_handoff_p95_ms=0.274, frame_server_delivery_p95_ms=28.390, duplicates=428, skipped=431, idle_black_ms<=50.7, recovery=yes, restart=yes, reboot=not-needed, clean_remove=yes, admin_install=yes, admin_runtime=no, upstream=626f8b19c5f367602f2e89c6b314573d3776c9df`.

**Accepted product decision (2026-11-08):** v1 exposes one virtual-camera device and supports one active consuming application at a time. The recorded A2 result remains the evidence that same-device concurrency is unsupported; it is no longer a v1 acceptance criterion. Multi-consumer support is deferred post-v1 without a committed version or implementation route. The v1 specification, root READMEs, compatibility matrix, documentation index, and original Phase-0 verdict log were updated accordingly.

- [x] Delete `spikes/virtualcam-a2/` and ignored probe artifacts.
- [x] Retain only plan/verdict documentation and any explicitly approved durable spec change.
- [x] Commit the verdict on the feature branch.
- [ ] Push only after separate user approval. Not pushed.

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

A2 cannot choose this outcome by itself. The product owner explicitly approved the required v1 contract change on 2026-11-08:

- v1 exposes one virtual-camera device;
- one consuming application may use it at a time;
- concurrent consumers are deferred post-v1;
- the v1 specification, README product claims, compatibility matrix, tests, and original Phase-0 verdict log reflect the restriction.

Therefore A2 closes as **GO-WITH-CAVEAT** while preserving its measured concurrency failure.

## Stop Conditions

Stop and report before continuing when any of these occurs:

- the official baseline fails its own applicable source contract tests;
- the documented shared-stream attribute is not visible to Frame Server;
- either real application still receives idle-only frames or reports a capture exception;
- internal handoff p95 remains above 3ms after queueing is removed;
- cleanup leaves an orphaned PnP device or COM registration.

The real-application concurrency stop condition fired and was recorded. After the explicit 2026-11-08 contract decision, it no longer blocks Spike B/C because concurrency is outside v1 scope; all other stop conditions remain binding.

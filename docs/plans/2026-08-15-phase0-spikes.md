# SparkFace Phase-0 Spikes Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** De-risk the three load-bearing unknowns of SparkFace v1 (Windows 11 Media Foundation user-mode virtual camera, D3D11↔D3D12 GPU interop and virtual-camera handoff, face-model iGPU/DirectML performance) through durable feasibility probes with hard pass/fail criteria — before writing the detailed v1 build plan.

**Architecture:** Three independent spikes (A: `MFCreateVirtualCamera` + custom media source, B: DXGI cross-device sharing + explicit fence synchronization + camera handoff, C: ONNX/DirectML face-model benchmark) plus a shared Task 0 build-environment gate. Each produces a **verdict + measured numbers** and remains executable evidence on the long-lived `spikes` branch, not reusable product code. Spike outcomes feed a second writing-plans pass that produces the detailed v1 build plan.

**Tech Stack:** C++17, CMake, Windows 11 build 22000+, Windows SDK, Media Foundation virtual-camera API, bgfx (D3D12 backend), ONNX Runtime + DirectML EP, Intel Iris Xe iGPU reference machine.

**Spec:** [v1 MVP design](../specs/2026-08-15-v1-mvp-design.md) (§6.3 mandates these spikes before v1 implementation)

## Global Constraints

- **Execution environment:** Windows 11 build 22000+ with Visual Studio 2022 or newer, Windows SDK 10.0.22000.0+, CMake, and an **iGPU reference machine (Intel Iris Xe)**. This plan CANNOT run on Windows 10 or macOS. All `Run:` commands are Windows (PowerShell/cmd).
- **Evidence retention and reproducibility:** Probe/product code NEVER merges into `main`; retain executable evidence on long-lived `spikes` and never remove it after a verdict. Every direct `spikes/<id>/` directory except `spikes/template/` requires `README.md`, strict-JSON `spike.json`, and `CMakeLists.txt`; the manifest requires `schema_version`, `id`, `title`, `status`, `upstream`, `hardware`, `dependencies`, `artifacts`, non-empty `commands.build`/`run`/`cleanup`, `gates`, and `verdict`. Completed or closed spikes also retain `results/summary.json`. Keep large models/binaries out of Git, but retain fixed acquisition URLs, versions, hashes, licenses, and acquisition commands in metadata. Preserve upstream attribution, exact reproduction commands, and measured results. Do not represent unavailable historical probe code as retained evidence.
- **Language:** C++17; build with CMake + `CMakePresets.json`.
- **Render backend:** bgfx pinned to its **D3D12** backend on Windows (to align with DirectML, which is D3D12-based). See spec §2.1.
- **Perf target:** iGPU 30fps = **≤33.3ms/frame** budget (30fps frame time = 33.33ms). Spike B/C measure components of this budget. See spec §2.2.
- **Dependencies (mixed acquisition, spec §4.1):** bgfx via vcpkg port or FetchContent; ONNX Runtime + DirectML via official prebuilt redist (`Microsoft.ML.OnnxRuntime.DirectML` NuGet unpacked, or the C/C++ redist zip from onnxruntime releases); small libs (gtest) via FetchContent.
- **Output format/resolution:** target 1080p, NV12 primary / BGRA8 fallback. Spike A confirms what the Frame Server virtual-camera path and target consumers accept; Spike B confirms which formats can stay GPU-resident across D3D11↔D3D12 boundaries.
- **Evidence and verdict PRs:** Open executable probe code, measurements, manifests, and reproducibility assets in an evidence PR targeting `spikes`. Open a separate verdict-only documentation PR targeting `main`; it records the canonical verdict but MUST NOT add probe/product code. Parallel code work requires separate worktrees and exclusive file ownership, with no more than five agents; one integration owner serializes all hardware/GPU/virtual-camera/Frame Server/OBS/meeting-app measurement and final evidence capture.
- **Decision baseline:** v1 targets Windows 11 build 22000+ and uses the user-mode `MFCreateVirtualCamera` API. A kernel AVStream fallback is out of scope; a Spike A NO-GO stops the plan and triggers a product/OS-support decision instead of silently adding a driver.

---

### Task 0: Windows 11 Build Environment + iGPU Reference Gate

**Files:**
- Create: `CMakeLists.txt` and `CMakePresets.json` on the `spikes` evidence branch
- Create: `vcpkg.json` (full Git baseline + explicit bgfx version override)
- Create: `scripts/bootstrap-vcpkg.ps1` (bootstraps the same vcpkg tool commit on every developer/CI machine)
- Create: `scripts/build-phase0-env.ps1` (single configure/build/run entry point)
- Create: `spikes/env/main.cpp` and `spikes/env/CMakeLists.txt`

**Interfaces:**
- Consumes: none (first task)
- Produces: a working Windows 11 user-mode build environment that can compile/link C++17/CMake, use the Media Foundation virtual-camera API, drive bgfx (D3D12), drive ONNX Runtime DirectML, and identify the iGPU adapter. All later spikes depend on this.

- [x] **Step 1: Install prerequisites and bootstrap the pinned toolchain**

Install (if absent): Visual Studio 2022 or newer ("Desktop development with C++" workload), Windows SDK 10.0.22000.0 or newer, CMake ≥3.27, Git, and the Visual Studio Ninja component. `scripts/bootstrap-vcpkg.ps1` reads the full `builtin-baseline` commit from `vcpkg.json`, fetches that exact vcpkg source/tool into ignored `build/tools/vcpkg`, and disables metrics. Developer machines and CI MUST use this script rather than an arbitrary system or Visual Studio-bundled vcpkg. WDK, Qt, and an EV certificate are not Phase-0 prerequisites.

- [x] **Step 2: Confirm the OS floor and iGPU reference adapter**

Run PowerShell:
```powershell
[Environment]::OSVersion.Version
Get-CimInstance Win32_VideoController | Select-Object Name, DriverVersion, AdapterRAM
```
Require Windows build ≥22000 and record the Intel Iris Xe adapter name. If the OS is older or only a dGPU exists, STOP: the selected virtual-camera API or the iGPU performance promise cannot be validated.

- [x] **Step 3: Fetch locked dependencies**

`vcpkg.json` pins the registry snapshot and explicitly overrides bgfx to `1.129.8940-496#1`; transitive ports resolve with triplet `x64-windows` (bgfx remains a static library but uses the dynamic MSVC CRT, matching ONNX Runtime and the later Qt app). CMake downloads ONNX Runtime DirectML from the versioned NuGet URL with an expected SHA-256. `CMakePresets.json` fixes Ninja, Debug, the vcpkg toolchain, and the triplet. The gate records CMake, MSVC, Windows SDK, vcpkg baseline, bgfx port, and ONNX Runtime versions so cross-machine drift is visible.

- [x] **Step 4: Write the trivial gate program**

`spikes/env/main.cpp`: create a D3D12 device on the Intel adapter; initialize bgfx with that external D3D12 context and a hidden Win32 surface, transfer device ownership to bgfx, and run one `bgfx::frame`; create an ONNX Runtime session with CPU fallback disabled and execute a pinned Mul model through DirectML; call `MFStartup`; compile/link against `mfplat.lib` and `mfsensorgroup.lib`; verify `MFCreateVirtualCamera` is exported by `mfsensorgroup.dll`. Print toolchain versions, OS build, adapter identity, provider/result, and a pass line. Actual camera registration belongs to Spike A.

- [x] **Step 5: Build + run**

Run only the repository entry point:
```powershell
.\scripts\build-phase0-env.ps1 -Run
```
Expected: pinned vcpkg bootstrap, successful dependency restore/configure/build, Windows build ≥22000, Intel Iris Xe, D3D12 bgfx frame, `MFCreateVirtualCamera`, a finite DirectML model result with CPU fallback disabled, and `ENV_GATE_PASS`. Missing any item is a blocker.

Observed PASS on the Iris Xe reference workstation after disabling Windows Smart App Control, which does not support per-application exceptions. The gate selected bgfx Direct3D 12, loaded `MFCreateVirtualCamera`, executed the pinned ONNX Mul model through `DmlExecutionProvider` with CPU fallback disabled, returned finite output `2`, and printed `ENV_GATE_PASS`.

- [x] **Step 6: Record env gate and preserve evidence**

Append `env gate PASS — os_build=<n>, adapter=<name>, MFCreateVirtualCamera=available, bgfx=D3D12, DirectML=<version/result>, cmake=<version>, msvc=<version>, windows_sdk=<version>, vcpkg_baseline=<commit>` to the Verdict Log. Submit the environment scaffolding, manifest, reproduction instructions, and measured result in an evidence PR to `spikes`. Submit the matching verdict-log update separately in a verdict-only PR to `main`.

---

### Task 1: Spike A — Windows 11 User-Mode Virtual Camera Feasibility

**Files:**
- Create: `spikes/virtualcam/media-source/` (minimal custom Media Foundation media-source DLL)
- Create: `spikes/virtualcam/registrar/` (`MFCreateVirtualCamera` register/start/stop/remove utility)
- Create: `spikes/virtualcam/feeder/` (1080p NV12 test-pattern producer over shared memory)
- Create: `spikes/virtualcam/CMakeLists.txt`

**Hypothesis:** On Windows 11 build 22000+, a custom user-mode Media Foundation source registered through `MFCreateVirtualCamera` can appear as a standard camera in target applications, deliver 1080p NV12 at 30fps, emit black frames when the feeder is absent, and install/remove cleanly without a kernel driver or EV certificate. (Spec §2.1, §6.1 #1, §6.3 spike 1.)

**Interfaces:**
- Consumes: Task 0 build environment and Media Foundation virtual-camera API availability.
- Produces: **Verdict A** — GO / GO-WITH-CAVEAT / NO-GO + measured/observed: enumeration and video in OBS plus at least one meeting app, sustained frame rate, accepted media types, feeder-to-source IPC latency/CPU, idle black-frame behavior, restart/reboot persistence, clean removal, permissions/admin requirements, and two-consumer behavior.

- [x] **Step 1: Reduce Microsoft's official virtual-camera sample**

Use the `microsoft/Windows-Camera` `Samples/VirtualCamera` implementation as the reference, retaining only the custom media-source activation, one video stream, and registrar lifecycle needed by this spike. Do not copy the sample's UI, camera-wrapper modes, or MSI project. Record the upstream commit used.

- [x] **Step 2: Implement the user-mode frame handoff**

Create a bounded named shared-memory ring (`CreateFileMapping`) plus events and monotonically increasing sequence/timestamps. The feeder writes generated 1920×1080 NV12 frames at 30fps; the Frame Server-hosted media source reads the latest complete slot without blocking its work queue. No per-frame heap allocation. If no fresh frame arrives for 100ms, the source generates black NV12 frames while preserving timestamps and cadence.

- [x] **Step 3: Register and start the virtual camera**

Register the media-source COM activation required by Frame Server, then call `MFCreateVirtualCamera` with `MFVirtualCameraType_SoftwareCameraSource`. Test `MFVirtualCameraAccess_CurrentUser` first; record if machine-wide COM registration or administrator access is still required. Use a stable friendly name and source CLSID so the same camera can be reopened and removed deterministically.

- [x] **Step 4: Confirm enumeration and negotiated formats**

Enumerate the camera through Media Foundation, then open it in OBS and at least one of Zoom/Teams/Discord. Confirm the test pattern renders at 1920×1080, 30fps. Record whether NV12 is accepted end-to-end and which fallback format is required if not.

- [x] **Step 5: Measure delivery and idle behavior**

Run for at least 10 minutes. Record achieved fps, dropped/duplicated frames, feeder CPU, media-source CPU, and feeder-write-to-sample timestamp latency. Stop the feeder while consumers remain open; confirm black frames begin within 100ms without freezing or disconnecting. Restart the feeder and confirm recovery without reopening the camera.

- [x] **Step 6: Verify lifecycle, persistence, and cleanup**

Exercise Start/Stop/Shutdown/Remove, application restart, and one reboot with the chosen lifetime. Confirm there is no orphaned camera or COM registration after removal/uninstall. Record exact privilege requirements.

- [x] **Step 7: Verify two consumers**

Open the camera concurrently in OBS and one meeting app. Confirm both receive valid frames or record the precise Frame Server limitation. Multi-consumer failure is GO-WITH-CAVEAT only if v1 explicitly narrows its contract; otherwise it is NO-GO.

- [x] **Step 8: Record Verdict A and preserve evidence**

Append: `Spike A: <GO|GO-WITH-CAVEAT|NO-GO> — apps=<list>, 1080p30=<yes/no>, formats=<list>, feeder_to_sample_ms=<x>, cpu=<x%>, idle_black=<yes/no>, restart=<yes/no>, reboot_persistence=<yes/no>, clean_remove=<yes/no>, admin_required=<yes/no>, two_consumers=<yes/no>`.

Submit the executable probe, strict manifest, reproduction instructions, attribution, and measured result in an evidence PR to `spikes`; do not remove it after the verdict. Submit only the verdict-log documentation in a separate PR to `main`. If historical probe code is unavailable, preserve the measured verdict without claiming executable evidence exists.

---

### Task 2: Spike B — D3D11↔D3D12 Sharing, Fence Synchronization, and Camera Handoff

**Files:**
- Create: `spikes/sharing/main.cpp` (evidence probe)
- Create: `spikes/sharing/CMakeLists.txt`

**Hypothesis:** On the Iris Xe iGPU, frames can remain GPU-resident through MF-capture-D3D11 → DirectML-D3D12 → bgfx-D3D12 using NT shared handles, explicit shared fences, and a documented resource-state contract. The Windows 11 custom media source can then consume a GPU-backed `IMFSample`; if Frame Server rejects that path, one measured GPU→CPU readback plus the Spike A shared-memory handoff still fits the 30fps budget. (Spec §2.1, §6.1 #2, §6.3 spike 2.)

**Interfaces:**
- Consumes: Task 0 GPU stack and Spike A's accepted virtual-camera media types/IPC contract. It does not consume Spike A probe code.
- Produces: **Verdict B** — GO / GO-WITH-CAVEAT / NO-GO + measured: copy-free GPU path, working shared formats, D3D11↔D3D12 fence and resource-state protocol, synchronization cost, DirectML I/O-binding behavior, bgfx native-resource access, GPU-backed virtual-camera sample support, fallback readback+IPC cost, same-adapter confirmation, and total cost against ≤33.3ms.

- [x] **Step 1: Force all GPU components onto the iGPU adapter**

Enumerate adapters and select the Intel Iris Xe by LUID. Create the D3D11 capture device, DirectML D3D12 device/queue, and bgfx D3D12 device on that adapter. Assert matching adapter LUIDs. A component that cannot be placed on the same adapter is a NO-GO finding for the copy-free design.

- [x] **Step 2: Confirm resource-sharing formats and APIs**

For NV12, BGRA8, RGBA8, and R8, create D3D11 textures with `D3D11_RESOURCE_MISC_SHARED_NTHANDLE`, export with `IDXGIResource1::CreateSharedHandle`, and open on D3D12 with `ID3D12Device::OpenSharedHandle`. Record creation/open success, required flags, row/layout constraints, and whether a shared heap or committed resource is exposed. Do not use the nonexistent `OpenSharedHandle1` API.

- [x] **Step 3: Define and prove synchronization/state ownership**

Use an `ID3D12Fence` shared handle and `ID3D11Device5::OpenSharedFence` where supported. Define, in the probe, which component owns the resource, the fence value before/after each hop, and the D3D12 state on entry/exit. Run a known-pattern round trip for 1,000 frames and validate contents after GPU completion. Do not treat `IDXGIKeyedMutex` as the D3D12 synchronization primitive.

- [x] **Step 4: Prove DirectML and bgfx interop**

Bind the shared resource to ONNX Runtime DirectML through supported device-memory/I/O-binding APIs and run a trivial model without `Map` or a staging upload in the per-frame GPU path. Import or access the resulting D3D12 resource through bgfx's supported native-resource interface and run one compute pass. Record any hidden copy or API limitation as a caveat or NO-GO.

- [ ] **Step 5: Test GPU-backed Media Foundation samples**

Wrap the final DXGI surface with `MFCreateDXGISurfaceBuffer`, place it in an `IMFSample`, and deliver it from a reduced custom media source using the media type accepted by Spike A. Verify the camera in OBS and inspect whether Frame Server preserves the GPU surface or forces a copy/conversion. Record support and cost per format.

- [x] **Step 6: Measure the CPU fallback only if required**

If the GPU-backed sample path is unsupported or unstable, read the final 1080p frame into a persistently allocated staging/readback buffer and publish it through the bounded shared-memory ring from Spike A. Measure readback, copy/IPC, and media-source delivery separately. No per-frame allocation.

- [ ] **Step 7: Measure sustained end-to-end overhead**

After warm-up, run at least 1,000 frames and then a 10-minute 1080p30 soak. Record per-hop fence wait/signal cost, DirectML binding/inference floor, bgfx pass, virtual-camera handoff, p50/p95/p99 frame cost, dropped frames, and total against the 33.3ms envelope.

- [x] **Step 8: Record Verdict B and preserve evidence**

Append: `Spike B: <verdict> — copy_free_gpu=<yes/no>, formats=<list>, fence_protocol=<ok/fail>, sync_p95_ms=<x>, dml_binding=<direct/copy>, bgfx_native=<yes/no>, camera_gpu_sample=<yes/no>, fallback_readback_ipc_ms=<x|n/a>, same_adapter=<yes/no>, total_p95_ms=<x>, fits_30fps=<yes/no>`.

Submit the executable probe, strict manifest, reproduction instructions, attribution, and measured result in an evidence PR to `spikes`; do not remove it after the verdict. Submit only the verdict-log documentation in a separate PR to `main`. If historical probe code is unavailable, preserve the measured verdict without claiming executable evidence exists.

### Task 2.1: Spike B2 — End-to-End CPU Fallback Transport (reproducible evidence branch)

**Files:**
- Create and retain on branch `spikes`: `spikes/fallback/`.
- Modify on branch `spikes`: root `CMakeLists.txt` to build the probe.
- Verdict-only branches retain this task and its final verdict but do not merge probe code into `main`.

**Hypothesis:** After the copy-free Spike B chain failed, a fully preallocated CPU fallback can still deliver correct 1920×1080 frames through D3D11 readback → CPU staging → bgfx upload/compute/readback → bounded shared-memory IPC → custom Media Foundation source → Frame Server at 30fps. This spike measures a transport floor with synthetic compute; it does not substitute for Spike C or prove the final beauty pipeline budget.

**Interfaces:**
- Consumes: Spike A/A2's accepted single-consumer NV12 virtual-camera contract and Spike B's measured copy-path findings.
- Produces: **Verdict B2** — correctness, per-stage latency, full fallback transport latency, Frame Server delivery, sustained 1080p30 behavior, idle/recovery behavior, and cleanup evidence.

- [x] **Step 1: Pin the reproducible baseline**

Record the Windows-Camera upstream commit, toolchain, Iris Xe driver, bgfx port, media types, virtual-camera CLSID/name, and exact build/run commands. Preserve Microsoft license and attribution for derived source files.

- [x] **Step 2: Implement the allocation-free fallback transport**

Generate a deterministic 1080p frame in a D3D11 texture, read into a persistent staging buffer, upload into a bgfx-owned D3D12 texture, run a synthetic compute pass, read back into a persistent output buffer, and publish the latest complete frame through a fixed-size named shared-memory ring. No per-frame heap allocation is allowed after warm-up.

- [x] **Step 3: Instrument every transport stage**

Use QPC timestamps for D3D11 readback, CPU preparation, bgfx upload/compute/readback, ring publication, source acquisition/sample enqueue, and Frame Server/client delivery. Report p50/p95/p99/max and missed 33.3ms deadlines separately; never collapse missing stages into a claimed end-to-end total.

- [x] **Step 4: Prove the Frame Server path**

Use the pinned Windows-Camera source contract and the accepted single-consumer NV12 1920×1080/30 media type. Validate a diagnostic MF consumer, OBS, and Tencent Meeting individually. Producer loss must switch to black within 100ms and recovery must not require reopening the camera.

- [x] **Step 5: Validate correctness and sustained behavior**

Run 1,000 known-pattern frames with zero content/sequence errors, then a 10-minute 18,000-frame soak. Record achieved fps, duplicate/skipped sequences, missed deadlines, process CPU, and all per-stage latency distributions.

- [x] **Step 6: Record Verdict B2 and retain the probe**

Append: `Spike B2: <GO-WITH-CAVEAT|NO-GO> — correctness=<pass/fail>, d3d11_readback_p95_ms=<x>, bgfx_roundtrip_p95_ms=<x>, ring_publish_p95_ms=<x>, source_enqueue_p95_ms=<x>, frame_server_delivery_p95_ms=<x>, transport_total_p95_ms=<x|n/a>, soak_missed_deadlines=<n>, idle_black_ms=<x>, recovery=<yes/no>, clean_remove=<yes/no>, product_30fps_proven=no`.

Keep `spikes/fallback/` on branch `spikes` with reproduction instructions and upstream attribution. Do not merge probe code into `main`; create a separate verdict-only branch for durable documentation.

---
### Task 2.2: Spike B3 — Frame-Server Latency Decomposition and Pacing Comparison

**Files:**
- Modify and retain on branch `spikes`: `spikes/fallback/media-source/windows-camera-b2.patch`, `spikes/fallback/consumer/mf_main.cpp`, and `spikes/fallback/README.md`.
- Modify this plan with the B3 task and, only after measurement, its verdict-log entry.

**Hypothesis:** The B2 fallback's latency can be honestly decomposed across producer publication, Frame Server media-source acquisition, source copy/payload finalization, and client delivery. Comparing the existing bounded wait-for-next-sequence pacing with immediate-latest-frame pacing over the same 1,000-frame workload identifies whether request pacing—not a hidden or renamed boundary—changes the measured producer-to-client frame age.

**Interfaces:**
- Consumes: the retained B2 ring, marker/checksum contract, diagnostic MF consumer, and single-consumer virtual-camera contract.
- Produces: **Verdict B3** — two measured 1,000-frame pacing distributions, sequence/content validity, duplicate/skipped counts, explicit source-boundary availability, recovery/app validation for the winner, and a conditional soak result. It does not revise the B/B2 verdicts without measured evidence.

- [ ] **Step 1: Add activation-scoped pacing modes**

Keep `wait` as the default: it waits up to 35ms for a sequence newer than the last delivered sequence. Add `immediate`, which selects the latest complete sequence without the new-sequence wait and may duplicate that sequence. Read the probe-only cross-process mode flag once per media-source activation; do not perform per-frame filesystem or registry lookup. Document exact mode-file setting, client shutdown, and Frame Server release/restart steps: mode changes take effect only after applications and Frame Server release the old source. Preserve torn-slot rejection, stale-frame black fallback, idle-black timing, and producer recovery in both modes.

- [ ] **Step 2: Carry honest source diagnostics in the payload**

Preserve bytes 0..39 (`SFB2` marker, sequence, checksum, producer QPC, B2 source copy-complete QPC, published QPC). Add QPC fields after byte 40 for media-source request entry, complete sequence acquired, and source copy complete. Record sample enqueue only if the patched stream can write it before queueing without allocation or unsafe post-queue payload mutation; otherwise name the nearest safe boundary exactly and state the instrumentation limitation. Require the consumer to check its sample length before reading each added field. Do not change marker/checksum semantics or allocate per frame.

- [ ] **Step 3: Report segmented distributions and validity**

For every available segment report p50/p95/p99/max, with safe `n/a` output for an unavailable segment: producer→publish, published age at request, source wait, source copy, source-ready→enqueue (or the explicitly named nearest safe boundary), Frame Server/client delivery, and producer→client frame age. A published-age sample exists only when publication precedes request entry; count frames published after request separately rather than reporting a negative age. Report the declared pacing mode, B3-diagnostic sample count, sequence/content validity, duplicates, skips, idle-black frames, and recovery. Keep `--frames` and `--expect-recovery` compatible.

- [ ] **Step 4: Run matched 1,000-frame comparison and select conditionally**

On the pinned Iris Xe baseline, run 1,000 valid frames in each mode with the same producer workload. Record all distributions and validity counters; immediate-mode duplicates are valid observations, not content errors. A provisional winner requires zero marker/content errors in both runs and is the mode with the lower producer-to-client frame-age p95. Do not claim a winner if the inputs or diagnostics are incomplete.

- [ ] **Step 5: Validate the winner, then soak only if it clears the gate**

For the provisional winner, prove idle-black and producer recovery with `--expect-recovery`, then open OBS and Tencent Meeting separately and confirm the valid moving pattern. Only if the winner's 1,000-frame producer-to-client frame-age p95 is **≤33.3ms**, run a 10-minute soak and record its distributions, missed deadlines, duplicate/skipped sequences, and recovery. If p95 exceeds 33.3ms, skip the soak, retain the B/B2 NO-GO, and record the comparison without fabricating a GO.

- [ ] **Step 6: Clean up and record Verdict B3 after measurement**

Remove the mode file after the experiment and remove the virtual camera with the existing registrar procedure. Append only measured evidence in this exact form:

`Spike B3: <GO-WITH-CAVEAT|NO-GO> — wait_producer_to_client_p95_ms=<x>, immediate_producer_to_client_p95_ms=<x>, winner=<wait|immediate|none>, wait_validity=<pass/fail>, immediate_validity=<pass/fail>, winner_recovery=<yes/no>, winner_apps=<OBS/Tencent result>, winner_soak=<ran result|not-run p95 gate>, source_enqueue_boundary=<exact name|unavailable>, product_30fps_proven=no`.

Do not append this entry until measurements exist. Keep the evidence probe on its dedicated branch; no B3 code or result is product source.

---
### Task 2.3: Spike B4 — Asynchronous Media-Source Pacing

**Files:**
- Modify and retain on branch `spikes`: `spikes/fallback/media-source/windows-camera-b2.patch`, `spikes/fallback/consumer/mf_main.cpp`, and `spikes/fallback/README.md`.
- Modify this plan only with the measured B4 verdict; no probe code enters `main`.

**Hypothesis:** Moving producer waits out of `RequestSample` and onto a private serial Media Foundation work queue can remove synchronous request pacing from the Frame Server path and deliver 1,000 valid 1920×1080 frames at ≥29.5fps with producer-to-client frame-age p95 ≤33.3ms.

**Interfaces:**
- Consumes: the retained B2 ring and B3 payload diagnostics, plus the accepted single-consumer virtual-camera contract.
- Produces: **Verdict B4** — non-blocking request behavior, complete source timing, achieved client fps, producer-to-client frame age, sequence validity, and a conditional recovery/application/soak result.

- [x] **Step 1: Make `RequestSample` non-blocking**

Queue each request in a fixed 16-entry buffer and return immediately. Use a private serial work queue to wait on the producer frame-ready event and a 33ms deadline timer. Deliver the oldest request when a complete fresh sequence is available; deliver black on the deadline when input is absent or stale. Cancel waits and flush requests on pause, stop, and shutdown.

- [x] **Step 2: Measure the complete Source Reader loop**

Preserve the B3 payload fields and add a consumer-side count of valid B4 diagnostics. Measure elapsed time and achieved fps around the complete Source Reader loop. Release the Source Reader and source references before `IMFActivate::ShutdownObject`; releasing the Source Reader already shuts down its media source.

- [x] **Step 3: Run the 1,000-frame gate**

Require 1,000 valid frames, 1,000 B4 diagnostic frames, zero marker and duplicate errors, ≥29.5fps, and producer-to-client frame-age p95 ≤33.3ms. Run recovery, OBS, Tencent Meeting, and the 10-minute soak only after the automated gate passes.

- [x] **Step 4: Record Verdict B4**

Retain the reproducible probe only on branch `spikes`. Record the measured verdict in the log below and preserve the existing B/B2 NO-GO when either the fps or frame-age gate fails.

---


### Task 3: Spike C — Face Model iGPU DirectML Perf + Selection

**Files:**
- Create: `spikes/face/main.cpp` (evidence probe)
- Create: `spikes/face/CMakeLists.txt`
- Create: `spikes/face/models/` (candidate ONNX models; large binaries stay out of Git with acquisition metadata in the manifest)

**Hypothesis:** At least one lightweight face-landmark ONNX model runs on the Iris Xe iGPU via ONNX Runtime DirectML within ~5-8ms/frame at 1080p, with enough landmarks (≥68, ideally ~106-468) to drive natural face-reshape. (Spec §6.1 #5, §9 "face model final selection", §6.3 spike 3.)

**Interfaces:**
- Consumes: Task 0 env (ONNX RT DirectML on the iGPU).
- Produces: **Verdict C** — the **selected model** (name, landmark count, source, license) + measured per-frame latency on the iGPU + the fallback (lighter) model. This selection is consumed by the later v1 build plan's `core/face/` tasks.

- [ ] **Step 1: Gather candidate models**

Collect candidate ONNX face-landmark models (with license check — must be Apache-2.0/MIT-compatible):
- MediaPipe Face Landmarker (468 landmarks) exported to ONNX (Apache-2.0).
- A lighter 68-pt or 106-pt model (e.g., from an open, permissively-licensed source). Verify each model's license is compatible with Apache-2.; record it.

Place model files in `spikes/face/models/` (git-ignore large binaries).

- [ ] **Step 2: Write the benchmark harness**

`spikes/face/main.cpp`: load a model via `Ort::Session` with the DirectML execution provider (D3D12 device on the iGPU adapter, same as Spike B). Feed a 1080p test frame (NV12 or BGRA → convert to the model's input layout). Warm up 10 frames, then time 100 frames. Record `per_frame_ms` and peak working set.

- [ ] **Step 3: Run all candidates + record**

For each candidate: run the harness, record `{model, landmarks, license, per_frame_ms, vram_kb}`. Compute whether `per_frame_ms` fits the spec §2.2 face-ML budget line (~5-8ms).

- [ ] **Step 4: Visual quality sanity (landmarks on a real face)**

Run the top candidate(s) on a few real-face stills; overlay landmarks; confirm they land sensibly (eyes/mouth/jaw). This guards against a model that's fast but inaccurate (spec §6.1 #6). Record a subjective quality note (-3).

- [ ] **Step 5: Select + designate fallback**

Pick the **lightest model that meets the latency budget AND has sensible landmark placement**. Designate a **fallback** (even lighter, if the primary busts the budget on weaker iGPUs). Record both with rationale.

- [ ] **Step 6: Record Verdict C and preserve evidence**

Append to Verdict Log: `Spike C: GO — selected=<model name, N landmarks, license>, per_frame_ms_iGPU=<x>, quality=<0-3>, fallback=<model>`. If no candidate fits ~5-8ms with sensible landmarks, record NO-GO and flag (would force a heavier face-pipeline rethink). Submit the executable probe, strict manifest, reproduction instructions, model acquisition metadata, and measured result in an evidence PR to `spikes`; do not remove it after the verdict. Submit only the verdict-log documentation in a separate PR to `main`.

---

## Verdict Log

*(Executor appends each spike's verdict here as tasks complete. GO or an explicitly accepted GO-WITH-CAVEAT may proceed; an unaccepted NO-GO stops planning.)*

- Task 0 env gate: **GO** — `Windows build 26300`, `Intel Iris Xe 32.0.101.7088`, `MFCreateVirtualCamera` available, bgfx `1.129.8940-496#1` selected Direct3D 12, ONNX Runtime DirectML `1.24.4` returned finite output `2` with CPU fallback disabled, `CMake 4.4.3`, `MSVC 19.51.36256`, `Windows SDK 10.0.26100.0`, vcpkg baseline `a4f7741c53c97354fc43222fea17955c657dd549`; `ENV_GATE_PASS` observed.
- Spike A/A2: **GO-WITH-CAVEAT** — on 2026-11-08 the product contract was explicitly narrowed to one `MFCreateVirtualCamera` device with one active consuming application at a time; concurrent consumers are deferred post-v1. Reference=`microsoft/Windows-Camera@626f8b19c5f367602f2e89c6b314573d3776c9df`; apps=`OBS Studio 33.0.0, Tencent Meeting 3.47.2.405`; single-consumer OBS=`1920x1080 NV12, 30fps, 360-frame recording, zero OBS render/output skips`; single-consumer Tencent Meeting=`device selected and moving frames rendered`; 10-minute run=`18,000 samples at 30.00fps`; idle black=`yes`; producer recovery=`yes`; app restart/reboot persistence/clean remove=`yes`; privileges=`admin install/remove for HKLM COM, non-admin runtime`. The accepted v1 contract was proven and unblocked Spike B/C at the completion of A2.
  - Corrective plan and full evidence: [Spike A2 — Frame Server multi-consumer recovery](2026-11-06-phase0-spike-a2-multiconsumer.md).
  - Deferred limitation: real applications cannot concurrently consume the same device. OBS-first left Tencent Meeting empty; Tencent-first left OBS black. A2 confirmed the failure occurs in Frame Server/client access-mode arbitration before the second source reaches streaming. Diagnostic consumers did share successfully; internal handoff was `0.200ms` average / `0.274ms` p95 and Frame Server/client delivery was `27.666ms` average / `28.390ms` p95. This evidence informs a future multi-consumer architecture decision but is not a v1 acceptance gate.
- Spike B: **NO-GO** — `copy_free_gpu=no`; `formats=D3D12→D3D11 {BGRA8,RGBA8,R8} only` (`NV12` failed and D3D11→D3D12 failed for all four tested formats); `fence_protocol=ok`; `sync_p95_ms=3.107`; `dml_binding=copy` (direct resource I/O binding exited with `0xC0000005`); `bgfx_native=no` (native imported-resource path exited with `0xC0000005`; bgfx-owned compute did run); `camera_gpu_sample=no` (D3D11 NV12 wrapped as an `IMFSample`, but Frame Server preservation was not proven; D3D12 NV12 returned `E_NOINTERFACE`); `fallback_readback_ipc_ms=4.502 p95` over 1,000 frames (`6.416ms` p95 in the 10-minute soak); `same_adapter=yes`; `total_p95_ms=n/a`; `fits_30fps=no`. On Iris Xe driver `32.0.101.7092`, 1,000 known-pattern round trips passed per-frame validation and the 18,000-frame fallback soak recorded four missed 33.3ms deadlines. The measured partial-copy component sum was `8.855ms` p95, but excludes capture/readback-to-DirectML, full-frame upload into bgfx, and Frame Server delivery, so it is not an end-to-end total. The planned GPU-resident chain is unsupported; Spike C and the v1 build plan remain gated.
- Spike B2: **NO-GO** — `correctness=pass`; `d3d11_readback_p95_ms=6.375`; `bgfx_roundtrip_p95_ms=10.932`; `ring_publish_p95_ms=0.359`; producer-side `transport_total_p95_ms=22.014`; real camera `source_enqueue_p95_ms=16.439`, `frame_server_delivery_p95_ms=48.826`, and producer-to-client `p95_ms=78.099`; 1,000 valid MF frames had `0` skipped, `23` duplicate, and `0` marker/content errors; idle black began in `46.442ms`; producer recovery without reopening=`yes`; OBS Studio 33.0.0 and Tencent Meeting 3.47.2.405 rendered the full moving 1920×1080 pattern separately; clean remove=`yes`; `product_30fps_proven=no`. The fallback proves synthetic 1080p30 throughput and application compatibility but exceeds the 33.3ms product budget before face ML and production beauty shaders. Full evidence: [Spike B/B2 — GPU handoff and CPU fallback](2026-11-10-phase0-spike-b2-fallback.md).
- Spike B3: **NO-GO** — `wait_producer_to_client_p95_ms=79.820`, `immediate_producer_to_client_p95_ms=57.635`, `winner=immediate`, `wait_validity=pass`, `immediate_validity=pass`, `winner_recovery=yes`, `winner_apps=OBS moving pattern rendered / Tencent Meeting moving pattern rendered`, `winner_soak=not-run p95 gate`, `source_enqueue_boundary=unavailable`, `product_30fps_proven=no`.
- Spike B4: **NO-GO** — `async_request_queue=yes`; `mf_frames=1000`; `mf_b4_diagnostic_frames=1000`; `mf_fps_achieved=26.644`; `producer_to_client_frame_age_p95_ms=48.758`; `source_wait_p95_ms=63.205`; `producer_fps=29.992`; producer `transport_total_p95_ms=22.920`; `skipped_sequences=125`; `duplicate_sequences=0`; `marker_errors=0`; recovery/apps/soak=`not-run automated gate failed`; `product_30fps_proven=no`. Asynchronous `RequestSample` removed the synchronous wait but did not meet either the 29.5fps or 33.3ms gate.
- [Spike C — face model verdict]

## Gate to v1 Build Plan

The detailed v1 build plan is **blocked**. Spike A/A2 established an accepted single-consumer virtual-camera contract, but Spikes B, B2, B3, and B4 all returned **NO-GO** for the selected 1080p30 iGPU handoff architecture. Spike C may be measured independently, but it cannot unblock v1 planning.

Before writing `docs/plans/<date>-v1-build.md`, make an explicit product/architecture decision that changes the failed constraint—such as the performance promise, default resolution, or transport architecture—and validate the replacement handoff with a GO or explicitly accepted GO-WITH-CAVEAT. The eventual build plan must be informed by:
- the custom media-source registration, IPC, lifecycle, accepted formats, and app compatibility confirmed in Spike A,
- the measured GPU and CPU handoff limits from Spikes B through B4,
- the selected face model and measured latency from Spike C.

Only after those gates are accepted should a second planning pass implement the modules in the [v1 MVP design](../specs/2026-08-15-v1-mvp-design.md) §3.

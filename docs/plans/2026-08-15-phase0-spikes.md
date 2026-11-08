# SparkFace Phase-0 Spikes Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** De-risk the three load-bearing unknowns of SparkFace v1 (Windows 11 Media Foundation user-mode virtual camera, D3D11↔D3D12 GPU interop and virtual-camera handoff, face-model iGPU/DirectML performance) via throwaway feasibility probes with hard pass/fail criteria — before writing the detailed v1 build plan.

**Architecture:** Three independent throwaway spikes (A: `MFCreateVirtualCamera` + custom media source, B: DXGI cross-device sharing + explicit fence synchronization + camera handoff, C: ONNX/DirectML face-model benchmark) plus a shared Task 0 build-environment gate. Each spike produces a **verdict + measured numbers**, NOT reusable code — probe code is discarded after the verdict. Spike outcomes feed a second writing-plans pass that produces the detailed v1 build plan.

**Tech Stack:** C++17, CMake, Windows 11 build 22000+, Windows SDK, Media Foundation virtual-camera API, bgfx (D3D12 backend), ONNX Runtime + DirectML EP, Intel Iris Xe iGPU reference machine.

**Spec:** [v1 MVP design](../specs/2026-08-15-v1-mvp-design.md) (§6.3 mandates these spikes before v1 implementation)

## Global Constraints

- **Execution environment:** Windows 11 build 22000+ with Visual Studio 2022 or newer, Windows SDK 10.0.22000.0+, CMake, and an **iGPU reference machine (Intel Iris Xe)**. This plan CANNOT run on Windows 10 or macOS. All `Run:` commands are Windows (PowerShell/cmd).
- **License:** Apache-2.0 (core). Spikes are throwaway — probe code is NOT merged into the product; it lives in a temp/scratch location or an isolated branch and is deleted after the verdict.
- **Language:** C++17; build with CMake + `CMakePresets.json`.
- **Render backend:** bgfx pinned to its **D3D12** backend on Windows (to align with DirectML, which is D3D12-based). See spec §2.1.
- **Perf target:** iGPU 30fps = **≤33.3ms/frame** budget (30fps frame time = 33.33ms). Spike B/C measure components of this budget. See spec §2.2.
- **Dependencies (mixed acquisition, spec §4.1):** bgfx via vcpkg port or FetchContent; ONNX Runtime + DirectML via official prebuilt redist (`Microsoft.ML.OnnxRuntime.DirectML` NuGet unpacked, or the C/C++ redist zip from onnxruntime releases); small libs (gtest) via FetchContent.
- **Output format/resolution:** target 1080p, NV12 primary / BGRA8 fallback. Spike A confirms what the Frame Server virtual-camera path and target consumers accept; Spike B confirms which formats can stay GPU-resident across D3D11↔D3D12 boundaries.
- **Commits:** per git_rules, implementer commits on a `feat/*` branch if agreed upfront at execution start; **push needs separate explicit user consent**. Spike verdicts/findings are recorded in this plan's "Verdict Log" section (added by the executor) regardless of whether probe code is kept.
- **Decision baseline:** v1 targets Windows 11 build 22000+ and uses the user-mode `MFCreateVirtualCamera` API. A kernel AVStream fallback is out of scope; a Spike A NO-GO stops the plan and triggers a product/OS-support decision instead of silently adding a driver.

---

### Task 0: Windows 11 Build Environment + iGPU Reference Gate

**Files:**
- Create: `CMakeLists.txt` and `CMakePresets.json` (throwaway Phase-0 root)
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

`spikes/env/main.cpp` (throwaway): create a D3D12 device on the Intel adapter; initialize bgfx with that external D3D12 context and a hidden Win32 surface, transfer device ownership to bgfx, and run one `bgfx::frame`; create an ONNX Runtime session with CPU fallback disabled and execute a pinned Mul model through DirectML; call `MFStartup`; compile/link against `mfplat.lib` and `mfsensorgroup.lib`; verify `MFCreateVirtualCamera` is exported by `mfsensorgroup.dll`. Print toolchain versions, OS build, adapter identity, provider/result, and a pass line. Actual camera registration belongs to Spike A.

- [x] **Step 5: Build + run**

Run only the repository entry point:
```powershell
.\scripts\build-phase0-env.ps1 -Run
```
Expected: pinned vcpkg bootstrap, successful dependency restore/configure/build, Windows build ≥22000, Intel Iris Xe, D3D12 bgfx frame, `MFCreateVirtualCamera`, a finite DirectML model result with CPU fallback disabled, and `ENV_GATE_PASS`. Missing any item is a blocker.

Observed PASS on the Iris Xe reference workstation after disabling Windows Smart App Control, which does not support per-application exceptions. The gate selected bgfx Direct3D 12, loaded `MFCreateVirtualCamera`, executed the pinned ONNX Mul model through `DmlExecutionProvider` with CPU fallback disabled, returned finite output `2`, and printed `ENV_GATE_PASS`.

- [x] **Step 6: Record env gate + commit**

Append `env gate PASS — os_build=<n>, adapter=<name>, MFCreateVirtualCamera=available, bgfx=D3D12, DirectML=<version/result>, cmake=<version>, msvc=<version>, windows_sdk=<version>, vcpkg_baseline=<commit>` to the Verdict Log. Commit the env scaffolding on the `feat/*` branch:
```bash
git add .gitignore CMakeLists.txt CMakePresets.json vcpkg.json scripts/bootstrap-vcpkg.ps1 scripts/build-phase0-env.ps1 spikes/env docs/plans/2026-08-15-phase0-spikes.md
git commit -m "spike(env): gate Windows 11 virtual camera and GPU stack"
```

---

### Task 1: Spike A — Windows 11 User-Mode Virtual Camera Feasibility (throwaway)

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

- [x] **Step 8: Record Verdict A + throw away**

Append: `Spike A: <GO|GO-WITH-CAVEAT|NO-GO> — apps=<list>, 1080p30=<yes/no>, formats=<list>, feeder_to_sample_ms=<x>, cpu=<x%>, idle_black=<yes/no>, restart=<yes/no>, reboot_persistence=<yes/no>, clean_remove=<yes/no>, admin_required=<yes/no>, two_consumers=<yes/no>`.

Delete `spikes/virtualcam/` probe code (or leave it on an isolated branch, NOT merged). Commit the verdict-log update only:
```bash
git add docs/plans/2026-08-15-phase0-spikes.md
git commit -m "spike(A): Windows user-mode virtual camera verdict"
```

---

### Task 2: Spike B — D3D11↔D3D12 Sharing, Fence Synchronization, and Camera Handoff (throwaway)

**Files:**
- Create: `spikes/sharing/main.cpp` (throwaway probe)
- Create: `spikes/sharing/CMakeLists.txt`

**Hypothesis:** On the Iris Xe iGPU, frames can remain GPU-resident through MF-capture-D3D11 → DirectML-D3D12 → bgfx-D3D12 using NT shared handles, explicit shared fences, and a documented resource-state contract. The Windows 11 custom media source can then consume a GPU-backed `IMFSample`; if Frame Server rejects that path, one measured GPU→CPU readback plus the Spike A shared-memory handoff still fits the 30fps budget. (Spec §2.1, §6.1 #2, §6.3 spike 2.)

**Interfaces:**
- Consumes: Task 0 GPU stack and Spike A's accepted virtual-camera media types/IPC contract. It does not consume Spike A probe code.
- Produces: **Verdict B** — GO / GO-WITH-CAVEAT / NO-GO + measured: copy-free GPU path, working shared formats, D3D11↔D3D12 fence and resource-state protocol, synchronization cost, DirectML I/O-binding behavior, bgfx native-resource access, GPU-backed virtual-camera sample support, fallback readback+IPC cost, same-adapter confirmation, and total cost against ≤33.3ms.

- [ ] **Step 1: Force all GPU components onto the iGPU adapter**

Enumerate adapters and select the Intel Iris Xe by LUID. Create the D3D11 capture device, DirectML D3D12 device/queue, and bgfx D3D12 device on that adapter. Assert matching adapter LUIDs. A component that cannot be placed on the same adapter is a NO-GO finding for the copy-free design.

- [ ] **Step 2: Confirm resource-sharing formats and APIs**

For NV12, BGRA8, RGBA8, and R8, create D3D11 textures with `D3D11_RESOURCE_MISC_SHARED_NTHANDLE`, export with `IDXGIResource1::CreateSharedHandle`, and open on D3D12 with `ID3D12Device::OpenSharedHandle`. Record creation/open success, required flags, row/layout constraints, and whether a shared heap or committed resource is exposed. Do not use the nonexistent `OpenSharedHandle1` API.

- [ ] **Step 3: Define and prove synchronization/state ownership**

Use an `ID3D12Fence` shared handle and `ID3D11Device5::OpenSharedFence` where supported. Define, in the probe, which component owns the resource, the fence value before/after each hop, and the D3D12 state on entry/exit. Run a known-pattern round trip for 1,000 frames and validate contents after GPU completion. Do not treat `IDXGIKeyedMutex` as the D3D12 synchronization primitive.

- [ ] **Step 4: Prove DirectML and bgfx interop**

Bind the shared resource to ONNX Runtime DirectML through supported device-memory/I/O-binding APIs and run a trivial model without `Map` or a staging upload in the per-frame GPU path. Import or access the resulting D3D12 resource through bgfx's supported native-resource interface and run one compute pass. Record any hidden copy or API limitation as a caveat or NO-GO.

- [ ] **Step 5: Test GPU-backed Media Foundation samples**

Wrap the final DXGI surface with `MFCreateDXGISurfaceBuffer`, place it in an `IMFSample`, and deliver it from a reduced custom media source using the media type accepted by Spike A. Verify the camera in OBS and inspect whether Frame Server preserves the GPU surface or forces a copy/conversion. Record support and cost per format.

- [ ] **Step 6: Measure the CPU fallback only if required**

If the GPU-backed sample path is unsupported or unstable, read the final 1080p frame into a persistently allocated staging/readback buffer and publish it through the bounded shared-memory ring from Spike A. Measure readback, copy/IPC, and media-source delivery separately. No per-frame allocation.

- [ ] **Step 7: Measure sustained end-to-end overhead**

After warm-up, run at least 1,000 frames and then a 10-minute 1080p30 soak. Record per-hop fence wait/signal cost, DirectML binding/inference floor, bgfx pass, virtual-camera handoff, p50/p95/p99 frame cost, dropped frames, and total against the 33.3ms envelope.

- [ ] **Step 8: Record Verdict B + throw away**

Append: `Spike B: <verdict> — copy_free_gpu=<yes/no>, formats=<list>, fence_protocol=<ok/fail>, sync_p95_ms=<x>, dml_binding=<direct/copy>, bgfx_native=<yes/no>, camera_gpu_sample=<yes/no>, fallback_readback_ipc_ms=<x|n/a>, same_adapter=<yes/no>, total_p95_ms=<x>, fits_30fps=<yes/no>`.

Delete `spikes/sharing/`. Commit the verdict-log update:
```bash
git add docs/plans/2026-08-15-phase0-spikes.md
git commit -m "spike(B): GPU sharing and virtual-camera handoff verdict"
```

---

### Task 3: Spike C — Face Model iGPU DirectML Perf + Selection (throwaway)

**Files:**
- Create: `spikes/face/main.cpp` (throwaway probe)
- Create: `spikes/face/CMakeLists.txt`
- Create: `spikes/face/models/` (candidate ONNX models, git-ignored if large)

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

- [ ] **Step 6: Record Verdict C + throw away**

Append to Verdict Log: `Spike C: GO — selected=<model name, N landmarks, license>, per_frame_ms_iGPU=<x>, quality=<0-3>, fallback=<model>`. If no candidate fits ~5-8ms with sensible landmarks, record NO-GO and flag (would force a heavier face-pipeline rethink). Delete `spikes/face/` probe code. Commit verdict-log update:
```bash
git add docs/plans/2026-08-15-phase0-spikes.md
git commit -m "spike(C): face model iGPU DirectML perf + selection verdict"
```

---

## Verdict Log

*(Executor appends each spike's verdict here as tasks complete. All three verdicts GO = proceed to the second writing-plans pass for the detailed v1 build plan. Any NO-GO = stop and report to the user before planning.)*

- Task 0 env gate: **GO** — `Windows build 26300`, `Intel Iris Xe 32.0.101.7088`, `MFCreateVirtualCamera` available, bgfx `1.129.8940-496#1` selected Direct3D 12, ONNX Runtime DirectML `1.24.4` returned finite output `2` with CPU fallback disabled, `CMake 4.4.3`, `MSVC 19.51.36256`, `Windows SDK 10.0.26100.0`, vcpkg baseline `a4f7741c53c97354fc43222fea17955c657dd549`; `ENV_GATE_PASS` observed.
- Spike A: **NO-GO** — reference=`microsoft/Windows-Camera@626f8b19c5f367602f2e89c6b314573d3776c9df`; apps=`OBS Studio 33.0.0, Tencent Meeting 3.47.2.405`; `1080p30=yes`; formats=`NV12 (YUY2 also advertised)`; 10-minute run=`18000 samples, 30.00 fps, 3 duplicate frames`; feeder-to-sample latency=`57.99 ms average, 65.14 ms p95`; CPU=`feeder 1.27% of one core, Frame Server 15% sampled`; idle black=`yes, 66.32 ms`; feeder recovery without reopen=`yes`; app restart=`yes`; reboot persistence=`yes`; clean remove=`yes`; privileges=`current-user camera, but probe's HKLM COM registration made install/remove admin-only`; single-consumer OBS=`1920x1080 NV12, 30 fps, 360-frame recording with zero OBS render/output skips`; single-consumer Tencent Meeting=`device selected and frames rendered`; two consumers=`no` — OBS-first caused Tencent Meeting “camera capture data exception”; Tencent-first left OBS receiving only idle frames. Because the v1 contract has not explicitly excluded concurrent consumers, Step 7 classifies this as NO-GO. Probe code deleted after recording this verdict.
  - Corrective plan: [Spike A2 — Frame Server multi-consumer recovery](2026-11-06-phase0-spike-a2-multiconsumer.md).
  - Spike A2: **NO-GO** — shared attribute=`stream and descriptor confirmed`; diagnostic consumers=`two concurrent 1080p30 consumers passed`; internal handoff=`0.200 ms average, 0.274 ms p95`; Frame Server/client delivery=`27.666 ms average, 28.390 ms p95`; 10-minute run=`18,000 frames per consumer at 30fps`; idle black=`yes, <=50.7 ms`; producer recovery=`yes`; apps=`OBS Studio 33.0.0, Tencent Meeting 3.47.2.405`; single consumer=`both apps render the moving pattern`; activation orders=`fail/fail` — OBS first left Tencent on an empty preview, Tencent first left OBS black, and the second client never reached a running media-source instance; failing layer=`Frame Server/client access-mode arbitration before media-source streaming`; restart=`yes`; reboot=`not needed, registration/lifetime unchanged`; clean remove=`yes`; privileges=`admin install/remove for HKLM COM, non-admin runtime`; upstream=`microsoft/Windows-Camera@626f8b19c5f367602f2e89c6b314573d3776c9df`. The mandatory real-app multi-consumer contract remains unsatisfied; Spike B/C and the v1 build plan remain blocked.
- [Spike B — GPU sharing/fence/camera-handoff verdict]
- [Spike C — face model verdict]

## Gate to v1 Build Plan

After Spikes A/B/C all return **GO** (or GO-WITH-CAVEAT with explicitly accepted product constraints): invoke `superpowers:writing-plans` again to write the **detailed v1 build plan** (`docs/plans/<date>-v1-build.md`), now informed by:
- the custom media-source registration, IPC, lifecycle, accepted formats, and app compatibility confirmed in Spike A,
- the shareable format set, fence/resource-state protocol, DirectML/bgfx interop, and GPU-or-CPU camera handoff costs measured in Spike B,
- the selected face model + measured latency from Spike C.

That second plan implements the modules in the [v1 MVP design](../specs/2026-08-15-v1-mvp-design.md) §3, TDD task-by-task.

# SparkFace Phase-0 Spikes Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** De-risk the three load-bearing unknowns of SparkFace v1 (kernel AVStream virtual-camera driver, cross-device GPU texture-sharing + driver readback, face-model iGPU/DirectML perf) via throwaway feasibility probes with hard pass/fail criteria — before writing the detailed v1 build plan.

**Architecture:** Three independent throwaway spikes (A: kernel minidriver, B: DXGI cross-device sharing + readback, C: ONNX/DirectML face-model benchmark) plus a shared Task 0 build-environment gate. Each spike produces a **verdict + measured numbers**, NOT reusable code — probe code is discarded after the verdict. Spike outcomes feed a second writing-plans pass that produces the detailed v1 build plan.

**Tech Stack:** C++17, CMake, Windows 10/11 + WDK, bgfx (D3D12 backend), ONNX Runtime + DirectML EP, Media Foundation, Intel Iris Xe iGPU reference machine.

**Spec:** `spec/2026-08-15-v1-mvp-design.md` (§6.3 mandates these spikes before v1 implementation)

## Global Constraints

- **Execution environment:** Windows 10/11 with WDK + VS 2022 + CMake + an **iGPU reference machine (Intel Iris Xe)**. This plan CANNOT run on macOS. All `Run:` commands are Windows (PowerShell/cmd).
- **License:** Apache-2.0 (core). Spikes are throwaway — probe code is NOT merged into the product; it lives in a temp/scratch location or an isolated branch and is deleted after the verdict.
- **Language:** C++17; build with CMake + `CMakePresets.json`.
- **Render backend:** bgfx pinned to its **D3D12** backend on Windows (to align with DirectML, which is D3D12-based). See spec §2.1.
- **Perf target:** iGPU 30fps = **≤33.3ms/frame** budget (30fps frame time = 33.33ms). Spike B/C measure components of this budget. See spec §2.2.
- **Dependencies (mixed acquisition, spec §4.1):** bgfx via vcpkg port or FetchContent; ONNX Runtime + DirectML via official prebuilt redist (`Microsoft.ML.OnnxRuntime.DirectML` NuGet unpacked, or the C/C++ redist zip from onnxruntime releases); small libs (gtest) via FetchContent.
- **Output format/resolution:** target 1080p, NV12 primary / ARGB fallback; DXGI cross-device sharing has format constraints — Spike B confirms the shareable format set. See spec §2.2.
- **Commits:** per git_rules, implementer commits on a `feat/*` branch if agreed upfront at execution start; **push needs separate explicit user consent**. Spike verdicts/findings are recorded in this plan's "Verdict Log" section (added by the executor) regardless of whether probe code is kept.

---

### Task 0: Windows Build Environment + iGPU Reference Gate

**Files:**
- Create: `CMakeLists.txt` (throwaway root, just to gate the toolchain)
- Create: `CMakePresets.json`
- Create: `spikes/env/main.cpp` (trivial: prints adapter name + runs one bgfx clear + one DirectML op)
- Create: `spikes/env/CMakeLists.txt`

**Interfaces:**
- Consumes: none (first task)
- Produces: a working Windows build env that can compile+link C++17/CMake, drive bgfx (D3D12), drive ONNX Runtime DirectML, and identify the iGPU adapter. All later spikes depend on this.

- [ ] **Step 1: Install prerequisites on the Windows box**

Install (if absent): Visual Studio 2022 ("Desktop development with C++" + "Universal Windows Platform development" workloads), WDK (matching Windows target), CMake ≥3.27, Git, vcpkg (bootstrap at `C:\vcpkg`).

- [ ] **Step 2: Identify the iGPU reference machine + adapter**

Run PowerShell:
```powershell
Get-CimInstance Win32_VideoController | Select-Object Name, DriverVersion, AdapterRAM
```
Record the Intel Iris Xe adapter name. This machine is the perf reference for Spikes B and C. If only a dGPU exists, STOP and flag to the user — the iGPU-smooth target cannot be validated without an iGPU.

- [ ] **Step 3: Fetch deps (ONNX Runtime DirectML redist + bgfx)**

Create `CMakePresets.json` with a `windows-x64-debug` preset (generator "Ninja", MSVC toolset). Acquire:
- ONNX Runtime DirectML redist: download the C/C++ `onnxruntime-win-x64-dml-*` zip from `https://github.com/microsoft/onnxruntime/releases` (pick the DirectML build), unpack to `spikes/env/ort-dml/`.
- bgfx: `vcpkg install bgfx` (or FetchContent a known-good bgfx CMake wrapper).

- [ ] **Step 4: Write the trivial gate program**

`spikes/env/main.cpp` (throwaway): create a D3D12 device, init bgfx with the D3D12 backend on the iGPU adapter, run one `bgfx::frame`; init an ONNX Runtime DirectML `OrtEnv` + enumerate providers (assert "DirectML" available). Print the adapter name and a pass line.

- [ ] **Step 5: Build + run**

Run:
```powershell
cmake --preset windows-x64-debug
cmake --build --preset windows-x64-debug
.\build\spikes\env\Debug\env_gate.exe
```
Expected: prints the Iris Xe adapter name + "DirectML provider available". If "DirectML provider available" is missing, STOP — DirectML on this iGPU is a blocker (spec §6.1 #5/#9).

- [ ] **Step 6: Record env gate + commit**

Append a one-line verdict ("env gate PASS, Iris Xe adapter=<name>, DirectML available") to the plan's Verdict Log. Commit the env scaffolding on the `feat/*` branch:
```bash
git add spikes/env CMakeLists.txt CMakePresets.json
git commit -m "spike(env): windows build env gate (iGPU + DirectML + bgfx)"
```

---

### Task 1: Spike A — Kernel AVStream Minidriver Feasibility (throwaway)

**Files:**
- Create: `spikes/driver/` (WDK driver project, throwaway — NOT merged into `driver/virtualcam/`)
- Create: `spikes/driver/testsign.ps1` (test-sign + install script)
- Create: `spikes/driver/push_frame.cpp` (user-mode frame pusher via DeviceIoControl)

**Hypothesis:** A minimal AVStream minidriver can be written, test-signed, installed, and recognized by OBS as a camera at 30fps, fed by user-mode frames pushed via `DeviceIoControl`, emitting black frames when no feeder runs. (Spec §2.1, §6.1 #1, §6.3 spike 1.)

**Interfaces:**
- Consumes: Task 0 build env (WDK installed).
- Produces: **Verdict A** — GO / GO-WITH-CAVEAT / NO-GO + measured: (a) does OBS enumerate the device, (b) does OBS display pushed frames at 30fps, (c) does the test-signing flow complete, (d) black-frame-when-idle behavior works, (e) any BSODs. This verdict determines whether the kernel-driver path is viable for v1.

- [ ] **Step 1: Clone the WDK AVStream sample as the skeleton**

From the Windows-driver-samples repo, take the `video/avstream/avshws` (AVStream hardware-simulation) sample as the starting skeleton — it already simulates a capture device. Copy it to `spikes/driver/`.

- [ ] **Step 2: Strip to a single capture pin + ring-buffer fill**

Reduce the sample to one capture pin (NV12, 1920×1080). Replace the hardware-simulation fill with a frame source driven by a user-mode IOCTL: expose a `IOCTL_SPARKFACE_PUSH_FRAME` that copies a caller-supplied 1080p NV12 buffer into the pin's allocated frame buffer. When no IOCTL has arrived within 100ms, fill the frame with black (spec §5.3 engine-off behavior).

- [ ] **Step 3: Add the .inf + catalog**

Create a minimal `.inf` matching the driver's device hardware ID (a root-enumerated device for the spike — `Root\SparkFaceSpike`). Generate the catalog with `inf2cat` (no real signature yet — test-sign next).

- [ ] **Step 4: Test-sign the driver**

Enable test-signing on the dev box: `bcdedit /set testsigning on` (reboot). Build the driver (WDK, `msbuild`/`Build.exe`). Test-sign with a local test cert:
```powershell
.\signtool sign /fd SHA256 /s My /n "SparkFace Test" driver.sys
inf2cat /driver:. /os:10_x64
signtool sign /fd SHA256 /s My /n "SparkFace Test" driver.cat
```

- [ ] **Step 5: Install the device**

Create the root device and install:
```powershell
pnputil /add-driver driver.inf /install
# or use devcon: devcon install driver.inf Root\SparkFaceSpike
```

- [ ] **Step 6: Write the user-mode frame pusher**

`spikes/driver/push_frame.cpp`: open the device, build a test-pattern 1080p NV12 frame, `DeviceIoControl(IOCTL_SPARKFACE_PUSH_FRAME, frame, ...)` at 30fps in a loop. Measure achieved push rate.

- [ ] **Step 7: Verify OBS recognition + display**

Launch OBS → add a "Video Capture Device" source → confirm "SparkFace Spike" appears → select → confirm the test-pattern frames render at ~30fps. Then kill the pusher (leave device installed) → confirm OBS shows black frames (not a freeze/error) within ~100ms.

- [ ] **Step 8: Verify no BSOD across the run**

Check Windows reliability/dispatcher for any bugcheck during Steps 5-7. `Get-WinEvent -LogName System | Where-Object Id -eq 1001` (bugcheck) should be empty for the spike window.

- [ ] **Step 9: Record Verdict A + throw away**

Append to Verdict Log: `Spike A: <GO|GO-WITH-CAVEAT|NO-GO> — OBS enumerate=<yes/no>, 30fps display=<yes/no>, test-sign flow=<ok/fail>, idle black frames=<yes/no>, BSOD=<none/N count>`. Delete `spikes/driver/` probe code (or leave on an isolated branch, NOT merged). Commit the verdict-log update only:
```bash
git add plans/2026-08-15-phase0-spikes.md
git commit -m "spike(A): kernel AVStream driver feasibility verdict"
```

---

### Task 2: Spike B — Cross-Device Texture Sharing + Driver Readback (throwaway)

**Files:**
- Create: `spikes/sharing/main.cpp` (throwaway probe)
- Create: `spikes/sharing/CMakeLists.txt`

**Hypothesis:** On the Iris Xe iGPU, a texture can flow copy-free across the chain MF-capture-D3D11 → DirectML-D3D12 → bgfx-D3D12 via DXGI shared handles (NT handle + keyed mutex), and the final frame can be GPU→CPU read back + pushed via a stub IOCTL at 30fps within the budget. (Spec §2.1, §6.1 #2, §6.3 spike 2.)

**Interfaces:**
- Consumes: Task 0 env (bgfx D3D12 + ONNX RT DirectML on the iGPU). Spike A's real driver is NOT required — use a stub shared-surface in place of the kernel driver to keep this spike independent.
- Produces: **Verdict B** — GO / GO-WITH-CAVEAT / NO-GO + measured: (a) copy-free round-trip confirmed (pattern written on device X read correctly on device Y, no per-frame CPU copy in the GPU path), (b) keyed-mutex sync cost per hop (ms), (c) final GPU→CPU readback cost @1080p (ms), (d) which DXGI formats are shareable across the D3D11↔D3D12 hops, (e) multi-GPU same-adapter confirmation (all three devices on the Iris Xe adapter), (f) total GPU-pipeline+readback cost vs the ≤33.3ms budget.

- [ ] **Step 1: Force all D3D devices onto the iGPU adapter**

Enumerate adapters, pick the Intel Iris Xe by name (from Task 0 Step 2). Create the D3D11 capture device, the DirectML D3D12 device, and the bgfx D3D12 device **all on that same adapter** (spec §6.1 #7). If you cannot place them on the same adapter, that is a NO-GO finding for hybrid laptops — record it.

- [ ] **Step 2: Confirm the shareable format set**

For candidate formats (NV12, BGRA8, RGBA8, R8), attempt `ID3D11Device::CreateTexture2D` with `D3D11_RESOURCE_MISC_SHARED_NTHANDLE | KEYEDMUTEX` and open on the D3D12 side via `ID3D12Device::OpenSharedHandle1`. Record which formats succeed — this is the format set the v1 output/intermediate textures must use (spec §2.2).

- [ ] **Step 3: Write the copy-free round-trip probe**

`spikes/sharing/main.cpp`: write a known pattern (e.g., per-pixel coordinate) into a shared NV12/BGRA texture on the D3D11 capture device → acquire keyed mutex → read on the DirectML D3D12 device (run a trivial ORT session reading the texture as input tensor) → read on the bgfx D3D12 device (run one compute pass) → assert the pattern is intact at the end (no CPU copy was made — verify by timing + by checking no `Map`/staging-buffer CPU touch in the hot path).

- [ ] **Step 4: Measure keyed-mutex + sharing overhead**

Time N=1000 iterations of the acquire/release across the 2-3 hops; divide for per-frame cost. Record `keyed_mutex_per_hop_ms`.

- [ ] **Step 5: Measure the final GPU→CPU readback + stub-IOCTL push**

After the bgfx compute pass, read the output texture back to CPU (`Map` a staging texture) and write the bytes into a stub ring buffer (simulating the DeviceIoControl push from Spike A). Time it at 1080p NV12. Record `readback_plus_push_ms`.

- [ ] **Step 6: Confirm bgfx native handle extraction works**

Use bgfx's native-handle interop to get the underlying D3D12 resource from a bgfx texture (the v1 driver handoff depends on this). If bgfx cannot expose the native D3D12 handle for sharing, record as a GO-WITH-CAVEAT (the v1 design's handoff would need a workaround).

- [ ] **Step 7: Sum against the budget**

Sum measured (capture-simulated ~2-4 + keyed-mutex + DirectML-trivial + bgfx-trivial + readback) and compare to the spec §2.2 budget table. Record whether the GPU-pipeline+readback portion fits the iGPU 30fps envelope. (Face ML and beauty passes are measured in Spike C / a later build; here we measure the sharing/overhead floor.)

- [ ] **Step 8: Record Verdict B + throw away**

Append to Verdict Log: `Spike B: <verdict> — copy_free=<yes/no>, formats_shared=<list>, keyed_mutex_per_hop_ms=<x>, readback_push_ms=<y>, bgfx_native_handle=<exposed/not>, same_adapter=<yes/no>, fits_30fps_envelope=<yes/provisional/no>`. Delete `spikes/sharing/`. Commit verdict-log update:
```bash
git add plans/2026-08-15-phase0-spikes.md
git commit -m "spike(B): cross-device texture sharing + readback verdict"
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
git add plans/2026-08-15-phase0-spikes.md
git commit -m "spike(C): face model iGPU DirectML perf + selection verdict"
```

---

## Verdict Log

*(Executor appends each spike's verdict here as tasks complete. All three verdicts GO = proceed to the second writing-plans pass for the detailed v1 build plan. Any NO-GO = stop and report to the user before planning.)*

- [Task 0 env gate verdict]
- [Spike A verdict]
- [Spike B verdict]
- [Spike C verdict]

## Gate to v1 Build Plan

After Spikes A/B/C all return **GO** (or GO-WITH-CAVEAT with documented workarounds): invoke `superpowers:writing-plans` again to write the **detailed v1 build plan** (`plans/<date>-v1-build.md`), now informed by:
- the exact driver handoff mechanism confirmed in Spike A,
- the shareable format set + measured sharing/readback costs from Spike B,
- the selected face model + measured latency from Spike C.
That second plan implements the modules in `spec/2026-08-15-v1-mvp-design.md` §3, TDD task-by-task.

# SparkFace — Project Rules

Open-source (Apache-2.0) commercial desktop beauty camera app. Real-time webcam
beautification rendered to a virtual camera, exposed as a standard video device
that OBS / Zoom / Teams / Discord / DingTalk / Tencent Meeting can consume.
Windows-first; macOS deferred to v5. Bilingual (Chinese + English) from day one.

> This is a practical "how to work in this repo" guide, not a full spec. For
> design detail see [spec/2026-08-15-v1-mvp-design.md](spec/2026-08-15-v1-mvp-design.md).
> The Phase-0 de-risking spike plan is
> [plans/2026-08-15-phase0-spikes.md](plans/2026-08-15-phase0-spikes.md) — those
> spikes must pass before the detailed v1 build plan is written.

## Tech Stack

| Area | Choice |
|------|--------|
| Core language | C++17 |
| Build | CMake + `CMakePresets.json` (MSVC x64 Windows; Clang macOS later) |
| Capture | Windows Media Foundation (incl. MJPEG decode) |
| Render / compute | bgfx, pinned to **D3D12** on Windows (aligns with DirectML) |
| Face ML | ONNX Runtime + DirectML EP, lightweight landmark model |
| Virtual camera | Kernel-mode AVStream minidriver (WDK + EV cert attestation signing) |
| UI | Qt6 (v1); engine is UI-agnostic via the C ABI |
| Driver toolchain | WDK (separate build track from the app) |

## Module Map

```
src/
├── app/                 # Qt6 frontend shell (v1). i18n zh/en. UI only.
├── core/                # Engine internals
│   ├── engine/          # Beauty + render pipeline (bgfx/D3D12). Own shaders.
│   ├── capture/         # Media Foundation capture -> D3D11 shared texture.
│   ├── face/            # ONNX Runtime + DirectML EP, face landmarks.
│   ├── sinks/           # Output sink interface + virtual-cam sink (v1).
│   └── presets/         # Level / preset / custom three-tier system.
├── driver/
│   └── virtualcam/      # Kernel-mode AVStream minidriver.
├── plugin/
│   └── ndi/             # NDI output (SparkFace v4 milestone; NDI® SDK, optional proprietary plugin, out-of-core open build).
└── c-api/               # Public C ABI facade (extern "C" + opaque handle).
```

## C ABI Contract — the only public surface

`src/c-api/` exposes `extern "C"` + an opaque handle (`sf_engine_t*`). The Qt6
app calls only the C ABI. Switching later to WinUI3 (C++/WinRT or C#) or
SwiftUI (Swift) touches no engine code — the C ABI is the single public surface
all three UI toolkits consume.

- **Lifecycle:** `sf_init(...) → sf_start(h) → … → sf_stop(h) → sf_shutdown(h)`.
  `sf_init` failure returns NULL.
- **Thread safety:** C ABI calls are thread-safe (callable from the UI thread).
  Callbacks (preview-frame-ready, etc.) fire on the **engine thread** — the UI
  must marshal back to its own thread before touching widgets.
- **Preview:** `sf_register_preview_callback` delivers a GPU shared preview
  texture handle by default (zero-copy); `sf_preview_query_format` checks
  whether a pixel-buffer fallback is available. Do not route preview back
  through the virtual camera.
- **Errors:** every C ABI function returns `sf_result_t` (int enum: `SF_OK` /
  `SF_ERR_*`).
- **ABI version:** `SF_ABI_VERSION` macro + `sf_query_abi_version()`. Additive
  changes only; breaking changes bump major.

## Dependency Acquisition (mixed — no single tool)

| Dependency | How | Why |
|------------|-----|-----|
| Qt6 | Prebuilt binary (aqtinstall / Qt installer) + `find_package(Qt6)` | Building Qt from source takes hours and is error-prone |
| ONNX Runtime + DirectML | Official prebuilt redist (NuGet unpacked / C++ redist zip) + manual link | vcpkg source-port support uncertain; DirectML enablement is non-trivial |
| bgfx | vcpkg port or FetchContent third-party CMake wrapper | bgfx ships its own genie build, no first-class CMake |
| gtest / Catch2 | FetchContent | Small; fetch-and-build is simplest |

NDI SDK is **optional and proprietary** — pulled in only when
`SPARKFACE_BUILD_NDI=ON`, and never enters the open-source build.

## Build Tracks (two independent, plus an NDI opt-in)

1. **User-mode app + core** — CMake + the dependency mix above. The standard
   track.
2. **Kernel driver** (`driver/virtualcam`) — **WDK toolchain**, produces `.sys`
   + `.inf` + `.cat`, decoupled from the app (the app does not link the driver;
   the installer installs it). A separate WDK sub-build.
3. **NDI plugin** (optional) — proprietary; built into track 1 only when opted
   in. Not in the open-source artifact.

## License

- **Core:** Apache-2.0.
- **Driver:** source-available — source is open, but without an EV certificate
  you cannot produce a newly attestation-signed, installable binary. We
  distribute the pre-signed `.sys`.
- **Qt6:** LGPL via dynamic link (obligation: provide object files / relink
  instructions).
- **NDI:** isolated in `plugin/ndi`, never linked into the core build.

CI runs reuse/SPDX license audits from day one.

## Platform

Windows-first. macOS is a **v5** milestone (bgfx Metal backend + CoreMediaIO
virtual camera). Do not add macOS codepaths now — the render-backend abstraction
is a door left open, not yet built.

## Performance Target

**iGPU (Intel Iris Xe) @ 30fps smooth** at default 1080p is the load-bearing
promise. Total frame budget is ~33.3ms across capture + decode + face ML +
beauty pass + driver readback + sync + jitter — see spec §2.2 for the
breakdown. 60fps is dGPU-recommended / iGPU-best-effort, **not** a promise.
Budget must be **measured** on the iGPU reference machine, not estimated.

## Workflow Notes

- Phase-0 spikes are **throwaway feasibility probes** — probe code is not
  merged into the product. Spike verdicts (with measured numbers) gate the v1
  build plan.
- Code quality: clang-format / clang-tidy / cmake-format + pre-commit hook.
- Branches & tags: feature work on `feat/*`; never commit directly to `main` /
  `dev`. SemVer tags (`vX.Y.Z[-rc.N]`) per `git_rules`.
- iGPU 30fps performance gate cannot run in public CI (no iGPU runner) — use a
  self-hosted iGPU runner or manual regression before merge.

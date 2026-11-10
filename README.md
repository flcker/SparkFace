# SparkFace

> **星颜（相机）** · Desktop beauty software with virtual camera output.

**English** | [中文](README_zh.md)

![Status](https://img.shields.io/badge/status-concept-orange)
![Platform](https://img.shields.io/badge/platform-Windows%2011%2B-blue)

SparkFace (Chinese: **星颜**, "sparkling face") is a desktop beauty application that
processes your webcam feed in real time and exposes the beautified result as a
**virtual camera** for video applications such as OBS, Zoom, Teams, and Discord.
v1 exposes one virtual-camera device and supports one active consuming application
at a time; concurrent consumers are deferred to a later release. It delivers smooth,
natural face beautification live, with filters and AR effects, and is designed to
later output **NDI** for network video streaming.

> **Status:** Phase-0 is blocked. The Windows 11 environment gate passed, and the user-mode virtual-camera gate is **GO-WITH-CAVEAT** for the accepted v1 single-consumer contract. Spike B/B2/B3 GPU handoff is **NO-GO**: the CPU fallback is correct and reaches synthetic 1080p30, but B3's better pacing mode still measured `57.635ms` p95 producer-to-client latency against the 33.3ms budget. Spike C and product implementation remain gated pending a product/architecture decision. See the [documentation index](docs/README.md).

## ✨ Planned Features

- **Real-time beauty** — skin smoothing, brightening, face slimming, eye enlargement, makeup
- **Virtual camera output** — appears as one webcam to OBS, Zoom, Teams, Discord, etc.; v1 supports one consuming application at a time
- **Webcam / screen capture input**
- **Face detection & tracking** — landmarks & mesh for accurate effect placement
- **Filters & AR effects** — color grading, stickers, masks
- **NDI output** (planned) — stream the beautified feed over the network via NDI®
- **Cross-platform** (planned) — Windows first, macOS later

## Tech Stack (locked — see spec)

| Area | Choice |
|------|--------|
| Platform | Windows 11 build 22000+ (v1), macOS v5 |
| Language | C++17 |
| Capture | Windows Media Foundation |
| Render/compute | bgfx (D3D12 on Windows, pinned to align DirectML); self-write shaders, referencing GPUPixel/GPUImage |
| Face ML | ONNX Runtime + DirectML EP (lightweight landmark model) |
| Virtual camera | Windows 11 `MFCreateVirtualCamera` + custom Media Foundation source (user-mode) |
| Output | NV12 primary, BGRA8 fallback; 1080p default |
| UI | Qt6 (v1); engine UI-agnostic via C ABI (WinUI3/SwiftUI later) |
| NDI | NDI® SDK (optional proprietary plugin, out of core open build) — SparkFace v4 milestone |
| Build | CMake + CMakePresets; mixed dependency acquisition |
| License | Apache-2.0 core/virtual camera; Qt6 LGPL dynamic link; NDI isolated |

## Project Structure

```
SparkFace/
├── docs/                   # Specifications, development guides, and plans
│   ├── specs/              # Product specifications and durable decisions
│   ├── development/        # Reproducible developer workflows
│   └── plans/              # Executable plans and measured verdicts
├── src/
│   ├── app/                # Application & UI (Qt6)
│   ├── core/               # Engine internals
│   │   ├── engine/         # Beauty & render pipeline (bgfx/D3D12)
│   │   ├── capture/        # Media Foundation capture
│   │   ├── face/           # ONNX Runtime + DirectML face landmarks
│   │   ├── sinks/          # Output sink interface + virtual-cam sink
│   │   └── presets/        # Graded-level preset system
│   ├── virtualcam/         # User-mode MF custom media source + registrar
│   ├── plugin/ndi/         # NDI output plugin (SparkFace v4, proprietary, out-of-core)
│   └── c-api/              # C ABI facade (extern "C" + opaque handle)
├── CMakeLists.txt
├── AGENT.md
├── CLAUDE.md
├── README.md
└── README_zh.md
```

## Documentation

- [Documentation index](docs/README.md) — canonical entry point for long-form project documentation.
- [v1 MVP design](docs/specs/2026-08-15-v1-mvp-design.md) — product scope, architecture, module map, C ABI, build system, and risks.
- [Phase-0 Windows environment](docs/development/phase0-windows-environment.md) — prerequisites, dependency locks, security constraints, and verified gate output.
- [Phase-0 spike plan](docs/plans/2026-08-15-phase0-spikes.md) — user-mode virtual camera, GPU interop/handoff, and face-model performance gates.

## Roadmap

- [x] Project kickoff & naming
- [ ] Phase-0 feasibility validation
- [ ] Project scaffold
- [ ] Webcam capture & preview
- [ ] Face landmark detection
- [ ] First beauty filter
- [ ] Virtual camera output
- [ ] AR stickers / filters
- [ ] NDI output
- [ ] macOS support

## Getting Started

Phase-0 environment gate only (not the product application):

```powershell
.\scripts\build-phase0-env.ps1 -Run
```

The script bootstraps the repository-pinned vcpkg tool/registry commit, restores explicitly locked dependencies, configures, builds, and runs the gate. It requires Windows 11 build 22000+, Visual Studio C++ tools, a matching Windows SDK, CMake, Git, and the Visual Studio Ninja component. Smart App Control blocks unsigned local development binaries and does not support per-application exceptions.

See the [Phase-0 Windows environment guide](docs/development/phase0-windows-environment.md) for the complete PASS contract and troubleshooting steps.

## License

Apache-2.0.

## Contributing

Not yet open — the project is in the concept phase. Once development starts,
contribution guidelines will be published here.

---

*SparkFace · 星颜（相机）*

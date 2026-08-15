# SparkFace

> **星颜（相机）** · Desktop beauty software with virtual camera output.

**English** | [中文](README_zh.md)

![Status](https://img.shields.io/badge/status-concept-orange)
![Platform](https://img.shields.io/badge/platform-Windows%20(proposed)-blue)

SparkFace (Chinese: **星颜**, "sparkling face") is a desktop beauty application that
processes your webcam feed in real time and exposes the beautified result as a
**virtual camera** — usable in any video app (OBS, Zoom, Teams, Discord, …). It
delivers smooth, natural face beautification live, with filters and AR effects,
and is designed to later output **NDI** for network video streaming.

> **Status:** Design spec complete — see [spec/2026-08-15-v1-mvp-design.md](spec/2026-08-15-v1-mvp-design.md).
> A Phase-0 de-risking spike plan is created — see [plans/2026-08-15-phase0-spikes.md](plans/2026-08-15-phase0-spikes.md).
> Implementation has not started; there is no runnable code yet. The tech stack
> and feature scope are locked in the design spec — see `spec/2026-08-15-v1-mvp-design.md`.

## ✨ Planned Features

- **Real-time beauty** — skin smoothing, brightening, face slimming, eye enlargement, makeup
- **Virtual camera output** — appears as a webcam to OBS, Zoom, Teams, Discord, etc.
- **Webcam / screen capture input**
- **Face detection & tracking** — landmarks & mesh for accurate effect placement
- **Filters & AR effects** — color grading, stickers, masks
- **NDI output** (planned) — stream the beautified feed over the network via NDI®
- **Cross-platform** (planned) — Windows first, macOS later

## Tech Stack (locked — see spec)

| Area | Choice |
|------|--------|
| Platform | Windows (first), macOS later |
| Language | C++17 |
| Capture | Windows Media Foundation |
| Render/compute | bgfx (D3D12 on Windows, pinned to align DirectML); self-write shaders, referencing GPUPixel/GPUImage |
| Face ML | ONNX Runtime + DirectML EP (lightweight landmark model) |
| Virtual camera | Kernel-mode AVStream minidriver (WDK + EV cert attestation signing) |
| Output | NV12 primary, ARGB fallback; 1080p default |
| UI | Qt6 (v1); engine UI-agnostic via C ABI (WinUI3/SwiftUI later) |
| NDI | NDI® SDK (optional proprietary plugin, out of core open build) — SparkFace v4 milestone |
| Build | CMake + CMakePresets; mixed dependency acquisition |
| License | Apache-2.0; driver source-available; Qt6 LGPL dynamic link; NDI isolated |

## Project Structure

```
SparkFace/
├── docs/                   # User & general documentation
├── spec/                   # Technical design & specs
├── plans/                  # Implementation plans
├── src/
│   ├── app/                # Application & UI (Qt6)
│   ├── core/               # Engine internals
│   │   ├── engine/         # Beauty & render pipeline (bgfx/D3D12)
│   │   ├── capture/        # Media Foundation capture
│   │   ├── face/           # ONNX Runtime + DirectML face landmarks
│   │   ├── sinks/          # Output sink interface + virtual-cam sink
│   │   └── presets/        # Graded-level preset system
│   ├── driver/virtualcam/  # Kernel AVStream minidriver
│   ├── plugin/ndi/         # NDI output plugin (SparkFace v4, proprietary, out-of-core)
│   └── c-api/              # C ABI facade (extern "C" + opaque handle)
├── CMakeLists.txt
├── AGENT.md
├── CLAUDE.md
├── README.md
└── README_zh.md
```

## Design & Specs

- [spec/2026-08-15-v1-mvp-design.md](spec/2026-08-15-v1-mvp-design.md) — v1 (MVP) design spec (architecture, module map, C ABI, build system, risks).
- [plans/2026-08-15-phase0-spikes.md](plans/2026-08-15-phase0-spikes.md) — Phase-0 de-risking spike plan (kernel driver, GPU texture sharing, face-model perf).

## Roadmap

- [x] Project kickoff & naming
- [ ] Tech stack confirmation
- [ ] Project scaffold
- [ ] Webcam capture & preview
- [ ] Face landmark detection
- [ ] First beauty filter
- [ ] Virtual camera output
- [ ] AR stickers / filters
- [ ] NDI output
- [ ] macOS support

## Getting Started

> TBD — scaffold and build instructions will be added once the project structure
> is in place.

## License

Apache-2.0.

## Contributing

Not yet open — the project is in the concept phase. Once development starts,
contribution guidelines will be published here.

---

*SparkFace · 星颜（相机）*

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

> 🚧 **Status:** Early concept / pre-development. There is no runnable code yet —
> the repository currently holds only documentation. The tech stack and feature
> scope below are **proposed** and open for discussion.

## ✨ Planned Features

- **Real-time beauty** — skin smoothing, brightening, face slimming, eye enlargement, makeup
- **Virtual camera output** — appears as a webcam to OBS, Zoom, Teams, Discord, etc.
- **Webcam / screen capture input**
- **Face detection & tracking** — landmarks & mesh for accurate effect placement
- **Filters & AR effects** — color grading, stickers, masks
- **NDI output** (planned) — stream the beautified feed over the network via NDI®
- **Cross-platform** (planned) — Windows first, macOS later

## 🛠 Tech Stack (proposed)

| Area | Choice (to confirm) |
|------|---------------------|
| Platform | Windows (first), macOS later |
| Core engine | C++ |
| Capture | Windows Media Foundation |
| Virtual camera | Virtual camera driver (DirectShow / Media Foundation) |
| Rendering | D3D11 / OpenGL ES shaders |
| Face / ML | MediaPipe / ONNX Runtime (TBD) |
| NDI | NDI® SDK (planned) |
| UI | Qt (proposed) |

## 📂 Planned Structure

```
SparkFace/
├── docs/                   # User & general documentation
├── spec/                   # Technical design & specs
├── src/                    # Source code
│   ├── app/                # Application & UI
│   ├── core-engine/        # Beauty & rendering pipeline (C++)
│   ├── core-capture/       # Webcam / screen capture
│   ├── driver-virtualcam/  # Virtual camera driver
│   └── plugin-ndi/         # NDI output plugin (planned)
├── CMakeLists.txt
├── README.md
└── README_zh.md
```

## 🗺 Roadmap

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

## 🚀 Getting Started

> TBD — scaffold and build instructions will be added once the project structure
> is in place.

## 📜 License

TBD (consider MIT or Apache-2.0).

## 🤝 Contributing

Not yet open — the project is in the concept phase. Once development starts,
contribution guidelines will be published here.

---

*SparkFace · 星颜（相机）*

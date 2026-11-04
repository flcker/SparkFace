# 星颜（相机）

> **SparkFace (Cam)** · 桌面端美颜软件，支持虚拟摄像头输出。

[English](README.md) | **中文**

![状态](https://img.shields.io/badge/状态-概念阶段-orange)
![平台](https://img.shields.io/badge/平台-Windows%2011%2B-blue)

星颜（英文名 **SparkFace**，"闪耀之颜"）是一款桌面端美颜软件：实时处理摄像头画面，并将美化后的结果作为
**虚拟摄像头** 输出，可供任意视频软件（OBS、Zoom、Teams、Discord 等）调用。它提供自然流畅的实时美颜，
并支持滤镜与 AR 特效，后续将支持 **NDI** 网络推流。

> **当前状态：** Windows 11 用户态虚拟摄像头架构的 Phase-0 正在执行。Task 0 已在 Intel Iris Xe 参考工作站通过：Media Foundation 虚拟摄像头 API、bgfx Direct3D 12 和 ONNX Runtime DirectML 均已成功运行。正式产品实现尚未开始。详见 [v1 设计](spec/2026-08-15-v1-mvp-design.md) 和 [实测 spike 计划](plans/2026-08-15-phase0-spikes.md)。

## ✨ 规划功能

- **实时美颜** —— 磨皮、提亮、瘦脸、大眼、美妆
- **虚拟摄像头输出** —— 对 OBS、Zoom、Teams、Discord 等表现为一个摄像头设备
- **摄像头 / 屏幕采集输入**
- **人脸检测与跟踪** —— 关键点与网格，精确定位特效
- **滤镜与 AR 特效** —— 调色、贴纸、面具
- **NDI 输出**（规划）—— 通过 NDI® 将美化画面经网络推流
- **跨平台**（规划）—— Windows 优先，后续 macOS

## 技术栈（锁定 — 见 spec）

| 方向 | 选型 |
|------|------|
| 平台 | Windows 11 build 22000+（v1），macOS v5 |
| 语言 | C++17 |
| 采集 | Windows Media Foundation |
| 渲染/计算 | bgfx（Windows 上 pin D3D12 对齐 DirectML）；自写 shader，算法参考 GPUPixel/GPUImage |
| 人脸 ML | ONNX Runtime + DirectML EP（轻量关键点模型） |
| 虚拟摄像头 | Windows 11 `MFCreateVirtualCamera` + 自定义 Media Foundation media source（用户态） |
| 输出 | NV12 为主、BGRA8 兜底；默认 1080p |
| UI | Qt6（v1）；引擎经 C ABI 与 UI 解耦（WinUI3/SwiftUI 后续） |
| NDI | NDI® SDK（可选专有插件，核心开源构建外）— SparkFace v4 里程碑 |
| 构建 | CMake + CMakePresets；依赖混合获取 |
| License | 核心/虚拟摄像头 Apache-2.0；Qt6 LGPL 动态链接；NDI 隔离 |

## 项目结构

```
SparkFace/
├── docs/                   # 用户与通用文档
├── spec/                   # 技术方案与设计规格
├── plans/                  # 实现计划
├── src/
│   ├── app/                # 应用与 UI（Qt6）
│   ├── core/               # 引擎内部
│   │   ├── engine/         # 美颜与渲染管线（bgfx/D3D12）
│   │   ├── capture/        # Media Foundation 采集
│   │   ├── face/           # ONNX Runtime + DirectML 人脸关键点
│   │   ├── sinks/          # 输出端接口 + 虚拟摄像头 sink
│   │   └── presets/        # 分级预设系统
│   ├── virtualcam/         # 用户态 MF custom media source + registrar
│   ├── plugin/ndi/         # NDI 输出插件（SparkFace v4，专有，构建外）
│   └── c-api/              # C ABI facade（extern "C" + opaque handle）
├── CMakeLists.txt
├── AGENT.md
├── CLAUDE.md
├── README.md
└── README_zh.md
```

## 设计与规格

- [spec/2026-08-15-v1-mvp-design.md](spec/2026-08-15-v1-mvp-design.md) —— v1（MVP）设计规格（架构、模块图、C ABI、构建系统、风险）。
- [plans/2026-08-15-phase0-spikes.md](plans/2026-08-15-phase0-spikes.md) —— Phase-0 风险验证 spike 计划（用户态虚拟摄像头、GPU 互操作/交接、人脸模型性能）。

## 路线图

- [x] 项目启动与命名
- [ ] Phase-0 可行性验证
- [ ] 项目脚手架
- [ ] 摄像头采集与预览
- [ ] 人脸关键点检测
- [ ] 首个美颜滤镜
- [ ] 虚拟摄像头输出
- [ ] AR 贴纸 / 滤镜
- [ ] NDI 输出
- [ ] macOS 支持

## 快速开始

仅用于 Phase-0 环境门禁（不是产品应用）：

```powershell
.\scripts\build-phase0-env.ps1 -Run
```

脚本会引导仓库锁定的 vcpkg 工具/registry commit、恢复显式固定的依赖、配置、构建并运行门禁。要求 Windows 11 build 22000+、Visual Studio C++ 工具、匹配的 Windows SDK、CMake、Git 和 Visual Studio Ninja 组件。Smart App Control 会阻止本地未签名开发程序，且不支持单应用例外。

## 许可证

Apache-2.0。

## 贡献

暂未开放——项目仍处于概念阶段。开发启动后，将在此发布贡献指南。

---

*SparkFace · 星颜（相机）*

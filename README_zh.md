# 星颜（相机）

> **SparkFace (Cam)** · 桌面端美颜软件，支持虚拟摄像头输出。

[English](README.md) | **中文**

![状态](https://img.shields.io/badge/状态-概念阶段-orange)
![平台](https://img.shields.io/badge/平台-Windows%20(提议)-blue)

星颜（英文名 **SparkFace**，"闪耀之颜"）是一款桌面端美颜软件：实时处理摄像头画面，并将美化后的结果作为
**虚拟摄像头** 输出，可供任意视频软件（OBS、Zoom、Teams、Discord 等）调用。它提供自然流畅的实时美颜，
并支持滤镜与 AR 特效，后续将支持 **NDI** 网络推流。

> 🚧 **当前状态：** 早期概念 / 预开发阶段。尚无可运行代码——仓库目前仅包含文档。
> 下方的技术栈与功能范围均为**提议方案**，欢迎讨论调整。

## ✨ 规划功能

- **实时美颜** —— 磨皮、提亮、瘦脸、大眼、美妆
- **虚拟摄像头输出** —— 对 OBS、Zoom、Teams、Discord 等表现为一个摄像头设备
- **摄像头 / 屏幕采集输入**
- **人脸检测与跟踪** —— 关键点与网格，精确定位特效
- **滤镜与 AR 特效** —— 调色、贴纸、面具
- **NDI 输出**（规划）—— 通过 NDI® 将美化画面经网络推流
- **跨平台**（规划）—— Windows 优先，后续 macOS

## 🛠 技术栈（提议）

| 方向 | 选型（待确认） |
|------|----------------|
| 平台 | Windows（优先），后续 macOS |
| 核心引擎 | C++ |
| 采集 | Windows Media Foundation |
| 虚拟摄像头 | 虚拟摄像头驱动（DirectShow / Media Foundation） |
| 渲染 | D3D11 / OpenGL ES 着色器 |
| 人脸 / ML | MediaPipe / ONNX Runtime（待定） |
| NDI | NDI® SDK（规划） |
| UI | Qt（提议） |

## 📂 规划结构

```
SparkFace/
├── docs/                   # 用户与通用文档
├── spec/                   # 技术方案与设计规格
├── src/                    # 工程源码
│   ├── app/                # 应用与 UI
│   ├── core-engine/        # 美颜与渲染管线（C++）
│   ├── core-capture/       # 摄像头 / 屏幕采集
│   ├── driver-virtualcam/  # 虚拟摄像头驱动
│   └── plugin-ndi/         # NDI 输出插件（规划）
├── CMakeLists.txt
├── README.md
└── README_zh.md
```

## 🗺 路线图

- [x] 项目启动与命名
- [ ] 技术栈确认
- [ ] 项目脚手架
- [ ] 摄像头采集与预览
- [ ] 人脸关键点检测
- [ ] 首个美颜滤镜
- [ ] 虚拟摄像头输出
- [ ] AR 贴纸 / 滤镜
- [ ] NDI 输出
- [ ] macOS 支持

## 🚀 快速开始

> 待补充——待项目结构建立后，将补充脚手架与构建说明。

## 📜 许可证

待定（考虑 MIT 或 Apache-2.0）。

## 🤝 贡献

暂未开放——项目仍处于概念阶段。开发启动后，将在此发布贡献指南。

---

*SparkFace · 星颜（相机）*

# SparkFace v1（MVP）设计规格

> 状态：设计稿（经两轮独立 subagent 审核修订），待 review · 日期：2026-08-15 · License：Apache-2.

## 0. 概述

SparkFace（星颜）是面向他人的商业桌面美颜软件：实时处理摄像头画面，把美化结果作为**虚拟摄像头**输出（OBS / Zoom / Teams / Discord / 钉钉 / 腾讯会议等可调用），核心开源（Apache-2.0）。

- **主打差异化**：免费/开源 + 美颜分级可快速设置 + 兼容自然美颜 → 美妆 → 娱乐 AR 全谱。
- **首发场景优先级**：直播/推流 + 内容录制 优先；实时视频通话/会议 后置（延迟要求更严，后续阶段再压）。
- **首发市场**：中英双语并重。

> MVP 阶段虚拟摄像头的任务只是"做一个 OBS 能识别的视频设备"，这在两市场是同一段代码，双语不会让驱动工作量翻倍，主要加 i18n + 双分发渠道。因市场而异的重活（Zoom/Teams vs 钉钉/腾讯会议 的 app 兼容）落在后期通话阶段。

## 1. 范围

### 1.1 v1（MVP）范围

- 采集：Media Foundation 摄像头采集
- 人脸：轻量人脸关键点模型（ONNX Runtime + DirectML EP）
- 自然美颜效果：磨皮（双边滤波 + 肤肤 mask）、提亮、瘦脸（网格形变）、大眼（网格形变）
- 分级预设系统：分级(level) / 预置(style) / 自定义 三层控制
- 虚拟摄像头输出：内核态 AVStream minidriver
- UI：Qt6 前端壳，中英 i18n
- 性能：iGPU（Iris Xe）@30fps 流畅（默认）；fps 可设

### 1.2 后续里程碑

| 版本 | 范围 |
|------|------|
| v1 | 采集 + 人脸关键点 + 自然美颜(磨皮/提亮/瘦脸/大眼) + 分级预设系统 + 虚拟摄像头 + Qt6 UI(i18n) |
| v2 | 美妆（口红/腮红/眼影）作为新效果 + "美妆"预置 style |
| v3 | 娱乐 AR（贴纸/面具/特效）；tgfx 作 AR/合成层候选 |
| v4 | NDI 输出（可选专有插件，核心构建外）+ 可能 Spout/文件录制 sinks |
| v5 | macOS 移植（bgfx Metal 后端 + CoreMediaIO 虚拟摄像头）+ 通话场景 app 兼容压延迟 |

## 2. 架构与数据流

实时、GPU 驻留的帧循环：

```
摄像头 → [Media Foundation 采集线程]
   → D3D11 共享纹理(输入帧, GPU)
   → [人脸 ML: ONNX Runtime + DirectML EP] 在 D3D12 device
        → 人脸关键点 + 网格
   → [美颜渲染: bgfx(D3D12)]
        • 双边滤波磨皮(compute, 肤肤 mask)
        • 提亮(色彩 shader)
        • 瘦脸/大眼(网格顶点位移, 关键点驱动)
   → 处理后输出纹理(bgfx, GPU)
   → [输出端: 虚拟摄像头] 末帧 GPU→CPU 读回(~1-2ms) → 用户态经 DeviceIoControl 推内核共享环形缓冲
        → 内核态 AVStream minidriver → OBS 等识别为摄像头
   ↑ 控制回路: Qt6 UI(C ABI) 下发 level / fps / 效果参数
```

### 2.1 GPU device 与纹理共享拓扑（iGPU 30fps 生死线）

- **共享链里三个用户态 D3D device 实例**：MF 采集 device（D3D11，源纹理持有者）+ DirectML 的 D3D12 device（人脸 ML）+ bgfx 渲染 device（D3D12，**pin 以对齐 DirectML**）。我们拥有后两个计算 device，MF 的 D3D11 是源纹理的共享参与方。内核驱动**不持有 GPU device**（AVStream minidriver 是内核态）。
- **GPU 管线内**（采集→人脸→美颜）纹理经 **DXGI 共享句柄（NT handle + keyed mutex）** 跨 device 流转，**全程不下 GPU 显存、零逐帧 CPU 拷贝**。
- **驱动交接处必然一次 GPU→CPU 读回**：内核 minidriver 无法直接持有 D3D11 纹理句柄，标准做法（OBS Virtual Cam 同理）是用户态把末帧读回系统内存 → 经 `DeviceIoControl` 推进驱动的共享环形缓冲。读回成本 ~1-2ms@1080p，已计入预算（§2.2）。
- 链路：采集纹理(MF→D3D11) → 共享给 DirectML device 跑人脸 → 共享回 bgfx device 做美颜 → 末帧 GPU→CPU 读回 → IOCTL 进内核驱动。
- **#1 集成风险**，早期 spike 验证（见 §6）。**决定**：bgfx 在 Windows pin 到 D3D12 后端以与 DirectML 对齐（都 D3D12，共享最顺）。spike 验证的是"GPU 管线 copy-free + 驱动读回/IOCTL 推送"能否 @30fps 跑通，而非 D3D12 选择本身。

### 2.2 帧预算（iGPU Iris Xe @30fps, 33ms, 默认 1080p）

| 阶段 | 预算（粗估，spike 实测确认） |
|------|------|
| 采集 + MJPEG 解码（若摄像头出 MJPEG） | ~0-6ms（原生 NV12/YUY2 省回解码；解码输出格式须可 DXGI 共享） |
| 跨 device 同步（keyed mutex acquire/release ×2-3 hop） | ~1-3ms |
| 人脸 ML（轻量模型） | ~5-8ms |
| 美颜 pass | ~6-10ms |
| 驱动交接（GPU→CPU 读回 + IOCTL 推送） | ~1-2ms |
| present/vsync + OS 调度抖动 | ~2-4ms |

合计粗估落在 18-33ms 区间——**高端已贴 30fps 帧时间(33.3ms)、零安全余量，各阶段须落在中低段**，spike 是这条预算能否成立的载重验证。爆预算按 §6.1 #3 兜底。

**输出分辨率/格式**：默认 1080p；输出格式集 **NV12 为主**（虚拟摄像头消费端兼容最广）、ARGB 兜底。**DXGI 跨 device 共享有格式约束**（非所有 DXGI 格式可跨 D3D11/D3D12 共享），spike 确认可共享格式集。

## 3. 模块、C ABI、项目结构

```
src/
├── app/                    # Qt6 前端壳(v1)。i18n 中英。仅 UI
├── core/                   # 引擎内部(合并同类)
│   ├── engine/            # 美颜+渲染管线(bgfx/D3D12)。自写 shader，算法参考 GPUPixel/GPUImage
│   │   ├── render-backend/   # bgfx 薄封装(day1；自抽象以后再说)
│   │   ├── filters/          # bilateral 磨皮 / brighten 提亮 / face-reshape 瘦脸大眼
│   │   └── pipeline/         # 帧编排
│   ├── capture/           # Media Foundation 采集 → D3D11 共享纹理
│   ├── face/              # ONNX Runtime + DirectML EP，轻量人脸关键点
│   ├── sinks/             # 输出端接口 + virtual-cam sink 实现(v1)
│   └── presets/           # 分级预设系统
├── driver/                # 驱动(合并同类)
│   └── virtualcam/        # 内核态 AVStream minidriver
├── plugin/                # 插件(合并同类)
│   └── ndi/               # NDI(v4，专有，core 构建外)
└── c-api/                 # 对外 C ABI facade(extern "C" + opaque handle)
```

### 3.1 C ABI 边界（UI 可替换的关键）

core 暴露 `extern "C"` + opaque handle（如 `sf_engine_t*`）。Qt6 app 只调 C ABI + 渲染预览。换 WinUI3(C++/WinRT 或 C#) / SwiftUI(Swift) 不动引擎——C ABI 是 Swift 和 C# 都好消费的唯一公共面。

- **生命周期**：`sf_init(...)` → `sf_start(h)` → … → `sf_stop(h)` → `sf_shutdown(h)`；opaque handle 由 `sf_init` 返回、`sf_shutdown` 释放。
- **线程契约**（三种 UI 工具包都必须遵守）：引擎在自有线程跑采集/处理；C ABI 所有调用**线程安全**，可从 UI 线程直接调；**回调（预览帧就绪等）在引擎线程触发**，UI 侧必须 marshal 回 UI 线程后再碰 widget。
- **预览路径**：引擎提供专用预览回调 `sf_register_preview_callback(h, fn, userdata)`，每处理帧就绪回调一次。回调载荷**默认是 GPU 共享预览纹理句柄**（UI 直接渲染、零拷贝）；`sf_preview_query_format` 可查是否支持像素-buffer 模式（给不能导入 GPU 句柄的 UI 兜底）。**不**走"再订阅虚拟摄像头"那条往返内核的弯路。
- **摄像头选择**：`sf_enum_cameras(h, ...) → count` + `sf_select_camera(h, index)`。
- **错误模型**：所有 C ABI 函数返回 `sf_result_t`（int 枚举：`SF_OK` / `SF_ERR_*`）；`sf_init` 失败返回 NULL；非法参数返回 `SF_ERR_INVALID_ARG`，设备/模型不可用返回对应码。
- **ABI 版本**：头文件带 `SF_ABI_VERSION` 宏 + `sf_query_abi_version()`；新增效果/参数走加法不破坏消费者，破坏性变更 bump major。

### 3.2 依赖方向

`app → c-api → core 各模块`；driver 与 plugin 经共享 surface / 插件接口与 core 解耦。引擎内部模块不互依 UI。

## 4. 构建系统与项目管理

### 4.1 构建

- CMake (C++17) + `CMakePresets.json`（MSVC x64 Win；后补 Clang macOS）。各模块 = CMake target。
- **依赖按依赖混合获取（不单一工具）**，CMake `find_package`/FetchContent 编排：

  | 依赖 | 获取方式 | 理由 |
  |------|----------|------|
  | Qt6 | 预编译二进制(aqtinstall / Qt installer) + `find_package(Qt6)` | vcpkg 源码编 Qt 数小时且易错；FetchContent 源码编更不现实 |
  | ONNX Runtime + DirectML | 官方预编译 redist（NuGet `Microsoft.ML.OnnxRuntime.DirectML` 解包 / C++ redist zip）+ 手动 link | DirectML 的 vcpkg 源码端口支持不确定；源码自建重且 DirectML 启用非平凡 |
  | bgfx | vcpkg port（干净）或 FetchContent 第三方 CMake wrapper | bgfx 自带 genie 构建、无一流 CMake |
  | gtest / Catch2 | FetchContent | 小，源码即取即编最省事 |

- 开源核心 vs 完整构建：CMake option `SPARKFACE_BUILD_NDI=OFF`（默认，开源产物干净）/ `ON`（装 NDI SDK 才编 `plugin/ndi`）。

### 4.2 两条独立构建轨道（驱动不能并入 app 构建）

- ① 用户态 app+core：CMake + 依赖，常规
- ② 内核驱动 `driver/virtualcam`：**WDK 工具链**编 .sys + .inf + .cat，与 app 解耦（app 不 link 驱动，安装器装）。WDK 子构建
- ③ 可选 NDI 插件：专有，仅轨道① opt-in 时编，不进开源产物

### 4.3 驱动签名与分发

驱动 source 开源、签名证书机密。release 用 EV 证书经微软硬件门户 attestation 签名出 .cat；**release 包携带已签名驱动二进制**（用户无需自签），与开源 source 并存——标准做法（OBS Virtual Cam 同理）。**诚实口径**：驱动是 source-available——用户可读/改源码，但无 EV 证书无法重新 attestation 签名出可装机的新二进制；实际使用靠我们分发的已签名 .sys。核心 app 部分（不含驱动）可自由构建。

### 4.4 打包发布

WiX(MSI) 安装器捆绑 app + 已签名驱动（PnPUtil 装 .sys+.inf）；macOS 后续 pkg/notarize。版本号遵循 git_rules（SemVer `vX.Y.Z[-rc.N]`，gray rc → prod）。

### 4.5 CI/CD（GitHub Actions）

- PR：build(MSVC) + 单测 + clang-format/tidy lint + license 合规(reuse)
- 驱动签名/打包 = release workflow，走 secrets（EV 证书不入公开 CI）
- iGPU 30fps 性能门禁 CI 跑不了（无 iGPU runner）→ self-hosted iGPU runner 或手动回归

### 4.6 代码质量

clang-format / clang-tidy / cmake-format + pre-commit hook。

### 4.7 SDK 导出

`c-api` target 产 lib + 公共头（公共面），未来 WinUI3/SwiftUI 前端只消费这套头+lib。

### 4.8 项目约定文档

按全局约定，建 **AGENT.md** 存项目规则（技术栈/构建/开发规范），`CLAUDE.md` 只放 `@AGENT.md`。

## 5. 分级预设系统（签名级 UX）

三层控制（快→深），各层不互斥、可叠加：

1. **分级 level 0..N — 快速设置**：一键调，直播中用，主控。在当前预置的 level 表上滑动。
2. **预置 named style — 预置设置**：选一套预调好的风格（"自然"/"清新"/"浓妆"...），每套=一张完整 level 表。切风格=换表。v1 内置"自然"一套，schema 从 day1 支持多套。
3. **自定义 — 各项功能独立覆盖设置**：每个支持的效果（磨皮/提亮/瘦脸/大眼/美妆...）独立可调，**覆盖**当前 level 基准向量里的该单个效果参数（其余沿用 level 值）；调完可 `sf_save_preset` 存为自己的新预置。

**关系（override 语义）**：预置(style) 含一张 level 表；分级 在当前预置表上选行=设基准向量；自定义 在该基准上覆盖单个效果参数，其余沿用 level 值。最终向量 = level 行 + 各被覆盖效果。三级覆盖主播直播(分级)/挑风格(预置)/精调(自定义)三种场景。

### 5.1 效果参数（引擎实际消费的旋钮）

- 磨皮：双边滤波强度（spatial/range sigma + 混合比）+ 肤肤 mask（只磨皮肤，不动眼/发/背景）
- 提亮：亮度/白平衡增量（可肤色定向）
- 瘦脸：下颌/颊网格形变强度
- 大眼：眼区网格形变强度
- v2 扩展：口红/腮红/眼影 intensity+color

### 5.2 level → 参数向量映射

预设表 `level → {smooth, brighten, slim, eye, ...}`。level 0=全零(直通)、level 1=微自然、level N=全开。预设是**数据**(JSON/TOML)非代码——可无重编译调参/分享/导入。v1 发"自然"一套（level 0..5）。

### 5.3 引擎行为

- 分级切档参数**缓动**过渡（~200-300ms，不跳变——否则直播切档会跳）。
- 自定义改单效果同样**缓动**应用，与分级切档一致（统一一套过渡行为，避免两种手感）。
- **拖动中不平滑**：用户拖滑块/level 时，拖动过程中参数立即跟随（无缓动），松手后才对最终值缓动收敛——否则滑块滞后光标、手感粘滞。
- **override 生命周期**：切 level 时已设的 per-effect override 在当前预置内**保留**（基准行变了，override 叠在新基准上）；切预置(style)时 override **清零**（新预置从干净基准开始）。
- **无人脸降级**：检测不到人脸、或关键点不稳定（遮挡/侧脸）时，部位级效果（瘦脸/大眼）静默禁用，只留全帧磨皮/提亮——三层都适用（自定义设了瘦脸但无人脸，该效果静默禁用不报错）。
- **引擎未运行时虚拟摄像头输出**：驱动持续输出**黑帧**（维持设备在线、不卡消费端），不输出末帧/测试图（避免误导）。

### 5.4 fps 设置（管线参数，非美颜参数）

- 模式：match-source（跟摄像头原生）/ 固定档（24 / 30 / 60）/ custom
- C ABI：`sf_set_fps_mode` + `sf_set_fps`；虚拟摄像头输出帧率跟随处理 fps
- 性能承诺：**iGPU 流畅 = 30fps（默认）**——"集显也要流畅"的具体兑现线；**60fps = dGPU 推荐 / iGPU 尽力**（全管线含人脸 ML 在 16.6ms 预算内对 iGPU 不现实，不承诺集显 60 流畅）
- 爆预算兜底：iGPU 持不住 30 全开效果时，用户设 24fps 或自动降级——fps 设置本身是优雅降级一环
- **多 GPU 注意**：切 dGPU 会让 iGPU 采集纹理跨 adapter 拷贝（见 §6.1 #7），60fps-on-dGPU 路线须在 spike 里权衡。

### 5.5 预设/fps 相关 C ABI

`sf_set_level` / `sf_get_level` / `sf_select_preset(name)` / `sf_set_effect_param(id,val)` / `sf_get_effect_param` / `sf_save_preset(name,path)` / `sf_load_preset_file(path)` / `sf_set_fps_mode` / `sf_set_fps` / `sf_begin_drag(h)` / `sf_end_drag(h)`（拖动区间内参数立即跟随、松手后缓动收敛）；effect id 枚举 `SF_EFFECT_SMOOTH/BRIGHTEN/SLIM/EYE_ENLARGE/...`。（生命周期/线程/预览/摄像头/错误模型/ABI 版本见 §3.1。）

## 6. 风险与去风险

### 6.1 技术风险（按严重度）

| # | 风险 | 去风险 |
|---|------|--------|
| 1 | **内核态 AVStream 驱动 + 签名**：Windows 无用户态虚拟摄像头 API，必须内核 minidriver；EV 证书 + 微软门户 attestation；跨机兼容 finicky | **实现第一步前置 spike**：throwaway 最小 minidriver，验证 OBS 能识别 + 签名流程端到端跑通 + 用户态 DeviceIoControl 推帧 + 引擎未运行时输出黑帧，再铺开管线 |
| 2 | **跨 device 纹理共享链 + 驱动读回**：GPU 管线内须 copy-free（MF D3D11→DirectML D3D12→bgfx D3D12 经 DXGI 共享句柄），驱动交接处一次 GPU→CPU 读回 + IOCTL 推送 = iGPU 30fps 生死线 | **早期 spike**：iGPU 参考机测 GPU 管线 copy-free + 末帧读回/IOCTL 推送 @30fps；确认 bgfx-D3D12 与 DirectML-D3D12 无拷贝共享、bgfx 取原生句柄 |
| 3 | **iGPU 30fps 预算**（Iris Xe @33ms 跑采集+解码+人脸+美颜+读回+同步+抖动）= 最难承诺，高端零安全余量 | iGPU 参考机从 day1 当性能门禁；预算**实测非估**；爆预算兜底：换更轻人脸模型/降人脸检测频率(隔帧)/降 24fps |
| 4 | **bgfx compute + 原生句柄交接**未验（双边 compute 多遍 + 取 D3D12 原生句柄交驱动） | 早期 spike：bgfx D3D12 下双边 compute on iGPU + 取原生句柄 |
| 5 | **人脸模型选型**（轻到 iGPU DirectML ~5-8ms、关键点够 face-reshape"自然"） | benchmark 候选（MediaPipe 468-pt ONNX / 68/106/190-pt 轻）on iGPU DirectML；取最轻且达质；兜底更轻 |
| 6 | **"自然不塑料"质量天花板**（自写双边+肤 mask+face-reshape 不塑料） | 真人脸素材早做视觉验证；调预置对照参考(GPUPixel/GPUImage 输出)做 sanity |
| 7 | **多 GPU 拓扑**：混合本(iGPU+dGPU)三个用户态 device 须同一 adapter 否则 DXGI 共享断；切 dGPU(60fps)逼 iGPU 采集纹理跨 adapter 拷贝，违背 copy-free | 优先单 adapter(iGPU)跑全链；dGPU 路线接受跨 adapter 拷贝或把采集也搬 dGPU(若摄像头可路由)，spike 验 |
| 8 | **MJPEG 解码**：很多摄像头只出 MJPEG，MF 解码加延迟且输出格式须可 DXGI 共享 | 预算计入解码(§2.2)；spike 验 MJPEG 解码输出格式可共享，否则转 CPU 上传或找 HW 解码路径 |
| 9 | **运行时边角**：iGPU 持续负载热降频 / Win11 摄像头隐私指示灯交互 / 多消费者(OBS+Zoom 同时读虚拟摄像头) | 热降频：perf 门禁测持续负载 5-10min(非瞬态)；隐私指示灯：测 Win11 下 app 兼容；多消费者：驱动支持多 pin/多读，测并发消费 |

### 6.2 发布/合规风险

| # | 风险 | 去风险 |
|---|------|--------|
| 10 | **EV 证书 + 硬件门户**（~$300-400/年 + attestation lead time） | 尽早采购 EV 证书 + 注册 MS 硬件 dashboard，别到首发才卡 |
| 11 | **license 合规**（核心 Apache-2.0 干净、NDI 隔离、Qt6 LGPL 动态链接义务） | CI 从 day1 跑 reuse/SPDX 审计；核实 Qt6 LGPL 义务（提供目标文件/可重链说明） |
| 12 | macOS 移植(v5)：D3D12→Metal（bgfx 有 Metal 后端，渲染 OK）+ 驱动 AVStream→CoreMediaIO（macOS 专属硬骨头） | v1 不处理；v5 立项，渲染后端抽象已留门 |

### 6.3 实现首批三件 throwaway spike（writing-plans 阶段排为前置）

1. 内核驱动可行性（OBS 识别 + 签名流程 + 用户态 DeviceIoControl 推帧 + 引擎未运行时黑帧输出）
2. 跨 device 纹理共享链 + 驱动读回（iGPU copy-free GPU 管线 + 末帧 GPU→CPU 读回/IOCTL 推送 + bgfx 原生句柄 + 多 GPU 同 adapter 约束 + 可共享格式集）
3. 人脸模型 iGPU DirectML 性能（含 #5 选型）

三件 spike 过了，再按 writing-plans 铺开 v1 正式实现。

## 7. 测试与验证

分层：GoogleTest/Catch2 单测为基（CI 跑）；perf/driver/visual 三类需实机（self-hosted 或手动）。

1. **单测（CI）**
   - filters：双边滤波正确性(合成输入验像素统计)、face-reshape 形变数学(关键点→位移)、提亮/白平衡增量
   - presets：level→参数向量映射、预置 load/save(JSON 往返)、override 解析(level 基准+单效果 override→最终向量)、override 生命周期(切 level 保留/切预置清零)
   - c-api：ABI 行为(level/fps/effect-param get/set、opaque handle 生命周期、线程安全、错误码)
   - 无人脸降级逻辑(无脸/关键点不稳定时瘦脸/大眼禁用、磨皮留全帧)
2. **集成（本地实机）**：合成视频文件当"摄像头"，跑 capture→face→beauty→virtualcam sink 全链验输出帧；驱动侧 test harness 读虚拟摄像头输出断言分辨率/帧率/格式
3. **性能门禁（iGPU 参考机 Iris Xe，最关键）**：测每阶段延迟，30fps 默认全开效果@1080p；**门禁 = 总 ≤33.3ms(30fps 帧时间 33.33ms)**，附预算分解；**测持续负载 5-10min**(非瞬态，验热降频)；回归按 commit 触发(self-hosted iGPU runner 或合入前手动)；24/30 在 iGPU 测(承诺线)，60 尽力(报告不门禁)
4. **驱动兼容矩阵（最风险外表面）**：已签名虚拟摄像头在 OBS(两市场首要) + Zoom/Teams/Discord/钉钉/腾讯会议(识别基础) 被识别且帧正确；跨 Win10/11、GPU 厂商(Intel iGPU / NVIDIA / AMD dGPU)、单/混合 GPU 拓扑；MSI(PnPUtil) 装/卸干净；**多消费者并发**(OBS+Zoom 同时读)
5. **视觉质量验证（"自然不塑料"）**：真人脸素材库(多肤色/性别/眼镜/运动)；按预置 level 人工视觉评审；对照 GPUPixel/GPUImage 输出做 sanity；无人脸/多人/半脸/遮挡侧脸边角
6. **i18n**：中英串加载、无硬编码用户可见串、CJK 字体渲染(UI+叠加)
7. **license 合规（CI）**：reuse/SPDX 扫描，核心无 GPL 污染、Qt6 LGPL 义务文档化
8. **smoke 回归**：每支持配置跑 app 启动→摄像头开→level 切→fps 切→退出
9. **预览路径 + 引擎离线**：预览回调帧率达标、回调在引擎线程触发、UI marshal 正确不卡顿；引擎未运行时虚拟摄像头输出黑帧、不卡消费端

## 8. 技术栈总表

| 方向 | 选型 |
|------|------|
| 平台 | Windows 优先（macOS v5） |
| 核心语言 | C++17 |
| 采集 | Windows Media Foundation（含 MJPEG 解码） |
| 渲染/计算后端 | bgfx（Windows pin D3D12 对齐 DirectML）；自写 shader，算法参考 GPUPixel/GPUImage |
| 人脸 ML | ONNX Runtime + DirectML EP，轻量人脸关键点模型 |
| 虚拟摄像头 | 内核态 AVStream minidriver（WDK + EV 证书 attestation 签名；用户态 DeviceIoControl 推帧） |
| 输出格式 | NV12 为主、ARGB 兜底；默认 1080p |
| UI | Qt6（v1）；引擎经 C ABI 与 UI 解耦，未来可换 WinUI3/SwiftUI |
| AR/合成层（v3） | tgfx 候选 |
| NDI（v4） | NDI® SDK，可选专有插件，核心开源构建外 |
| 构建 | CMake + CMakePresets；依赖混合获取（Qt6/ONNX RT 预编译+find_package，bgfx vcpkg/FetchContent，小库 FetchContent） |
| 依赖管理 | 非单一工具，按依赖取最合适方式 |
| License | Apache-2.0；驱动 source-available（签名二进制由我们分发）；NDI 隔离；Qt6 LGPL 动态链接 |
| CI | GitHub Actions（build/单测/lint/license）；驱动签名+打包走 release workflow secrets；iGPU 性能 self-hosted/手动 |

## 9. 延迟/后置决策

- **bgfx 自抽象/封装 API**：以后再说。day1 用 bgfx 薄封装；需要 Metal/Vulkan 后端、或 GL 性能不能满足、或想换后端时再做自抽象。
- **tgfx**：v3 AR/合成层候选，届时评估。
- **NDI**：v4 可选专有插件；或后续用 SRT/WebRTC 纯开源替代做网络推流。
- **通话场景延迟压低**：v5（macOS + 通话 app 兼容）阶段再处理，v1 不压通话延迟。
- **人脸模型最终选型**：spike benchmark 后定（MediaPipe 468-pt vs 更轻 68/106/190-pt）。
- **dGPU 60fps 路线**：跨 adapter 拷贝权衡留 spike，v1 不承诺集显 60 流畅。
- **像素-buffer 预览模式**：`sf_preview_query_format` 支持与否待实现期定，看 UI 端需求。

# SparkFace v1（MVP）设计规格

> 状态：Phase-0 修订版，Spike A/A2 已接受单消费者限制 · 初稿：2026-08-15 · 修订：2026-11-08 · License：Apache-2.0

## 0. 概述

SparkFace（星颜）是面向他人的商业桌面美颜软件：实时处理摄像头画面，把美化结果作为**虚拟摄像头**输出（OBS / Zoom / Teams / Discord / 钉钉 / 腾讯会议等可调用），核心开源（Apache-2.0）。v1 暴露一个虚拟摄像头设备，同一时间支持一个消费应用。

- **主打差异化**：免费/开源 + 美颜分级可快速设置 + 兼容自然美颜 → 美妆 → 娱乐 AR 全谱。
- **首发场景优先级**：直播/推流 + 内容录制 优先；实时视频通话/会议 后置（延迟要求更严，后续阶段再压）。
- **首发市场**：中英双语并重。

> MVP 阶段虚拟摄像头的任务是“做一个 OBS 和主流会议软件能分别识别和使用的视频设备”。v1 最低系统为 Windows 11 build 22000，使用系统用户态虚拟摄像头 API；消费应用必须单独使用该设备，不承诺 OBS 与会议软件或两个会议软件并发取流。双语主要增加 i18n 与双分发渠道，单消费者兼容矩阵仍须实机验证。

## 1. 范围

### 1.1 v1（MVP）范围

- 采集：Media Foundation 摄像头采集
- 人脸：轻量人脸关键点模型（ONNX Runtime + DirectML EP）
- 自然美颜效果：磨皮（双边滤波 + 肤肤 mask）、提亮、瘦脸（网格形变）、大眼（网格形变）
- 分级预设系统：分级(level) / 预置(style) / 自定义 三层控制
- 虚拟摄像头输出：一个 Windows 11 `MFCreateVirtualCamera` 设备 + 自定义 Media Foundation media source（用户态）；同一时间支持一个消费应用
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

多消费者并发属于 post-v1 能力，版本和实现路线（多设备别名、应用插件或 AVStream 驱动）尚未承诺。

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
   → [输出端: Windows 用户态虚拟摄像头]
        → 首选：DXGI surface → MFCreateDXGISurfaceBuffer → IMFSample
        → 实测兜底：一次 GPU→CPU 读回 → 有界共享内存 ring
        → 自定义 Media Foundation source → Windows Frame Server → OBS 等
   ↑ 控制回路: Qt6 UI(C ABI) 下发 level / fps / 效果参数
```

### 2.1 GPU device、同步与虚拟摄像头交接（iGPU 30fps 生死线）

- **共享链里三个用户态 D3D device 实例**：MF 采集 device（D3D11，源纹理持有者）+ DirectML 的 D3D12 device（人脸 ML）+ bgfx 渲染 device（D3D12，pin 以对齐 DirectML）。虚拟摄像头同样是用户态 custom media source，由 Windows Frame Server 激活。
- **GPU 管线内**（采集→人脸→美颜）通过 DXGI NT shared handle 共享资源；D3D11↔D3D12 同步使用共享 `ID3D12Fence`（D3D11 侧通过 `ID3D11Device5::OpenSharedFence`）和明确的资源状态/所有权协议，不把 `IDXGIKeyedMutex` 当作 D3D12 同步原语。目标是零逐帧 CPU 拷贝，须由 Spike B 证明。
- **虚拟摄像头交接首选 GPU surface**：末帧包装为 `MFCreateDXGISurfaceBuffer`/`IMFSample` 交给 custom media source。Frame Server 是否保留 GPU surface、支持哪些格式，必须实测，不能先验宣称零拷贝。
- **兜底仅在实测需要时启用**：若 GPU-backed sample 不受支持或不稳定，使用持久分配的 readback buffer + 有界命名共享内存 ring；不允许逐帧分配。Spike B 单独量出读回、IPC 和 media-source delivery 成本。
- **链路**：采集纹理(MF/D3D11) → shared handle + fence → DirectML/D3D12 → bgfx/D3D12 → GPU-backed MF sample，或经一次实测兜底读回 → custom media source → Frame Server。
- **#1 集成风险**，早期 spike 验证（见 §6）。bgfx 在 Windows pin 到 D3D12；Spike B 决定最终 camera handoff，不在设计阶段伪定零拷贝或必然读回。

### 2.2 帧预算（iGPU Iris Xe @30fps, 33ms, 默认 1080p）

| 阶段 | 预算（粗估，spike 实测确认） |
|------|------|
| 采集 + MJPEG 解码（若摄像头出 MJPEG） | ~0-6ms（原生 NV12/YUY2 省回解码；解码输出格式须可 DXGI 共享） |
| 跨 device fence 同步 + resource transition | ~1-3ms |
| 人脸 ML（轻量模型） | ~5-8ms |
| 美颜 pass | ~6-10ms |
| 虚拟摄像头交接 | ~0-3ms（GPU-backed sample 为目标；读回+共享内存为实测兜底） |
| Frame Server / OS 调度抖动 | ~2-4ms |

粗估合计约 14-34ms；高端已超过 30fps 帧时间(33.3ms)，没有可假定的安全余量。各阶段必须在 Iris Xe 上实测，爆预算按 §6.1 #3 兜底。

**输出分辨率/格式**：默认 1080p；**NV12 为主、BGRA8 兜底**。Spike A 确认 Frame Server 与消费端接受的格式，Spike B 确认 D3D11↔D3D12 共享及 GPU-backed sample 可用的交集。

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
│   ├── sinks/             # 输出端接口 + virtual-camera producer(v1)
│   └── presets/           # 分级预设系统
├── virtualcam/            # MF custom media source + registrar + 独立 frame-transport consumer
├── plugin/
│   └── ndi/               # NDI(v4，专有，core 构建外)
└── c-api/                 # 对外 C ABI facade(extern "C" + opaque handle)
```

### 3.1 C ABI 边界（UI 可替换的关键）

core 暴露 `extern "C"` + opaque handle（如 `sf_engine_t*`）。Qt6 app 只调 C ABI + 渲染预览。换 WinUI3(C++/WinRT 或 C#) / SwiftUI(Swift) 不动引擎——C ABI 是 Swift 和 C# 都好消费的唯一公共面。

- **生命周期**：`sf_init(...)` → `sf_start(h)` → … → `sf_stop(h)` → `sf_shutdown(h)`；opaque handle 由 `sf_init` 返回、`sf_shutdown` 释放。
- **线程契约**（三种 UI 工具包都必须遵守）：引擎在自有线程跑采集/处理；C ABI 所有调用**线程安全**，可从 UI 线程直接调；**回调（预览帧就绪等）在引擎线程触发**，UI 侧必须 marshal 回 UI 线程后再碰 widget。
- **预览路径**：引擎提供专用预览回调 `sf_register_preview_callback(h, fn, userdata)`，每处理帧就绪回调一次。回调载荷**默认是 GPU 共享预览纹理句柄**（UI 直接渲染、零拷贝）；`sf_preview_query_format` 可查是否支持像素-buffer 模式（给不能导入 GPU 句柄的 UI 兜底）。预览不经过 Frame Server 虚拟摄像头路径。
- **摄像头选择**：`sf_enum_cameras(h, ...) → count` + `sf_select_camera(h, index)`。
- **错误模型**：所有 C ABI 函数返回 `sf_result_t`（int 枚举：`SF_OK` / `SF_ERR_*`）；`sf_init` 失败返回 NULL；非法参数返回 `SF_ERR_INVALID_ARG`，设备/模型不可用返回对应码。
- **ABI 版本**：头文件带 `SF_ABI_VERSION` 宏 + `sf_query_abi_version()`；新增效果/参数走加法不破坏消费者，破坏性变更 bump major。

### 3.2 依赖方向

`app → c-api → core 各模块`；`core/sinks` 的 producer 与 `virtualcam` media source 只共享窄的 frame-transport 协议，media source 不链接整个 engine；plugin 经插件接口与 core 解耦。引擎内部模块不互依 UI。

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

### 4.2 构建目标

- ① 用户态 app + core + `virtualcam`：CMake + 依赖，MSVC x64；virtual-camera media source 是独立 DLL target，registrar 是独立 executable target
- ② 可选 NDI 插件：专有，仅在 `SPARKFACE_BUILD_NDI=ON` 时编译，不进默认开源产物

v1 没有内核驱动构建轨道，不需要 `.sys`、`.inf`、`.cat`、EV 证书或微软硬件门户 attestation。

### 4.3 虚拟摄像头安装与分发

安装器部署 custom media-source DLL 和 registrar，注册 COM activation，然后通过 `MFCreateVirtualCamera` 创建摄像头。卸载必须先 `IMFVirtualCamera::Remove`，再注销 media source 并删除文件。CurrentUser/AllUsers、COM 注册位置、是否需要管理员权限及重启恢复策略由 Spike A 实测后写入正式 v1 计划。

### 4.4 打包发布

WiX(MSI) 安装器捆绑 app、custom media-source DLL 与 registrar；macOS 后续使用 pkg/notarize。版本号遵循 git_rules（SemVer `vX.Y.Z[-rc.N]`，gray rc → prod）。

### 4.5 CI/CD（GitHub Actions）

- PR：build(MSVC) + 单测 + custom media-source contract tests + clang-format/tidy lint + license 合规(reuse)
- 虚拟摄像头消费端兼容和 iGPU 30fps 门禁无法在普通公共 CI 可靠执行 → self-hosted Windows 11 iGPU runner 或合入前手动回归

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
| 1 | **Windows 11 user-mode virtual camera**：custom media source 由 Frame Server 激活，涉及 COM 注册、生命周期、隐私权限、格式协商和消费端差异 | **Spike A/A2：GO-WITH-CAVEAT**；OBS 与腾讯会议单独使用均通过，真实应用并发失败。v1 明确采用单设备、单消费者契约 |
| 2 | **跨 device GPU 链 + camera handoff**：D3D11→D3D12 共享、fence/state ownership、DirectML/bgfx interop、Frame Server 是否保留 GPU surface 都未验 | **Spike B**：NT shared handle + shared fence；验证 DirectML I/O binding、bgfx native resource、`MFCreateDXGISurfaceBuffer`；不通才测一次 readback + shared-memory fallback |
| 3 | **iGPU 30fps 预算**（Iris Xe @33ms 跑采集+解码+人脸+美颜+handoff+调度）没有安全余量 | iGPU 参考机从 day1 当性能门禁；预算实测非估；爆预算兜底：更轻模型、隔帧检测或 24fps |
| 4 | **bgfx compute + 原生资源交接**未验 | Spike B 在 D3D12 下运行 compute，验证支持的 native-resource 接口及是否隐藏复制 |
| 5 | **人脸模型选型**（轻到 iGPU DirectML ~5-8ms、关键点够 face-reshape“自然”） | benchmark MediaPipe 468-pt ONNX 与轻量 68/106/190-pt 候选；选最轻达标模型并指定 fallback |
| 6 | **“自然不塑料”质量天花板** | 真人脸素材早做视觉验证；调预置并对照 GPUPixel/GPUImage 输出做 sanity |
| 7 | **多 GPU 拓扑**：三个 device 不在同一 adapter 时共享链可能失效 | v1 优先同 adapter(iGPU)；Spike B 按 adapter LUID 断言；dGPU 60fps 路线后置 |
| 8 | **MJPEG 解码**：解码延迟及输出格式可能破坏共享链 | 预算计入解码；验证硬解输出格式，否则明确测量转换成本 |
| 9 | **运行时边角**：热降频、摄像头隐私、Frame Server 重启、第二应用误占用 | 10 分钟 soak、隐私开关、Frame Server 恢复；UI/文档明确同一时间仅支持一个消费应用 |

### 6.2 发布/合规风险

| # | 风险 | 去风险 |
|---|------|--------|
| 10 | **Windows 11 build 22000+ 系统下限**缩小可安装范围 | README、安装器与运行时门禁明确系统要求；若将来恢复 Windows 10，作为单独产品决策重新评估 backend，不在 v1 隐式兼容 |
| 11 | **license 合规**（核心 Apache-2.0、微软 sample 派生代码许可证、NDI 隔离、Qt6 LGPL 动态链接义务） | 记录 sample 上游 commit/license；CI 从 day1 跑 reuse/SPDX；文档化 Qt6 LGPL 义务 |
| 12 | macOS 移植(v5)：D3D12→Metal + Windows virtual camera→CoreMediaIO | v1 不处理；v5 立项，渲染后端抽象已留门 |

### 6.3 实现首批三件 throwaway spike（正式实现前置）

1. Windows 11 用户态虚拟摄像头（注册、Frame Server custom media source、共享内存 feeder、格式/消费端、黑帧、生命周期、单消费者；A2 另记录真实应用并发限制）
2. D3D11↔D3D12 GPU 共享与 camera handoff（shared handle + fence/state contract + DirectML/bgfx + GPU-backed MF sample；必要时测 readback fallback）
3. 人脸模型 iGPU DirectML 性能与质量选型

三件 spike 通过后，再编写 v1 正式实现计划。

## 7. 测试与验证

分层：GoogleTest/Catch2 单测为基（CI 跑）；perf/virtual-camera/visual 三类需 Windows 11 实机（self-hosted 或手动）。

1. **单测（CI）**
   - filters：双边滤波正确性(合成输入验像素统计)、face-reshape 形变数学(关键点→位移)、提亮/白平衡增量
   - presets：level→参数向量映射、预置 load/save(JSON 往返)、override 解析(level 基准+单效果 override→最终向量)、override 生命周期(切 level 保留/切预置清零)
   - c-api：ABI 行为(level/fps/effect-param get/set、opaque handle 生命周期、线程安全、错误码)
   - virtualcam：media-source stream state、时间戳/帧率、格式协商、黑帧状态、frame-transport ring 边界
   - 无人脸降级逻辑(无脸/关键点不稳定时瘦脸/大眼禁用、磨皮留全帧)
2. **集成（本地实机）**：合成视频文件当输入，跑 capture→face→beauty→virtualcam producer→custom media source 全链；测试消费者断言分辨率/帧率/格式
3. **性能门禁（Iris Xe）**：测每阶段延迟，30fps 默认全开效果@1080p；总 p95 ≤33.3ms；持续负载 10min 验热降频；24/30 为承诺线，60 只报告不门禁
4. **虚拟摄像头兼容矩阵**：Windows 11 支持版本上，OBS、Zoom/Teams/Discord/钉钉/腾讯会议逐一单独验证识别与帧正确；Intel/NVIDIA/AMD、单/混合 GPU；安装/卸载无残留。v1 不把两个应用同时打开同一设备列为通过条件
5. **视觉质量验证**：真人脸素材库覆盖多肤色/性别/眼镜/运动；按 level 人工评审；无人脸/多人/半脸/遮挡/侧脸边角
6. **i18n**：中英串加载、无硬编码用户可见串、CJK 字体渲染(UI+叠加)
7. **license 合规（CI）**：reuse/SPDX 扫描，核心无 GPL 污染、微软 sample 归属正确、Qt6 LGPL 义务文档化
8. **smoke 回归**：app 启动→摄像头开→level/fps 切换→虚拟摄像头消费→退出
9. **预览路径 + 引擎离线**：预览回调帧率与线程契约正确；feeder 停止时虚拟摄像头持续黑帧，恢复 feeder 后无需重开消费者

## 8. 技术栈总表

| 方向 | 选型 |
|------|------|
| 平台 | Windows 11 build 22000+（macOS v5） |
| 核心语言 | C++17 |
| 采集 | Windows Media Foundation（含 MJPEG 解码） |
| 渲染/计算后端 | bgfx（Windows pin D3D12 对齐 DirectML）；自写 shader，算法参考 GPUPixel/GPUImage |
| 人脸 ML | ONNX Runtime + DirectML EP，轻量人脸关键点模型 |
| 虚拟摄像头 | 一个 `MFCreateVirtualCamera` + custom Media Foundation source（用户态）；同一时间一个消费应用 |
| 输出格式 | NV12 为主、BGRA8 兜底；默认 1080p |
| UI | Qt6（v1）；引擎经 C ABI 与 UI 解耦，未来可换 WinUI3/SwiftUI |
| AR/合成层（v3） | tgfx 候选 |
| NDI（v4） | NDI® SDK，可选专有插件，核心开源构建外 |
| 构建 | CMake + CMakePresets；依赖混合获取（Qt6/ONNX RT 预编译+find_package，bgfx vcpkg/FetchContent，小库 FetchContent） |
| 依赖管理 | 非单一工具，按依赖取最合适方式 |
| License | Apache-2.0；NDI 隔离；Qt6 LGPL 动态链接；微软 sample 派生代码保留上游归属 |
| CI | GitHub Actions（build/单测/lint/license）；虚拟摄像头兼容与 iGPU 性能走 self-hosted/手动 |

## 9. 延迟/后置决策

- **bgfx 自抽象/封装 API**：以后再说。day1 用 bgfx 薄封装；需要 Metal/Vulkan 后端、或 GL 性能不能满足、或想换后端时再做自抽象。
- **tgfx**：v3 AR/合成层候选，届时评估。
- **NDI**：v4 可选专有插件；或后续用 SRT/WebRTC 纯开源替代做网络推流。
- **Windows 10**：v1 明确不支持；若未来有市场需求，单独评估 backend，不为其保留内核驱动占位。
- **通话场景延迟压低**：v5（macOS + 通话 app 兼容）阶段再处理，v1 不压通话延迟。
- **人脸模型最终选型**：spike benchmark 后定（MediaPipe 468-pt vs 更轻 68/106/190-pt）。
- **dGPU 60fps 路线**：跨 adapter 拷贝权衡留 spike，v1 不承诺集显 60 流畅。
- **像素-buffer 预览模式**：`sf_preview_query_format` 支持与否待实现期定，看 UI 端需求。
- **多消费者虚拟摄像头**：post-v1 重新立项；v1 不承诺两个应用同时打开同一设备。届时在多设备别名、应用插件和 AVStream 驱动之间按兼容性与维护成本重新选型。

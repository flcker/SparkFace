# SparkFace Documentation / 文档

Long-form project documentation lives under `docs/`. The root READMEs remain the short project overview and entry point.

项目的长篇文档统一存放在 `docs/`。仓库根目录 README 仅保留项目概览和入口。

## Index / 索引

| Area | Document | Purpose |
|---|---|---|
| Specifications / 规格 | [v1 MVP design](specs/2026-08-15-v1-mvp-design.md) | Product scope, architecture, C ABI, performance targets, risks |
| Development / 开发 | [Phase-0 Windows environment](development/phase0-windows-environment.md) | Prerequisites, dependency locks, build command, security constraints, verified result |
| Plans / 计划 | [Phase-0 spikes](plans/2026-08-15-phase0-spikes.md) | Executable feasibility plan and measured verdict log |
| Plans / 计划 | [Spike A2 multi-consumer recovery](plans/2026-11-06-phase0-spike-a2-multiconsumer.md) | Corrective Frame Server sharing, broadcast IPC, latency, and dual-client gate |
| Plans / 计划 | [Spike B/B2 fallback evidence](plans/2026-11-10-phase0-spike-b2-fallback.md) | GPU interop failures, preallocated CPU fallback, Frame Server latency, recovery, and real-app evidence |
| Plans / 计划 | [Spike C DirectML face-model verdict](plans/2026-11-12-phase0-spike-c-face-model.md) | Candidate provenance, quality checks, DirectML latency, and model-selection NO-GO |
| Plans / 计划 | [Spike D D3D11 handoff verdict](plans/2026-11-13-phase0-spike-d-d3d11-handoff.md) | Cross-session adapter-LUID failure for the Frame Server shared-texture path |

## Current Gate Status / 当前门禁状态

- Task 0 Windows 11 environment gate: **GO**.
- Spike A/A2 user-mode virtual camera: **GO-WITH-CAVEAT** — OBS and Tencent Meeting pass individually; v1 supports one virtual-camera consumer at a time, and concurrent consumers are deferred post-v1.
- Spike B/B2/B3/B4 GPU sharing and camera handoff: **NO-GO** — the GPU-resident chain is unsupported; the CPU fallback is correct, but B4 still measured `48.758ms` p95 at `26.644fps`, failing both the 33.3ms latency and 29.5fps gates.
- Spike C face-model DirectML performance: **NO-GO** — both candidates passed visual quality, but the lighter fallback still measured `9.8847ms` p95 against the `8.0ms` inference gate.
- Spike D cross-process D3D11 handoff: **NO-GO** — the interactive Iris Xe producer and Frame Server source allocator exposed different adapter LUIDs (`00000000:0000ffd8` vs. `00000000:000178d8`), so zero frames crossed the required same-adapter shared-texture path.
- Product implementation remains blocked until the transport and face-model NO-GOs are resolved or the affected product constraints are explicitly revised.

## Documentation Rules / 文档规则

- Specification documents record durable product decisions and constraints.
- Plans use a `YYYY-MM-DD-<topic>.md` filename and contain executable acceptance criteria.
- Measured results belong in the relevant plan's verdict log; do not replace measurements with estimates.
- Development guides document only commands that have been exercised on the supported workstation.
- Update this index whenever a long-form document is added, moved, or removed.

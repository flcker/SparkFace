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

## Current Gate Status / 当前门禁状态

- Task 0 Windows 11 environment gate: **GO**.
- Spike A/A2 user-mode virtual camera: **GO-WITH-CAVEAT** — OBS and Tencent Meeting pass individually; v1 supports one virtual-camera consumer at a time, and concurrent consumers are deferred post-v1.
- Spike B/B2 GPU sharing and camera handoff: **NO-GO** — the GPU-resident chain is unsupported; the CPU fallback is correct and sustains synthetic 1080p30 in OBS and Tencent Meeting, but measured `78.099ms` p95 producer-to-client latency exceeds the 33.3ms product budget before face ML and production shaders.
- Spike C face-model DirectML performance: pending; blocked by the unaccepted Spike B NO-GO.
- Product implementation remains blocked until the Spike B NO-GO is resolved and all remaining Phase-0 gates return GO or an explicitly accepted GO-WITH-CAVEAT.

## Documentation Rules / 文档规则

- Specification documents record durable product decisions and constraints.
- Plans use a `YYYY-MM-DD-<topic>.md` filename and contain executable acceptance criteria.
- Measured results belong in the relevant plan's verdict log; do not replace measurements with estimates.
- Development guides document only commands that have been exercised on the supported workstation.
- Update this index whenever a long-form document is added, moved, or removed.

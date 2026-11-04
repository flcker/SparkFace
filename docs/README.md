# SparkFace Documentation / 文档

Long-form project documentation lives under `docs/`. The root READMEs remain the short project overview and entry point.

项目的长篇文档统一存放在 `docs/`。仓库根目录 README 仅保留项目概览和入口。

## Index / 索引

| Area | Document | Purpose |
|---|---|---|
| Specifications / 规格 | [v1 MVP design](specs/2026-08-15-v1-mvp-design.md) | Product scope, architecture, C ABI, performance targets, risks |
| Development / 开发 | [Phase-0 Windows environment](development/phase0-windows-environment.md) | Prerequisites, dependency locks, build command, security constraints, verified result |
| Plans / 计划 | [Phase-0 spikes](plans/2026-08-15-phase0-spikes.md) | Executable feasibility plan and measured verdict log |

## Current Gate Status / 当前门禁状态

- Task 0 Windows 11 environment gate: **GO**.
- Spike A user-mode virtual camera: pending.
- Spike B GPU sharing and camera handoff: pending.
- Spike C face-model DirectML performance: pending.
- Product implementation starts only after all Phase-0 spikes return GO or an explicitly accepted GO-WITH-CAVEAT.

## Documentation Rules / 文档规则

- Specification documents record durable product decisions and constraints.
- Plans use a `YYYY-MM-DD-<topic>.md` filename and contain executable acceptance criteria.
- Measured results belong in the relevant plan's verdict log; do not replace measurements with estimates.
- Development guides document only commands that have been exercised on the supported workstation.
- Update this index whenever a long-form document is added, moved, or removed.

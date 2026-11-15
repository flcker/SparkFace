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
| Plans / 计划 | [Spike C DirectML face-model verdict](plans/2026-11-12-phase0-spike-c-face-model.md) | Candidate provenance, fixed-batch DirectML retest, complete-stage latency, and selected v1 model path |
| Plans / 计划 | [Spike D D3D11 handoff verdict](plans/2026-11-13-phase0-spike-d-d3d11-handoff.md) | Cross-session adapter-LUID failure for the Frame Server shared-texture path |
| Plans / 计划 | [Transport architecture selection plan](plans/2026-11-14-virtual-camera-transport-selection.md) | Executed architecture inventory, staged kill gates, compatibility matrix, and decision rules |
| Plans / 计划 | [Transport selection verdict](plans/2026-11-14-phase0-transport-selection-verdict.md) | Wave-1 replacement transport measurements, rejected candidates, and no-selection decision |
| Plans / 计划 | [H8A source-local physical-camera verdict](plans/2026-11-15-phase0-source-local-physical-camera.md) | RCA-corrected physical-camera evidence and the final throughput/latency no-selection impact |

## Current Gate Status / 当前门禁状态

- Task 0 Windows 11 environment gate: **GO**.
- Spike A/A2 user-mode virtual camera: **GO-WITH-CAVEAT** — OBS and Tencent Meeting pass individually; v1 supports one virtual-camera consumer at a time, and concurrent consumers are deferred post-v1.
- Spike B/B2/B3/B4 GPU sharing and camera handoff: **NO-GO** — the GPU-resident chain is unsupported; the CPU fallback is correct, but B4 still measured `48.758ms` p95 at `26.644fps`, failing both the 33.3ms latency and 29.5fps gates.
- Spike C face-model DirectML performance: **GO** — fixed-batch 468-point Face Mesh measured `2.2347ms` landmark-only p95; BlazeFace → ROI → Face Mesh measured `5.4926ms` p95 across 1,000 complete per-frame stages with zero frames over the `8.0ms` gate.
- Spike D cross-process D3D11 handoff: **NO-GO** — the interactive Iris Xe producer and Frame Server source allocator exposed different adapter LUIDs (`00000000:0000ffd8` vs. `00000000:000178d8`), so zero frames crossed the required same-adapter shared-texture path.
- Transport architecture selection: **NO-GO** — S1 deterministic source-local rendering and H9 protocol-v2 productization passed. Corrected H8A sustained 1,000 contiguous physical-camera frames with zero correctness errors or pre-Frame-Server CPU copies, but reached only `29.327fps` and `50.167ms` capture-to-client p95 against the `29.5fps` and `33.3ms` gates. The source-adapter handoff had already failed at `46.001ms` p95; no transport primary or technical fallback is selected.
- Product implementation remains blocked by the unresolved virtual-camera architecture; the face-model gate is resolved.

## Documentation Rules / 文档规则

- Specification documents record durable product decisions and constraints.
- Plans use a `YYYY-MM-DD-<topic>.md` filename and contain executable acceptance criteria.
- Measured results belong in the relevant plan's verdict log; do not replace measurements with estimates.
- Development guides document only commands that have been exercised on the supported workstation.
- Update this index whenever a long-form document is added, moved, or removed.

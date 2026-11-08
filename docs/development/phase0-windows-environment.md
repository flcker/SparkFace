# Phase-0 Windows 11 Environment Gate

**Status:** GO  
**Verified:** 2026-11-05  
**Scope:** Throwaway Phase-0 feasibility environment; not the SparkFace product build.

## Purpose

The gate proves that the reference workstation can:

- load the Windows 11 `MFCreateVirtualCamera` API,
- create a D3D12 device on Intel Iris Xe,
- initialize bgfx with the external D3D12 device and submit a frame,
- execute a pinned ONNX model through ONNX Runtime DirectML with CPU fallback disabled,
- build the same dependency graph from repository-pinned inputs.

Camera registration and frame delivery are Spike A work. GPU sharing and readback measurements are Spike B work.

## Required Host

| Requirement | Minimum |
|---|---|
| OS | Windows 11 build 22000 or newer |
| Reference GPU | Intel Iris Xe |
| Compiler | Visual Studio 2022 or newer with Desktop development with C++ |
| SDK | Windows SDK 10.0.22000.0 or newer |
| Build tools | CMake 3.27 or newer, Git, Visual Studio Ninja component |

WDK, Qt, and an EV code-signing certificate are not required for this gate.

## Dependency Locks

| Dependency | Lock |
|---|---|
| vcpkg tool and builtin registry | Commit `a4f7741c53c97354fc43222fea17955c657dd549` |
| vcpkg mode | Manifest mode, triplet `x64-windows` |
| bgfx port | Explicit override `1.129.8940-496#1` |
| ONNX Runtime DirectML | Official NuGet package `1.24.4`, URL and SHA-256 fixed in `spikes/env/CMakeLists.txt` |
| Gate ONNX model | Upstream `mul_1.onnx`, SHA-256 fixed in `spikes/env/CMakeLists.txt` |

The repository bootstrap script installs the pinned vcpkg checkout under ignored `build/tools/vcpkg`. Do not use an arbitrary system or Visual Studio-bundled vcpkg checkout.

## Build and Run

From the repository root:

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\build-phase0-env.ps1 -Run
```

This is the supported entry point. It enters the Visual Studio developer environment, selects the repository-pinned vcpkg toolchain, configures with the preset, builds, and runs the gate. Running `cmake --build` from a plain shell can omit MSVC and Windows SDK include paths.

## PASS Contract

A successful run exits with code `0` and includes:

```text
virtual_camera_api=MFCreateVirtualCamera
bgfx_renderer=Direct3D 12
directml_provider=DmlExecutionProvider
directml_first_output=2
ENV_GATE_PASS
```

The gate also prints the CMake, MSVC, Windows SDK, vcpkg baseline, bgfx port, ONNX Runtime, OS, and adapter identities so dependency or host drift is visible.

## Verified Result

| Field | Observed value |
|---|---|
| Windows | `10.0.26300` |
| GPU | `Intel(R) Iris(R) Xe Graphics`, driver `32.0.101.7088` |
| CMake | `4.4.3` |
| MSVC | `19.51.36256.0` |
| Windows SDK | `10.0.26100.0` |
| bgfx port | `1.129.8940-496#1` |
| ONNX Runtime DirectML | `1.24.4` |
| DirectML result | finite output `2`, CPU fallback disabled |
| Final verdict | `ENV_GATE_PASS` |

## Smart App Control

Windows 11 Smart App Control's `VerifiedAndReputable Desktop` Code Integrity policy blocks locally built unsigned executables and does not support a per-application exception.

Preferred development choices:

1. Use a dedicated developer workstation where Smart App Control is disabled.
2. Use a trusted code-signing certificate for local binaries.

Disabling Smart App Control is system-wide and reduces protection against unknown or unsigned applications. Do not delete files from `C:\Windows\System32\CodeIntegrity\CiPolicies\Active` or edit protected Code Integrity registry state directly. Use **Windows Security → App & browser control → Smart App Control settings**.

## Troubleshooting

| Symptom | Cause and action |
|---|---|
| Code Integrity event 3077 names policy `{0283AC0F-FFF1-49AE-ADA1-8A933130CAD6}` | Smart App Control blocked the unsigned gate; use one of the supported development choices above. |
| `stdarg.h` or another SDK header is missing | The build ran outside the Visual Studio developer environment; use `scripts/build-phase0-env.ps1`. |
| bgfx does not report Direct3D 12 | Confirm the Intel adapter is present and use the repository-pinned bgfx build. |
| `DmlExecutionProvider` is unavailable | Confirm the pinned DirectML NuGet runtime DLLs were copied beside `env_gate.exe`. |

## Related Documents

- [v1 MVP design](../specs/2026-08-15-v1-mvp-design.md)
- [Phase-0 spike plan and verdict log](../plans/2026-08-15-phase0-spikes.md)

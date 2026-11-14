# SPDX-License-Identifier: Apache-2.0
[CmdletBinding()]
param()

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"
$repositoryRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot ".."))
$buildRoot = Join-Path $repositoryRoot "build\windows-x64-release-transport-selection"

$vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path -LiteralPath $vswhere -PathType Leaf)) {
    throw "vswhere.exe was not found"
}
$visualStudioRoot = (& $vswhere -latest -products * `
    -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
    -property installationPath)
if (-not $visualStudioRoot) {
    throw "Visual Studio with the x64 C++ toolchain was not found"
}
$devShell = Join-Path $visualStudioRoot "Common7\Tools\Launch-VsDevShell.ps1"
$ninjaDirectory = Join-Path $visualStudioRoot "Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja"
$ninja = Join-Path $ninjaDirectory "ninja.exe"
if (-not (Test-Path -LiteralPath $ninja -PathType Leaf)) {
    throw "The Visual Studio Ninja installation was not found"
}

& $devShell -Arch amd64 -HostArch amd64 -SkipAutomaticLocation
$env:PATH = "$ninjaDirectory;$env:PATH"

Push-Location $repositoryRoot
try {
    & cmake.exe -S . -B $buildRoot -G Ninja `
        "-DCMAKE_BUILD_TYPE=Release" `
        "-DCMAKE_MAKE_PROGRAM=$ninja" `
        "-DSPARKFACE_BUILD_ENV_GATE=OFF" `
        "-DSPARKFACE_BUILD_FALLBACK=OFF" `
        "-DSPARKFACE_BUILD_FACE_SPIKE=OFF" `
        "-DSPARKFACE_BUILD_D3D11_HANDOFF=OFF" `
        "-DSPARKFACE_BUILD_TRANSPORT_SELECTION=ON"
    if ($LASTEXITCODE -ne 0) {
        throw "Transport-selection CMake configure failed with exit code $LASTEXITCODE"
    }
    & cmake.exe --build $buildRoot --target transport_cross_adapter_probe `
        transport_source_adapter_producer transport_source_adapter_consumer `
        transport_source_adapter_registrar transport_shared_buffer_producer `
        transport_activation_local_server transport_codec_fixture
    if ($LASTEXITCODE -ne 0) {
        throw "Transport-selection build failed with exit code $LASTEXITCODE"
    }
    & (Join-Path $repositoryRoot "scripts\build-phase0-d.ps1")
    if ($LASTEXITCODE -ne 0) {
        throw "Source-adapter media-source build failed with exit code $LASTEXITCODE"
    }
    & (Join-Path $repositoryRoot `
        "spikes\transport-selection\media-source\build-shared-buffer.ps1")
    if ($LASTEXITCODE -ne 0) {
        throw "Shared-buffer media-source build failed with exit code $LASTEXITCODE"
    }
} finally {
    Pop-Location
}

# SPDX-License-Identifier: Apache-2.0
[CmdletBinding()]
param(
    [switch]$FetchModels
)

$ErrorActionPreference = "Stop"
$repositoryRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot ".."))
$buildRoot = Join-Path $repositoryRoot "build\windows-x64-release-face"

if ($FetchModels) {
    & (Join-Path $repositoryRoot "spikes\face\fetch-models.ps1")
    if ($LASTEXITCODE -ne 0) {
        throw "Face-model acquisition failed with exit code $LASTEXITCODE"
    }
}

$vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere -PathType Leaf)) {
    throw "vswhere.exe was not found"
}
$visualStudioRoot = (& $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath)
if (-not $visualStudioRoot) {
    throw "Visual Studio with the x64 C++ toolchain was not found"
}
$devShell = Join-Path $visualStudioRoot "Common7\Tools\Launch-VsDevShell.ps1"
$ninjaDirectory = Join-Path $visualStudioRoot "Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja"
$ninja = Join-Path $ninjaDirectory "ninja.exe"
if (-not (Test-Path $ninja -PathType Leaf)) {
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
        "-DSPARKFACE_BUILD_FACE_SPIKE=ON"
    if ($LASTEXITCODE -ne 0) {
        throw "CMake configure failed with exit code $LASTEXITCODE"
    }
    & cmake.exe --build $buildRoot --target face_spike
    if ($LASTEXITCODE -ne 0) {
        throw "Face spike build failed with exit code $LASTEXITCODE"
    }
} finally {
    Pop-Location
}

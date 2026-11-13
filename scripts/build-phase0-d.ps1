# SPDX-License-Identifier: Apache-2.0
[CmdletBinding()]
param()

$ErrorActionPreference = "Stop"
$repositoryRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot ".."))
$buildRoot = Join-Path $repositoryRoot "build\windows-x64-release-d"
$toolsRoot = Join-Path $repositoryRoot "build\tools"
$nuget = Join-Path $toolsRoot "nuget.exe"
$nugetUrl = "https://dist.nuget.org/win-x86-commandline/v7.9.0/nuget.exe"
$nugetSha256 = "992d70cac5b06c38efec91806caba64cdcc07e6d963a0959dbbbaf264d33b800"

function Get-Sha256([string]$Path) {
    $stream = [System.IO.File]::OpenRead($Path)
    try {
        $sha = [System.Security.Cryptography.SHA256]::Create()
        try {
            return ([BitConverter]::ToString($sha.ComputeHash($stream))).Replace("-", "").ToLowerInvariant()
        } finally {
            $sha.Dispose()
        }
    } finally {
        $stream.Dispose()
    }
}

New-Item -ItemType Directory -Force -Path $toolsRoot | Out-Null
if (-not (Test-Path -LiteralPath $nuget -PathType Leaf)) {
    Invoke-WebRequest -UseBasicParsing -Uri $nugetUrl -OutFile $nuget
}
$actualNugetHash = Get-Sha256 $nuget
if ($actualNugetHash -ne $nugetSha256) {
    throw "NuGet SHA-256 mismatch: expected $nugetSha256, got $actualNugetHash"
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
        "-DSPARKFACE_BUILD_FACE_SPIKE=OFF" `
        "-DSPARKFACE_BUILD_D3D11_HANDOFF=ON"
    if ($LASTEXITCODE -ne 0) {
        throw "CMake configure failed with exit code $LASTEXITCODE"
    }
    & cmake.exe --build $buildRoot --target d3d11_handoff_producer d3d11_handoff_consumer d3d11_handoff_registrar
    if ($LASTEXITCODE -ne 0) {
        throw "Spike D executable build failed with exit code $LASTEXITCODE"
    }
    & (Join-Path $repositoryRoot "spikes\d3d11-handoff\media-source\build-upstream.ps1")
    if ($LASTEXITCODE -ne 0) {
        throw "Spike D media-source build failed with exit code $LASTEXITCODE"
    }
} finally {
    Pop-Location
}

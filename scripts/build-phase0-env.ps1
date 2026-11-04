[CmdletBinding()]
param(
    [switch]$Run
)

$ErrorActionPreference = "Stop"
$RepositoryRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot ".."))

. (Join-Path $PSScriptRoot "bootstrap-vcpkg.ps1")
$pinnedVcpkgRoot = $env:VCPKG_ROOT

$vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) {
    throw "vswhere.exe was not found"
}

$visualStudioRoot = (& $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath)
if (-not $visualStudioRoot) {
    throw "Visual Studio with the x64 C++ toolchain was not found"
}

$devShell = Join-Path $visualStudioRoot "Common7\Tools\Launch-VsDevShell.ps1"
$ninjaDirectory = Join-Path $visualStudioRoot "Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja"
$ninja = Join-Path $ninjaDirectory "ninja.exe"
if (-not (Test-Path $ninja)) {
    throw "The Visual Studio Ninja installation was not found"
}

& $devShell -Arch amd64 -HostArch amd64 -SkipAutomaticLocation
$env:VCPKG_ROOT = $pinnedVcpkgRoot
$env:PATH = "$ninjaDirectory;$env:PATH"

Push-Location $RepositoryRoot
try {
    & cmake.exe --preset windows-x64-debug --fresh "-DCMAKE_MAKE_PROGRAM=$ninja" "-DCMAKE_TOOLCHAIN_FILE=$pinnedVcpkgRoot\scripts\buildsystems\vcpkg.cmake"
    if ($LASTEXITCODE -ne 0) {
        throw "CMake configure failed with exit code $LASTEXITCODE"
    }

    & cmake.exe --build --preset windows-x64-debug
    if ($LASTEXITCODE -ne 0) {
        throw "CMake build failed with exit code $LASTEXITCODE"
    }

    if ($Run) {
        $gate = Join-Path $RepositoryRoot "build\windows-x64-debug\spikes\env\env_gate.exe"
        & $gate
        if ($LASTEXITCODE -ne 0) {
            throw "env_gate failed with exit code $LASTEXITCODE"
        }
    }
} finally {
    Pop-Location
}

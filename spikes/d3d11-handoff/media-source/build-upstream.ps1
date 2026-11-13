# SPDX-License-Identifier: Apache-2.0
[CmdletBinding()]
param(
    [string]$WorkRoot = ""
)

$ErrorActionPreference = "Stop"
$commit = "626f8b19c5f367602f2e89c6b314573d3776c9df"
$archiveHash = "d4c4217684d76e6e5934114a4d9ade0d0e6249fef70462d5cc451e3613674eec"
$url = "https://codeload.github.com/microsoft/Windows-Camera/tar.gz/$commit"
$repo = (Resolve-Path (Join-Path $PSScriptRoot "../../..")).Path
if (-not $WorkRoot) {
    $WorkRoot = Join-Path $repo "build/phase0-d-upstream"
}
$archive = Join-Path $WorkRoot "windows-camera.tar.gz"
$source = Join-Path $WorkRoot "Windows-Camera-$commit"
$virtualCamera = Join-Path $source "Samples/VirtualCamera"
$patches = @(
    (Join-Path $PSScriptRoot "windows-camera-b2.patch"),
    (Join-Path $PSScriptRoot "windows-camera-d3d11.patch")
)
$nuget = Join-Path $repo "build/tools/nuget.exe"

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

if (Test-Path $WorkRoot) {
    Remove-Item -Recurse -Force $WorkRoot
}
New-Item -ItemType Directory -Path $WorkRoot | Out-Null
Invoke-WebRequest -Uri $url -OutFile $archive
$actualHash = Get-Sha256 $archive
if ($actualHash -ne $archiveHash) {
    throw "Windows-Camera archive hash mismatch: $actualHash"
}

tar.exe -xf $archive -C $WorkRoot
if ($LASTEXITCODE -ne 0) {
    throw "tar extraction failed with exit code $LASTEXITCODE"
}

$previousCeiling = $env:GIT_CEILING_DIRECTORIES
$applyExit = 0
try {
    $env:GIT_CEILING_DIRECTORIES = $WorkRoot
    Push-Location $virtualCamera
    try {
        foreach ($patch in $patches) {
            git apply --whitespace=nowarn --verbose $patch
            if ($LASTEXITCODE -ne 0) {
                $applyExit = $LASTEXITCODE
                break
            }
        }
    } finally {
        Pop-Location
    }
} finally {
    $env:GIT_CEILING_DIRECTORIES = $previousCeiling
}
if ($applyExit -ne 0) {
    throw "media-source patch failed with exit code $applyExit"
}

& $nuget restore (Join-Path $virtualCamera "VirtualCameraSample.sln") `
    -PackagesDirectory (Join-Path $virtualCamera "packages") -NonInteractive
if ($LASTEXITCODE -ne 0) {
    throw "NuGet restore failed with exit code $LASTEXITCODE"
}

$solutionDir = $virtualCamera + "\"
msbuild (Join-Path $virtualCamera "VirtualCameraMediaSource/VirtualCameraMediaSource.vcxproj") `
    /t:Rebuild /m /p:Configuration=Release /p:Platform=x64 /p:PlatformToolset=v145 `
    "/p:SolutionDir=$solutionDir"
if ($LASTEXITCODE -ne 0) {
    throw "media-source build failed with exit code $LASTEXITCODE"
}

$dll = Join-Path $virtualCamera "x64/Release/VirtualCameraMediaSource.dll"
if (-not (Test-Path $dll)) {
    throw "media-source DLL was not produced at $dll"
}
Write-Output "media_source_dll=$dll"
Write-Output "upstream_commit=$commit"
Write-Output "upstream_archive_sha256=$archiveHash"

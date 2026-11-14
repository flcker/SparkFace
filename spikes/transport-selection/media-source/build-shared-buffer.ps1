# SPDX-License-Identifier: Apache-2.0
[CmdletBinding()]
param([string]$WorkRoot = "")

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"
$commit = "626f8b19c5f367602f2e89c6b314573d3776c9df"
$archiveHash = "d4c4217684d76e6e5934114a4d9ade0d0e6249fef70462d5cc451e3613674eec"
$url = "https://codeload.github.com/microsoft/Windows-Camera/tar.gz/$commit"
$repositoryRoot = (Resolve-Path (Join-Path $PSScriptRoot "../../..")).Path
if (-not $WorkRoot) {
    $WorkRoot = Join-Path $repositoryRoot "build/tsb"
}
$archive = Join-Path $WorkRoot "windows-camera.tar.gz"
$source = Join-Path $WorkRoot "Windows-Camera-$commit"
$virtualCamera = Join-Path $source "Samples/VirtualCamera"
$mediaSource = Join-Path $virtualCamera "VirtualCameraMediaSource"
$patches = @(
    (Join-Path $repositoryRoot "spikes/fallback/media-source/windows-camera-b2.patch"),
    (Join-Path $PSScriptRoot "windows-camera-shared-buffer.patch"),
    (Join-Path $PSScriptRoot "windows-camera-compressed.patch"),
    (Join-Path $PSScriptRoot "windows-camera-reduced-modes.patch")
)
$nuget = Join-Path $repositoryRoot "build/tools/nuget.exe"

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

if (-not (Test-Path -LiteralPath $nuget -PathType Leaf)) {
    throw "Pinned NuGet executable is missing: $nuget"
}
if (Test-Path -LiteralPath $WorkRoot) {
    Remove-Item -Recurse -Force $WorkRoot
}
New-Item -ItemType Directory -Path $WorkRoot | Out-Null
Invoke-WebRequest -UseBasicParsing -Uri $url -OutFile $archive
$actualHash = Get-Sha256 $archive
if ($actualHash -ne $archiveHash) {
    throw "Windows-Camera archive hash mismatch: $actualHash"
}

tar.exe -xf $archive -C $WorkRoot
if ($LASTEXITCODE -ne 0) {
    throw "tar extraction failed with exit code $LASTEXITCODE"
}

$normalizedPatchRoot = Join-Path $WorkRoot "normalized-patches"
New-Item -ItemType Directory -Path $normalizedPatchRoot | Out-Null
$normalizedPatches = foreach ($patch in $patches) {
    $normalizedPatch = Join-Path $normalizedPatchRoot ([IO.Path]::GetFileName($patch))
    $patchText = [IO.File]::ReadAllText($patch).Replace("`r`n", "`n")
    [IO.File]::WriteAllText($normalizedPatch, $patchText,
        [Text.UTF8Encoding]::new($false))
    $normalizedPatch
}

$previousCeiling = $env:GIT_CEILING_DIRECTORIES
$applyExit = 0
try {
    $env:GIT_CEILING_DIRECTORIES = $WorkRoot
    Push-Location $virtualCamera
    try {
        foreach ($patch in $normalizedPatches) {
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
    throw "shared-buffer media-source patch failed with exit code $applyExit"
}

Copy-Item -LiteralPath (Join-Path $PSScriptRoot "../mf-shared-buffer/SharedMemorySampleSource.h") `
    -Destination (Join-Path $mediaSource "SharedMemorySampleSource.h")
Copy-Item -LiteralPath (Join-Path $PSScriptRoot "../mf-shared-buffer/shared_ring.h") `
    -Destination (Join-Path $mediaSource "SharedRingProtocol.h")
Copy-Item -LiteralPath (Join-Path $PSScriptRoot "../codec-bridge/CompressedFrameSource.h") `
    -Destination (Join-Path $mediaSource "CompressedFrameSource.h")

& $nuget restore (Join-Path $virtualCamera "VirtualCameraSample.sln") `
    -PackagesDirectory (Join-Path $virtualCamera "packages") -NonInteractive
if ($LASTEXITCODE -ne 0) {
    throw "NuGet restore failed with exit code $LASTEXITCODE"
}

$solutionDirectory = $virtualCamera + "\"
msbuild (Join-Path $mediaSource "VirtualCameraMediaSource.vcxproj") `
    /t:Rebuild /m /p:Configuration=Release /p:Platform=x64 /p:PlatformToolset=v145 `
    "/p:SolutionDir=$solutionDirectory"
if ($LASTEXITCODE -ne 0) {
    throw "shared-buffer media-source build failed with exit code $LASTEXITCODE"
}

$dll = Join-Path $virtualCamera "x64/Release/VirtualCameraMediaSource.dll"
if (-not (Test-Path -LiteralPath $dll -PathType Leaf)) {
    throw "shared-buffer media-source DLL was not produced: $dll"
}
Write-Output "media_source_dll=$dll"
Write-Output "upstream_commit=$commit"
Write-Output "upstream_archive_sha256=$archiveHash"

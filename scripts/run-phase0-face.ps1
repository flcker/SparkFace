# SPDX-License-Identifier: Apache-2.0
[CmdletBinding()]
param(
    [int]$Warmup = 10,
    [int]$Frames = 100,
    [double]$MaxP95Ms = 8.0
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"
if ($Warmup -lt 0 -or $Frames -le 0 -or $MaxP95Ms -lt 0.0) {
    throw "Warmup, Frames, or MaxP95Ms is outside its valid range"
}

$repositoryRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot ".."))
$manifestPath = Join-Path $repositoryRoot "spikes\face\models\models.json"
$modelRoot = Join-Path $repositoryRoot "build\face-models"
$resultRoot = Join-Path $repositoryRoot "build\face-results"
$executable = Join-Path $repositoryRoot "build\windows-x64-release-face\spikes\face\face_spike.exe"

if (-not (Test-Path -LiteralPath $manifestPath -PathType Leaf)) {
    throw "Model manifest is missing: $manifestPath"
}
if (-not (Test-Path -LiteralPath $executable -PathType Leaf)) {
    throw "Face spike executable is missing. Run scripts/build-phase0-face.ps1 first."
}

$manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
$imagePath = Join-Path $modelRoot $manifest.overlay_test_still.ppm_filename
if (-not (Test-Path -LiteralPath $imagePath -PathType Leaf)) {
    throw "Cropped face PPM is missing. Run spikes/face/fetch-models.ps1 first."
}
New-Item -ItemType Directory -Force -Path $resultRoot | Out-Null

$invariant = [System.Globalization.CultureInfo]::InvariantCulture
function Join-InvariantFloats($Values) {
    return [string]::Join(",", @($Values | ForEach-Object {
        ([double]$_).ToString("R", $invariant)
    }))
}

$failed = New-Object System.Collections.Generic.List[string]
foreach ($model in $manifest.models) {
    $modelPath = Join-Path $modelRoot $model.filename
    if (-not (Test-Path -LiteralPath $modelPath -PathType Leaf)) {
        throw "Pinned model is missing: $modelPath"
    }

    $profile = $model.profile
    $inputProfile = $profile.input
    $overlayPath = Join-Path $resultRoot "$($model.id)-overlay.ppm"
    $logPath = Join-Path $resultRoot "$($model.id).log"
    $arguments = @(
        "--model", $modelPath,
        "--image", $imagePath,
        "--input-width", [string]$inputProfile.width,
        "--input-height", [string]$inputProfile.height,
        "--layout", [string]$inputProfile.layout,
        "--channel-order", [string]$inputProfile.channel_order,
        "--scale", (Join-InvariantFloats $inputProfile.scale),
        "--bias", (Join-InvariantFloats $inputProfile.bias),
        "--landmark-count", [string]$profile.landmark_count,
        "--dimensions", [string]$profile.dimensions,
        "--coordinate-mode", [string]$profile.coordinate_mode.Replace("_", "-"),
        "--input-index", [string]$profile.input_index,
        "--output-index", [string]$profile.landmark_output_index,
        "--warmup", [string]$Warmup,
        "--frames", [string]$Frames,
        "--max-p95-ms", $MaxP95Ms.ToString("R", $invariant),
        "--overlay", $overlayPath
    )

    Write-Host "candidate=$($model.id)"
    & $executable @arguments 2>&1 | Tee-Object -FilePath $logPath
    if ($LASTEXITCODE -ne 0) {
        $failed.Add("$($model.id): exit $LASTEXITCODE")
    }
}

if ($failed.Count -ne 0) {
    throw "One or more face candidates failed: $([string]::Join('; ', $failed))"
}
Write-Host "FACE_SPIKE_CANDIDATES_PASS"

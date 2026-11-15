# SPDX-License-Identifier: Apache-2.0
[CmdletBinding()]
param(
    [int]$Warmup = 100,
    [int]$Frames = 1000,
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

$model = @($manifest.models | Where-Object { $_.id -eq "mediapipe-face-mesh-468-f32-onnx" })
$detector = @($manifest.detectors | Where-Object { $_.id -eq "mediapipe-blazeface-short-range-f32-onnx" })
if ($model.Count -ne 1 -or $detector.Count -ne 1) {
    throw "Expected exactly one pinned 468-point landmark model and one BlazeFace detector"
}

$model = $model[0]
$detector = $detector[0]
$modelPath = Join-Path $modelRoot $model.filename
$detectorPath = Join-Path $modelRoot $detector.filename
if (-not (Test-Path -LiteralPath $modelPath -PathType Leaf) -or
    -not (Test-Path -LiteralPath $detectorPath -PathType Leaf)) {
    throw "Pinned face model or detector is missing. Run spikes/face/fetch-models.ps1 first."
}

$profile = $model.profile
$inputProfile = $profile.input
$commonArguments = @(
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
    "--max-p95-ms", $MaxP95Ms.ToString("R", $invariant)
)

function Invoke-FaceProbe([string]$Name, [object[]]$Arguments, [string]$LogPath) {
    Write-Host "probe=$Name"
    & $executable @Arguments 2>&1 | Tee-Object -FilePath $LogPath
    if ($LASTEXITCODE -ne 0) {
        throw "$Name failed with exit code $LASTEXITCODE"
    }
}

$fixedArguments = $commonArguments + @(
    "--overlay", (Join-Path $resultRoot "$($model.id)-fixed-n-overlay.ppm")
)
Invoke-FaceProbe -Name "fixed-n-landmark" -Arguments $fixedArguments `
    -LogPath (Join-Path $resultRoot "$($model.id)-fixed-n.log")

$pipelineArguments = @("--detector-model", $detectorPath) + $commonArguments + @(
    "--overlay", (Join-Path $resultRoot "blazeface-face-mesh-468-fixed-n-overlay.ppm")
)
Invoke-FaceProbe -Name "complete-face-stage" -Arguments $pipelineArguments `
    -LogPath (Join-Path $resultRoot "blazeface-face-mesh-468-fixed-n.log")

Write-Host "FACE_SPIKE_RETEST_PASS"

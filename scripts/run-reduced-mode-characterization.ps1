# SPDX-License-Identifier: Apache-2.0
[CmdletBinding()]
param([int]$Frames = 1000)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"
if ($Frames -le 0) {
    throw "Frames must be positive"
}

$repositoryRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot ".."))
$runner = Join-Path $PSScriptRoot "run-shared-buffer-handoff.ps1"
$resultRoot = Join-Path $repositoryRoot "build\transport-selection-results"
$resultPath = Join-Path $resultRoot "reduced-modes.json"
$culture = [Globalization.CultureInfo]::InvariantCulture
$modes = @(
    [PSCustomObject]@{ Id = "720p30"; Width = 1280; Height = 720; FrameRate = 30; MinimumFps = 29.5; MaximumP95Ms = 33.3 },
    [PSCustomObject]@{ Id = "1080p24"; Width = 1920; Height = 1080; FrameRate = 24; MinimumFps = 23.5; MaximumP95Ms = 41.667 },
    [PSCustomObject]@{ Id = "1080p15"; Width = 1920; Height = 1080; FrameRate = 15; MinimumFps = 14.5; MaximumP95Ms = 66.667 },
    [PSCustomObject]@{ Id = "720p24"; Width = 1280; Height = 720; FrameRate = 24; MinimumFps = 23.5; MaximumP95Ms = 41.667 }
)

function Read-Metrics([string]$Path) {
    $metrics = [ordered]@{}
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        return $metrics
    }
    foreach ($line in Get-Content -LiteralPath $Path) {
        if ($line -match '^([^=]+)=(.+)$') {
            $metrics[$Matches[1]] = $Matches[2]
        }
    }
    return $metrics
}

function Get-Int64($Metrics, [string]$Name) {
    if (-not $Metrics.Contains($Name)) { return $null }
    return [Int64]::Parse([string]$Metrics[$Name], $culture)
}

function Get-Double($Metrics, [string]$Name) {
    if (-not $Metrics.Contains($Name)) { return $null }
    return [double]::Parse([string]$Metrics[$Name], $culture)
}

New-Item -ItemType Directory -Force -Path $resultRoot | Out-Null
$results = [Collections.Generic.List[object]]::new()
foreach ($mode in $modes) {
    Write-Host "Running $($mode.Id): $($mode.Width)x$($mode.Height) at $($mode.FrameRate)fps"
    Get-Process FrameServer -ErrorAction SilentlyContinue | Stop-Process -Force
    $runError = $null
    try {
        & $runner -Mode SourceOwned -Frames $Frames -Width $mode.Width `
            -Height $mode.Height -FrameRate $mode.FrameRate `
            -MinimumFps $mode.MinimumFps -MaximumP95Ms $mode.MaximumP95Ms `
            -ResultName $mode.Id
    } catch {
        $runError = $_.Exception.Message
        Write-Warning "$($mode.Id) did not pass: $runError"
    }

    $modeRoot = Join-Path $resultRoot "reduced-modes\$($mode.Id)"
    $consumer = Read-Metrics (Join-Path $modeRoot "consumer.log")
    $producer = Read-Metrics (Join-Path $modeRoot "producer.log")
    $deliveredFrames = Get-Int64 $consumer "mf_frames"
    $achievedFps = Get-Double $consumer "mf_fps_achieved"
    $p50Ms = Get-Double $consumer "producer_to_client_frame_age_p50_ms"
    $p95Ms = Get-Double $consumer "producer_to_client_frame_age_p95_ms"
    $p99Ms = Get-Double $consumer "producer_to_client_frame_age_p99_ms"
    $maxMs = Get-Double $consumer "producer_to_client_frame_age_max_ms"
    $skipped = Get-Int64 $consumer "skipped_sequences"
    $duplicates = Get-Int64 $consumer "duplicate_sequences"
    $markerErrors = Get-Int64 $consumer "marker_errors"
    $copyBeforeFrameServer = Get-Int64 $consumer "full_frame_cpu_copy_before_frame_server"
    $noFreeSlots = Get-Int64 $producer "no_free_slot_count"

    $failures = [Collections.Generic.List[string]]::new()
    if ($deliveredFrames -ne $Frames -or $skipped -ne 0 -or
        $duplicates -ne 0 -or $markerErrors -ne 0) {
        $failures.Add("correctness")
    }
    if ($null -eq $achievedFps -or $achievedFps -lt $mode.MinimumFps) {
        $failures.Add("throughput")
    }
    if ($null -eq $p95Ms -or $p95Ms -gt $mode.MaximumP95Ms) {
        $failures.Add("latency")
    }
    if ($copyBeforeFrameServer -ne 0) {
        $failures.Add("pre_frame_server_copy")
    }
    if ($null -ne $runError -and $null -eq $deliveredFrames) {
        $failures.Add("execution")
    }

    $results.Add([ordered]@{
        id = $mode.Id
        format = "NV12"
        width = $mode.Width
        height = $mode.Height
        target_fps = $mode.FrameRate
        delivered_frames = $deliveredFrames
        minimum_fps = $mode.MinimumFps
        achieved_fps = $achievedFps
        maximum_frame_age_p95_ms = $mode.MaximumP95Ms
        producer_to_client_frame_age_p50_ms = $p50Ms
        producer_to_client_frame_age_p95_ms = $p95Ms
        producer_to_client_frame_age_p99_ms = $p99Ms
        producer_to_client_frame_age_max_ms = $maxMs
        skipped_sequences = $skipped
        duplicate_sequences = $duplicates
        marker_errors = $markerErrors
        full_frame_cpu_copy_before_frame_server = $copyBeforeFrameServer
        no_free_slot_count = $noFreeSlots
        outcome = if ($failures.Count -eq 0) { "PASS" } else { "NO-GO" }
        failed_gates = @($failures)
        execution_error = $runError
    })
}

$document = [ordered]@{
    schema_version = 1
    date = (Get-Date -Format "yyyy-MM-dd")
    ownership_mode = "source-owned"
    frames_per_mode = $Frames
    gate_policy = [ordered]@{
        minimum_fps = "target fps minus 0.5"
        maximum_frame_age_p95_ms = "one target-frame interval; 33.3ms retained for 30fps"
        correctness = "all requested frames delivered; zero skipped, duplicate, or malformed frames"
        pre_frame_server_copy = "zero full-frame CPU copies"
    }
    modes = @($results)
    verdict = if (@($results | Where-Object outcome -eq "PASS").Count -gt 0) {
        "At least one reduced product mode passed its mode-specific automated gate."
    } else {
        "No reduced product mode passed its mode-specific automated gate."
    }
}
[IO.File]::WriteAllText($resultPath, ($document | ConvertTo-Json -Depth 8) + "`n",
    [Text.UTF8Encoding]::new($false))
Write-Host "REDUCED_MODE_CHARACTERIZATION_COMPLETE result=$resultPath"

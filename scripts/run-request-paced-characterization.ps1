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
$resultPath = Join-Path $resultRoot "request-paced.json"
$culture = [Globalization.CultureInfo]::InvariantCulture
$modes = @(
    [PSCustomObject]@{ Id = "1080p30"; Width = 1920; Height = 1080; FrameRate = 30; MinimumFps = 29.5; MaximumP95Ms = 33.3 },
    [PSCustomObject]@{ Id = "720p30"; Width = 1280; Height = 720; FrameRate = 30; MinimumFps = 29.5; MaximumP95Ms = 33.3 },
    [PSCustomObject]@{ Id = "1080p24"; Width = 1920; Height = 1080; FrameRate = 24; MinimumFps = 23.5; MaximumP95Ms = 41.667 },
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

function Get-String($Metrics, [string]$Name) {
    if (-not $Metrics.Contains($Name)) { return $null }
    return [string]$Metrics[$Name]
}

New-Item -ItemType Directory -Force -Path $resultRoot | Out-Null
$results = [Collections.Generic.List[object]]::new()
foreach ($mode in $modes) {
    Write-Host "Running request-credit $($mode.Id): $($mode.Width)x$($mode.Height) at $($mode.FrameRate)fps"
    Get-Process FrameServer -ErrorAction SilentlyContinue | Stop-Process -Force
    $runError = $null
    try {
        & $runner -Mode SourceOwned -PacingMode RequestCredit -Frames $Frames `
            -Width $mode.Width -Height $mode.Height -FrameRate $mode.FrameRate `
            -MinimumFps $mode.MinimumFps -MaximumP95Ms $mode.MaximumP95Ms `
            -ResultName $mode.Id
    } catch {
        $runError = $_.Exception.Message
        Write-Warning "$($mode.Id) did not pass: $runError"
    }

    $modeRoot = Join-Path $resultRoot "request-paced\$($mode.Id)"
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
    $producerFrames = Get-Int64 $producer "producer_frames"
    $producerFps = Get-Double $producer "producer_fps"
    $pacingMode = Get-String $producer "producer_pacing_mode"
    $requested = Get-Int64 $producer "requested_sequence"
    $published = Get-Int64 $producer "published_sequence"
    $consumed = Get-Int64 $producer "consumed_sequence"
    $cancelledThrough = Get-Int64 $producer "cancelled_through_sequence"
    $creditSignals = Get-Int64 $producer "credit_signal_count"
    $cancelledCredits = Get-Int64 $producer "cancelled_credit_count"
    $creditWaits = Get-Int64 $producer "credit_wait_count"
    $requestToPublishAverageMs = Get-Double $producer "request_to_publish_average_ms"
    $requestToPublishMaxMs = Get-Double $producer "request_to_publish_max_ms"
    $noFreeSlots = Get-Int64 $producer "no_free_slot_count"
    $contiguousCopyTo = Get-Int64 $producer "contiguous_copy_to_calls"
    $contiguousCopyFrom = Get-Int64 $producer "contiguous_copy_from_calls"
    $copy2DTo = Get-Int64 $producer "copy2d_to_calls"
    $releasedBuffers = Get-Int64 $producer "released_buffer_count"

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
    if ($copyBeforeFrameServer -ne 0 -or $contiguousCopyTo -ne 0 -or
        $contiguousCopyFrom -ne 0 -or $copy2DTo -ne 0) {
        $failures.Add("pre_frame_server_copy")
    }
    $cancelledExpected = if ($null -ne $requested -and $null -ne $published) {
        $requested - $published
    } else {
        $null
    }
    $cancellationBoundaryValid =
        ($cancelledCredits -eq 0 -and $cancelledThrough -eq 0) -or
        ($cancelledCredits -gt 0 -and $cancelledThrough -eq $requested)
    if ($pacingMode -ne "request-credit" -or $producerFrames -ne $Frames -or
        $requested -lt $Frames -or $published -ne $Frames -or
        $consumed -ne $requested -or $creditSignals -ne $requested -or
        $cancelledCredits -ne $cancelledExpected -or
        -not $cancellationBoundaryValid -or $releasedBuffers -ne $published -or
        $noFreeSlots -ne 0) {
        $failures.Add("credit_protocol")
    }
    $expectedGateFailure =
        $deliveredFrames -eq $Frames -and $null -ne $runError -and
        $runError -match '^Shared-buffer consumer failed with exit code \d+:\s*$'
    $executionError = if ($null -ne $runError -and -not $expectedGateFailure) {
        $runError
    } else {
        $null
    }
    if ($null -ne $executionError) {
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
        producer_fps = $producerFps
        maximum_frame_age_p95_ms = $mode.MaximumP95Ms
        producer_to_client_frame_age_p50_ms = $p50Ms
        producer_to_client_frame_age_p95_ms = $p95Ms
        producer_to_client_frame_age_p99_ms = $p99Ms
        producer_to_client_frame_age_max_ms = $maxMs
        request_to_publish_average_ms = $requestToPublishAverageMs
        request_to_publish_max_ms = $requestToPublishMaxMs
        skipped_sequences = $skipped
        duplicate_sequences = $duplicates
        marker_errors = $markerErrors
        full_frame_cpu_copy_before_frame_server = $copyBeforeFrameServer
        requested_sequence = $requested
        published_sequence = $published
        consumed_sequence = $consumed
        credit_signal_count = $creditSignals
        credit_wait_count = $creditWaits
        cancelled_credit_count = $cancelledCredits
        cancelled_through_sequence = $cancelledThrough
        no_free_slot_count = $noFreeSlots
        contiguous_copy_to_calls = $contiguousCopyTo
        contiguous_copy_from_calls = $contiguousCopyFrom
        copy2d_to_calls = $copy2DTo
        released_buffer_count = $releasedBuffers
        outcome = if ($failures.Count -eq 0) { "PASS" } else { "NO-GO" }
        failed_gates = @($failures)
        execution_error = $executionError
    })
}

$fullRate = @($results | Where-Object id -eq "1080p30")[0]
$document = [ordered]@{
    schema_version = 1
    date = (Get-Date -Format "yyyy-MM-dd")
    hardware = "Windows 11 Pro build 26300; 13th Gen Intel Core i7-13700H; Intel Iris Xe Graphics; driver 32.0.101.7092"
    ownership_mode = "source-owned"
    pacing_mode = "request-credit"
    frames_per_mode = $Frames
    gate_policy = [ordered]@{
        minimum_fps = "target fps minus 0.5"
        maximum_frame_age_p95_ms = "one target-frame interval; 33.3ms retained for 30fps"
        correctness = "all target frames delivered; zero skipped, duplicate, or malformed frames"
        credit_protocol = "one publication or explicit cancellation per request; all published buffers released; zero slot pressure"
        pre_frame_server_copy = "zero full-frame CPU copies"
    }
    modes = @($results)
    verdict = if ($fullRate.outcome -eq "PASS") {
        "Request-credit pacing passed the unchanged NV12 1920x1080 at 30fps automated gate."
    } elseif (@($results | Where-Object outcome -eq "PASS").Count -gt 0) {
        "Request-credit pacing did not recover 1080p30; at least one reduced mode passed its mode-specific gate."
    } else {
        "Request-credit pacing did not produce an automated-gate survivor."
    }
}
[IO.File]::WriteAllText($resultPath, ($document | ConvertTo-Json -Depth 8) + "`n",
    [Text.UTF8Encoding]::new($false))
Write-Host "REQUEST_PACED_CHARACTERIZATION_COMPLETE result=$resultPath"

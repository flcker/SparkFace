# SPDX-License-Identifier: Apache-2.0
[CmdletBinding()]
param(
    [ValidateRange(1, 1000)]
    [int]$Frames = 1000,
    [ValidateRange(0.1, 120.0)]
    [double]$MinimumFps = 29.5,
    [ValidateRange(0.1, 120.0)]
    [double]$MaximumP95Ms = 33.3
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Assert-Administrator {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = [Security.Principal.WindowsPrincipal]::new($identity)
    if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
        throw "The physical-camera probe temporarily updates HKLM and requires elevated PowerShell"
    }
}

function Set-SourceRegistration([string]$Path) {
    $key = [Microsoft.Win32.Registry]::LocalMachine.OpenSubKey($serverSubkey, $true)
    if ($null -eq $key) {
        throw "Could not open the retained B2 COM registration for update"
    }
    try {
        $key.SetValue($null, $Path, [Microsoft.Win32.RegistryValueKind]::String)
    } finally {
        $key.Close()
    }
}

function Restart-FrameServer {
    $service = Get-Service FrameServer
    if ($service.Status -ne [System.ServiceProcess.ServiceControllerStatus]::Stopped) {
        Stop-Service FrameServer -Force
        $service.WaitForStatus(
            [System.ServiceProcess.ServiceControllerStatus]::Stopped,
            [TimeSpan]::FromSeconds(10))
    }
    Start-Sleep -Seconds 2
}

function Assert-CameraReady {
    for ($attempt = 0; $attempt -lt 120; ++$attempt) {
        $cameras = (& $registrar list 2>&1 | Out-String)
        if ($LASTEXITCODE -eq 0 -and
            $cameras -match "SparkFace Phase0 B2 Camera") {
            return
        }
        Start-Sleep -Milliseconds 500
    }
    throw "Retained B2 camera did not become available"
}

function Start-ControlProducer([string]$CameraName) {
    $stdout = Join-Path $resultRoot "producer.log"
    $stderr = Join-Path $resultRoot "producer.err.log"
    Remove-Item -Force -ErrorAction SilentlyContinue $stdout, $stderr
    $arguments = "--mode hold --revision 201 --value 35 --heartbeat-ms 100 " +
        "--physical-camera --camera-name `"$CameraName`""
    $process = Start-Process -FilePath $producer -NoNewWindow -PassThru `
        -ArgumentList $arguments -RedirectStandardOutput $stdout `
        -RedirectStandardError $stderr
    Start-Sleep -Milliseconds 750
    if ($process.HasExited) {
        $errorOutput = Get-Content -LiteralPath $stderr -Raw -ErrorAction SilentlyContinue
        throw "Physical-camera control producer exited: $errorOutput"
    }
    $output = Get-Content -LiteralPath $stdout -Raw -ErrorAction SilentlyContinue
    foreach ($line in @(
        "control_publish_revision=201",
        "control_physical_camera=1",
        "control_protocol_version=2"
    )) {
        if ($output -notmatch "(?m)^$([regex]::Escape($line))`r?$") {
            throw "Physical-camera control producer did not prove $line"
        }
    }
    return $process
}

function Stop-ControlProducer($Process) {
    if ($null -ne $Process -and -not $Process.HasExited) {
        Stop-Process -Id $Process.Id -Force
        $Process.WaitForExit()
    }
}

function Invoke-PhysicalConsumer([string]$Name, [int]$FrameCount) {
    $stdout = Join-Path $resultRoot "$Name.consumer.log"
    $stderr = Join-Path $resultRoot "$Name.consumer.err.log"
    Remove-Item -Force -ErrorAction SilentlyContinue $stdout, $stderr
    $arguments = "--frames $FrameCount --width 1920 --height 1080 --frame-rate 30 " +
        "--minimum-fps $MinimumFps --maximum-p95-ms $MaximumP95Ms " +
        "--warmup-frames 0 --expect-control-revision 201 --expect-control-value 35 " +
        "--expect-control-status 0 --expect-physical-camera"
    $process = Start-Process -FilePath $consumer -NoNewWindow -PassThru `
        -ArgumentList $arguments -RedirectStandardOutput $stdout `
        -RedirectStandardError $stderr
    $timeout = [Math]::Max(150000, [int](($FrameCount / 10.0 + 30.0) * 1000.0))
    if (-not $process.WaitForExit($timeout)) {
        Stop-Process -Id $process.Id -Force
        $process.WaitForExit()
        throw "Physical-camera gate watchdog expired"
    }
    $process.WaitForExit()
    $output = Get-Content -LiteralPath $stdout -Raw -ErrorAction SilentlyContinue
    $errorOutput = Get-Content -LiteralPath $stderr -Raw -ErrorAction SilentlyContinue
    if (-not [string]::IsNullOrWhiteSpace($errorOutput)) {
        throw "Physical-camera gate reported stderr: stderr=$errorOutput stdout=$output"
    }
    foreach ($pattern in @(
        "(?m)^source_local_frames=$FrameCount`r?$",
        "(?m)^source_local_gpu_conversion_count=$FrameCount`r?$",
        "(?m)^skipped_sequences=0`r?$",
        "(?m)^duplicate_sequences=0`r?$",
        "(?m)^marker_errors=0`r?$",
        "(?m)^payload_marker_errors=0`r?$",
        "(?m)^control_marker_errors=0`r?$",
        "(?m)^order_marker_errors=0`r?$",
        "(?m)^physical_content_errors=0`r?$",
        "(?m)^physical_luma_range_min=[1-9][0-9]*`r?$",
        "(?m)^full_frame_cpu_copy_before_frame_server=0`r?$",
        "(?m)^SOURCE_LOCAL_GATE_FAIL`r?$"
    )) {
        if ($output -notmatch $pattern) {
            throw "Physical-camera gate did not match $pattern`: $output"
        }
    }
    if ($output -notmatch "(?m)^source_local_fps_achieved=([0-9.]+)`r?$" -or
        $output -notmatch "(?m)^source_local_capture_to_client_p95_ms=([0-9.]+)`r?$") {
        throw "Physical-camera gate omitted throughput or latency metrics: $output"
    }
    $fpsMatch = [regex]::Match($output, "(?m)^source_local_fps_achieved=([0-9.]+)`r?$")
    $p95Match = [regex]::Match($output, "(?m)^source_local_capture_to_client_p95_ms=([0-9.]+)`r?$")
    $culture = [Globalization.CultureInfo]::InvariantCulture
    $fps = [double]::Parse($fpsMatch.Groups[1].Value, $culture)
    $p95 = [double]::Parse($p95Match.Groups[1].Value, $culture)
    $failedGates = @()
    if ($fps -lt $MinimumFps) { $failedGates += "throughput" }
    if ($p95 -gt $MaximumP95Ms) { $failedGates += "latency" }
    if ($failedGates.Count -eq 0) {
        throw "Physical-camera strict gate reported failure without failing throughput or latency: $output"
    }
    $reason = ($failedGates -join "-") + "-gate"
    Add-Content -LiteralPath $stateLog -Value `
        "scenario=$Name frames=$FrameCount result=no-go reason=$reason fps=$fps p95_ms=$p95"
    Write-Output "physical_camera_no_go_reason=$reason"
}

Assert-Administrator
$repositoryRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot ".."))
$buildRoot = Join-Path $repositoryRoot `
    "build\windows-x64-release-transport-selection\spikes\transport-selection"
$consumer = Join-Path $buildRoot "transport_source_local_consumer.exe"
$producer = Join-Path $buildRoot "transport_source_local_control_producer.exe"
$registrar = Join-Path $buildRoot "transport_source_adapter_registrar.exe"
$mediaSource = Join-Path $repositoryRoot `
    "build\tsb\Windows-Camera-626f8b19c5f367602f2e89c6b314573d3776c9df\Samples\VirtualCamera\x64\Release\VirtualCameraMediaSource.dll"
$resultRoot = Join-Path $repositoryRoot `
    "build\transport-selection-results\source-local-physical-camera"
$stageRoot = Join-Path $resultRoot "staged"
$stateLog = Join-Path $resultRoot "scenarios.log"
$sourceTrace = "C:\Users\Public\SparkFaceSourceLocalTrace.log"
$sourceLocalMode = "C:\Users\Public\SparkFaceTransportSourceLocal.flag"
$h9Mode = "C:\Users\Public\SparkFaceTransportSourceLocalH9.flag"
$videoModeFile = "C:\Users\Public\SparkFaceTransportVideoMode.bin"
$conflictingFiles = @(
    "C:\Users\Public\SparkFaceTransportSelectionSourceOwned.flag",
    "C:\Users\Public\SparkFaceTransportCompressed.mode",
    "C:\Users\Public\SparkFaceTransportCompressed.frame",
    "C:\Users\Public\SparkFaceTransportRequestCredit.flag"
)
foreach ($path in @($consumer, $producer, $registrar, $mediaSource)) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Required physical-camera artifact is missing: $path"
    }
}
New-Item -ItemType Directory -Force -Path $resultRoot | Out-Null
Remove-Item -Force -ErrorAction SilentlyContinue $stateLog, $sourceTrace

$cameraList = & $producer --mode list-cameras 2>&1 | Out-String
if ($LASTEXITCODE -ne 0 -or
    $cameraList -notmatch "(?m)^physical_camera_count=1`r?$" -or
    $cameraList -notmatch "(?m)^physical_camera_0_name=Integrated Camera`r?$") {
    throw "H8A requires the single expected Integrated Camera. Observed:`n$cameraList"
}
$cameraName = "Integrated Camera"

$sourceClsid = "{A1D6A9E3-6D95-4DA8-AB2E-987B5322F861}"
$classSubkey = "Software\Classes\CLSID\$sourceClsid"
$serverSubkey = "$classSubkey\InprocServer32"
$readKey = [Microsoft.Win32.Registry]::LocalMachine.OpenSubKey($serverSubkey, $false)
if ($null -eq $readKey) {
    throw "Retained B2 in-process registration is missing"
}
$previousMediaSource = [string]$readKey.GetValue($null)
$readKey.Close()
if ([string]::IsNullOrWhiteSpace($previousMediaSource) -or
    -not (Test-Path -LiteralPath $previousMediaSource -PathType Leaf)) {
    throw "Retained B2 media-source path is invalid: $previousMediaSource"
}

$producerProcess = $null
$registrationChanged = $false
$runSucceeded = $false
try {
    Remove-Item -Force -ErrorAction SilentlyContinue $sourceLocalMode, $h9Mode,
        $videoModeFile
    Remove-Item -Force -ErrorAction SilentlyContinue $conflictingFiles
    Remove-Item -Recurse -Force -ErrorAction SilentlyContinue $stageRoot
    New-Item -ItemType Directory -Force -Path $stageRoot | Out-Null
    $stagedDll = Join-Path $stageRoot "VirtualCameraMediaSource.dll"
    Copy-Item -LiteralPath $mediaSource -Destination $stagedDll
    $fixture = [Text.Encoding]::ASCII.GetBytes("SparkFace source-local H9 fixture`n")
    [IO.File]::WriteAllBytes(
        (Join-Path $stageRoot "SparkFaceSourceLocal.fixture"), $fixture)

    $modeBytes = [byte[]]::new(12)
    [BitConverter]::GetBytes([uint32]1920).CopyTo($modeBytes, 0)
    [BitConverter]::GetBytes([uint32]1080).CopyTo($modeBytes, 4)
    [BitConverter]::GetBytes([uint32]30).CopyTo($modeBytes, 8)
    [IO.File]::WriteAllBytes($videoModeFile, $modeBytes)
    Set-Content -LiteralPath $sourceLocalMode -Value "source-local" -NoNewline
    Set-Content -LiteralPath $h9Mode -Value "h8a" -NoNewline

    $producerProcess = Start-ControlProducer $cameraName
    Set-SourceRegistration $stagedDll
    $registrationChanged = $true
    Restart-FrameServer
    Assert-CameraReady

    if ($Frames -lt 2) {
        throw "H8A gate requires at least two requested frames"
    }
    Invoke-PhysicalConsumer "recursive-physical-capture" $Frames

    if (-not (Test-Path -LiteralPath $sourceTrace -PathType Leaf)) {
        throw "Physical-camera source trace was not produced"
    }
    $trace = Get-Content -LiteralPath $sourceTrace -Raw
    foreach ($pattern in @(
        "(?m)^physical_camera_initialize_hresult=0x00000000`r?$",
        "(?m)^physical_camera_protocol=2 symbolic_link_hash=[0-9a-fA-F]{8} adapter_luid=[0-9a-fA-F]{8}:[0-9a-fA-F]{8}`r?$",
        "(?m)^source_local_gpu_conversion_count=$Frames`r?$"
    )) {
        if ($trace -notmatch $pattern) {
            throw "Physical-camera source trace did not match $pattern"
        }
    }
    Copy-Item -LiteralPath $sourceTrace `
        -Destination (Join-Path $resultRoot "source-trace.log") -Force
    Add-Content -LiteralPath $stateLog -Value "source_local_physical_camera=no-go"
    $runSucceeded = $true
} finally {
    if (Test-Path -LiteralPath $sourceTrace -PathType Leaf) {
        Copy-Item -LiteralPath $sourceTrace `
            -Destination (Join-Path $resultRoot "source-trace.log") -Force
    }
    Stop-ControlProducer $producerProcess
    $restoreError = $null
    if ($registrationChanged) {
        try {
            Set-SourceRegistration $previousMediaSource
        } catch {
            $restoreError = $_.Exception.Message
        }
    }
    Restart-FrameServer
    Remove-Item -Force -ErrorAction SilentlyContinue $sourceTrace,
        $sourceLocalMode, $h9Mode, $videoModeFile
    Remove-Item -Force -ErrorAction SilentlyContinue $conflictingFiles
    for ($attempt = 0; $attempt -lt 50; ++$attempt) {
        Remove-Item -Recurse -Force -ErrorAction SilentlyContinue $stageRoot
        if (-not (Test-Path -LiteralPath $stageRoot)) { break }
        Start-Sleep -Milliseconds 200
    }
    foreach ($path in @($sourceTrace, $sourceLocalMode, $h9Mode,
                          $videoModeFile, $stageRoot)) {
        if (Test-Path -LiteralPath $path) {
            throw "H8A cleanup left artifact: $path"
        }
    }
    if ($null -ne $restoreError) {
        throw "Could not restore the retained B2 COM registration: $restoreError"
    }
}
if ($runSucceeded) {
    Write-Host "SOURCE_LOCAL_PHYSICAL_CAMERA_NO_GO"
}

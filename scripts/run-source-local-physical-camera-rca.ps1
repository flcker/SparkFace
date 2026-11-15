# SPDX-License-Identifier: Apache-2.0
[CmdletBinding()]
param(
    [ValidateRange(1, 10000)]
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
        throw "The H8A RCA runner updates HKLM and restarts Frame Server; use elevated PowerShell"
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
        if ($LASTEXITCODE -eq 0 -and $cameras -match "SparkFace Phase0 B2 Camera") {
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
    $arguments = "--mode hold --revision 301 --value 35 --heartbeat-ms 100 " +
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
        "control_publish_revision=301",
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

function Invoke-ProcessWithWatchdog(
    [string]$Path,
    [string]$Arguments,
    [string]$Stdout,
    [string]$Stderr,
    [int]$TimeoutMilliseconds
) {
    Remove-Item -Force -ErrorAction SilentlyContinue $Stdout, $Stderr
    $process = Start-Process -FilePath $Path -NoNewWindow -PassThru `
        -ArgumentList $Arguments -RedirectStandardOutput $Stdout `
        -RedirectStandardError $Stderr
    if (-not $process.WaitForExit($TimeoutMilliseconds)) {
        Stop-Process -Id $process.Id -Force
        $process.WaitForExit()
        throw "Process watchdog expired: $Path $Arguments"
    } else {
        $process.WaitForExit()
    }
    $errorOutput = Get-Content -LiteralPath $Stderr -Raw -ErrorAction SilentlyContinue
    if (-not [string]::IsNullOrWhiteSpace($errorOutput)) {
        $output = Get-Content -LiteralPath $Stdout -Raw -ErrorAction SilentlyContinue
        throw "Process reported stderr: stdout=$output stderr=$errorOutput"
    }
}

function Invoke-Isolation {
    $stdout = Join-Path $resultRoot "standalone.consumer.log"
    $stderr = Join-Path $resultRoot "standalone.consumer.err.log"
    Remove-Item -Force -ErrorAction SilentlyContinue $sourceTrace
    $timeout = [Math]::Max(75000, [int](($Frames / 20.0 + 30.0) * 1000.0))
    Invoke-ProcessWithWatchdog $isolation `
        "--frames $Frames --camera-name `"Integrated Camera`"" `
        $stdout $stderr $timeout
    $output = Get-Content -LiteralPath $stdout -Raw
    if ($output -notmatch "(?m)^PHYSICAL_CAMERA_ISOLATION_PASS`r?$") {
        throw "Standalone isolation did not pass: $output"
    }
    if (-not (Test-Path -LiteralPath $sourceTrace -PathType Leaf)) {
        throw "Standalone isolation did not produce a source trace"
    }
    Copy-Item -LiteralPath $sourceTrace `
        -Destination (Join-Path $resultRoot "standalone.source-trace.log") -Force
    Add-Content -LiteralPath $stateLog -Value `
        "scenario=standalone frames=$Frames result=pass"
}


function Invoke-SourceStage([string]$Mode, [bool]$StrictTiming) {
    $stdout = Join-Path $resultRoot "$Mode.consumer.log"
    $stderr = Join-Path $resultRoot "$Mode.consumer.err.log"
    $traceCopy = Join-Path $resultRoot "$Mode.source-trace.log"
    $eventLog = Join-Path $resultRoot "$Mode.events.log"
    Remove-Item -Force -ErrorAction SilentlyContinue $stdout, $stderr, $traceCopy,
        $eventLog, $sourceTrace
    Set-Content -LiteralPath $sourceRcaMode -Value $Mode -NoNewline
    Restart-FrameServer
    Assert-CameraReady

    $stageStarted = Get-Date
    $stageError = $null
    try {
        $effectiveMinimumFps = if ($StrictTiming) { $MinimumFps } else { 1.0 }
        $effectiveMaximumP95Ms = if ($StrictTiming) {
            $MaximumP95Ms
        } else {
            1000000000000.0
        }
        $arguments = "--frames $Frames --width 1920 --height 1080 --frame-rate 30 " +
            "--minimum-fps $effectiveMinimumFps " +
            "--maximum-p95-ms $effectiveMaximumP95Ms " +
            "--warmup-frames 0 --expect-control-revision 301 " +
            "--expect-control-value 35 --expect-control-status 0 " +
            "--expect-physical-camera"
        $timeout = [Math]::Max(150000, [int](($Frames / 10.0 + 30.0) * 1000.0))
        Invoke-ProcessWithWatchdog $consumer $arguments $stdout $stderr $timeout
    } catch {
        $stageError = $_
    }
    $frameServerEvents = Get-WinEvent -FilterHashtable @{
        LogName = "Application"
        StartTime = $stageStarted
        Level = 1, 2, 3
    } -ErrorAction SilentlyContinue | Where-Object {
        $_.ProviderName -eq "Application Error" -and
        $_.Message -match "svchost.exe_FrameServer"
    }
    $frameServerEvents | Select-Object TimeCreated, Id, ProviderName, Message |
        Format-List | Out-File -LiteralPath $eventLog -Encoding utf8
    if (@($frameServerEvents).Count -ne 0) {
        throw "Source-local $Mode crashed Frame Server; see $eventLog"
    }
    if ($null -ne $stageError) {
        throw $stageError
    }

    $output = Get-Content -LiteralPath $stdout -Raw
    foreach ($pattern in @(
        "(?m)^source_local_gpu_conversion_count=$Frames`r?$",
        "(?m)^skipped_sequences=0`r?$",
        "(?m)^duplicate_sequences=0`r?$",
        "(?m)^payload_marker_errors=0`r?$",
        "(?m)^order_marker_errors=0`r?$",
        "(?m)^physical_content_errors=0`r?$",
        "(?m)^full_frame_cpu_copy_before_frame_server=0`r?$",
        "(?m)^SOURCE_LOCAL_GATE_(PASS|FAIL)`r?$"
    )) {
        if ($output -notmatch $pattern) {
            throw "Source-local $Mode consumer did not match $pattern`: $output"
        }
    }
    if (-not (Test-Path -LiteralPath $sourceTrace -PathType Leaf)) {
        throw "Source-local $Mode stage did not produce a source trace"
    }
    $trace = Get-Content -LiteralPath $sourceTrace -Raw
    $requiredPatterns = @(
        "(?m)^source_local_diagnostic_mode=$([regex]::Escape($Mode))`r?$",
        "(?m)^physical_camera_initialize_hresult=0x00000000`r?$",
        "(?m)^source_local_multithread_protected=1`r?$",
        "(?m)^physical_camera_read sequence=[1-9][0-9]* .+ hresult=0x00000000`r?$",
        "(?m)^source_local_stage sequence=[1-9][0-9]* mode=$([regex]::Escape($Mode)) stage=capture hresult=0x00000000 removed_reason=0x00000000`r?$"
    )
    if ($Mode -eq "capture-only") {
        $requiredPatterns += "(?m)stage=capture-release hresult=0x00000000 removed_reason=0x00000000`r?$"
    } else {
        $requiredPatterns += "(?m)stage=copy-complete hresult=0x00000000 removed_reason=0x00000000`r?$"
        $requiredPatterns += "(?m)stage=sample-release hresult=0x00000000 removed_reason=0x00000000`r?$"
    }
    foreach ($pattern in $requiredPatterns) {
        if ($trace -notmatch $pattern) {
            throw "Source-local $Mode trace did not match $pattern"
        }
    }
    $successfulReads = ([regex]::Matches(
        $trace,
        "(?m)^physical_camera_read sequence=[1-9][0-9]* .+ hresult=0x00000000`r?$")).Count
    if ($Mode -ne "full" -and $successfulReads -lt $Frames) {
        throw "Source-local $Mode captured only $successfulReads of $Frames frames"
    }
    if ($Mode -eq "full" -and
        $trace -notmatch "(?m)^physical_camera_read sequence=$Frames .+ hresult=0x00000000`r?$") {
        throw "Source-local full trace did not reach frame $Frames"
    }
    if ($trace -match "removed_reason=0x(?!00000000)[0-9a-fA-F]{8}") {
        throw "Source-local $Mode observed device removal"
    }
    Copy-Item -LiteralPath $sourceTrace -Destination $traceCopy -Force
    $gate = if ($output -match "(?m)^SOURCE_LOCAL_GATE_PASS`r?$") {
        "pass"
    } else {
        "fail"
    }
    Add-Content -LiteralPath $stateLog -Value `
        "scenario=$Mode frames=$Frames result=pass delivered_frames=$Frames trace_successful_reads=$successfulReads strict_gate=$gate"
}

Assert-Administrator
$repositoryRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot ".."))
$buildRoot = Join-Path $repositoryRoot `
    "build\windows-x64-release-transport-selection\spikes\transport-selection"
$consumer = Join-Path $buildRoot "transport_source_local_consumer.exe"
$producer = Join-Path $buildRoot "transport_source_local_control_producer.exe"
$isolation = Join-Path $buildRoot "transport_physical_camera_isolation.exe"
$registrar = Join-Path $buildRoot "transport_source_adapter_registrar.exe"
$mediaSource = Join-Path $repositoryRoot `
    "build\tsb\Windows-Camera-626f8b19c5f367602f2e89c6b314573d3776c9df\Samples\VirtualCamera\x64\Release\VirtualCameraMediaSource.dll"
$resultRoot = Join-Path $repositoryRoot `
    "build\transport-selection-results\source-local-physical-camera-rca"
$stageRoot = Join-Path $resultRoot "staged"
$stateLog = Join-Path $resultRoot "scenarios.log"
$sourceTrace = "C:\Users\Public\SparkFaceSourceLocalTrace.log"
$sourceLocalMode = "C:\Users\Public\SparkFaceTransportSourceLocal.flag"
$h9Mode = "C:\Users\Public\SparkFaceTransportSourceLocalH9.flag"
$sourceRcaMode = "C:\Users\Public\SparkFaceSourceLocalRca.mode"
$videoModeFile = "C:\Users\Public\SparkFaceTransportVideoMode.bin"
$conflictingFiles = @(
    "C:\Users\Public\SparkFaceTransportSelectionSourceOwned.flag",
    "C:\Users\Public\SparkFaceTransportCompressed.mode",
    "C:\Users\Public\SparkFaceTransportCompressed.frame",
    "C:\Users\Public\SparkFaceTransportRequestCredit.flag"
)
foreach ($path in @($consumer, $producer, $isolation, $registrar, $mediaSource)) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Required H8A RCA artifact is missing: $path"
    }
}
New-Item -ItemType Directory -Force -Path $resultRoot | Out-Null
Remove-Item -Force -ErrorAction SilentlyContinue $stateLog, $sourceTrace

$cameraList = & $producer --mode list-cameras 2>&1 | Out-String
if ($LASTEXITCODE -ne 0 -or
    $cameraList -notmatch "(?m)^physical_camera_count=1`r?$" -or
    $cameraList -notmatch "(?m)^physical_camera_0_name=Integrated Camera`r?$") {
    throw "H8A RCA requires the single expected Integrated Camera. Observed:`n$cameraList"
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
        $sourceRcaMode, $videoModeFile
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
    Set-Content -LiteralPath $h9Mode -Value "h8a-rca" -NoNewline

    Invoke-Isolation
    $producerProcess = Start-ControlProducer $cameraName
    Set-SourceRegistration $stagedDll
    $registrationChanged = $true

    Invoke-SourceStage "capture-only" $false
    Invoke-SourceStage "copy-only" $false
    Invoke-SourceStage "full" $true
    Add-Content -LiteralPath $stateLog -Value "source_local_physical_camera_rca=pass"
    $runSucceeded = $true
} finally {
    if (Test-Path -LiteralPath $sourceTrace -PathType Leaf) {
        Copy-Item -LiteralPath $sourceTrace `
            -Destination (Join-Path $resultRoot "last.source-trace.log") -Force
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
        $sourceLocalMode, $h9Mode, $sourceRcaMode, $videoModeFile
    Remove-Item -Force -ErrorAction SilentlyContinue $conflictingFiles
    for ($attempt = 0; $attempt -lt 50; ++$attempt) {
        Remove-Item -Recurse -Force -ErrorAction SilentlyContinue $stageRoot
        if (-not (Test-Path -LiteralPath $stageRoot)) { break }
        Start-Sleep -Milliseconds 200
    }
    foreach ($path in @($sourceTrace, $sourceLocalMode, $h9Mode,
                          $sourceRcaMode, $videoModeFile, $stageRoot)) {
        if (Test-Path -LiteralPath $path) {
            throw "H8A RCA cleanup left artifact: $path"
        }
    }
    if ($null -ne $restoreError) {
        throw "Could not restore the retained B2 COM registration: $restoreError"
    }
}
if ($runSucceeded) {
    Write-Host "SOURCE_LOCAL_PHYSICAL_CAMERA_RCA_PASS"
}

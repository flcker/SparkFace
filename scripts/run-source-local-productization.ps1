# SPDX-License-Identifier: Apache-2.0
[CmdletBinding()]
param(
    [ValidateRange(1, 1000)]
    [int]$Frames = 120,
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
        throw "The source-local productization probe temporarily updates HKLM and requires elevated PowerShell"
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

function Stop-FrameServer {
    $service = Get-Service FrameServer
    if ($service.Status -ne [System.ServiceProcess.ServiceControllerStatus]::Stopped) {
        Stop-Service FrameServer -Force
        $service.WaitForStatus(
            [System.ServiceProcess.ServiceControllerStatus]::Stopped,
            [TimeSpan]::FromSeconds(10))
    }
}

function Restart-FrameServer {
    Stop-FrameServer
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

function Start-Producer([string]$Name, [string]$Mode, [int]$Revision,
                        [int]$Value) {
    $stdout = Join-Path $resultRoot "$Name.producer.log"
    $stderr = Join-Path $resultRoot "$Name.producer.err.log"
    Remove-Item -Force -ErrorAction SilentlyContinue $stdout, $stderr
    $process = Start-Process -FilePath $producer -NoNewWindow -PassThru `
        -ArgumentList "--mode $Mode --revision $Revision --value $Value --heartbeat-ms 100" `
        -RedirectStandardOutput $stdout -RedirectStandardError $stderr
    Start-Sleep -Milliseconds 500
    if ($process.HasExited) {
        throw "Producer $Name exited before publication"
    }
    $log = Get-Content -LiteralPath $stdout -Raw -ErrorAction SilentlyContinue
    if ($log -notmatch "(?m)^control_publish_revision=$Revision`r?$") {
        throw "Producer $Name did not publish revision $Revision"
    }
    return $process
}

function Stop-Producer($Process) {
    if ($null -ne $Process -and -not $Process.HasExited) {
        Stop-Process -Id $Process.Id -Force
        $Process.WaitForExit()
    }
}

function Invoke-Consumer([string]$Name, [int]$Revision, [int]$Value,
                         [int]$Status) {
    $log = Join-Path $resultRoot "$Name.consumer.log"
    $errorLog = Join-Path $resultRoot "$Name.consumer.err.log"
    $arguments = @(
        "--frames", $Frames, "--minimum-fps", $MinimumFps,
        "--maximum-p95-ms", $MaximumP95Ms,
        "--expect-control-revision", $Revision,
        "--expect-control-value", $Value,
        "--expect-control-status", $Status
    )
    $output = & $consumer @arguments 2>&1 | Out-String
    $exitCode = $LASTEXITCODE
    Set-Content -LiteralPath $log -Value $output -NoNewline
    if ($exitCode -ne 0) {
        Set-Content -LiteralPath $errorLog -Value "exit_code=$exitCode`n$output" -NoNewline
        throw "Consumer $Name failed with exit code $exitCode"
    }
    foreach ($line in @(
        "source_local_control_revision=$Revision",
        "source_local_control_value=$Value",
        "source_local_control_status=$Status",
        "SOURCE_LOCAL_GATE_PASS"
    )) {
        if ($output -notmatch "(?m)^$([regex]::Escape($line))`r?$") {
            throw "Consumer $Name did not prove $line"
        }
    }
    if (-not (Test-Path -LiteralPath $sourceTrace -PathType Leaf)) {
        throw "Consumer $Name did not produce a source-local trace"
    }
    $trace = Get-Content -LiteralPath $sourceTrace -Raw
    $traceLine = "source_local_h9_status=$Status revision=$Revision value=$Value"
    if ($trace -notmatch "(?m)^$([regex]::Escape($traceLine))`r?$") {
        throw "Consumer $Name did not prove source application in the renderer trace"
    }

    Add-Content -LiteralPath $stateLog -Value "scenario=$Name result=pass"
}

function Invoke-CrashTransition($Process, [int]$Revision, [int]$Value) {
    $name = "producer-crash-transition"
    $log = Join-Path $resultRoot "$name.consumer.log"
    $errorLog = Join-Path $resultRoot "$name.consumer.err.log"
    $appliedFrames = [Math]::Min(30, $Frames - 1)
    $arguments = @(
        "--frames", $Frames, "--minimum-fps", $MinimumFps,
        "--maximum-p95-ms", $MaximumP95Ms,
        "--expect-control-revision", $Revision,
        "--expect-control-value", $Value,
        "--expect-control-status", 0,
        "--expect-control-dead-after", $appliedFrames
    )
    Remove-Item -Force -ErrorAction SilentlyContinue $log, $errorLog
    $capture = Start-Process -FilePath $consumer -NoNewWindow -PassThru `
        -ArgumentList $arguments -RedirectStandardOutput $log `
        -RedirectStandardError $errorLog
    $ready = $false
    for ($attempt = 0; $attempt -lt 240; ++$attempt) {
        if ($capture.HasExited) { break }
        if ((Get-Content -LiteralPath $log -Raw -ErrorAction SilentlyContinue) -match
            "(?m)^control_transition_ready=1`r?$") {
            $ready = $true
            break
        }
        Start-Sleep -Milliseconds 100
    }
    if (-not $ready) {
        if (-not $capture.HasExited) { Stop-Process -Id $capture.Id -Force }
        throw "Crash transition consumer did not reach applied control frames"
    }
    Stop-Producer $Process
    if (-not $capture.WaitForExit(30000)) {
        Stop-Process -Id $capture.Id -Force
        throw "Crash transition consumer did not finish"
    }
    $output = Get-Content -LiteralPath $log -Raw
    if ($capture.ExitCode -ne 0 -or
        $output -notmatch "(?m)^source_local_control_transition_applied=1`r?$" -or
        $output -notmatch "(?m)^source_local_control_transition_dead=1`r?$" -or
        $output -notmatch "(?m)^source_local_control_status=7`r?$" -or
        $output -notmatch "(?m)^marker_errors=0`r?$" -or
        $output -notmatch "(?m)^SOURCE_LOCAL_GATE_PASS`r?$") {
        throw "Crash transition consumer did not prove Applied-to-ProducerDead"
    }
    $trace = Get-Content -LiteralPath $sourceTrace -Raw
    if ($trace -notmatch "(?m)^source_local_h9_status=7 revision=0 value=0`r?$") {
        throw "Crash transition did not prove ProducerDead in the renderer trace"
    }
    Add-Content -LiteralPath $stateLog -Value "scenario=producer-crash-transition result=pass"
}

function Invoke-AclProbe {
    $log = Join-Path $resultRoot "acl-probe.log"
    $output = & $producer --mode acl-probe 2>&1 | Out-String
    $exitCode = $LASTEXITCODE
    Set-Content -LiteralPath $log -Value $output -NoNewline
    if ($exitCode -ne 0 -or
        $output -notmatch "(?m)^control_acl_restricted_write_denied=1`r?$" -or
        $output -notmatch "(?m)^control_acl_low_integrity_write_denied=1`r?$" -or
        $output -notmatch "(?m)^control_acl_non_admin=1`r?$" -or
        $output -notmatch "(?m)^control_acl_non_admin_write_open=1`r?$") {
        throw "ACL probe did not prove restricted denial and non-admin access"
    }
    Add-Content -LiteralPath $stateLog -Value "scenario=acl-denial result=pass"
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
    "build\transport-selection-results\source-local-productization"
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
        throw "Required productization artifact is missing: $path"
    }
}
New-Item -ItemType Directory -Force -Path $resultRoot | Out-Null
Remove-Item -Force -ErrorAction SilentlyContinue $stateLog

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
    Remove-Item -Force -ErrorAction SilentlyContinue $sourceTrace, $sourceLocalMode,
        $h9Mode, $videoModeFile
    Remove-Item -Force -ErrorAction SilentlyContinue $conflictingFiles
    Remove-Item -Recurse -Force -ErrorAction SilentlyContinue $stageRoot
    New-Item -ItemType Directory -Force -Path $stageRoot | Out-Null
    $fixture = [Text.Encoding]::ASCII.GetBytes("SparkFace source-local H9 fixture`n")
    foreach ($version in @("v1", "v2")) {
        $directory = Join-Path $stageRoot $version
        New-Item -ItemType Directory -Force -Path $directory | Out-Null
        Copy-Item -LiteralPath $mediaSource -Destination (Join-Path $directory "VirtualCameraMediaSource.dll")
        [IO.File]::WriteAllBytes((Join-Path $directory "SparkFaceSourceLocal.fixture"), $fixture)
    }
    $modeBytes = [byte[]]::new(12)
    [BitConverter]::GetBytes([uint32]1920).CopyTo($modeBytes, 0)
    [BitConverter]::GetBytes([uint32]1080).CopyTo($modeBytes, 4)
    [BitConverter]::GetBytes([uint32]30).CopyTo($modeBytes, 8)
    [IO.File]::WriteAllBytes($videoModeFile, $modeBytes)
    Set-Content -LiteralPath $sourceLocalMode -Value "source-local" -NoNewline
    Set-Content -LiteralPath $h9Mode -Value "h9" -NoNewline

    $v1Dll = Join-Path $stageRoot "v1\VirtualCameraMediaSource.dll"
    Set-SourceRegistration $v1Dll
    $registrationChanged = $true
    Restart-FrameServer
    $producerProcess = Start-Producer "normal-v1" "hold" 101 20
    Assert-CameraReady
    Invoke-Consumer "normal-v1" 101 20 0
    Invoke-AclProbe

    Stop-Producer $producerProcess
    $producerProcess = Start-Producer "protocol-mismatch" "mismatch" 102 30
    Invoke-Consumer "protocol-mismatch" 0 0 5

    Stop-Producer $producerProcess
    $producerProcess = Start-Producer "crash-transition" "hold" 103 30
    Invoke-CrashTransition $producerProcess 103 30
    $producerProcess = $null

    $producerProcess = Start-Producer "producer-recovery" "hold" 104 40
    Invoke-Consumer "producer-recovery" 104 40 0
    Restart-FrameServer
    Assert-CameraReady
    Invoke-Consumer "frame-server-restart" 104 40 0

    $v2Dll = Join-Path $stageRoot "v2\VirtualCameraMediaSource.dll"
    Set-SourceRegistration $v2Dll
    Restart-FrameServer
    Assert-CameraReady
    Invoke-Consumer "side-by-side-v2" 104 40 0
    Add-Content -LiteralPath $stateLog -Value "source_local_productization=pass"
    $runSucceeded = $true
} finally {
    Stop-Producer $producerProcess
    $restoreError = $null
    if ($registrationChanged) {
        try {
            Set-SourceRegistration $previousMediaSource
        } catch {
            $restoreError = $_.Exception.Message
        }
    }
    Stop-FrameServer
    Remove-Item -Force -ErrorAction SilentlyContinue $sourceTrace, $sourceLocalMode,
        $h9Mode, $videoModeFile
    Remove-Item -Force -ErrorAction SilentlyContinue $conflictingFiles
    for ($attempt = 0; $attempt -lt 50; ++$attempt) {
        Remove-Item -Recurse -Force -ErrorAction SilentlyContinue $stageRoot
        if (-not (Test-Path -LiteralPath $stageRoot)) { break }
        Start-Sleep -Milliseconds 200
    }
    foreach ($path in @($sourceTrace, $sourceLocalMode, $h9Mode, $videoModeFile,
                          $stageRoot)) {
        if (Test-Path -LiteralPath $path) {
            throw "H9 cleanup left artifact: $path"
        }
    }
    if ($null -ne $restoreError) {
        throw "Could not restore the retained B2 COM registration: $restoreError"
    }
}
if ($runSucceeded) {
    Write-Host "SOURCE_LOCAL_PRODUCTIZATION_PASS"
}

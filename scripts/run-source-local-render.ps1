# SPDX-License-Identifier: Apache-2.0
[CmdletBinding()]
param(
    [int]$Frames = 1,
    [double]$MinimumFps = 0.1,
    [double]$MaximumP95Ms = 1000.0,
    [switch]$KeepInstalled
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"
if ($Frames -le 0 -or $MinimumFps -le 0.0 -or $MaximumP95Ms -le 0.0) {
    throw "Frames and gate values must be positive"
}
$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = [Security.Principal.WindowsPrincipal]::new($identity)
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw "The source-local probe temporarily updates HKLM and requires elevated PowerShell"
}

function Start-CapturedProcess([string]$Path, [string]$Arguments) {
    $startInfo = [Diagnostics.ProcessStartInfo]::new()
    $startInfo.FileName = $Path
    $startInfo.Arguments = $Arguments
    $startInfo.UseShellExecute = $false
    $startInfo.CreateNoWindow = $true
    $startInfo.RedirectStandardOutput = $true
    $startInfo.RedirectStandardError = $true
    $process = [Diagnostics.Process]::new()
    $process.StartInfo = $startInfo
    if (-not $process.Start()) {
        $process.Dispose()
        throw "Process did not start: $Path"
    }
    return [PSCustomObject]@{
        Process = $process
        Stdout = $process.StandardOutput.ReadToEndAsync()
        Stderr = $process.StandardError.ReadToEndAsync()
    }
}

function Complete-CapturedProcess($Capture, [int]$TimeoutMilliseconds,
                                  [string]$OutputPath, [string]$ErrorPath) {
    if (-not $Capture.Process.WaitForExit($TimeoutMilliseconds)) {
        $Capture.Process.Kill()
        $Capture.Process.WaitForExit()
        $standardOutput = $Capture.Stdout.GetAwaiter().GetResult()
        $standardError = $Capture.Stderr.GetAwaiter().GetResult()
        [IO.File]::WriteAllText($OutputPath, $standardOutput,
            [Text.UTF8Encoding]::new($false))
        [IO.File]::WriteAllText($ErrorPath, $standardError,
            [Text.UTF8Encoding]::new($false))
        throw "Process exceeded the ${TimeoutMilliseconds}ms watchdog"
    }
    $Capture.Process.WaitForExit()
    $standardOutput = $Capture.Stdout.GetAwaiter().GetResult()
    $standardError = $Capture.Stderr.GetAwaiter().GetResult()
    [IO.File]::WriteAllText($OutputPath, $standardOutput,
        [Text.UTF8Encoding]::new($false))
    [IO.File]::WriteAllText($ErrorPath, $standardError,
        [Text.UTF8Encoding]::new($false))
    return [PSCustomObject]@{
        ExitCode = $Capture.Process.ExitCode
        StandardOutput = $standardOutput
        StandardError = $standardError
    }
}

$repositoryRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot ".."))
$buildRoot = Join-Path $repositoryRoot `
    "build\windows-x64-release-transport-selection\spikes\transport-selection"
$consumer = Join-Path $buildRoot "transport_source_local_consumer.exe"
$registrar = Join-Path $buildRoot "transport_source_adapter_registrar.exe"
$mediaSource = Join-Path $repositoryRoot `
    "build\tsb\Windows-Camera-626f8b19c5f367602f2e89c6b314573d3776c9df\Samples\VirtualCamera\x64\Release\VirtualCameraMediaSource.dll"
$resultRoot = Join-Path $repositoryRoot "build\transport-selection-results\source-local"
$consumerLog = Join-Path $resultRoot "consumer.log"
$consumerErrorLog = Join-Path $resultRoot "consumer.err.log"
$sourceTraceLog = Join-Path $resultRoot "source-trace.log"
$stateLog = Join-Path $resultRoot "state.log"
$previousMediaSourceLog = Join-Path $resultRoot "previous-media-source.txt"
$sourceTrace = "C:\Users\Public\SparkFaceSourceLocalTrace.log"
$sourceLocalMode = "C:\Users\Public\SparkFaceTransportSourceLocal.flag"
$videoModeFile = "C:\Users\Public\SparkFaceTransportVideoMode.bin"
$conflictingFiles = @(
    "C:\Users\Public\SparkFaceTransportSelectionSourceOwned.flag",
    "C:\Users\Public\SparkFaceTransportCompressed.mode",
    "C:\Users\Public\SparkFaceTransportCompressed.frame",
    "C:\Users\Public\SparkFaceTransportRequestCredit.flag"
)
foreach ($path in @($consumer, $registrar, $mediaSource)) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Required source-local artifact is missing: $path"
    }
}

New-Item -ItemType Directory -Force -Path $resultRoot | Out-Null
Remove-Item -Force -ErrorAction SilentlyContinue $consumerLog, $consumerErrorLog,
    $sourceTraceLog, $stateLog

$sourceClsid = "{A1D6A9E3-6D95-4DA8-AB2E-987B5322F861}"
$classSubkey = "Software\Classes\CLSID\$sourceClsid"
$serverSubkey = "$classSubkey\InprocServer32"
$classKey = [Microsoft.Win32.Registry]::LocalMachine.OpenSubKey($classSubkey, $false)
if ($null -eq $classKey) {
    throw "Retained B2 media-source COM registration is missing"
}
$classKey.Close()
$serverKey = [Microsoft.Win32.Registry]::LocalMachine.OpenSubKey($serverSubkey, $false)
if ($null -eq $serverKey) {
    throw "Retained B2 in-process registration is missing"
}
$previousMediaSource = [string]$serverKey.GetValue($null)
$serverKey.Close()
if ([string]::IsNullOrWhiteSpace($previousMediaSource) -or
    -not (Test-Path -LiteralPath $previousMediaSource -PathType Leaf)) {
    throw "Retained B2 media-source path is invalid: $previousMediaSource"
}
if (Test-Path -LiteralPath $previousMediaSourceLog -PathType Leaf) {
    $retainedMediaSource = [IO.File]::ReadAllText($previousMediaSourceLog).Trim()
    if ([string]::IsNullOrWhiteSpace($retainedMediaSource) -or
        -not (Test-Path -LiteralPath $retainedMediaSource -PathType Leaf)) {
        throw "Retained previous media-source path is invalid: $retainedMediaSource"
    }
    $previousMediaSource = $retainedMediaSource
} elseif ($KeepInstalled) {
    [IO.File]::WriteAllText($previousMediaSourceLog, $previousMediaSource,
        [Text.UTF8Encoding]::new($false))
}

$consumerCapture = $null
$registrationOverridden = Test-Path -LiteralPath $previousMediaSourceLog -PathType Leaf
$runSucceeded = $false
try {
    Remove-Item -Force -ErrorAction SilentlyContinue $sourceTrace,
        $sourceLocalMode, $videoModeFile
    Remove-Item -Force -ErrorAction SilentlyContinue $conflictingFiles
    $modeBytes = [byte[]]::new(12)
    [BitConverter]::GetBytes([uint32]1920).CopyTo($modeBytes, 0)
    [BitConverter]::GetBytes([uint32]1080).CopyTo($modeBytes, 4)
    [BitConverter]::GetBytes([uint32]30).CopyTo($modeBytes, 8)
    [IO.File]::WriteAllBytes($videoModeFile, $modeBytes)
    Set-Content -LiteralPath $sourceLocalMode -Value "source-local" -NoNewline
    $serverKey = [Microsoft.Win32.Registry]::LocalMachine.OpenSubKey($serverSubkey, $true)
    if ($null -eq $serverKey) {
        throw "Could not open the retained B2 COM registration for update"
    }
    $serverKey.SetValue($null, $mediaSource,
        [Microsoft.Win32.RegistryValueKind]::String)
    $serverKey.Close()
    $registrationOverridden = $true
    Set-Content -LiteralPath $stateLog -Value "registration-overridden"

    Get-Process FrameServer -ErrorAction SilentlyContinue | Stop-Process -Force
    Add-Content -LiteralPath $stateLog -Value "frame-server-restarted"
    $cameraReady = $false
    for ($attempt = 0; $attempt -lt 360; ++$attempt) {
        $cameraList = (& $registrar list 2>&1 | Out-String)
        if ($LASTEXITCODE -eq 0 -and
            $cameraList -match "SparkFace Phase0 B2 Camera") {
            $cameraReady = $true
            break
        }
        Start-Sleep -Milliseconds 250
    }
    if (-not $cameraReady) {
        throw "Retained B2 camera did not enumerate within 90 seconds. Last list: $cameraList"
    }
    Add-Content -LiteralPath $stateLog -Value "camera-ready"

    $consumerArguments = "--frames $Frames --width 1920 --height 1080 " +
        "--frame-rate 30 --minimum-fps $MinimumFps --maximum-p95-ms $MaximumP95Ms"
    $consumerCapture = Start-CapturedProcess $consumer $consumerArguments
    Add-Content -LiteralPath $stateLog -Value `
        "consumer-started pid=$($consumerCapture.Process.Id) frames=$Frames"
    $expectedDurationMs = [Math]::Ceiling($Frames * 1000.0 / 30.0)
    $timeoutMs = [Math]::Max(30000, $expectedDurationMs + 30000)
    $consumerResult = Complete-CapturedProcess $consumerCapture $timeoutMs `
        $consumerLog $consumerErrorLog
    Write-Output $consumerResult.StandardOutput.TrimEnd()
    if ($consumerResult.ExitCode -ne 0) {
        throw "Source-local consumer failed with exit code $($consumerResult.ExitCode): $($consumerResult.StandardError)"
    }
    if ($consumerResult.StandardOutput -notmatch
            "(?m)^source_local_frames=$Frames\r?$" -or
        $consumerResult.StandardOutput -notmatch
            "(?m)^SOURCE_LOCAL_GATE_PASS\r?$") {
        throw "Source-local consumer output did not contain the expected strict gate result"
    }
    if (-not (Test-Path -LiteralPath $sourceTrace -PathType Leaf)) {
        throw "Source-local trace was not produced"
    }
    $trace = Get-Content -LiteralPath $sourceTrace -Raw
    if ($trace -notmatch "source_local_adapter_luid=[0-9a-fA-F]{8}:[0-9a-fA-F]{8}") {
        throw "Source-local trace did not record the allocator adapter LUID"
    }
    $expectedTraceCount = if ($Frames -lt 100) { 1 } else {
        [Math]::Floor($Frames / 100) * 100
    }
    if ($trace -notmatch "source_local_gpu_conversion_count=$expectedTraceCount(?:\r?\n|$)") {
        throw "Source-local trace did not record expected GPU conversion checkpoint $expectedTraceCount"
    }
    $runSucceeded = $true
    Write-Host "SOURCE_LOCAL_RENDER_PASS"
} finally {
    if ($null -ne $consumerCapture) {
        if (-not $consumerCapture.Process.HasExited) {
            $consumerCapture.Process.Kill()
            $consumerCapture.Process.WaitForExit()
        }
        $consumerCapture.Process.Dispose()
    }
    if (Test-Path -LiteralPath $sourceTrace -PathType Leaf) {
        Copy-Item -LiteralPath $sourceTrace -Destination $sourceTraceLog -Force
    }
    if (-not $KeepInstalled -or -not $runSucceeded) {
        if ($registrationOverridden) {
            $serverKey = [Microsoft.Win32.Registry]::LocalMachine.OpenSubKey($serverSubkey, $true)
            if ($null -eq $serverKey) {
                Write-Warning "Could not restore the retained B2 COM registration"
            } else {
                $serverKey.SetValue($null, $previousMediaSource,
                    [Microsoft.Win32.RegistryValueKind]::String)
                $serverKey.Close()
            }
        }
        Get-Process FrameServer -ErrorAction SilentlyContinue | Stop-Process -Force
        Remove-Item -Force -ErrorAction SilentlyContinue $sourceTrace,
            $sourceLocalMode, $videoModeFile
        Remove-Item -Force -ErrorAction SilentlyContinue $previousMediaSourceLog
    }
    Add-Content -LiteralPath $stateLog -Value $(
        if ($KeepInstalled -and $runSucceeded) {
            "installed-state-retained"
        } else {
            "cleanup-complete"
        })
}

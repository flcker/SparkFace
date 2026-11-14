# SPDX-License-Identifier: Apache-2.0
[CmdletBinding()]
param(
    [int]$Frames = 2,
    [double]$MinimumFps = 0.1,
    [double]$MaximumP95Ms = 1000.0,
    [ValidatePattern("^[0-9A-Fa-f]{8}:[0-9A-Fa-f]{8}$")]
    [string]$AdapterLuid = "00000000:000178d8",
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
    throw "The source-adapter handoff temporarily updates HKLM and requires elevated PowerShell"
}

function Start-CapturedProcess([string]$Path, [string]$Arguments) {
    $startInfo = [System.Diagnostics.ProcessStartInfo]::new()
    $startInfo.FileName = $Path
    $startInfo.Arguments = $Arguments
    $startInfo.UseShellExecute = $false
    $startInfo.CreateNoWindow = $true
    $startInfo.RedirectStandardOutput = $true
    $startInfo.RedirectStandardError = $true
    $process = [System.Diagnostics.Process]::new()
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
        throw "Process exceeded the ${TimeoutMilliseconds}ms watchdog: $($Capture.Process.StartInfo.FileName)"
    }
    $Capture.Process.WaitForExit()
    $standardOutput = $Capture.Stdout.GetAwaiter().GetResult()
    $standardError = $Capture.Stderr.GetAwaiter().GetResult()
    [System.IO.File]::WriteAllText($OutputPath, $standardOutput,
        [Text.UTF8Encoding]::new($false))
    [System.IO.File]::WriteAllText($ErrorPath, $standardError,
        [Text.UTF8Encoding]::new($false))
    return [PSCustomObject]@{
        ExitCode = $Capture.Process.ExitCode
        StandardOutput = $standardOutput
        StandardError = $standardError
    }
}

$repositoryRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot ".."))
$buildRoot = Join-Path $repositoryRoot "build\windows-x64-release-transport-selection\spikes\transport-selection"
$producer = Join-Path $buildRoot "transport_source_adapter_producer.exe"
$consumer = Join-Path $buildRoot "transport_source_adapter_consumer.exe"
$registrar = Join-Path $buildRoot "transport_source_adapter_registrar.exe"
$mediaSource = Join-Path $repositoryRoot "build\phase0-d-upstream\Windows-Camera-626f8b19c5f367602f2e89c6b314573d3776c9df\Samples\VirtualCamera\x64\Release\VirtualCameraMediaSource.dll"
$resultRoot = Join-Path $repositoryRoot "build\transport-selection-results\source-adapter-handoff"
$producerLog = Join-Path $resultRoot "producer.log"
$producerErrorLog = Join-Path $resultRoot "producer.err.log"
$consumerLog = Join-Path $resultRoot "consumer.log"
$consumerErrorLog = Join-Path $resultRoot "consumer.err.log"
$stateLog = Join-Path $resultRoot "state.log"
$mediaTrace = "C:\Users\Public\SparkFaceDTrace.log"
$controlFile = "C:\Users\Public\SparkFacePhase0DControl.bin"

foreach ($path in @($producer, $consumer, $registrar, $mediaSource)) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Required source-adapter artifact is missing: $path"
    }
}
New-Item -ItemType Directory -Force -Path $resultRoot | Out-Null
Remove-Item -Force -ErrorAction SilentlyContinue $producerLog, $producerErrorLog,
    $consumerLog, $consumerErrorLog, $stateLog, $mediaTrace, $controlFile

$sharedComSubkey = "Software\Classes\CLSID\{A1D6A9E3-6D95-4DA8-AB2E-987B5322F861}\InprocServer32"
$sharedComKey = [Microsoft.Win32.Registry]::LocalMachine.OpenSubKey($sharedComSubkey, $false)
if ($null -eq $sharedComKey) {
    throw "Retained B2 media-source COM registration is missing"
}
$previousMediaSource = [string]$sharedComKey.GetValue($null)
$sharedComKey.Close()
if ([string]::IsNullOrWhiteSpace($previousMediaSource) -or
    -not (Test-Path -LiteralPath $previousMediaSource -PathType Leaf)) {
    throw "Retained B2 media-source path is invalid: $previousMediaSource"
}

$producerCapture = $null
$consumerCapture = $null
$registrationOverridden = $false
try {
    $sharedComKey = [Microsoft.Win32.Registry]::LocalMachine.OpenSubKey($sharedComSubkey, $true)
    if ($null -eq $sharedComKey) {
        throw "Could not open the retained B2 COM registration for update"
    }
    $sharedComKey.SetValue($null, $mediaSource,
        [Microsoft.Win32.RegistryValueKind]::String)
    $sharedComKey.Close()
    $registrationOverridden = $true
    Set-Content -LiteralPath $stateLog -Value "registration-overridden"

    $cameraReady = $false
    for ($attempt = 0; $attempt -lt 360; ++$attempt) {
        $cameraList = (& $registrar list 2>&1 | Out-String)
        if ($LASTEXITCODE -eq 0 -and $cameraList -match "SparkFace Phase0 B2 Camera") {
            $cameraReady = $true
            break
        }
        Start-Sleep -Milliseconds 250
    }
    if (-not $cameraReady) {
        throw "Retained B2 camera did not enumerate within 90 seconds. Last list: $cameraList"
    }
    Add-Content -LiteralPath $stateLog -Value "camera-ready"

    $producerArguments = "--frames $Frames --adapter-luid $AdapterLuid"
    $producerCapture = Start-CapturedProcess $producer $producerArguments
    Add-Content -LiteralPath $stateLog -Value "producer-started pid=$($producerCapture.Process.Id)"
    Start-Sleep -Milliseconds 100

    $consumerArguments = "--frames $Frames --minimum-fps $MinimumFps --maximum-p95-ms $MaximumP95Ms"
    $consumerCapture = Start-CapturedProcess $consumer $consumerArguments
    Add-Content -LiteralPath $stateLog -Value "consumer-started pid=$($consumerCapture.Process.Id)"

    $consumerTimeoutMs = [Math]::Max(30000, ($Frames * 50) + 30000)
    $consumerDeadline = [DateTime]::UtcNow.AddMilliseconds($consumerTimeoutMs)
    while (-not $consumerCapture.Process.HasExited -and
           [DateTime]::UtcNow -lt $consumerDeadline) {
        $trace = if (Test-Path -LiteralPath $mediaTrace) {
            Get-Content -LiteralPath $mediaTrace -Raw -ErrorAction SilentlyContinue
        } else {
            ""
        }
        if ($trace -match "adapter-luid-mismatch[^\r\n]*") {
            throw "Source-adapter same-adapter gate failed: $($Matches[0])"
        }
        Start-Sleep -Milliseconds 100
    }
    $consumerResult = Complete-CapturedProcess $consumerCapture 1000 `
        $consumerLog $consumerErrorLog
    if ($consumerResult.ExitCode -ne 0) {
        throw "Source-adapter consumer failed with exit code $($consumerResult.ExitCode): $($consumerResult.StandardError)"
    }
    Write-Output $consumerResult.StandardOutput.TrimEnd()

    $producerResult = Complete-CapturedProcess $producerCapture 60000 `
        $producerLog $producerErrorLog
    if ($producerResult.ExitCode -ne 0) {
        throw "Source-adapter producer failed with exit code $($producerResult.ExitCode): $($producerResult.StandardError)"
    }
    Write-Output $producerResult.StandardOutput.TrimEnd()
    Write-Host "SOURCE_ADAPTER_HANDOFF_PASS"
} finally {
    foreach ($capture in @($consumerCapture, $producerCapture)) {
        if ($null -ne $capture) {
            if (-not $capture.Process.HasExited) {
                $capture.Process.Kill()
                $capture.Process.WaitForExit()
            }
            $capture.Process.Dispose()
        }
    }
    if (-not $KeepInstalled -and $registrationOverridden) {
        $sharedComKey = [Microsoft.Win32.Registry]::LocalMachine.OpenSubKey($sharedComSubkey, $true)
        if ($null -eq $sharedComKey) {
            Write-Warning "Could not reopen the retained B2 COM registration for restoration"
        } else {
            $sharedComKey.SetValue($null, $previousMediaSource,
                [Microsoft.Win32.RegistryValueKind]::String)
            $sharedComKey.Close()
        }
    }
    Remove-Item -Force -ErrorAction SilentlyContinue $controlFile
    Add-Content -LiteralPath $stateLog -Value "cleanup-complete"
}

# SPDX-License-Identifier: Apache-2.0
[CmdletBinding()]
param(
    [int]$Frames = 1000,
    [double]$MinimumFps = 29.5,
    [double]$MaximumP95Ms = 33.3,
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
    throw "Spike D camera installation requires an elevated PowerShell"
}


$repositoryRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot ".."))
$buildRoot = Join-Path $repositoryRoot "build\windows-x64-release-d\spikes\d3d11-handoff"
$producer = Join-Path $buildRoot "d3d11_handoff_producer.exe"
$consumer = Join-Path $buildRoot "d3d11_handoff_consumer.exe"
$registrar = Join-Path $buildRoot "d3d11_handoff_registrar.exe"
$mediaSource = Join-Path $repositoryRoot "build\phase0-d-upstream\Windows-Camera-626f8b19c5f367602f2e89c6b314573d3776c9df\Samples\VirtualCamera\x64\Release\VirtualCameraMediaSource.dll"
$resultRoot = Join-Path $repositoryRoot "build\phase0-d-results"
$producerLog = Join-Path $resultRoot "producer.log"
$producerErrorLog = Join-Path $resultRoot "producer.err.log"
$consumerLog = Join-Path $resultRoot "consumer.log"
$consumerErrorLog = Join-Path $resultRoot "consumer.err.log"
$stateLog = Join-Path $resultRoot "state.log"
$mediaTrace = "C:\Users\Public\SparkFaceDTrace.log"

foreach ($path in @($producer, $consumer, $registrar, $mediaSource)) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Required Spike D artifact is missing: $path"
    }
}
New-Item -ItemType Directory -Force -Path $resultRoot | Out-Null
Remove-Item -Force -ErrorAction SilentlyContinue $producerLog, $producerErrorLog, $consumerLog, $consumerErrorLog, $stateLog, $mediaTrace
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


$producerProcess = $null
$consumerProcess = $null
$registrationOverridden = $false
try {
    $sharedComKey = [Microsoft.Win32.Registry]::LocalMachine.OpenSubKey($sharedComSubkey, $true)
    if ($null -eq $sharedComKey) {
        throw "Could not open the retained B2 COM registration for update"
    }
    $sharedComKey.SetValue($null, $mediaSource, [Microsoft.Win32.RegistryValueKind]::String)
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


    $producerProcess = Start-Process -FilePath $producer `
        -ArgumentList @("--frames", [string]$Frames) `
        -RedirectStandardOutput $producerLog `
        -RedirectStandardError $producerErrorLog `
        -PassThru -NoNewWindow
    Add-Content -LiteralPath $stateLog -Value "producer-started pid=$($producerProcess.Id)"
    Start-Sleep -Milliseconds 100

    $consumerProcess = Start-Process -FilePath $consumer `
        -ArgumentList @("--frames", [string]$Frames, "--minimum-fps", [string]$MinimumFps,
            "--maximum-p95-ms", [string]$MaximumP95Ms) `
        -RedirectStandardOutput $consumerLog `
        -RedirectStandardError $consumerErrorLog `
        -PassThru -NoNewWindow
    Add-Content -LiteralPath $stateLog -Value "consumer-started pid=$($consumerProcess.Id)"
    $consumerTimeoutMs = [Math]::Max(30000, ($Frames * 50) + 30000)
    $consumerDeadline = [DateTime]::UtcNow.AddMilliseconds($consumerTimeoutMs)
    while (-not $consumerProcess.HasExited -and [DateTime]::UtcNow -lt $consumerDeadline) {
        $trace = if (Test-Path -LiteralPath $mediaTrace) {
            Get-Content -LiteralPath $mediaTrace -Raw -ErrorAction SilentlyContinue
        } else {
            ""
        }
        if ($trace -match "adapter-luid-mismatch[^\r\n]*") {
            throw "Spike D same-adapter gate failed: $($Matches[0])"
        }
        Start-Sleep -Milliseconds 100
    }
    if (-not $consumerProcess.HasExited) {
        throw "Spike D consumer did not exit within $consumerTimeoutMs ms"
    }
    Get-Content -LiteralPath $consumerLog
    if ($consumerProcess.ExitCode -ne 0) {
        throw "Spike D automated gate failed with exit code $($consumerProcess.ExitCode): $(Get-Content -LiteralPath $consumerErrorLog -Raw)"
    }

    $producerProcess.WaitForExit(60000)
    if (-not $producerProcess.HasExited) {
        throw "Spike D producer did not exit within 60 seconds"
    }
    if ($producerProcess.ExitCode -ne 0) {
        throw "Spike D producer failed with exit code $($producerProcess.ExitCode): $(Get-Content -LiteralPath $producerErrorLog -Raw)"
    }
    Get-Content -LiteralPath $producerLog
    Write-Host "D3D11_HANDOFF_AUTOMATED_PASS"
} finally {
    if ($null -ne $consumerProcess -and -not $consumerProcess.HasExited) {
        Stop-Process -Id $consumerProcess.Id -Force -ErrorAction SilentlyContinue
    }
    if ($null -ne $producerProcess -and -not $producerProcess.HasExited) {
        Stop-Process -Id $producerProcess.Id -Force -ErrorAction SilentlyContinue
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
    Add-Content -LiteralPath $stateLog -Value "cleanup-complete"
}

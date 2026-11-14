# SPDX-License-Identifier: Apache-2.0
[CmdletBinding()]
param(
    [int]$Frames = 2,
    [int]$Width = 1920,
    [int]$Height = 1080,
    [int]$FrameRate = 30,
    [double]$MinimumFps = 0.1,
    [double]$MaximumP95Ms = 1000.0,
    [ValidateSet("ProducerOwned", "SourceOwned")]
    [string]$Mode = "ProducerOwned",
    [ValidateSet("Latest", "RequestCredit")]
    [string]$PacingMode = "Latest",
    [ValidateSet("InProcess", "LocalServer", "Surrogate")]
    [string]$ActivationTopology = "InProcess",
    [ValidateSet("None", "H264", "Mjpeg")]
    [string]$CompressedMode = "None",
    [ValidatePattern("^[a-z0-9-]*$")]
    [string]$ResultName = "",
    [switch]$KeepInstalled
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"
if ($Frames -le 0 -or $Width -le 0 -or $Width -gt 1920 -or ($Width % 2) -ne 0 -or
    $Height -le 0 -or $Height -gt 1080 -or ($Height % 2) -ne 0 -or
    $FrameRate -le 0 -or $FrameRate -gt 60 -or
    $MinimumFps -le 0.0 -or $MaximumP95Ms -le 0.0) {
    throw "Frames, mode, and gate values must be valid and positive"
}
if ($CompressedMode -ne "None" -and
    ($Width -ne 1920 -or $Height -ne 1080 -or $FrameRate -ne 30)) {
    throw "Compressed subtype kill gates support only 1920x1080 at 30fps"
}
if ($PacingMode -eq "RequestCredit" -and
    ($Mode -ne "SourceOwned" -or $CompressedMode -ne "None" -or
     $ActivationTopology -ne "InProcess")) {
    throw "RequestCredit pacing requires SourceOwned, uncompressed, in-process mode"
}

$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = [Security.Principal.WindowsPrincipal]::new($identity)
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw "The shared-buffer handoff temporarily updates HKLM and requires elevated PowerShell"
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
        $Capture.Process.WaitForExit()
        $standardOutput = $Capture.Stdout.GetAwaiter().GetResult()
        $standardError = $Capture.Stderr.GetAwaiter().GetResult()
        [System.IO.File]::WriteAllText($OutputPath, $standardOutput,
            [Text.UTF8Encoding]::new($false))
        [System.IO.File]::WriteAllText($ErrorPath, $standardError,
            [Text.UTF8Encoding]::new($false))
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
$producer = Join-Path $buildRoot "transport_shared_buffer_producer.exe"
$consumer = Join-Path $buildRoot "transport_source_adapter_consumer.exe"
$registrar = Join-Path $buildRoot "transport_source_adapter_registrar.exe"
$localServer = Join-Path $buildRoot "transport_activation_local_server.exe"
$codecFixture = Join-Path $buildRoot "transport_codec_fixture.exe"
$mediaSource = Join-Path $repositoryRoot "build\tsb\Windows-Camera-626f8b19c5f367602f2e89c6b314573d3776c9df\Samples\VirtualCamera\x64\Release\VirtualCameraMediaSource.dll"
$topologySlug = $ActivationTopology.ToLowerInvariant()
$resultRoot = if ($ResultName -and $PacingMode -eq "RequestCredit") {
    Join-Path $repositoryRoot "build\transport-selection-results\request-paced\$ResultName"
} elseif ($ResultName) {
    Join-Path $repositoryRoot "build\transport-selection-results\reduced-modes\$ResultName"
} elseif ($CompressedMode -ne "None") {
    Join-Path $repositoryRoot "build\transport-selection-results\codec-bridge\$($CompressedMode.ToLowerInvariant())"
} elseif ($ActivationTopology -eq "InProcess") {
    Join-Path $repositoryRoot "build\transport-selection-results\shared-buffer-handoff"
} else {
    Join-Path $repositoryRoot "build\transport-selection-results\activation-topology\$topologySlug"
}
$producerLog = Join-Path $resultRoot "producer.log"
$producerErrorLog = Join-Path $resultRoot "producer.err.log"
$consumerLog = Join-Path $resultRoot "consumer.log"
$consumerErrorLog = Join-Path $resultRoot "consumer.err.log"
$localServerLog = Join-Path $resultRoot "local-server.log"
$localServerErrorLog = Join-Path $resultRoot "local-server.err.log"
$sourceTraceLog = Join-Path $resultRoot "source-trace.log"
$stateLog = Join-Path $resultRoot "state.log"
$rawCodecFrame = Join-Path $resultRoot "fixture.nv12"
$sourceTrace = "C:\Users\Public\SparkFaceSharedBufferTrace.log"
$ringFile = "C:\Users\Public\SparkFaceTransportSelectionRing.bin"
$sourceOwnedMode = "C:\Users\Public\SparkFaceTransportSelectionSourceOwned.flag"
$compressedModeFile = "C:\Users\Public\SparkFaceTransportCompressed.mode"
$compressedFrame = "C:\Users\Public\SparkFaceTransportCompressed.frame"
$videoModeFile = "C:\Users\Public\SparkFaceTransportVideoMode.bin"
$requestCreditModeFile = "C:\Users\Public\SparkFaceTransportRequestCredit.flag"
$localServerReady = "C:\Users\Public\SparkFaceTransportLocalServer.ready"
$localServerStop = "C:\Users\Public\SparkFaceTransportLocalServer.stop"

$requiredArtifacts = @($consumer, $registrar, $mediaSource)
if ($CompressedMode -eq "None") {
    $requiredArtifacts += $producer
} else {
    $requiredArtifacts += $codecFixture
    if ($Frames -ne 1) {
        throw "Compressed subtype kill gates require -Frames 1"
    }
}
if ($ActivationTopology -eq "LocalServer") {
    $requiredArtifacts += $localServer
}
foreach ($path in $requiredArtifacts) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Required shared-buffer artifact is missing: $path"
    }
}
New-Item -ItemType Directory -Force -Path $resultRoot | Out-Null
Remove-Item -Force -ErrorAction SilentlyContinue $producerLog, $producerErrorLog,
    $consumerLog, $consumerErrorLog, $localServerLog, $localServerErrorLog,
    $sourceTraceLog, $stateLog, $sourceTrace, $ringFile, $sourceOwnedMode,
    $compressedModeFile, $compressedFrame, $localServerReady, $localServerStop,
    $videoModeFile, $requestCreditModeFile
$modeBytes = [byte[]]::new(12)
[BitConverter]::GetBytes([uint32]$Width).CopyTo($modeBytes, 0)
[BitConverter]::GetBytes([uint32]$Height).CopyTo($modeBytes, 4)
[BitConverter]::GetBytes([uint32]$FrameRate).CopyTo($modeBytes, 8)
[IO.File]::WriteAllBytes($videoModeFile, $modeBytes)
if ($Mode -eq "SourceOwned" -and $CompressedMode -eq "None") {
    Set-Content -LiteralPath $sourceOwnedMode -Value "source-owned"
}
if ($PacingMode -eq "RequestCredit") {
    Set-Content -LiteralPath $requestCreditModeFile -Value "request-credit"
}
if ($CompressedMode -ne "None") {
    $ffmpeg = (Get-Command ffmpeg.exe -ErrorAction Stop).Source
    & $codecFixture $rawCodecFrame
    if ($LASTEXITCODE -ne 0) {
        throw "Codec fixture generation failed with exit code $LASTEXITCODE"
    }
    if ($CompressedMode -eq "H264") {
        & $ffmpeg -y -loglevel error -f rawvideo -pix_fmt nv12 -s 1920x1080 `
            -r 30 -i $rawCodecFrame -frames:v 1 -c:v libx264 -preset ultrafast `
            -tune zerolatency -profile:v baseline `
            -x264-params "keyint=1:min-keyint=1:scenecut=0:repeat-headers=1" `
            -f h264 $compressedFrame
        Set-Content -LiteralPath $compressedModeFile -Value "h264" -NoNewline -Encoding Ascii
    } else {
        & $ffmpeg -y -loglevel error -f rawvideo -pix_fmt nv12 -s 1920x1080 `
            -r 30 -i $rawCodecFrame -frames:v 1 -c:v mjpeg -q:v 2 -f mjpeg `
            $compressedFrame
        Set-Content -LiteralPath $compressedModeFile -Value "mjpeg" -NoNewline -Encoding Ascii
    }
    if ($LASTEXITCODE -ne 0) {
        throw "$CompressedMode fixture encoding failed with exit code $LASTEXITCODE"
    }
}

$sourceClsid = "{A1D6A9E3-6D95-4DA8-AB2E-987B5322F861}"
$surrogateAppId = "{D699783A-F944-41E5-B5A2-B109C8C8A8C6}"
$classSubkey = "Software\Classes\CLSID\$sourceClsid"
$sharedComSubkey = "$classSubkey\InprocServer32"
$classKey = [Microsoft.Win32.Registry]::LocalMachine.OpenSubKey($classSubkey, $false)
if ($null -eq $classKey) {
    throw "Retained B2 media-source COM registration is missing"
}
$previousAppId = $classKey.GetValue("AppID")
$classKey.Close()
$sharedComKey = [Microsoft.Win32.Registry]::LocalMachine.OpenSubKey($sharedComSubkey, $false)
if ($null -eq $sharedComKey) {
    throw "Retained B2 media-source in-process registration is missing"
}
$previousMediaSource = [string]$sharedComKey.GetValue($null)
$sharedComKey.Close()
if ([string]::IsNullOrWhiteSpace($previousMediaSource) -or
    -not (Test-Path -LiteralPath $previousMediaSource -PathType Leaf)) {
    throw "Retained B2 media-source path is invalid: $previousMediaSource"
}

$producerCapture = $null
$consumerCapture = $null
$localServerCapture = $null
$registrationOverridden = $false
try {
    $classKey = [Microsoft.Win32.Registry]::LocalMachine.OpenSubKey($classSubkey, $true)
    if ($null -eq $classKey) {
        throw "Could not open the retained B2 COM class for update"
    }
    if ($ActivationTopology -eq "LocalServer") {
        $classKey.DeleteSubKeyTree("InprocServer32", $false)
        $serverKey = $classKey.CreateSubKey("LocalServer32", $true)
        $serverCommand = "`"$localServer`" --dll `"$mediaSource`" --ready `"$localServerReady`" --stop `"$localServerStop`""
        $serverKey.SetValue($null, $serverCommand,
            [Microsoft.Win32.RegistryValueKind]::String)
        $serverKey.Close()
    } else {
        $sharedComKey = $classKey.CreateSubKey("InprocServer32", $true)
        $sharedComKey.SetValue($null, $mediaSource,
            [Microsoft.Win32.RegistryValueKind]::String)
        $sharedComKey.Close()
        if ($ActivationTopology -eq "Surrogate") {
            $classKey.SetValue("AppID", $surrogateAppId,
                [Microsoft.Win32.RegistryValueKind]::String)
            $appKey = [Microsoft.Win32.Registry]::LocalMachine.CreateSubKey(
                "Software\Classes\AppID\$surrogateAppId", $true)
            $appKey.SetValue("DllSurrogate", "",
                [Microsoft.Win32.RegistryValueKind]::String)
            $appKey.Close()
        }
    }
    $classKey.Close()
    $registrationOverridden = $true
    Set-Content -LiteralPath $stateLog -Value "registration-overridden topology=$ActivationTopology"
    if ($CompressedMode -ne "None") {
        $ffmpegVersion = (& $ffmpeg -version | Select-Object -First 1)
        Add-Content -LiteralPath $stateLog -Value "compressed=$CompressedMode $ffmpegVersion"
    }

    if ($ActivationTopology -eq "LocalServer") {
        $localServerArguments = "--dll `"$mediaSource`" --ready `"$localServerReady`" --stop `"$localServerStop`""
        $localServerCapture = Start-CapturedProcess $localServer $localServerArguments
        for ($attempt = 0; $attempt -lt 100; ++$attempt) {
            if (Test-Path -LiteralPath $localServerReady -PathType Leaf) {
                break
            }
            if ($localServerCapture.Process.HasExited) {
                throw "Local COM server exited before registration"
            }
            Start-Sleep -Milliseconds 100
        }
        if (-not (Test-Path -LiteralPath $localServerReady -PathType Leaf)) {
            throw "Local COM server did not register within 10 seconds"
        }
        Add-Content -LiteralPath $stateLog -Value "local-server-ready pid=$($localServerCapture.Process.Id)"
    }

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

    $consumerArguments = "--frames $Frames --width $Width --height $Height --frame-rate $FrameRate --minimum-fps $MinimumFps --maximum-p95-ms $MaximumP95Ms"
    if ($CompressedMode -ne "None") {
        $consumerCapture = Start-CapturedProcess $consumer $consumerArguments
        Add-Content -LiteralPath $stateLog -Value "consumer-started pid=$($consumerCapture.Process.Id) compressed=$CompressedMode"
    } elseif ($Mode -eq "ProducerOwned") {
        $producerFrames = $Frames + 300
        $producerArguments = "--frames $producerFrames --width $Width --height $Height --frame-rate $FrameRate"
        $producerCapture = Start-CapturedProcess $producer $producerArguments
        Add-Content -LiteralPath $stateLog -Value "producer-started pid=$($producerCapture.Process.Id) frames=$producerFrames"
        Start-Sleep -Milliseconds 100
        $consumerCapture = Start-CapturedProcess $consumer $consumerArguments
        Add-Content -LiteralPath $stateLog -Value "consumer-started pid=$($consumerCapture.Process.Id)"
    } else {
        $producerFrames = if ($PacingMode -eq "RequestCredit") {
            $Frames
        } else {
            $Frames + 300
        }
        $producerArguments = "--frames $producerFrames --width $Width --height $Height --frame-rate $FrameRate --source-owned"
        if ($PacingMode -eq "RequestCredit") {
            $producerArguments += " --request-credit"
            $producerCapture = Start-CapturedProcess $producer $producerArguments
            Add-Content -LiteralPath $stateLog -Value "producer-started pid=$($producerCapture.Process.Id) frames=$producerFrames"
        }
        $consumerCapture = Start-CapturedProcess $consumer $consumerArguments
        Add-Content -LiteralPath $stateLog -Value "consumer-started pid=$($consumerCapture.Process.Id)"
        $ringReady = $false
        for ($attempt = 0; $attempt -lt 100; ++$attempt) {
            if (Test-Path -LiteralPath $ringFile -PathType Leaf) {
                $ringReady = $true
                break
            }
            Start-Sleep -Milliseconds 100
        }
        if (-not $ringReady) {
            throw "Source-owned ring did not appear within 10 seconds"
        }
        if ($PacingMode -ne "RequestCredit") {
            $producerCapture = Start-CapturedProcess $producer $producerArguments
            Add-Content -LiteralPath $stateLog -Value "producer-started pid=$($producerCapture.Process.Id) frames=$producerFrames"
        }
    }

    $producerResult = $null
    if ($null -ne $producerCapture) {
        $expectedDurationMs = [Math]::Ceiling($Frames * 1000.0 / $FrameRate)
        $producerTimeoutMs = [Math]::Max(60000, $expectedDurationMs + 30000)
        $producerResult = Complete-CapturedProcess $producerCapture $producerTimeoutMs `
            $producerLog $producerErrorLog
    }
    $expectedDurationMs = [Math]::Ceiling($Frames * 1000.0 / $FrameRate)
    $consumerTimeoutMs = [Math]::Max(30000, $expectedDurationMs + 30000)
    $consumerResult = Complete-CapturedProcess $consumerCapture $consumerTimeoutMs `
        $consumerLog $consumerErrorLog

    Write-Output $consumerResult.StandardOutput.TrimEnd()
    if ($null -ne $producerResult) {
        Write-Output $producerResult.StandardOutput.TrimEnd()
    }
    if ($consumerResult.ExitCode -ne 0) {
        throw "Shared-buffer consumer failed with exit code $($consumerResult.ExitCode): $($consumerResult.StandardError)"
    }
    if ($null -ne $producerResult -and $producerResult.ExitCode -ne 0) {
        throw "Shared-buffer producer failed with exit code $($producerResult.ExitCode): $($producerResult.StandardError)"
    }
    Write-Host "SHARED_BUFFER_HANDOFF_PASS"
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
    if ($null -ne $localServerCapture) {
        Set-Content -LiteralPath $localServerStop -Value "stop"
        if (-not $localServerCapture.Process.WaitForExit(5000)) {
            $localServerCapture.Process.Kill()
            $localServerCapture.Process.WaitForExit()
        }
        $localServerOutput = $localServerCapture.Stdout.GetAwaiter().GetResult()
        $localServerError = $localServerCapture.Stderr.GetAwaiter().GetResult()
        [System.IO.File]::WriteAllText($localServerLog, $localServerOutput,
            [Text.UTF8Encoding]::new($false))
        [System.IO.File]::WriteAllText($localServerErrorLog, $localServerError,
            [Text.UTF8Encoding]::new($false))
        $localServerCapture.Process.Dispose()
    }
    if (Test-Path -LiteralPath $sourceTrace -PathType Leaf) {
        Copy-Item -LiteralPath $sourceTrace -Destination $sourceTraceLog -Force
    }
    if (($ActivationTopology -ne "InProcess" -or -not $KeepInstalled) -and
        $registrationOverridden) {
        $classKey = [Microsoft.Win32.Registry]::LocalMachine.OpenSubKey($classSubkey, $true)
        if ($null -eq $classKey) {
            Write-Warning "Could not reopen the retained B2 COM class for restoration"
        } else {
            $classKey.DeleteSubKeyTree("LocalServer32", $false)
            $classKey.DeleteSubKeyTree("InprocServer32", $false)
            $sharedComKey = $classKey.CreateSubKey("InprocServer32", $true)
            $sharedComKey.SetValue($null, $previousMediaSource,
                [Microsoft.Win32.RegistryValueKind]::String)
            $sharedComKey.Close()
            if ($null -eq $previousAppId) {
                $classKey.DeleteValue("AppID", $false)
            } else {
                $classKey.SetValue("AppID", $previousAppId,
                    [Microsoft.Win32.RegistryValueKind]::String)
            }
            $classKey.Close()
        }
        [Microsoft.Win32.Registry]::LocalMachine.DeleteSubKeyTree(
            "Software\Classes\AppID\$surrogateAppId", $false)
    }
    Remove-Item -Force -ErrorAction SilentlyContinue $sourceTrace, $ringFile,
        $sourceOwnedMode, $compressedModeFile, $compressedFrame,
        $localServerReady, $localServerStop, $videoModeFile,
        $requestCreditModeFile
    Add-Content -LiteralPath $stateLog -Value "cleanup-complete"
}

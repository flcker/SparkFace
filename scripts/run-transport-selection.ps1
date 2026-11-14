# SPDX-License-Identifier: Apache-2.0
[CmdletBinding()]
param(
    [ValidateSet("CrossAdapterCapability", "SourceAdapterHandoff", "SharedBufferHandoff")]
    [string]$Stage = "CrossAdapterCapability",
    [ValidatePattern("^[0-9A-Fa-f]{8}:[0-9A-Fa-f]{8}$")]
    [string]$ConsumerLuid = "00000000:000178d8",
    [int]$Frames = 2
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"
$repositoryRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot ".."))
$buildRoot = Join-Path $repositoryRoot "build\windows-x64-release-transport-selection\spikes\transport-selection"
$resultRoot = Join-Path $repositoryRoot "build\transport-selection-results"
$probe = Join-Path $buildRoot "transport_cross_adapter_probe.exe"
$output = Join-Path $resultRoot "cross-adapter-capability.log"
$errorOutput = Join-Path $resultRoot "cross-adapter-capability.err.log"

if (-not (Test-Path -LiteralPath $probe -PathType Leaf)) {
    throw "Transport capability probe was not built: $probe"
}
New-Item -ItemType Directory -Force -Path $resultRoot | Out-Null
Remove-Item -Force -ErrorAction SilentlyContinue $output, $errorOutput

if ($Stage -eq "CrossAdapterCapability") {
    $startInfo = [System.Diagnostics.ProcessStartInfo]::new()
    $startInfo.FileName = $probe
    $startInfo.Arguments = "--consumer-luid $ConsumerLuid"
    $startInfo.UseShellExecute = $false
    $startInfo.CreateNoWindow = $true
    $startInfo.RedirectStandardOutput = $true
    $startInfo.RedirectStandardError = $true
    $process = [System.Diagnostics.Process]::new()
    $process.StartInfo = $startInfo
    try {
        if (-not $process.Start()) {
            throw "Cross-adapter capability probe did not start"
        }
        $stdoutTask = $process.StandardOutput.ReadToEndAsync()
        $stderrTask = $process.StandardError.ReadToEndAsync()
        if (-not $process.WaitForExit(30000)) {
            $process.Kill()
            throw "Cross-adapter capability probe exceeded the 30-second watchdog"
        }
        $process.WaitForExit()
        $result = $stdoutTask.GetAwaiter().GetResult()
        $errorResult = $stderrTask.GetAwaiter().GetResult()
        [System.IO.File]::WriteAllText($output, $result, [Text.UTF8Encoding]::new($false))
        [System.IO.File]::WriteAllText($errorOutput, $errorResult, [Text.UTF8Encoding]::new($false))
        $exitCode = $process.ExitCode
    } finally {
        $process.Dispose()
    }
    if ($exitCode -ne 0) {
        throw "Cross-adapter capability probe failed with exit code $exitCode; see $errorOutput"
    }
    if ($result -notmatch "(?m)^probe_complete=true\r?$") {
        throw "Cross-adapter capability output is incomplete"
    }
    Get-Content -LiteralPath $output
}

if ($Stage -eq "SourceAdapterHandoff") {
    & (Join-Path $PSScriptRoot "run-source-adapter-handoff.ps1") `
        -Frames $Frames -AdapterLuid $ConsumerLuid
}

if ($Stage -eq "SharedBufferHandoff") {
    & (Join-Path $PSScriptRoot "run-shared-buffer-handoff.ps1") -Frames $Frames
}

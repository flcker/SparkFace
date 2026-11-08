[CmdletBinding()]
param(
    [string]$InstallRoot = (Join-Path $PSScriptRoot "..\build\tools\vcpkg")
)

$ErrorActionPreference = "Stop"
$ManifestPath = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot "..\vcpkg.json"))
$Manifest = Get-Content -Raw $ManifestPath | ConvertFrom-Json
$VcpkgCommit = $Manifest.'builtin-baseline'
if ($VcpkgCommit -notmatch '^[0-9a-f]{40}$') {
    throw "vcpkg.json must contain a full 40-character builtin-baseline commit"
}
$VcpkgRepository = "https://github.com/microsoft/vcpkg.git"
$InstallRoot = [System.IO.Path]::GetFullPath($InstallRoot)

function Invoke-Git {
    & git.exe @args
    if ($LASTEXITCODE -ne 0) {
        throw "git failed with exit code $LASTEXITCODE"
    }
}

if (-not (Test-Path (Join-Path $InstallRoot ".git"))) {
    if (Test-Path $InstallRoot) {
        $entries = @(Get-ChildItem -Force $InstallRoot)
        if ($entries.Count -ne 0) {
            throw "Refusing to replace non-vcpkg directory: $InstallRoot"
        }
    } else {
        New-Item -ItemType Directory -Path $InstallRoot | Out-Null
    }

    Invoke-Git -C $InstallRoot init
    Invoke-Git -C $InstallRoot remote add origin $VcpkgRepository
}

$head = (Get-Content -Raw (Join-Path $InstallRoot ".git\HEAD")).Trim()
$currentCommit = if ($head -match '^[0-9a-f]{40}$') { $head } else { $null }
if ($currentCommit -ne $VcpkgCommit) {
    Invoke-Git -C $InstallRoot fetch --depth 1 origin $VcpkgCommit
    Invoke-Git -C $InstallRoot checkout --detach FETCH_HEAD
}

$vcpkgExecutable = Join-Path $InstallRoot "vcpkg.exe"
$bootstrapStamp = Join-Path $InstallRoot ".sparkface-bootstrapped-commit"
$stampedCommit = if (Test-Path $bootstrapStamp) { (Get-Content -Raw $bootstrapStamp).Trim() } else { $null }
if (-not (Test-Path $vcpkgExecutable) -or $stampedCommit -ne $VcpkgCommit) {
    & (Join-Path $InstallRoot "bootstrap-vcpkg.bat") -disableMetrics
    if ($LASTEXITCODE -ne 0) {
        throw "bootstrap-vcpkg.bat failed with exit code $LASTEXITCODE"
    }
    Set-Content -NoNewline -Path $bootstrapStamp -Value $VcpkgCommit
}

$env:VCPKG_ROOT = $InstallRoot
Write-Host "VCPKG_ROOT=$InstallRoot"
Write-Host "vcpkg_commit=$VcpkgCommit"

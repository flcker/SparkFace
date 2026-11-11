[CmdletBinding()]
param([string]$BaseRef)

$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$spikes = Join-Path $root 'spikes'
$errors = [Collections.Generic.List[string]]::new()
function Fail([string]$message) { $script:errors.Add($message) }
function Require-Array($value, [string]$path) {
    if ($null -eq $value -or $value -isnot [System.Array] -or $value.Count -eq 0) { Fail "$path must be a non-empty array" }
}

$required = 'schema_version','id','title','status','upstream','hardware','dependencies','artifacts','commands','gates','verdict'
$completed = 'completed','closed'
$dirs = Get-ChildItem -LiteralPath $spikes -Directory
foreach ($dir in $dirs) {
    if ($dir.Name -eq 'template') { continue }
    foreach ($name in 'README.md','spike.json','CMakeLists.txt') {
        if (-not (Test-Path -LiteralPath (Join-Path $dir.FullName $name) -PathType Leaf)) { Fail "$($dir.Name): missing $name" }
    }
    $manifestPath = Join-Path $dir.FullName 'spike.json'
    if (-not (Test-Path -LiteralPath $manifestPath)) { continue }
    try { $manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json } catch { Fail "$($dir.Name): spike.json is not strict valid JSON: $($_.Exception.Message)"; continue }
    $manifestKeys = @($manifest.PSObject.Properties.Name)
    foreach ($key in $required) { if ($manifestKeys -notcontains $key) { Fail "$($dir.Name): spike.json missing '$key'" } }
    if ($manifestKeys -contains 'id' -and $manifest.id -ne $dir.Name) { Fail "$($dir.Name): spike.json id must match the directory name" }
    if ($manifestKeys -contains 'commands') {
        $commandKeys = @($manifest.commands.PSObject.Properties.Name)
        foreach ($key in 'build','run','cleanup') { if ($commandKeys -notcontains $key) { Fail "$($dir.Name): commands missing '$key'" } else { Require-Array $manifest.commands.$key "$($dir.Name): commands.$key" } }
    }
    if ($manifestKeys -contains 'artifacts') {
        $artifactIndex = 0
        foreach ($artifact in @($manifest.artifacts)) {
            $artifactKeys = @($artifact.PSObject.Properties.Name)
            foreach ($key in 'name','url','version','sha256','license','acquire') {
                if ($artifactKeys -notcontains $key -or [string]::IsNullOrWhiteSpace([string]$artifact.$key)) {
                    Fail "$($dir.Name): artifacts[$artifactIndex] missing '$key'"
                }
            }
            ++$artifactIndex
        }
    }
    if ($manifestKeys -contains 'status' -and $manifest.status -in $completed) {
        $summaryPath = Join-Path $dir.FullName 'results/summary.json'
        if (-not (Test-Path -LiteralPath $summaryPath -PathType Leaf)) { Fail "$($dir.Name): status '$($manifest.status)' requires results/summary.json" }
        else { try { Get-Content -LiteralPath $summaryPath -Raw | ConvertFrom-Json | Out-Null } catch { Fail "$($dir.Name): results/summary.json is not strict valid JSON: $($_.Exception.Message)" } }
    }
}

$blockedDirectories = 'build','out','cmake-build-debug','cmake-build-release','CMakeFiles','vcpkg_installed'
$blockedExtensions = '.obj','.o','.exe','.dll','.pdb','.lib','.exp','.ilk','.tlog','.ninja','.bin','.onnx','.pt','.pth','.tflite','.engine'
Get-ChildItem -LiteralPath $spikes -Recurse -Force | ForEach-Object {
    $relative = $_.FullName.Substring($spikes.Length + 1)
    if ($_.PSIsContainer -and $blockedDirectories -contains $_.Name) { Fail "${relative}: build directory must not be retained under spikes" }
    if (-not $_.PSIsContainer) {
        if ($blockedExtensions -contains $_.Extension.ToLowerInvariant()) { Fail "${relative}: build artifact must not be retained under spikes" }
        if ($_.Length -gt 10MB) { Fail "${relative}: file exceeds 10 MiB" }
    }
}
if ($BaseRef) {
    $base = (& git -C $root rev-parse --verify "$BaseRef^{commit}" 2>$null)
    if ($LASTEXITCODE -ne 0) { Fail "BaseRef '$BaseRef' cannot be resolved" }
    else {
        $before = & git -C $root ls-tree -d --name-only "$base`:spikes"
        $after = & git -C $root ls-tree -d --name-only 'HEAD:spikes'
        foreach ($name in $before) { if ($name -ne 'template' -and $after -notcontains $name) { Fail "spikes/$name`: existing direct spike directory was deleted relative to $BaseRef" } }
    }
}
if ($errors.Count) { $errors | ForEach-Object { Write-Error $_ -ErrorAction Continue }; exit 1 }
Write-Host "Spike evidence check passed: $($dirs.Count - 1) direct evidence directories validated."

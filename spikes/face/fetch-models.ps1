[CmdletBinding()]
param(
    [string]$Destination = "",
    [switch]$Force
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

if (-not $Destination) {
    $Destination = Join-Path $PSScriptRoot "..\..\build\face-models"
}

$manifestPath = Join-Path $PSScriptRoot "models\models.json"
if (-not (Test-Path -LiteralPath $manifestPath -PathType Leaf)) {
    throw "Model manifest is missing: $manifestPath"
}

$manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
if ($manifest.acquisition_root -ne "build/face-models") {
    throw "Refusing an unexpected acquisition root in ${manifestPath}: $($manifest.acquisition_root)"
}

$destinationPath = [System.IO.Path]::GetFullPath($Destination)
New-Item -ItemType Directory -Force -Path $destinationPath | Out-Null

function Get-Sha256([string]$Path) {
    $stream = [System.IO.File]::OpenRead($Path)
    try {
        $sha = [System.Security.Cryptography.SHA256]::Create()
        try {
            return ([BitConverter]::ToString($sha.ComputeHash($stream))).Replace("-", "").ToLowerInvariant()
        }
        finally {
            $sha.Dispose()
        }
    }
    finally {
        $stream.Dispose()
    }
}

function Get-AcquiredFile([pscustomobject]$Artifact) {
    $downloadUrl = $null
    if ($null -ne $Artifact.PSObject.Properties["download_url"]) {
        $downloadUrl = $Artifact.download_url
    } elseif ($null -ne $Artifact.PSObject.Properties["source"] -and
              $null -ne $Artifact.source.PSObject.Properties["download_url"]) {
        $downloadUrl = $Artifact.source.download_url
    }
    if ([string]::IsNullOrWhiteSpace($Artifact.filename) -or
        [string]::IsNullOrWhiteSpace($downloadUrl) -or
        $Artifact.sha256 -notmatch '^[0-9a-f]{64}$') {
        throw "Artifact metadata is incomplete or has a non-SHA-256 digest: $($Artifact.id)"
    }

    $target = Join-Path $destinationPath $Artifact.filename
    $partial = "$target.partial"

    if (Test-Path -LiteralPath $target -PathType Leaf) {
        $actual = Get-Sha256 $target
        if ($actual -eq $Artifact.sha256) {
            Write-Host "verified=$($Artifact.filename)"
            return $target
        }

        if (-not $Force) {
            throw "SHA-256 mismatch for existing $target. Expected $($Artifact.sha256), got $actual. Delete the file or re-run with -Force to replace it."
        }

        Remove-Item -LiteralPath $target -Force
    }

    Remove-Item -LiteralPath $partial -Force -ErrorAction SilentlyContinue
    try {
        Invoke-WebRequest -UseBasicParsing -Uri $downloadUrl -OutFile $partial -Headers @{
            "Accept" = "application/octet-stream"
            "User-Agent" = "SparkFace-SpikeC-model-acquisition"
            "X-GitHub-Api-Version" = "2022-11-28"
        }

        $actual = Get-Sha256 $partial
        if ($actual -ne $Artifact.sha256) {
            throw "SHA-256 mismatch after downloading $($Artifact.filename). Expected $($Artifact.sha256), got $actual. The partial file was discarded."
        }

        Move-Item -LiteralPath $partial -Destination $target
        Write-Host "downloaded=$($Artifact.filename)"
        return $target
    }
    finally {
        Remove-Item -LiteralPath $partial -Force -ErrorAction SilentlyContinue
    }
}

function Convert-JpegToPpm([string]$Source, [string]$Target, [object[]]$Crop) {
    if ($Crop.Count -ne 4) {
        throw "Expected a four-value crop_xywh for $Source"
    }

    if (Test-Path -LiteralPath $Target -PathType Leaf) {
        Write-Host "verified=$(Split-Path -Leaf $Target)"
        return
    }

    Add-Type -AssemblyName System.Drawing
    $bitmap = [System.Drawing.Bitmap]::new($Source)
    try {
        $cropRectangle = [System.Drawing.Rectangle]::new([int]$Crop[0], [int]$Crop[1], [int]$Crop[2], [int]$Crop[3])
        if ($cropRectangle.X -lt 0 -or $cropRectangle.Y -lt 0 -or
            $cropRectangle.Right -gt $bitmap.Width -or $cropRectangle.Bottom -gt $bitmap.Height) {
            throw "crop_xywh is outside source image bounds for $Source"
        }

        $croppedBitmap = $bitmap.Clone($cropRectangle, [System.Drawing.Imaging.PixelFormat]::Format24bppRgb)
        $bitmap.Dispose()
        $bitmap = $croppedBitmap
        $width = $bitmap.Width
        $height = $bitmap.Height
        $rectangle = [System.Drawing.Rectangle]::new(0, 0, $width, $height)
        $bitmapData = $bitmap.LockBits(
            $rectangle,
            [System.Drawing.Imaging.ImageLockMode]::ReadOnly,
            [System.Drawing.Imaging.PixelFormat]::Format24bppRgb)
        try {
            $stride = [Math]::Abs($bitmapData.Stride)
            $bgr = [byte[]]::new($stride * $height)
            [System.Runtime.InteropServices.Marshal]::Copy($bitmapData.Scan0, $bgr, 0, $bgr.Length)
            $rgb = [byte[]]::new($width * $height * 3)

            for ($y = 0; $y -lt $height; ++$y) {
                $sourceRow = if ($bitmapData.Stride -ge 0) { $y * $stride } else { ($height - 1 - $y) * $stride }
                $targetRow = $y * $width * 3
                for ($x = 0; $x -lt $width; ++$x) {
                    $sourceOffset = $sourceRow + ($x * 3)
                    $targetOffset = $targetRow + ($x * 3)
                    $rgb[$targetOffset] = $bgr[$sourceOffset + 2]
                    $rgb[$targetOffset + 1] = $bgr[$sourceOffset + 1]
                    $rgb[$targetOffset + 2] = $bgr[$sourceOffset]
                }
            }

            $header = [System.Text.Encoding]::ASCII.GetBytes("P6`n$width $height`n255`n")
            $stream = [System.IO.File]::Open($Target, [System.IO.FileMode]::CreateNew, [System.IO.FileAccess]::Write)
            try {
                $stream.Write($header, 0, $header.Length)
                $stream.Write($rgb, 0, $rgb.Length)
            }
            finally {
                $stream.Dispose()
            }
        }
        finally {
            $bitmap.UnlockBits($bitmapData)
        }
    }
    finally {
        $bitmap.Dispose()
    }

    Write-Host "generated=$(Split-Path -Leaf $Target)"
}

foreach ($model in $manifest.models) {
    Get-AcquiredFile $model | Out-Null
}

$stillPath = Get-AcquiredFile $manifest.overlay_test_still
$ppmPath = Join-Path $destinationPath $manifest.overlay_test_still.ppm_filename
Convert-JpegToPpm -Source $stillPath -Target $ppmPath -Crop $manifest.overlay_test_still.crop_xywh
Write-Host "FACE_MODELS_FETCH_PASS"

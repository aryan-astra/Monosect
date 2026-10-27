param(
    [Parameter(Mandatory = $true)][string]$Destination,
    [string]$Archive
)
$ErrorActionPreference = 'Stop'
$pin = Get-Content -LiteralPath (Join-Path $PSScriptRoot '../resources/ffmpeg-pin.json') -Raw | ConvertFrom-Json
$destinationPath = [IO.Path]::GetFullPath($Destination)
New-Item -ItemType Directory -Path $destinationPath -Force | Out-Null
$temporaryParent = [IO.Path]::GetFullPath([IO.Path]::GetTempPath())
$staging = Join-Path $temporaryParent ('monosect-ffmpeg-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $staging | Out-Null
try {
    if (-not $Archive) {
        $Archive = Join-Path $staging 'ffmpeg.zip'
        Invoke-WebRequest -Uri $pin.MONOSECT_FFMPEG_URL -OutFile $Archive -TimeoutSec 300
    }
    $actual = (Get-FileHash -LiteralPath $Archive -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($actual -ne $pin.MONOSECT_FFMPEG_SHA256) { throw 'FFmpeg archive digest mismatch' }
    $unpacked = Join-Path $staging 'unpacked'
    Expand-Archive -LiteralPath $Archive -DestinationPath $unpacked
    foreach ($tool in @('ffmpeg', 'ffprobe')) {
        $matches = @(Get-ChildItem -LiteralPath $unpacked -Recurse -Filter "$tool.exe")
        if ($matches.Count -ne 1) { throw "Expected exactly one $tool.exe" }
        $key = if ($tool -eq 'ffmpeg') { 'MONOSECT_FFMPEG_EXE_SHA256' } else { 'MONOSECT_FFPROBE_EXE_SHA256' }
        $actual = (Get-FileHash -LiteralPath $matches[0].FullName -Algorithm SHA256).Hash.ToLowerInvariant()
        if ($actual -ne $pin.$key) { throw "$tool executable digest mismatch" }
    }
    foreach ($tool in @('ffmpeg', 'ffprobe')) {
        $source = Get-ChildItem -LiteralPath $unpacked -Recurse -Filter "$tool.exe"
        $target = Join-Path $destinationPath "$tool.exe"
        Copy-Item -LiteralPath $source.FullName -Destination $target -Force
        if ($env:GITHUB_ENV) { "MONOSECT_$($tool.ToUpperInvariant())=$target" | Out-File -FilePath $env:GITHUB_ENV -Append -Encoding utf8 }
    }
    $license = @(Get-ChildItem -LiteralPath $unpacked -Recurse -File -Filter LICENSE)
    $readme = @(Get-ChildItem -LiteralPath $unpacked -Recurse -File -Filter README.txt)
    if ($license.Count -ne 1 -or $readme.Count -ne 1) { throw 'FFmpeg distribution notices are missing' }
    Copy-Item -LiteralPath $license[0].FullName -Destination (Join-Path $destinationPath 'FFmpeg-LICENSE.txt') -Force
    Copy-Item -LiteralPath $readme[0].FullName -Destination (Join-Path $destinationPath 'FFmpeg-README.txt') -Force
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot '../resources/ffmpeg-pin.json') -Destination $destinationPath -Force
    Write-Host "Verified FFmpeg $($pin.MONOSECT_FFMPEG_VERSION) at $destinationPath"
} finally {
    $resolvedStaging = [IO.Path]::GetFullPath($staging)
    if (-not $resolvedStaging.StartsWith($temporaryParent, [StringComparison]::OrdinalIgnoreCase) -or
        [IO.Path]::GetFileName($resolvedStaging) -notlike 'monosect-ffmpeg-*') { throw 'Unsafe cleanup path' }
    Remove-Item -LiteralPath $resolvedStaging -Recurse -Force
}

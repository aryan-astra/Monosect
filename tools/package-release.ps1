param(
    [Parameter(Mandatory = $true)][string]$BuildDirectory,
    [Parameter(Mandatory = $true)][string]$ToolsDirectory,
    [Parameter(Mandatory = $true)][string]$Destination,
    [switch]$OmitMediaTools,
    [switch]$UseSoftwareOpenGl
)
$ErrorActionPreference = 'Stop'
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$version = (Get-Content -LiteralPath (Join-Path $repo 'version.txt') -Raw).Trim()
$pin = Get-Content -LiteralPath (Join-Path $repo 'resources/ffmpeg-pin.json') -Raw | ConvertFrom-Json
$output = [IO.Path]::GetFullPath($Destination)
$stage = Join-Path $output "Monosect-$version"
if (Test-Path -LiteralPath $stage) { throw "Package directory already exists: $stage" }
New-Item -ItemType Directory -Path $stage -Force | Out-Null
Copy-Item -LiteralPath (Join-Path $BuildDirectory 'Monosect.exe') -Destination $stage
foreach ($tool in @('ffmpeg', 'ffprobe')) {
    $binary = Join-Path $ToolsDirectory "$tool.exe"
    $key = if ($tool -eq 'ffmpeg') { 'MONOSECT_FFMPEG_EXE_SHA256' } else { 'MONOSECT_FFPROBE_EXE_SHA256' }
    if ((Get-FileHash -LiteralPath $binary -Algorithm SHA256).Hash.ToLowerInvariant() -ne $pin.$key) {
        throw "$tool executable digest mismatch"
    }
    Copy-Item -LiteralPath $binary -Destination $stage
}
foreach ($notice in @('FFmpeg-LICENSE.txt', 'FFmpeg-README.txt', 'ffmpeg-pin.json')) {
    Copy-Item -LiteralPath (Join-Path $ToolsDirectory $notice) -Destination $stage
}
foreach ($file in @('LICENSE', 'THIRD_PARTY_NOTICES.md', 'README.md', 'Monosect-icon-square.png', 'Monosect-full-logo.png')) {
    Copy-Item -LiteralPath (Join-Path $repo $file) -Destination $stage
}
Copy-Item -LiteralPath (Join-Path $repo 'docs/licenses') -Destination $stage -Recurse
New-Item -ItemType Directory -Path (Join-Path $stage 'docs') | Out-Null
Copy-Item -LiteralPath (Join-Path $repo 'docs/PRODUCTION_READINESS.md') -Destination (Join-Path $stage 'docs')
Copy-Item -LiteralPath (Join-Path $repo 'docs/audits') -Destination (Join-Path $stage 'docs') -Recurse

# Package every non-system DLL imported by the executable and its runtime libraries.
$compilerEntry = Select-String -LiteralPath (Join-Path $BuildDirectory 'CMakeCache.txt') -Pattern '^CMAKE_CXX_COMPILER:FILEPATH=(.+)$'
if (-not $compilerEntry) { throw 'The build does not identify its C++ compiler' }
$compiler = $compilerEntry.Matches[0].Groups[1].Value
$compilerDirectory = Split-Path $compiler
function Copy-ImportedLibraries([string]$RootBinary) {
    $pending = [Collections.Generic.Queue[string]]::new()
    $pending.Enqueue($RootBinary)
    $seen = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    while ($pending.Count) {
        $binary = $pending.Dequeue()
        $imports = & (Join-Path $compilerDirectory 'objdump.exe') -p $binary
        if ($LASTEXITCODE -ne 0) { throw "Could not inspect imports: $binary" }
        foreach ($line in $imports) {
            if ($line -notmatch 'DLL Name:\s*(\S+)') { continue }
            $dll = $Matches[1]
            if (-not $seen.Add($dll)) { continue }
            if ($dll -match '^(api-ms-win-|ext-ms-win-)' -or
                (Test-Path -LiteralPath (Join-Path "$env:SystemRoot/System32" $dll))) { continue }
            $runtime = Join-Path $compilerDirectory $dll
            if (-not (Test-Path -LiteralPath $runtime)) { throw "A required DLL is missing: $dll" }
            $target = Join-Path $stage $dll
            Copy-Item -LiteralPath $runtime -Destination $target
            $pending.Enqueue($target)
        }
    }
}
Copy-ImportedLibraries (Join-Path $stage 'Monosect.exe')
# Hosted Windows runners may have no OpenGL 3.3 driver. Mesa is test-only.
$softwareLibraries = @()
if ($UseSoftwareOpenGl) {
    $packageLibraries = @(Get-ChildItem -LiteralPath $stage -Filter '*.dll' | Select-Object -ExpandProperty Name)
    foreach ($driver in @('opengl32.dll', 'libgallium_wgl.dll')) {
        $driverTarget = Join-Path $stage $driver
        Copy-Item -LiteralPath (Join-Path $compilerDirectory $driver) -Destination $driverTarget
        Copy-ImportedLibraries $driverTarget
    }
    $softwareLibraries = @(Get-ChildItem -LiteralPath $stage -Filter '*.dll' |
        Where-Object Name -NotIn $packageLibraries | Select-Object -ExpandProperty FullName)
}
$manifest = @{
    version = $version
    sourceRevision = (& git -C $repo rev-parse HEAD).Trim()
    workingTreeDirty = [bool](& git -C $repo status --porcelain --untracked-files=no)
    compiler = (& $compiler --version | Select-Object -First 1)
    ffmpeg = $pin.MONOSECT_FFMPEG_VERSION
}
$manifest | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $stage 'build-manifest.json') -Encoding utf8

$previousPath = $env:PATH
$previousGallium = $env:GALLIUM_DRIVER
try {
    if ($UseSoftwareOpenGl) { $env:GALLIUM_DRIVER = 'llvmpipe' }
    $env:PATH = "$env:SystemRoot\System32;$env:SystemRoot"
    $smokeOutput = Join-Path $stage 'smoke-stdout.log'
    $smokeError = Join-Path $stage 'smoke-stderr.log'
    $process = Start-Process -FilePath (Join-Path $stage 'Monosect.exe') -ArgumentList '--smoke-test' -WorkingDirectory $stage -WindowStyle Hidden -PassThru -RedirectStandardOutput $smokeOutput -RedirectStandardError $smokeError
    if (-not $process.WaitForExit(30000)) {
        $process.Kill($true)
        throw 'Packaged GUI smoke test timed out'
    }
    if ($process.ExitCode -ne 0) {
        $diagnostic = Get-Content -LiteralPath $smokeError -Raw
        throw "Packaged GUI smoke test failed: $($process.ExitCode). $diagnostic"
    }
    $process.Dispose()
    Remove-Item -LiteralPath $smokeOutput, $smokeError
} finally {
    $env:PATH = $previousPath
    $env:GALLIUM_DRIVER = $previousGallium
    foreach ($library in $softwareLibraries) { Remove-Item -LiteralPath $library }
}
if ($OmitMediaTools) {
    foreach ($file in @('ffmpeg.exe', 'ffprobe.exe', 'FFmpeg-LICENSE.txt', 'FFmpeg-README.txt')) {
        Remove-Item -LiteralPath (Join-Path $stage $file)
    }
    'FFmpeg and ffprobe are downloaded from the pinned upstream build on first launch. Internet access and write permission to this folder are required for that setup.' |
        Set-Content -LiteralPath (Join-Path $stage 'FIRST-RUN.txt') -Encoding utf8
}
$zip = Join-Path $output "Monosect-$version-portable.zip"
Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $zip -Force
$hash = (Get-FileHash -LiteralPath $zip -Algorithm SHA256).Hash.ToLowerInvariant()
"$hash  $([IO.Path]::GetFileName($zip))" | Set-Content -LiteralPath (Join-Path $output 'SHA256SUMS.txt') -Encoding ascii
Write-Host "Package passed the GUI smoke test with only Windows system directories on PATH: $zip"

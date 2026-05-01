[CmdletBinding()]
param(
    [Parameter(Mandatory = $true, Position = 0)]
    [ValidateSet('patch', 'minor', 'major')]
    [string]$Part
)

$ErrorActionPreference = 'Stop'

$repoRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$versionFile = Join-Path $repoRoot 'version.txt'

if (-not (Test-Path -Path $versionFile -PathType Leaf)) {
    throw "version.txt not found at $versionFile"
}

$raw = Get-Content -Path $versionFile -Raw
$currentVersion = $raw.Trim()

$versionPattern = '^(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)$'
if ($currentVersion -notmatch $versionPattern) {
    throw "Invalid version in version.txt: '$currentVersion'. Expected MAJOR.MINOR.PATCH."
}

[int]$major = $Matches[1]
[int]$minor = $Matches[2]
[int]$patch = $Matches[3]

switch ($Part) {
    'patch' {
        $patch += 1
    }
    'minor' {
        $minor += 1
        $patch = 0
    }
    'major' {
        $major += 1
        $minor = 0
        $patch = 0
    }
}

$nextVersion = "$major.$minor.$patch"

if ($nextVersion -notmatch $versionPattern) {
    throw "Computed version '$nextVersion' is invalid."
}

[System.IO.File]::WriteAllText(
    $versionFile,
    "$nextVersion`n",
    [System.Text.UTF8Encoding]::new($false)
)

Write-Output "Bumped version: $currentVersion -> $nextVersion"

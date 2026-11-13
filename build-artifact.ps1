$ErrorActionPreference = 'Stop'

& (Join-Path $PSScriptRoot 'build.ps1') -Architecture x64
& (Join-Path $PSScriptRoot 'build.ps1') -Architecture x86

$artifactRoot = Join-Path $PSScriptRoot 'tvrockontvtest'
New-Item -ItemType Directory -Force -Path (Join-Path $artifactRoot 'x64'), (Join-Path $artifactRoot 'x86') | Out-Null
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'target\x86_64-pc-windows-msvc\release\TvRockOnTVTest.x64.tvtp') -Destination (Join-Path $artifactRoot 'x64\TvRockOnTVTest.x64.tvtp') -Force
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'target\i686-pc-windows-msvc\release\TvRockOnTVTest.tvtp') -Destination (Join-Path $artifactRoot 'x86\TvRockOnTVTest.tvtp') -Force

# Include license texts and usage instructions in both CI artifacts and release ZIPs.
foreach ($document in @('README.md', 'LICENSE.md', 'THIRD_PARTY_NOTICES.md')) {
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot $document) -Destination (Join-Path $artifactRoot $document) -Force
}
$licenseRoot = Join-Path $artifactRoot 'licenses'
New-Item -ItemType Directory -Force -Path $licenseRoot | Out-Null
foreach ($license in @('CC0-1.0.txt', 'GPL-2.0.txt')) {
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot "licenses/$license") -Destination (Join-Path $licenseRoot $license) -Force
}

Write-Output "Artifacts: $artifactRoot"

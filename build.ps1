param(
    [ValidateSet('x64','x86')][string]$Architecture = 'x64',
    [switch]$Debug
)
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$target = if ($Architecture -eq 'x86') { 'i686-pc-windows-msvc' } else { 'x86_64-pc-windows-msvc' }
$profile = if ($Debug) { 'debug' } else { 'release' }
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (!(Test-Path -LiteralPath $vswhere)) { throw 'Visual Studio C++ Build Tools are required.' }
$installation = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (!$installation) { throw 'Visual Studio C++ Build Tools are required.' }
$devcmd = Join-Path $installation 'Common7\Tools\VsDevCmd.bat'
$output = Join-Path $root "target\$target\$profile"
New-Item -ItemType Directory -Force -Path $output | Out-Null
$flags = if ($Debug) { @('/Od','/Zi','/MDd','/D_DEBUG') } else { @('/O2','/MD','/DNDEBUG','/Gy') }
$name = if ($Architecture -eq 'x64') { 'TvRockOnTVTest.x64.tvtp' } else { 'TvRockOnTVTest.tvtp' }
$linkFlags = if ($Debug) { @('/DEBUG') } else { @('/OPT:REF','/OPT:ICF') }
Push-Location $output
try {
    $compilerArgs = @('/nologo','/LD','/EHsc','/std:c++17','/utf-8','/W3','/DUNICODE','/D_UNICODE','/DWIN32','/D_WINDOWS','/D_USRDLL') + $flags
    $compilerArgs += @(('"' + $root + '\TvRockOnTVTest.cpp"'), '/link', ('"/DEF:' + $root + '\Exports.def"'), ('/OUT:' + $name), '/INCREMENTAL:NO')
    $compilerArgs += @($linkFlags) + @('user32.lib','gdi32.lib','shell32.lib','shlwapi.lib')
    [IO.File]::WriteAllText((Join-Path $output 'compile.rsp'), ($compilerArgs -join ' '), [Text.Encoding]::Unicode)
    $commands = @('@echo off', ('call "{0}" -no_logo -arch={1} -host_arch=x64' -f $devcmd, $Architecture),
        'if errorlevel 1 exit /b %errorlevel%', 'cl @compile.rsp', 'exit /b %errorlevel%')
    [IO.File]::WriteAllLines((Join-Path $output 'compile.cmd'), $commands, [Text.Encoding]::Default)
    & $env:ComSpec /d /c compile.cmd
    if ($LASTEXITCODE) { throw "Plugin build failed (exit code $LASTEXITCODE)." }
} finally {
    Pop-Location
}
Write-Output "Built: $output\$name"

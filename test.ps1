param([ValidateSet('x64','x86')][string]$Architecture = 'x64')
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$output = Join-Path $root "target\tests\$Architecture"
New-Item -ItemType Directory -Force -Path $output | Out-Null
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$installation = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (!$installation) { throw 'Visual Studio C++ Build Tools are required.' }
$devcmd = Join-Path $installation 'Common7\Tools\VsDevCmd.bat'
$target = if ($Architecture -eq 'x86') { 'i686-pc-windows-msvc' } else { 'x86_64-pc-windows-msvc' }
$abiHeader = Join-Path (Split-Path $root -Parent) 'tvrockdtv\compat'
if (!(Test-Path (Join-Path $abiHeader 'TvRockDTV.h'))) { $abiHeader = $root }
# Compile the mock DLL with the DLL project's own header when available.
$mockArgs = '/nologo /LD /EHsc /std:c++17 /utf-8 /W3 /MD /DUNICODE /D_UNICODE /DWIN32 /I"{0}" "{1}\tests\mock_dtv.cpp" /link /OUT:TvRockDTV.dll' -f $abiHeader, $root
$testArgs = '/nologo /EHsc /std:c++17 /utf-8 /W3 /MD /DUNICODE /D_UNICODE /DWIN32 "{0}\tests\regression.cpp" "{0}\tests\sdk_compat.cpp" /link /OUT:regression.exe user32.lib gdi32.lib shell32.lib shlwapi.lib' -f $root
[IO.File]::WriteAllText((Join-Path $output 'mock.rsp'), $mockArgs, [Text.Encoding]::Unicode)
[IO.File]::WriteAllText((Join-Path $output 'test.rsp'), $testArgs, [Text.Encoding]::Unicode)
$commands = @('@echo off', ('call "{0}" -no_logo -arch={1} -host_arch=x64' -f $devcmd, $Architecture),
    'if errorlevel 1 exit /b %errorlevel%', 'cl @mock.rsp', 'if errorlevel 1 exit /b %errorlevel%',
    'cl @test.rsp', 'exit /b %errorlevel%')
[IO.File]::WriteAllLines((Join-Path $output 'test.cmd'), $commands, [Text.Encoding]::Default)
Push-Location $output
try {
    & $env:ComSpec /d /c test.cmd
    if ($LASTEXITCODE) { throw 'Test compilation failed.' }
    $actual = Join-Path (Split-Path $root -Parent) "tvrockdtv\target\$target\release\TvRockDTV.dll"
    $processArgs = @{
        FilePath = (Join-Path $output 'regression.exe')
        WindowStyle = 'Hidden'
        PassThru = $true
        RedirectStandardOutput = (Join-Path $output 'stdout.log')
        RedirectStandardError = (Join-Path $output 'stderr.log')
    }
    if (Test-Path $actual) { $processArgs.ArgumentList = '"' + $actual + '"' }
    $process = Start-Process @processArgs
    if (!$process.WaitForExit(30000)) {
        $process.Kill()
        throw 'Regression tests timed out.'
    }
    Get-Content (Join-Path $output 'stdout.log')
    Get-Content (Join-Path $output 'stderr.log')
    if ($process.ExitCode) { throw "Regression tests failed: $($process.ExitCode)" }
} finally { Pop-Location }

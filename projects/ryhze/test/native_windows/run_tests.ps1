[CmdletBinding()]
param(
    [string]$CMake = 'cmake',
    [string]$Generator = 'Visual Studio 18 2026',
    [string]$BuildDirectory = ''
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$repo = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
if (-not $BuildDirectory) { $BuildDirectory = Join-Path $repo 'build/native-windows-lifecycle' }
& $CMake -S $PSScriptRoot -B $BuildDirectory -G $Generator -A x64
if ($LASTEXITCODE -ne 0) { throw 'Native lifecycle test configuration failed.' }
& $CMake --build $BuildDirectory --config Release --parallel 1
if ($LASTEXITCODE -ne 0) { throw 'Native lifecycle test compilation failed.' }
$executable = Join-Path $BuildDirectory 'Release/host_lifecycle_test.exe'
& $executable
if ($LASTEXITCODE -ne 0) { throw 'Native lifecycle tests failed.' }

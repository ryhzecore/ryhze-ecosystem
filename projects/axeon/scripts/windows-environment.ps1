param([string]$ToolchainRoot = 'C:\Projects\Axeon\Windows\toolchains')
$ErrorActionPreference = 'Stop'
# Process-local environment only. Discover the installed x64 C++ workload through
# Microsoft's supported locator; prefer the qualified VS2022 installation.
$axeonVswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path -LiteralPath $axeonVswhere)) { throw 'Install Visual Studio Build Tools with the x64 C++ workload and Windows SDK; vswhere is missing.' }
$axeonVs = & $axeonVswhere -version '[17.0,18.0)' -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if ($LASTEXITCODE -ne 0 -or -not $axeonVs) { throw 'The qualified Visual Studio 2022 x64 C++ toolchain was not found.' }
$axeonVs = @($axeonVs)[0]
$axeonDevCmd = Join-Path $axeonVs 'Common7\Tools\VsDevCmd.bat'
# Batch setup cannot parse a quoted PATH entry inherited from another installer.
# PATH stores literal directories; trim entry quotes only in this process.
$env:PATH = (($env:PATH -split ';') | ForEach-Object { $_.Trim('"') }) -join ';'
$axeonEnvLines = & $env:ComSpec /d /c ('call "'+$axeonDevCmd+'" -no_logo -arch=amd64 -host_arch=amd64 && set')
if ($LASTEXITCODE -ne 0) { throw 'Visual Studio environment setup failed.' }
foreach ($axeonLine in $axeonEnvLines) {
    if ($axeonLine -match '^([^=]+)=(.*)$') {
        $axeonName = $matches[1]
        $axeonValue = $matches[2]
        if ($axeonName -match '^(PATH|INCLUDE|LIB|LIBPATH|VCToolsInstallDir|VCToolsVersion|VSINSTALLDIR|WindowsSdkDir|WindowsSDKVersion|UniversalCRTSdkDir|UCRTVersion)$') {
            [Environment]::SetEnvironmentVariable($axeonName,$axeonValue,'Process')
        }
    }
}
$axeonRustBin = Join-Path $ToolchainRoot 'rust-1.85.0-x86_64-pc-windows-msvc\bin'
$axeonLlvmBin = Join-Path $ToolchainRoot 'llvm-19.1.7-x86_64-pc-windows-msvc\bin'
foreach ($axeonTool in @((Join-Path $axeonRustBin 'rustc.exe'),(Join-Path $axeonRustBin 'cargo.exe'),(Join-Path $axeonLlvmBin 'clang.exe'))) {
    if (-not (Test-Path -LiteralPath $axeonTool -PathType Leaf)) { throw "Pinned native tool is missing: $axeonTool" }
}
$env:PATH = $axeonRustBin+';'+$axeonLlvmBin+';'+$env:PATH
$env:AXEON_CLANG = Join-Path $axeonLlvmBin 'clang.exe'
$env:CARGO_BUILD_JOBS = '1'
$env:RUST_TEST_THREADS = '1'
$env:CARGO_INCREMENTAL = '0'

# Build locally on an authorized Windows x64 host. Never installs, signs or submits.
param(
    [string]$Flutter = "flutter",
    [string]$Python = "python",
    [string]$Nsis = "",
    [Parameter(Mandatory = $true)][string]$MsvcRuntimeDirectory,
    [ValidateRange(1, 8)][int]$TestConcurrency = 1
)
$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest
$repo = (Resolve-Path (Join-Path $PSScriptRoot "../..")).Path
Push-Location $repo
try {
    if ([Environment]::OSVersion.Platform -ne [PlatformID]::Win32NT) {
        throw "A Windows host is required."
    }
    $dirty = & git status --porcelain
    if ($LASTEXITCODE -ne 0 -or $dirty) { throw "A clean, committed source tree is required." }
    $sourceRevision = (& git rev-parse HEAD).Trim()
    if ($LASTEXITCODE -ne 0) { throw "Cannot identify source." }
    $version = Select-String -Path "pubspec.yaml" -Pattern '^version: 0\.6\.0\+3$'
    if (-not $version) { throw "Candidate metadata must be 0.6.0+3." }
    $sdkOutput = & $Flutter --suppress-analytics --no-version-check --version --machine
    if ($LASTEXITCODE -ne 0) { throw "Cannot verify the Flutter SDK." }
    $sdk = ($sdkOutput | Out-String | ConvertFrom-Json)
    if ($sdk.frameworkRevision -ne "5fc346839b5d0eef006ed8404392afb4dfae428d" -or $sdk.dartSdkVersion -ne "3.13.5") {
        throw "Use the pinned Flutter 3.47.6 / Dart 3.13.5 toolchain; no implicit SDK upgrade."
    }
    & $Flutter --suppress-analytics --no-version-check pub get --enforce-lockfile
    if ($LASTEXITCODE -ne 0) { throw "Locked dependency preparation failed." }
    & $Flutter --suppress-analytics --no-version-check analyze --no-pub
    if ($LASTEXITCODE -ne 0) { throw "Analysis failed." }
    & $Flutter --suppress-analytics --no-version-check test --no-pub --concurrency=$TestConcurrency
    if ($LASTEXITCODE -ne 0) { throw "Flutter tests failed." }
    & $Python -m unittest discover -s packaging/windows -p "test_*.py"
    if ($LASTEXITCODE -ne 0) { throw "Packaging checks failed." }
    & $Flutter --suppress-analytics --no-version-check build windows --release --no-pub --target=lib/main.dart --build-name=0.6.0 --build-number=3
    if ($LASTEXITCODE -ne 0) { throw "Windows build failed." }
    $finalRevision = (& git rev-parse HEAD).Trim()
    if ($LASTEXITCODE -ne 0 -or $finalRevision -ne $sourceRevision) { throw "Source revision changed during the build." }
    $dirty = & git status --porcelain
    if ($LASTEXITCODE -ne 0 -or $dirty) { throw "Source changed during the build; preserve output for diagnosis only." }
    $bundle = Join-Path $repo "build/windows/x64/runner/Release"
    $artifacts = Join-Path $repo "build/windows/candidate-$sourceRevision"
    if (Test-Path $artifacts) { throw "Artifact destination already exists; preserve previous evidence." }
    New-Item -ItemType Directory -Path $artifacts | Out-Null
    & (Join-Path $PSScriptRoot "stage_msvc_runtime.ps1") -BundleDirectory $bundle -MsvcRuntimeDirectory $MsvcRuntimeDirectory -ProvenancePath (Join-Path $artifacts "msvc-runtime-provenance.json")
    $sdkOutput | Out-File (Join-Path $artifacts "sdk.json") -Encoding utf8
    $dependencyOutput = & $Flutter --suppress-analytics --no-version-check pub deps --json
    if ($LASTEXITCODE -ne 0) { throw "Cannot inventory dependencies." }
    $dependencyOutput | Out-File (Join-Path $artifacts "dependencies.json") -Encoding utf8
    & $Python packaging/windows/component_inventory.py --package-config ".dart_tool/package_config.json" --dependencies (Join-Path $artifacts "dependencies.json") --lockfile "pubspec.lock" --sdk (Join-Path $artifacts "sdk.json") --fonts "assets/fonts" --notices (Join-Path $bundle "data/flutter_assets/NOTICES.Z") --output (Join-Path $artifacts "inventory")
    if ($LASTEXITCODE -ne 0) { throw "Dependency/license inventory failed." }
    & $Python packaging/windows/prepare_bundle.py --bundle-dir $bundle --output-dir (Join-Path $artifacts "support")
    if ($LASTEXITCODE -ne 0) { throw "Bundle validation failed." }
    & $Python packaging/windows/payload_archive.py --bundle $bundle --output (Join-Path $artifacts "Ryhze-0.6.0+3-unsigned.zip") --version "0.6.0+3" --source-revision $sourceRevision --lockfile "pubspec.lock"
    if ($LASTEXITCODE -ne 0) { throw "Payload packaging failed." }
    if ($Nsis) {
        # The installed top-level makensis is a launcher. Wait for its process
        # tree before any caller may record pipeline completion or package hashes.
        $nsisArguments = @(
            ('/DBUNDLE_DIR="' + $bundle + '"'),
            '/DAPP_VERSION=0.6.0',
            ('/DPACKAGE_SUPPORT_DIR="' + $artifacts + '/support"'),
            ('/DOUTPUT_FILE="' + $artifacts + '/Ryhze-0.6.0-unsigned-setup.exe"'),
            'packaging/windows/ryhze-setup.nsi'
        )
        $nsisProcess = Start-Process -FilePath $Nsis -ArgumentList $nsisArguments -WorkingDirectory $repo -WindowStyle Hidden -Wait -PassThru -RedirectStandardOutput (Join-Path $artifacts 'nsis.stdout.log') -RedirectStandardError (Join-Path $artifacts 'nsis.stderr.log')
        if ($nsisProcess.ExitCode -ne 0) { throw 'NSIS process tree completed with a failure.' }
        if (-not (Test-Path -LiteralPath (Join-Path $artifacts 'Ryhze-0.6.0-unsigned-setup.exe') -PathType Leaf)) { throw 'NSIS did not produce its expected installer.' }
    }
    Write-Output "Unsigned candidate only. Native acceptance, publisher signing and release approval remain required."
} finally { Pop-Location }

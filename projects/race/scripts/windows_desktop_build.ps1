param([string]$Dependencies='C:\Projects\RACE\Workspace\Desktop-Dependencies-20261010', [switch]$Test, [switch]$Analyze,
  [switch]$Profile, [ValidatePattern('^[a-z0-9-]+$')][string]$ProfileLabel='baseline', [string]$EvidenceDirectory='')
$ErrorActionPreference='Stop'
$repo=Split-Path $PSScriptRoot -Parent

$run='windows-desktop-build'
if($Profile){$run='windows-desktop-profile-'+$ProfileLabel}
if($Analyze){$run='windows-desktop-analysis'}
$python=(Get-Command python).Source
$evidenceArgs=@('-B',(Join-Path $PSScriptRoot 'output_paths.py'),'evidence','--run',$run)
if($EvidenceDirectory){$evidenceArgs+=@('--output',$EvidenceDirectory)}
$evidence=(& $python @evidenceArgs | Out-String).Trim()
if($LASTEXITCODE -ne 0 -or -not $evidence){throw 'Unable to allocate fresh build evidence'}
$previousEvidenceRun=$env:RACE_EVIDENCE_RUN_DIR
$attemptStarted=(Get-Date).ToUniversalTime().ToString('o')
$attemptExitCode=$null
$attemptError=$null
$attemptStage='setup'
$transcriptStarted=$false
try {
  Start-Transcript -LiteralPath (Join-Path $evidence 'build.log') -NoClobber | Out-Null
  $transcriptStarted=$true
  $env:RACE_EVIDENCE_RUN_DIR=$evidence
  $attemptStage='dependencies'
  $verified=(& (Join-Path $PSScriptRoot 'desktop_dependencies.ps1') -Root $Dependencies) | ConvertFrom-Json
  if(-not $verified.verified){throw 'Desktop dependencies not verified'}
  $vswhere=Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
  $vs=& $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
  if(-not $vs){throw 'Visual Studio C++ tools not found'}
  $pythonDir=Split-Path $python
  $gitDir=Split-Path (Get-Command git).Source
  $env:Path="$env:SystemRoot\System32;$env:SystemRoot;$pythonDir;$gitDir"
  & (Join-Path $vs 'Common7\Tools\Launch-VsDevShell.ps1') -Arch amd64 -HostArch amd64 -SkipAutomaticLocation | Out-Null
  $cmake=Join-Path $vs 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
  $ctest=Join-Path (Split-Path $cmake) 'ctest.exe'
  $ninja=Join-Path $vs 'Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe'
  $build=Join-Path $repo 'out\windows-desktop'
  if($Profile){$build=Join-Path $repo ('out\windows-desktop-profile-'+$ProfileLabel)}
  if($Analyze){$build=Join-Path $repo 'out\windows-desktop-analysis'}
  $extra=@()
  if($Analyze){$extra=@('-DCMAKE_CXX_FLAGS=/analyze')}
  if($Profile){$extra+=@('-DRACE_DESKTOP_ALLOCATION_PROFILE=ON')}else{$extra+=@('-DRACE_DESKTOP_ALLOCATION_PROFILE=OFF')}
  $attemptStage='configure'
  & $cmake --preset windows-release -S $repo -B $build "-DCMAKE_MAKE_PROGRAM=$ninja" -DRACE_DESKTOP=ON "-DQt6_DIR=$($verified.Qt6_DIR)" "-DSDL3_DIR=$($verified.SDL3_DIR)" "-DRACE_DXC=$($verified.dxc)" @extra
  if($LASTEXITCODE -ne 0){$attemptExitCode=$LASTEXITCODE;exit $attemptExitCode}
  $attemptStage='build'
  if($Analyze){
    & $cmake --build $build --parallel 4 --target race_desktop_scene render_windows race_editor race_runtime_desktop
  }else{
    & $cmake --build $build --parallel 4
  }
  if($LASTEXITCODE -ne 0){$attemptExitCode=$LASTEXITCODE;exit $attemptExitCode}
  $attemptStage='deploy-dependencies'
  & $verified.windeployqt --release --no-translations --no-system-d3d-compiler --no-system-dxc-compiler --no-opengl-sw --no-compiler-runtime (Join-Path $build 'race_editor.exe')
  if($LASTEXITCODE -ne 0){$attemptExitCode=$LASTEXITCODE;exit $attemptExitCode}
  Copy-Item -LiteralPath $verified.sdlRuntime -Destination (Join-Path $build 'SDL3.dll')
  if($Test){$attemptStage='test';& $ctest --test-dir $build --output-on-failure --output-log (Join-Path $evidence 'ctest.log') --output-junit (Join-Path $evidence 'ctest.xml');$attemptExitCode=$LASTEXITCODE;exit $attemptExitCode}
  $attemptExitCode=0
} catch {
  $attemptExitCode=1
  $attemptError=$_.Exception.Message
  throw
} finally {
  $env:RACE_EVIDENCE_RUN_DIR=$previousEvidenceRun
  if($transcriptStarted){Stop-Transcript | Out-Null}
  $attempt=[ordered]@{schema=1;run=$run;started_utc=$attemptStarted;finished_utc=(Get-Date).ToUniversalTime().ToString('o');stage=$attemptStage;exit_code=$attemptExitCode;passed=($null -ne $attemptExitCode -and $attemptExitCode -eq 0);error=$attemptError;build_directory=$build;evidence_directory=$evidence}
  $attempt | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $evidence 'attempt.json') -Encoding UTF8
}

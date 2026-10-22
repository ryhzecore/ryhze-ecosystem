param([ValidateSet('Debug','Release')][string]$Configuration='Debug', [switch]$ConfigureOnly, [switch]$Test, [switch]$Analyze, [string]$EvidenceDirectory='')
$ErrorActionPreference='Stop'
$repo=Split-Path $PSScriptRoot -Parent

$run='windows-'+$Configuration.ToLowerInvariant()+'-build'
if($Analyze){$run='windows-analysis'}
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
  $vswhere=Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
  $vs=& $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
  if(-not $vs){throw 'Visual Studio C++ tools not found'}
  # Use a bounded build-process PATH; inherited quoted PATH entries can break
  # Visual Studio's batch bootstrap. This does not alter the user's stored PATH.
  $pythonDir=Split-Path $python
  $gitDir=Split-Path (Get-Command git).Source
  $env:Path="$env:SystemRoot\System32;$env:SystemRoot;$pythonDir;$gitDir"
  & (Join-Path $vs 'Common7\Tools\Launch-VsDevShell.ps1') -Arch amd64 -HostArch amd64 -SkipAutomaticLocation | Out-Null
  $cmake=Join-Path $vs 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
  $ctest=Join-Path (Split-Path $cmake) 'ctest.exe'
  $ninja=Join-Path $vs 'Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe'
  $preset='windows-'+$Configuration.ToLowerInvariant()
  $build=Join-Path $repo ('out\'+$preset)
  $attemptStage='configure'
  if($Analyze){
    $build=Join-Path $repo 'out\windows-analysis'
    & $cmake --preset $preset -S $repo -B $build "-DCMAKE_MAKE_PROGRAM=$ninja" -DCMAKE_CXX_FLAGS=/analyze
  }else{
    & $cmake --preset $preset -S $repo "-DCMAKE_MAKE_PROGRAM=$ninja"
  }
  if($LASTEXITCODE -ne 0){$attemptExitCode=$LASTEXITCODE;exit $attemptExitCode}
  if($ConfigureOnly){$attemptExitCode=0;exit 0}
  $attemptStage='build'
  if($Analyze){
    & $cmake --build $build --parallel 4 --target platform_windows race_atmosphere race_audio race_vfx
  }else{
    & $cmake --build $build --parallel 4
  }
  if($LASTEXITCODE -ne 0){$attemptExitCode=$LASTEXITCODE;exit $attemptExitCode}
  if($Test){$attemptStage='test';& $ctest --test-dir $build --output-on-failure --output-log (Join-Path $evidence 'ctest.log') --output-junit (Join-Path $evidence 'ctest.xml'); $attemptExitCode=$LASTEXITCODE;exit $attemptExitCode}
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

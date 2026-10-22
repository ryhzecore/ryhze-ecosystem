param(
  [Parameter(Mandatory=$true)][string]$Build,
  [string]$OutputDirectory='',
  [string]$Dependencies='C:\Projects\RACE\Workspace\Desktop-Dependencies-20261010',
  [string]$EvidenceDirectory=''
)
$ErrorActionPreference='Stop'
$repo=Split-Path $PSScriptRoot -Parent
$python=(Get-Command python).Source
$run='package-desktop'
$evidenceArgs=@('-B',(Join-Path $PSScriptRoot 'output_paths.py'),'evidence','--run',$run)
if($EvidenceDirectory){$evidenceArgs+=@('--output',$EvidenceDirectory)}
$evidence=(& $python @evidenceArgs | Out-String).Trim()
if($LASTEXITCODE -ne 0 -or -not $evidence){throw 'Unable to allocate fresh packaging evidence'}
$attemptStarted=(Get-Date).ToUniversalTime().ToString('o')
$attemptExitCode=$null
$attemptNativeExitCode=$null
$attemptError=$null
$attemptStage='setup'
$transcriptStarted=$false
try {
  Start-Transcript -LiteralPath (Join-Path $evidence 'package.log') -NoClobber | Out-Null
  $transcriptStarted=$true
$Build=[IO.Path]::GetFullPath($Build).TrimEnd('\','/')
function Assert-Regular([string]$Path) {
  $item=Get-Item -LiteralPath $Path
  if($item.PSIsContainer -or ($item.Attributes -band [IO.FileAttributes]::ReparsePoint)) {throw "Expected regular file: $Path"}
}
function Write-PortableZip([string]$SourceDirectory,[string]$Destination,[string[]]$RelativeFiles) {
  $names=New-Object 'System.Collections.Generic.HashSet[string]' ([StringComparer]::Ordinal)
  foreach($relative in $RelativeFiles) {
    if([string]::IsNullOrEmpty($relative) -or $relative.Contains('\') -or $relative.Contains(':') -or
       @($relative.Split('/') | Where-Object {$_ -eq '' -or $_ -eq '.' -or $_ -eq '..'}).Count -ne 0 -or
       -not $names.Add($relative)) {throw "Noncanonical or duplicate ZIP entry: $relative"}
  }
  # CreateFromDirectory on Windows PowerShell/.NET Framework can emit '\'
  # names. Write the manifest's exact portable '/' names, and never overwrite.
  $zipStream=[IO.File]::Open($Destination,[IO.FileMode]::CreateNew,[IO.FileAccess]::Write,[IO.FileShare]::None)
  try {
    $writer=[IO.Compression.ZipArchive]::new($zipStream,[IO.Compression.ZipArchiveMode]::Create,$true)
    try {
      foreach($relative in $RelativeFiles) {
        $entry=$writer.CreateEntry($relative,[IO.Compression.CompressionLevel]::Optimal)
        $inputStream=[IO.File]::OpenRead((Join-Path $SourceDirectory $relative))
        try {
          $entryStream=$entry.Open()
          try {$inputStream.CopyTo($entryStream)} finally {$entryStream.Dispose()}
        } finally {$inputStream.Dispose()}
      }
    } finally {$writer.Dispose()}
  } finally {$zipStream.Dispose()}
}
function Run-Native([string]$File,[string[]]$Arguments,[int]$Timeout=60000) {
  $info=New-Object System.Diagnostics.ProcessStartInfo
  $info.FileName=$File
  $quoted=foreach($argument in $Arguments) {
    if($argument -match '["\r\n]' -or $argument.EndsWith('\')) {throw 'Unsupported native argument quoting'}
    '"'+$argument+'"'
  }
  $info.Arguments=$quoted -join ' '
  $info.UseShellExecute=$false; $info.CreateNoWindow=$true
  $info.RedirectStandardOutput=$true; $info.RedirectStandardError=$true
  $process=[Diagnostics.Process]::Start($info)
  $stdout=$null; $stderr=$null
  try {
    $stdout=$process.StandardOutput.ReadToEndAsync(); $stderr=$process.StandardError.ReadToEndAsync()
    $timedOut=-not $process.WaitForExit($Timeout)
    if($timedOut) {
      try {$process.Kill()} catch {if(-not $process.HasExited){throw}}
    }
    # Kill requests termination; it does not establish process/reader quiescence.
    $process.WaitForExit()
    [Threading.Tasks.Task]::WaitAll([Threading.Tasks.Task[]]@($stdout,$stderr))
    $result=[ordered]@{file=$File;arguments=$Arguments;pid=$process.Id;exitCode=$process.ExitCode;stdout=$stdout.Result;stderr=$stderr.Result;quiescent=$true}
    if($timedOut -or $process.ExitCode -ne 0) {
      $message=if($timedOut){"Timed out: $File"}else{"Command failed ($($process.ExitCode)): $File`n$($result.stderr)"}
      $failure=[InvalidOperationException]::new($message); $failure.Data['processReceipt']=$result
      throw $failure
    }
    return $result
  } finally {
    try {
      if(-not $process.HasExited) {try {$process.Kill()} catch {if(-not $process.HasExited){Write-Warning 'Termination failed; retaining process ownership until exit'}}}
      $process.WaitForExit()
      $readers=@($stdout,$stderr) | Where-Object {$null -ne $_}
      if($readers.Count) {[Threading.Tasks.Task]::WaitAll([Threading.Tasks.Task[]]$readers)}
    } finally {$process.Dispose()}
  }
}
$attemptStage='dependencies'
$dependencyPaths=(& (Join-Path $PSScriptRoot 'desktop_dependencies.ps1') -Mode Verify -Root $Dependencies | Out-String | ConvertFrom-Json)
$attemptStage='build-identity'
$cache=Get-Content -LiteralPath (Join-Path $Build 'CMakeCache.txt') -Raw
if($cache -notmatch '(?m)^CMAKE_BUILD_TYPE:STRING=Release\r?$' -or $cache -notmatch '(?m)^RACE_DESKTOP:BOOL=ON\r?$') {throw 'A configured Release RACE_DESKTOP=ON build is required'}
if($cache -match '(?m)^RACE_DESKTOP_ALLOCATION_PROFILE:BOOL=ON\r?$') {throw 'Allocation-instrumented builds are measurement-only; package the regular desktop build'}
$inputs=@('race_mesh_import_worker.exe','race_editor.exe','race_runtime_desktop.exe','race_mesh_tool.exe','race_surface_tool.exe','race_content_tool.exe','race_package_tool.exe','shaders/viewport.vs.dxil','shaders/viewport.ps.dxil','shaders/viewport-bindings.json','shaders/viewport-bindings.rmanifest','shaders/viewport-physical.ps.dxil','shaders/viewport-output.vs.dxil','shaders/viewport-output.ps.dxil','shaders/viewport-light-output.json','shaders/viewport-light-output.rmanifest')
foreach($path in $inputs) {Assert-Regular (Join-Path $Build $path)}
$sourceDigest=(Run-Native $python @((Join-Path $PSScriptRoot 'source_manifest.py'),'--identity')).stdout.Trim()
$identity=(Run-Native (Join-Path $Build 'race_scene_headless.exe') @('--identity')).stdout | ConvertFrom-Json
if($identity.configuration -ne 'Release' -or $identity.authored_input_sha256 -ne $sourceDigest) {throw 'Shared build identity differs from current authored source; rebuild before packaging'}
# The authored digest distinguishes packages built from local source changes.
$candidateName=$identity.source_commit+'-'+$sourceDigest.Substring(0,12)
$candidateArgs=@('-B',(Join-Path $PSScriptRoot 'output_paths.py'),'candidate','--name',$candidateName,'--leaf','desktop-preview')
if($OutputDirectory){$candidateArgs+=@('--output',[IO.Path]::GetFullPath($OutputDirectory).TrimEnd('\','/'))}
$OutputDirectory=(Run-Native $python $candidateArgs).stdout.Trim()
if(-not $OutputDirectory){throw 'Unable to select fresh candidate package output'}
$zip=$OutputDirectory+'.zip'
$receipt=Join-Path $evidence 'package-validation.json'
foreach($path in @($OutputDirectory,$zip,$receipt)) {
  if(Test-Path -LiteralPath $path) {throw "Output exists; preserve it and choose a new name: $path"}
}
$ancestor=Split-Path $OutputDirectory -Parent
while($ancestor) {
  if((Test-Path -LiteralPath $ancestor) -and ((Get-Item -LiteralPath $ancestor).Attributes -band [IO.FileAttributes]::ReparsePoint)) {throw "Reparse output ancestor rejected: $ancestor"}
  $ancestor=Split-Path $ancestor -Parent
}
$attemptStage='package-preflight'
$vs='C:\Program Files\Microsoft Visual Studio\18\Community'
$crt=Join-Path $vs 'VC/Redist/MSVC/14.51.36231/x64/Microsoft.VC145.CRT'
$crtNames=@('concrt140.dll','msvcp140.dll','msvcp140_1.dll','msvcp140_2.dll','msvcp140_atomic_wait.dll','msvcp140_codecvt_ids.dll','vcruntime140.dll','vcruntime140_1.dll','vcruntime140_threads.dll')
foreach($name in $crtNames) {
  Assert-Regular (Join-Path $crt $name)
  if((Get-AuthenticodeSignature -LiteralPath (Join-Path $crt $name)).Status -ne 'Valid') {throw "Invalid installed Microsoft CRT signature: $name"}
}
$attemptStage='package-payload'
# New-Item fails if another packager wins the same output name. No output is removed.
[IO.Directory]::CreateDirectory((Split-Path $OutputDirectory -Parent)) | Out-Null
New-Item -ItemType Directory -Path $OutputDirectory -ErrorAction Stop | Out-Null
New-Item -ItemType Directory -Path (Join-Path $OutputDirectory 'shaders') | Out-Null
foreach($path in $inputs) {Copy-Item -LiteralPath (Join-Path $Build $path) -Destination (Join-Path $OutputDirectory $path)}
$deployArgs=@('--dry-run','--release','--no-translations','--no-compiler-runtime','--no-opengl-sw','--no-system-d3d-compiler','--no-system-dxc-compiler','--no-quick-import','--no-ffmpeg','--skip-plugin-types','generic,imageformats,styles,networkinformation,tls','--exclude-plugins','qpdf','--dir',$OutputDirectory,(Join-Path $Build 'race_editor.exe'))
$deployment=Run-Native $dependencyPaths.windeployqt $deployArgs
$deployment | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $OutputDirectory 'qt-deployment.json') -Encoding UTF8
# Qt 6.10.1 lacks --no-system-icu. Its dry-run plan can list the Windows
# system ICU DLL. Copy only these pinned Qt binaries; never copy OS binaries.
New-Item -ItemType Directory -Path (Join-Path $OutputDirectory 'platforms') -Force | Out-Null
foreach($name in @('Qt6Core.dll','Qt6Gui.dll','Qt6Widgets.dll')) {
  Copy-Item -LiteralPath (Join-Path $dependencyPaths.qtPrefix ('bin/'+$name)) -Destination (Join-Path $OutputDirectory $name)
}
Copy-Item -LiteralPath $dependencyPaths.qtPlatformPlugin -Destination (Join-Path $OutputDirectory 'platforms/qwindows.dll')
"[Paths]`nPrefix=.`nPlugins=.`n" | Set-Content -LiteralPath (Join-Path $OutputDirectory 'qt.conf') -Encoding ASCII
foreach($name in @('Qt6Core.dll','Qt6Gui.dll','Qt6Widgets.dll','platforms/qwindows.dll')) {Assert-Regular (Join-Path $OutputDirectory $name)}
Copy-Item -LiteralPath $dependencyPaths.sdlRuntime -Destination (Join-Path $OutputDirectory 'SDL3.dll')
$crtSources=@(foreach($name in $crtNames) {
  $source=Join-Path $crt $name
  Copy-Item -LiteralPath $source -Destination (Join-Path $OutputDirectory $name)
  [ordered]@{file=$name;source=$source;sha256=(Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash.ToLowerInvariant();signature='Valid'}
})
# Verify the actual packaged native import closure against app-local DLLs and
# this Windows 11 host. API-set names are resolved by Windows, not loose files.
$dumpbin=Join-Path $vs 'VC/Tools/MSVC/14.51.36231/bin/Hostx64/x64/dumpbin.exe'
$importChecks=@(foreach($binary in (Get-ChildItem -LiteralPath $OutputDirectory -Recurse -File | Where-Object {$_.Extension -in '.dll','.exe'})) {
  $dump=Run-Native $dumpbin @('/nologo','/dependents',$binary.FullName)
  $imports=@([regex]::Matches($dump.stdout,'(?im)^\s+([A-Za-z0-9_.-]+\.dll)\s*$') | ForEach-Object {$_.Groups[1].Value})
  foreach($import in $imports) {
    if($import -match '^(api|ext)-ms-win-') {continue}
    if(Test-Path -LiteralPath (Join-Path $OutputDirectory $import) -PathType Leaf) {continue}
    if(Test-Path -LiteralPath (Join-Path (Join-Path $env:SystemRoot 'System32') $import) -PathType Leaf) {continue}
    throw "Unresolved packaged dependency $import in $($binary.Name)"
  }
  [ordered]@{file=$binary.FullName.Substring($OutputDirectory.Length+1).Replace('\','/');imports=$imports;exitCode=$dump.exitCode}
})
$importChecks | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $OutputDirectory 'native-imports.json') -Encoding UTF8
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'desktop_dependencies.lock.json') -Destination (Join-Path $OutputDirectory 'desktop_dependencies.lock.json')
$licences=Join-Path $Dependencies 'licences'
if(Get-ChildItem -LiteralPath $licences -Recurse -Force | Where-Object {$_.Attributes -band [IO.FileAttributes]::ReparsePoint}) {throw 'Dependency license reparse entry rejected'}
Copy-Item -LiteralPath $licences -Destination (Join-Path $OutputDirectory 'licences') -Recurse
Copy-Item -LiteralPath (Join-Path $vs 'Licenses/1033/Redist.txt') -Destination (Join-Path $OutputDirectory 'licences/MSVC-Redist.txt')
Copy-Item -LiteralPath (Join-Path $vs 'Licenses/1033/ThirdPartyNotices.txt') -Destination (Join-Path $OutputDirectory 'licences/MSVC-ThirdPartyNotices.txt')
# The Qt editor embeds these unmodified fonts. Keep their original notices
# directly readable beside the package's other retained dependency notices.
$fontSources=Join-Path $repo 'apps/resources/fonts'
$fontLicences=Join-Path $OutputDirectory 'licences/fonts'
New-Item -ItemType Directory -Path $fontLicences -ErrorAction Stop | Out-Null
foreach($name in @('Inter-LICENSE.txt','SpaceGrotesk-LICENSE.txt','SOURCES.txt')) {
  $source=Join-Path $fontSources $name
  Assert-Regular $source
  Copy-Item -LiteralPath $source -Destination (Join-Path $fontLicences $name)
}
@'
RACE ENGINE - local Windows x64 desktop developer preview

Open race_editor.exe for the Qt editor. Add/edit/delete boxes or cooked native
meshes, save the authoring scene, and use Play/Stop for an isolated sample scene.
The D3D12 raster viewport supports opaque geometry, a grid and selection edges.
Properties > Lighting opts a Scene into PhysicalDiffuseV1: a white directional light in
lux, an environment in cd/m^2, exposure in stops and a reference luminance shade an FP16
scene-linear target that one exposure, ReinhardRGB-v1 and sRGB map to the SDR display.
Scenes without these settings keep the legacy unitless lighting exactly.
This is a local preview; full rendering, physics, audio, VFX and scripting remain
outside this increment. OBJ import supports positions, optional UVs and triangle faces.
Native opaque diffuse materials support one RGBA8 sRGB base-color texture.
Offline textures accept binary P6 PPM (max value 255) or uncompressed BMP24
(INFOHEADER40, no palette, either row orientation), each <=1024x1024.
The texture command accepts image.ppm or image.bmp; runtime reads native products.
Legacy materials use linear filtering/clamp at mip zero. Explicit schema2 materials
select complete cooked mip chains and isotropic trilinear filtering; full PBR remains open.
The validation host is Windows 11 x64 with a D3D12-capable graphics device.
Other Windows versions and graphics devices have not been qualified.

From this directory:
  .\race_editor.exe
  .\race_editor.exe --scene C:\path\authoring.scene.json
  .\race_runtime_desktop.exe --scene C:\path\runtime.rscene
The runtime also starts with its default box when --scene is omitted.
For meshes, create existing source/imported/assets directories, then run:
  .\race_mesh_tool.exe import C:\source shape.obj C:\imported 00000000000000010000000000000001
  .\race_mesh_tool.exe cook C:\imported <imported-revision>.rasset C:\assets
Open the assets folder in the editor's File menu and use Add mesh.
For a texture and material, use existing source/imported/assets directories:
  .\race_surface_tool.exe texture C:\source image.ppm C:\imported 00000000000000020000000000000001 srgb
  .\race_surface_tool.exe cook C:\imported <imported-texture-revision>.rasset C:\assets
Author material.json with schema 1, model "opaque-diffuse", base_color [1,1,1],
and base_color_texture {"asset":"<texture ID>","revision":"<cooked texture revision>"}
(or null for a constant color), then:
  .\race_surface_tool.exe material C:\source material.json C:\imported 00000000000000030000000000000001
  .\race_surface_tool.exe cook C:\imported <imported-material-revision>.rasset C:\assets
For explicit distance filtering, replace the texture cook command with:
  .\race_surface_tool.exe cook-mips C:\imported <imported-texture-revision>.rasset C:\assets

Use material source schema2, keep model/base_color/base_color_texture, and add
sampling "linear-clamp-mips". Pin the cooked complete-chain texture revision.
The ordinary cook command and schema1 materials preserve their mip-zero behavior.

Explicit surface recipes pin importer, encoding, cooking profile and complete
material texture keys. Runtime applications still load only cooked products:
  .\race_surface_tool.exe create-recipe C:\recipes color.json 00000000000000020000000000000001 image.bmp bmp-srgb-mips
  .\race_surface_tool.exe recipe C:\recipes color.json C:\source C:\assets
  .\race_content_tool.exe create C:\recipes project.raceproject texture 00000000000000020000000000000001 color.json <recipe-revision>
  .\race_content_tool.exe inspect C:\recipes project.raceproject
Project indexes use only native .raceproject files. Recipes remain JSON.
The typed catalog and editor Content panel support mesh, texture and material
recipe authoring, background import/checkpoint capture and explicit adoption.
For independent scalar data, import with linear and use cook-scalar-mips, or select
ppm-scalar-mips / bmp-scalar-mips in a texture recipe. These schema3 native products
average each RGB scalar independently. They are not normal maps and cannot bind
to the current diffuse base-color slot. Data capture preserves valid color choices.
Then import/cook the material using the same commands above.
Select a UV mesh or box, then use Assign material in Properties. The operation
prepares its pinned texture on a joined worker and supports Undo/Redo. The Clear
button restores the object color. Old meshes without UVs accept constant colors.
Both apps accept --assets C:\assets together with --scene. Scene references pin
all 32 revision bytes and the stable asset ID. Keep the revision-named cooked
.rasset products beside one another in that assets folder. Missing or mismatched
products reject Scene loading. Mesh selection and framing use mesh bounds.

To package one cooked Scene and its exact mesh/material/texture closure, copy the cooked .rscene
into the assets root, choose an existing output root and a stable Scene ID:
  .\race_package_tool.exe pack C:\assets scene.rscene C:\content 00000000000000020000000000000001
  .\race_package_tool.exe validate C:\content <package-hash>.rpak
  .\race_editor.exe --package C:\content\<package-hash>.rpak
  .\race_runtime_desktop.exe --package C:\content\<package-hash>.rpak
The editor also opens packages from File > Open content package on its retained
file worker. Cancel or a failed load preserves the current Scene. Saving creates
separate authoring JSON; packages are immutable and never edited in place.
Package input cannot be combined with --scene or --assets, and has no loose-file
fallback. New releases the selected content source and prepared dependencies.
This profile stores at most 128 entries, 16 MiB per chunk and 64 MiB per package.
It loads a resident immutable snapshot; range streaming and compression are not
implemented. Hashes identify bytes and do not authenticate their author.
The editor follows Scene above Objects, with Properties on the right. Ctrl+1
focuses the Scene viewport; Q/W/E/R select Select/Move/Rotate/Scale. Drag the
local RGB arrows, rings or square endpoints. Release commits one edit; Ctrl+Z
undoes it, and Escape cancels the preview. F frames the selection.
For editor camera flight, hold the right mouse button: WASD moves horizontally
and Q/E moves down/up. The SDL runtime uses these movement keys directly in its
focused viewport. Focus loss cancels held movement; press again after returning.
Right-drag orbits and the wheel zooms in either app.

Editable authoring source uses .scene.json; cooked RACESCN1 binary uses .rscene.
The editor saves source JSON and exports the cooked runtime representation.
The standalone SDL runtime accepts either representation. Keep .scene.json for
authoring. Stop Play before applying authoring changes.

Qt and SDL are dynamically linked. The Qt Windows platform plugin and installed official
MSVC x64 runtime DLLs are app-local; no installer or global changes are included.
Windows system DLLs, including ICU, are supplied by the operating system.
Precompiled shaders are in shaders/. DXC is not needed to run this package.
Retained dependency license references and installed MSVC notices are in licences/.
Inter and Space Grotesk are embedded in race_editor.exe under the SIL Open Font
License 1.1; their original copyright/license notices and provenance are in licences/fonts/.
This local development package is not publication, signing, complete third-party
redistribution/source compliance, compatibility qualification, or release approval.

manifest.json records every payload file except itself; the adjacent validation
receipt records the ZIP and manifest hashes. Packaging verifies bytes, not visible
UI or GPU correctness. Record application smoke evidence separately.
'@ | Set-Content -LiteralPath (Join-Path $OutputDirectory 'README.txt') -Encoding UTF8
$freshDigest=(Run-Native $python @((Join-Path $PSScriptRoot 'source_manifest.py'),'--identity')).stdout.Trim()
if($freshDigest -ne $sourceDigest) {throw 'Source changed during packaging; preserve this attempt and rebuild before a fresh package'}
$files=@(Get-ChildItem -LiteralPath $OutputDirectory -Recurse -File | Sort-Object FullName | ForEach-Object {
  $relative=$_.FullName.Substring($OutputDirectory.Length+1).Replace('\','/')
  if($relative -match '(?i)(^|/)(qt6pdf.*|qpdf.*|windeployqt.*|dxc\.exe|dxcompiler\.dll|dxil\.dll|icu[^/]*\.dll)$') {throw "Unexpected compiler, PDF tool or Windows ICU payload: $relative"}
  [ordered]@{path=$relative;bytes=$_.Length;sha256=(Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()}
})
$manifest=[ordered]@{schema=1;kind='race-windows-desktop-developer-preview';sharedBuildIdentity=$identity;authoredInputSha256=$sourceDigest;dependencyLock='desktop_dependencies.lock.json';crtSources=$crtSources;files=$files;qualification='Local package byte verification; visible UI/GPU smoke is separate'}
$manifestPath=Join-Path $OutputDirectory 'manifest.json'
$manifest | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath $manifestPath -Encoding UTF8
Add-Type -AssemblyName System.IO.Compression,System.IO.Compression.FileSystem
$attemptStage='package-zip'
Write-PortableZip $OutputDirectory $zip (@($files | ForEach-Object {$_.path})+@('manifest.json'))
$attemptStage='package-validation'
$archive=[IO.Compression.ZipFile]::OpenRead($zip)
try {
  if($archive.Entries.Count -ne ($files.Count+1)) {throw 'ZIP inventory count mismatch'}
  foreach($entry in $archive.Entries) {
    if($entry.FullName.Contains('\')) {throw "Nonportable ZIP entry: $($entry.FullName)"}
  }
  foreach($record in $files) {
    $entry=$archive.GetEntry($record.path)
    if(-not $entry -or $entry.Length -ne $record.bytes) {throw "ZIP size mismatch: $($record.path)"}
    $stream=$entry.Open(); $hash=[Security.Cryptography.SHA256]::Create()
    try {$digest=[BitConverter]::ToString($hash.ComputeHash($stream)).Replace('-','').ToLowerInvariant()}
    finally {$stream.Dispose(); $hash.Dispose()}
    if($digest -ne $record.sha256) {throw "ZIP hash mismatch: $($record.path)"}
  }
  $entry=$archive.GetEntry('manifest.json'); $stream=$entry.Open(); $hash=[Security.Cryptography.SHA256]::Create()
  try {$manifestDigest=[BitConverter]::ToString($hash.ComputeHash($stream)).Replace('-','').ToLowerInvariant()}
  finally {$stream.Dispose(); $hash.Dispose()}
  if($manifestDigest -ne (Get-FileHash -LiteralPath $manifestPath -Algorithm SHA256).Hash) {throw 'ZIP manifest differs'}
} finally {$archive.Dispose()}
$result=[ordered]@{package=$zip;directory=$OutputDirectory;bytes=(Get-Item -LiteralPath $zip).Length;sha256=(Get-FileHash -LiteralPath $zip -Algorithm SHA256).Hash.ToLowerInvariant();manifestSha256=$manifestDigest;payloadFiles=$files.Count;zipByteVerification='passed';qtDeploymentExitCode=$deployment.exitCode;qualification='Local developer preview; application smoke results recorded separately'}
$result | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath $receipt -Encoding UTF8
$result | ConvertTo-Json -Depth 4
  $attemptExitCode=0
} catch {
  $attemptExitCode=1
  $attemptError=$_.Exception.Message
  $nativeFailure=$_.Exception.Data['processReceipt']
  if($null -ne $nativeFailure){$attemptNativeExitCode=$nativeFailure.exitCode}
  throw
} finally {
  if($transcriptStarted){Stop-Transcript | Out-Null}
  $attempt=[ordered]@{schema=1;run=$run;started_utc=$attemptStarted;finished_utc=(Get-Date).ToUniversalTime().ToString('o');stage=$attemptStage;exit_code=$attemptExitCode;native_exit_code=$attemptNativeExitCode;passed=($null -ne $attemptExitCode -and $attemptExitCode -eq 0);error=$attemptError;build_directory=$Build;package_directory=$OutputDirectory;evidence_directory=$evidence}
  $attempt | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $evidence 'attempt.json') -Encoding UTF8
}

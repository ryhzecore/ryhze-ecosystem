param(
  [ValidateSet('Verify','Intake')][string]$Mode='Verify',
  [string]$Root='C:\Projects\RACE\Workspace\Desktop-Dependencies-20261010'
)
$ErrorActionPreference='Stop'
$lock=Get-Content -LiteralPath (Join-Path $PSScriptRoot 'desktop_dependencies.lock.json') -Raw | ConvertFrom-Json
$Root=[IO.Path]::GetFullPath($Root).TrimEnd('\','/')
$workspace=[IO.Path]::GetFullPath('C:\Projects\RACE\Workspace').TrimEnd('\')+'\'
if(-not $Root.StartsWith($workspace,[StringComparison]::OrdinalIgnoreCase)) {throw 'Dependency root must be a child of C:\Projects\RACE\Workspace'}

function Assert-Hash([string]$Path,[string]$Expected) {
  if(-not (Test-Path -LiteralPath $Path -PathType Leaf)) {throw "Missing dependency file: $Path"}
  if((Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash -ne $Expected) {throw "SHA256 mismatch: $Path"}
}
function Receive-Pinned([string]$Url,[string]$Path,[string]$Hash) {
  [IO.Directory]::CreateDirectory((Split-Path $Path -Parent)) | Out-Null
  if(Test-Path -LiteralPath $Path) {Assert-Hash $Path $Hash; return}
  $partial=$Path+'.partial-'+[Guid]::NewGuid().ToString('N')
  Invoke-WebRequest -UseBasicParsing -Uri $Url -OutFile $partial
  Assert-Hash $partial $Hash
  Move-Item -LiteralPath $partial -Destination $Path
}

# Intake creates a new dependency tree only. Existing trees are verified, never
# repaired or overlaid; choose a fresh directory to repeat a complete intake.
if($Mode -eq 'Intake' -and -not (Test-Path -LiteralPath $Root)) {
  $ancestor=Split-Path $Root -Parent
  while($ancestor) {
    if((Test-Path -LiteralPath $ancestor) -and ((Get-Item -LiteralPath $ancestor).Attributes -band [IO.FileAttributes]::ReparsePoint)) {throw "Reparse ancestor rejected: $ancestor"}
    $ancestor=Split-Path $ancestor -Parent
  }
  [IO.Directory]::CreateDirectory($Root) | Out-Null
  $tar=Join-Path $env:SystemRoot 'System32\tar.exe'
  foreach($package in $lock.packages) {
    $archive=Join-Path $Root ('downloads/'+$package.file)
    Receive-Pinned $package.url $archive $package.sha256
    $entries=@(& $tar -tf $archive)
    if($LASTEXITCODE -ne 0) {throw "Cannot list archive: $archive"}
    foreach($entry in $entries) {
      if($entry -match '(^[/\\]|^[A-Za-z]:|(^|[/\\])\.\.([/\\]|$))') {throw "Unsafe archive member: $entry"}
    }
    $destination=Join-Path $Root $package.extractTo
    [IO.Directory]::CreateDirectory($destination) | Out-Null
    & $tar -xf $archive -C $destination
    if($LASTEXITCODE -ne 0) {throw "Archive extraction failed: $archive"}
  }
  foreach($licence in $lock.licences) {Receive-Pinned $licence.url (Join-Path $Root $licence.path) $licence.sha256}
  Copy-Item -LiteralPath (Join-Path $Root 'SDL3-3.4.18/LICENSE.txt') -Destination (Join-Path $Root 'licences/SDL3-3.4.18-LICENSE.txt')
  foreach($installed in $lock.installed) {
    Assert-Hash $installed.path $installed.sha256
    if($installed.path.EndsWith('.rtf')) {Copy-Item -LiteralPath $installed.path -Destination (Join-Path $Root ('licences/'+(Split-Path $installed.path -Leaf)))}
  }
}

foreach($package in $lock.packages) {Assert-Hash (Join-Path $Root ('downloads/'+$package.file)) $package.sha256}
foreach($artifact in $lock.artifacts) {Assert-Hash (Join-Path $Root $artifact.path) $artifact.sha256}
foreach($licence in $lock.licences) {Assert-Hash (Join-Path $Root $licence.path) $licence.sha256}
foreach($installed in $lock.installed) {Assert-Hash $installed.path $installed.sha256}
Assert-Hash (Join-Path $Root 'licences/SDL3-3.4.18-LICENSE.txt') ($lock.artifacts | Where-Object {$_.path -eq 'SDL3-3.4.18/LICENSE.txt'}).sha256
foreach($installed in $lock.installed) {
  if($installed.path.EndsWith('.rtf')) {Assert-Hash (Join-Path $Root ('licences/'+(Split-Path $installed.path -Leaf))) $installed.sha256}
}
$qt=Join-Path $Root 'Qt/6.10.1/msvc2022_64'
$sdl=Join-Path $Root 'SDL3-3.4.18'
[ordered]@{
  verified=$true
  qtVersion=$lock.qtVersion
  qtPrefix=$qt
  Qt6_DIR=(Join-Path $qt 'lib/cmake/Qt6')
  qtTargets=@('Qt6::Core','Qt6::Gui','Qt6::Widgets')
  windeployqt=(Join-Path $qt 'bin/windeployqt.exe')
  qtPlatformPlugin=(Join-Path $qt 'plugins/platforms/qwindows.dll')
  sdlVersion=$lock.sdlVersion
  sdlPrefix=$sdl
  SDL3_DIR=(Join-Path $sdl 'cmake')
  sdlTarget='SDL3::SDL3'
  sdlInclude=(Join-Path $sdl 'include')
  sdlImportLibrary=(Join-Path $sdl 'lib/x64/SDL3.lib')
  sdlRuntime=(Join-Path $sdl 'lib/x64/SDL3.dll')
  dxc=$lock.installed[0].path
  dxcVersion=$lock.dxcVersion
  hlslFlags=@('-HV','2021')
  configuration='Release /MD; upstream Qt Debug iterator ABI is not qualified against RACE Debug IDL0'
} | ConvertTo-Json -Depth 4

# Explicit, isolated native installer exercise. Never launches Ryhze or a URI.
# Run only after the owner authorizes installation on this Windows test account.
param(
    [Parameter(Mandatory=$true)][string]$Installer,
    [Parameter(Mandatory=$true)][ValidatePattern('^[a-fA-F0-9]{64}$')][string]$InstallerSha256,
    [Parameter(Mandatory=$true)][string]$EvidenceDirectory,
    [ValidateSet('clean', 'preserve-owner')][string]$Scenario = 'clean',
    [string]$RecoveryPython,
    [string]$RecoveryManifest,
    [ValidatePattern('^[a-fA-F0-9]{64}$')][string]$RecoveryManifestSha256,
    [switch]$Execute
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
if ([Environment]::OSVersion.Platform -ne [PlatformID]::Win32NT) { throw 'Windows is required.' }
$installerPath = (Resolve-Path -LiteralPath $Installer).Path
$evidence = [IO.Path]::GetFullPath($EvidenceDirectory)
$installRoot = Join-Path $env:LOCALAPPDATA 'Programs\Ryhze-Installer-Acceptance'
$startMenu = [Environment]::GetFolderPath('Programs')
$shortcut = Join-Path $startMenu 'Ryhze Installer Acceptance.lnk'
$testProduct = 'Software\Ryhze\InstallerAcceptance'
$testUninstall = 'Software\Microsoft\Windows\CurrentVersion\Uninstall\RyhzeInstallerAcceptance'
$testProtocol = 'Software\Classes\ryhze-installer-acceptance'
$productionKeys = @('Software\Ryhze\Entertainment', 'Software\Microsoft\Windows\CurrentVersion\Uninstall\RyhzeEntertainment', 'Software\Classes\ryhze')

function Assert-RegularPath([string]$Path) {
    $current = [IO.Path]::GetFullPath($Path)
    while ($current) {
        if (Test-Path -LiteralPath $current) {
            $item = Get-Item -LiteralPath $current -Force
            if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw "Linked fixture path refused: $current" }
        }
        $parent = [IO.Directory]::GetParent($current)
        $current = if ($null -eq $parent) { '' } else { $parent.FullName }
    }
}
function Read-Key([Microsoft.Win32.RegistryKey]$Hive, [string]$Path) {
    $key = $Hive.OpenSubKey($Path, $false)
    if ($null -eq $key) { return $null }
    try {
        $values = [ordered]@{}
        foreach ($name in @($key.GetValueNames() | Sort-Object)) {
            $values[$name] = @{ kind = [string]$key.GetValueKind($name); value = $key.GetValue($name, $null, [Microsoft.Win32.RegistryValueOptions]::DoNotExpandEnvironmentNames) }
        }
        $children = [ordered]@{}
        foreach ($name in @($key.GetSubKeyNames() | Sort-Object)) { $children[$name] = Read-Key $key $name }
        return @{ values = $values; children = $children }
    } finally { $key.Dispose() }
}
function Snapshot-Production {
    $snapshot = [ordered]@{}
    foreach ($view in @([Microsoft.Win32.RegistryView]::Registry32, [Microsoft.Win32.RegistryView]::Registry64)) {
        $hive = [Microsoft.Win32.RegistryKey]::OpenBaseKey([Microsoft.Win32.RegistryHive]::CurrentUser, $view)
        try { foreach ($key in $productionKeys) { $snapshot["$view/$key"] = Read-Key $hive $key } }
        finally { $hive.Dispose() }
    }
    $productionShortcut = Join-Path $startMenu 'Ryhze Entertainment.lnk'
    $snapshot['shortcut'] = if (Test-Path -LiteralPath $productionShortcut) { (Get-FileHash -LiteralPath $productionShortcut -Algorithm SHA256).Hash } else { $null }
    return ($snapshot | ConvertTo-Json -Depth 24 -Compress)
}
function Read-Value([string]$Key, [string]$Name) {
    $opened = [Microsoft.Win32.Registry]::CurrentUser.OpenSubKey($Key, $false)
    if ($null -eq $opened) { return $null }
    try { return $opened.GetValue($Name, $null, [Microsoft.Win32.RegistryValueOptions]::DoNotExpandEnvironmentNames) }
    finally { $opened.Dispose() }
}
function Run-Installer([string]$File, [string]$Arguments, [string]$Stage, [bool]$Success) {
    $trace = Join-Path $evidence "$Stage.trace.log"
    $stderr = Join-Path $evidence "$Stage.stderr.log"
    if ((Test-Path -LiteralPath $trace) -or (Test-Path -LiteralPath $stderr)) { throw 'Diagnostic output must be new.' }
    $process = Start-Process -FilePath $File -ArgumentList $Arguments -WindowStyle Hidden -PassThru -RedirectStandardOutput $trace -RedirectStandardError $stderr
    if (-not $process.WaitForExit(120000)) { throw "$Stage exceeded 120 seconds; process retained for owner inspection." }
    $code = $process.ExitCode
    $diagnostics = @()
    if (Test-Path -LiteralPath $trace) {
        if ((Get-Item -LiteralPath $trace).Length -gt 65536) { throw 'Installer trace exceeded its bounded contract.' }
        $diagnostics = @(Get-Content -LiteralPath $trace | Where-Object { $_ -match '^stage=[a-z0-9-]+;code=-?[0-9]+$' })
    }
    $script:stages.Add(@{stage=$Stage; exitCode=$code; diagnostics=$diagnostics; traceFile=[IO.Path]::GetFileName($trace); stderrFile=[IO.Path]::GetFileName($stderr)})
    if (($code -eq 0) -ne $Success) { throw "Unexpected $Stage result: $code" }
}
function Run-Recovery([string]$Stage, [string[]]$Arguments) {
    $stdout = Join-Path $evidence "$Stage.json"
    $stderr = Join-Path $evidence "$Stage.stderr.log"
    if ((Test-Path -LiteralPath $stdout) -or (Test-Path -LiteralPath $stderr)) { throw 'Recovery evidence must be new.' }
    # All arguments are explicit data. Reject quotes/newlines/trailing separators
    # rather than interpolating them into a shell or an ambiguous native command.
    $quoted = foreach ($argument in (@('-B', $recoveryHelper) + $Arguments)) {
        if ($argument -match '["\r\n]' -or $argument.EndsWith('\')) { throw 'Unsupported recovery argument syntax.' }
        '"' + $argument + '"'
    }
    $process = Start-Process -FilePath $recoveryPythonPath -ArgumentList ($quoted -join ' ') -WindowStyle Hidden -PassThru -RedirectStandardOutput $stdout -RedirectStandardError $stderr
    if (-not $process.WaitForExit(240000)) { throw "$Stage exceeded 240 seconds; process and fixture retained." }
    $script:stages.Add(@{stage=$Stage; exitCode=$process.ExitCode; outputFile=[IO.Path]::GetFileName($stdout); stderrFile=[IO.Path]::GetFileName($stderr)})
    if ($process.ExitCode -ne 0) { throw "Recovery gate failed: $Stage" }
    if ((Get-Item -LiteralPath $stdout).Length -gt 12582912) { throw 'Recovery output exceeded its budget.' }
    $result = Get-Content -LiteralPath $stdout -Raw | ConvertFrom-Json
    if ($result.activation_allowed -ne $false) { throw 'Recovery unexpectedly admitted activation.' }
    return $result
}
function Assert-RecoveryPayloads([object]$Manifest, [string[]]$Roots) {
    $records = @()
    foreach ($record in $Manifest.files) {
        $verified = @()
        foreach ($root in $Roots) {
            $path = [IO.Path]::GetFullPath((Join-Path $root ([string]$record.path).Replace('/', '\')))
            $prefix = [IO.Path]::GetFullPath($root).TrimEnd('\') + '\'
            if (-not $path.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)) { throw 'Manifest path escaped its payload root.' }
            Assert-RegularPath $path
            $file = Get-Item -LiteralPath $path -Force
            if ($file.PSIsContainer -or $file.Length -ne $record.bytes -or (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash -ine $record.sha256) { throw 'Installed/retained/restored payload mismatch.' }
            $verified += $true
        }
        $records += @{path=$record.path; bytes=$record.bytes; sha256=$record.sha256; copiesVerified=$verified.Count}
    }
    $records | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $evidence 'recovery-payload-comparison.json') -Encoding UTF8
    $script:stages.Add(@{stage='recovery-byte-comparison'; files=$records.Count; copies=3; matched=$true; activationAllowed=$false})
}

Assert-RegularPath $installerPath
Assert-RegularPath $evidence
Assert-RegularPath $installRoot
if ((Get-FileHash -LiteralPath $installerPath -Algorithm SHA256).Hash -ine $InstallerSha256) { throw 'Installer hash mismatch.' }
if ([Diagnostics.FileVersionInfo]::GetVersionInfo($installerPath).ProductName -cne 'Ryhze Installer Acceptance') { throw 'Only an acceptance-identity installer is permitted.' }
foreach ($key in @($testProduct, $testUninstall, $testProtocol)) {
    if ($null -ne (Read-Key ([Microsoft.Win32.Registry]::CurrentUser) $key)) { throw "Existing acceptance state retained: $key" }
}
if ($null -ne (Read-Key ([Microsoft.Win32.Registry]::ClassesRoot) 'ryhze-installer-acceptance')) { throw 'A scheme already owns the acceptance identity.' }
if ((Test-Path -LiteralPath $installRoot) -or (Test-Path -LiteralPath $shortcut) -or (Test-Path -LiteralPath $evidence)) { throw 'Use a new evidence directory and an unused acceptance installation identity.' }
$recoveryInputs = @(@($RecoveryPython, $RecoveryManifest, $RecoveryManifestSha256) | Where-Object { -not [string]::IsNullOrWhiteSpace($_) })
if ($recoveryInputs.Count -ne 0 -and $recoveryInputs.Count -ne 3) { throw 'Supply RecoveryPython, RecoveryManifest and RecoveryManifestSha256 together.' }
$recoveryEnabled = $recoveryInputs.Count -eq 3
$recoveryPlan = $null
if ($recoveryEnabled) {
    $recoveryPythonPath = (Resolve-Path -LiteralPath $RecoveryPython).Path
    $recoveryManifestPath = (Resolve-Path -LiteralPath $RecoveryManifest).Path
    $recoveryHelper = Join-Path $PSScriptRoot 'installer_recovery.py'
    foreach ($path in @($recoveryPythonPath, $recoveryManifestPath, $recoveryHelper)) { Assert-RegularPath $path; if ((Get-Item -LiteralPath $path).PSIsContainer) { throw 'A regular recovery input file is required.' } }
    if ((Get-Item -LiteralPath $recoveryManifestPath).Length -gt 8388608 -or (Get-FileHash -LiteralPath $recoveryManifestPath -Algorithm SHA256).Hash -ine $RecoveryManifestSha256) { throw 'Recovery manifest size or identity mismatch.' }
    $manifestHash = $RecoveryManifestSha256.ToLowerInvariant()
    # Read-only Python schema validation occurs BEFORE evidence/installation mutation.
    # -B prevents interpreter cache files from being written during this preflight.
    $validation = & $recoveryPythonPath -B $recoveryHelper validate-manifest --manifest $recoveryManifestPath --manifest-sha256 $manifestHash
    if ($LASTEXITCODE -ne 0) { throw 'Recovery manifest validation failed before installation.' }
    $validated = ($validation -join "`n") | ConvertFrom-Json
    if ($validated.version -cne '0.6.0+3' -or $validated.activation_allowed -ne $false -or $validated.manifest_sha256 -cne $manifestHash) { throw 'Recovery manifest is not the selected inert candidate.' }
    if ((Get-FileHash -LiteralPath $recoveryManifestPath -Algorithm SHA256).Hash -ine $manifestHash) { throw 'Manifest changed during preflight.' }
    $recoveryManifestData = Get-Content -LiteralPath $recoveryManifestPath -Raw | ConvertFrom-Json
    # The current app writes schema2 (lib/data/store_schema.dart); this gate
    # asserts only the current-generation range2..2, not older-store migration.
    $recoveryPlan = @{python=$recoveryPythonPath; pythonSha256=(Get-FileHash -LiteralPath $recoveryPythonPath -Algorithm SHA256).Hash; helperSha256=(Get-FileHash -LiteralPath $recoveryHelper -Algorithm SHA256).Hash; manifest=$recoveryManifestPath; manifestSha256=$manifestHash; sourceRevision=$validated.source_revision; files=$validated.file_count; payloadBytes=$validated.payload_bytes; mode='repair'; version='0.6.0+3'; readerSchemaMin=2; readerSchemaMax=2; storeSchema=2; activationAllowed=$false}
}
$plan = @{ installer=$installerPath; sha256=$InstallerSha256; installRoot=$installRoot; shortcut=$shortcut; scenario=$Scenario; automaticAppLaunch=$false; productionMutation=$false; recovery=$recoveryPlan }
if (-not $Execute) { $plan | ConvertTo-Json; return }
[IO.Directory]::CreateDirectory($evidence) | Out-Null
$before = Snapshot-Production
[IO.File]::WriteAllText((Join-Path $evidence 'production-before.json'), $before)
$stages = [Collections.Generic.List[object]]::new()
$completed = $false
$failure = $null
try {
    # NSIS requires /D as the last argument, without quotes around its value.
    Run-Installer $installerPath "/S /D=$installRoot" 'fresh-install' $true
    $marker = Join-Path $installRoot '.ryhze-install.ini'
    $markerBytes = [IO.File]::ReadAllBytes($marker)
    $owner = Read-Value $testProduct 'RyhzeOwnerId'
    if (-not $owner -or (Read-Value $testProduct 'InstallDir') -cne $installRoot) { throw 'Product ownership mismatch.' }
    if ((Read-Value $testProtocol 'RyhzeOwnerId') -cne $owner -or (Read-Value $testProtocol 'RyhzeInstallRoot') -cne $installRoot) { throw 'Protocol ownership mismatch.' }
    $expectedCommand = '"' + $installRoot + '\app\ryhze.exe" "%1"'
    if ((Read-Value "$testProtocol\shell\open\command" '') -cne $expectedCommand) { throw 'Protocol command is not an exact quoted executable and single URI.' }
    $shell = New-Object -ComObject WScript.Shell
    $link = $shell.CreateShortcut($shortcut)
    if ($link.TargetPath -cne "$installRoot\app\ryhze.exe" -or $link.Arguments -cne '' -or $link.Description -cne "RyhzeInstallerAcceptance|$owner") { throw 'Shortcut ownership mismatch.' }
    $folderShell = New-Object -ComObject Shell.Application
    $item = $folderShell.NameSpace($startMenu).ParseName('Ryhze Installer Acceptance.lnk')
    if ($item.ExtendedProperty('System.AppUserModel.ID') -cne 'Ryhze.InstallerAcceptance' -or [string]$item.ExtendedProperty('System.AppUserModel.ToastActivatorCLSID') -ine '{9e4cdc8d-efc1-46fd-8402-b714695407d1}') { throw 'Shortcut notification identity mismatch.' }
    Run-Installer $installerPath "/S /D=$installRoot" 'repeat-refused' $false
    if ([Convert]::ToBase64String([IO.File]::ReadAllBytes($marker)) -cne [Convert]::ToBase64String($markerBytes)) { throw 'Repeated install changed ownership.' }
    if ($recoveryEnabled) {
        $retained = Join-Path $evidence 'recovery-retained'
        $restored = Join-Path $evidence 'recovery-restored'
        $common = @('--root', $installRoot, '--product', 'RyhzeInstallerAcceptance', '--manifest', $recoveryManifestPath, '--manifest-sha256', $manifestHash, '--target-version', '0.6.0+3', '--mode', 'repair')
        $inspection = Run-Recovery 'recovery-inspect' (@('inspect') + $common)
        if ($inspection.owner_id -cne $owner -or $inspection.mode -cne 'repair' -or $inspection.version -cne '0.6.0+3') { throw 'Live recovery observation mismatched the installed owner/version.' }
        $retention = Run-Recovery 'recovery-retain' (@('retain') + $common + @('--output', $retained, '--reader-schema-min', '2', '--reader-schema-max', '2'))
        if ($retention.recovery_sha256 -notmatch '^[a-f0-9]{64}$') { throw 'Invalid recovery completion identity.' }
        $restoration = Run-Recovery 'recovery-restore' @('restore', '--recovery', $retained, '--recovery-sha256', $retention.recovery_sha256, '--output', $restored, '--store-schema', '2')
        if ($restoration.existing_installation_changed -ne $false -or $restoration.user_data_changed -ne $false -or $restoration.store_schema -ne 2) { throw 'Recovery crossed its inert staging boundary.' }
        Assert-RecoveryPayloads $recoveryManifestData @((Join-Path $installRoot 'app'), (Join-Path $retained 'app'), (Join-Path $restored 'app'))
        if ([Convert]::ToBase64String([IO.File]::ReadAllBytes($marker)) -cne [Convert]::ToBase64String($markerBytes)) { throw 'Recovery changed installed ownership.' }
    }
    $sentinel = Join-Path $installRoot 'app\unknown-user-file.txt'
    [IO.File]::WriteAllText($sentinel, 'Synthetic acceptance sentinel: preserve me.')
    if ($Scenario -eq 'preserve-owner') {
        $key = [Microsoft.Win32.Registry]::CurrentUser.OpenSubKey("$testProtocol\shell\open\command", $true)
        try { $key.SetValue('', '"C:\SyntheticSuccessor\never-executed.exe" "%1"', [Microsoft.Win32.RegistryValueKind]::String) } finally { $key.Dispose() }
        $key = [Microsoft.Win32.Registry]::CurrentUser.OpenSubKey($testProduct, $true)
        try { $key.SetValue('UnknownFixtureValue', 'preserve', [Microsoft.Win32.RegistryValueKind]::String) } finally { $key.Dispose() }
    }
    # Execute a retained copy with _?= to keep the original installation root and
    # avoid the asynchronous NSIS self-copy relaunch obscuring process completion.
    $uninstaller = Join-Path $evidence 'acceptance-uninstall.exe'
    [IO.File]::Copy((Join-Path $installRoot 'Uninstall.exe'), $uninstaller, $false)
    $locked = [IO.File]::Open("$installRoot\app\ryhze.exe", [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read)
    try { Run-Installer $uninstaller "/S _?=$installRoot" 'locked-removal-refused' $false }
    finally { $locked.Dispose() }
    if (-not (Test-Path -LiteralPath $marker) -or -not (Test-Path -LiteralPath "$installRoot\Uninstall.exe")) { throw 'Failed uninstall discarded recovery authority.' }
    Run-Installer $uninstaller "/S _?=$installRoot" 'uninstall-retry' $true
    if ([IO.File]::ReadAllText($sentinel) -cne 'Synthetic acceptance sentinel: preserve me.') { throw 'Unknown file was not preserved.' }
    if ((Test-Path -LiteralPath $shortcut) -or (Test-Path -LiteralPath $marker) -or (Test-Path -LiteralPath "$installRoot\Uninstall.exe")) { throw 'Owned installation metadata remained.' }
    if ($null -ne (Read-Key ([Microsoft.Win32.Registry]::CurrentUser) $testUninstall)) { throw 'Owned uninstall registration remained.' }
    if ($Scenario -eq 'clean') {
        foreach ($key in @($testProduct, $testProtocol)) {
            if ($null -ne (Read-Key ([Microsoft.Win32.Registry]::CurrentUser) $key)) { throw "Owned registration remained: $key" }
        }
    } else {
        if ((Read-Value "$testProtocol\shell\open\command" '') -cne '"C:\SyntheticSuccessor\never-executed.exe" "%1"' -or (Read-Value $testProduct 'UnknownFixtureValue') -cne 'preserve') { throw 'Reassigned or unknown registration was removed.' }
    }
    $completed = $true
} catch { $failure = $_.Exception.Message; throw }
finally {
    $after = Snapshot-Production
    [IO.File]::WriteAllText((Join-Path $evidence 'production-after.json'), $after)
    $unchanged = $before -ceq $after
    @{ plan=$plan; stages=$stages; completed=$completed; failure=$failure; productionUnchanged=$unchanged; retainedFixtureRoot=$installRoot; noAppOrUriExecuted=$true } | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath (Join-Path $evidence 'result.json') -Encoding UTF8
    if (-not $unchanged) { throw 'Production registration/shortcut changed during acceptance; inspect evidence before continuing.' }
}

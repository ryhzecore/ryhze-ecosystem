# Stage the reviewed x64 MSVC import closure; never downloads or installs a runtime.
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$BundleDirectory,
    [Parameter(Mandatory = $true)][string]$MsvcRuntimeDirectory,
    [string]$ProvenancePath = "",
    [switch]$ValidateOnly
)
$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

function Get-CheckedPath([string]$Path, [bool]$Directory) {
    if (-not [IO.Path]::IsPathRooted($Path)) { throw "An absolute local path is required: $Path" }
    $full = [IO.Path]::GetFullPath($Path).TrimEnd('\')
    if ($full -notmatch '^[A-Za-z]:\\' -or $full.Substring(2).Contains(':')) {
        throw "Only local drive paths without alternate streams are supported: $Path"
    }
    $item = Get-Item -LiteralPath $full -Force
    if ([bool]$item.PSIsContainer -ne $Directory) { throw "Unexpected file type: $full" }
    $cursor = $item
    while ($null -ne $cursor) {
        if (($cursor.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
            throw "Reparse points are not accepted: $($cursor.FullName)"
        }
        $cursor = if ($cursor -is [IO.DirectoryInfo]) { $cursor.Parent } else { $cursor.Directory }
    }
    return $item.FullName.TrimEnd('\')
}

function Get-Sha256([string]$Path) {
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}

function Get-MicrosoftSignature([string]$Path) {
    $signature = Get-AuthenticodeSignature -LiteralPath $Path
    if ($signature.Status -ne [System.Management.Automation.SignatureStatus]::Valid -or
        $null -eq $signature.SignerCertificate) { throw "A valid trusted Microsoft signature is required: $Path" }
    $certificate = $signature.SignerCertificate
    $publisher = $certificate.GetNameInfo([Security.Cryptography.X509Certificates.X509NameType]::SimpleName, $false)
    if ($publisher -notin @('Microsoft Corporation', 'Microsoft Windows Software Compatibility Publisher') -or
        $certificate.Subject -notmatch '(^|,\s*)O=Microsoft Corporation(,|$)') {
        throw "Unexpected signed publisher: $Path"
    }
    return [ordered]@{ status = 'Valid'; publisher = $publisher; subject = $certificate.Subject; thumbprint = $certificate.Thumbprint }
}

function Get-X64Pe([string]$Path) {
    $stream = [IO.File]::Open($Path, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read)
    $reader = New-Object IO.BinaryReader($stream)
    try {
        if ($stream.Length -lt 64 -or $reader.ReadUInt16() -ne 0x5A4D) { throw "Missing PE DOS header: $Path" }
        $stream.Position = 0x3C
        $offset = $reader.ReadInt32()
        if ($offset -lt 64 -or $offset -gt $stream.Length - 28) { throw "Invalid PE header offset: $Path" }
        $stream.Position = $offset
        if ($reader.ReadUInt32() -ne 0x00004550 -or $reader.ReadUInt16() -ne 0x8664) { throw "An x64 PE is required: $Path" }
        $stream.Position = $offset + 24
        if ($reader.ReadUInt16() -ne 0x20B) { throw "A PE32+ image is required: $Path" }
        $major = $reader.ReadByte(); $minor = $reader.ReadByte()
        return [ordered]@{ machine = 'x64'; linkerMajor = [int]$major; linkerMinor = [int]$minor }
    } finally { $reader.Dispose() }
}

function Get-Imports([string]$Path) {
    $output = @(& $script:dumpbin /nologo /dependents $Path 2>&1)
    if ($LASTEXITCODE -ne 0) { throw "dumpbin could not inspect dependencies: $Path" }
    $names = @($output | ForEach-Object {
        if ([string]$_ -match '^\s+([A-Za-z0-9_.+-]+\.dll)\s*$') { $Matches[1].ToLowerInvariant() }
    } | Sort-Object -Unique)
    if ($names.Count -eq 0) { throw "No imports could be verified: $Path" }
    return ,$names
}

if ([Environment]::OSVersion.Platform -ne [PlatformID]::Win32NT) { throw "A Windows host is required." }
$bundle = Get-CheckedPath $BundleDirectory $true
$runtime = Get-CheckedPath $MsvcRuntimeDirectory $true
if ($runtime -notmatch '^(?<vc>.+\\VC)\\Redist\\MSVC\\(?<toolset>14\.\d+\.\d+)\\x64\\Microsoft\.VC\d+\.CRT$') {
    throw "Use an installed release x64 Microsoft.VC*.CRT directory under Visual Studio VC\Redist\MSVC."
}
$vcRoot = $Matches['vc']; $toolset = [version]$Matches['toolset']
$script:dumpbin = Get-CheckedPath (Join-Path $vcRoot "Tools\MSVC\$toolset\bin\Hostx64\x64\dumpbin.exe") $false
$dumpbinSignature = Get-MicrosoftSignature $script:dumpbin
$dumpbinHash = Get-Sha256 $script:dumpbin
$required = @('msvcp140.dll', 'vcruntime140.dll', 'vcruntime140_1.dll')
$heldStreams = New-Object 'System.Collections.Generic.List[System.IDisposable]'
$runtimeRecords = @(); $binaryRecords = @(); $staged = @()
try {
    # Keep verified input files open without write/delete sharing until staging ends.
    foreach ($name in $required) {
        $path = Get-CheckedPath (Join-Path $runtime $name) $false
        $heldStreams.Add([IO.File]::Open($path, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read))
        $before = Get-Sha256 $path
        $signature = Get-MicrosoftSignature $path
        $pe = Get-X64Pe $path
        $version = [Diagnostics.FileVersionInfo]::GetVersionInfo($path)
        $fileVersion = [version]$version.FileVersion
        if ($version.OriginalFilename -ine $name -or $version.CompanyName -ne 'Microsoft Corporation' -or
            $fileVersion.Major -ne $toolset.Major -or $fileVersion.Minor -ne $toolset.Minor -or
            $fileVersion -lt $toolset) { throw "Runtime source identity/version mismatch: $path" }
        $imports = Get-Imports $path
        if ((Get-Sha256 $path) -ne $before) { throw "Runtime source changed during validation: $path" }
        $runtimeRecords += [ordered]@{ name = $name; source = $path; bytes = (Get-Item -LiteralPath $path).Length;
            sha256 = $before; fileVersion = $version.FileVersion; originalFilename = $version.OriginalFilename;
            signature = $signature; pe = $pe; imports = $imports }
    }
    if (@($runtimeRecords | ForEach-Object { $_.fileVersion } | Sort-Object -Unique).Count -ne 1) {
        throw "The runtime source mixes servicing versions."
    }
    foreach ($name in @('ryhze.exe', 'flutter_windows.dll')) { $null = Get-CheckedPath (Join-Path $bundle $name) $false }
    $nativeFiles = @(Get-ChildItem -LiteralPath $bundle -Force | Where-Object {
        -not $_.PSIsContainer -and $_.Extension -in @('.exe', '.dll') -and $_.Name.ToLowerInvariant() -notin $required
    } | Sort-Object Name)
    foreach ($file in $nativeFiles) {
        $path = Get-CheckedPath $file.FullName $false
        $heldStreams.Add([IO.File]::Open($path, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read))
        $pe = Get-X64Pe $path
        if ($file.Name -ieq 'ryhze.exe' -and ($pe.linkerMajor -ne $toolset.Major -or $pe.linkerMinor -ne $toolset.Minor)) {
            throw "The runtime source does not match the application's MSVC linker family."
        }
        $binaryRecords += [ordered]@{ name = $file.Name; sha256 = Get-Sha256 $path; pe = $pe; imports = Get-Imports $path }
    }
    $allImports = @(@($binaryRecords) + @($runtimeRecords) | ForEach-Object { $_.imports } | Sort-Object -Unique)
    $msvcImports = @($allImports | Where-Object { $_ -match '^(msvcp|msvcr|vcruntime|vccorlib|concrt|vcamp|vcomp|mfc|mfcm)\d' })
    if (@(Compare-Object $required $msvcImports).Count -ne 0) {
        throw "The native import closure differs from the reviewed three-DLL MSVC runtime; review additional or missing runtime dependencies."
    }
    # Reject conflicting existing payload files before writing any new runtime DLL.
    foreach ($record in $runtimeRecords) {
        $destination = Join-Path $bundle $record.name
        # Get-Item also observes a dangling link, which Test-Path can miss.
        if ($null -ne (Get-Item -LiteralPath $destination -Force -ErrorAction SilentlyContinue)) {
            $destination = Get-CheckedPath $destination $false
            $heldStreams.Add([IO.File]::Open($destination, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read))
            if ((Get-Sha256 $destination) -ne $record.sha256) { throw "Existing runtime differs from the verified source: $destination" }
            $record['destinationState'] = 'already-identical'
        } else { $record['destinationState'] = 'missing' }
    }
    $receipt = [ordered]@{ schema = 1; status = 'validated-only'; sourceDirectory = $runtime; bundleDirectory = $bundle;
        toolsetDirectoryVersion = $toolset.ToString(); dumpbin = [ordered]@{ path = $script:dumpbin; sha256 = $dumpbinHash; signature = $dumpbinSignature };
        payloadBinaries = $binaryRecords; runtimeFiles = $runtimeRecords;
        scope = 'Reviewed static/delay import closure only. Windows system/UCRT imports are not copied. Dynamic LoadLibrary use and clean-machine execution remain acceptance checks.' }
    if ($ValidateOnly) { return [pscustomobject]$receipt }
    foreach ($process in @(Get-Process -Name ryhze -ErrorAction SilentlyContinue)) {
        if (-not $process.Path) { throw "Cannot establish whether an existing Ryhze process uses this bundle." }
        if ([IO.Path]::GetFullPath($process.Path) -ieq (Join-Path $bundle 'ryhze.exe')) { throw "Close this bundle's Ryhze process before staging runtime files." }
    }
    if (-not $ProvenancePath -or -not [IO.Path]::IsPathRooted($ProvenancePath)) { throw "An absolute, new ProvenancePath outside the bundle is required." }
    $receiptPath = [IO.Path]::GetFullPath($ProvenancePath)
    $null = Get-CheckedPath ([IO.Path]::GetDirectoryName($receiptPath)) $true
    if ($receiptPath.StartsWith($bundle + '\', [StringComparison]::OrdinalIgnoreCase) -or
        $receiptPath.StartsWith($runtime + '\', [StringComparison]::OrdinalIgnoreCase) -or
        (Test-Path -LiteralPath $receiptPath) -or [IO.Path]::GetFileName($receiptPath).Contains(':')) {
        throw "Provenance must use a new external file; existing files are preserved."
    }
    foreach ($record in $runtimeRecords) {
        if ($record.destinationState -eq 'missing') {
            $destination = Join-Path $bundle $record.name
            $null = Get-CheckedPath $bundle $true
            $inputStream = [IO.File]::Open($record.source, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read)
            try {
                $outputStream = [IO.File]::Open($destination, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::Read)
                try { $inputStream.CopyTo($outputStream); $outputStream.Flush($true) } finally { $outputStream.Dispose() }
            } finally { $inputStream.Dispose() }
            $staged += $destination
            $null = Get-CheckedPath $destination $false
            $heldStreams.Add([IO.File]::Open($destination, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read))
            if ((Get-Sha256 $destination) -ne $record.sha256) { throw "Staged runtime hash mismatch: $destination" }
            $null = Get-MicrosoftSignature $destination
            $record['destinationState'] = 'staged-and-verified'
        }
    }
    $receipt['status'] = 'staged-and-verified'
    $receipt['completedUtc'] = [DateTime]::UtcNow.ToString('o')
    $json = $receipt | ConvertTo-Json -Depth 10
    $receiptStream = [IO.File]::Open($receiptPath, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::None)
    try {
        $bytes = (New-Object Text.UTF8Encoding($false)).GetBytes($json + [Environment]::NewLine)
        $receiptStream.Write($bytes, 0, $bytes.Length); $receiptStream.Flush($true)
    } finally { $receiptStream.Dispose() }
    Write-Output "Verified app-local MSVC runtime; provenance: $receiptPath"
} catch {
    if ($staged.Count -gt 0) { Write-Warning "Staging failed; preserve the partial payload for diagnosis. No installer or archive may be produced from this attempt." }
    throw
} finally {
    foreach ($stream in $heldStreams) { $stream.Dispose() }
}

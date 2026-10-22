param(
    [Parameter(Mandatory=$true)][string]$EvidenceDir,
    [Parameter(Mandatory=$true)][string]$Label,
    [Parameter(Mandatory=$true)][string]$WorkingDirectory,
    [Parameter(Mandatory=$true)][string]$Supervisor,
    [Parameter(Mandatory=$true)][string[]]$Command,
    [int]$Seconds=300
)
$ErrorActionPreference='Stop'
if($Label -notmatch '^[a-z0-9-]+$') { throw 'Invalid run label' }
$axeonRun = [IO.Path]::GetFullPath($EvidenceDir)
if($axeonRun -notmatch '^C:\\Projects\\Axeon\\Evidence\\[0-9]{4}-[0-9]{2}-[0-9]{2}\\[a-z0-9-]+\\attempt-[0-9]{2,}$') { throw 'Use a reserved dated Evidence attempt' }
if(-not (Test-Path -LiteralPath $axeonRun -PathType Container)) {throw 'Reserve the attempt before running'}
$axeonOS = Get-CimInstance Win32_OperatingSystem
$axeonDisk = Get-PSDrive C
$axeonReady = $axeonOS.FreePhysicalMemory -ge 2097152 -and $axeonOS.FreeVirtualMemory -ge 4194304 -and $axeonDisk.Free -ge 4GB
$axeonGuard = [ordered]@{utc=[DateTime]::UtcNow.ToString('o'); ready=$axeonReady; free_physical_kib=$axeonOS.FreePhysicalMemory; minimum_physical_kib=2097152; free_commit_kib=$axeonOS.FreeVirtualMemory; minimum_commit_kib=4194304; free_disk_bytes=$axeonDisk.Free; minimum_disk_bytes=4GB; command=$Command; limits='private job: 2GiB aggregate commit,180s CPU,256 processes; jobs1,testthreads1'; seconds=$Seconds}
$axeonGuardPath=Join-Path $axeonRun ($Label+'-host-guard.json')
$axeonStream=[IO.File]::Open($axeonGuardPath,[IO.FileMode]::CreateNew)
try { $axeonBytes=[Text.UTF8Encoding]::new($false).GetBytes(($axeonGuard|ConvertTo-Json -Depth 5)); $axeonStream.Write($axeonBytes,0,$axeonBytes.Length) } finally {$axeonStream.Dispose()}
if(-not $axeonReady) { $axeonGuard | ConvertTo-Json; exit 75 }
$env:CARGO_BUILD_JOBS='1'; $env:RUST_TEST_THREADS='1'; $env:CARGO_INCREMENTAL='0'
$axeonCommandPath=Join-Path $axeonRun ($Label+'-command.json')
$axeonCommandStream=[IO.File]::Open($axeonCommandPath,[IO.FileMode]::CreateNew)
try { $axeonBytes=[Text.UTF8Encoding]::new($false).GetBytes((ConvertTo-Json -InputObject @($Command))); $axeonCommandStream.Write($axeonBytes,0,$axeonBytes.Length) } finally {$axeonCommandStream.Dispose()}
& python -B (Join-Path $PSScriptRoot 'record-windows-command.py') --output-dir (Join-Path $axeonRun $Label) --supervisor $Supervisor --cwd $WorkingDirectory --seconds $Seconds --command-file $axeonCommandPath
exit $LASTEXITCODE

param([Parameter(Mandatory=$true)][string]$Child)
$ErrorActionPreference='Stop'
# Extract only the local helper definition; never execute package publication.
$parseErrors=$null; $tokens=$null
$ast=[Management.Automation.Language.Parser]::ParseFile((Join-Path $PSScriptRoot 'package_desktop.ps1'),[ref]$tokens,[ref]$parseErrors)
if($parseErrors.Count){throw 'Package script parsing failed'}
$definition=$ast.Find({param($node) $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq 'Run-Native'},$true)
if($null -eq $definition){throw 'Native helper missing'}
. ([ScriptBlock]::Create($definition.Extent.Text))
$normal=Run-Native $Child @('exit','0')
if($normal.exitCode -ne 0 -or -not $normal.quiescent){throw 'Natural completion failed'}
try {Run-Native $Child @('abrupt') | Out-Null; throw 'Abrupt completion unexpectedly succeeded'} catch {
  $receipt=$_.Exception.Data['processReceipt']
  if($null -eq $receipt -or -not $receipt.quiescent -or $receipt.exitCode -ne -1073741819 -or $receipt.stderr -ne "before abrupt termination`n"){throw}
}
$name='Local\race-packager-timeout-'+$PID
$gate=[Threading.EventWaitHandle]::new($false,[Threading.EventResetMode]::ManualReset,$name)
try {
  try {Run-Native $Child @('gate',$name) 500 | Out-Null; throw 'Gate unexpectedly completed'} catch {
    $receipt=$_.Exception.Data['processReceipt']
    if($null -eq $receipt -or -not $receipt.quiescent -or $_.Exception.Message -notlike 'Timed out:*' -or $receipt.stdout -notmatch '^READY [0-9]+') {throw}
    if(Get-Process -Id $receipt.pid -ErrorAction SilentlyContinue){throw 'Timed-out child survived helper return'}
  }
} finally {$gate.Dispose()}
'native tool natural/abrupt/timeout quiescence passed'

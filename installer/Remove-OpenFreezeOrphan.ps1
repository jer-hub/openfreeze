#Requires -Version 5.1
#Requires -RunAsAdministrator
<#
.SYNOPSIS
  Remove OpenFreezeSvc + C:\OpenFreeze when the OfFilter driver is missing
  (half-install / Panel "query failed err=1"). Does not require thaw.
#>
[CmdletBinding()]
param(
  [switch]$RemoveAuth
)
$ErrorActionPreference = 'Continue'
function L([string]$m){ Write-Host "[OrphanCleanup] $m" }

$drv = Get-Service OfFilter -EA SilentlyContinue
$sysPath = "$env:SystemRoot\System32\drivers\OfFilter.sys"
if ($drv) {
  L "OfFilter is registered (Status=$($drv.Status)). Use Uninstall-OpenFreeze.ps1 while thawed instead."
  exit 1
}
if (Test-Path $sysPath) {
  L "OfFilter.sys present at $sysPath but service missing. Use Uninstall-OpenFreeze.ps1 / pnputil cleanup while thawed."
  exit 1
}
# Leftover Parameters key without a real service (half-install on wrong machine).
$ofKey = 'HKLM:\SYSTEM\CurrentControlSet\Services\OfFilter'
if (Test-Path $ofKey) {
  Remove-Item $ofKey -Recurse -Force -EA SilentlyContinue
  L 'removed orphan HKLM\...\Services\OfFilter'
}

$svc = Get-Service OpenFreezeSvc -EA SilentlyContinue
if ($svc) {
  if ($svc.Status -ne 'Stopped') {
    Stop-Service OpenFreezeSvc -Force -EA SilentlyContinue
    Start-Sleep 2
  }
  Get-Process -Name OpenFreeze.Service,OpenFreeze.Panel,ofpipe -EA SilentlyContinue |
    Stop-Process -Force -EA SilentlyContinue
  sc.exe delete OpenFreezeSvc | Out-Null
  Start-Sleep 2
  L 'deleted OpenFreezeSvc'
} else { L 'OpenFreezeSvc not present' }

if (Test-Path C:\OpenFreeze) {
  Remove-Item C:\OpenFreeze -Recurse -Force
  L 'removed C:\OpenFreeze'
}
if ($RemoveAuth -and (Test-Path HKLM:\SOFTWARE\OpenFreeze)) {
  Remove-Item HKLM:\SOFTWARE\OpenFreeze -Recurse -Force
  L 'removed HKLM\SOFTWARE\OpenFreeze'
}
L 'END_PASS'
exit 0

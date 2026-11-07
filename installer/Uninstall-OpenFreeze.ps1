#Requires -Version 5.1
#Requires -RunAsAdministrator
<#
.SYNOPSIS
  Lab uninstall: require thawed/passthrough, remove service, overlay params, driver, C:\OpenFreeze.
#>
[CmdletBinding()]
param(
  [switch]$RemoveAuth,
  [switch]$SkipRebootPrompt
)
$ErrorActionPreference = 'Stop'

function Write-Step([string]$m) { Write-Host "[OpenFreeze] $m" }
function Test-IsAdmin {
  return ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
    [Security.Principal.WindowsBuiltInRole]::Administrator)
}

if (-not (Test-IsAdmin)) { throw 'Must run elevated (Run as administrator).' }

$dest = 'C:\OpenFreeze'
$oq = Join-Path $dest 'ofquery.exe'
if (-not (Test-Path $oq)) {
  # try package-relative if run from package before copy
  $alt = Join-Path $PSScriptRoot 'tools\ofquery.exe'
  if (Test-Path $alt) { $oq = $alt }
}

if (Test-Path $oq) {
  $q = (& $oq query 2>&1 | Out-String)
  Write-Step "ofquery: $($q.Trim())"
  if ($q -match 'cow=1' -and $q -match 'state=FROZEN') {
    throw @'
OfFilter is still FROZEN. Uninstall would leave a messy UpperFilters state and discard nothing safely.
  1) ofpipe setmode <password> thawed
  2) reboot
  3) re-run Uninstall-OpenFreeze.ps1
'@
  }
} else {
  Write-Step 'ofquery not found — continuing (driver may already be removed)'
}

Write-Step 'stopping OpenFreezeSvc'
$svc = Get-Service OpenFreezeSvc -EA SilentlyContinue
if ($svc) {
  if ($svc.Status -ne 'Stopped') {
    Stop-Service OpenFreezeSvc -Force -EA SilentlyContinue
    Start-Sleep 2
  }
  sc.exe delete OpenFreezeSvc | Out-Null
  Start-Sleep 2
  Write-Step 'service deleted'
} else {
  Write-Step 'service not present'
}

if ($RemoveAuth) {
  $auth = 'HKLM:\SOFTWARE\OpenFreeze'
  if (Test-Path $auth) {
    Remove-Item $auth -Recurse -Force
    Write-Step 'removed HKLM\SOFTWARE\OpenFreeze'
  }
}

$ofov = Join-Path $dest 'ofoverlay.exe'
if (Test-Path $ofov) {
  Write-Step 'ofoverlay clear'
  & $ofov clear 2>&1 | Out-Host
}

$overlay = Join-Path $dest 'overlay.bin'
if (Test-Path $overlay) {
  Remove-Item $overlay -Force -EA SilentlyContinue
  Write-Step 'deleted overlay.bin'
}

# Remove from Volume class UpperFilters
Write-Step 'removing OfFilter from Volume UpperFilters'
$volClass = 'HKLM:\SYSTEM\CurrentControlSet\Control\Class\{71A27CDD-812A-11D0-BEC7-08002BE2092F}'
if (Test-Path $volClass) {
  $uf = (Get-ItemProperty $volClass -Name UpperFilters -EA SilentlyContinue).UpperFilters
  if ($uf) {
    $new = @($uf | Where-Object { $_ -ne 'OfFilter' })
    if ($new.Count -ne @($uf).Count) {
      if ($new.Count -eq 0) {
        Remove-ItemProperty -Path $volClass -Name UpperFilters -Force -EA SilentlyContinue
      } else {
        Set-ItemProperty -Path $volClass -Name UpperFilters -Value $new -Type MultiString -Force
      }
      Write-Step 'UpperFilters updated'
    }
  }
}

# Delete published driver package(s) named OfFilter
Write-Step 'pnputil delete OfFilter packages'
$drivers = & pnputil.exe /enum-drivers 2>&1 | Out-String
$blocks = $drivers -split '(?=Published Name:)'
foreach ($b in $blocks) {
  if ($b -match '(?i)Original Name:\s*OfFilter\.inf' -or $b -match '(?i)Driver Name:\s*OfFilter') {
    if ($b -match 'Published Name:\s*(\S+)') {
      $pub = $Matches[1]
      Write-Step "deleting $pub"
      & pnputil.exe /delete-driver $pub /uninstall /force 2>&1 | Out-Host
    }
  }
}

# Clear service Parameters remnants
$pk = 'HKLM:\SYSTEM\CurrentControlSet\Services\OfFilter'
if (Test-Path $pk) {
  Write-Step 'removing Services\OfFilter key'
  Remove-Item $pk -Recurse -Force -EA SilentlyContinue
}

if (Test-Path $dest) {
  Write-Step 'removing C:\OpenFreeze'
  Remove-Item $dest -Recurse -Force -EA SilentlyContinue
}

Write-Step 'UNINSTALL COMPLETE — reboot required'
if (-not $SkipRebootPrompt) {
  $r = Read-Host 'Reboot now? [y/N]'
  if ($r -eq 'y' -or $r -eq 'Y') {
    Restart-Computer -Force
  }
}
exit 0

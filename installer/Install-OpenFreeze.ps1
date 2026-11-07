#Requires -Version 5.1
#Requires -RunAsAdministrator
<#
.SYNOPSIS
  Lab install: test-signed OfFilter + overlay + OpenFreezeSvc + Panel/tools.
  Fail-safe: EnableCow stays 0 unless -FreezeAfterInstall. Lab/testsigning only.
  Refuses to leave OpenFreezeSvc running without a registered OfFilter driver.
#>
[CmdletBinding()]
param(
  [double]$OverlaySizeGB = 2,
  [string]$BootstrapPassword = '',
  [switch]$FreezeAfterInstall,
  [switch]$AllowNonVm,
  [switch]$SkipRebootPrompt,
  [switch]$Silent
)
$ErrorActionPreference = 'Stop'
if ($Silent) { $SkipRebootPrompt = $true }

$PackageRoot = $PSScriptRoot
if (-not (Test-Path (Join-Path $PackageRoot 'driver\OfFilter.sys'))) {
  throw "Run from package root (missing driver\OfFilter.sys). Root=$PackageRoot"
}

function Write-Step([string]$m) { Write-Host "[OpenFreeze] $m" }
function Test-IsAdmin {
  return ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
    [Security.Principal.WindowsBuiltInRole]::Administrator)
}
function Get-TestSigningEnabled {
  # Prefer cmd.exe so PowerShell never mangled {current}; fall back to full bcdedit dump.
  $out = (cmd.exe /c "bcdedit /enum {current}" 2>&1 | Out-String)
  if ($out -match '(?im)Access is denied|could not be opened') {
    throw 'Cannot read BCD (bcdedit). Re-run elevated as Administrator.'
  }
  if ($out -match '(?im)testsigning\s+Yes') { return $true }
  $all = (cmd.exe /c "bcdedit" 2>&1 | Out-String)
  if ($all -match '(?im)testsigning\s+Yes') { return $true }
  return $false
}
function Test-IsVirtualMachine {
  # Do NOT treat HypervisorPresent alone as a VM — Hyper-V *hosts* set that on bare metal.
  try {
    $cs = Get-CimInstance -ClassName Win32_ComputerSystem -EA Stop
    $model = [string]$cs.Model
    $mfg = [string]$cs.Manufacturer
    if ($model -match 'Virtual Machine|Virtual|VMware|VirtualBox|HVM|Hyper-V|KVM|QEMU|Xen') { return $true }
    if ($mfg -match 'VMware|innotek|QEMU|Xen' ) { return $true }
    if ($mfg -match 'Microsoft Corporation' -and $model -match 'Virtual') { return $true }
    $bios = Get-CimInstance -ClassName Win32_BIOS -EA SilentlyContinue
    if ($bios -and ([string]$bios.Version + [string]$bios.SMBIOSBIOSVersion) -match 'VBOX|VMWARE|Hyper-V|BOCHS|SeaBIOS') { return $true }
    $csd = Get-CimInstance -ClassName Win32_ComputerSystemProduct -EA SilentlyContinue
    if ($csd -and [string]$csd.Version -match 'Virtual|VMware|VirtualBox|Hyper-V') { return $true }
    return $false
  } catch { return $false }
}
function Test-IsHyperVHost {
  try {
    $cs = Get-CimInstance -ClassName Win32_ComputerSystem -EA Stop
    return [bool]$cs.HypervisorPresent -and -not (Test-IsVirtualMachine)
  } catch { return $false }
}
function Test-OfFilterDriverRegistered {
  $svc = Get-Service -Name OfFilter -EA SilentlyContinue
  if ($svc) { return $true }
  $sys = Test-Path "$env:SystemRoot\System32\drivers\OfFilter.sys"
  return [bool]$sys
}
function Remove-OpenFreezeSvcIfPresent {
  $svc = Get-Service OpenFreezeSvc -EA SilentlyContinue
  if (-not $svc) { return }
  if ($svc.Status -ne 'Stopped') {
    Stop-Service OpenFreezeSvc -Force -EA SilentlyContinue
    Start-Sleep 2
  }
  Get-Process -Name 'OpenFreeze.Service','OpenFreeze.Panel','ofpipe' -EA SilentlyContinue |
    Stop-Process -Force -EA SilentlyContinue
  sc.exe delete OpenFreezeSvc | Out-Null
  Start-Sleep 2
}
function Get-OfQueryText {
  $oq = Join-Path $PackageRoot 'tools\ofquery.exe'
  if (-not (Test-Path $oq)) { $oq = 'C:\OpenFreeze\ofquery.exe' }
  if (-not (Test-Path $oq)) { return '' }
  return ((& $oq query 2>&1) | Out-String)
}

if (-not (Test-IsAdmin)) { throw 'Must run elevated (Run as administrator).' }

Write-Step 'checking testsigning'
if (-not (Get-TestSigningEnabled)) {
  throw @'
testsigning is not enabled. Lab package requires a test-signed driver.
  bcdedit /set testsigning on
  (Secure Boot usually must be off)
Then reboot and re-run Install-OpenFreeze.ps1.
Do not install on a PC that cannot run Test Mode.
'@
}

$isVm = Test-IsVirtualMachine
$isHvHost = Test-IsHyperVHost
$modelNow = (Get-CimInstance Win32_ComputerSystem).Model
Write-Step ("machine VM-like=$isVm HyperVHost=$isHvHost Model=$modelNow")
if ($isHvHost -and -not $AllowNonVm) {
  throw @"
This looks like a Hyper-V *host* (hypervisor present, model='$modelNow'), not a guest VM.
Install refused — do not put OfFilter on the build/dev host.
Run OpenFreeze-Setup.exe inside the DriverTest guest (or another lab VM), or on a dedicated lab PC image with -AllowNonVm / the Setup checkbox.
"@
}
if (-not $isVm) {
  if (-not $AllowNonVm) {
    throw @'
This does not look like a Hyper-V/VMware/VirtualBox guest.
Install refused (protects Hyper-V *hosts* and daily drivers).
For a managed lab PC image with testsigning, re-run with -AllowNonVm (Setup: check Allow non-VM).
'@
  }
  Write-Step 'WARN: -AllowNonVm set; proceeding on non-VM / lab bare metal'
}

$freeB = [UInt64]((Get-PSDrive C).Free)
$needB = [UInt64](($OverlaySizeGB + 2) * 1GB)
if ($freeB -lt $needB) {
  throw ("Not enough free space on C:. Need ~{0:N1} GB, have {1:N1} GB." -f ($OverlaySizeGB + 2), ($freeB / 1GB))
}

$oqPre = Get-OfQueryText
if ($oqPre -match 'cow=1' -and $oqPre -match 'state=FROZEN') {
  throw 'OfFilter is FROZEN. Thaw (ofpipe setmode ... thawed) and reboot before reinstalling/replacing the driver.'
}

$dest = 'C:\OpenFreeze'
New-Item -ItemType Directory -Path $dest -Force | Out-Null

Write-Step 'importing test signing certificate'
$cer = Join-Path $PackageRoot 'driver\DriverTestSigning.cer'
if (Test-Path $cer) {
  certutil -addstore -f Root $cer | Out-Null
  certutil -addstore -f TrustedPublisher $cer | Out-Null
} else {
  Write-Step 'WARN: DriverTestSigning.cer missing — ensure the driver signature is already trusted'
}

Write-Step 'disabling hibernation / Fast Startup helper'
& powercfg.exe /h off | Out-Null

Write-Step 'installing OfFilter.sys (EnableCow=0 fail-safe)'
$inf = Join-Path $PackageRoot 'driver\OfFilter.inf'
$pnp = & pnputil.exe /add-driver $inf /install 2>&1 | Out-String
Write-Host $pnp

# Must have OfFilter service registered before we install the user-mode service.
if (-not (Test-OfFilterDriverRegistered)) {
  Remove-OpenFreezeSvcIfPresent
  throw @'
OfFilter driver did not register (pnputil). OpenFreezeSvc was NOT left running.
Check: testsigning Yes, Secure Boot off, catalog/signature, Event Viewer System log.
'@
}
Write-Step 'OfFilter driver package registered'

Write-Step 'stopping OpenFreezeSvc so binaries can be replaced'
Remove-OpenFreezeSvcIfPresent
Get-Process -Name 'OpenFreeze.Service','OpenFreeze.Panel','ofpipe' -EA SilentlyContinue |
  Stop-Process -Force -EA SilentlyContinue
Start-Sleep 1

Write-Step 'copying tools and app to C:\OpenFreeze'
Copy-Item (Join-Path $PackageRoot 'tools\*') $dest -Force -Recurse
Copy-Item (Join-Path $PackageRoot 'app\*') $dest -Force -Recurse
Copy-Item (Join-Path $PackageRoot 'EXPECT_SYS.txt') (Join-Path $dest 'EXPECT_SYS.txt') -Force -EA SilentlyContinue
Copy-Item (Join-Path $PackageRoot 'HASHES.txt') (Join-Path $dest 'HASHES.txt') -Force -EA SilentlyContinue
$verifySrc = Join-Path $PackageRoot 'Verify-OpenFreeze.ps1'
if (Test-Path $verifySrc) { Copy-Item $verifySrc (Join-Path $dest 'Verify-OpenFreeze.ps1') -Force }

Write-Step ("creating overlay {0} GB" -f $OverlaySizeGB)
$ofov = Join-Path $dest 'ofoverlay.exe'
if (-not (Test-Path $ofov)) { throw "missing $ofov" }
$gbArg = [int][Math]::Floor($OverlaySizeGB)
if ($gbArg -lt 1) { throw 'OverlaySizeGB must be >= 1 for ofoverlay create' }
$cr = & $ofov create $gbArg 2>&1 | Out-String
Write-Host $cr
if ($cr -notmatch 'RESULT OK') {
  Remove-OpenFreezeSvcIfPresent
  throw "ofoverlay create failed: $cr"
}

# Re-check driver before starting user-mode service (avoid orphan Panel/service).
if (-not (Test-OfFilterDriverRegistered)) {
  Remove-OpenFreezeSvcIfPresent
  throw 'OfFilter disappeared after overlay create; refusing to start OpenFreezeSvc.'
}

Write-Step 'installing OpenFreezeSvc'
$exe = Join-Path $dest 'OpenFreeze.Service.exe'
if (-not (Test-Path $exe)) { throw "missing $exe" }
New-Service -Name OpenFreezeSvc -BinaryPathName "`"$exe`"" -DisplayName 'OpenFreeze Control Service' `
  -Description 'Password-gated OpenFreeze freeze/thaw control plane (lab).' -StartupType Automatic | Out-Null
sc.exe failure OpenFreezeSvc reset= 86400 actions= restart/5000/restart/5000/restart/5000 | Out-Null
sc.exe failureflag OpenFreezeSvc 1 | Out-Null
Start-Service OpenFreezeSvc
Start-Sleep 2
$st = (Get-Service OpenFreezeSvc).Status
if ($st -ne 'Running') {
  Remove-OpenFreezeSvcIfPresent
  throw "OpenFreezeSvc failed to start (Status=$st)"
}
Write-Step "service Running; pipe \\.\pipe\OpenFreeze.Control"

if ($BootstrapPassword) {
  Write-Step 'bootstrapping password via ofpipe'
  $ofpipe = Join-Path $dest 'ofpipe.exe'
  $boot = & $ofpipe bootstrap $BootstrapPassword 2>&1 | Out-String
  Write-Host $boot
  if ($boot -notmatch '(?i)ok|success|HasPassword|AlreadyHasPassword|bootstrap' -and $LASTEXITCODE -ne 0) {
    Write-Step "WARN: bootstrap may have failed (exit=$LASTEXITCODE). Set password in Panel after reboot."
  }
} else {
  Write-Step 'No -BootstrapPassword. Set password in Panel (Set / Change PW) or: ofpipe bootstrap <pw>'
}

if ($FreezeAfterInstall) {
  if (-not $BootstrapPassword) {
    throw '-FreezeAfterInstall requires -BootstrapPassword to arm freeze via ofpipe'
  }
  Write-Step 'arming Frozen for next boot'
  $ofpipe = Join-Path $dest 'ofpipe.exe'
  $sm = & $ofpipe setmode $BootstrapPassword frozen 2>&1 | Out-String
  Write-Host $sm
  $pk = 'HKLM:\SYSTEM\CurrentControlSet\Services\OfFilter\Parameters'
  if (Test-Path $pk) { Set-ItemProperty $pk -Name EnableCow -Value 1 -Type DWord -Force }
} else {
  Write-Step 'leaving EnableCow=0 / passthrough until you freeze via Panel or ofpipe'
}

Write-Host ''
Write-Step 'INSTALL COMPLETE (lab package)'
Write-Host '  Tools/app:  C:\OpenFreeze\'
Write-Host '  Panel:      C:\OpenFreeze\OpenFreeze.Panel.exe'
Write-Host '  Verify:     powershell -File C:\OpenFreeze\Verify-OpenFreeze.ps1   (after reboot)'
Write-Host ''
Write-Host 'Reboot is required for the volume filter to attach. Then run Verify-OpenFreeze.ps1.'
if ($FreezeAfterInstall) {
  Write-Host 'After reboot the volume should be FROZEN (cow=1) if arming succeeded.'
} else {
  Write-Host 'After reboot verify passthrough, then: ofpipe setmode <pw> frozen  and reboot again to freeze.'
}
if (-not $SkipRebootPrompt) {
  $r = Read-Host 'Reboot now? [y/N]'
  if ($r -eq 'y' -or $r -eq 'Y') {
    Restart-Computer -Force
  }
}
exit 0

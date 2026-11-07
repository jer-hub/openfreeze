#Requires -RunAsAdministrator
<#
.SYNOPSIS
  Phase 1 deploy helper for the OfFilter passthrough volume filter (test VM only).

.DESCRIPTION
  Test-signs OfFilter.sys (if unsigned), installs the INF via pnputil, and prompts
  for reboot. Never run this on the host development machine.
#>
[CmdletBinding()]
param(
    [Parameter()]
    [string]$DriverDir = (Join-Path $PSScriptRoot '..\driver\x64\Debug'),

    [Parameter()]
    [switch]$SkipSign
)

$ErrorActionPreference = 'Stop'

$sys = Join-Path $DriverDir 'OfFilter.sys'
$inf = Join-Path $DriverDir 'OfFilter.inf'
if (-not (Test-Path $sys)) { throw "Missing $sys — build driver\OfFilter.sln (Debug|x64) first." }
if (-not (Test-Path $inf)) { throw "Missing $inf" }

Write-Host "Deploying from $DriverDir"

if (-not $SkipSign) {
    $signtool = Get-ChildItem 'C:\Program Files (x86)\Windows Kits\10\bin' -Recurse -Filter signtool.exe |
        Where-Object { $_.FullName -match '\\x64\\signtool\.exe$' } |
        Select-Object -First 1 -ExpandProperty FullName
    if (-not $signtool) { throw 'signtool.exe not found' }

    $storeCert = Get-ChildItem Cert:\CurrentUser\My -CodeSigningCert -ErrorAction SilentlyContinue |
        Where-Object { $_.Subject -match 'WDRT|WDK|Test' } |
        Select-Object -First 1

    if (-not $storeCert) {
        Write-Host 'Creating self-signed WDK test certificate...'
        $storeCert = New-SelfSignedCertificate -Type CodeSigningCert -Subject 'CN=OpenFreeze WDK Test' `
            -CertStoreLocation 'Cert:\CurrentUser\My'
    }

    & $signtool sign /v /s My /n $storeCert.Subject.Replace('CN=', '') /fd sha256 $sys
    if ($LASTEXITCODE -ne 0) { throw "signtool failed: $LASTEXITCODE" }
}

Write-Host 'Installing INF (class upper filter)...'
& pnputil /add-driver $inf /install
if ($LASTEXITCODE -ne 0) { throw "pnputil failed: $LASTEXITCODE" }

Write-Host @'

OfFilter installed into Volume UpperFilters (boot start).
Enable Driver Verifier for OfFilter.sys, then reboot:

  verifier /standard /driver OfFilter.sys
  Restart-Computer

After reboot, confirm load with:  sc.exe query OfFilter
Collect WPP with:  traceview / logman against OfFilterTraceGuid
  {A3F7C2E1-4B5D-4E8A-9C1F-2D3E4F5A6B7C}

Phase 2 CoW (RAM overlay) — after you capture VolumeUniqueId as REG_BINARY under
  HKLM\SYSTEM\CurrentControlSet\Services\OfFilter\Parameters\VolumeUniqueId
set EnableCow=1 (DWORD) there and reboot. Writes on that volume stay off the base
disk; a reboot drops the RAM map (file disappears).
'@

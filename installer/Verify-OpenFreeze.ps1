#Requires -Version 5.1
#Requires -RunAsAdministrator
<#
.SYNOPSIS
  Post-reboot health check for lab OpenFreeze installs.
  Exit 0 = OK; 2 = failed (driver missing / query broken / service down).
#>
[CmdletBinding()]
param()
$ErrorActionPreference = 'Continue'
$fail = 0
function L([string]$m){ Write-Host "[Verify] $m" }

$of = Get-Service OfFilter -EA SilentlyContinue
if (-not $of) {
  L 'FAIL: OfFilter driver service not registered'
  $fail++
} else {
  L "OfFilter Status=$($of.Status) StartType=$($of.StartType)"
}

$svc = Get-Service OpenFreezeSvc -EA SilentlyContinue
if (-not $svc) {
  L 'FAIL: OpenFreezeSvc missing'
  $fail++
} elseif ($svc.Status -ne 'Running') {
  L "FAIL: OpenFreezeSvc Status=$($svc.Status)"
  $fail++
} else {
  L 'OpenFreezeSvc Running'
}

$oq = 'C:\OpenFreeze\ofquery.exe'
if (-not (Test-Path $oq)) {
  L 'FAIL: C:\OpenFreeze\ofquery.exe missing'
  $fail++
} else {
  $q = & $oq query 2>&1 | Out-String
  L "ofquery: $($q.Trim())"
  if ($q -match 'query failed|open failed') {
    L 'FAIL: ofquery error — filter may not be attached (reboot? testsigning?)'
    $fail++
  } elseif ($q -notmatch 'cow=') {
    L 'FAIL: unexpected ofquery output'
    $fail++
  } else {
    L 'ofquery OK'
  }
}

$pipe = 'C:\OpenFreeze\ofpipe.exe'
if (Test-Path $pipe) {
  $st = & $pipe status 2>&1 | Out-String
  L "ofpipe: $($st.Trim())"
  if ($st -match 'query failed err=1|DriverError|Cannot open') {
    L 'FAIL: ofpipe status cannot talk to driver (orphan service risk)'
    $fail++
  }
}

if ($fail -eq 0) {
  L 'END_PASS'
  exit 0
}
L "END_FAIL checks=$fail"
exit 2

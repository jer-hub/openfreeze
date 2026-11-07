#Requires -Version 5.1
<#
.SYNOPSIS
  Copy current stage3 into a prod-candidate folder and write a signing readiness checklist.
  Does not sign anything — run after you have an EV/attestation pipeline.
#>
[CmdletBinding()]
param(
  [string]$StageDir = 'C:\VMs\offilter_stage3',
  [string]$OutDir = 'C:\VMs\offilter_stage_prod_candidate',
  [string]$RepoRoot = ''
)
$ErrorActionPreference = 'Stop'
if (-not $PSScriptRoot) { $PSScriptRoot = Split-Path -Parent $MyInvocation.MyCommand.Path }
if (-not $RepoRoot) { $RepoRoot = Split-Path $PSScriptRoot -Parent }

if (-not (Test-Path (Join-Path $StageDir 'OfFilter.sys'))) {
  throw "missing $StageDir\OfFilter.sys"
}

if (Test-Path $OutDir) { Remove-Item $OutDir -Recurse -Force }
New-Item -ItemType Directory -Path $OutDir -Force | Out-Null
Copy-Item (Join-Path $StageDir '*') $OutDir -Recurse -Force

$expect = ''
$ep = Join-Path $OutDir 'EXPECT_SYS.txt'
if (Test-Path $ep) { $expect = (Get-Content $ep -Raw).Trim() }
$hash = (Get-FileHash (Join-Path $OutDir 'OfFilter.sys') -Algorithm SHA256).Hash

$checklist = @"
OpenFreeze production stage candidate
Created: $(Get-Date -Format o)
Source:  $StageDir
Out:     $OutDir
OfFilter.sys SHA256: $hash
EXPECT_SYS:          $expect

READINESS (manual — see docs/production-signing.md)
[ ] Partner Center / Hardware Dev Center org active
[ ] EV code signing certificate installed (signtool /s My sees it)
[ ] Secure Boot ON pilot machine available
[ ] Replace DriverTestSigning.cer workflow with attestation-signed .sys + .cat
[ ] INF DriverVer bumped for the signed build
[ ] Sign user-mode EXEs (optional): installer\Sign-UserMode.ps1 -CertThumbprint ...
[ ] Rebuild package: New-LabPackage.ps1 -StageDir $OutDir
[ ] Pilot: Setup /S -AllowNonVm on Secure Boot PC + Verify-OpenFreeze.ps1
[ ] Retire testsigning from golden image

Do NOT ship this folder to labs until OfFilter.sys is attestation-signed.
Current copy is still the lab/test-signed binary unless you replaced it.
"@
Set-Content (Join-Path $OutDir 'PROD_CHECKLIST.txt') $checklist -Encoding UTF8
Write-Host $checklist
Write-Host "OK wrote $OutDir\PROD_CHECKLIST.txt"
exit 0

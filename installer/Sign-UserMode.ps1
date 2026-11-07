#Requires -Version 5.1
<#
.SYNOPSIS
  Authenticode-sign OpenFreeze user-mode binaries with a code-signing cert (path B).
  Does not sign the kernel driver — that requires Hardware Dev Center attestation.
#>
[CmdletBinding()]
param(
  [Parameter(Mandatory = $true)]
  [string]$CertThumbprint,
  [string]$RepoRoot = '',
  [string]$Configuration = 'Release'
)
$ErrorActionPreference = 'Stop'
if (-not $PSScriptRoot) { $PSScriptRoot = Split-Path -Parent $MyInvocation.MyCommand.Path }
if (-not $RepoRoot) { $RepoRoot = Split-Path $PSScriptRoot -Parent }

$signtool = Get-ChildItem 'C:\Program Files (x86)\Windows Kits\10\bin' -Recurse -Filter signtool.exe -EA SilentlyContinue |
  Where-Object { $_.FullName -match '\\x64\\signtool\.exe$' } |
  Select-Object -First 1 -ExpandProperty FullName
if (-not $signtool) { throw 'signtool.exe not found (install Windows SDK)' }

$thumb = ($CertThumbprint -replace '\s', '').ToUpperInvariant()
$cert = Get-ChildItem Cert:\CurrentUser\My, Cert:\LocalMachine\My -CodeSigningCert -EA SilentlyContinue |
  Where-Object { $_.Thumbprint.ToUpperInvariant() -eq $thumb } |
  Select-Object -First 1
if (-not $cert) { throw "code-signing cert thumbprint $thumb not found in My stores" }

$targets = @(
  (Join-Path $RepoRoot "user-mode\OpenFreeze.Service\bin\$Configuration\net48\OpenFreeze.Service.exe"),
  (Join-Path $RepoRoot "user-mode\OpenFreeze.Panel\bin\$Configuration\net48\OpenFreeze.Panel.exe"),
  (Join-Path $RepoRoot "user-mode\OpenFreeze.Cli\bin\$Configuration\net48\ofpipe.exe"),
  (Join-Path $RepoRoot "user-mode\OpenFreeze.Setup\bin\$Configuration\net48\OpenFreeze.Setup.exe"),
  (Join-Path $RepoRoot 'dist\OpenFreeze-Setup.exe')
) | Where-Object { Test-Path $_ }

if ($targets.Count -eq 0) { throw 'no built EXEs found — build user-mode and New-LabPackage first' }

foreach ($t in $targets) {
  Write-Host "signing $t"
  & $signtool sign /fd SHA256 /td SHA256 /tr http://timestamp.digicert.com /sha1 $thumb $t
  if ($LASTEXITCODE -ne 0) { throw "signtool failed on $t exit=$LASTEXITCODE" }
}
Write-Host "OK signed $($targets.Count) file(s)"
exit 0

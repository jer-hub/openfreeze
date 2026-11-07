#Requires -Version 5.1
<#
.SYNOPSIS
  Assemble dist/OpenFreeze-lab from offilter_stage3 + net48 Service/Panel/Cli builds.
#>
[CmdletBinding()]
param(
  [string]$StageDir = 'C:\VMs\offilter_stage3',
  [string]$RepoRoot = '',
  [string]$OutDir = ''
)
$ErrorActionPreference = 'Stop'
if (-not $PSScriptRoot) {
  $PSScriptRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
}
if (-not $RepoRoot) { $RepoRoot = Split-Path $PSScriptRoot -Parent }
if (-not $OutDir) { $OutDir = Join-Path $RepoRoot 'dist\OpenFreeze-lab' }

$svcBin = Join-Path $RepoRoot 'user-mode\OpenFreeze.Service\bin\Debug\net48'
$panelBin = Join-Path $RepoRoot 'user-mode\OpenFreeze.Panel\bin\Debug\net48'
$cliBin = Join-Path $RepoRoot 'user-mode\OpenFreeze.Cli\bin\Debug\net48'

function Require-File([string]$p) {
  if (-not (Test-Path -LiteralPath $p)) { throw "missing $p" }
}

Require-File (Join-Path $StageDir 'OfFilter.sys')
Require-File (Join-Path $StageDir 'OfFilter.inf')
Require-File (Join-Path $svcBin 'OpenFreeze.Service.exe')
Require-File (Join-Path $panelBin 'OpenFreeze.Panel.exe')
Require-File (Join-Path $cliBin 'ofpipe.exe')

$expectPath = Join-Path $StageDir 'EXPECT_SYS.txt'
Require-File $expectPath
$expect = (Get-Content $expectPath -Raw).Trim().ToUpperInvariant()
$actual = (Get-FileHash (Join-Path $StageDir 'OfFilter.sys') -Algorithm SHA256).Hash.ToUpperInvariant()
if ($actual -ne $expect) {
  throw "OfFilter.sys hash $actual != EXPECT_SYS.txt $expect"
}

if (Test-Path -LiteralPath $OutDir) {
  Remove-Item -LiteralPath $OutDir -Recurse -Force
}
New-Item -ItemType Directory -Path $OutDir -Force | Out-Null
foreach ($sub in @('driver', 'tools', 'app')) {
  New-Item -ItemType Directory -Path (Join-Path $OutDir $sub) -Force | Out-Null
}

# Driver payload
$driverNames = @(
  'OfFilter.sys', 'OfFilter.inf', 'OfFilter.cat', 'offilter.cat',
  'DriverTestSigning.cer', 'EXPECT_SYS.txt'
)
foreach ($n in $driverNames) {
  $src = Join-Path $StageDir $n
  if (Test-Path -LiteralPath $src) {
    Copy-Item $src (Join-Path $OutDir "driver\$n") -Force
  }
}
# Prefer OfFilter.cat name; stage may ship offilter.cat
$catOut = Join-Path $OutDir 'driver\OfFilter.cat'
if (-not (Test-Path $catOut)) {
  $alt = Join-Path $OutDir 'driver\offilter.cat'
  if (Test-Path $alt) { Copy-Item $alt $catOut -Force }
}

# Tools
foreach ($n in @('ofoverlay.exe', 'ofquery.exe', 'ofctl.exe', 'ofpipe.exe', 'OpenFreeze.Core.dll')) {
  $fromStage = Join-Path $StageDir $n
  $fromCli = Join-Path $cliBin $n
  if ($n -eq 'ofpipe.exe' -or $n -eq 'OpenFreeze.Core.dll') {
    if (Test-Path $fromCli) { Copy-Item $fromCli (Join-Path $OutDir "tools\$n") -Force }
    elseif (Test-Path $fromStage) { Copy-Item $fromStage (Join-Path $OutDir "tools\$n") -Force }
    else { throw "missing tool $n" }
  } else {
    Require-File $fromStage
    Copy-Item $fromStage (Join-Path $OutDir "tools\$n") -Force
  }
}
# Prefer freshly built ofpipe + Core over stage copies
Copy-Item (Join-Path $cliBin 'ofpipe.exe') (Join-Path $OutDir 'tools\ofpipe.exe') -Force
Copy-Item (Join-Path $cliBin 'OpenFreeze.Core.dll') (Join-Path $OutDir 'tools\OpenFreeze.Core.dll') -Force
if (Test-Path (Join-Path $cliBin 'ofpipe.exe.config')) {
  Copy-Item (Join-Path $cliBin 'ofpipe.exe.config') (Join-Path $OutDir 'tools\ofpipe.exe.config') -Force
}

# App: Service + Panel (+ shared Core)
Copy-Item (Join-Path $svcBin 'OpenFreeze.Service.exe') (Join-Path $OutDir 'app\') -Force
Copy-Item (Join-Path $svcBin 'OpenFreeze.Service.exe.config') (Join-Path $OutDir 'app\') -Force -EA SilentlyContinue
Copy-Item (Join-Path $svcBin 'OpenFreeze.Core.dll') (Join-Path $OutDir 'app\') -Force
Copy-Item (Join-Path $panelBin 'OpenFreeze.Panel.exe') (Join-Path $OutDir 'app\') -Force
Copy-Item (Join-Path $panelBin 'OpenFreeze.Panel.exe.config') (Join-Path $OutDir 'app\') -Force -EA SilentlyContinue
# ofpipe next to service for bootstrap from install dir
Copy-Item (Join-Path $OutDir 'tools\ofpipe.exe') (Join-Path $OutDir 'app\ofpipe.exe') -Force
Copy-Item (Join-Path $OutDir 'tools\OpenFreeze.Core.dll') (Join-Path $OutDir 'app\OpenFreeze.Core.dll') -Force
if (Test-Path (Join-Path $OutDir 'tools\ofpipe.exe.config')) {
  Copy-Item (Join-Path $OutDir 'tools\ofpipe.exe.config') (Join-Path $OutDir 'app\ofpipe.exe.config') -Force
}

# Install / uninstall / verify scripts from installer/
foreach ($scriptName in @(
  'Install-OpenFreeze.ps1',
  'Uninstall-OpenFreeze.ps1',
  'Verify-OpenFreeze.ps1',
  'Remove-OpenFreezeOrphan.ps1'
)) {
  Copy-Item (Join-Path $PSScriptRoot $scriptName) (Join-Path $OutDir $scriptName) -Force
}
$deploySrc = Join-Path $PSScriptRoot 'deploy'
$deployDst = Join-Path $OutDir 'deploy'
if (Test-Path $deploySrc) {
  New-Item -ItemType Directory -Path $deployDst -Force | Out-Null
  Copy-Item (Join-Path $deploySrc '*') $deployDst -Force
}

# HASHES for package contents
$hashLines = New-Object System.Collections.Generic.List[string]
$hashLines.Add("=== OpenFreeze-lab $(Get-Date -Format o) ===")
$hashLines.Add("EXPECT_SYS=$expect")
Get-ChildItem $OutDir -Recurse -File | Sort-Object FullName | ForEach-Object {
  $rel = $_.FullName.Substring($OutDir.Length).TrimStart('\')
  $h = (Get-FileHash $_.FullName -Algorithm SHA256).Hash
  $hashLines.Add("$h  $rel")
}
$hashLines | Set-Content (Join-Path $OutDir 'HASHES.txt') -Encoding ASCII
Copy-Item (Join-Path $OutDir 'driver\EXPECT_SYS.txt') (Join-Path $OutDir 'EXPECT_SYS.txt') -Force

$srcReadme = Join-Path $PSScriptRoot 'lab-package-README.md'
Require-File $srcReadme
Copy-Item $srcReadme (Join-Path $OutDir 'README.md') -Force

Write-Host "OK package=$OutDir expect=$expect files=$((Get-ChildItem $OutDir -Recurse -File).Count)"

# --- Embed lab folder into OpenFreeze.Setup and emit dist/OpenFreeze-Setup.exe ---
$setupProjDir = Join-Path $RepoRoot 'user-mode\OpenFreeze.Setup'
$setupCsproj = Join-Path $setupProjDir 'OpenFreeze.Setup.csproj'
Require-File $setupCsproj
$payloadZip = Join-Path $setupProjDir 'payload.zip'
if (Test-Path -LiteralPath $payloadZip) { Remove-Item -LiteralPath $payloadZip -Force }
# Zip *contents* of OutDir so Install-OpenFreeze.ps1 is at zip root
Compress-Archive -Path (Join-Path $OutDir '*') -DestinationPath $payloadZip -Force
Write-Host "payload.zip=$payloadZip size=$([math]::Round((Get-Item $payloadZip).Length/1MB,2)) MB"

Write-Host 'Building OpenFreeze.Setup (Release)...'
& dotnet build $setupCsproj -c Release --nologo
if ($LASTEXITCODE -ne 0) { throw "OpenFreeze.Setup build failed: $LASTEXITCODE" }

$builtExe = Join-Path $setupProjDir 'bin\Release\net48\OpenFreeze.Setup.exe'
Require-File $builtExe
$distRoot = Split-Path $OutDir -Parent
if (-not (Test-Path $distRoot)) { New-Item -ItemType Directory -Path $distRoot -Force | Out-Null }
$setupOut = Join-Path $distRoot 'OpenFreeze-Setup.exe'
Copy-Item $builtExe $setupOut -Force
$cfg = [IO.Path]::ChangeExtension($builtExe, '.exe.config')
if (Test-Path $cfg) { Copy-Item $cfg (Join-Path $distRoot 'OpenFreeze-Setup.exe.config') -Force }
Write-Host "OK setup=$setupOut hash=$((Get-FileHash $setupOut -Algorithm SHA256).Hash)"
exit 0

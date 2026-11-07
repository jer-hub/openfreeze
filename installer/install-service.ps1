# Install OpenFreeze.Service (LocalSystem). Run elevated on the test VM.
param(
  [string]$BinDir = ""
)
$ErrorActionPreference = 'Stop'
if (-not $BinDir) {
  $BinDir = Join-Path $PSScriptRoot '..\user-mode\OpenFreeze.Service\bin\Debug\net48'
}
$exe = Join-Path $BinDir 'OpenFreeze.Service.exe'
if (-not (Test-Path $exe)) { throw "missing $exe — build user-mode\OpenFreeze.sln first" }

$svc = Get-Service OpenFreezeSvc -EA SilentlyContinue
if ($svc) {
  if ($svc.Status -eq 'Running') { Stop-Service OpenFreezeSvc -Force }
  sc.exe delete OpenFreezeSvc | Out-Null
  Start-Sleep 2
}

New-Service -Name OpenFreezeSvc -BinaryPathName "`"$exe`"" -DisplayName 'OpenFreeze Control Service' `
  -Description 'Password-gated OpenFreeze freeze/thaw control plane.' -StartupType Automatic
# Phase 5: restart on failure (reset fail count after 24h).
sc.exe failure OpenFreezeSvc reset= 86400 actions= restart/5000/restart/5000/restart/5000 | Out-Null
sc.exe failureflag OpenFreezeSvc 1 | Out-Null
Start-Service OpenFreezeSvc
Get-Service OpenFreezeSvc | Format-List Name, Status, StartType
Write-Host "Pipe: \\.\pipe\OpenFreeze.Control"
Write-Host "Bootstrap password from an elevated console:"
Write-Host "  OpenFreeze.Service.exe --console   # or use Panel 'Set / Change PW' while service runs"

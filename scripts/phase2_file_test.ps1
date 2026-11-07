# Phase 2 FILE backend VM test (stage3) v8. Resumable via state file. PS 5.1-safe.
# v8: stack fix driver 0.8.6.1 (pool cfgBuf+combined). FailFast/first-freeze capture HV 18590.
#     expectSys=F4290E88...; still prefers checkpoint v0.8.6.0 unfrozen until retaken.
# v6: marker ArgList fix (empty-expect false SURVIVED). Driver 0.8.5.3
# v7: config-sector ThawNextBoot (driver 0.8.6.0). Phase D uses ofquery thaw-set/clear
#     instead of registry EnableCow=0 while frozen. Checkpoint v0.8.6.0 unfrozen.
#     ofquery V4: cfgValid/thawNext/cfgSeq; reasons ThawRequested|ConfigCorrupt.
#     initStep Done=13 (ConfigSector inserted at 6).
#     empty expect => HARNESS_ERROR; guest echoes receivedExpect=.
# v5: marker-after uses host-known name (v4 false-SURVIVED when CoW discarded
#     of_cycle_marker_name.txt then Test-Path "C:\$null" => True). Driver 0.8.5.3
#     slot-I/O-in-extents assert; expect hash set after stage.
# v4: ConfigRefuse fix driver 0.8.5.2; ofquery V3 initStep/initStatus; expect 3F64ACFA;
#     resume from v0.8.5.1 CP + reinstall; take/prefer v0.8.5.2 unfrozen CP; no overlay recreate unless invalid.
# Never prints password/KDNET key. No Restart-VM -Force, no -TurnOff, no VM memory changes.
# Defaults assume a local lab layout under C:\VMs\ (stage, logs, guest password file).
# Do not commit secrets (e.g. devadmin_password.txt).
$ErrorActionPreference = 'Continue'

$vm = 'DriverTest'
$cpFix5 = 'Phase2 fix5 deployed unfrozen'
$cpFile = 'Phase2 file backend staged unfrozen'
$cpFile0851 = 'Phase2 file backend v0.8.5.1 unfrozen'
$cpFile0852 = 'Phase2 file backend v0.8.5.2 unfrozen'
$cpFile0853 = 'Phase2 file backend v0.8.5.3 unfrozen'
$cpFileNew = 'Phase2 file backend v0.8.6.0 unfrozen'
$stage = 'C:\VMs\offilter_stage3'
$expectSys = 'F4290E88B44886906E49CEE59DCFDE83B163898AFF64370726B8053D5784F25E'
$statePath = 'C:\VMs\phase2_file_test_state.txt'
$cyclesTarget = 20
$cred = $null
$script:EndWritten = $false
$script:tag = $null
$script:log = $null
$script:dir = $null
$script:phase = 'A'
$script:cycle = 0

function L([string]$m) {
  $line = "$(Get-Date -f HH:mm:ss) $m"
  if ($script:log) { $line | Out-File $script:log -Append -Encoding utf8 }
  Write-Host $line
}
function Save([string]$name, $obj) {
  try { ($obj | Out-String -Width 400) | Out-File "$($script:dir)\$name.txt" -Encoding utf8 }
  catch { L "save $name failed: $($_.Exception.Message)" }
}
function VMS {
  try {
    $v = Get-VM $vm -EA Stop
    "State=$($v.State) Status=$($v.Status) Heartbeat=$($v.Heartbeat) MemAssignedMB=$([math]::Round($v.MemoryAssigned/1MB)) Uptime=$($v.Uptime)"
  } catch { "Get-VM: $($_.Exception.Message)" }
}
function EndLine([string]$status, [string]$reason = '') {
  if ($script:EndWritten) { return }
  $script:EndWritten = $true
  $line = if ($reason) { "$status $reason" } else { $status }
  if ($script:log) { $line | Out-File $script:log -Append -Encoding utf8 }
  Write-Host $line
  try {
    @"
TAG=$($script:tag)
LOG=$($script:log)
DIR=$($script:dir)
PHASE=$($script:phase)
CYCLE=$($script:cycle)
STATUS=$status
REASON=$reason
UPDATED=$(Get-Date -f 'yyyy-MM-dd HH:mm:ss')
"@ | Set-Content $statePath -Encoding utf8
  } catch {}
}
function SaveState {
  @"
TAG=$($script:tag)
LOG=$($script:log)
DIR=$($script:dir)
PHASE=$($script:phase)
CYCLE=$($script:cycle)
STATUS=running
UPDATED=$(Get-Date -f 'yyyy-MM-dd HH:mm:ss')
"@ | Set-Content $statePath -Encoding utf8
}
function LoadState {
  if (-not (Test-Path $statePath)) { return $false }
  $m = @{}
  Get-Content $statePath | ForEach-Object {
    if ($_ -match '^([A-Z]+)=(.*)$') { $m[$Matches[1]] = $Matches[2] }
  }
  if ($m['STATUS'] -ne 'running') { return $false }
  if (-not $m['TAG'] -or -not $m['LOG'] -or -not $m['DIR']) { return $false }
  $script:tag = $m['TAG']; $script:log = $m['LOG']; $script:dir = $m['DIR']
  $script:phase = $(if ($m['PHASE']) { $m['PHASE'] } else { 'A' })
  $script:cycle = 0
  [void][int]::TryParse([string]$m['CYCLE'], [ref]$script:cycle)
  return $true
}

# Exact ofquery field parse ÃƒÆ’Ã†â€™Ãƒâ€ Ã¢â‚¬â„¢ÃƒÆ’Ã¢â‚¬Â ÃƒÂ¢Ã¢â€šÂ¬Ã¢â€žÂ¢ÃƒÆ’Ã†â€™ÃƒÂ¢Ã¢â€šÂ¬Ã‚Â ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÂ¢Ã¢â‚¬Å¾Ã‚Â¢ÃƒÆ’Ã†â€™Ãƒâ€ Ã¢â‚¬â„¢ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬Ãƒâ€¦Ã‚Â¡ÃƒÆ’Ã†â€™ÃƒÂ¢Ã¢â€šÂ¬Ã…Â¡ÃƒÆ’Ã¢â‚¬Å¡Ãƒâ€šÃ‚Â¢ÃƒÆ’Ã†â€™Ãƒâ€ Ã¢â‚¬â„¢ÃƒÆ’Ã¢â‚¬Â ÃƒÂ¢Ã¢â€šÂ¬Ã¢â€žÂ¢ÃƒÆ’Ã†â€™ÃƒÂ¢Ã¢â€šÂ¬Ã…Â¡ÃƒÆ’Ã¢â‚¬Å¡Ãƒâ€šÃ‚Â¢ÃƒÆ’Ã†â€™Ãƒâ€ Ã¢â‚¬â„¢ÃƒÆ’Ã¢â‚¬Å¡Ãƒâ€šÃ‚Â¢ÃƒÆ’Ã†â€™Ãƒâ€šÃ‚Â¢ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬Ãƒâ€¦Ã‚Â¡ÃƒÆ’Ã¢â‚¬Å¡Ãƒâ€šÃ‚Â¬ÃƒÆ’Ã†â€™ÃƒÂ¢Ã¢â€šÂ¬Ã‚Â¦ÃƒÆ’Ã¢â‚¬Å¡Ãƒâ€šÃ‚Â¡ÃƒÆ’Ã†â€™Ãƒâ€ Ã¢â‚¬â„¢ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬Ãƒâ€¦Ã‚Â¡ÃƒÆ’Ã†â€™ÃƒÂ¢Ã¢â€šÂ¬Ã…Â¡ÃƒÆ’Ã¢â‚¬Å¡Ãƒâ€šÃ‚Â¬ÃƒÆ’Ã†â€™Ãƒâ€ Ã¢â‚¬â„¢ÃƒÆ’Ã¢â‚¬Â ÃƒÂ¢Ã¢â€šÂ¬Ã¢â€žÂ¢ÃƒÆ’Ã†â€™ÃƒÂ¢Ã¢â€šÂ¬Ã…Â¡ÃƒÆ’Ã¢â‚¬Å¡Ãƒâ€šÃ‚Â¢ÃƒÆ’Ã†â€™Ãƒâ€ Ã¢â‚¬â„¢ÃƒÆ’Ã¢â‚¬Å¡Ãƒâ€šÃ‚Â¢ÃƒÆ’Ã†â€™Ãƒâ€šÃ‚Â¢ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã…Â¡Ãƒâ€šÃ‚Â¬ÃƒÆ’Ã¢â‚¬Â¦Ãƒâ€šÃ‚Â¡ÃƒÆ’Ã†â€™ÃƒÂ¢Ã¢â€šÂ¬Ã…Â¡ÃƒÆ’Ã¢â‚¬Å¡Ãƒâ€šÃ‚Â¬ÃƒÆ’Ã†â€™Ãƒâ€ Ã¢â‚¬â„¢ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬Ãƒâ€¦Ã‚Â¡ÃƒÆ’Ã†â€™ÃƒÂ¢Ã¢â€šÂ¬Ã…Â¡ÃƒÆ’Ã¢â‚¬Å¡Ãƒâ€šÃ‚Â never substring-match bare 'cow=1' (that hits EnableCow=1).
function Parse-OfQuery([string]$text) {
  $o = [ordered]@{
    raw = "$text"
    ok = $false
    cow = $null; torn = $null; slotsUsed = $null; full = $null
    state = $null; target = $null; reason = $null; initStep = $null; initStatus = $null; cfgValid = $null; thawNext = $null; cfgSeq = $null
  }
  if ([string]::IsNullOrWhiteSpace($text)) { return [pscustomobject]$o }
  # Host log lines embed "ofquery=cow=..."; peel that so field anchors still work.
  if ($text -match '(?i)\bofquery=') {
    $text = [regex]::Replace($text, '(?i)^.*?\bofquery=', '')
  }
  if ($text -match '(?m)(?:^|\s)cow=(\d+)(?:\s|$)') { $o.cow = [int]$Matches[1] }
  if ($text -match '(?m)(?:^|\s)torn=(\d+)(?:\s|$)') { $o.torn = [int]$Matches[1] }
  if ($text -match '(?m)(?:^|\s)slotsUsed=(\d+)(?:\s|$)') { $o.slotsUsed = [int]$Matches[1] }
  if ($text -match '(?m)(?:^|\s)full=(\d+)(?:\s|$)') { $o.full = [UInt64]$Matches[1] }
  if ($text -match '(?m)(?:^|\s)state=(FROZEN|PASSTHROUGH|TORN)(?:\s|$)') { $o.state = $Matches[1] }
  if ($text -match '(?m)(?:^|\s)target=(\d+)(?:\s|$)') { $o.target = [int]$Matches[1] }
  if ($text -match '(?m)(?:^|\s)reason=(None|NotTarget|ConfigRefuse|ThawRequested|ConfigCorrupt)(?:\s|$)') { $o.reason = $Matches[1] }
  if ($text -match '(?m)(?:^|\s)initStep=(\d+)(?:\s|$)') { $o.initStep = [int]$Matches[1] }
  if ($text -match '(?m)(?:^|\s)initStatus=(0x[0-9A-Fa-f]+|\d+)(?:\s|$)') { $o.initStatus = $Matches[1] }
  if ($text -match '(?m)(?:^|\s)cfgValid=(\d+)(?:\s|$)') { $o.cfgValid = [int]$Matches[1] }
  if ($text -match '(?m)(?:^|\s)thawNext=(\d+)(?:\s|$)') { $o.thawNext = [int]$Matches[1] }
  if ($text -match '(?m)(?:^|\s)cfgSeq=(\d+)(?:\s|$)') { $o.cfgSeq = [int]$Matches[1] }
  $o.ok = ($null -ne $o.cow)
  return [pscustomobject]$o
}
function OfQuery-IsFrozen($q) { return ($q.ok -and $q.cow -eq 1 -and $q.state -eq 'FROZEN') }
function OfQuery-IsPassthroughOrRefused($q) {
  if (-not $q.ok) { return $false }
  if ($q.state -eq 'PASSTHROUGH') { return $true }
  if ($q.reason -in @('NotTarget', 'ConfigRefuse', 'ThawRequested', 'ConfigCorrupt')) { return $true }
  if ($q.cow -eq 0 -and $q.state -ne 'FROZEN') { return $true }
  return $false
}

function G([string]$name, [scriptblock]$sb, [int]$cap = 90, [object[]]$ArgList = @()) {
  $j = $null
  try {
    # NOTE: never name this param $Args Ã¢â‚¬â€ automatic $args shadows it and empties -ArgumentList.
    if ($null -ne $ArgList -and $ArgList.Count -gt 0) {
      $j = Invoke-Command -VMName $vm -Credential $cred -ScriptBlock $sb -ArgumentList $ArgList -AsJob -EA Stop
    } else {
      $j = Invoke-Command -VMName $vm -Credential $cred -ScriptBlock $sb -AsJob -EA Stop
    }
    if (Wait-Job $j -Timeout $cap) {
      $r = Receive-Job $j -ErrorVariable ev -EA SilentlyContinue
      if ($ev) { L "$name errors: $(($ev | ForEach-Object { $_.ToString() }) -join ' || ')" }
      L "$name done ($($j.State))"
      Save $name $r
      foreach ($x in @($r)) { L ("{0}: {1}" -f $name, "$x") }
      return ,$r
    }
    L "$name TIMEOUT after ${cap}s"; Save $name "TIMEOUT after ${cap}s"; return $null
  } catch {
    L "$name FAILED: $($_.Exception.Message)"; Save $name "FAILED: $($_.Exception.Message)"; return $null
  } finally {
    if ($j) { try { Stop-Job $j -EA 0; Remove-Job $j -Force -EA 0 } catch {} }
  }
}

function IC {
  param([Parameter(Position = 0)][scriptblock]$Sb, [object[]]$A = @(), [string]$What = 'guest call')
  $t = (Get-Date).AddMinutes(3); $d = 5; $e = ''
  while ($true) {
    try {
      $r = Invoke-Command -VMName $vm -Credential $cred -ScriptBlock $Sb -ArgumentList $A -EA Stop
      if ($null -ne $r -and "$r".Trim() -ne '') { return ,$r }
      $e = 'empty output'
    } catch { $e = $_.Exception.Message }
    if ((Get-Date) -gt $t) { break }
    L "PS Direct '$What' retry in ${d}s ($e)"; Start-Sleep $d; $d = [math]::Min($d * 2, 30)
  }
  L "PS Direct '$What' failed after ~3 min: $e"
  throw "PSDIRECT_FAIL:$What"
}

function WaitVM([int]$tmo) {
  $t = (Get-Date).AddSeconds($tmo)
  while ((Get-Date) -lt $t) {
    try { if ((Invoke-Command -VMName $vm -Credential $cred -ScriptBlock { 1 } -EA Stop) -eq 1) { return $true } } catch {}
    Start-Sleep 10
  }
  return $false
}

function Reboot([string]$what) {
  try { Invoke-Command -VMName $vm -Credential $cred { Restart-Computer -Force } -EA Stop }
  catch { L "restart call: $($_.Exception.Message)" }
  Start-Sleep 45
  if (-not (WaitVM 600)) { L "Reboot '$what' PS Direct not up in 600s; VM $(VMS)"; return $false }
  $pr = IC -What "$what probe" { "ok uptimeS=$([math]::Round(((Get-Date)-(Get-CimInstance Win32_OperatingSystem).LastBootUpTime).TotalSeconds))" }
  L "$what probe: $pr"
  return $true
}

function GracefulOff {
  L "graceful Off; VM $(VMS)"
  if ((Get-VM $vm).State -eq 'Off') { L 'already Off'; return $true }
  if ((Get-VM $vm).State -eq 'Running') {
    try { [void](G 'stop_computer' { Stop-Computer -Force } 90) } catch {}
  }
  $deadline = (Get-Date).AddMinutes(5)
  while ((Get-VM $vm).State -ne 'Off' -and (Get-Date) -lt $deadline) { Start-Sleep 5 }
  if ((Get-VM $vm).State -ne 'Off') {
    L 'plain Stop-VM (no -Force/-TurnOff)'
    $sj = Start-Job -ArgumentList $vm { param($n) Stop-VM -Name $n -Confirm:$false }
    while ((Get-VM $vm).State -ne 'Off' -and (Get-Date) -lt $deadline) { Start-Sleep 5 }
    try { Stop-Job $sj -EA 0; Remove-Job $sj -Force -EA 0 } catch {}
  }
  if ((Get-VM $vm).State -ne 'Off') { L "STOP: still $(VMS)"; return $false }
  L 'VM Off'; return $true
}

function RestoreSafe([string]$why) {
  L "RestoreSafe: $why"
  try {
    if ((Get-VM $vm).State -eq 'Running') {
      if (-not (WaitVM 30)) {
        L 'guest unreachable; graceful Stop-VM'
        if (-not (GracefulOff)) { return $false }
      } else {
        if (-not (GracefulOff)) { return $false }
      }
    } elseif ((Get-VM $vm).State -eq 'Saved') {
      L 'VM Saved; restore replaces saved state'
    }
    $names = @(Get-VMSnapshot -VMName $vm -EA 0 | ForEach-Object { $_.Name })
    $pick = $null
    if ($names -contains $cpFileNew) { $pick = $cpFileNew }
  elseif ($names -contains $cpFile0853) { $pick = $cpFile0853 }
    elseif ($names -contains $cpFile0852) { $pick = $cpFile0852 }
    elseif ($names -contains $cpFile0851) { $pick = $cpFile0851 }
    elseif ($names -contains $cpFile) { $pick = $cpFile }
    elseif ($names -contains $cpFix5) { $pick = $cpFix5 }
    else { L "no checkpoint among: $($names -join '; ')"; return $false }
    L "restoring '$pick'"
    Restore-VMSnapshot -VMSnapshot (Get-VMSnapshot -VMName $vm -Name $pick -EA Stop) -Confirm:$false
    Start-VM $vm
    if (-not (WaitVM 600)) { L "PS Direct not up after restore; $(VMS)"; return $false }
    L "restored OK; $(VMS)"; return $true
  } catch { L "RestoreSafe ERROR: $($_.Exception.Message)"; return $false }
}

function Tools {
  $t = (Get-Date).AddMinutes(3); $d = 5
  while ($true) {
    $s = $null
    try {
      $s = New-PSSession -VMName $vm -Credential $cred -EA Stop
      foreach ($e in 'ofquery.exe', 'ofranges.exe', 'ofoverlay.exe') {
        Copy-Item "C:\VMs\ofquery\$e" "C:\$e" -ToSession $s -Force -EA Stop
      }
      Remove-PSSession $s; return
    } catch {
      $m = $_.Exception.Message
      if ($s) { Remove-PSSession $s -EA 0 }
      if ((Get-Date) -gt $t) { L "copy tools failed: $m"; throw 'PSDIRECT_FAIL:copy tools' }
      L "copy tools retry in ${d}s ($m)"; Start-Sleep $d; $d = [math]::Min($d * 2, 30)
    }
  }
}

function Capture-HvBugcheck([string]$tag) {
  # Host-side read-only: Hyper-V Worker 18590 carries guest bugcheck codes (0x7F etc).
  try {
    if (-not $script:dir) { return }
    New-Item $script:dir -ItemType Directory -Force | Out-Null
    $start = (Get-Date).AddMinutes(-45)
    $ev = @(Get-WinEvent -FilterHashtable @{
      LogName = 'Microsoft-Windows-Hyper-V-Worker-Admin'
      Id = 18590
      StartTime = $start
    } -MaxEvents 20 -EA SilentlyContinue)
    if (-not $ev -or $ev.Count -eq 0) {
      $ev = @(Get-WinEvent -FilterHashtable @{
        LogName = 'System'; Id = 18590; StartTime = $start
      } -MaxEvents 20 -EA SilentlyContinue)
    }
    $out = Join-Path $script:dir ("hv_bugcheck_{0}_{1}.txt" -f $tag, (Get-Date -f yyyyMMdd_HHmmss))
    if (-not $ev -or $ev.Count -eq 0) {
      "no 18590 events since $($start.ToString('s'))" | Set-Content $out -Encoding utf8
      L "hv18590: none -> $out"
      return
    }
    $lines = foreach ($e in $ev) {
      $m = ("$($e.Message)" -replace '\s+', ' ')
      "{0} Id={1} {2}" -f $e.TimeCreated.ToString('yyyy-MM-dd HH:mm:ss'), $e.Id, $m.Substring(0, [Math]::Min(400, $m.Length))
    }
    $lines | Set-Content $out -Encoding utf8
    L "hv18590: $($ev.Count) event(s) -> $out"
    foreach ($ln in ($lines | Select-Object -First 5)) { L "  $ln" }
  } catch {
    L "hv18590 capture error: $($_.Exception.Message)"
  }
}

function FailFast([string]$reason) {
  L "FAIL_FAST: $reason"
  try { Capture-HvBugcheck 'failfast' } catch {}
  try { [void](RestoreSafe "failfast: $reason") } catch {}
  EndLine 'END_FAIL' $reason
  throw "FAIL:$reason"
}

function GuestEvents {
  G 'events_ntfs_disk' {
    $start = (Get-Date).AddMinutes(-30)
    $ev = Get-WinEvent -FilterHashtable @{ LogName = 'System'; StartTime = $start } -MaxEvents 80 -EA SilentlyContinue |
      Where-Object { $_.Id -in 140, 55, 98, 7, 11, 51 -or $_.ProviderName -match 'Ntfs|disk|Disk' } |
      Select-Object -First 20
    if (-not $ev) { 'no matching events' }
    else {
      $ev | ForEach-Object {
        $m = ("$($_.Message)" -replace '\s+', ' ')
        "$($_.TimeCreated.ToString('HH:mm:ss')) $($_.ProviderName) id=$($_.Id): $($m.Substring(0, [math]::Min(220, $m.Length)))"
      }
    }
  } 90
}

function Install-Stage3 {
  Tools
  $ss = $null
  try {
    $ss = New-PSSession -VMName $vm -Credential $cred -EA Stop
    Invoke-Command -Session $ss { New-Item C:\drivers\OfFilter3 -ItemType Directory -Force | Out-Null }
    Copy-Item "$stage\*" -Destination C:\drivers\OfFilter3 -ToSession $ss -Force
    Copy-Item C:\VMs\ofquery\ofoverlay.exe -Destination C:\ofoverlay.exe -ToSession $ss -Force
    Copy-Item "$stage\ofquery.exe" -Destination C:\ofquery.exe -ToSession $ss -Force
    $inst = Invoke-Command -Session $ss -ArgumentList $expectSys -ScriptBlock {
      param($expect)
      $o = @()
      certutil -addstore -f Root C:\drivers\OfFilter3\DriverTestSigning.cer | Out-Null
      certutil -addstore -f TrustedPublisher C:\drivers\OfFilter3\DriverTestSigning.cer | Out-Null
      $p = Start-Process rundll32.exe -ArgumentList 'setupapi.dll,InstallHinfSection DefaultInstall.NTamd64 128 C:\drivers\OfFilter3\OfFilter.inf' -Wait -PassThru
      $o += "InstallHinfSection exit=$($p.ExitCode)"
      $h1 = (Get-FileHash C:\Windows\System32\drivers\OfFilter.sys).Hash
      $h2 = (Get-FileHash C:\drivers\OfFilter3\OfFilter.sys).Hash
      $o += "installed=$h1 staged=$h2 expect=$expect"
      if ($h1 -ne $expect -or $h2 -ne $expect) { $o += 'HASHMISMATCH'; return $o }
      $pk = 'HKLM:\SYSTEM\CurrentControlSet\Services\OfFilter\Parameters'
      $uid = (Get-ItemProperty HKLM:\SYSTEM\MountedDevices).'\DosDevices\C:'
      New-ItemProperty $pk -Name VolumeUniqueId -PropertyType Binary -Value ([byte[]]$uid) -Force | Out-Null
      New-ItemProperty $pk -Name EnableCow -PropertyType DWord -Value 0 -Force | Out-Null
      $o += ((verifier /standard /driver OfFilter.sys 2>&1) -join ' | ')
      $o += "verifier=$(((verifier /query 2>&1) | Select-String 'OfFilter') -join '; ')"
      $o
    }
    Save 'a_install' $inst
    L "install: $($inst -join ' || ')"
    if ($inst -contains 'HASHMISMATCH') { throw 'stage3 hash mismatch after install' }
  } finally { if ($ss) { Remove-PSSession $ss -EA 0 } }
}

function Take-FileCheckpoint {
  if (Get-VMSnapshot -VMName $vm -Name $cpFile -EA SilentlyContinue) {
    L "removing existing '$cpFile'"
    Remove-VMSnapshot -VMName $vm -Name $cpFile -Confirm:$false
    $t = (Get-Date).AddMinutes(5)
    while ((Get-VMSnapshot -VMName $vm -Name $cpFile -EA 0) -and (Get-Date) -lt $t) { Start-Sleep 5 }
  }
  L "taking checkpoint '$cpFile' ($(VMS))"
  try { Checkpoint-VM -Name $vm -SnapshotName $cpFile -EA Stop }
  catch {
    L "Checkpoint while running failed: $($_.Exception.Message); stop then retry"
    if (-not (GracefulOff)) { FailFast 'could not stop for checkpoint' }
    Checkpoint-VM -Name $vm -SnapshotName $cpFile -EA Stop
    Start-VM $vm
    if (-not (WaitVM 600)) { FailFast 'PS Direct not up after checkpoint' }
  }
  L "checkpoint '$cpFile' taken; snaps=$((Get-VMSnapshot -VMName $vm | ForEach-Object Name) -join '; ')"
}

# ---------------- main ----------------
try {
  # Fresh run unless an in-progress STATUS=running state is being resumed mid-crash.
  $resume = LoadState
  if ($resume) {
    L "=== RESUME tag=$($script:tag) phase=$($script:phase) cycle=$($script:cycle) ==="
    New-Item $script:dir -ItemType Directory -Force | Out-Null
  } else {
    $script:tag = Get-Date -f 'yyyyMMdd_HHmmss'
    $env:OF_RUNTAG = $script:tag
    $script:log = "C:\VMs\phase2_file_test_$($script:tag).log"
    $script:dir = "C:\VMs\phase2_diag\file_$($script:tag)"
    New-Item C:\VMs\phase2_diag -ItemType Directory -Force | Out-Null
    New-Item $script:dir -ItemType Directory -Force | Out-Null
    $script:phase = 'A'; $script:cycle = 0
    SaveState
    L "=== phase2_file_test v2 start tag=$($script:tag) ==="
  }

  $isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
  L ("host={0} elevated={1}" -f $env:COMPUTERNAME, $isAdmin)
  L "VM $(VMS)"

  $staged = (Get-FileHash "$stage\OfFilter.sys").Hash
  if ($staged -ne $expectSys) { FailFast "stage3 hash $staged != $expectSys" }
  L "stage3 OfFilter.sys sha256 $staged"

  $pw = (Get-Content C:\VMs\devadmin_password.txt -Raw).Trim()
  $cred = New-Object PSCredential('.\devadmin', (ConvertTo-SecureString $pw -AsPlainText -Force))
  $pw = $null

  if ((Get-VM $vm).State -ne 'Running') { Start-VM $vm; L 'Start-VM' }
  if (-not (WaitVM 600)) { FailFast "PS Direct not up at start; $(VMS)" }

$cpNewExists = [bool](Get-VMSnapshot -VMName $vm -Name $cpFileNew -EA SilentlyContinue)
$cp0853Exists = [bool](Get-VMSnapshot -VMName $vm -Name $cpFile0853 -EA SilentlyContinue)
$cp0852Exists = [bool](Get-VMSnapshot -VMName $vm -Name $cpFile0852 -EA SilentlyContinue)
$cp0851Exists = [bool](Get-VMSnapshot -VMName $vm -Name $cpFile0851 -EA SilentlyContinue)
$cpExists = [bool](Get-VMSnapshot -VMName $vm -Name $cpFile -EA SilentlyContinue)
L "checkpoint '$cpFileNew' exists=$cpNewExists; '$cpFile0853' exists=$cp0853Exists; '$cpFile0852' exists=$cp0852Exists; '$cpFile0851' exists=$cp0851Exists; '$cpFile' exists=$cpExists"

  # Decide whether Phase A can be skipped (guest already on current stage3).
  $guestHash = $null
  try {
    $guestHash = [string](IC -What 'guest hash' { (Get-FileHash C:\Windows\System32\drivers\OfFilter.sys).Hash })
  } catch { $guestHash = $null }
  L "guest OfFilter.sys=$guestHash expect=$expectSys"
  $skipA = ($cpNewExists -or $cp0853Exists -or $cp0852Exists -or $cp0851Exists -or $cpExists) -and ($guestHash -eq $expectSys) -and ($script:phase -ne 'A' -or $resume)

  # New runs with matching hash+CP: skip A. New runs with CP but old hash: restore CP then reinstall (Phase A deploy).
  if (-not $resume) {
    $hasFileCp = $cpNewExists -or $cp0853Exists -or $cp0852Exists -or $cp0851Exists -or $cpExists
    if ($hasFileCp -and $guestHash -eq $expectSys) {
      L 'Phase A SKIP: file-backend checkpoint present and guest hash matches current stage3'
      $script:phase = 'B'; $script:cycle = 0; SaveState
      $skipA = $true
    } elseif ($hasFileCp -and $guestHash -ne $expectSys) {
      L 'Phase A REINSTALL path: restore preferred CP (v0.8.6.0>v0.8.5.3>v0.8.5.2>v0.8.5.1>staged) then install current stage3'
      if (-not (RestoreSafe 'reinstall from file-backend checkpoint')) { FailFast 'could not restore file-backend checkpoint' }
      $script:phase = 'A'; SaveState
      $skipA = $false
    } else {
      L 'Phase A FULL: no usable file-backend checkpoint / hash'
      $skipA = $false
    }
  } else {
    $skipA = ($script:phase -ne 'A')
  }

  # ========== PHASE A ==========
  if ($script:phase -eq 'A' -and -not $skipA) {
    L '---- PHASE A: thawed setup / stage3 deploy ----'

    $pre = IC -What 'precheck' {
      $par = Get-ItemProperty HKLM:\SYSTEM\CurrentControlSet\Services\OfFilter\Parameters -EA SilentlyContinue
      $h = (Get-FileHash C:\Windows\System32\drivers\OfFilter.sys -EA SilentlyContinue).Hash
      "hash=$h EnableCow=$($par.EnableCow) Backend=$($par.OverlayBackend) SlotCount=$($par.SlotCount) svc=$((sc.exe query OfFilter 2>&1 | Select-String STATE) -join '')"
    }
    L "precheck: $pre"
    # Exact EnableCow parse (not substring)
    $preCow = $null
    if ("$pre" -match '(?m)(?:^|\s)EnableCow=(\d+)(?:\s|$)') { $preCow = [int]$Matches[1] }
    if ($preCow -ne 0) {
      L 'EnableCow not 0; restoring to unfrozen checkpoint'
      if (-not (RestoreSafe 'need EnableCow=0')) { FailFast 'could not restore to unfrozen' }
    }

    [void](G 'power_restore_vss' {
      $o = @()
      $o += "hiberfilExists=$(Test-Path C:\hiberfil.sys)"
      $hb = (Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Control\Session Manager\Power' -Name HiberbootEnabled -EA SilentlyContinue).HiberbootEnabled
      $o += "HiberbootEnabled=$hb"
      try { $o += "RestorePoints=$((Get-ComputerRestorePoint -EA Stop | Measure-Object).Count)" }
      catch { $o += "RestorePoints=ERR" }
      try { $o += "vss=$((vssadmin list shadows /for=C: 2>&1) -join ' | ')" } catch { $o += 'vss=ERR' }
      $o += "freeDiskGB=$([math]::Round((Get-PSDrive C).Free / 1GB, 2))"
      $o
    } 90)

    [void](G 'disable_hiber_restore' {
      $o = @()
      $o += ((powercfg /h off 2>&1) -join ' | ')
      try { Disable-ComputerRestore -Drive 'C:\' -EA Stop; $o += 'Disable-ComputerRestore OK' }
      catch { $o += "Disable-ComputerRestore: $($_.Exception.Message)" }
      Set-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Control\Session Manager\Power' -Name HiberbootEnabled -Value 0 -Type DWord -Force -EA SilentlyContinue
      $o += "HiberbootEnabledNow=$((Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Control\Session Manager\Power' -Name HiberbootEnabled).HiberbootEnabled)"
      $o
    } 90)

    $freeProbe = G 'free_disk' { "freeDiskGB=$([math]::Round((Get-PSDrive C).Free / 1GB, 2))" } 90
    $freeGB = 0.0
    if ("$freeProbe" -match 'freeDiskGB=([0-9.]+)') { $freeGB = [double]$Matches[1] }
    L "freeDiskGB=$freeGB"
    if ($freeGB -lt 9.0) { FailFast "free disk ${freeGB}GB < 9GB (need 8GB overlay + margin)" }

    Install-Stage3

    # ofoverlay create only if file backend not already staged
    $needOv = $true
    $reg0 = G 'params_before_ofoverlay' {
      $pk = 'HKLM:\SYSTEM\CurrentControlSet\Services\OfFilter\Parameters'
      $p = Get-ItemProperty $pk -EA SilentlyContinue
      $fs = $null; try { $fs = (Get-ItemProperty $pk -Name OverlayFileSize).OverlayFileSize } catch {}
      $fsN = if ($fs -and $fs.Length -ge 8) { [BitConverter]::ToUInt64($fs, 0) } else { 0 }
      "EnableCow=$($p.EnableCow) Backend=$($p.OverlayBackend) FileSize=$fsN Path=$($p.OverlayPath) ovExists=$(Test-Path 'C:\OpenFreeze\overlay.bin')"
    } 90
    if ("$reg0" -match 'Backend=1' -and "$reg0" -match 'ovExists=True' -and "$reg0" -match 'FileSize=8589934592') {
      L 'ofoverlay already present from checkpoint; skipping create'
      $needOv = $false
    }
    if ($needOv) {
      $ov = G 'ofoverlay_create' {
        $o = @(& C:\ofoverlay.exe create 8 2>&1)
        $o
        '---SHOW---'
        @(& C:\ofoverlay.exe show 2>&1)
      } 120
      if (-not ("$ov" -match 'RESULT OK')) { FailFast 'ofoverlay create failed' }
    } else {
      [void](G 'ofoverlay_show' { @(& C:\ofoverlay.exe show 2>&1) } 90)
    }

    $reg = G 'params_after_ofoverlay' {
      $pk = 'HKLM:\SYSTEM\CurrentControlSet\Services\OfFilter\Parameters'
      $p = Get-ItemProperty $pk
      $ex = $null; try { $ex = (Get-ItemProperty $pk -Name OverlayExtents).OverlayExtents } catch {}
      $fs = $null; try { $fs = (Get-ItemProperty $pk -Name OverlayFileSize).OverlayFileSize } catch {}
      $fsN = if ($fs -and $fs.Length -ge 8) { [BitConverter]::ToUInt64($fs, 0) } else { 0 }
      "EnableCow=$($p.EnableCow) Backend=$($p.OverlayBackend) SlotCount=$($p.SlotCount) Gen=$($p.OverlayGeneration) FileSize=$fsN ExtentsBytes=$(if ($ex) { $ex.Length } else { 0 }) Path=$($p.OverlayPath)"
    } 90
    # If skipped create but registry/file looks invalid, recreate once.
    $extOk = ("$reg" -match 'ExtentsBytes=(\d+)') -and ([int]$Matches[1] -gt 0)
    $fsOk = ("$reg" -match 'FileSize=8589934592')
    if (-not $needOv -and (-not $extOk -or -not $fsOk -or ("$reg" -notmatch '(?m)(?:^|\s)Backend=1(?:\s|$)'))) {
      L "overlay validation failed after skip (reg=$reg); recreating"
      $ov = G 'ofoverlay_create' {
        $o = @(& C:\ofoverlay.exe create 8 2>&1)
        $o
        '---SHOW---'
        @(& C:\ofoverlay.exe show 2>&1)
      } 120
      if (-not ("$ov" -match 'RESULT OK')) { FailFast 'ofoverlay recreate failed' }
      $reg = G 'params_after_ofoverlay_recreate' {
        $pk = 'HKLM:\SYSTEM\CurrentControlSet\Services\OfFilter\Parameters'
        $p = Get-ItemProperty $pk
        $ex = $null; try { $ex = (Get-ItemProperty $pk -Name OverlayExtents).OverlayExtents } catch {}
        $fs = $null; try { $fs = (Get-ItemProperty $pk -Name OverlayFileSize).OverlayFileSize } catch {}
        $fsN = if ($fs -and $fs.Length -ge 8) { [BitConverter]::ToUInt64($fs, 0) } else { 0 }
        "EnableCow=$($p.EnableCow) Backend=$($p.OverlayBackend) SlotCount=$($p.SlotCount) Gen=$($p.OverlayGeneration) FileSize=$fsN ExtentsBytes=$(if ($ex) { $ex.Length } else { 0 }) Path=$($p.OverlayPath)"
      } 90
    }
    if ("$reg" -notmatch '(?m)(?:^|\s)Backend=1(?:\s|$)') { FailFast "OverlayBackend not 1 ($reg)" }

    if (-not (Reboot 'post-stage3-thawed')) { FailFast 'reboot after stage3 install failed' }

    $post = IC -What 'post-reboot verify' {
      $h = (Get-FileHash C:\Windows\System32\drivers\OfFilter.sys).Hash
      $par = Get-ItemProperty HKLM:\SYSTEM\CurrentControlSet\Services\OfFilter\Parameters
      $svc = (sc.exe query OfFilter | Select-String STATE) -join ''
      $q = (& C:\ofquery.exe 2>&1) -join ' '
      "hash=$h EnableCow=$($par.EnableCow) Backend=$($par.OverlayBackend) SlotCount=$($par.SlotCount) svc=$svc ofquery=$q"
    }
    L "post-reboot: $post"
    if ("$post" -notmatch "hash=$expectSys") { FailFast "loaded driver not current stage3" }
    if ("$post" -notmatch 'RUNNING') { FailFast 'OfFilter not RUNNING' }
    $postCow = $null
    if ("$post" -match '(?m)(?:^|\s)EnableCow=(\d+)(?:\s|$)') { $postCow = [int]$Matches[1] }
    if ($postCow -ne 0) { FailFast 'EnableCow not 0 after thawed reboot' }
    $pq = Parse-OfQuery "$post"
    L "post ofquery parse: cow=$($pq.cow) state=$($pq.state) target=$($pq.target) reason=$($pq.reason) initStep=$($pq.initStep) initStatus=$($pq.initStatus)"
    if (OfQuery-IsFrozen $pq) { FailFast 'unexpectedly frozen after thawed reboot' }

    Take-FileCheckpoint
    # Preferred resume/restore target for this driver build.
    if (Get-VMSnapshot -VMName $vm -Name $cpFileNew -EA SilentlyContinue) {
      L "removing existing '$cpFileNew'"
      Remove-VMSnapshot -VMName $vm -Name $cpFileNew -Confirm:$false
      $tRm = (Get-Date).AddMinutes(5)
      while ((Get-VMSnapshot -VMName $vm -Name $cpFileNew -EA 0) -and (Get-Date) -lt $tRm) { Start-Sleep 5 }
    }
    L "taking checkpoint '$cpFileNew' ($(VMS))"
    try { Checkpoint-VM -Name $vm -SnapshotName $cpFileNew -EA Stop }
    catch {
      L "Checkpoint '$cpFileNew' while running failed: $($_.Exception.Message); stop then retry"
      if (-not (GracefulOff)) { FailFast "could not stop for checkpoint $cpFileNew" }
      Checkpoint-VM -Name $vm -SnapshotName $cpFileNew -EA Stop
      Start-VM $vm
      if (-not (WaitVM 600)) { FailFast 'PS Direct not up after new checkpoint' }
    }
    L "checkpoint '$cpFileNew' taken; snaps=$((Get-VMSnapshot -VMName $vm | ForEach-Object Name) -join '; ')"

    $script:phase = 'B'; $script:cycle = 0; SaveState
  }

  # ========== PHASE B ==========
  if ($script:phase -eq 'B') {
    L '---- PHASE B: negative gate (bad VolumeUniqueId + marker survive) ----'
    Tools

    [void](G 'neg_backup_uid' {
      $pk = 'HKLM:\SYSTEM\CurrentControlSet\Services\OfFilter\Parameters'
      $uid = [byte[]](Get-ItemProperty $pk).VolumeUniqueId
      [IO.File]::WriteAllBytes('C:\of_uid_backup.bin', $uid)
      "backed uidBytes=$($uid.Length)"
    } 90)

    [void](G 'neg_corrupt' {
      $pk = 'HKLM:\SYSTEM\CurrentControlSet\Services\OfFilter\Parameters'
      $bad = New-Object byte[] 16
      (New-Object Random).NextBytes($bad)
      New-ItemProperty $pk -Name VolumeUniqueId -PropertyType Binary -Value $bad -Force | Out-Null
      Set-ItemProperty $pk -Name EnableCow -Value 1
      $p = Get-ItemProperty $pk
      "EnableCow=$($p.EnableCow) Backend=$($p.OverlayBackend) uid0=$($p.VolumeUniqueId[0])"
    } 90)

    if (-not (Reboot 'neg-gate-boot')) { FailFast 'neg-gate reboot failed (guest >10 min)' }

    $negQraw = G 'neg_ofquery' {
      $q = (& C:\ofquery.exe 2>&1) -join ' '
      $par = Get-ItemProperty HKLM:\SYSTEM\CurrentControlSet\Services\OfFilter\Parameters
      "ofquery=$q EnableCow=$($par.EnableCow) Backend=$($par.OverlayBackend)"
    } 90
    $negQ = Parse-OfQuery "$negQraw"
    L "neg ofquery parse: ok=$($negQ.ok) cow=$($negQ.cow) state=$($negQ.state) target=$($negQ.target) reason=$($negQ.reason) initStep=$($negQ.initStep) initStatus=$($negQ.initStatus) raw=$negQraw"
    if (OfQuery-IsFrozen $negQ) { FailFast "negative gate: ofquery reports FROZEN despite bad VolumeUniqueId ($negQraw)" }
    if (-not (OfQuery-IsPassthroughOrRefused $negQ)) {
      FailFast "negative gate: ofquery did not report PASSTHROUGH/refused ($negQraw)"
    }

    $marker = "of_neg_$($script:tag).txt"
    [void](IC -What 'neg_seed_marker' -A @($marker) {
      param($m)
      Set-Content C:\of_neg_marker_name.txt $m -Encoding ascii -Force
      $payload = "neg-marker $(Get-Date -f o)"
      Set-Content "C:\$m" $payload -Encoding ascii -Force
      "wrote=$m"
    })

    if (-not (Reboot 'neg-gate-marker')) { FailFast 'neg-gate marker reboot failed' }

    $surv = G 'neg_marker_survive' {
      $m = (Get-Content C:\of_neg_marker_name.txt -Raw).Trim()
      $exists = Test-Path "C:\$m"
      $q = (& C:\ofquery.exe 2>&1) -join ' '
      "markerExists=$exists marker=$m ofquery=$q"
    } 90
    L "neg survive: $surv"
    if ("$surv" -notmatch '(?m)(?:^|\s)markerExists=True(?:\s|$)') {
      FailFast 'negative gate: marker DID NOT survive (looks frozen)'
    }
    $survQ = Parse-OfQuery "$surv"
    L "surv ofquery parse: cow=$($survQ.cow) state=$($survQ.state) target=$($survQ.target) reason=$($survQ.reason) initStep=$($survQ.initStep) initStatus=$($survQ.initStatus)"
    if (OfQuery-IsFrozen $survQ) { FailFast "negative gate: frozen after marker reboot ($surv)" }

    [void](G 'neg_restore_uid' {
      $pk = 'HKLM:\SYSTEM\CurrentControlSet\Services\OfFilter\Parameters'
      $uid = [IO.File]::ReadAllBytes('C:\of_uid_backup.bin')
      New-ItemProperty $pk -Name VolumeUniqueId -PropertyType Binary -Value $uid -Force | Out-Null
      Set-ItemProperty $pk -Name EnableCow -Value 0
      $p = Get-ItemProperty $pk
      "EnableCow=$($p.EnableCow) uidBytes=$($uid.Length) Backend=$($p.OverlayBackend)"
    } 90)

    if (-not (Reboot 'neg-restore-thawed')) { FailFast 'reboot after neg restore failed' }
    $vraw = G 'neg_verify_thawed' {
      $q = (& C:\ofquery.exe 2>&1) -join ' '
      $par = Get-ItemProperty HKLM:\SYSTEM\CurrentControlSet\Services\OfFilter\Parameters
      "ofquery=$q EnableCow=$($par.EnableCow) Backend=$($par.OverlayBackend)"
    } 90
    $vq = Parse-OfQuery "$vraw"
    L "verify ofquery parse: cow=$($vq.cow) state=$($vq.state) target=$($vq.target) reason=$($vq.reason) initStep=$($vq.initStep) initStatus=$($vq.initStatus)"
    if (OfQuery-IsFrozen $vq) { FailFast "still frozen after neg restore ($vraw)" }

    $script:phase = 'C'; $script:cycle = 0; SaveState
  }

  # ========== PHASE C ==========
  if ($script:phase -eq 'C') {
    L "---- PHASE C: frozen gate + $cyclesTarget cycles (at cycle=$($script:cycle)) ----"
    Tools

    if ($script:cycle -le 0) {
      [void](G 'ofranges_set' { (& C:\ofranges.exe set 2>&1) -join ' | ' } 90)

      $arm = G 'arm_freeze' {
        $pk = 'HKLM:\SYSTEM\CurrentControlSet\Services\OfFilter\Parameters'
        Set-ItemProperty $pk -Name OverlayBackend -Value 1
        Set-ItemProperty $pk -Name EnableCow -Value 1
        $p = Get-ItemProperty $pk
        "EnableCow=$($p.EnableCow) Backend=$($p.OverlayBackend) SlotCount=$($p.SlotCount)"
      } 90
      $armCow = $null; $armBe = $null
      if ("$arm" -match '(?m)(?:^|\s)EnableCow=(\d+)(?:\s|$)') { $armCow = [int]$Matches[1] }
      if ("$arm" -match '(?m)(?:^|\s)Backend=(\d+)(?:\s|$)') { $armBe = [int]$Matches[1] }
      if ($armCow -ne 1 -or $armBe -ne 1) { FailFast "arm freeze failed ($arm)" }

      if (-not (Reboot 'first-freeze')) { Capture-HvBugcheck 'first-freeze'; FailFast 'first freeze reboot failed / unreachable >10 min' }

      $bootRaw = G 'c_boot_ofquery' {
        $q = (& C:\ofquery.exe 2>&1) -join ' '
        $par = Get-ItemProperty HKLM:\SYSTEM\CurrentControlSet\Services\OfFilter\Parameters
        $os = Get-CimInstance Win32_OperatingSystem
        "ofquery=$q EnableCow=$($par.EnableCow) Backend=$($par.OverlayBackend) SlotCount=$($par.SlotCount) freeMB=$([math]::Round($os.FreePhysicalMemory / 1KB)) uptimeS=$([math]::Round(((Get-Date) - $os.LastBootUpTime).TotalSeconds))"
      } 90
      $bq = Parse-OfQuery "$bootRaw"
      L "first freeze parse: cow=$($bq.cow) state=$($bq.state) slots=$($bq.slotsUsed) full=$($bq.full) target=$($bq.target) reason=$($bq.reason) initStep=$($bq.initStep) initStatus=$($bq.initStatus)"
      if (-not (OfQuery-IsFrozen $bq)) { FailFast "not frozen after EnableCow=1 ($bootRaw)" }
      if ("$bootRaw" -notmatch '(?m)(?:^|\s)Backend=1(?:\s|$)') { FailFast "Backend not 1 ($bootRaw)" }
      if ($null -ne $bq.full -and $bq.full -gt 0) { FailFast "overlay full at boot ($bootRaw)" }
      $script:cycle = 1; SaveState
    }

    for (; $script:cycle -le $cyclesTarget; $script:cycle++) {
      $n = $script:cycle
      L "==== CYCLE ${n} / $cyclesTarget ===="
      SaveState
      Tools

      $stRaw = G "c${n}_state" {
        $q = (& C:\ofquery.exe 2>&1) -join ' '
        $os = Get-CimInstance Win32_OperatingSystem
        $par = Get-ItemProperty HKLM:\SYSTEM\CurrentControlSet\Services\OfFilter\Parameters
        "ofquery=$q freeMB=$([math]::Round($os.FreePhysicalMemory / 1KB)) EnableCow=$($par.EnableCow) Backend=$($par.OverlayBackend) SlotCount=$($par.SlotCount) uptimeS=$([math]::Round(((Get-Date) - $os.LastBootUpTime).TotalSeconds))"
      } 90
      if (-not $stRaw) { FailFast "cycle ${n}: ofquery timeout / unreachable" }
      $sq = Parse-OfQuery "$stRaw"
      L "cycle ${n} parse: cow=$($sq.cow) state=$($sq.state) slots=$($sq.slotsUsed) full=$($sq.full) initStep=$($sq.initStep) initStatus=$($sq.initStatus) free=$((Select-String -InputObject "$stRaw" -Pattern 'freeMB=(\d+)').Matches.Groups[1].Value)"
      if (-not (OfQuery-IsFrozen $sq)) { FailFast "cycle ${n}: not frozen ($stRaw)" }
      $uptime = $null
      if ("$stRaw" -match 'uptimeS=(\d+)') { $uptime = [int]$Matches[1] }
      if ($null -ne $uptime -and $uptime -lt 180 -and $null -ne $sq.full -and $sq.full -gt 0) {
        FailFast "cycle ${n}: overlay full at boot ($stRaw)"
      }

      $marker = "of_cycle_${n}_$($script:tag).txt"
      [void](IC -What "c${n}_seed_marker" -A @($marker) {
        param($m) Set-Content C:\of_cycle_marker_name.txt $m -Encoding ascii -Force; "seeded=$m"
      })
      $rw = G "c${n}_raw" {
        $markerLocal = (Get-Content C:\of_cycle_marker_name.txt -Raw).Trim()
        $path = "C:\$markerLocal"
        $payload = "cycle-marker $(Get-Date -f o)"
        Set-Content -Path $path -Value $payload -Encoding ascii -Force
        $readBack = (Get-Content $path -Raw).Trim()
        $ovOk = 'overlayMissing'
        $ovPath = 'C:\OpenFreeze\overlay.bin'
        if (Test-Path $ovPath) {
          $fs = [IO.File]::Open($ovPath, [IO.FileMode]::Open, [IO.FileAccess]::ReadWrite, [IO.FileShare]::ReadWrite)
          try {
            $msg = [Text.Encoding]::ASCII.GetBytes(('C' + (Get-Date -f 'HHmm')))
            $off = [int64]65536
            $fs.Seek($off, 'Begin') | Out-Null
            $fs.Write($msg, 0, [math]::Min(4, $msg.Length))
            $fs.Flush($true)
            $fs.Seek($off, 'Begin') | Out-Null
            $chk = New-Object byte[] 4
            [void]$fs.Read($chk, 0, 4)
            $ovOk = "overlayPassThrough=$([Text.Encoding]::ASCII.GetString($chk).Trim([char]0))"
          } finally { $fs.Dispose() }
        }
        "marker=$markerLocal writeOK=$($readBack -eq $payload) $ovOk"
      } 90
      if ("$rw" -notmatch '(?m)(?:^|\s)writeOK=True(?:\s|$)') { FailFast "cycle ${n}: read-after-write failed ($rw)" }
      if ("$rw" -notmatch 'overlayPassThrough=') { L "WARN cycle ${n}: overlay passthrough check inconclusive ($rw)" }

      [void](GuestEvents)

      if (-not (Reboot "cycle-${n}-reboot")) { FailFast "cycle ${n}: reboot failed / unreachable >10 min" }

      # Host-known $marker is authoritative. of_cycle_marker_name.txt is itself a
      # frozen write; when CoW works it is gone after reboot. Old code did
      # Test-Path "C:\$null" => True => false markerGone=False / SURVIVED.
      $afterRaw = G "c${n}_after" {
        param($expectMarker)
        # Echo first so host logs prove ArgList delivery even on empty-expect.
        $recv = "receivedExpect=$expectMarker"
        if ([string]::IsNullOrWhiteSpace([string]$expectMarker)) {
          "$recv markerGone=False marker=MISSING nameFileExists=False markerExists=False err=empty-expect"
          return
        }
        $nameExists = Test-Path -LiteralPath 'C:\of_cycle_marker_name.txt'
        $markerExists = Test-Path -LiteralPath "C:\$expectMarker"
        $gone = -not $markerExists
        $q = (& C:\ofquery.exe 2>&1) -join ' '
        $os = Get-CimInstance Win32_OperatingSystem
        "$recv markerGone=$gone marker=$expectMarker nameFileExists=$nameExists markerExists=$markerExists ofquery=$q freeMB=$([math]::Round($os.FreePhysicalMemory / 1KB)) uptimeS=$([math]::Round(((Get-Date) - $os.LastBootUpTime).TotalSeconds))"
      } 90 -ArgList @($marker)
      $aq = Parse-OfQuery "$afterRaw"
      L "cycle ${n} after: receivedExpect=$((Select-String -InputObject "$afterRaw" -Pattern 'receivedExpect=([^\s]+)').Matches.Groups[1].Value) markerGone=$((Select-String -InputObject "$afterRaw" -Pattern 'markerGone=(\w+)').Matches.Groups[1].Value) markerExists=$((Select-String -InputObject "$afterRaw" -Pattern 'markerExists=(\w+)').Matches.Groups[1].Value) nameFileExists=$((Select-String -InputObject "$afterRaw" -Pattern 'nameFileExists=(\w+)').Matches.Groups[1].Value) cow=$($aq.cow) state=$($aq.state) slots=$($aq.slotsUsed) full=$($aq.full) initStep=$($aq.initStep) initStatus=$($aq.initStatus)"
      if ("$afterRaw" -match '(?m)(?:^|\s)err=empty-expect(?:\s|$)' -or "$afterRaw" -match '(?m)(?:^|\s)marker=MISSING(?:\s|$)' -or "$afterRaw" -notmatch 'receivedExpect=') {
        FailFast "cycle ${n}: HARNESS_ERROR empty expectMarker / ArgList delivery failed ($afterRaw)"
      }
      if ("$afterRaw" -notmatch '(?m)(?:^|\s)markerGone=True(?:\s|$)') {
        FailFast "cycle ${n}: marker SURVIVED reboot (CoW not discarding) ($afterRaw)"
      }
      if (-not (OfQuery-IsFrozen $aq)) { FailFast "cycle ${n}: not frozen after reboot ($afterRaw)" }
      if ($null -ne $aq.full -and $aq.full -gt 0) { FailFast "cycle ${n}: overlay full at boot ($afterRaw)" }

      SaveState
    }

    L "PHASE C complete: $cyclesTarget cycles OK"
    $script:phase = 'D'; SaveState
  }

  # ========== PHASE D ==========
  if ($script:phase -eq 'D') {
    L '---- PHASE D: thaw-on-next-boot wrap-up ----'
    Tools

    # D1: while frozen, set ThawNextBoot via IOCTL (registry EnableCow=0 would be discarded)
    $thawSetRaw = G 'thaw_set' {
      $o = (& C:\ofquery.exe thaw-set 2>&1) -join ' '
      $q = (& C:\ofquery.exe 2>&1) -join ' '
      "thawSet=$o ofquery=$q"
    } 90
    L "thaw-set: $thawSetRaw"
    if ("$thawSetRaw" -notmatch 'RESULT OK thaw-set') { FailFast "thaw-set IOCTL failed ($thawSetRaw)" }
    $tsq = Parse-OfQuery "$thawSetRaw"
    if (-not (OfQuery-IsFrozen $tsq)) { FailFast "expected still FROZEN after thaw-set ($thawSetRaw)" }
    if ($tsq.thawNext -ne 1) { FailFast "thawNext not 1 after thaw-set ($thawSetRaw)" }

    if (-not (Reboot 'thaw-next-boot')) { FailFast 'reboot after thaw-set failed' }

    # D2: expect PASSTHROUGH reason=ThawRequested (EnableCow still 1; flag sticky)
    $thawBootRaw = G 'thaw_boot' {
      $q = (& C:\ofquery.exe 2>&1) -join ' '
      $par = Get-ItemProperty HKLM:\SYSTEM\CurrentControlSet\Services\OfFilter\Parameters
      "ofquery=$q EnableCow=$($par.EnableCow) Backend=$($par.OverlayBackend)"
    } 90
    $tbq = Parse-OfQuery "$thawBootRaw"
    L "thaw-boot parse: cow=$($tbq.cow) state=$($tbq.state) reason=$($tbq.reason) cfgValid=$($tbq.cfgValid) thawNext=$($tbq.thawNext) cfgSeq=$($tbq.cfgSeq) initStep=$($tbq.initStep)"
    if (OfQuery-IsFrozen $tbq) { FailFast "still frozen after thaw-next boot ($thawBootRaw)" }
    if ($tbq.reason -ne 'ThawRequested') { FailFast "expected reason=ThawRequested ($thawBootRaw)" }
    if ($tbq.thawNext -ne 1) { FailFast "thawNext should remain 1 until tool clears ($thawBootRaw)" }
    if ("$thawBootRaw" -notmatch '(?m)(?:^|\s)EnableCow=1(?:\s|$)') { FailFast "EnableCow should still be 1 on sticky thaw boot ($thawBootRaw)" }

    # D3: persist EnableCow=0 now that writes stick, clear thaw flag
    [void](G 'persist_unfreeze' {
      Set-ItemProperty HKLM:\SYSTEM\CurrentControlSet\Services\OfFilter\Parameters -Name EnableCow -Value 0
      $clr = (& C:\ofquery.exe thaw-clear 2>&1) -join ' '
      $q = (& C:\ofquery.exe 2>&1) -join ' '
      $par = Get-ItemProperty HKLM:\SYSTEM\CurrentControlSet\Services\OfFilter\Parameters
      "clear=$clr ofquery=$q EnableCow=$($par.EnableCow)"
    } 90)

    if (-not (Reboot 'plain-unfrozen')) { FailFast 'reboot after EnableCow=0 + thaw-clear failed' }

    $finRaw = G 'final_state' {
      $q = (& C:\ofquery.exe 2>&1) -join ' '
      $par = Get-ItemProperty HKLM:\SYSTEM\CurrentControlSet\Services\OfFilter\Parameters
      $h = (Get-FileHash C:\Windows\System32\drivers\OfFilter.sys).Hash
      "ofquery=$q EnableCow=$($par.EnableCow) Backend=$($par.OverlayBackend) hash=$h"
    } 90
    $fq = Parse-OfQuery "$finRaw"
    L "final ofquery parse: cow=$($fq.cow) state=$($fq.state) reason=$($fq.reason) thawNext=$($fq.thawNext) cfgValid=$($fq.cfgValid) initStep=$($fq.initStep)"
    if (OfQuery-IsFrozen $fq) { FailFast "still frozen after plain unfreeze ($finRaw)" }
    if ("$finRaw" -notmatch '(?m)(?:^|\s)EnableCow=0(?:\s|$)') { FailFast "EnableCow not 0 after plain unfreeze ($finRaw)" }
    if ($fq.reason -eq 'ThawRequested') { FailFast "ThawRequested should be cleared ($finRaw)" }

    # D4 (cheap negative): corrupt config sector refuses freeze
    $negRaw = G 'corrupt_cfg_neg' {
      $path = 'C:\OpenFreeze\overlay.bin'
      if (-not (Test-Path $path)) { return 'err=no-overlay' }
      $fs = [IO.File]::Open($path, 'Open', 'ReadWrite', 'ReadWrite')
      try {
        $junk = New-Object byte[] 4096
        for ($i = 0; $i -lt 4096; $i++) { $junk[$i] = 0x41 }
        $fs.Position = 0
        $fs.Write($junk, 0, 4096)
        $fs.Flush($true)
      } finally { $fs.Close() }
      Set-ItemProperty HKLM:\SYSTEM\CurrentControlSet\Services\OfFilter\Parameters -Name EnableCow -Value 1
      'corrupted=1 EnableCow=1'
    } 90
    L "corrupt prep: $negRaw"
    if ("$negRaw" -match 'err=no-overlay') { FailFast 'overlay.bin missing for corrupt neg' }
    if (-not (Reboot 'corrupt-cfg')) { FailFast 'reboot for corrupt cfg failed' }
    $corRaw = G 'corrupt_check' {
      $q = (& C:\ofquery.exe 2>&1) -join ' '
      $par = Get-ItemProperty HKLM:\SYSTEM\CurrentControlSet\Services\OfFilter\Parameters
      "ofquery=$q EnableCow=$($par.EnableCow)"
    } 90
    $cq = Parse-OfQuery "$corRaw"
    L "corrupt parse: cow=$($cq.cow) state=$($cq.state) reason=$($cq.reason) cfgValid=$($cq.cfgValid)"
    if (OfQuery-IsFrozen $cq) { FailFast "corrupt config must not freeze ($corRaw)" }
    if ($cq.reason -ne 'ConfigCorrupt') { FailFast "expected reason=ConfigCorrupt ($corRaw)" }

    # Restore clean: zero sector, EnableCow=0
    [void](G 'corrupt_cleanup' {
      $path = 'C:\OpenFreeze\overlay.bin'
      $fs = [IO.File]::Open($path, 'Open', 'ReadWrite', 'ReadWrite')
      try {
        $zero = New-Object byte[] 4096
        $fs.Position = 0
        $fs.Write($zero, 0, 4096)
        $fs.Flush($true)
      } finally { $fs.Close() }
      Set-ItemProperty HKLM:\SYSTEM\CurrentControlSet\Services\OfFilter\Parameters -Name EnableCow -Value 0
      try { (& C:\ofquery.exe thaw-clear 2>&1) | Out-Null } catch {}
      'cleaned=1'
    } 90)
    if (-not (Reboot 'post-corrupt-cleanup')) { FailFast 'reboot after corrupt cleanup failed' }
    $cleanRaw = G 'cleanup_verify' {
      $q = (& C:\ofquery.exe 2>&1) -join ' '
      $par = Get-ItemProperty HKLM:\SYSTEM\CurrentControlSet\Services\OfFilter\Parameters
      "ofquery=$q EnableCow=$($par.EnableCow)"
    } 90
    $clq = Parse-OfQuery "$cleanRaw"
    if (OfQuery-IsFrozen $clq) { FailFast "frozen after corrupt cleanup ($cleanRaw)" }
    if ("$cleanRaw" -notmatch '(?m)(?:^|\s)EnableCow=0(?:\s|$)') { FailFast "EnableCow not 0 after cleanup ($cleanRaw)" }

    [void](G 'chkntfs' { ((chkntfs C: 2>&1) -join ' | ') } 90)
    $ck = G 'chkdsk_scan' { $o = chkdsk C: /scan 2>&1; "exit=$LASTEXITCODE"; $o } 600
    if ($ck) {
      L "chkdsk summary: $(@($ck)[0]) | $((@($ck) | Select-String 'found no problems|Windows has scanned|errors|corrupt' | Select-Object -First 6) -join ' | ')"
    } else {
      L 'chkdsk /scan TIMEOUT or failed (logged)'
    }

    L "DONE VM $(VMS); evidence $($script:dir); log $($script:log)"
    EndLine 'END_PASS'
  }

} catch {
  if ("$($_.Exception.Message)" -match '^FAIL:') {
    # FailFast already wrote END_FAIL
  } else {
    L "ERROR: $($_.Exception.Message) at line $($_.InvocationInfo.ScriptLineNumber)"
    try { [void](RestoreSafe "catch: $($_.Exception.Message)") } catch {}
    if (-not $script:EndWritten) { EndLine 'END_FAIL' $_.Exception.Message }
  }
} finally {
  $cred = $null
  if (-not $script:EndWritten) { EndLine 'END_ABORT' 'script exited without END status' }
  L "log=$($script:log)"
}

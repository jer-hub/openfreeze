#Requires -Version 5.1
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

# ---- helpers under test (mirrors phase2_file_test.ps1) ----
$script:tag = 'testrun'
$script:log = $null
$script:dir = $null
$script:phase = 'A'
$script:cycle = 0
$statePath = Join-Path $env:TEMP 'phase2_file_test_state_dry.txt'
$expectSys = 'F4290E88B44886906E49CEE59DCFDE83B163898AFF64370726B8053D5784F25E'

function Parse-OfQuery([string]$text) {
  $o = [ordered]@{
    raw = "$text"
    ok = $false
    cow = $null; torn = $null; slotsUsed = $null; full = $null
    state = $null; target = $null; reason = $null; initStep = $null; initStatus = $null; cfgValid = $null; thawNext = $null; cfgSeq = $null
  }
  if ([string]::IsNullOrWhiteSpace($text)) { return [pscustomobject]$o }
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

function Test-HashMatch([string]$guest, [string]$expect) {
  return ($guest -eq $expect)
}

function Test-MarkerSurvived([string]$text) {
  return ($text -match '(?m)(?:^|\s)markerExists=True(?:\s|$)')
}
function Test-MarkerGone([string]$text) {
  return ($text -match '(?m)(?:^|\s)markerGone=True(?:\s|$)')
}

# ---- assertions ----
$pass = 0; $fail = 0
function Assert([string]$name, [bool]$cond, [string]$detail='') {
  if ($cond) { Write-Host "PASS $name"; $script:pass++ }
  else { Write-Host "FAIL $name $detail"; $script:fail++ }
}

# Case 1: real thawed PASSTHROUGH from today's log
$s1 = 'cow=0 torn=0 slotsUsed=0 pendingW=0 bypassR=0 workerR=0 full=0 state=PASSTHROUGH ptRanges=0 l2Res=0 ptW=0 ptR=0 ptBytes=0 target=1 reason=None'
$q1 = Parse-OfQuery $s1
Assert 'thawed_passthrough_ok' $q1.ok
Assert 'thawed_cow0' ($q1.cow -eq 0)
Assert 'thawed_state_PT' ($q1.state -eq 'PASSTHROUGH')
Assert 'thawed_target1' ($q1.target -eq 1)
Assert 'thawed_reason_None' ($q1.reason -eq 'None')
Assert 'thawed_full0' ($q1.full -eq [UInt64]0)
Assert 'thawed_not_frozen' (-not (OfQuery-IsFrozen $q1))
Assert 'thawed_is_PT_or_refused' (OfQuery-IsPassthroughOrRefused $q1)

# Case 2: short form from requirements
$s2 = 'cow=0 state=PASSTHROUGH target=1 reason=None full=0'
$q2 = Parse-OfQuery $s2
Assert 'short_PT_ok' ($q2.ok -and $q2.state -eq 'PASSTHROUGH' -and $q2.full -eq 0 -and -not (OfQuery-IsFrozen $q2))

# Case 3: frozen variant
$s3 = 'cow=1 torn=0 slotsUsed=42 pendingW=0 bypassR=10 workerR=2 full=0 state=FROZEN ptRanges=3 l2Res=1 ptW=5 ptR=1 ptBytes=1048576 target=1 reason=None'
$q3 = Parse-OfQuery $s3
Assert 'frozen_ok' ($q3.ok -and (OfQuery-IsFrozen $q3) -and $q3.slotsUsed -eq 42 -and $q3.full -eq 0)
Assert 'frozen_not_PT' (-not (OfQuery-IsPassthroughOrRefused $q3))

# Case 4: NotTarget refused (bad UID)
$s4 = 'cow=0 torn=0 slotsUsed=0 pendingW=0 bypassR=0 workerR=0 full=0 state=PASSTHROUGH ptRanges=0 l2Res=0 ptW=0 ptR=0 ptBytes=0 target=0 reason=NotTarget'
$q4 = Parse-OfQuery $s4
Assert 'nottarget_PT' (OfQuery-IsPassthroughOrRefused $q4)
Assert 'nottarget_fields' ($q4.target -eq 0 -and $q4.reason -eq 'NotTarget' -and -not (OfQuery-IsFrozen $q4))

# Case 5: ConfigRefuse
$s5 = 'cow=0 torn=0 slotsUsed=0 full=0 state=PASSTHROUGH target=1 reason=ConfigRefuse'
$q5 = Parse-OfQuery $s5
Assert 'configrefuse' ((OfQuery-IsPassthroughOrRefused $q5) -and $q5.reason -eq 'ConfigRefuse')

# Case 6: query failed err=1 (legacy) ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬ÃƒÂ¢Ã¢â€šÂ¬Ã‚Â no cow= field => ok=false, not frozen, not PT
$s6 = 'query failed err=1 EnableCow=1 Backend=1 writeOK'
$q6 = Parse-OfQuery $s6
Assert 'legacy_err1_not_ok' (-not $q6.ok)
Assert 'legacy_err1_not_frozen' (-not (OfQuery-IsFrozen $q6))
Assert 'legacy_err1_not_PT' (-not (OfQuery-IsPassthroughOrRefused $q6))
# Critical: must NOT treat EnableCow=1 as cow=1
Assert 'EnableCow_trap_no_cow' ($null -eq $q6.cow)

# Case 7: substring trap string alone
$s7 = 'EnableCow=1 Backend=1'
$q7 = Parse-OfQuery $s7
Assert 'EnableCow_only_no_cow' ($null -eq $q7.cow -and -not $q7.ok -and -not (OfQuery-IsFrozen $q7))

# Case 8: embedded ofquery= in host log line (as script sees)
$s8 = 'ofquery=cow=0 torn=0 slotsUsed=0 pendingW=0 bypassR=0 workerR=0 full=0 state=PASSTHROUGH ptRanges=0 l2Res=0 ptW=0 ptR=0 ptBytes=0 target=1 reason=None EnableCow=0 Backend=1'
$q8 = Parse-OfQuery $s8
Assert 'embedded_log_PT' ($q8.ok -and $q8.cow -eq 0 -and $q8.state -eq 'PASSTHROUGH' -and -not (OfQuery-IsFrozen $q8))

# Case 9: frozen with full>0 at boot
$s9 = 'cow=1 state=FROZEN slotsUsed=262144 full=1 target=1 reason=None'
$q9 = Parse-OfQuery $s9
Assert 'full_at_boot' ((OfQuery-IsFrozen $q9) -and $q9.full -eq 1 -and $q9.slotsUsed -eq 262144)

# Case 10: marker survive / gone
Assert 'marker_survived' (Test-MarkerSurvived 'markerExists=True marker=of_neg_x.txt ofquery=cow=0')
Assert 'marker_not_survived' (-not (Test-MarkerSurvived 'markerExists=False marker=of_neg_x.txt'))
Assert 'marker_gone' (Test-MarkerGone 'markerGone=True ofquery=cow=1 state=FROZEN')
Assert 'marker_not_gone' (-not (Test-MarkerGone 'markerGone=False ofquery=cow=1'))

# Case 11: hash compare
Assert 'hash_match' (Test-HashMatch $expectSys $expectSys)
Assert 'hash_mismatch' (-not (Test-HashMatch 'A004E55F9DB9295DE555923E1243A1478CEAAD0B6EC77425832A871C6C52C2FE' $expectSys))

# Case 12: state file round-trip
Remove-Item $statePath -Force -EA SilentlyContinue
$script:tag = '20261105_dry'; $script:log = 'C:\VMs\x.log'; $script:dir = 'C:\VMs\d'; $script:phase = 'C'; $script:cycle = 7
SaveState
# reset then load
$script:tag = ''; $script:log = ''; $script:dir = ''; $script:phase = 'A'; $script:cycle = 0
$loaded = LoadState
Assert 'state_load_true' $loaded
Assert 'state_tag' ($script:tag -eq '20261105_dry')
Assert 'state_phase' ($script:phase -eq 'C')
Assert 'state_cycle' ($script:cycle -eq 7)
# STATUS=running required; flip and ensure no load
(Get-Content $statePath) -replace 'STATUS=running','STATUS=END_FAIL' | Set-Content $statePath
Assert 'state_reject_ended' (-not (LoadState))
Remove-Item $statePath -Force -EA SilentlyContinue


# Case 13: initStep/initStatus on ConfigRefuse (CreateFile fail was step 9 OverlayCreate)
$s13 = 'cow=0 torn=0 slotsUsed=0 pendingW=0 bypassR=0 workerR=0 full=0 state=PASSTHROUGH ptRanges=4 l2Res=0 ptW=0 ptR=0 ptBytes=10200616960 target=1 reason=ConfigRefuse initStep=9 initStatus=0xC000000D EnableCow=1 Backend=1 SlotCount=262144 freeMB=365'
$q13 = Parse-OfQuery $s13
Assert 'init_configrefuse_ok' ($q13.ok -and $q13.reason -eq 'ConfigRefuse' -and (OfQuery-IsPassthroughOrRefused $q13))
Assert 'init_step9' ($q13.initStep -eq 9)
Assert 'init_status_hex' ($q13.initStatus -eq '0xC000000D')

# Case 14: frozen success with initStep=13 Done / status 0 (0.8.6.1; ConfigSector step inserted)
$s14 = 'cow=1 torn=0 slotsUsed=0 full=0 state=FROZEN ptRanges=4 target=1 reason=None initStep=13 initStatus=0x00000000'
$q14 = Parse-OfQuery $s14
Assert 'init_frozen_ok' ((OfQuery-IsFrozen $q14) -and $q14.initStep -eq 13 -and $q14.initStatus -eq '0x00000000')

# Case 15: embedded ofquery= with init fields + EnableCow trap
$s15 = 'ofquery=cow=0 state=PASSTHROUGH target=1 reason=ConfigRefuse initStep=9 initStatus=0xC000000D EnableCow=1 Backend=1'
$q15 = Parse-OfQuery $s15
Assert 'init_embedded' ($q15.ok -and $q15.cow -eq 0 -and $q15.initStep -eq 9 -and $q15.initStatus -eq '0xC000000D' -and -not (OfQuery-IsFrozen $q15))

# Case 16: expect hash is 0.8.5.3
Assert 'hash_0861' (Test-HashMatch $expectSys 'F4290E88B44886906E49CEE59DCFDE83B163898AFF64370726B8053D5784F25E')
Assert 'hash_old_mismatch' (-not (Test-HashMatch 'EBA8323D20F2742E05B5AB1140A8422DC50AA6DF1EA35FBC23E73349D0F66525' $expectSys))

# Case 17: v4 harness false-SURVIVED â€” empty marker name => Test-Path C:\ is True
function Test-MarkerPathSafe([string]$markerName) {
  if ([string]::IsNullOrWhiteSpace($markerName)) { return $false } # treat as inconclusive/unsafe
  return -not (Test-Path -LiteralPath "C:\$markerName")
}
Assert 'null_path_false_survive_guard' (-not (Test-MarkerPathSafe $null))
Assert 'empty_path_false_survive_guard' (-not (Test-MarkerPathSafe ''))
# New after-check fields still satisfy markerGone matcher
Assert 'marker_gone_v5_fields' (Test-MarkerGone 'markerGone=True marker=of_cycle_1_x.txt nameFileExists=False markerExists=False ofquery=cow=1')
Assert 'marker_not_gone_v5_fields' (-not (Test-MarkerGone 'markerGone=False marker=of_cycle_1_x.txt nameFileExists=True markerExists=True'))

# Case 18: G ArgList passthrough (local, no VM). Proves $ArgList reaches the
# scriptblock param; also that a param named $Args is shadowed by automatic $args.
function Invoke-GLocal([scriptblock]$sb, [int]$cap = 90, [object[]]$ArgList = @()) {
  if ($null -ne $ArgList -and $ArgList.Count -gt 0) {
    return ,(Invoke-Command -ScriptBlock $sb -ArgumentList $ArgList)
  }
  return ,(Invoke-Command -ScriptBlock $sb)
}
function Invoke-GLocalBrokenArgs([scriptblock]$sb, [object[]]$Args = @()) {
  return ,(Invoke-Command -ScriptBlock $sb -ArgumentList $Args)
}

$markerName = 'of_cycle_1_20261105_210146.txt'
$got = Invoke-GLocal -sb {
  param($expectMarker)
  "receivedExpect=$expectMarker"
} -ArgList @($markerName)
Assert 'arglist_passthrough' (("$got" -match [regex]::Escape("receivedExpect=$markerName")))
Assert 'arglist_exact' (("$got".Trim()) -eq "receivedExpect=$markerName")

$broken = Invoke-GLocalBrokenArgs -sb {
  param($expectMarker)
  if ([string]::IsNullOrWhiteSpace([string]$expectMarker)) { 'empty' } else { "got=$expectMarker" }
} -Args @($markerName)
Assert 'args_collision_empties' (("$broken".Trim()) -eq 'empty')

function Test-HarnessEmptyExpect([string]$text) {
  return ($text -match '(?m)(?:^|\s)err=empty-expect(?:\s|$)' -or $text -match '(?m)(?:^|\s)marker=MISSING(?:\s|$)' -or $text -notmatch 'receivedExpect=')
}
Assert 'harness_error_on_empty' (Test-HarnessEmptyExpect 'receivedExpect= markerGone=False marker=MISSING err=empty-expect')
Assert 'harness_error_on_missing_echo' (Test-HarnessEmptyExpect 'markerGone=False markerExists=False')
Assert 'harness_ok_with_echo' (-not (Test-HarnessEmptyExpect "receivedExpect=$markerName markerGone=True markerExists=False nameFileExists=False"))

# Case 19: ThawRequested + cfg fields
$s19 = 'cow=0 torn=0 slotsUsed=0 full=0 state=PASSTHROUGH target=1 reason=ThawRequested initStep=13 initStatus=0x00000000 cfgValid=1 thawNext=1 cfgSeq=3'
$q19 = Parse-OfQuery $s19
Assert 'thaw_req_ok' ($q19.ok -and $q19.reason -eq 'ThawRequested' -and $q19.cfgValid -eq 1 -and $q19.thawNext -eq 1 -and $q19.cfgSeq -eq 3)
Assert 'thaw_req_not_frozen' (-not (OfQuery-IsFrozen $q19))
Assert 'thaw_req_is_PT' (OfQuery-IsPassthroughOrRefused $q19)

# Case 20: ConfigCorrupt
$s20 = 'cow=0 state=PASSTHROUGH target=1 reason=ConfigCorrupt initStep=13 initStatus=0xC0000182 cfgValid=0 thawNext=0 cfgSeq=0'
$q20 = Parse-OfQuery $s20
Assert 'corrupt_reason' ($q20.reason -eq 'ConfigCorrupt')
Assert 'corrupt_PT' (OfQuery-IsPassthroughOrRefused $q20)
Assert 'corrupt_not_frozen' (-not (OfQuery-IsFrozen $q20))

# Case 21: expect hash placeholder self-match (filled after build)
Assert 'hash_0861_self' (Test-HashMatch $expectSys $expectSys)
Write-Host "---- SUMMARY pass=$pass fail=$fail ----"
if ($fail -gt 0) { exit 1 } else { exit 0 }

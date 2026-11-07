# Phase 4 — Control plane (service + panel + ofctl)

Status after Phase 3 / Exp C land (`0.8.7.0`): freeze, sticky thaw, plain thaw,
and re-freeze work via overlay config sector + `ofquery` on `\\.\C:`.

## Goal

Password-gated mode changes through a SYSTEM service and WinForms panel, with
`ofctl` for scripted tests. Driver exposes a **control device** (not only the
volume path) with SYSTEM-only SDDL.

## IOCTL surface (control device `\\.\OfFilter`)

| Code | Name | Notes |
|------|------|--------|
| 0xA01 | `GET_STATUS` | mode, overlay used/total slots, thawNext, refuse reason |
| 0xA02 | `SET_NEXT_BOOT_MODE` | Frozen / Thawed / ThawedForN(n); config sector (+ EnableCow for Frozen/Thawed) |
| 0xA03 | `GET_OVERLAY_EVENT` | inverted call: pends until near-full (≥90%) or full; service warns / reboots |

Existing volume IOCTLs (`0x901`–`0x904` on `\\.\C:`) stay for harness/`ofquery`.
Control-device codes use `OF_IOCTL_DEVICE_TYPE` (`0x8F4F`) function range `0xA0x`
so they do not collide.

SDDL: `D:P(A;;GA;;;SY)` — SYSTEM only. Service opens the handle; panel talks to
the service over a named pipe (Authenticated Users).

## User-mode layout

```
user-mode/OpenFreeze.sln
  OpenFreeze.Core/     # P/Invoke, models, PBKDF2 password store, pipe framing
  OpenFreeze.Service/  # LocalSystem service, pipe server, owns driver handle
  OpenFreeze.Panel/    # WinForms + tray skeleton
  OpenFreeze.Cli/      # ofpipe.exe — named-pipe client for harness/panel scripting
  OpenFreeze.Tests/    # xUnit (not started)
```
Host CLI for volume IOCTLs remains `tools\ofctl` until migrated.

## Pipe protocol

- Named pipe: `\\.\pipe\OpenFreeze.Control`
- Framing: 4-byte little-endian length + UTF-8 JSON (`DataContractJsonSerializer`)
- ACL: SYSTEM full + Authenticated Users read/write
- Commands: `Ping`, `GetStatus`, `SetNextBootMode`, `ChangePassword`, `SetPasswordBootstrap`
- Auth: PBKDF2-SHA256, 600k iterations, salt+hash in `HKLM\SOFTWARE\OpenFreeze\Auth`
  (SYSTEM + Administrators ACL). Lockout after 5 failures / 300s.
- Password change / mode arm only while thawed (`!CowEnabled && !CowTornDown`).
- First password: `SetPasswordBootstrap` (no prior hash). Panel asks service via `HasPassword`
  on the response — it does not read the Auth key itself.

## Implementation order (this phase)

1. **ofctl** — thin CLI over volume IOCTLs + registry; prefers `\\.\OfFilter` when openable. ✅
2. **Control device** in `OfFilter` (`Control.c`) + `shared/OfIoctl.h` — GET_STATUS / SET_NEXT_BOOT_MODE / thaw. ✅ in **0.8.7.1** (SYSTEM SDDL).
3. **Service** — pipe protocol, password hash in HKLM (SYSTEM ACL), lockout. ✅ skeleton
4. **Panel** — status, mode radios, N spinner, password, overlay bar, tray. ✅ skeleton
5. VM tests:
   - Wrong password + thaw survival: ✅ `phase4_service_smoke.ps1`
     (`phase4_svc_20261106_150149.log`).
   - Freeze-via-pipe reboot cycle: ✅ `phase4_freeze_cycle.ps1`
     (`phase4_frz_20261106_151331.log`).
     `setmode frozen` → reboot **FROZEN** → marker written → `setmode thawed`
     while frozen → reboot **THAWED_STICKY** / marker gone / service Running.
   Phase 4 control-plane ✅ criterion met for pipe path.
6. **ThawedForN + overlay event (0.8.7.3)** — config `Reserved0` countdown;
   `GET_OVERLAY_EVENT` waiter; service near-full EventLog + full→reboot.
   Unit: `BlockMapTests` `TestThawedForNConfig` ✅.
   Driver staged/signed through 0.8.7.3 (`90CC8264…`); current stage3 is
   **0.8.7.4** (see HASHES / EXPECT_SYS.txt).
   Volume path handles `SET_NEXT_BOOT_MODE` (admin cannot open `\\.\OfFilter`).
   VM countdown cycle: ✅ `phase4_thawedn_finish.ps1`
   (`phase4_thawedn_fin_20261106_204242.log` **END_PASS**).
   Arm via volume `SET_NEXT_BOOT_MODE` (ofpipe can SEM_TIMEOUT if a prior
   ofpipe holds `MaxInstances=1`); verify with `ofquery thawNext=1`.
   Sequence: arm N=2 → boot1 thawed (`ThawRequested`, thawNext=1) → boot2
   thawed (thawNext=0) → boot3 **FROZEN**.
   Service overlay watch uses pended `GET_OVERLAY_EVENT` on **0.8.7.4+**
   (parallel control queue). Always **Stop-Service** before replacing
   `OpenFreeze.Service.exe`.
7. **0.8.7.4 parallel control queue** — `Control.c` default queue is
   `WdfIoQueueDispatchParallel` so a pended overlay waiter cannot block
   GET_STATUS / SET_MODE. Staged lab `offilter_stage3` EXPECT
   `78279924…`. Service uses a **dedicated** `DriverClient` for
   `WaitOverlayEvent` (sharing one handle blocked pipe GetStatus).
   Install driver only while thawed (frozen CoW discards `.sys` on reboot).
   VM: ✅ `phase4_0874_overlay_smoke.ps1`
   (`phase4_0874_ovl_20261106_210652.log` **END_PASS** — status under
   pended overlay wait + Stop-Service without taskkill).

Install (elevated, test VM only): `installer\install-service.ps1` or the smoke
harness after `dotnet build user-mode\OpenFreeze.sln -c Debug`. Console debug:
`OpenFreeze.Service.exe --console`. Guest path used by smoke: `C:\OpenFreeze\`.

Note: `\\.\OfFilter` is SYSTEM-only; interactive admin `ofctl status` falls back to `\\.\C:`.

## Fail-safe

Same as Phase 3: bad config never fails the volume stack; stay passthrough.
Password changes only while thawed (sticky or plain).

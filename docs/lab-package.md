# OpenFreeze lab package (portable)

## Purpose

Ship a **self-contained folder** (`dist/OpenFreeze-lab`) and a WinForms
**`dist/OpenFreeze-Setup.exe`** for **managed PC laboratory images** (and VMs)
with **testsigning** enabled. Same stack as DriverTest: driver, overlay file
backend, service, panel, ofpipe.

This is **not** a production Secure Boot / WHQL release. For that path, see
[production-signing.md](production-signing.md) (follow-on).

## Build the package (host)

```powershell
dotnet build user-mode\OpenFreeze.sln -c Debug
powershell -NoProfile -File installer\New-LabPackage.ps1 -RepoRoot .
```

Outputs:
- `dist\OpenFreeze-lab\` — scripts + binaries
- `dist\OpenFreeze-Setup.exe` — elevated wizard (+ silent `/S`)

## Managed lab PC / image install (path A)

1. Golden image: Secure Boot **off**, then:
   ```text
   bcdedit /set testsigning on
   ```
   Reboot (Test Mode watermark is expected).
2. Install (physical lab PC needs `-AllowNonVm` / wizard checkbox):
   ```text
   OpenFreeze-Setup.exe /S -Password "YourLabPassword!" -AllowNonVm -OverlaySizeGB 2
   ```
   Or interactive Setup.exe as Administrator.
3. **Reboot**, then verify:
   ```text
   powershell -NoProfile -File C:\OpenFreeze\Verify-OpenFreeze.ps1
   ```
4. Freeze via Panel or `ofpipe setmode <pw> frozen` + reboot.

Guards (hardened):

- Refuses if testsigning is off.
- Hyper-V **hosts** are not treated as VMs (`HypervisorPresent` alone is ignored).
- Bare-metal lab PCs need `-AllowNonVm`.
- Refuses to start `OpenFreezeSvc` if `OfFilter` did not register (no orphan Panel).
- Refuses install/replace while `state=FROZEN`.

### Half-install cleanup

If Panel shows `query failed err=1` and `OfFilter` is missing:

```text
powershell -NoProfile -File C:\OpenFreeze\Remove-OpenFreezeOrphan.ps1 -RemoveAuth
```

(or from the package folder before files were deleted).

### Uninstall (thawed)

```text
.\Uninstall-OpenFreeze.ps1 -SkipRebootPrompt
```

## Silent flags (Setup.exe)

| Flag | Meaning |
|------|---------|
| `/S` or `-Silent` | No UI; log `%TEMP%\OpenFreeze-Setup-silent.log` |
| `-Password <pw>` | Bootstrap / existing password |
| `-OverlaySizeGB N` | Default 2 |
| `-AllowNonVm` | Required on non-VM lab PCs |
| `-FreezeAfterInstall` | Arm frozen for next boot |

## Safety

- Never install on the OpenFreeze **dev host** daily driver.
- Fail-safe: install leaves `EnableCow=0` unless `-FreezeAfterInstall`.
- Hibernation disabled during install.

## Fleet ops (path A)

Step-by-step golden image + SCCM/Intune notes: [lab-fleet-runbook.md](lab-fleet-runbook.md).

## Production (path B)

EV/attestation signing checklist + host scripts: [production-signing.md](production-signing.md).
`Prepare-ProdStage.ps1` / `Sign-UserMode.ps1` are ready when you have a cert.

## Smoke

Hardened Setup + Verify on DriverTest completed with **END_PASS** in the local lab smoke log (Setup EXE path).

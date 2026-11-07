# OpenFreeze lab package

**Lab / test-signing only.** Not for production Secure Boot / WHQL. Do **not** install on your daily development host.

## Prerequisites

1. Windows 10/11 x64 lab VM or spare PC (VM strongly preferred).
2. Elevated PowerShell.
3. Test signing on, then reboot:
   ```text
   bcdedit /set testsigning on
   ```
   Secure Boot usually must be off (or the test cert must be trusted).
4. Free space on C: ≥ overlay size + ~2 GB (default overlay is **2 GB**).
5. BitLocker unlock timing is **untested** — prefer BitLocker off on lab images.

## Install (Setup EXE)

From the parent `dist` folder (after `New-LabPackage.ps1`):

1. Ensure testsigning is on and you already rebooted.
2. Right-click **`OpenFreeze-Setup.exe`** → Run as administrator.
3. Wizard: set password, overlay GB (default 2), check **Allow non-VM** on a physical lab PC, optionally **Freeze after install**.
4. Reboot, then: `powershell -File C:\OpenFreeze\Verify-OpenFreeze.ps1`

Silent (image bake / fleet):

```text
OpenFreeze-Setup.exe /S -Password "YourLabPassword!" -AllowNonVm -OverlaySizeGB 2
```

Orphan half-install (service without driver): `Remove-OpenFreezeOrphan.ps1`

Production Secure Boot signing is **not** in this package — see `docs/production-signing.md`.

## Install (scripts)

```powershell
cd <this-folder>
.\Install-OpenFreeze.ps1 -BootstrapPassword 'YourLabPassword!'
# optional: -OverlaySizeGB 2 -FreezeAfterInstall -AllowNonVm -SkipRebootPrompt
```

Default leaves the filter in **passthrough** (`EnableCow=0`) until you freeze. Reboot after install.

Then freeze:

```text
C:\OpenFreeze\ofpipe.exe setmode YourLabPassword! frozen
```

Reboot again → `ofquery` should show `cow=1` / `state=FROZEN`.

## Day-to-day

| Action | How |
|--------|-----|
| Status | Panel tray, or `ofpipe status`, or `ofquery query` |
| Freeze | Panel → Frozen → Apply → **reboot** |
| Thaw | Panel → Thawed → Apply → **reboot** |
| Thaw N boots | Panel → Thawed for N, or `ofpipe setmode <pw> thawedn 2` |
| Change password | Only while **thawed** |

Panel: `C:\OpenFreeze\OpenFreeze.Panel.exe`

## Uninstall

Must be **thawed** (passthrough) first:

```powershell
.\Uninstall-OpenFreeze.ps1
# optional: -RemoveAuth -SkipRebootPrompt
```

Then reboot.

## Limits

See repo `docs/architecture.md` and `docs/lab-package.md`: raw PhysicalDrive bypass, volume IOCTL thaw path, test-signed only, hibernation must stay off.

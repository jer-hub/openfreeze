# OpenFreeze — Implementation Plan (Deep Freeze clone, working prototype)

## Context
The PDF ("Prompt Plan for Deep Freeze Clone") lays out 5 phases: a KMDF storage filter, copy-on-write redirection, boot-time start plus reset, an IOCTL control panel, and anti-tamper. The goal is software that returns the Windows system drive to its original state at every reboot.

This plan turns those phases into a buildable design for this repository (Phases 0–6 prototype landed).

Decisions made:
- **Scope:** a working prototype that freezes C: in a test VM (`DriverTest`).
- **Overlay store:** a preallocated overlay file.
- **User-mode stack:** C#/.NET Framework 4.8.

The PDF's workflow advice still applies: implement one phase per session, compile it, and test it in a VM. Clear or compact the context between phases.

**Host toolchain (current):** VS/WDK for the driver, .NET SDK for user-mode, Hyper-V with guest `DriverTest`. Never load OfFilter on the build host.

## Core design (the key idea)
- **Driver type.** `OfFilter.sys` is a KMDF **volume upper filter**, registered in `UpperFilters` of the Volume class `{71A27CDD-812A-11D0-BEC7-08002BE2092F}`. It sits below NTFS and works on volume byte offsets. It attaches to every volume but freezes only the one whose `IOCTL_MOUNTDEV_QUERY_UNIQUE_ID` matches the configured value.
- **Block map in RAM only.** Writes are redirected at 4 KB block granularity into slots of the overlay file. The map from block to slot is a sparse two-level table in NonPagedPool, plus a slot allocator. **The map is never persisted.** At reboot it disappears, so every redirected write is discarded and the base volume is still pristine. "Reset on shutdown" therefore needs no cleanup code. This is the main simplification over the PDF's Phase 3.
- **I/O paths:**
  - **Write:** look up the block, or allocate a slot. For a partial block, read the original block (or the existing slot), merge, then write the full block into the slot. The original sectors are never touched.
  - **Read:** split the request per block. Mapped blocks come from the overlay slot; unmapped blocks come from the lower device.
  - **Overlay I/O:** the driver sends raw sector I/O to the overlay file's extents directly to the lower device object. This bypasses NTFS and the filter's own redirection. Overlay extents are always passthrough.
- **Overlay file.** `C:\OpenFreeze\overlay.bin` is created while thawed at install time, with a configurable size (default 8 GB).
  - The installer calls `SetFileValidData` and `FSCTL_MARK_HANDLE` (`MARK_HANDLE_PROTECT_CLUSTERS`) so defrag cannot move it.
  - The extent list comes from `FSCTL_GET_RETRIEVAL_POINTERS` and is stored in `HKLM\SYSTEM\CurrentControlSet\Services\OfFilter\Parameters`, together with the volume unique ID.
- **Persistent config sector.** The first 4 KB of the overlay holds mode, thaw counter, magic and CRC. It is written raw, so it survives even while frozen. This is how "Thaw on next boot" can be issued from a frozen session: registry writes made while frozen would be lost.
- **Modes:** `Frozen`, `Thawed`, `ThawedForN`.
  - The mode is read once at boot.
  - A change takes effect at the next boot, as in Deep Freeze.
  - With `ThawedForN`, the driver decrements the counter at each boot and switches back to Frozen when it reaches 0.
- **Processing model.** IRPs are forwarded from a KMDF parallel queue to a dedicated system worker thread. That thread does the synchronous lower I/O at PASSIVE_LEVEL.
  - The prototype uses one map lock and in-order processing.
  - All buffers are preallocated. Memory comes from `ExAllocatePool2(POOL_FLAG_NON_PAGED)`, as the PDF's safety rules require.
- **Must-handle edge cases while frozen:**
  - Swallow TRIM: `IOCTL_STORAGE_MANAGE_DATA_SET_ATTRIBUTES` returns success with no action. Passing it through would destroy base data.
  - Disable hibernation and Fast Startup (`powercfg /h off`). A hiberfile resumed without the RAM map would corrupt the volume.
  - Disable System Restore and VSS on C:.
  - **Overlay full:** fail writes with `STATUS_DISK_FULL`, notify the service, and the service forces a reboot.

## Repository layout (current)
```
openfreeze/
  CLAUDE.md / .gitignore
  shared/OfIoctl.h
  driver/OfFilter/           # KMDF: Driver, Device, Io, BlockMap, Overlay*, OfConfig, Control, Protect, RangeList
  driver/BlockMapTests/      # user-mode tests for BlockMap/OfConfig/RangeList/OverlayMap
  user-mode/OpenFreeze.sln
    OpenFreeze.Core/         # DriverClient, PasswordStore, pipe protocol
    OpenFreeze.Service/      # OpenFreezeSvc + named pipe
    OpenFreeze.Panel/        # WinForms control panel
    OpenFreeze.Cli/          # ofpipe.exe
    OpenFreeze.Setup/        # OpenFreeze-Setup.exe (embeds lab payload)
  installer/                 # Install/Uninstall/Verify/Orphan + New-LabPackage + deploy/
  scripts/                   # phase2_file_test.ps1 (+ helpers dry)
  tools/                     # ofquery, ofoverlay, ofctl sources
  docs/                      # see docs/README.md
  dist/                      # generated lab package (local)
```
Safety rules (also in CLAUDE.md / `.cursor/rules`): NonPagedPoolNx + `OF_POOL_TAG` for large buffers; fail-safe passthrough on config errors; test only in DriverTest, never on the host.

## Phases

### Phase 0 — Toolchain & test VM
1. Install VS 2022 (Desktop C++ workload plus Spectre-mitigated libraries), the matching Windows SDK, and the WDK plus its VS extension.
2. Enable Hyper-V. Create a Windows 11 test VM (Gen2, Secure Boot off) and take a checkpoint.
3. In the VM, run `bcdedit /set testsigning on` and set up KDNET kernel debugging with WinDbg from the host.
4. Run `git init`, write CLAUDE.md and `.gitignore`, and add the empty solution skeletons.

✅ An empty KMDF driver builds, is test-signed, and loads in the VM.

### Phase 1 — Passthrough volume filter (PDF Phase 1)
- Implement `DriverEntry` and `EvtDeviceAdd` with `WdfFdoInitSetFilter`, and find the target volume through the unique ID.
- Forward all IRPs with `WdfRequestSend` and send-and-forget.
- Add WPP tracing of read and write offsets.
- Write the INF with an AddReg to the Volume class UpperFilters, `StartType=0` (boot) and `LoadOrderGroup="PnP Filter"`.

✅ The VM boots with the filter, Driver Verifier is on (standard plus I/O verification), traces show C: writes, and there is no BSOD.

### Phase 2 — CoW engine (PDF Phase 2)
- `BlockMap.c`: sparse two-level table, slot allocator and bitmap. It is pure logic, unit-tested in `BlockMapTests`.
- `Overlay.c` backend interface with two implementations:
  1. **RAM backend** first, as a de-risking milestone.
  2. **File backend**, which translates slot to overlay extent to volume offset and does raw I/O to the lower device.
- `Io.c`: read splitting and merging, read-modify-write for partial blocks, overlay-extent passthrough, TRIM swallow.

✅ While frozen, a file created on C: disappears after reboot. `chkdsk C: /scan` is clean after 20 cycles.

### Phase 3 — Boot state & mode logic (PDF Phase 3)
- `Config.c`: read and validate the config sector (magic, CRC) and the registry parameters at device start.
- Apply the mode and the ThawedForN countdown, written raw.
- **Fail safe:** if the config is invalid, boot in passthrough (thawed) and log an error, rather than bugchecking.
- `install.ps1`:
  1. Create and lock the overlay.
  2. Save the extents and the unique ID to the registry.
  3. Run `powercfg /h off` and disable System Restore.
  4. Install the INF and reboot.
- `uninstall.ps1`: requires Thawed mode, removes UpperFilters, deletes the overlay.

✅ Switching between Frozen, Thawed and ThawedForN(2) works across reboots.

### Phase 4 — IOCTL + .NET service + panel (PDF Phase 4)
- Driver `Ioctl.c` exposes a control device. Its SDDL `D:P(A;;GA;;;SY)` allows SYSTEM only.
- IOCTLs: `GET_STATUS` (mode, overlay used/total, counter), `SET_NEXT_BOOT_MODE`, `GET_OVERLAY_EVENT` (inverted call for overflow and near-full alerts).
- **Service:**
  - Owns the driver handle.
  - Hosts a named pipe with an ACL for Authenticated Users, so the panel can connect and send requests.
  - Checks the password against a PBKDF2-SHA256 hash (`Rfc2898DeriveBytes.Pbkdf2`, 600k iterations, salted). The hash is stored in an HKLM key with a SYSTEM-only ACL.
  - Allows password changes only while Thawed, because a change made while frozen would be discarded at reboot.
  - Handles overflow with a warning, then a forced reboot.
  - Locks out after N wrong attempts.
- **Panel:** status, mode radio buttons, an N-reboots spinner, a password prompt, and an overlay usage bar. The tray icon is a snowflake for frozen and an open icon for thawed.
- **CLI:** `ofctl` for scripted tests.

✅ Every mode change goes through the panel and needs the password. A wrong password is rejected.

### Phase 5 — Self-protection (PDF Phase 5, prototype level)
- Device SDDL, plus a check that the IOCTL caller is the service process (PID registered at first open).
- `ObRegisterCallbacks` strips `PROCESS_TERMINATE` and `PROCESS_VM_WRITE` from handles to the service process opened by non-SYSTEM callers. This needs the `/INTEGRITYCHECK` linker flag.
- `CmRegisterCallbackEx` blocks changes to `Services\OfFilter`, `Services\OpenFreezeSvc` and the Volume class `UpperFilters` while frozen.
- Configure service recovery to restart the service on failure. There is no unload routine while frozen.
- **Note:** while frozen, most tampering with files or the registry is undone at the next reboot anyway.
- Document the known gaps in `docs/architecture.md`: raw `\\.\PhysicalDriveN` writes go below the volume filter, BitLocker is untested, and a kernel-mode attacker is out of scope.

✅ A standard user and a local admin cannot kill the service, edit the filter keys, or send IOCTLs.

### Phase 6 — Validation matrix
Run all scenarios in the VM, starting from the checkpoint each time:
- Install an app, change the wallpaper, delete user files, and corrupt a non-critical System32 file. Reboot: everything is restored.
- Windows Update while frozen: discarded after reboot, and the system is stable.
- Fill the overlay with `diskspd` or a write loop: the alert fires, the reboot happens, and there is no corruption.
- Thaw, install an app, freeze: the app persists from then on.
- Run 50 reboot cycles with a script: `chkdsk /scan` stays clean and Driver Verifier reports nothing.
- Unit tests: `BlockMapTests` and `dotnet test` pass.

## Verification (end-to-end)
- **Build:** run `msbuild driver\OfFilter.sln /p:Configuration=Debug /p:Platform=x64` and `dotnet build user-mode\OpenFreeze.sln`. Signing is automatic with the WDK test certificate.
- **Deploy:** copy the files to the VM and run `installer\install.ps1`. Use `ofctl status` and the panel to check the state.
- **Debug:** use WinDbg over KDNET. Keep Driver Verifier on for `OfFilter.sys` throughout development.
- Each phase's ✅ criterion must pass in the VM before the next phase starts.

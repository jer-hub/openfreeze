# PC laboratory fleet runbook (path A — testsigning)

Use this until [production-signing.md](production-signing.md) is complete.
**Dev host:** never install OfFilter here. Build on the host; deploy only to lab images / VMs.

## Golden image bake

1. Sysprep-ready Windows 10/11 lab image.
2. Firmware: Secure Boot **Off** (required for test-signed drivers).
3. Elevated:
   ```text
   bcdedit /set testsigning on
   powercfg /h off
   ```
4. Reboot (Test Mode watermark is expected).
5. Copy `OpenFreeze-Setup.exe` onto the image and run:
   ```text
   OpenFreeze-Setup.exe /S -Password "CHANGE-ME-LAB" -AllowNonVm -OverlaySizeGB 2
   ```
6. Reboot, then:
   ```text
   powershell -NoProfile -ExecutionPolicy Bypass -File C:\OpenFreeze\Verify-OpenFreeze.ps1
   ```
   Expect `END_PASS`.
7. Optionally arm freeze and reboot once to confirm Panel/`ofpipe` show `FROZEN`.
8. Sysprep / capture image; clone to lab PCs.

## Day-2 ops

| Task | Command / action |
|------|------------------|
| Status | Panel, or `ofpipe status` / `ofquery query` |
| Thaw | Panel → Thawed → Apply → reboot |
| Freeze | Panel → Frozen → Apply → reboot |
| Half-install (`err=1`) | `Remove-OpenFreezeOrphan.ps1 -RemoveAuth` |
| Full remove | Thaw → reboot → `Uninstall-OpenFreeze.ps1` → reboot |

## Mass deploy (SCCM / Intune / PDQ)

Detection: `Get-Service OfFilter` exists **and** `Verify-OpenFreeze.ps1` exit 0.

Install program (example):

```text
OpenFreeze-Setup.exe /S -Password "%LAB_PW%" -AllowNonVm -OverlaySizeGB 2
```

Require reboot. Post-reboot script: `Verify-OpenFreeze.ps1`.

Wrapper scripts ship in `dist\OpenFreeze-lab\deploy\` after `New-LabPackage.ps1`.

## Password hygiene

- Do not bake a production password into a public share; use task-sequence variables or LAPS-style secrets.
- Change password once while **thawed** after first boot if the image used a bootstrap secret.

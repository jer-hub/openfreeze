# Phase 6 — Validation matrix

Driver under test: **0.8.7.6** (lab stage `offilter_stage3`, EXPECT in HASHES).
Driver Verifier on for `OfFilter.sys` in DriverTest (keep enabled).

## Scenarios

| # | Scenario | Status |
|---|----------|--------|
| 1 | Frozen session changes discarded after reboot (files / wallpaper proxy marker / System32 canary) | ✅ `phase6_matrix_20261106_213226.log` PASS S1 |
| 2 | Windows Update while frozen → discarded, stable | optional (long / network) — not required to close Phase 6 |
| 3 | Overlay near-full / full → service alert + reboot, no corruption | ✅ core PASS `phase6_ovlfull_20261106_220732.log`: near-full EventLog, FULL EventLog + service reboot. Online `chkdsk /scan` hit VSS snapshot error (soft). Overlay restored to **2GB / 262144 slots** via `phase6_overlay_restore.ps1` (`phase6_ovlrestore_20261107_011516.log` END_PASS; host-aware size — AVHDX free space blocked 4–8GB). |
| 4 | Thaw → install marker/app → freeze → reboot: marker persists | ✅ `phase6_thawpersist_20261106_215026.log` END_PASS |
| 5 | N reboot cycles; `chkdsk C: /scan` clean; no Verifier bugcheck | ✅ 5 cycles (matrix) + **50 soak** `phase6_reboot_cycle_20261107_013437.log` END_PASS: hash 0.8.7.6 each boot, frozen, chkdsk clean. Checkpoint `pre-p6-soak-20261107_013437`. |
| 6 | `BlockMapTests` + `dotnet test` / build usermode | ✅ BlockMapTests; no OpenFreeze.Tests project yet |

## Harnesses

Local lab scripts (not in-repo; live next to your staged driver):

- `phase6_freeze_restore.ps1` — scenario 1
- `phase6_thaw_persist.ps1` — scenario 4
- `phase6_reboot_cycle.ps1` — scenario 5 soak (`-Cycles N`, default 50; optional `-SkipCheckpoint`)

## Notes

- Install/replace `OfFilter.sys` only while **thawed** (frozen CoW discards the binary).
- Host free space: prefer ≤1 new checkpoint. Checkpoint chain pruned 2026-11-07 (`phase6_host_cleanup_20261107_012925.log`): 13 snaps removed, host free ~9→18GB; VM current state kept, no rollback snap.
- S2 Windows Update while frozen is **optional** (schedule separately if desired). Required Phase 6 scenarios are otherwise complete.
- Lab portable package: [lab-package.md](lab-package.md) / Setup EXE; production signing follow-on: [production-signing.md](production-signing.md).

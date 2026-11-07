# OpenFreeze docs

## Start here
| Doc | Topic |
|-----|--------|
| [architecture.md](architecture.md) | Volume filter, CoW overlay, control plane overview |
| [implementation-plan.md](implementation-plan.md) | Phased build plan (prototype through Phase 6) |
| [lab-package.md](lab-package.md) | Lab zip / `OpenFreeze-Setup.exe` packing |
| [lab-fleet-runbook.md](lab-fleet-runbook.md) | Path A: testsigning golden images / SCCM |
| [production-signing.md](production-signing.md) | Path B: EV cert / attestation / Secure Boot |

## Design notes
| Doc | Topic |
|-----|--------|
| [phase2-file-backend.md](phase2-file-backend.md) | Overlay file backend |
| [config-sector.md](config-sector.md) | Overlay config sector / thaw-next-boot |
| [phase4-control-plane.md](phase4-control-plane.md) | Service, Panel, ofpipe |
| [phase5-self-protection.md](phase5-self-protection.md) | Ob/Cm self-protection |
| [phase6-validation.md](phase6-validation.md) | Validation / soak notes |

## Repo map
```text
driver/OfFilter/     KMDF volume upper filter
driver/BlockMapTests Unit tests for block map / config logic
shared/OfIoctl.h     Shared IOCTL contracts
user-mode/           Core, Service, Panel, Cli (ofpipe), Setup
installer/           Lab install/uninstall/verify + New-LabPackage.ps1
scripts/             phase2_file_test harness (+ dry helpers)
tools/               ofquery, ofoverlay, ofctl sources
dist/                Generated lab package / Setup.exe (local pack output)
```

**Safety:** never load OfFilter on the Hyper-V build host — test in the DriverTest VM only.

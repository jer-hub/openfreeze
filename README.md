# OpenFreeze

Open-source Deep Freeze–style prototype: KMDF volume CoW filter (**OfFilter**) plus a .NET control plane. Lab/testsigning builds; production Secure Boot needs attestation signing.

## Warning

This is a **kernel volume filter**. Use it only on disposable **test VMs** or managed **lab images** with testsigning. It is **not** Microsoft attestation/WHQL signed. Do not install on your daily-driver or Hyper-V build host.

## Features

- Freeze / thaw / thawed-for-N (next-boot mode, Deep Freeze–like)
- Copy-on-write overlay file so frozen writes never touch the base volume
- Password-gated control plane: Windows service, WinForms Panel, `ofpipe`
- Lab installer: `OpenFreeze-Setup.exe` (wizard + silent `/S`)

## Repository layout

| Path | Contents |
|------|----------|
| `driver/OfFilter/` | KMDF volume upper filter |
| `driver/BlockMapTests/` | User-mode unit tests for map/config logic |
| `shared/OfIoctl.h` | Shared IOCTL contract |
| `user-mode/` | Core, Service, Panel, Cli (`ofpipe`), Setup |
| `installer/` | Install / uninstall / verify / lab pack scripts |
| `scripts/` | Phase 2 file-test harness (+ dry helpers) |
| `tools/` | `ofquery`, `ofoverlay`, `ofctl` sources |
| `docs/` | Architecture, phases, lab & signing guides |

## Quick start

1. Build the driver with VS + WDK; build user-mode with .NET Framework 4.8:
   ```powershell
   dotnet build user-mode\OpenFreeze.sln -c Debug
   ```
2. Stage a test-signed `OfFilter.sys` (lab stage directory), then pack:
   ```powershell
   powershell -NoProfile -File installer\New-LabPackage.ps1 -RepoRoot .
   ```
3. Deploy **inside a guest VM** or lab PC (testsigning on; Secure Boot usually off). See [docs/lab-package.md](docs/lab-package.md) and [docs/lab-fleet-runbook.md](docs/lab-fleet-runbook.md).

For Secure Boot / production signing, see [docs/production-signing.md](docs/production-signing.md).

## Documentation

Full index: **[docs/README.md](docs/README.md)**

## Contributing / security

- [CONTRIBUTING.md](CONTRIBUTING.md) — VM-only testing and workflow
- [SECURITY.md](SECURITY.md) — reporting and deployment caveats

## License

[MIT](LICENSE) — Copyright (c) 2026 OpenFreeze contributors

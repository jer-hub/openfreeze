# Production signing & Secure Boot (path B)

Status: **scaffolding in repo** — signing still blocked on your EV cert / Partner Center.
Lab fleets until then: [lab-fleet-runbook.md](lab-fleet-runbook.md) (testsigning images).

## Why this is required for “any lab PC”

Windows 10/11 with **Secure Boot** will not load an unsigned or test-signed
kernel driver. School / enterprise lab images almost always keep Secure Boot on.
The current `DriverTestSigning.cer` path is for prototypes and controlled images only.

## Target outcome

1. `OfFilter.sys` signed for **Windows Hardware Dev Center attestation**
   (or legacy WHQL if still using that path for your OS set).
2. `OpenFreeze-Setup.exe` and user-mode binaries Authenticode-signed (optional
   for SmartScreen; driver signing is the hard gate).
3. Install works with Secure Boot **on**, no `bcdedit testsigning`, no Test Mode
   watermark.
4. Silent deploy via SCCM / Intune / PDQ using Setup `/S` flags.

## Repo helpers (run on the build host)

| Script | Purpose |
|--------|---------|
| [`installer/Prepare-ProdStage.ps1`](../installer/Prepare-ProdStage.ps1) | Copy lab stage dir → `offilter_stage_prod_candidate` + `PROD_CHECKLIST.txt` (defaults under your local lab folder) |
| [`installer/Sign-UserMode.ps1`](../installer/Sign-UserMode.ps1) | Authenticode-sign Service/Panel/ofpipe/Setup with `-CertThumbprint` (after EV cert is installed) |
| [`installer/New-LabPackage.ps1`](../installer/New-LabPackage.ps1) | Rebuild Setup from a stage dir once the **attestation-signed** `.sys/.cat` are in place (`-StageDir`) |

Kernel attestation signing is **not** automated here — that stays in Hardware Dev Center / HLK.

## Checklist when you have the cert

```powershell
# 1) Snapshot current binaries into a prod-candidate folder
powershell -NoProfile -File installer\Prepare-ProdStage.ps1

# 2) (External) Submit OfFilter.sys for attestation; drop signed .sys + .cat into stage_prod_candidate
# 3) Optional: sign user-mode EXEs
powershell -NoProfile -File installer\Sign-UserMode.ps1 -CertThumbprint YOUR_EV_THUMBPRINT

# 4) Pack from the signed stage
powershell -NoProfile -File installer\New-LabPackage.ps1 -RepoRoot . -StageDir path\to\offilter_stage_prod_candidate

# 5) Pilot on a Secure Boot ON lab PC
#    OpenFreeze-Setup.exe /S -Password '...' -AllowNonVm
#    reboot → C:\OpenFreeze\Verify-OpenFreeze.ps1
```

## High-level org steps

1. **Company / org** registered with Microsoft Partner Center / Hardware Dev Center.
2. Purchase an **EV code signing certificate** from a supported CA.
3. Sign + submit attestation package for `OfFilter.sys` + catalog; bump INF `DriverVer`.
4. Rebuild Setup from the signed stage; pilot; then golden-image without testsigning.

## Cost / timeline (rough)

- EV cert: typically hundreds of USD / year + identity vetting.
- First attestation pipeline: days to weeks.
- Not a weekend code change — mostly process and signing infrastructure.

## Until then

Use **path A**: [lab-fleet-runbook.md](lab-fleet-runbook.md).

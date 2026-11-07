# Contributing

## Safety first

- Never load or install `OfFilter` on the development host.
- Test only in a Hyper-V (or similar) guest with Driver Verifier on for
  `OfFilter.sys`, or on a dedicated lab image with testsigning.
- PowerShell scripts target **Windows PowerShell 5.1** (`[UInt64]`, not
  `[ulong]`; never name a parameter `$Args`).

## Workflow

1. Read [docs/implementation-plan.md](docs/implementation-plan.md) and work
   **one phase / one change** at a time.
2. Pure logic changes under `driver/OfFilter/BlockMap.c` (and related) should
   keep `driver/BlockMapTests` green.
3. Before VM runs, run `scripts/phase2_file_test_helpers_dry.ps1` (56-pass dry
   harness) when touching harness helpers.
4. Bump INF `DriverVer` when staging a new `OfFilter.sys` for lab packages.

## Lab harness layout

`scripts/phase2_file_test.ps1` defaults assume a local lab folder such as
`C:\VMs\` for staged drivers and guest credentials. That layout is **not**
committed; do not add password files to git.

## Pull requests

- Describe what was verified (unit tests, dry harness, VM smoke).
- Do not include `dist/`, `bin/`, `obj/`, or signed binaries unless explicitly
  requested.

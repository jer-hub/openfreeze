# Phase 5 — Self-protection (prototype)

Phase 4 control plane is complete (0.8.7.4). This phase hardens the service and
driver against casual local admin tampering while frozen.

## Goal

A standard user and a local admin cannot kill the service, edit the filter
keys, or send control IOCTLs except through the service.

## Work items

1. **Service recovery** — `sc failure` / `failureflag` restart on crash in
   `installer\install-service.ps1`.
2. **IOCTL caller gate** — register service PID on first successful open of
   `\\.\OfFilter`; reject SET_* from other PIDs (SYSTEM SDDL already limits
   openers; this is defense in depth).
3. **`ObRegisterCallbacks`** — strip `PROCESS_TERMINATE` /
   `PROCESS_VM_WRITE` from handles to the service process opened by
   non-SYSTEM callers. Needs `/INTEGRITYCHECK` on `OfFilter.sys`.
4. **`CmRegisterCallbackEx`** — while frozen, block writes to
   `Services\OfFilter`, `Services\OpenFreezeSvc`, and Volume class
   `UpperFilters`.
5. **Docs** — known gaps in `docs\architecture.md` (raw PhysicalDrive,
   BitLocker untested, KM attacker out of scope).

## Fail-safe

Protection must never prevent a thawed uninstall or a fail-safe passthrough
boot. Prefer deny-write while `CowEnabled`; allow when thawed.

## Status

1. Service recovery in `install-service.ps1` ✅ (also applied by
   `phase5_pidgate_smoke.ps1` via `sc failure`).
2. IOCTL PID gate on `\\.\OfFilter` SET_* ✅ in **0.8.7.5** (`Control.c`
   file create/cleanup + `OfFilterControlCallerIsOwner`). Volume `\\.\C:`
   SET path intentionally ungated (see `architecture.md`).
3. ObRegisterCallbacks ✅ **0.8.7.6** (`Protect.c`, `/INTEGRITYCHECK`) —
   strips `PROCESS_TERMINATE` / `PROCESS_VM_WRITE` / `PROCESS_VM_OPERATION`
   on handles to the control-owner PID for non-SYSTEM callers.
4. CmRegisterCallbackEx ✅ **0.8.7.6** — while frozen, non-SYSTEM writes to
   `Services\OfFilter`, `Services\OpenFreezeSvc`, and Volume class GUID key
   return `STATUS_ACCESS_DENIED`. SYSTEM (service) still allowed.
5. Known gaps ✅ `docs/architecture.md`.

Harnesses:
- `phase5_pidgate_smoke.ps1` — PID gate + sc failure (**END_PASS**).
- `phase5_protect_smoke.ps1` — Ob anti-kill + Cm registry deny while frozen
  (`phase5_protect_20261106_212420.log` **END_PASS** — taskkill Access
  denied; Parameters Set-ItemProperty denied). Stage EXPECT `0D63000F…`.

Phase 5 prototype ✅ criterion met (admin cannot kill service / edit filter
keys / send control SET IOCTLs except via the service).

# OpenFreeze — Architecture notes & known gaps

## What is protected

`OfFilter.sys` is a KMDF **volume upper filter** on the Volume class. While
frozen, writes to C: are redirected into an overlay file; the RAM block map is
discarded at reboot, so the base volume returns to its prior state.

Mode changes for next boot go through a config sector in the overlay (raw I/O,
survives freeze) and/or `EnableCow` in the service Parameters key.

User-mode control plane (Phase 4): LocalSystem `OpenFreezeSvc` owns
`\\.\OfFilter` (SYSTEM SDDL) and exposes `\\.\pipe\OpenFreeze.Control` to
Authenticated Users after password check.

## Phase 5 protections (prototype)

| Layer | Status |
|-------|--------|
| Control device SDDL `SY` only | ✅ |
| Service recovery (`sc failure` restart) | ✅ install script |
| SET_* only from registered opener PID | ✅ 0.8.7.5 |
| ObRegisterCallbacks (anti-kill / anti-VM-write) | ✅ 0.8.7.6 |
| CmRegisterCallbackEx (filter/service keys while frozen) | ✅ 0.8.7.6 |

## Known gaps (out of prototype scope or deferred)

1. **Raw `\\.\PhysicalDriveN` / disk filter bypass** — writes below the volume
   stack are not intercepted. A local admin with raw disk access can modify
   the base volume while “frozen.”
2. **Volume-path IOCTLs (`\\.\C:`)** — `SET_THAW` / `SET_NEXT_BOOT_MODE` remain
   available to Administrators for harness/`ofquery`. Control-device PID gating
   does not cover this path. Closing it would break thawed tooling unless a
   signed helper is introduced.
3. **BitLocker** — not tested. Unlock timing vs deferred CoW init is unknown.
4. **Kernel-mode attacker** — a loaded KM driver can bypass all of the above.
5. **Secure Boot / HVCI production signing** — test-signed only in DriverTest.
6. **Hibernation / Fast Startup** — installer turns hibernation off; users who
   re-enable it can corrupt state (map is RAM-only).
7. **Named pipe `MaxInstances=1`** — a wedged/orphaned client can block new
   connections until the service restarts; not a security boundary.

## Fail-safe

Invalid config, overlay, or init failure → stay **passthrough** (never freeze).
Device start must not fail because of OpenFreeze configuration.

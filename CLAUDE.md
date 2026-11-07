# OpenFreeze — Deep Freeze-style system restore (prototype)

Full design and phase plan: `docs/implementation-plan.md`. Work **one phase per session**; each phase's ✅ criterion must pass in the test VM before starting the next.

## Environment & Target
- Driver: C (KMDF volume upper filter), MSVC + Windows Driver Kit, Windows 10/11 x64
- User mode: C# / .NET (service, WinForms panel, `ofctl` CLI) under `user-mode/`
- Shared IOCTL contract: `shared/OfIoctl.h` — keep the C# mirror in `OpenFreeze.Core` in sync

## Safety Rules for Driver Code
1. Use NonPagedPool (`ExAllocatePool2(POOL_FLAG_NON_PAGED, ...)`) for all memory touched in I/O paths; preallocate where possible.
2. Null-check every allocation, IRP, MDL and request; free/complete them on both success and error paths.
3. `__try`/`__except` only around probing user buffers — never as a substitute for validation.
4. Never pass TRIM (`IOCTL_STORAGE_MANAGE_DATA_SET_ATTRIBUTES`) through to a frozen volume.
5. On invalid config, fail safe to passthrough (thawed) and log — never bugcheck.
6. Keep user-mode code completely separated in `user-mode/`.

## Testing
- **Never load the driver on the host machine.** Test only in the Hyper-V VM (testsigning on, KDNET + WinDbg, Driver Verifier enabled for OfFilter.sys).
- Pure logic (e.g. `BlockMap.c`) must stay free of kernel APIs so `driver/BlockMapTests` can unit-test it in user mode.

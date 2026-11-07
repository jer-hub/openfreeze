# Config-sector thaw-on-next-boot (0.8.6.0)

## Why

While frozen, registry writes (including `EnableCow=0`) are discarded by CoW.
Phase D of the file-backend harness therefore could not thaw via registry alone.
The reserved 4 KB at the start of `overlay.bin` is already a passthrough volume
extent, so a small raw config sector can carry a sticky "thaw next boot" bit
that survives a frozen session.

## Layout

Offset 0 of the overlay file, 4096 bytes. Struct (little-endian):

| Field     | Type   | Notes |
|-----------|--------|-------|
| Magic     | U32    | `'OFCF'` = `0x4643464F` |
| Version   | U32    | `1` |
| Size      | U32    | Bytes covered by CRC (= offset of `Crc32`) |
| Sequence  | U32    | Bumped on every tool write |
| Flags     | U32    | bit0 = `ThawNextBoot` |
| Reserved0 | U32    | `ThawedForN` remaining boots (0 = sticky-only / none) |
| Reserved1 | U32    | 0 |
| Crc32     | U32    | CRC-32/IEEE over `[0, Size)` with `Crc32` zeroed |

All-zero sector = **absent** (today's behavior: freeze if `EnableCow=1`).
Bad magic / bad CRC / nonzero junk = **corrupt** → do not freeze.

I/O uses `OfOverlayConfigSectorRead/Write` → map file offset 0 →
`OfOverlayLowerRead/Write` (direct lower device, bypasses CoW).

## Boot gate

CoW init is **deferred** from `PrepareHardware`: a system thread polls until
`OfOverlayProbeVolumeUnlocked` sees an NTFS/FAT signature on LBA0 (avoids FVE
success+zeros false-Absent → freeze → bugcheck `0xED`). Then, after overlay
extents + UID validate and **before** enabling CoW:

1. Read config sector (short lower reads fail closed, not treated as Absent).
2. `OfConfigShouldFreeze`:
   - absent → proceed to freeze
   - valid + ThawNextBoot → PASSTHROUGH, `reason=ThawRequested` (3)
   - corrupt → PASSTHROUGH, `reason=ConfigCorrupt` (4)
   - read fail → PASSTHROUGH, `reason=ConfigRefuse` (2)
3. Driver does **not** auto-clear ThawNextBoot on the thawed boot.

Shipped as **0.8.7.0** (Exp C landed). **ThawedForN** countdown in Reserved0
shipped as **0.8.7.2**.

`ofquery` V4 exposes `cfgValid`, `thawNext`, `cfgSeq` (buffer 100 bytes).
Control-device `GET_STATUS` / `ofpipe status` also report `thawedForN`.

### ThawedForN countdown

`SET_NEXT_BOOT_MODE` with `OF_MODE_THAWED_FOR_N` writes `ThawNext` + `N` (1..99).
Each thawed boot consumes one: `N' = N-1`. When `N'` hits 0 the flag is cleared
so the **next** boot freezes (EnableCow still 1). Plain `OF_MODE_THAWED` keeps
`N=0` sticky until the tool clears.

## Flag-clearing state machine (tool-driven)

```
FROZEN (EnableCow=1, thawNext=0)
  ofquery thaw-set     -->  sector flag set (seq++), still FROZEN this boot
  reboot
THAWED_STICKY (PASSTHROUGH, reason=ThawRequested, thawNext=1, EnableCow still 1 in registry)
  admin makes persistent changes (e.g. EnableCow=0)
  ofquery thaw-clear   -->  sector flag cleared (seq++)
  reboot
PLAIN_UNFROZEN (PASSTHROUGH, reason=None, EnableCow=0)

Alternate: leave EnableCow=1 after thaw-clear --> next boot freezes again.
```

Rationale for sticky flag: clearing in the same thawed boot would race with
admin registry edits and could re-freeze on an accidental reboot mid-change.
The tool clears only after the desired persistent registry state is written.

IOCTL: `IOCTL_OF_SET_THAW_NEXT_BOOT` (0x904, WRITE access). Input ULONG 1=set, 0=clear.
`ofoverlay create` still zeros the sector.

## Fail-safe

Corrupt or unreadable config refuses freeze (logged). Absent keeps legacy path.
## Boot-safe init status

`OfFilterInitializeCow` returns `STATUS_SUCCESS` for every config-sector
outcome (ThawRequested, ConfigCorrupt, read failure). `PrepareHardware` always
returns `STATUS_SUCCESS` as well, so the filter / boot volume stack never fail
to start because of the config sector. Diagnostics live in `CowRefuseReason`
and `CowInitStatus` (exposed by ofquery as `reason` / `initStatus`) while
the device remains passthrough (`CowEnabled=FALSE`).

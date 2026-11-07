# Phase 2 file overlay backend

## Goal

Keep the RAM CoW engine, and add a selectable file backend so the overlay
lives in `C:\OpenFreeze\overlay.bin` (default 8 GB) instead of NonPagedPool.
`OverlayBackend` in `Services\OfFilter\Parameters`: `0` = RAM, `1` = File.

## Layout

```
overlay.bin
+------------------+  offset 0
| config sector   |  4 KB (OFCF/CRC/ThawNextBoot; see docs/config-sector.md)
+------------------+  offset 4096
| slot 0           |  4 KB
| slot 1           |
| ...              |
| slot N-1         |
+------------------+
```

Slot count `N = (fileSize - 4096) / 4096`, capped at `OF_MAX_SLOT_COUNT`
(262144). The block map stays in NonPagedPool and is sized from `N`.

## Thawed setup (`ofoverlay create`)

While thawed (and only then):

1. Create/truncate `overlay.bin`, `SetFileValidData` for the full size.
2. Zero the reserved 4 KB config sector.
3. `FSCTL_MARK_HANDLE` with `MARK_HANDLE_PROTECT_CLUSTERS` (defrag must not move it).
4. `FSCTL_GET_RETRIEVAL_POINTERS` for the extent list.
5. Write registry:
   - `OverlayExtents` (REG_BINARY: triples of UINT64 file/volume/length)
   - `OverlayFileSize` (REG_BINARY UINT64)
   - `OverlayGeneration` (REG_DWORD)
   - `VolumeUniqueId` (REG_BINARY, from MountedDevices `\DosDevices\C:`)
   - `OverlayBackend=1`, `SlotCount=N`, `OverlayPath`
6. Does **not** set `EnableCow=1` (freezing stays a separate gate).

## Driver load path

`OfFilterLoadParameters` reads the extents, file size, and generation.
At CoW init on the matched volume:

1. Compare live volume unique ID to the stored one.
2. `OfOverlayValidateExtents` (alignment, no file-space overlap, every slot maps,
   optional volume bounds).
3. On failure: stay passthrough, refuse to freeze (no silent RAM fallback when
   `OverlayBackend=1`).
4. On success: `OfOverlayCreateFile`, derive/confirm slot count, merge overlay
   volume extents into the passthrough `RangeList` so overlay I/O never hits CoW.
5. Slot I/O is raw synchronous read/write to the lower device at the mapped
   volume offset (existing `OfOverlayFileReadSlot` / `WriteSlot`).

STATUS_DISK_FULL and atomic `OfBlockMapPrepareRange` semantics are unchanged.

## Pure helpers

`OverlayMap.c` / `OverlayMap.h` (no kernel APIs): capacity, validation,
slot-to-volume mapping, unique-id compare. Covered by BlockMapTests.

## Staging

Host-only Debug x64 build is signed into a **new** folder
Lab stage `offilter_stage3` (stage2 untouched). INF `DriverVer` pinned for the
file-backend drop.


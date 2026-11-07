/*++

Module Name:

    OverlayMap.h

Abstract:

    Pure overlay file mapping helpers (extent validation, slot capacity,
    slot-to-volume offset). No kernel APIs. Used by the driver and by
    BlockMapTests.

--*/

#pragma once

#include "BlockMap.h"

#ifdef __cplusplus
extern "C" {
#endif

/* First 4 KB of overlay.bin is reserved for the Phase 3 config sector. */
#define OF_OVERLAY_CONFIG_SECTOR_SIZE OF_BLOCK_SIZE

#ifndef OF_DEFAULT_SLOT_COUNT
#define OF_DEFAULT_SLOT_COUNT 65536u
#endif
#ifndef OF_MAX_SLOT_COUNT
#define OF_MAX_SLOT_COUNT     262144u
#endif

/* Max retrieval-pointer extents persisted for one overlay file. */
#define OF_MAX_OVERLAY_EXTENTS 256u

typedef struct _OF_OVERLAY_EXTENT {
    OF_U64 FileOffset;           /* byte offset within overlay.bin */
    OF_U64 VolumeOffset;         /* byte offset on the volume */
    OF_U64 Length;               /* bytes */
} OF_OVERLAY_EXTENT, *POF_OVERLAY_EXTENT;

/* Usable slots after reserving the config sector. 0 if FileSize too small. */
OF_U32
OfOverlayComputeSlotCount(
    _In_ OF_U64 FileSizeBytes
    );

/*
 * Structural checks: non-empty, 4 KB aligned offsets/lengths, no file-space
 * overlap, every slot in [0, SlotCount) maps into some extent past the
 * config sector. VolumeSizeBytes 0 skips volume-bound checks.
 */
OF_STATUS
OfOverlayValidateExtents(
    _In_reads_(ExtentCount) const OF_OVERLAY_EXTENT *Extents,
    _In_ OF_U32 ExtentCount,
    _In_ OF_U64 FileSizeBytes,
    _In_ OF_U32 SlotCount,
    _In_ OF_U64 VolumeSizeBytes
    );

OF_STATUS
OfOverlayMapFileOffsetToVolume(
    _In_reads_(ExtentCount) const OF_OVERLAY_EXTENT *Extents,
    _In_ OF_U32 ExtentCount,
    _In_ OF_U64 FileOffset,
    _In_ OF_U64 Length,
    _Out_ OF_U64 *VolumeOffset
    );

OF_STATUS
OfOverlayMapSlotToVolume(
    _In_reads_(ExtentCount) const OF_OVERLAY_EXTENT *Extents,
    _In_ OF_U32 ExtentCount,
    _In_ OF_U64 ConfigSectorSize,
    _In_ OF_U32 SlotCount,
    _In_ OF_U32 Slot,
    _Out_ OF_U64 *VolumeOffset
    );

/*
 * TRUE if [VolumeOffset, VolumeOffset+Length) lies wholly inside one extent.
 * Used as a safety check before slot I/O hits the volume.
 */
BOOLEAN
OfOverlayVolumeRangeInExtents(
    _In_reads_(ExtentCount) const OF_OVERLAY_EXTENT *Extents,
    _In_ OF_U32 ExtentCount,
    _In_ OF_U64 VolumeOffset,
    _In_ OF_U64 Length
    );

BOOLEAN
OfOverlayUniqueIdEqual(
    _In_reads_bytes_(LeftLength) const UCHAR *Left,
    _In_ OF_U32 LeftLength,
    _In_reads_bytes_(RightLength) const UCHAR *Right,
    _In_ OF_U32 RightLength
    );

#ifdef __cplusplus
}
#endif

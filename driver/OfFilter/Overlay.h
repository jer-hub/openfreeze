/*++

Module Name:

    Overlay.h

Abstract:

    Overlay slot storage backends (RAM and file/extent).

--*/

#pragma once

#include "Driver.h"
#include "OverlayMap.h"

/* RAM backend is preallocated in 1 MB NonPagedPool chunks at device start so
   the write path never allocates slot memory. */
#define OF_RAM_CHUNK_SLOTS 256u

typedef enum _OF_OVERLAY_KIND {
    OfOverlayRam = 0,
    OfOverlayFile = 1
} OF_OVERLAY_KIND;

typedef struct _OF_OVERLAY OF_OVERLAY, *POF_OVERLAY;

typedef
NTSTATUS
(*PFN_OF_OVERLAY_READ_SLOT)(
    _In_ POF_OVERLAY Overlay,
    _In_ OF_U32 Slot,
    _Out_writes_bytes_(OF_BLOCK_SIZE) PVOID Buffer
    );

typedef
NTSTATUS
(*PFN_OF_OVERLAY_WRITE_SLOT)(
    _In_ POF_OVERLAY Overlay,
    _In_ OF_U32 Slot,
    _In_reads_bytes_(OF_BLOCK_SIZE) PVOID Buffer
    );

struct _OF_OVERLAY {
    OF_OVERLAY_KIND Kind;
    OF_U32 SlotCount;
    WDFDEVICE Device;            /* owning filter device (for lower I/O) */
    PFN_OF_OVERLAY_READ_SLOT ReadSlot;
    PFN_OF_OVERLAY_WRITE_SLOT WriteSlot;

    /* RAM backend */
    PUCHAR *RamSlots;            /* SlotCount pointers into RamChunks (preallocated) */
    PUCHAR *RamChunks;           /* RamChunkCount chunks of OF_RAM_CHUNK_SLOTS blocks */
    OF_U32 RamChunkCount;

    /* File backend */
    POF_OVERLAY_EXTENT Extents;
    OF_U32 ExtentCount;
    OF_U64 ConfigSectorSize;     /* bytes reserved at start of overlay file */
};

NTSTATUS
OfOverlayCreateRam(
    _In_ WDFDEVICE Device,
    _In_ OF_U32 SlotCount,
    _Out_ POF_OVERLAY *Overlay
    );

NTSTATUS
OfOverlayCreateFile(
    _In_ WDFDEVICE Device,
    _In_ OF_U32 SlotCount,
    _In_reads_(ExtentCount) POF_OVERLAY_EXTENT Extents,
    _In_ OF_U32 ExtentCount,
    _In_ OF_U64 FileSizeBytes,
    _Out_ POF_OVERLAY *Overlay
    );

VOID
OfOverlayDestroy(
    _In_opt_ POF_OVERLAY Overlay
    );

NTSTATUS
OfOverlayReadSlot(
    _In_ POF_OVERLAY Overlay,
    _In_ OF_U32 Slot,
    _Out_writes_bytes_(OF_BLOCK_SIZE) PVOID Buffer
    );

NTSTATUS
OfOverlayWriteSlot(
    _In_ POF_OVERLAY Overlay,
    _In_ OF_U32 Slot,
    _In_reads_bytes_(OF_BLOCK_SIZE) PVOID Buffer
    );

/* TRUE if [Offset, Offset+Length) intersects any overlay extent on the volume. */
BOOLEAN
OfOverlayRangeIsProtected(
    _In_ POF_OVERLAY Overlay,
    _In_ LONGLONG Offset,
    _In_ size_t Length
    );

NTSTATUS
OfOverlaySlotToVolumeOffset(
    _In_ POF_OVERLAY Overlay,
    _In_ OF_U32 Slot,
    _Out_ LONGLONG *VolumeOffset
    );

/*
 * PASSIVE. Read volume LBA0; return TRUE if a filesystem signature is present
 * (NTFS/FAT). Used to avoid config-sector reads while FVE still returns
 * success+zeros (false Absent -> freeze -> 0xED).
 */
BOOLEAN
OfOverlayProbeVolumeUnlocked(
    _In_ WDFDEVICE Device
    );

/* Raw 4 KB config-sector I/O at overlay file offset 0 via lower device. */
NTSTATUS
OfOverlayConfigSectorRead(
    _In_ WDFDEVICE Device,
    _In_reads_(ExtentCount) const OF_OVERLAY_EXTENT *Extents,
    _In_ OF_U32 ExtentCount,
    _Out_writes_bytes_(OF_OVERLAY_CONFIG_SECTOR_SIZE) PVOID Buffer
    );

NTSTATUS
OfOverlayConfigSectorWrite(
    _In_ WDFDEVICE Device,
    _In_reads_(ExtentCount) const OF_OVERLAY_EXTENT *Extents,
    _In_ OF_U32 ExtentCount,
    _In_reads_bytes_(OF_OVERLAY_CONFIG_SECTOR_SIZE) PVOID Buffer
    );

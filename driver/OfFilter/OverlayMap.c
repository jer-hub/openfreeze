/*++

Module Name:

    OverlayMap.c

Abstract:

    Pure overlay file mapping helpers (see OverlayMap.h).

--*/

#include "OverlayMap.h"

OF_U32
OfOverlayComputeSlotCount(
    _In_ OF_U64 FileSizeBytes
    )
{
    OF_U64 usable;
    OF_U64 slots;

    if (FileSizeBytes <= OF_OVERLAY_CONFIG_SECTOR_SIZE) {
        return 0;
    }

    usable = FileSizeBytes - OF_OVERLAY_CONFIG_SECTOR_SIZE;
    usable &= ~((OF_U64)OF_BLOCK_SIZE - 1u);

    slots = usable / OF_BLOCK_SIZE;
    if (slots == 0) {
        return 0;
    }
    if (slots > OF_MAX_SLOT_COUNT) {
        return OF_MAX_SLOT_COUNT;
    }
    return (OF_U32)slots;
}

static
BOOLEAN
OfOverlayIsBlockAligned(
    _In_ OF_U64 Value
    )
{
    return (Value & ((OF_U64)OF_BLOCK_SIZE - 1u)) == 0;
}

OF_STATUS
OfOverlayMapFileOffsetToVolume(
    _In_reads_(ExtentCount) const OF_OVERLAY_EXTENT *Extents,
    _In_ OF_U32 ExtentCount,
    _In_ OF_U64 FileOffset,
    _In_ OF_U64 Length,
    _Out_ OF_U64 *VolumeOffset
    )
{
    OF_U32 i;
    OF_U64 end;

    if (Extents == NULL || VolumeOffset == NULL || ExtentCount == 0 || Length == 0 ||
        FileOffset > ~(OF_U64)0 - Length) {
        return OfStatusInvalidParameter;
    }
    end = FileOffset + Length;
    for (i = 0; i < ExtentCount; ++i) {
        const OF_OVERLAY_EXTENT *e = &Extents[i];
        if (FileOffset >= e->FileOffset && end <= e->FileOffset + e->Length) {
            *VolumeOffset = e->VolumeOffset + (FileOffset - e->FileOffset);
            return OfStatusOk;
        }
    }
    return OfStatusNotFound;
}

OF_STATUS
OfOverlayMapSlotToVolume(
    _In_reads_(ExtentCount) const OF_OVERLAY_EXTENT *Extents,
    _In_ OF_U32 ExtentCount,
    _In_ OF_U64 ConfigSectorSize,
    _In_ OF_U32 SlotCount,
    _In_ OF_U32 Slot,
    _Out_ OF_U64 *VolumeOffset
    )
{
    OF_U64 fileOffset;
    OF_U32 i;

    if (Extents == NULL || VolumeOffset == NULL || ExtentCount == 0 ||
        Slot >= SlotCount) {
        return OfStatusInvalidParameter;
    }

    fileOffset = ConfigSectorSize + ((OF_U64)Slot * OF_BLOCK_SIZE);

    for (i = 0; i < ExtentCount; ++i) {
        const OF_OVERLAY_EXTENT *e = &Extents[i];
        if (fileOffset >= e->FileOffset &&
            fileOffset + OF_BLOCK_SIZE <= e->FileOffset + e->Length) {
            *VolumeOffset = e->VolumeOffset + (fileOffset - e->FileOffset);
            return OfStatusOk;
        }
    }

    return OfStatusNotFound;
}

OF_STATUS
OfOverlayValidateExtents(
    _In_reads_(ExtentCount) const OF_OVERLAY_EXTENT *Extents,
    _In_ OF_U32 ExtentCount,
    _In_ OF_U64 FileSizeBytes,
    _In_ OF_U32 SlotCount,
    _In_ OF_U64 VolumeSizeBytes
    )
{
    OF_U32 i;
    OF_U32 j;
    OF_U32 slot;
    OF_U64 mapped;
    OF_STATUS status;

    if (Extents == NULL || ExtentCount == 0 ||
        ExtentCount > OF_MAX_OVERLAY_EXTENTS ||
        SlotCount == 0 ||
        FileSizeBytes < OF_OVERLAY_CONFIG_SECTOR_SIZE +
            ((OF_U64)SlotCount * OF_BLOCK_SIZE)) {
        return OfStatusInvalidParameter;
    }

    if (!OfOverlayIsBlockAligned(FileSizeBytes)) {
        return OfStatusInvalidParameter;
    }

    for (i = 0; i < ExtentCount; ++i) {
        const OF_OVERLAY_EXTENT *e = &Extents[i];

        if (e->Length == 0 ||
            !OfOverlayIsBlockAligned(e->FileOffset) ||
            !OfOverlayIsBlockAligned(e->VolumeOffset) ||
            !OfOverlayIsBlockAligned(e->Length)) {
            return OfStatusInvalidParameter;
        }

        if (e->FileOffset >= FileSizeBytes ||
            e->Length > FileSizeBytes - e->FileOffset) {
            return OfStatusInvalidParameter;
        }

        if (VolumeSizeBytes != 0) {
            if (e->VolumeOffset >= VolumeSizeBytes ||
                e->Length > VolumeSizeBytes - e->VolumeOffset) {
                return OfStatusInvalidParameter;
            }
        }

        for (j = i + 1; j < ExtentCount; ++j) {
            const OF_OVERLAY_EXTENT *o = &Extents[j];
            OF_U64 a0 = e->FileOffset;
            OF_U64 a1 = e->FileOffset + e->Length;
            OF_U64 b0 = o->FileOffset;
            OF_U64 b1 = o->FileOffset + o->Length;
            if (a0 < b1 && b0 < a1) {
                return OfStatusInvalidParameter;
            }
        }
    }

    for (slot = 0; slot < SlotCount; ++slot) {
        status = OfOverlayMapSlotToVolume(Extents,
                                          ExtentCount,
                                          OF_OVERLAY_CONFIG_SECTOR_SIZE,
                                          SlotCount,
                                          slot,
                                          &mapped);
        if (status != OfStatusOk) {
            return status;
        }
    }

    return OfStatusOk;
}


BOOLEAN
OfOverlayVolumeRangeInExtents(
    _In_reads_(ExtentCount) const OF_OVERLAY_EXTENT *Extents,
    _In_ OF_U32 ExtentCount,
    _In_ OF_U64 VolumeOffset,
    _In_ OF_U64 Length
    )
{
    OF_U32 i;
    OF_U64 end;

    if (Extents == NULL || ExtentCount == 0 || Length == 0 ||
        VolumeOffset > ~(OF_U64)0 - Length) {
        return FALSE;
    }
    end = VolumeOffset + Length;
    for (i = 0; i < ExtentCount; ++i) {
        OF_U64 eStart = Extents[i].VolumeOffset;
        OF_U64 eEnd = eStart + Extents[i].Length;
        if (VolumeOffset >= eStart && end <= eEnd) {
            return TRUE;
        }
    }
    return FALSE;
}
BOOLEAN
OfOverlayUniqueIdEqual(
    _In_reads_bytes_(LeftLength) const UCHAR *Left,
    _In_ OF_U32 LeftLength,
    _In_reads_bytes_(RightLength) const UCHAR *Right,
    _In_ OF_U32 RightLength
    )
{
    OF_U32 i;

    if (Left == NULL || Right == NULL || LeftLength == 0 ||
        LeftLength != RightLength) {
        return FALSE;
    }

    for (i = 0; i < LeftLength; ++i) {
        if (Left[i] != Right[i]) {
            return FALSE;
        }
    }
    return TRUE;
}

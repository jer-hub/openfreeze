/*++

Module Name:

    Overlay.c

Abstract:

    RAM and file/extent overlay backends.

--*/

#include "Overlay.h"
#include "overlay.tmh"

static
PVOID
OfOverlayPoolAlloc(
    _In_ SIZE_T Size
    )
{
    return ExAllocatePool2(POOL_FLAG_NON_PAGED, Size, OF_POOL_TAG);
}

static
NTSTATUS
OfOverlayRamReadSlot(
    _In_ POF_OVERLAY Overlay,
    _In_ OF_U32 Slot,
    _Out_writes_bytes_(OF_BLOCK_SIZE) PVOID Buffer
    )
{
    if (Slot >= Overlay->SlotCount || Overlay->RamSlots == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    if (Overlay->RamSlots[Slot] == NULL) {
        return STATUS_INVALID_DEVICE_STATE;
    }

    RtlCopyMemory(Buffer, Overlay->RamSlots[Slot], OF_BLOCK_SIZE);
    return STATUS_SUCCESS;
}

static
NTSTATUS
OfOverlayRamWriteSlot(
    _In_ POF_OVERLAY Overlay,
    _In_ OF_U32 Slot,
    _In_reads_bytes_(OF_BLOCK_SIZE) PVOID Buffer
    )
{
    PUCHAR block;

    if (Slot >= Overlay->SlotCount || Overlay->RamSlots == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    block = Overlay->RamSlots[Slot];
    if (block == NULL) {
        return STATUS_INVALID_DEVICE_STATE;
    }

    RtlCopyMemory(block, Buffer, OF_BLOCK_SIZE);
    return STATUS_SUCCESS;
}

static
NTSTATUS
OfOverlayLowerRead(
    _In_ WDFDEVICE Device,
    _In_ LONGLONG Offset,
    _Out_writes_bytes_(Length) PVOID Buffer,
    _In_ ULONG Length
    )
{
    WDF_MEMORY_DESCRIPTOR descriptor;
    ULONG_PTR bytesReturned = 0;
    WDF_REQUEST_SEND_OPTIONS options;
    NTSTATUS status;

    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&descriptor, Buffer, Length);
    WDF_REQUEST_SEND_OPTIONS_INIT(&options, 0);

    status = WdfIoTargetSendReadSynchronously(WdfDeviceGetIoTarget(Device),
                                              NULL,
                                              &descriptor,
                                              &Offset,
                                              &options,
                                              &bytesReturned);
    if (NT_SUCCESS(status) && bytesReturned != Length) {
        /* Pre-zeroed callers (config sector) must not treat a short SUCCESS
         * as Absent/all-zero; that false-Absent froze while FVE was still
         * locked and NTFS mounted with STATUS_FVE_LOCKED_VOLUME (0xED). */
        DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
                   "OfFilter: OfOverlayLowerRead short Offset=%I64d Want=%lu Got=%Iu\n",
                   Offset, Length, bytesReturned);
        status = STATUS_IO_DEVICE_ERROR;
    }
    return status;
}

static
NTSTATUS
OfOverlayLowerWrite(
    _In_ WDFDEVICE Device,
    _In_ LONGLONG Offset,
    _In_reads_bytes_(Length) PVOID Buffer,
    _In_ ULONG Length
    )
{
    WDF_MEMORY_DESCRIPTOR descriptor;
    ULONG_PTR bytesReturned = 0;
    WDF_REQUEST_SEND_OPTIONS options;
    NTSTATUS status;

    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&descriptor, Buffer, Length);
    WDF_REQUEST_SEND_OPTIONS_INIT(&options, 0);

    status = WdfIoTargetSendWriteSynchronously(WdfDeviceGetIoTarget(Device),
                                               NULL,
                                               &descriptor,
                                               &Offset,
                                               &options,
                                               &bytesReturned);
    if (NT_SUCCESS(status) && bytesReturned != Length) {
        DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
                   "OfFilter: OfOverlayLowerWrite short Offset=%I64d Want=%lu Got=%Iu\n",
                   Offset, Length, bytesReturned);
        status = STATUS_IO_DEVICE_ERROR;
    }
    return status;
}

NTSTATUS
OfOverlaySlotToVolumeOffset(
    _In_ POF_OVERLAY Overlay,
    _In_ OF_U32 Slot,
    _Out_ LONGLONG *VolumeOffset
    )
{
    OF_U64 mapped = 0;
    OF_STATUS mapStatus;

    if (Overlay == NULL || VolumeOffset == NULL || Slot >= Overlay->SlotCount) {
        return STATUS_INVALID_PARAMETER;
    }

    if (Overlay->Kind != OfOverlayFile ||
        Overlay->Extents == NULL ||
        Overlay->ExtentCount == 0) {
        return STATUS_INVALID_DEVICE_STATE;
    }

    mapStatus = OfOverlayMapSlotToVolume(Overlay->Extents,
                                         Overlay->ExtentCount,
                                         Overlay->ConfigSectorSize,
                                         Overlay->SlotCount,
                                         Slot,
                                         &mapped);
    if (mapStatus == OfStatusOk) {
        *VolumeOffset = (LONGLONG)mapped;
        return STATUS_SUCCESS;
    }
    if (mapStatus == OfStatusNotFound) {
        return STATUS_NOT_FOUND;
    }
    return STATUS_INVALID_PARAMETER;
}

static
NTSTATUS
OfOverlayFileReadSlot(
    _In_ POF_OVERLAY Overlay,
    _In_ OF_U32 Slot,
    _Out_writes_bytes_(OF_BLOCK_SIZE) PVOID Buffer
    )
{
    LONGLONG volumeOffset = 0;
    NTSTATUS status;

    status = OfOverlaySlotToVolumeOffset(Overlay, Slot, &volumeOffset);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    if (!OfOverlayVolumeRangeInExtents(Overlay->Extents,
                                       Overlay->ExtentCount,
                                       (OF_U64)volumeOffset,
                                       OF_BLOCK_SIZE)) {
        DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
                   "OfFilter: slot I/O target outside overlay extents slot=%lu volOff=0x%I64x (READ) - failing I/O\n",
                   Slot, (OF_U64)volumeOffset);
        return STATUS_DEVICE_DATA_ERROR;
    }

    return OfOverlayLowerRead(Overlay->Device, volumeOffset, Buffer, OF_BLOCK_SIZE);
}

static
NTSTATUS
OfOverlayFileWriteSlot(
    _In_ POF_OVERLAY Overlay,
    _In_ OF_U32 Slot,
    _In_reads_bytes_(OF_BLOCK_SIZE) PVOID Buffer
    )
{
    LONGLONG volumeOffset = 0;
    NTSTATUS status;

    status = OfOverlaySlotToVolumeOffset(Overlay, Slot, &volumeOffset);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    if (!OfOverlayVolumeRangeInExtents(Overlay->Extents,
                                       Overlay->ExtentCount,
                                       (OF_U64)volumeOffset,
                                       OF_BLOCK_SIZE)) {
        DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
                   "OfFilter: slot I/O target outside overlay extents slot=%lu volOff=0x%I64x (WRITE) - failing I/O\n",
                   Slot, (OF_U64)volumeOffset);
        return STATUS_DEVICE_DATA_ERROR;
    }

    return OfOverlayLowerWrite(Overlay->Device, volumeOffset, Buffer, OF_BLOCK_SIZE);
}

NTSTATUS
OfOverlayCreateRam(
    _In_ WDFDEVICE Device,
    _In_ OF_U32 SlotCount,
    _Out_ POF_OVERLAY *Overlay
    )
{
    POF_OVERLAY overlay;
    SIZE_T tableBytes;

    if (Overlay == NULL || SlotCount == 0) {
        return STATUS_INVALID_PARAMETER;
    }

    *Overlay = NULL;
    overlay = (POF_OVERLAY)OfOverlayPoolAlloc(sizeof(OF_OVERLAY));
    if (overlay == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    RtlZeroMemory(overlay, sizeof(*overlay));

    tableBytes = (SIZE_T)SlotCount * sizeof(PUCHAR);
    overlay->RamSlots = (PUCHAR *)OfOverlayPoolAlloc(tableBytes);
    if (overlay->RamSlots == NULL) {
        ExFreePoolWithTag(overlay, OF_POOL_TAG);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    RtlZeroMemory(overlay->RamSlots, tableBytes);

    overlay->RamChunkCount = (SlotCount + OF_RAM_CHUNK_SLOTS - 1u) / OF_RAM_CHUNK_SLOTS;
    overlay->RamChunks = (PUCHAR *)OfOverlayPoolAlloc((SIZE_T)overlay->RamChunkCount * sizeof(PUCHAR));
    if (overlay->RamChunks == NULL) {
        OfOverlayDestroy(overlay);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    RtlZeroMemory(overlay->RamChunks, (SIZE_T)overlay->RamChunkCount * sizeof(PUCHAR));
    overlay->SlotCount = SlotCount;

    {
        OF_U32 chunk;
        for (chunk = 0; chunk < overlay->RamChunkCount; ++chunk) {
            OF_U32 first = chunk * OF_RAM_CHUNK_SLOTS;
            OF_U32 n = SlotCount - first;
            OF_U32 j;
            if (n > OF_RAM_CHUNK_SLOTS) {
                n = OF_RAM_CHUNK_SLOTS;
            }
            overlay->RamChunks[chunk] = (PUCHAR)OfOverlayPoolAlloc((SIZE_T)n * OF_BLOCK_SIZE);
            if (overlay->RamChunks[chunk] == NULL) {
                TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE,
                            "RAM overlay prealloc failed at chunk %lu of %lu",
                            chunk, overlay->RamChunkCount);
                OfOverlayDestroy(overlay);
                return STATUS_INSUFFICIENT_RESOURCES;
            }
            for (j = 0; j < n; ++j) {
                overlay->RamSlots[first + j] = overlay->RamChunks[chunk] + ((SIZE_T)j * OF_BLOCK_SIZE);
            }
        }
    }

    overlay->Kind = OfOverlayRam;
    overlay->SlotCount = SlotCount;
    overlay->Device = Device;
    overlay->ReadSlot = OfOverlayRamReadSlot;
    overlay->WriteSlot = OfOverlayRamWriteSlot;
    overlay->ConfigSectorSize = OF_OVERLAY_CONFIG_SECTOR_SIZE;

    *Overlay = overlay;
    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DEVICE,
                "RAM overlay created SlotCount=%lu", SlotCount);
    return STATUS_SUCCESS;
}

NTSTATUS
OfOverlayCreateFile(
    _In_ WDFDEVICE Device,
    _In_ OF_U32 SlotCount,
    _In_reads_(ExtentCount) POF_OVERLAY_EXTENT Extents,
    _In_ OF_U32 ExtentCount,
    _In_ OF_U64 FileSizeBytes,
    _Out_ POF_OVERLAY *Overlay
    )
{
    POF_OVERLAY overlay;
    SIZE_T extentBytes;
    OF_U64 neededFileSize;

    if (Overlay == NULL || SlotCount == 0 || Extents == NULL || ExtentCount == 0) {
        return STATUS_INVALID_PARAMETER;
    }

    *Overlay = NULL;
    neededFileSize = OF_OVERLAY_CONFIG_SECTOR_SIZE + ((OF_U64)SlotCount * OF_BLOCK_SIZE);
    /* Extents describe the real overlay.bin (often larger than SlotCount*block
       when ofoverlay caps SlotCount at OF_MAX_SLOT_COUNT). Validate against the
       real file size, not the slot-implied size ? otherwise a full-file extent
       (len==FileSize) fails with ConfigRefuse after Device.c already accepted it. */
    if (FileSizeBytes < neededFileSize) {
        DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
                   "OfFilter: CreateFile fileSize=%I64u < needed=%I64u\n",
                   FileSizeBytes, neededFileSize);
        return STATUS_INVALID_PARAMETER;
    }

    overlay = (POF_OVERLAY)OfOverlayPoolAlloc(sizeof(OF_OVERLAY));
    if (overlay == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    RtlZeroMemory(overlay, sizeof(*overlay));

    extentBytes = (SIZE_T)ExtentCount * sizeof(OF_OVERLAY_EXTENT);
    overlay->Extents = (POF_OVERLAY_EXTENT)OfOverlayPoolAlloc(extentBytes);
    if (overlay->Extents == NULL) {
        ExFreePoolWithTag(overlay, OF_POOL_TAG);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    RtlCopyMemory(overlay->Extents, Extents, extentBytes);

    if (OfOverlayValidateExtents(overlay->Extents,
                                 ExtentCount,
                                 FileSizeBytes,
                                 SlotCount,
                                 0) != OfStatusOk) {
        DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
                   "OfFilter: CreateFile ValidateExtents failed fileSize=%I64u slots=%lu extents=%lu\n",
                   FileSizeBytes, SlotCount, ExtentCount);
        ExFreePoolWithTag(overlay->Extents, OF_POOL_TAG);
        ExFreePoolWithTag(overlay, OF_POOL_TAG);
        return STATUS_INVALID_PARAMETER;
    }

    overlay->Kind = OfOverlayFile;
    overlay->SlotCount = SlotCount;
    overlay->ExtentCount = ExtentCount;
    overlay->Device = Device;
    overlay->ReadSlot = OfOverlayFileReadSlot;
    overlay->WriteSlot = OfOverlayFileWriteSlot;
    overlay->ConfigSectorSize = OF_OVERLAY_CONFIG_SECTOR_SIZE;

    *Overlay = overlay;
    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DEVICE,
                "File overlay created SlotCount=%lu Extents=%lu",
                SlotCount, ExtentCount);
    return STATUS_SUCCESS;
}

VOID
OfOverlayDestroy(
    _In_opt_ POF_OVERLAY Overlay
    )
{
    OF_U32 i;

    if (Overlay == NULL) {
        return;
    }

    if (Overlay->RamChunks != NULL) {
        for (i = 0; i < Overlay->RamChunkCount; ++i) {
            if (Overlay->RamChunks[i] != NULL) {
                ExFreePoolWithTag(Overlay->RamChunks[i], OF_POOL_TAG);
            }
        }
        ExFreePoolWithTag(Overlay->RamChunks, OF_POOL_TAG);
    }

    if (Overlay->RamSlots != NULL) {
        ExFreePoolWithTag(Overlay->RamSlots, OF_POOL_TAG);
    }

    if (Overlay->Extents != NULL) {
        ExFreePoolWithTag(Overlay->Extents, OF_POOL_TAG);
    }

    ExFreePoolWithTag(Overlay, OF_POOL_TAG);
}

NTSTATUS
OfOverlayReadSlot(
    _In_ POF_OVERLAY Overlay,
    _In_ OF_U32 Slot,
    _Out_writes_bytes_(OF_BLOCK_SIZE) PVOID Buffer
    )
{
    if (Overlay == NULL || Overlay->ReadSlot == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    return Overlay->ReadSlot(Overlay, Slot, Buffer);
}

NTSTATUS
OfOverlayWriteSlot(
    _In_ POF_OVERLAY Overlay,
    _In_ OF_U32 Slot,
    _In_reads_bytes_(OF_BLOCK_SIZE) PVOID Buffer
    )
{
    if (Overlay == NULL || Overlay->WriteSlot == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    return Overlay->WriteSlot(Overlay, Slot, Buffer);
}

BOOLEAN
OfOverlayRangeIsProtected(
    _In_ POF_OVERLAY Overlay,
    _In_ LONGLONG Offset,
    _In_ size_t Length
    )
{
    OF_U64 start;
    OF_U64 end;
    OF_U32 i;

    if (Overlay == NULL ||
        Overlay->Kind != OfOverlayFile ||
        Overlay->Extents == NULL ||
        Length == 0 ||
        Offset < 0) {
        return FALSE;
    }

    start = (OF_U64)Offset;
    end = start + (OF_U64)Length;

    for (i = 0; i < Overlay->ExtentCount; ++i) {
        OF_U64 eStart = Overlay->Extents[i].VolumeOffset;
        OF_U64 eEnd = eStart + Overlay->Extents[i].Length;
        if (start < eEnd && end > eStart) {
            return TRUE;
        }
    }

    return FALSE;
}

BOOLEAN
OfOverlayProbeVolumeUnlocked(
    _In_ WDFDEVICE Device
    )
{
    PUCHAR buf;
    NTSTATUS st;
    BOOLEAN ok = FALSE;
    LONGLONG off = 0;

    buf = (PUCHAR)ExAllocatePool2(POOL_FLAG_NON_PAGED, 512, OF_POOL_TAG);
    if (buf == NULL) {
        return FALSE;
    }
    RtlZeroMemory(buf, 512);
    st = OfOverlayLowerRead(Device, off, buf, 512);
    if (NT_SUCCESS(st)) {
        /* NTFS OEM ID at offset 3, or FAT "FAT" / "FAT32" at 0x36/0x52. */
        if (buf[3] == 'N' && buf[4] == 'T' && buf[5] == 'F' && buf[6] == 'S') {
            ok = TRUE;
        } else if (buf[0x36] == 'F' && buf[0x37] == 'A' && buf[0x38] == 'T') {
            ok = TRUE;
        } else if (buf[0x52] == 'F' && buf[0x53] == 'A' && buf[0x54] == 'T') {
            ok = TRUE;
        }
    }
    ExFreePoolWithTag(buf, OF_POOL_TAG);
    return ok;
}

NTSTATUS
OfOverlayConfigSectorRead(
    _In_ WDFDEVICE Device,
    _In_reads_(ExtentCount) const OF_OVERLAY_EXTENT *Extents,
    _In_ OF_U32 ExtentCount,
    _Out_writes_bytes_(OF_OVERLAY_CONFIG_SECTOR_SIZE) PVOID Buffer
    )
{
    OF_U64 volOff = 0;
    OF_STATUS ms;
    if (Device == NULL || Extents == NULL || Buffer == NULL || ExtentCount == 0) {
        return STATUS_INVALID_PARAMETER;
    }
    ms = OfOverlayMapFileOffsetToVolume(Extents,
                                        ExtentCount,
                                        0,
                                        OF_OVERLAY_CONFIG_SECTOR_SIZE,
                                        &volOff);
    if (ms != OfStatusOk) {
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }
    if (!OfOverlayVolumeRangeInExtents(Extents, ExtentCount, volOff, OF_OVERLAY_CONFIG_SECTOR_SIZE)) {
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }
    return OfOverlayLowerRead(Device, (LONGLONG)volOff, Buffer, OF_OVERLAY_CONFIG_SECTOR_SIZE);
}

NTSTATUS
OfOverlayConfigSectorWrite(
    _In_ WDFDEVICE Device,
    _In_reads_(ExtentCount) const OF_OVERLAY_EXTENT *Extents,
    _In_ OF_U32 ExtentCount,
    _In_reads_bytes_(OF_OVERLAY_CONFIG_SECTOR_SIZE) PVOID Buffer
    )
{
    OF_U64 volOff = 0;
    OF_STATUS ms;
    if (Device == NULL || Extents == NULL || Buffer == NULL || ExtentCount == 0) {
        return STATUS_INVALID_PARAMETER;
    }
    ms = OfOverlayMapFileOffsetToVolume(Extents,
                                        ExtentCount,
                                        0,
                                        OF_OVERLAY_CONFIG_SECTOR_SIZE,
                                        &volOff);
    if (ms != OfStatusOk) {
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }
    if (!OfOverlayVolumeRangeInExtents(Extents, ExtentCount, volOff, OF_OVERLAY_CONFIG_SECTOR_SIZE)) {
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }
    return OfOverlayLowerWrite(Device, (LONGLONG)volOff, Buffer, OF_OVERLAY_CONFIG_SECTOR_SIZE);
}

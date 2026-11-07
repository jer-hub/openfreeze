/*++

Module Name:

    Io.c

Abstract:

    Passthrough for non-frozen volumes (EnableCow=0 or no match). While
    frozen: ordered worker thread, CoW read split/merge, RMW writes, overlay
    extent passthrough, TRIM swallow, STATUS_DISK_FULL when overlay is full.

--*/

#include "Io.h"
#include "Device.h"
#include "OfConfig.h"
#include "io.tmh"

#ifdef ALLOC_PRAGMA
#pragma alloc_text(PAGE, OfFilterConfigureIoQueues)
#pragma alloc_text(PAGE, OfFilterApplyThawNextBoot)
#endif

//
// ---------------------------------------------------------------------------
// Phase 2 CoW engine (RAM backend).
//
// Model: while frozen, EvtIoRead/EvtIoWrite/flush forward to the manual
// DEVICE_CONTEXT::WorkQueue (reads may bypass). One system thread pulls in
// arrival order and does all lower I/O synchronously at PASSIVE_LEVEL using a
// single preallocated WDFREQUEST, under the one map lock (CowLock). Nothing in
// this path allocates slot memory: RAM slots are preallocated at device start.
// ---------------------------------------------------------------------------
//

static
NTSTATUS
OfFilterLowerRead(
    _In_ WDFDEVICE Device,
    _In_ LONGLONG Offset,
    _Out_writes_bytes_(Length) PVOID Buffer,
    _In_ ULONG Length
    )
{
    PDEVICE_CONTEXT deviceCtx = OfFilterGetDeviceContext(Device);
    WDF_MEMORY_DESCRIPTOR descriptor;
    WDF_REQUEST_REUSE_PARAMS reuse;
    ULONG_PTR bytesReturned = 0;
    NTSTATUS status;

    if (deviceCtx->LowerRequest == NULL || Buffer == NULL || Length == 0) {
        return STATUS_INVALID_PARAMETER;
    }

    WDF_REQUEST_REUSE_PARAMS_INIT(&reuse, WDF_REQUEST_REUSE_NO_FLAGS, STATUS_SUCCESS);
    status = WdfRequestReuse(deviceCtx->LowerRequest, &reuse);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&descriptor, Buffer, Length);
    status = WdfIoTargetSendReadSynchronously(WdfDeviceGetIoTarget(Device),
                                              deviceCtx->LowerRequest,
                                              &descriptor,
                                              &Offset,
                                              NULL,
                                              &bytesReturned);
    if (NT_SUCCESS(status) && bytesReturned != Length) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_IO,
                    "Short lower read Offset=%!I64d! Want=%lu Got=%Iu",
                    Offset, Length, bytesReturned);
        status = STATUS_IO_DEVICE_ERROR;
    }
    return status;
}

static
NTSTATUS
OfFilterLowerReadIntoMdl(
    _In_ WDFDEVICE Device,
    _In_ LONGLONG Offset,
    _In_ PMDL SourceMdl,
    _In_ size_t MdlOffset,
    _In_ ULONG Length
    );

//
// Read the original contents of one 4 KB block from the lower device into
// Buffer. A last block that is shorter than 4 KB is zero-padded.
//
static
NTSTATUS
OfFilterLowerReadBlock(
    _In_ WDFDEVICE Device,
    _In_ OF_U32 BlockIndex,
    _Out_writes_bytes_(OF_BLOCK_SIZE) PUCHAR Buffer
    )
{
    PDEVICE_CONTEXT deviceCtx = OfFilterGetDeviceContext(Device);
    OF_U64 base = (OF_U64)BlockIndex << OF_BLOCK_SHIFT;
    OF_U64 avail;
    ULONG readLen = OF_BLOCK_SIZE;

    if (base >= deviceCtx->VolumeSizeBytes) {
        return STATUS_INVALID_PARAMETER;
    }
    avail = deviceCtx->VolumeSizeBytes - base;
    if (avail < OF_BLOCK_SIZE) {
        readLen = (ULONG)avail;
        RtlZeroMemory(Buffer + readLen, OF_BLOCK_SIZE - readLen);
    }
    if (Buffer == deviceCtx->ScratchBlock && deviceCtx->ScratchMdl != NULL) {
        /* Preallocated MDL: no allocation on the worker path. */
        return OfFilterLowerReadIntoMdl(Device, (LONGLONG)base, deviceCtx->ScratchMdl, 0, readLen);
    }
    return OfFilterLowerRead(Device, (LONGLONG)base, Buffer, readLen);
}

static
NTSTATUS
OfFilterCheckRange(
    _In_ PDEVICE_CONTEXT DeviceCtx,
    _In_ LONGLONG Offset,
    _In_ size_t Length
    )
{
    if (Offset < 0 ||
        (OF_U64)Offset > DeviceCtx->VolumeSizeBytes ||
        (OF_U64)Length > DeviceCtx->VolumeSizeBytes - (OF_U64)Offset ||
        Length > MAXULONG) {
        return STATUS_INVALID_PARAMETER;
    }
    return STATUS_SUCCESS;
}

//
// Frozen read. Splits on 4 KB block boundaries. Mapped blocks are copied from
// the overlay slot. Runs of consecutive unmapped blocks are read from the
// lower device in one call, straight into the caller's buffer (run edges are
// either the request edges or 4 KB boundaries, so they stay sector aligned).
// Completes the request on every path.
//
//
// Read straight into part of the caller's direct-I/O MDL. Builds a partial
// MDL over the caller's already-locked pages instead of letting the framework
// probe-and-lock a system-space alias of them (MmProbeAndLockPages on a
// mapped paging-I/O buffer bugchecked 0xA under Verifier during shutdown).
//
static
NTSTATUS
OfFilterLowerReadIntoMdl(
    _In_ WDFDEVICE Device,
    _In_ LONGLONG Offset,
    _In_ PMDL SourceMdl,
    _In_ size_t MdlOffset,
    _In_ ULONG Length
    )
{
    PDEVICE_CONTEXT deviceCtx = OfFilterGetDeviceContext(Device);
    WDF_MEMORY_DESCRIPTOR descriptor;
    WDF_REQUEST_REUSE_PARAMS reuse;
    ULONG_PTR bytesReturned = 0;
    PUCHAR va = (PUCHAR)MmGetMdlVirtualAddress(SourceMdl) + MdlOffset;
    PMDL partial = deviceCtx->PartialMdl;
    NTSTATUS status;

    //
    // The partial MDL is preallocated for OF_MAX_LOWER_RUN bytes (callers
    // split longer runs) and only used by the worker thread, one I/O at a
    // time, so nothing here allocates.
    //
    if (deviceCtx->LowerRequest == NULL || partial == NULL ||
        Length == 0 || Length > OF_MAX_LOWER_RUN) {
        return STATUS_INVALID_PARAMETER;
    }
    MmInitializeMdl(partial, (PVOID)(ULONG_PTR)(PAGE_SIZE - 1), OF_MAX_LOWER_RUN);
    IoBuildPartialMdl(SourceMdl, partial, va, Length);

    WDF_REQUEST_REUSE_PARAMS_INIT(&reuse, WDF_REQUEST_REUSE_NO_FLAGS, STATUS_SUCCESS);
    status = WdfRequestReuse(deviceCtx->LowerRequest, &reuse);
    if (NT_SUCCESS(status)) {
        WDF_MEMORY_DESCRIPTOR_INIT_MDL(&descriptor, partial, Length);
        status = WdfIoTargetSendReadSynchronously(WdfDeviceGetIoTarget(Device),
                                                  deviceCtx->LowerRequest,
                                                  &descriptor, &Offset, NULL,
                                                  &bytesReturned);
        if (NT_SUCCESS(status) && bytesReturned != Length) {
            status = STATUS_IO_DEVICE_ERROR;
        }
    }
    MmPrepareMdlForReuse(partial);   /* drops any system mapping the lower stack made */
    return status;
}
//
// TRUE if a frozen request may go straight to disk because it lies wholly
// inside one passthrough range (pagefile/swapfile/hiberfil/bootstat
// extents) and none of its blocks is mapped. A mapped block means the
// overlay holds the current data, so the map wins and the request takes the
// normal CoW path. Pure lookups: the caller either is the worker (the only
// thread that changes the map) or holds CowLock.
//
static
BOOLEAN
OfFilterPassthroughClean(
    _In_ PDEVICE_CONTEXT DeviceCtx,
    _In_ LONGLONG Offset,
    _In_ size_t Length
    )
{
    OF_U64 block;
    OF_U64 last;

    if (DeviceCtx->Passthrough.Count == 0 || Length == 0 ||
        !NT_SUCCESS(OfFilterCheckRange(DeviceCtx, Offset, Length)) ||
        !OfRangeListContains(&DeviceCtx->Passthrough, (OF_U64)Offset, (OF_U64)Length)) {
        return FALSE;
    }
    if (OfBlockMapSlotsUsed(&DeviceCtx->BlockMap) == 0) {
        return TRUE;
    }
    last = ((OF_U64)Offset + Length - 1) >> OF_BLOCK_SHIFT;
    for (block = (OF_U64)Offset >> OF_BLOCK_SHIFT; block <= last; block++) {
        OF_U32 slot;
        if (OfBlockMapLookup(&DeviceCtx->BlockMap, (OF_U32)block, &slot) != OfStatusNotFound) {
            return FALSE;
        }
    }
    return TRUE;
}

static
VOID
OfFilterProcessCowRead(
    _In_ WDFDEVICE Device,
    _In_ WDFREQUEST Request
    )
{
    PDEVICE_CONTEXT deviceCtx = OfFilterGetDeviceContext(Device);
    WDF_REQUEST_PARAMETERS params;
    PUCHAR userBuffer = NULL;
    PMDL requestMdl = NULL;
    size_t bufferLength = 0;
    LONGLONG offset;
    size_t length;
    size_t pos = 0;
    NTSTATUS status;

    WDF_REQUEST_PARAMETERS_INIT(&params);
    WdfRequestGetParameters(Request, &params);
    offset = params.Parameters.Read.DeviceOffset;
    length = params.Parameters.Read.Length;

    if (OfOverlayRangeIsProtected(deviceCtx->Overlay, offset, length)) {
        OfFilterForwardRequest(Request, WdfDeviceGetIoTarget(Device));
        return;
    }
    if (OfFilterPassthroughClean(deviceCtx, offset, length)) {
        InterlockedIncrement64(&deviceCtx->PassthroughReads);
        OfFilterForwardRequest(Request, WdfDeviceGetIoTarget(Device));
        return;
    }

    if (length == 0) {
        WdfRequestCompleteWithInformation(Request, STATUS_SUCCESS, 0);
        return;
    }

    status = OfFilterCheckRange(deviceCtx, offset, length);
    if (!NT_SUCCESS(status)) {
        WdfRequestComplete(Request, status);
        return;
    }

    status = WdfRequestRetrieveOutputBuffer(Request, length, (PVOID *)&userBuffer, &bufferLength);
    if (!NT_SUCCESS(status) || userBuffer == NULL || bufferLength < length) {
        WdfRequestComplete(Request, !NT_SUCCESS(status) ? status : STATUS_BUFFER_TOO_SMALL);
        return;
    }

    if (!NT_SUCCESS(WdfRequestRetrieveOutputWdmMdl(Request, &requestMdl))) {
        requestMdl = NULL;   // buffered/neither I/O: fall back to the buffer path
    }

    WdfWaitLockAcquire(deviceCtx->CowLock, NULL);

    status = STATUS_SUCCESS;
    while (pos < length) {
        OF_U64 cur = (OF_U64)offset + pos;
        OF_U32 blockIndex = (OF_U32)(cur >> OF_BLOCK_SHIFT);
        ULONG blockOff = (ULONG)(cur & (OF_BLOCK_SIZE - 1));
        size_t chunk = OF_BLOCK_SIZE - blockOff;
        OF_U32 slot;
        OF_STATUS mapStatus;

        if (chunk > length - pos) {
            chunk = length - pos;
        }

        mapStatus = OfBlockMapLookup(&deviceCtx->BlockMap, blockIndex, &slot);
        if (mapStatus == OfStatusOk) {
            status = OfOverlayReadSlot(deviceCtx->Overlay, slot, deviceCtx->ScratchBlock);
            if (!NT_SUCCESS(status)) {
                break;
            }
            RtlCopyMemory(userBuffer + pos, deviceCtx->ScratchBlock + blockOff, chunk);
            pos += chunk;
            continue;
        }
        if (mapStatus != OfStatusNotFound) {
            status = STATUS_INVALID_PARAMETER;
            break;
        }

        //
        // Extend the unmapped run across following blocks.
        //
        {
            size_t runLen = chunk;
            while (pos + runLen < length) {
                OF_U32 nextBlock = (OF_U32)(((OF_U64)offset + pos + runLen) >> OF_BLOCK_SHIFT);
                size_t nextChunk = OF_BLOCK_SIZE;
                if (nextChunk > length - pos - runLen) {
                    nextChunk = length - pos - runLen;
                }
                if (runLen + nextChunk > OF_MAX_LOWER_RUN) {
                    break;   /* preallocated partial MDL size; next loop continues */
                }
                if (OfBlockMapLookup(&deviceCtx->BlockMap, nextBlock, &slot) != OfStatusNotFound) {
                    break;
                }
                runLen += nextChunk;
            }

            status = (requestMdl != NULL)
                ? OfFilterLowerReadIntoMdl(Device, (LONGLONG)cur, requestMdl, pos, (ULONG)runLen)
                : OfFilterLowerRead(Device, (LONGLONG)cur, userBuffer + pos, (ULONG)runLen);
            if (!NT_SUCCESS(status)) {
                break;
            }
            pos += runLen;
        }
    }

    WdfWaitLockRelease(deviceCtx->CowLock);

    if (NT_SUCCESS(status)) {
        WdfRequestCompleteWithInformation(Request, STATUS_SUCCESS, length);
    } else {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_IO,
                    "CoW read failed Offset=%!I64d! Length=%Iu %!STATUS!",
                    offset, length, status);
        WdfRequestComplete(Request, status);
    }
}

//
// Frozen write. Every touched 4 KB block gets an overlay slot. Full blocks
// are written straight from the caller's buffer. Partial blocks are
// read-modify-write: original block (new slot) or existing slot, merge, write
// the full block to the slot. The lower device is never written.
// Overlay full -> STATUS_DISK_FULL. A slot created for a block whose fill
// failed is unmapped again so it can never expose undefined data.
//
static
VOID
OfFilterProcessCowWrite(
    _In_ WDFDEVICE Device,
    _In_ WDFREQUEST Request
    )
{
    PDEVICE_CONTEXT deviceCtx = OfFilterGetDeviceContext(Device);
    WDF_REQUEST_PARAMETERS params;
    PUCHAR userBuffer = NULL;
    PMDL requestMdl = NULL;
    size_t bufferLength = 0;
    LONGLONG offset;
    size_t length;
    size_t pos = 0;
    NTSTATUS status;

    WDF_REQUEST_PARAMETERS_INIT(&params);
    WdfRequestGetParameters(Request, &params);
    offset = params.Parameters.Write.DeviceOffset;
    length = params.Parameters.Write.Length;

    if (OfOverlayRangeIsProtected(deviceCtx->Overlay, offset, length)) {
        OfFilterForwardRequest(Request, WdfDeviceGetIoTarget(Device));
        return;
    }
    if (OfFilterPassthroughClean(deviceCtx, offset, length)) {
        InterlockedIncrement64(&deviceCtx->PassthroughWrites);
        OfFilterForwardRequest(Request, WdfDeviceGetIoTarget(Device));
        return;
    }

    if (length == 0) {
        WdfRequestCompleteWithInformation(Request, STATUS_SUCCESS, 0);
        return;
    }

    status = OfFilterCheckRange(deviceCtx, offset, length);
    if (!NT_SUCCESS(status)) {
        WdfRequestComplete(Request, status);
        return;
    }

    status = WdfRequestRetrieveInputBuffer(Request, length, (PVOID *)&userBuffer, &bufferLength);
    if (!NT_SUCCESS(status) || userBuffer == NULL || bufferLength < length) {
        WdfRequestComplete(Request, !NT_SUCCESS(status) ? status : STATUS_BUFFER_TOO_SMALL);
        return;
    }

    if (!NT_SUCCESS(WdfRequestRetrieveOutputWdmMdl(Request, &requestMdl))) {
        requestMdl = NULL;   // buffered/neither I/O: fall back to the buffer path
    }

    WdfWaitLockAcquire(deviceCtx->CowLock, NULL);

    //
    // Atomic capacity check: before any block of this request is touched,
    // make sure every L2 table exists and enough free slots remain for all
    // of its unmapped blocks. If not, the whole request fails with
    // STATUS_DISK_FULL (or STATUS_INSUFFICIENT_RESOURCES) and no block is
    // remapped or modified. Nothing on this path forwards to the disk.
    //
    {
        OF_U32 needed = 0;
        OF_STATUS prep = OfBlockMapPrepareRange(&deviceCtx->BlockMap,
                                                (OF_U32)((OF_U64)offset >> OF_BLOCK_SHIFT),
                                                (OF_U32)(((OF_U64)offset + length - 1) >> OF_BLOCK_SHIFT),
                                                &needed);
        if (prep != OfStatusOk) {
            if (prep == OfStatusFull) {
                deviceCtx->OverlayFullCount += 1;
                if (!deviceCtx->OverlayFullLogged) {
                    deviceCtx->OverlayFullLogged = TRUE;
                    TraceEvents(TRACE_LEVEL_ERROR, TRACE_IO,
                                "Overlay FULL (%lu slots used, %lu needed) - failing writes with STATUS_DISK_FULL",
                                OfBlockMapSlotsUsed(&deviceCtx->BlockMap), needed);
                    DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
                               "OfFilter: overlay FULL (%lu slots) - writes fail with STATUS_DISK_FULL\n",
                               OfBlockMapSlotsUsed(&deviceCtx->BlockMap));
                }
                OfFilterSignalOverlayEvent(deviceCtx, OF_OVERLAY_EVENT_FULL);
                status = STATUS_DISK_FULL;
            } else {
                status = STATUS_INSUFFICIENT_RESOURCES;
                TraceEvents(TRACE_LEVEL_ERROR, TRACE_IO,
                            "CoW write precheck failed (%d) Offset=%!I64d! Length=%Iu",
                            (int)prep, offset, length);
            }
            WdfWaitLockRelease(deviceCtx->CowLock);
            WdfRequestComplete(Request, status);
            return;
        }
    }

    status = STATUS_SUCCESS;
    while (pos < length) {
        OF_U64 cur = (OF_U64)offset + pos;
        OF_U32 blockIndex = (OF_U32)(cur >> OF_BLOCK_SHIFT);
        ULONG blockOff = (ULONG)(cur & (OF_BLOCK_SIZE - 1));
        size_t chunk = OF_BLOCK_SIZE - blockOff;
        OF_U32 slot;
        BOOLEAN created = FALSE;
        OF_STATUS mapStatus;

        if (chunk > length - pos) {
            chunk = length - pos;
        }

        mapStatus = OfBlockMapMapBlock(&deviceCtx->BlockMap, blockIndex, &slot, &created);
        if (mapStatus == OfStatusFull) {
            deviceCtx->OverlayFullCount += 1;
            if (!deviceCtx->OverlayFullLogged) {
                deviceCtx->OverlayFullLogged = TRUE;
                TraceEvents(TRACE_LEVEL_ERROR, TRACE_IO,
                            "Overlay FULL (%lu slots) - failing writes with STATUS_DISK_FULL",
                            OfBlockMapSlotsUsed(&deviceCtx->BlockMap));
                DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
                           "OfFilter: overlay FULL (%lu slots) - writes fail with STATUS_DISK_FULL\n",
                           OfBlockMapSlotsUsed(&deviceCtx->BlockMap));
            }
            OfFilterSignalOverlayEvent(deviceCtx, OF_OVERLAY_EVENT_FULL);
            status = STATUS_DISK_FULL;
            break;
        }
        if (mapStatus != OfStatusOk) {
            status = STATUS_INSUFFICIENT_RESOURCES;
            break;
        }

        /* Near-full alert while still accepting writes. */
        {
            OF_U32 usedNow = OfBlockMapSlotsUsed(&deviceCtx->BlockMap);
            OF_U32 totalNow = deviceCtx->BlockMap.SlotCount;
            if (totalNow > 0 &&
                usedNow * 100u >= totalNow * OF_OVERLAY_NEAR_FULL_PCT) {
                OfFilterSignalOverlayEvent(deviceCtx, OF_OVERLAY_EVENT_NEAR_FULL);
            }
        }

        if (blockOff == 0 && chunk == OF_BLOCK_SIZE) {
            status = OfOverlayWriteSlot(deviceCtx->Overlay, slot, userBuffer + pos);
        } else {
            if (created) {
                status = OfFilterLowerReadBlock(Device, blockIndex, deviceCtx->ScratchBlock);
            } else {
                status = OfOverlayReadSlot(deviceCtx->Overlay, slot, deviceCtx->ScratchBlock);
            }
            if (NT_SUCCESS(status)) {
                RtlCopyMemory(deviceCtx->ScratchBlock + blockOff, userBuffer + pos, chunk);
                status = OfOverlayWriteSlot(deviceCtx->Overlay, slot, deviceCtx->ScratchBlock);
            }
        }

        if (!NT_SUCCESS(status)) {
            if (created) {
                (VOID)OfBlockMapUnmapBlock(&deviceCtx->BlockMap, blockIndex);
            }
            break;
        }

        pos += chunk;
    }

    WdfWaitLockRelease(deviceCtx->CowLock);

    if (NT_SUCCESS(status)) {
        WdfRequestCompleteWithInformation(Request, STATUS_SUCCESS, length);
    } else {
        if (status != STATUS_DISK_FULL) {
            TraceEvents(TRACE_LEVEL_ERROR, TRACE_IO,
                        "CoW write failed Offset=%!I64d! Length=%Iu %!STATUS!",
                        offset, length, status);
        }
        WdfRequestComplete(Request, status);
    }
}

VOID
OfFilterSetWorkerStop(
    _In_ PDEVICE_CONTEXT DeviceCtx,
    _In_ BOOLEAN Stop
    )
{
    KIRQL irql;
    KeAcquireSpinLock(&DeviceCtx->WorkLock, &irql);
    DeviceCtx->WorkerStop = Stop;
    KeReleaseSpinLock(&DeviceCtx->WorkLock, irql);
}

//
// Hand a frozen request to the worker's manual queue. Returns FALSE (request
// untouched, still owned by the caller) if the worker is not accepting work.
// The WorkerStop check and the forward happen under WorkLock so a request can
// never land in the queue after the worker has drained it and exited.
//
static
BOOLEAN
OfFilterQueueToWorker(
    _In_ PDEVICE_CONTEXT DeviceCtx,
    _In_ WDFREQUEST Request,
    _In_ BOOLEAN IsWrite
    )
{
    KIRQL irql;
    BOOLEAN queued = FALSE;
    NTSTATUS status;

    KeAcquireSpinLock(&DeviceCtx->WorkLock, &irql);
    if (!DeviceCtx->WorkerStop && DeviceCtx->WorkQueue != NULL) {
        if (IsWrite) {
            DeviceCtx->PendingWrites += 1;
        }
        status = WdfRequestForwardToIoQueue(Request, DeviceCtx->WorkQueue);
        if (NT_SUCCESS(status)) {
            queued = TRUE;
        } else {
            if (IsWrite) {
                DeviceCtx->PendingWrites -= 1;
            }
            TraceEvents(TRACE_LEVEL_ERROR, TRACE_IO,
                        "WdfRequestForwardToIoQueue failed %!STATUS!", status);
        }
    }
    KeReleaseSpinLock(&DeviceCtx->WorkLock, irql);

    if (queued) {
        KeSetEvent(&DeviceCtx->WorkEvent, IO_NO_INCREMENT, FALSE);
    }
    return queued;
}

static
VOID
OfFilterDispatchFrozen(
    _In_ WDFDEVICE Device,
    _In_ WDFREQUEST Request,
    _In_ BOOLEAN IsWrite
    )
{
    PDEVICE_CONTEXT deviceCtx = OfFilterGetDeviceContext(Device);

    if (!OfFilterQueueToWorker(deviceCtx, Request, IsWrite)) {
        //
        // Frozen but the worker is stopping: never fall back to passthrough
        // for reads/writes, the overlay holds newer data than the disk.
        //
        WdfRequestComplete(Request, STATUS_DEVICE_NOT_READY);
    }
}

//
// Read bypass. A frozen read may go straight to the lower device (send and
// forget, no worker) only when:
//   1. no write is queued or in progress in the worker (PendingWrites == 0),
//      so the read cannot overtake an earlier write to the same blocks;
//   2. the map lock can be taken without waiting (try-acquire, legal at
//      DISPATCH_LEVEL), so the worker is not mid-update; and
//   3. every block in the range is unmapped in the overlay.
// A write that arrives after check 1 is concurrent with this read; the upper
// layer gives no ordering between overlapping in-flight I/O, so either result
// is valid. A write already finished has its blocks mapped before
// PendingWrites drops, so check 3 sees it. Otherwise the read goes to the
// worker. Called at <= DISPATCH_LEVEL. Returns TRUE if Request was consumed.
//
static
BOOLEAN
OfFilterTryBypassRead(
    _In_ WDFDEVICE Device,
    _In_ WDFREQUEST Request,
    _In_ LONGLONG Offset,
    _In_ size_t Length
    )
{
    PDEVICE_CONTEXT deviceCtx = OfFilterGetDeviceContext(Device);
    LONGLONG zero;
    KIRQL irql;
    LONG pending;
    BOOLEAN clean = TRUE;
    OF_U64 first;
    OF_U64 last;
    OF_U64 block;

    if (Length == 0 || !NT_SUCCESS(OfFilterCheckRange(deviceCtx, Offset, Length))) {
        return FALSE;
    }

    KeAcquireSpinLock(&deviceCtx->WorkLock, &irql);
    pending = deviceCtx->PendingWrites;
    KeReleaseSpinLock(&deviceCtx->WorkLock, irql);
    if (pending != 0) {
        return FALSE;
    }

    zero = 0;
    if (WdfWaitLockAcquire(deviceCtx->CowLock, &zero) != STATUS_SUCCESS) {
        return FALSE;
    }

    first = (OF_U64)Offset >> OF_BLOCK_SHIFT;
    last = ((OF_U64)Offset + Length - 1) >> OF_BLOCK_SHIFT;
    if (OfBlockMapSlotsUsed(&deviceCtx->BlockMap) != 0) {
        for (block = first; block <= last; block++) {
            OF_U32 slot;
            if (OfBlockMapLookup(&deviceCtx->BlockMap, (OF_U32)block, &slot) != OfStatusNotFound) {
                clean = FALSE;
                break;
            }
        }
    }

    WdfWaitLockRelease(deviceCtx->CowLock);

    if (!clean) {
        return FALSE;
    }

    InterlockedIncrement64(&deviceCtx->BypassReads);
    OfFilterForwardRequest(Request, WdfDeviceGetIoTarget(Device));
    return TRUE;
}

//
// Write fast path for passthrough ranges (pagefile and friends), so paging
// writes do not wait behind the worker. Same ordering rule as the read
// bypass: only when no write is queued or in progress, the map lock is free
// (try-acquire, legal at DISPATCH_LEVEL) and no block is mapped. Otherwise
// the worker handles it in order (and still passes it through if clean).
// Returns TRUE if Request was consumed.
//
static
BOOLEAN
OfFilterTryPassthroughWrite(
    _In_ WDFDEVICE Device,
    _In_ WDFREQUEST Request,
    _In_ LONGLONG Offset,
    _In_ size_t Length
    )
{
    PDEVICE_CONTEXT deviceCtx = OfFilterGetDeviceContext(Device);
    LONGLONG zero = 0;
    KIRQL irql;
    LONG pending;
    BOOLEAN clean;

    if (deviceCtx->Passthrough.Count == 0 ||
        !OfRangeListContains(&deviceCtx->Passthrough, (OF_U64)Offset, (OF_U64)Length)) {
        return FALSE;
    }

    KeAcquireSpinLock(&deviceCtx->WorkLock, &irql);
    pending = deviceCtx->PendingWrites;
    KeReleaseSpinLock(&deviceCtx->WorkLock, irql);
    if (pending != 0) {
        return FALSE;
    }

    if (WdfWaitLockAcquire(deviceCtx->CowLock, &zero) != STATUS_SUCCESS) {
        return FALSE;
    }
    clean = OfFilterPassthroughClean(deviceCtx, Offset, Length);
    WdfWaitLockRelease(deviceCtx->CowLock);

    if (!clean) {
        return FALSE;
    }
    InterlockedIncrement64(&deviceCtx->PassthroughWrites);
    OfFilterForwardRequest(Request, WdfDeviceGetIoTarget(Device));
    return TRUE;
}

_Use_decl_annotations_
VOID
OfFilterCowWorker(
    PVOID StartContext
    )
{
    WDFDEVICE device = (WDFDEVICE)StartContext;
    PDEVICE_CONTEXT deviceCtx = OfFilterGetDeviceContext(device);

    ULONG batch = 0;

    //
    // Above normal threads and the memory-compression workers (priority 7
    // in the 22:12 NMI capture), so frozen I/O keeps draining under memory
    // pressure. The worker blocks only on lower I/O and WorkEvent.
    //
    (VOID)KeSetPriorityThread(KeGetCurrentThread(), LOW_REALTIME_PRIORITY);
    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_IO, "CoW worker started");

    for (;;) {
        WDFREQUEST request = NULL;
        NTSTATUS status;
        BOOLEAN stop;
        KIRQL irql;

        if (++batch >= OF_WORKER_BATCH) {
            //
            // Bounded batch: give other ready threads a turn. Queued
            // requests stay in order; this only delays the next one.
            //
            LARGE_INTEGER noDelay;
            batch = 0;
            noDelay.QuadPart = 0;
            (VOID)KeDelayExecutionThread(KernelMode, FALSE, &noDelay);
        }

        status = WdfIoQueueRetrieveNextRequest(deviceCtx->WorkQueue, &request);
        if (!NT_SUCCESS(status) || request == NULL) {
            //
            // Queue empty (STATUS_NO_MORE_ENTRIES) or not accepting. Exit
            // only once stop is set; the stop flag and every forward share
            // WorkLock, so nothing can arrive after this check.
            //
            KeAcquireSpinLock(&deviceCtx->WorkLock, &irql);
            stop = deviceCtx->WorkerStop;
            KeReleaseSpinLock(&deviceCtx->WorkLock, irql);
            if (stop) {
                status = WdfIoQueueRetrieveNextRequest(deviceCtx->WorkQueue, &request);
                if (!NT_SUCCESS(status) || request == NULL) {
                    break;
                }
            } else {
                (VOID)KeWaitForSingleObject(&deviceCtx->WorkEvent, Executive, KernelMode, FALSE, NULL);
                batch = 0;
                continue;
            }
        }

        {
            WDF_REQUEST_PARAMETERS params;

            WDF_REQUEST_PARAMETERS_INIT(&params);
            WdfRequestGetParameters(request, &params);

            switch (params.Type) {
            case WdfRequestTypeRead:
                InterlockedIncrement64(&deviceCtx->WorkerReads);
                OfFilterProcessCowRead(device, request);
                break;
            case WdfRequestTypeWrite:
                OfFilterProcessCowWrite(device, request);
                //
                // Blocks are mapped (or the write failed) by now, so reads
                // may bypass again once no other write is pending.
                //
                KeAcquireSpinLock(&deviceCtx->WorkLock, &irql);
                deviceCtx->PendingWrites -= 1;
                KeReleaseSpinLock(&deviceCtx->WorkLock, irql);
                break;
            default:
                //
                // Flush and other ordered requests: every earlier write has
                // already been absorbed by the overlay, so forwarding now
                // keeps ordering.
                //
                OfFilterForwardRequest(request, WdfDeviceGetIoTarget(device));
                break;
            }
        }
    }

    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_IO,
                "CoW worker exiting BypassReads=%I64d WorkerReads=%I64d",
                deviceCtx->BypassReads, deviceCtx->WorkerReads);
    PsTerminateSystemThread(STATUS_SUCCESS);
}

NTSTATUS
OfFilterConfigureIoQueues(
    _In_ WDFDEVICE Device
    )
{
    WDF_IO_QUEUE_CONFIG queueConfig;
    WDFQUEUE queue;
    PDEVICE_CONTEXT deviceCtx = OfFilterGetDeviceContext(Device);
    NTSTATUS status;

    PAGED_CODE();

    //
    // Default queue: parallel, no forced PASSIVE execution level. Callbacks
    // only forward (send-and-forget or to the manual queue) or complete, all
    // legal at DISPATCH_LEVEL, so the framework never has to defer storage
    // I/O (including paging I/O) to a work item.
    //
    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&queueConfig, WdfIoQueueDispatchParallel);
    queueConfig.PowerManaged = WdfFalse;
    queueConfig.EvtIoRead = OfFilterEvtIoRead;
    queueConfig.EvtIoWrite = OfFilterEvtIoWrite;
    queueConfig.EvtIoDefault = OfFilterEvtIoDefault;
    queueConfig.EvtIoDeviceControl = OfFilterEvtIoDeviceControl;

    status = WdfIoQueueCreate(Device, &queueConfig, WDF_NO_OBJECT_ATTRIBUTES, &queue);
    if (!NT_SUCCESS(status)) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_IO,
                    "WdfIoQueueCreate (default) failed %!STATUS!", status);
        return status;
    }

    //
    // Manual queue owned by the CoW worker.
    //
    WDF_IO_QUEUE_CONFIG_INIT(&queueConfig, WdfIoQueueDispatchManual);
    queueConfig.PowerManaged = WdfFalse;
    status = WdfIoQueueCreate(Device, &queueConfig, WDF_NO_OBJECT_ATTRIBUTES, &deviceCtx->WorkQueue);
    if (!NT_SUCCESS(status)) {
        deviceCtx->WorkQueue = NULL;
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_IO,
                    "WdfIoQueueCreate (worker) failed %!STATUS!", status);
        return status;
    }

    return STATUS_SUCCESS;
}
VOID
OfFilterForwardRequest(
    _In_ WDFREQUEST Request,
    _In_ WDFIOTARGET Target
    )
{
    WDF_REQUEST_SEND_OPTIONS options;
    BOOLEAN sent;
    NTSTATUS status;

    WDF_REQUEST_SEND_OPTIONS_INIT(&options, WDF_REQUEST_SEND_OPTION_SEND_AND_FORGET);

    sent = WdfRequestSend(Request, Target, &options);
    if (!sent) {
        status = WdfRequestGetStatus(Request);
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_IO,
                    "WdfRequestSend failed %!STATUS!", status);
        WdfRequestComplete(Request, status);
    }
}

NTSTATUS
OfFilterApplyThawNextBoot(
    _In_ WDFDEVICE Device,
    _In_ BOOLEAN WantSet
    )
{
    return OfFilterApplyNextBootMode(Device,
                                     WantSet ? OF_MODE_THAWED : OF_MODE_FROZEN,
                                     0);
}

NTSTATUS
OfFilterApplyNextBootMode(
    _In_ WDFDEVICE Device,
    _In_ ULONG Mode,
    _In_ ULONG ThawedForN
    )
{
    PDEVICE_CONTEXT deviceCtx;
    PDRIVER_CONTEXT driverCtx;
    PUCHAR cfgBuf = NULL;
    OF_CONFIG_SECTOR hdr;
    OF_CONFIG_PARSE parse;
    OF_U32 seq;
    OF_U32 flags;
    OF_U32 n;
    NTSTATUS ts;

    PAGED_CODE();

    deviceCtx = OfFilterGetDeviceContext(Device);
    if (!deviceCtx->IsTargetVolume) {
        return STATUS_INVALID_DEVICE_STATE;
    }
    driverCtx = OfFilterGetDriverContext(WdfDeviceGetDriver(Device));
    if (driverCtx->OverlayBackend != (ULONG)OfOverlayFile ||
        !driverCtx->OverlayExtentsLoaded ||
        driverCtx->OverlayExtentCount == 0) {
        return STATUS_INVALID_DEVICE_STATE;
    }

    if (Mode == OF_MODE_FROZEN) {
        flags = 0;
        n = 0;
    } else if (Mode == OF_MODE_THAWED) {
        flags = OF_CONFIG_FLAG_THAW_NEXT;
        n = 0; /* sticky until tool clears */
    } else if (Mode == OF_MODE_THAWED_FOR_N) {
        if (ThawedForN == 0 || ThawedForN > 99) {
            return STATUS_INVALID_PARAMETER;
        }
        flags = OF_CONFIG_FLAG_THAW_NEXT;
        n = ThawedForN;
    } else {
        return STATUS_INVALID_PARAMETER;
    }

    cfgBuf = (PUCHAR)ExAllocatePool2(POOL_FLAG_NON_PAGED,
                                     OF_OVERLAY_CONFIG_SECTOR_SIZE,
                                     OF_POOL_TAG);
    if (cfgBuf == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    RtlZeroMemory(cfgBuf, OF_OVERLAY_CONFIG_SECTOR_SIZE);
    ts = OfOverlayConfigSectorRead(Device,
                                   driverCtx->OverlayExtents,
                                   driverCtx->OverlayExtentCount,
                                   cfgBuf);
    if (!NT_SUCCESS(ts)) {
        RtlZeroMemory(cfgBuf, OF_OVERLAY_CONFIG_SECTOR_SIZE);
        seq = 0;
    } else {
        parse = OfConfigParse(cfgBuf, OF_OVERLAY_CONFIG_SECTOR_SIZE, &hdr);
        seq = (parse == OfConfigParseValid) ? hdr.Sequence : 0;
    }
    seq = seq + 1u;
    if (seq == 0) { seq = 1; }
    OfConfigBuild(cfgBuf, OF_OVERLAY_CONFIG_SECTOR_SIZE, seq, flags, n);
    ts = OfOverlayConfigSectorWrite(Device,
                                    driverCtx->OverlayExtents,
                                    driverCtx->OverlayExtentCount,
                                    cfgBuf);
    if (!NT_SUCCESS(ts)) {
        DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
                   "OfFilter: next-boot-mode write failed 0x%08X\n", ts);
        ExFreePoolWithTag(cfgBuf, OF_POOL_TAG);
        return ts;
    }
    deviceCtx->CfgValid = 1;
    deviceCtx->ThawNext = (flags & OF_CONFIG_FLAG_THAW_NEXT) ? 1u : 0u;
    deviceCtx->ThawedForN = n;
    deviceCtx->CfgSeq = seq;
    ExFreePoolWithTag(cfgBuf, OF_POOL_TAG);
    DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
               "OfFilter: next-boot mode=%lu n=%lu thawNext=%lu seq=%lu\n",
               Mode, n, deviceCtx->ThawNext, seq);
    return STATUS_SUCCESS;
}

VOID
OfFilterSignalOverlayEvent(
    _In_ PDEVICE_CONTEXT DeviceCtx,
    _In_ ULONG Event
    )
{
    WDFREQUEST waiter = NULL;
    KIRQL irql;
    POF_OVERLAY_EVENT_V1 ev;
    size_t outLen = 0;
    NTSTATUS st;
    ULONG used;
    ULONG total;

    if (DeviceCtx == NULL || Event == OF_OVERLAY_EVENT_NONE) {
        return;
    }

    KeAcquireSpinLock(&DeviceCtx->OverlayEventLock, &irql);
    waiter = DeviceCtx->OverlayEventWaiter;
    DeviceCtx->OverlayEventWaiter = NULL;
    KeReleaseSpinLock(&DeviceCtx->OverlayEventLock, irql);

    if (waiter == NULL) {
        return;
    }

    st = WdfRequestUnmarkCancelable(waiter);
    if (st == STATUS_CANCELLED) {
        /* Cancel callback owns completion. */
        return;
    }

    used = DeviceCtx->BlockMapReady ? DeviceCtx->BlockMap.SlotsUsed : 0;
    total = DeviceCtx->BlockMapReady ? DeviceCtx->BlockMap.SlotCount : 0;
    st = WdfRequestRetrieveOutputBuffer(waiter, sizeof(*ev), (PVOID *)&ev, &outLen);
    if (!NT_SUCCESS(st) || ev == NULL) {
        WdfRequestComplete(waiter, st);
        return;
    }
    RtlZeroMemory(ev, sizeof(*ev));
    ev->Event = Event;
    ev->SlotsUsed = used;
    ev->SlotCount = total;
    WdfRequestCompleteWithInformation(waiter, STATUS_SUCCESS, sizeof(*ev));
}

VOID
OfFilterEvtIoRead(
    _In_ WDFQUEUE Queue,
    _In_ WDFREQUEST Request,
    _In_ size_t Length
    )
{
    WDFDEVICE device;
    PDEVICE_CONTEXT deviceCtx;
    WDF_REQUEST_PARAMETERS params;
    device = WdfIoQueueGetDevice(Queue);
    deviceCtx = OfFilterGetDeviceContext(device);

    WDF_REQUEST_PARAMETERS_INIT(&params);
    WdfRequestGetParameters(Request, &params);

    TraceEvents(TRACE_LEVEL_VERBOSE, TRACE_IO,
                "READ Cow=%!BOOLEAN! Offset=%!I64d! Length=%Iu",
                deviceCtx->CowEnabled,
                params.Parameters.Read.DeviceOffset,
                Length);

    if (deviceCtx->CowEnabled) {
        if (OfFilterTryBypassRead(device, Request, params.Parameters.Read.DeviceOffset, Length)) {
            return;
        }
        OfFilterDispatchFrozen(device, Request, FALSE);
        return;
    }
    if (deviceCtx->CowTornDown) {
        WdfRequestComplete(Request, STATUS_DEVICE_NOT_READY);
        return;
    }

    OfFilterForwardRequest(Request, WdfDeviceGetIoTarget(device));
}

VOID
OfFilterEvtIoWrite(
    _In_ WDFQUEUE Queue,
    _In_ WDFREQUEST Request,
    _In_ size_t Length
    )
{
    WDFDEVICE device;
    PDEVICE_CONTEXT deviceCtx;
    WDF_REQUEST_PARAMETERS params;
    device = WdfIoQueueGetDevice(Queue);
    deviceCtx = OfFilterGetDeviceContext(device);

    WDF_REQUEST_PARAMETERS_INIT(&params);
    WdfRequestGetParameters(Request, &params);

    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_IO,
                "WRITE Cow=%!BOOLEAN! Offset=%!I64d! Length=%Iu Used=%lu Bypass=%I64d",
                deviceCtx->CowEnabled,
                params.Parameters.Write.DeviceOffset,
                Length,
                deviceCtx->BlockMapReady ? OfBlockMapSlotsUsed(&deviceCtx->BlockMap) : 0,
                deviceCtx->BypassReads);

    if (deviceCtx->CowEnabled) {
        if (OfFilterTryPassthroughWrite(device, Request, params.Parameters.Write.DeviceOffset, Length)) {
            return;
        }
        OfFilterDispatchFrozen(device, Request, TRUE);
        return;
    }
    if (deviceCtx->CowTornDown) {
        WdfRequestComplete(Request, STATUS_DEVICE_NOT_READY);
        return;
    }

    OfFilterForwardRequest(Request, WdfDeviceGetIoTarget(device));
}
VOID
OfFilterEvtIoDeviceControl(
    _In_ WDFQUEUE Queue,
    _In_ WDFREQUEST Request,
    _In_ size_t OutputBufferLength,
    _In_ size_t InputBufferLength,
    _In_ ULONG IoControlCode
    )
{
    WDFDEVICE device;
    PDEVICE_CONTEXT deviceCtx;


    UNREFERENCED_PARAMETER(OutputBufferLength);   /* used only in DBG builds */

    device = WdfIoQueueGetDevice(Queue);
    deviceCtx = OfFilterGetDeviceContext(device);

#if DBG
    if (IoControlCode == IOCTL_OF_DEBUG_QUERY_COW) {
        POF_DEBUG_COW_STATE st = NULL;
        NTSTATUS qs = WdfRequestRetrieveOutputBuffer(Request, sizeof(*st), (PVOID *)&st, NULL);
        if (!NT_SUCCESS(qs) || st == NULL) {
            WdfRequestComplete(Request, !NT_SUCCESS(qs) ? qs : STATUS_BUFFER_TOO_SMALL);
            return;
        }
        /* Always answer on every attached volume. Forwarding when !IsTargetVolume
           made ofquery fail with ERROR_INVALID_FUNCTION after a bad VolumeUniqueId
           boot (refuse-freeze), which hid the PASSTHROUGH state from tools. */
        RtlZeroMemory(st, sizeof(*st));
        if (deviceCtx->IsTargetVolume) {
            st->CowEnabled = deviceCtx->CowEnabled;
            st->CowTornDown = deviceCtx->CowTornDown;
            st->SlotsUsed = deviceCtx->BlockMapReady ? OfBlockMapSlotsUsed(&deviceCtx->BlockMap) : 0;
            st->PendingWrites = (ULONG)deviceCtx->PendingWrites;
            st->BypassReads = deviceCtx->BypassReads;
            st->WorkerReads = deviceCtx->WorkerReads;
            st->OverlayFullCount = deviceCtx->OverlayFullCount;
        }
        if (OutputBufferLength >= sizeof(OF_DEBUG_COW_STATE_V2)) {
            POF_DEBUG_COW_STATE_V2 st2 = (POF_DEBUG_COW_STATE_V2)st;
            ULONG r;
            size_t v2Extra = sizeof(OF_DEBUG_COW_STATE_V2) - sizeof(*st);
            RtlZeroMemory((PUCHAR)st2 + sizeof(*st), v2Extra);
            st2->IsTargetVolume = deviceCtx->IsTargetVolume ? 1u : 0u;
            if (!deviceCtx->IsTargetVolume) {
                st2->RefuseReason = OF_REFUSE_NOT_TARGET;
            } else if (!deviceCtx->CowEnabled && !deviceCtx->CowTornDown) {
                st2->RefuseReason = deviceCtx->CowRefuseReason;
            } else {
                st2->RefuseReason = OF_REFUSE_NONE;
            }
            if (deviceCtx->IsTargetVolume) {
                st2->PassthroughRangeCount = deviceCtx->Passthrough.Count;
                st2->L2ReserveUsed = deviceCtx->L2ReserveUsed;
                st2->PassthroughWrites = deviceCtx->PassthroughWrites;
                st2->PassthroughReads = deviceCtx->PassthroughReads;
                for (r = 0; r < deviceCtx->Passthrough.Count && r < OF_MAX_PASSTHROUGH_RANGES; ++r) {
                    st2->PassthroughBytes += deviceCtx->Passthrough.Ranges[r].Length;
                }
            }
            if (OutputBufferLength >= sizeof(OF_DEBUG_COW_STATE_V3)) {
                POF_DEBUG_COW_STATE_V3 st3 = (POF_DEBUG_COW_STATE_V3)st;
                st3->InitStep = deviceCtx->CowInitStep;
                st3->InitStatus = (ULONG)deviceCtx->CowInitStatus;
                if (OutputBufferLength >= sizeof(OF_DEBUG_COW_STATE_V4)) {
                    POF_DEBUG_COW_STATE_V4 st4 = (POF_DEBUG_COW_STATE_V4)st;
                    st4->CfgValid = deviceCtx->CfgValid;
                    st4->ThawNext = deviceCtx->ThawNext;
                    st4->CfgSeq = deviceCtx->CfgSeq;
                    WdfRequestCompleteWithInformation(Request, STATUS_SUCCESS, sizeof(*st4));
                    return;
                }
                WdfRequestCompleteWithInformation(Request, STATUS_SUCCESS, sizeof(*st3));
                return;
            }
            WdfRequestCompleteWithInformation(Request, STATUS_SUCCESS, sizeof(*st2));
            return;
        }
        WdfRequestCompleteWithInformation(Request, STATUS_SUCCESS, sizeof(*st));
        return;
    }
    if (IoControlCode == IOCTL_OF_DEBUG_ENABLE_COW) {
        NTSTATUS es;
        if (!deviceCtx->IsTargetVolume) {
            OfFilterForwardRequest(Request, WdfDeviceGetIoTarget(device));
            return;
        }
        if (WdfRequestGetRequestorMode(Request) != UserMode && WdfRequestGetRequestorMode(Request) != KernelMode) {
            WdfRequestComplete(Request, STATUS_ACCESS_DENIED);
            return;
        }
        if (KeGetCurrentIrql() != PASSIVE_LEVEL) {
            WdfRequestComplete(Request, STATUS_INVALID_DEVICE_STATE);
            return;
        }
        if (deviceCtx->CowEnabled) {
            WdfRequestComplete(Request, STATUS_SUCCESS);
            return;
        }
        if (deviceCtx->CowTornDown || deviceCtx->BlockMapReady) {
            WdfRequestComplete(Request, STATUS_INVALID_DEVICE_STATE);
            return;
        }
        es = OfFilterInitializeCow(device);
        TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_IO,
                    "DEBUG runtime CoW enable %!STATUS!", es);
        DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
                   "OfFilter: DEBUG runtime CoW enable 0x%08X\n", es);
        WdfRequestComplete(Request, es);
        return;
    }
#endif
    if (IoControlCode == IOCTL_OF_SET_THAW_NEXT_BOOT ||
        IoControlCode == IOCTL_OF_SET_NEXT_BOOT_MODE) {
        PULONG inVal = NULL;
        POF_SET_NEXT_BOOT_MODE_IN modeIn = NULL;
        size_t inLen = 0;
        NTSTATUS ts;
        BOOLEAN wantSet;

        if (!deviceCtx->IsTargetVolume) {
            OfFilterForwardRequest(Request, WdfDeviceGetIoTarget(device));
            return;
        }
        if (KeGetCurrentIrql() != PASSIVE_LEVEL) {
            WdfRequestComplete(Request, STATUS_INVALID_DEVICE_STATE);
            return;
        }
        if (IoControlCode == IOCTL_OF_SET_NEXT_BOOT_MODE) {
            ts = WdfRequestRetrieveInputBuffer(Request, sizeof(*modeIn), (PVOID *)&modeIn, &inLen);
            if (!NT_SUCCESS(ts) || modeIn == NULL) {
                WdfRequestComplete(Request, !NT_SUCCESS(ts) ? ts : STATUS_BUFFER_TOO_SMALL);
                return;
            }
            ts = OfFilterApplyNextBootMode(device, modeIn->Mode, modeIn->ThawedForN);
            WdfRequestComplete(Request, ts);
            return;
        }
        ts = WdfRequestRetrieveInputBuffer(Request, sizeof(ULONG), (PVOID *)&inVal, &inLen);
        if (!NT_SUCCESS(ts) || inVal == NULL) {
            WdfRequestComplete(Request, !NT_SUCCESS(ts) ? ts : STATUS_BUFFER_TOO_SMALL);
            return;
        }
        wantSet = (*inVal != 0) ? TRUE : FALSE;
        ts = OfFilterApplyThawNextBoot(device, wantSet);
        if (NT_SUCCESS(ts)) {
            WdfRequestCompleteWithInformation(Request, STATUS_SUCCESS, 0);
        } else {
            WdfRequestComplete(Request, ts);
        }
        return;
    }
    if (IoControlCode == IOCTL_OF_SET_PASSTHROUGH_RANGES) {
        PVOID in = NULL;
        size_t inLength = 0;
        NTSTATUS ps;
        if (!deviceCtx->IsTargetVolume) {
            OfFilterForwardRequest(Request, WdfDeviceGetIoTarget(device));
            return;
        }
        if (KeGetCurrentIrql() != PASSIVE_LEVEL) {
            WdfRequestComplete(Request, STATUS_INVALID_DEVICE_STATE);
            return;
        }
        if (InputBufferLength != 0) {
            ps = WdfRequestRetrieveInputBuffer(Request, sizeof(OF_RANGE), &in, &inLength);
            if (!NT_SUCCESS(ps) || in == NULL || (inLength % sizeof(OF_RANGE)) != 0 ||
                inLength / sizeof(OF_RANGE) > OF_MAX_PASSTHROUGH_RANGES) {
                WdfRequestComplete(Request, !NT_SUCCESS(ps) ? ps : STATUS_INVALID_PARAMETER);
                return;
            }
        }
        ps = OfFilterSetPassthroughRanges(device, (const OF_RANGE *)in,
                                          (ULONG)(inLength / sizeof(OF_RANGE)));
        WdfRequestComplete(Request, ps);
        return;
    }
    if ((deviceCtx->CowEnabled || deviceCtx->CowTornDown) &&
        IoControlCode == IOCTL_STORAGE_MANAGE_DATA_SET_ATTRIBUTES) {
        PDEVICE_MANAGE_DATA_SET_ATTRIBUTES dsm = NULL;
        size_t dsmLength = 0;
        NTSTATUS status;
        DEVICE_DATA_MANAGEMENT_SET_ACTION action;

        status = WdfRequestRetrieveInputBuffer(Request,
                                               sizeof(DEVICE_MANAGE_DATA_SET_ATTRIBUTES),
                                               (PVOID *)&dsm,
                                               &dsmLength);
        if (!NT_SUCCESS(status) || dsm == NULL) {
            // Unparseable: never let it reach the base volume.
            WdfRequestComplete(Request, STATUS_INVALID_PARAMETER);
            return;
        }

        action = dsm->Action & ~DeviceDsmActionFlag_NonDestructive;
        if (action == DeviceDsmAction_Trim) {
            // Swallow TRIM: success, no action. Passing it down would
            // discard base blocks the frozen image still needs.
            TraceEvents(TRACE_LEVEL_VERBOSE, TRACE_IO, "TRIM swallowed (frozen)");
            WdfRequestComplete(Request, STATUS_SUCCESS);
            return;
        }
        if ((dsm->Action & DeviceDsmActionFlag_NonDestructive) == 0) {
            // Other destructive DSM actions (offload write, etc.) would
            // bypass the overlay. Refuse so callers fall back to normal I/O.
            TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_IO,
                        "DSM action 0x%lx refused (frozen)", dsm->Action);
            WdfRequestComplete(Request, STATUS_NOT_SUPPORTED);
            return;
        }
        // Non-destructive DSM (queries): pass through.
    }

    OfFilterForwardRequest(Request, WdfDeviceGetIoTarget(device));
}


VOID
OfFilterEvtIoDefault(
    _In_ WDFQUEUE Queue,
    _In_ WDFREQUEST Request
    )
{
    WDFDEVICE device = WdfIoQueueGetDevice(Queue);
    PDEVICE_CONTEXT deviceCtx = OfFilterGetDeviceContext(device);
    WDF_REQUEST_PARAMETERS params;

    WDF_REQUEST_PARAMETERS_INIT(&params);
    WdfRequestGetParameters(Request, &params);

    //
    // While frozen, route flushes through the worker queue so they are
    // ordered behind every write queued before them.
    //
    if (deviceCtx->CowEnabled && params.Type == WdfRequestTypeFlushBuffers) {
        if (OfFilterQueueToWorker(deviceCtx, Request, FALSE)) {
            return;
        }
    }

    OfFilterForwardRequest(Request, WdfDeviceGetIoTarget(device));
}

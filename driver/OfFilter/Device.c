/*++

Module Name:

    Device.c

Abstract:

    KMDF volume upper-filter device creation, unique-id matching, CoW init.

--*/

#include "Device.h"
#include "Io.h"
#include "OfConfig.h"
#include "device.tmh"

#ifdef ALLOC_PRAGMA
#pragma alloc_text(PAGE, OfFilterCreateDevice)
#pragma alloc_text(PAGE, OfFilterEvtDevicePrepareHardware)
#pragma alloc_text(PAGE, OfFilterEvtDeviceReleaseHardware)
#pragma alloc_text(PAGE, OfFilterInitializeCow)
#pragma alloc_text(PAGE, OfFilterTeardownCow)
#pragma alloc_text(PAGE, OfFilterSetPassthroughRanges)
#pragma alloc_text(PAGE, OfFilterDeferredCowInit)
#endif

/* Debugger anchors + Phase 4 control-device target (sticky thaw has no CowEnabled). */
PDEVICE_CONTEXT g_OfTargetDeviceContext = NULL;
WDFDEVICE g_OfTargetDevice = NULL;
WDFQUEUE g_OfWorkQueue = NULL;

static
NTSTATUS
OfFilterQueryVolumeUniqueId(
    _In_ WDFDEVICE Device,
    _Out_writes_bytes_(OutputLength) PUCHAR Output,
    _In_ ULONG OutputLength,
    _Out_ PUSHORT UniqueIdLength
    );

static
NTSTATUS
OfFilterQueryVolumeSize(
    _In_ WDFDEVICE Device,
    _Out_ OF_U64 *SizeBytes
    );

//
// Block-map allocator. L2 tables (the only allocation made while frozen,
// from the worker under CowLock) come from the preallocated reserve first,
// then from non-paged pool (ExAllocatePool2 never waits; on failure the
// write fails with STATUS_INSUFFICIENT_RESOURCES). Non-paged: called from
// the worker. Reserve state is only touched under CowLock or at init and
// teardown when no I/O can reach the map.
//
static
PVOID
OfFilterBlockMapAlloc(
    _In_ OF_SIZE Size,
    _In_opt_ PVOID AllocContext
    )
{
    PDEVICE_CONTEXT deviceCtx = (PDEVICE_CONTEXT)AllocContext;

    if (deviceCtx != NULL && Size == OF_L2_BYTES &&
        deviceCtx->L2Reserve != NULL && deviceCtx->L2ReserveTop > 0) {
        OF_U32 index = deviceCtx->L2ReserveFree[--deviceCtx->L2ReserveTop];
        deviceCtx->L2ReserveUsed += 1;
        return deviceCtx->L2Reserve + ((SIZE_T)index * OF_L2_BYTES);
    }
    return ExAllocatePool2(POOL_FLAG_NON_PAGED, Size, OF_POOL_TAG);
}

static
VOID
OfFilterBlockMapFree(
    _In_opt_ PVOID Pointer,
    _In_opt_ PVOID AllocContext
    )
{
    PDEVICE_CONTEXT deviceCtx = (PDEVICE_CONTEXT)AllocContext;

    if (Pointer == NULL) {
        return;
    }
    if (deviceCtx != NULL && deviceCtx->L2Reserve != NULL &&
        (PUCHAR)Pointer >= deviceCtx->L2Reserve &&
        (PUCHAR)Pointer < deviceCtx->L2Reserve + ((SIZE_T)OF_L2_RESERVE_COUNT * OF_L2_BYTES)) {
        OF_U32 index = (OF_U32)(((PUCHAR)Pointer - deviceCtx->L2Reserve) / OF_L2_BYTES);
        if (deviceCtx->L2ReserveTop < OF_L2_RESERVE_COUNT) {
            deviceCtx->L2ReserveFree[deviceCtx->L2ReserveTop++] = index;
        }
        if (deviceCtx->L2ReserveUsed > 0) {
            deviceCtx->L2ReserveUsed -= 1;
        }
        return;
    }
    ExFreePoolWithTag(Pointer, OF_POOL_TAG);
}

NTSTATUS
OfFilterSetPassthroughRanges(
    _In_ WDFDEVICE Device,
    _In_reads_opt_(Count) const OF_RANGE *Raw,
    _In_ ULONG Count
    )
{
    PDEVICE_CONTEXT deviceCtx;
    OF_U64 volumeSize = 0;
    OF_STATUS rs;
    NTSTATUS status;

    PAGED_CODE();

    deviceCtx = OfFilterGetDeviceContext(Device);
    if (Count > OF_MAX_PASSTHROUGH_RANGES || (Count != 0 && Raw == NULL)) {
        return STATUS_INVALID_PARAMETER;
    }

    status = OfFilterQueryVolumeSize(Device, &volumeSize);
    if (!NT_SUCCESS(status) || volumeSize == 0) {
        return !NT_SUCCESS(status) ? status : STATUS_INVALID_DEVICE_STATE;
    }

    WdfWaitLockAcquire(deviceCtx->CowLock, NULL);
    if (deviceCtx->CowEnabled || deviceCtx->CowTornDown) {
        status = STATUS_INVALID_DEVICE_STATE;
    } else {
        rs = OfRangeListBuild(&deviceCtx->Passthrough, Raw, Count, volumeSize);
        if (rs == OfStatusOk) {
            deviceCtx->PassthroughSetByIoctl = TRUE;
            status = STATUS_SUCCESS;
        } else {
            deviceCtx->Passthrough.Count = 0;
            status = (rs == OfStatusFull) ? STATUS_INSUFFICIENT_RESOURCES : STATUS_INVALID_PARAMETER;
        }
    }
    WdfWaitLockRelease(deviceCtx->CowLock);

    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DEVICE,
                "Passthrough ranges set by IOCTL raw=%lu normalized=%lu %!STATUS!",
                Count, deviceCtx->Passthrough.Count, status);
    DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
               "OfFilter: passthrough ranges set raw=%lu normalized=%lu 0x%08X\n",
               Count, deviceCtx->Passthrough.Count, status);
    return status;
}

NTSTATUS
OfFilterCreateDevice(
    _In_ WDFDRIVER Driver,
    _Inout_ PWDFDEVICE_INIT DeviceInit
    )
{
    WDF_OBJECT_ATTRIBUTES attributes;
    WDF_PNPPOWER_EVENT_CALLBACKS pnpPowerCallbacks;
    WDFDEVICE device;
    PDEVICE_CONTEXT deviceCtx;
    NTSTATUS status;

    PAGED_CODE();
    UNREFERENCED_PARAMETER(Driver);

    WdfFdoInitSetFilter(DeviceInit);

    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnpPowerCallbacks);
    pnpPowerCallbacks.EvtDevicePrepareHardware = OfFilterEvtDevicePrepareHardware;
    pnpPowerCallbacks.EvtDeviceReleaseHardware = OfFilterEvtDeviceReleaseHardware;
    WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &pnpPowerCallbacks);

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, DEVICE_CONTEXT);

    status = WdfDeviceCreate(&DeviceInit, &attributes, &device);
    if (!NT_SUCCESS(status)) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE,
                    "WdfDeviceCreate failed %!STATUS!", status);
        return status;
    }

    deviceCtx = OfFilterGetDeviceContext(device);
    RtlZeroMemory(deviceCtx, sizeof(*deviceCtx));
    KeInitializeSpinLock(&deviceCtx->WorkLock);
    KeInitializeSpinLock(&deviceCtx->OverlayEventLock);
    KeInitializeEvent(&deviceCtx->WorkEvent, SynchronizationEvent, FALSE);
    deviceCtx->WorkerStop = TRUE;   /* no worker until CoW initializes */

    status = WdfWaitLockCreate(WDF_NO_OBJECT_ATTRIBUTES, &deviceCtx->CowLock);
    if (!NT_SUCCESS(status)) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE,
                    "WdfWaitLockCreate failed %!STATUS!", status);
        return status;
    }

    status = OfFilterConfigureIoQueues(device);
    if (!NT_SUCCESS(status)) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE,
                    "OfFilterConfigureIoQueues failed %!STATUS!", status);
        return status;
    }

    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DEVICE,
                "Filter device created %p", device);
    return STATUS_SUCCESS;
}

NTSTATUS
OfFilterEvtDevicePrepareHardware(
    _In_ WDFDEVICE Device,
    _In_ WDFCMRESLIST ResourcesRaw,
    _In_ WDFCMRESLIST ResourcesTranslated
    )
{
    PDEVICE_CONTEXT deviceCtx;
    PDRIVER_CONTEXT driverCtx;
    UCHAR uniqueId[OF_UNIQUE_ID_MAX_BYTES];
    USHORT uniqueIdLength = 0;
    NTSTATUS status;

    PAGED_CODE();
    UNREFERENCED_PARAMETER(ResourcesRaw);
    UNREFERENCED_PARAMETER(ResourcesTranslated);

    deviceCtx = OfFilterGetDeviceContext(Device);
    driverCtx = OfFilterGetDriverContext(WdfDeviceGetDriver(Device));

    status = OfFilterQueryVolumeUniqueId(Device,
                                         uniqueId,
                                         sizeof(uniqueId),
                                         &uniqueIdLength);
    if (!NT_SUCCESS(status)) {
        TraceEvents(TRACE_LEVEL_WARNING, TRACE_DEVICE,
                    "IOCTL_MOUNTDEV_QUERY_UNIQUE_ID failed %!STATUS! - passthrough without match",
                    status);
        deviceCtx->UniqueIdValid = FALSE;
        deviceCtx->IsTargetVolume = FALSE;
        return STATUS_SUCCESS;
    }

    deviceCtx->UniqueIdValid = TRUE;
    deviceCtx->UniqueIdLength = uniqueIdLength;
    RtlCopyMemory(deviceCtx->UniqueId, uniqueId, uniqueIdLength);

    deviceCtx->IsTargetVolume = FALSE;
    if (driverCtx->UniqueIdConfigured &&
        driverCtx->UniqueIdLength == uniqueIdLength &&
        RtlCompareMemory(driverCtx->UniqueId, uniqueId, uniqueIdLength) == uniqueIdLength) {
        deviceCtx->IsTargetVolume = TRUE;
        g_OfTargetDevice = Device;
        g_OfTargetDeviceContext = deviceCtx;
    }

    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DEVICE,
                "Volume UniqueIdLength=%hu IsTarget=%!BOOLEAN! EnableCow=%!BOOLEAN!",
                uniqueIdLength,
                deviceCtx->IsTargetVolume,
                driverCtx->EnableCow);

    /*
     * Exp C: do NOT InitializeCow here. A config-sector (or any volume) READ
     * during PrepareHardware can SUCCESS+zero under FVE → false Absent →
     * freeze → NTFS 0xED. Defer to a system thread that waits until the
     * volume boot sector shows an NTFS/FAT signature, then inits.
     */
    if (deviceCtx->IsTargetVolume &&
        driverCtx->EnableCow &&
        !deviceCtx->CowInitDeferredStarted) {
        HANDLE threadHandle = NULL;
        status = PsCreateSystemThread(&threadHandle,
                                      THREAD_ALL_ACCESS,
                                      NULL,
                                      NULL,
                                      NULL,
                                      OfFilterDeferredCowInit,
                                      Device);
        if (!NT_SUCCESS(status)) {
            DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
                       "OfFilter: deferred CoW thread create failed 0x%08X (passthrough)\n",
                       status);
            deviceCtx->CowRefuseReason = OF_REFUSE_CONFIG;
            deviceCtx->CowInitStatus = status;
        } else {
            status = ObReferenceObjectByHandle(threadHandle,
                                               THREAD_ALL_ACCESS,
                                               *PsThreadType,
                                               KernelMode,
                                               (PVOID *)&deviceCtx->CowInitThread,
                                               NULL);
            ZwClose(threadHandle);
            if (!NT_SUCCESS(status)) {
                deviceCtx->CowInitThread = NULL;
                DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
                           "OfFilter: deferred CoW thread ref failed 0x%08X\n", status);
                deviceCtx->CowRefuseReason = OF_REFUSE_CONFIG;
                deviceCtx->CowInitStatus = status;
            } else {
                deviceCtx->CowInitDeferredStarted = TRUE;
                DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
                           "OfFilter: deferred CoW init thread started\n");
            }
        }
    }

    return STATUS_SUCCESS;
}

VOID
OfFilterDeferredCowInit(
    _In_ PVOID StartContext
    )
{
    WDFDEVICE device = (WDFDEVICE)StartContext;
    PDEVICE_CONTEXT deviceCtx;
    PDRIVER_CONTEXT driverCtx;
    NTSTATUS status;
    ULONG i;
    LARGE_INTEGER interval;
    BOOLEAN unlocked = FALSE;

    PAGED_CODE();

    deviceCtx = OfFilterGetDeviceContext(device);
    driverCtx = OfFilterGetDriverContext(WdfDeviceGetDriver(device));

    /* ~15s of 50ms polls for volume FS signature (FVE unlocked). */
    interval.QuadPart = -50 * 10000LL; /* 50ms relative */
    for (i = 0; i < 300; ++i) {
        if (deviceCtx->CowInitCancel) {
            PsTerminateSystemThread(STATUS_SUCCESS);
            return;
        }
        if (OfOverlayProbeVolumeUnlocked(device)) {
            unlocked = TRUE;
            break;
        }
        KeDelayExecutionThread(KernelMode, FALSE, &interval);
    }

    if (!unlocked) {
        deviceCtx->CowRefuseReason = OF_REFUSE_CONFIG;
        deviceCtx->CowEnabled = FALSE;
        deviceCtx->CowInitStep = (ULONG)OfCowInitDone;
        deviceCtx->CowInitStatus = STATUS_DEVICE_BUSY;
        DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
                   "OfFilter: deferred CoW: volume never unlocked (passthrough, boot-ok)\n");
        PsTerminateSystemThread(STATUS_SUCCESS);
        return;
    }

    DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
               "OfFilter: deferred CoW: volume unlocked after %lu polls; InitializeCow\n",
               i);

    if (!deviceCtx->CowInitCancel &&
        deviceCtx->IsTargetVolume &&
        driverCtx->EnableCow &&
        !deviceCtx->CowEnabled &&
        !deviceCtx->CowTornDown) {
        status = OfFilterInitializeCow(device);
        if (!NT_SUCCESS(status)) {
            deviceCtx->CowEnabled = FALSE;
            if (deviceCtx->CowRefuseReason == OF_REFUSE_NONE) {
                deviceCtx->CowRefuseReason = OF_REFUSE_CONFIG;
            }
            DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
                       "OfFilter: deferred InitializeCow failed 0x%08X (passthrough)\n",
                       status);
        }
    }

    PsTerminateSystemThread(STATUS_SUCCESS);
}

NTSTATUS
OfFilterEvtDeviceReleaseHardware(
    _In_ WDFDEVICE Device,
    _In_ WDFCMRESLIST ResourcesTranslated
    )
{
    PAGED_CODE();
    UNREFERENCED_PARAMETER(ResourcesTranslated);

    if (g_OfTargetDevice == Device) {
        g_OfTargetDevice = NULL;
        g_OfTargetDeviceContext = NULL;
    }
    OfFilterTeardownCow(Device);
    return STATUS_SUCCESS;
}

NTSTATUS
OfFilterInitializeCow(
    _In_ WDFDEVICE Device
    )
{
    PDEVICE_CONTEXT deviceCtx;
    PDRIVER_CONTEXT driverCtx;
    OF_U64 volumeSize = 0;
    OF_U32 maxBlocks;
    OF_STATUS mapStatus;
    NTSTATUS status;

    PAGED_CODE();

    deviceCtx = OfFilterGetDeviceContext(Device);
    driverCtx = OfFilterGetDriverContext(WdfDeviceGetDriver(Device));

    deviceCtx->CowInitStep = (ULONG)OfCowInitVolumeSize;
    deviceCtx->CowInitStatus = STATUS_SUCCESS;

    status = OfFilterQueryVolumeSize(Device, &volumeSize);
    if (!NT_SUCCESS(status) || volumeSize == 0) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE,
                    "Volume size query failed %!STATUS!", status);
        status = !NT_SUCCESS(status) ? status : STATUS_INVALID_DEVICE_STATE;
        deviceCtx->CowInitStatus = status;
        deviceCtx->CowRefuseReason = OF_REFUSE_CONFIG;
        DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
                   "OfFilter: CoW init fail step=%lu status=0x%08X (volume size)\n",
                   deviceCtx->CowInitStep, status);
        return status;
    }

    deviceCtx->VolumeSizeBytes = volumeSize;
    maxBlocks = (OF_U32)((volumeSize + OF_BLOCK_SIZE - 1) / OF_BLOCK_SIZE);

    //
    // Passthrough ranges: the IOCTL list (runtime path) wins; otherwise
    // normalize the registry list now that the volume size is known. The
    // list is fixed from here until teardown.
    //
    deviceCtx->CowInitStep = (ULONG)OfCowInitPassthrough;
    if (!deviceCtx->PassthroughSetByIoctl) {
        OF_STATUS rs = OfRangeListBuild(&deviceCtx->Passthrough,
                                        driverCtx->PassthroughRaw,
                                        driverCtx->PassthroughRawCount,
                                        volumeSize);
        if (rs != OfStatusOk) {
            deviceCtx->Passthrough.Count = 0;
            TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE,
                        "PassthroughRanges rejected (%d) - none in effect", (int)rs);
            DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
                       "OfFilter: CoW init passthrough rejected (%d)\n", (int)rs);
        }
    }
    DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
               "OfFilter: CoW init passthrough ranges=%lu (ioctl=%u)\n",
               deviceCtx->Passthrough.Count, (ULONG)deviceCtx->PassthroughSetByIoctl);

    deviceCtx->CowInitStep = (ULONG)OfCowInitL2Reserve;
    deviceCtx->L2Reserve = (PUCHAR)ExAllocatePool2(POOL_FLAG_NON_PAGED,
                                                   (SIZE_T)OF_L2_RESERVE_COUNT * OF_L2_BYTES,
                                                   OF_POOL_TAG);
    if (deviceCtx->L2Reserve != NULL) {
        OF_U32 r;
        for (r = 0; r < OF_L2_RESERVE_COUNT; ++r) {
            deviceCtx->L2ReserveFree[r] = OF_L2_RESERVE_COUNT - 1u - r;
        }
        deviceCtx->L2ReserveTop = OF_L2_RESERVE_COUNT;
        deviceCtx->L2ReserveUsed = 0;
    } else {
        DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
                   "OfFilter: CoW init L2Reserve alloc failed (continuing without reserve)\n");
    }

    //
    // File backend: validate extents + volume unique ID and derive SlotCount
    // before sizing the block map. Invalid config => passthrough (refuse freeze).
    //
    if (driverCtx->OverlayBackend == (ULONG)OfOverlayFile) {
        OF_U32 derived;
        OF_STATUS vs;

        deviceCtx->CowInitStep = (ULONG)OfCowInitFileConfig;
        if (!driverCtx->OverlayExtentsLoaded ||
            !driverCtx->UniqueIdConfigured ||
            !OfOverlayUniqueIdEqual(driverCtx->UniqueId,
                                    driverCtx->UniqueIdLength,
                                    deviceCtx->UniqueId,
                                    deviceCtx->UniqueIdLength)) {
            TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE,
                        "File overlay rejected (extents/unique id) - passthrough");
            DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
                       "OfFilter: CoW init fail step=%lu status=0x%08X (file config extents=%u uidCfg=%u)\n",
                       deviceCtx->CowInitStep,
                       STATUS_DEVICE_CONFIGURATION_ERROR,
                       (ULONG)driverCtx->OverlayExtentsLoaded,
                       (ULONG)driverCtx->UniqueIdConfigured);
            deviceCtx->CowInitStatus = STATUS_DEVICE_CONFIGURATION_ERROR;
            deviceCtx->CowRefuseReason = OF_REFUSE_CONFIG;
            return STATUS_DEVICE_CONFIGURATION_ERROR;
        }

        derived = OfOverlayComputeSlotCount(driverCtx->OverlayFileSize);
        if (derived == 0) {
            deviceCtx->CowInitStatus = STATUS_DEVICE_CONFIGURATION_ERROR;
            deviceCtx->CowRefuseReason = OF_REFUSE_CONFIG;
            DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
                       "OfFilter: CoW init fail step=%lu status=0x%08X (derived SlotCount=0 fileSize=%I64u)\n",
                       deviceCtx->CowInitStep,
                       STATUS_DEVICE_CONFIGURATION_ERROR,
                       driverCtx->OverlayFileSize);
            return STATUS_DEVICE_CONFIGURATION_ERROR;
        }

        deviceCtx->CowInitStep = (ULONG)OfCowInitValidateExtents;
        vs = OfOverlayValidateExtents(driverCtx->OverlayExtents,
                                      driverCtx->OverlayExtentCount,
                                      driverCtx->OverlayFileSize,
                                      derived,
                                      volumeSize);
        if (vs != OfStatusOk) {
            TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE,
                        "OverlayExtents validate failed (%d) - passthrough", (int)vs);
            deviceCtx->CowInitStatus = STATUS_DEVICE_CONFIGURATION_ERROR;
            deviceCtx->CowRefuseReason = OF_REFUSE_CONFIG;
            DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
                       "OfFilter: CoW init fail step=%lu status=0x%08X (ValidateExtents=%d fileSize=%I64u slots=%lu extents=%lu)\n",
                       deviceCtx->CowInitStep,
                       STATUS_DEVICE_CONFIGURATION_ERROR,
                       (int)vs,
                       driverCtx->OverlayFileSize,
                       derived,
                       driverCtx->OverlayExtentCount);
            return STATUS_DEVICE_CONFIGURATION_ERROR;
        }

        driverCtx->SlotCount = derived;

        if (!deviceCtx->PassthroughSetByIoctl) {
            /* Pool: OF_RANGE[1024] is 16 KB - must not live on D0Entry stack. */
            OF_RANGE *combined;
            OF_U32 combinedCount = 0;
            OF_U32 i;
            combined = (OF_RANGE *)ExAllocatePool2(POOL_FLAG_NON_PAGED,
                                                   sizeof(OF_RANGE) * OF_MAX_PASSTHROUGH_RANGES,
                                                   OF_POOL_TAG);
            if (combined == NULL) {
                deviceCtx->Passthrough.Count = 0;
                TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE,
                            "passthrough+overlay combined alloc failed");
                DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
                           "OfFilter: CoW init combined[] alloc failed\n");
            } else {
                for (i = 0; i < driverCtx->PassthroughRawCount && combinedCount < OF_MAX_PASSTHROUGH_RANGES; ++i) {
                    combined[combinedCount++] = driverCtx->PassthroughRaw[i];
                }
                for (i = 0; i < driverCtx->OverlayExtentCount && combinedCount < OF_MAX_PASSTHROUGH_RANGES; ++i) {
                    combined[combinedCount].Offset = driverCtx->OverlayExtents[i].VolumeOffset;
                    combined[combinedCount].Length = driverCtx->OverlayExtents[i].Length;
                    combinedCount++;
                }
                {
                    OF_STATUS rs = OfRangeListBuild(&deviceCtx->Passthrough,
                                                    combined,
                                                    combinedCount,
                                                    volumeSize);
                    if (rs != OfStatusOk) {
                        deviceCtx->Passthrough.Count = 0;
                        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE,
                                    "passthrough+overlay ranges rejected (%d)", (int)rs);
                        DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
                                   "OfFilter: CoW init passthrough+overlay rejected (%d)\n", (int)rs);
                    } else {
                        TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DEVICE,
                                    "Passthrough includes overlay extents; ranges=%lu",
                                    deviceCtx->Passthrough.Count);
                    }
                }
                ExFreePoolWithTag(combined, OF_POOL_TAG);
            }
        }
    }

    //
    // File-backend config sector: after extents are validated and merged into
    // passthrough, read thaw-on-next-boot BEFORE allocating the block map.
    // ThawRequested => SUCCESS with CowEnabled left FALSE (flag stays set).
    // Corrupt / read-fail => refuse freeze, still SUCCESS (boot-safe).
    //
    if (driverCtx->OverlayBackend == (ULONG)OfOverlayFile) {
        PUCHAR cfgBuf;
        NTSTATUS cfgSt;
        OF_U32 refuse = OF_REFUSE_NONE;
        OF_U32 cfgValid = 0;
        OF_U32 thawNext = 0;
        OF_U32 cfgSeq = 0;
        OF_U32 thawedForN = 0;
        BOOLEAN shouldFreeze;

        deviceCtx->CowInitStep = (ULONG)OfCowInitConfigSector;
        cfgBuf = (PUCHAR)ExAllocatePool2(POOL_FLAG_NON_PAGED,
                                         OF_OVERLAY_CONFIG_SECTOR_SIZE,
                                         OF_POOL_TAG);
        if (cfgBuf == NULL) {
            deviceCtx->CowRefuseReason = OF_REFUSE_CONFIG;
            deviceCtx->CowEnabled = FALSE;
            deviceCtx->CowInitStep = (ULONG)OfCowInitDone;
            deviceCtx->CowInitStatus = STATUS_INSUFFICIENT_RESOURCES;
            DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
                       "OfFilter: CoW init cfgBuf alloc failed (passthrough, boot-ok)\n");
            return STATUS_SUCCESS;
        }
        RtlZeroMemory(cfgBuf, OF_OVERLAY_CONFIG_SECTOR_SIZE);
        cfgSt = OfOverlayConfigSectorRead(Device,
                                          driverCtx->OverlayExtents,
                                          driverCtx->OverlayExtentCount,
                                          cfgBuf);
        shouldFreeze = OfConfigShouldFreeze(TRUE,
                                            TRUE,
                                            NT_SUCCESS(cfgSt) ? TRUE : FALSE,
                                            cfgBuf,
                                            OF_OVERLAY_CONFIG_SECTOR_SIZE,
                                            &refuse,
                                            &cfgValid,
                                            &thawNext,
                                            &cfgSeq,
                                            &thawedForN);
        deviceCtx->CfgValid = cfgValid;
        deviceCtx->ThawNext = thawNext;
        deviceCtx->CfgSeq = cfgSeq;
        deviceCtx->ThawedForN = thawedForN;

        /* ThawedForN: consume one boot and rewrite so N==0 clears thaw for next boot. */
        if (!shouldFreeze &&
            refuse == OF_REFUSE_THAW_REQUESTED &&
            thawedForN > 0 &&
            NT_SUCCESS(cfgSt)) {
            OF_U32 outFlags = 0;
            OF_U32 outN = 0;
            OF_U32 outSeq = 0;
            OF_U32 oldFlags = thawNext ? OF_CONFIG_FLAG_THAW_NEXT : 0u;
            if (OfConfigConsumeThawedForN(oldFlags, thawedForN, cfgSeq,
                                          &outFlags, &outN, &outSeq)) {
                OfConfigBuild(cfgBuf, OF_OVERLAY_CONFIG_SECTOR_SIZE, outSeq, outFlags, outN);
                cfgSt = OfOverlayConfigSectorWrite(Device,
                                                   driverCtx->OverlayExtents,
                                                   driverCtx->OverlayExtentCount,
                                                   cfgBuf);
                if (NT_SUCCESS(cfgSt)) {
                    deviceCtx->ThawNext = (outFlags & OF_CONFIG_FLAG_THAW_NEXT) ? 1u : 0u;
                    deviceCtx->ThawedForN = outN;
                    deviceCtx->CfgSeq = outSeq;
                    DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
                               "OfFilter: ThawedForN consume -> n=%lu thawNext=%lu seq=%lu\n",
                               outN, deviceCtx->ThawNext, outSeq);
                } else {
                    DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
                               "OfFilter: ThawedForN rewrite failed 0x%08X (still thawed this boot)\n",
                               cfgSt);
                }
            }
        }

        ExFreePoolWithTag(cfgBuf, OF_POOL_TAG);
        cfgBuf = NULL;

        if (!shouldFreeze) {
            deviceCtx->CowRefuseReason = refuse;
            deviceCtx->CowEnabled = FALSE;
            deviceCtx->CowInitStep = (ULONG)OfCowInitDone;
            if (refuse == OF_REFUSE_THAW_REQUESTED) {
                deviceCtx->CowInitStatus = STATUS_SUCCESS;
            } else if (!NT_SUCCESS(cfgSt)) {
                deviceCtx->CowInitStatus = cfgSt;
            } else {
                deviceCtx->CowInitStatus = STATUS_DEVICE_CONFIGURATION_ERROR;
            }
            DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
                       "OfFilter: CoW init config refuse=%lu cfgValid=%lu thawNext=%lu n=%lu seq=%lu read=0x%08X initStatus=0x%08X (passthrough, boot-ok)\n",
                       refuse, cfgValid, deviceCtx->ThawNext, deviceCtx->ThawedForN, deviceCtx->CfgSeq, cfgSt, deviceCtx->CowInitStatus);
            TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DEVICE,
                        "Config sector refuse=%lu thawNext=%lu n=%lu seq=%lu initStatus=%!STATUS! (passthrough)",
                        refuse, deviceCtx->ThawNext, deviceCtx->ThawedForN, deviceCtx->CfgSeq, deviceCtx->CowInitStatus);
            return STATUS_SUCCESS;
        }
    }

    deviceCtx->CowInitStep = (ULONG)OfCowInitBlockMap;
    mapStatus = OfBlockMapInitialize(&deviceCtx->BlockMap,
                                     maxBlocks,
                                     driverCtx->SlotCount,
                                     OfFilterBlockMapAlloc,
                                     OfFilterBlockMapFree,
                                     deviceCtx);
    if (mapStatus != OfStatusOk) {
        deviceCtx->CowInitStatus = STATUS_INSUFFICIENT_RESOURCES;
        deviceCtx->CowRefuseReason = OF_REFUSE_CONFIG;
        DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
                   "OfFilter: CoW init fail step=%lu status=0x%08X (BlockMap maxBlocks=%lu slots=%lu mapStatus=%d)\n",
                   deviceCtx->CowInitStep,
                   STATUS_INSUFFICIENT_RESOURCES,
                   maxBlocks,
                   driverCtx->SlotCount,
                   (int)mapStatus);
        OfFilterTeardownCow(Device);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    deviceCtx->BlockMapReady = TRUE;

    deviceCtx->CowInitStep = (ULONG)OfCowInitScratch;
    deviceCtx->ScratchBlock = (PUCHAR)ExAllocatePool2(POOL_FLAG_NON_PAGED,
                                                      OF_BLOCK_SIZE,
                                                      OF_POOL_TAG);
    if (deviceCtx->ScratchBlock == NULL) {
        deviceCtx->CowInitStatus = STATUS_INSUFFICIENT_RESOURCES;
        deviceCtx->CowRefuseReason = OF_REFUSE_CONFIG;
        DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
                   "OfFilter: CoW init fail step=%lu status=0x%08X (ScratchBlock)\n",
                   deviceCtx->CowInitStep, STATUS_INSUFFICIENT_RESOURCES);
        OfFilterTeardownCow(Device);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    deviceCtx->ScratchMdl = IoAllocateMdl(deviceCtx->ScratchBlock, OF_BLOCK_SIZE, FALSE, FALSE, NULL);
    if (deviceCtx->ScratchMdl == NULL) {
        deviceCtx->CowInitStatus = STATUS_INSUFFICIENT_RESOURCES;
        deviceCtx->CowRefuseReason = OF_REFUSE_CONFIG;
        DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
                   "OfFilter: CoW init fail step=%lu status=0x%08X (ScratchMdl)\n",
                   deviceCtx->CowInitStep, STATUS_INSUFFICIENT_RESOURCES);
        OfFilterTeardownCow(Device);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    MmBuildMdlForNonPagedPool(deviceCtx->ScratchMdl);

    //
    // Partial-MDL target big enough for any OF_MAX_LOWER_RUN range at any
    // page offset (MmSizeOfMdl at VA PAGE_SIZE-1 gives the worst-case span).
    // IoBuildPartialMdl fills it in; MmPrepareMdlForReuse after each use.
    //
    deviceCtx->CowInitStep = (ULONG)OfCowInitPartialMdl;
    {
        SIZE_T mdlBytes = MmSizeOfMdl((PVOID)(ULONG_PTR)(PAGE_SIZE - 1), OF_MAX_LOWER_RUN);
        deviceCtx->PartialMdl = (PMDL)ExAllocatePool2(POOL_FLAG_NON_PAGED, mdlBytes, OF_POOL_TAG);
        if (deviceCtx->PartialMdl == NULL) {
            deviceCtx->CowInitStatus = STATUS_INSUFFICIENT_RESOURCES;
            deviceCtx->CowRefuseReason = OF_REFUSE_CONFIG;
            DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
                       "OfFilter: CoW init fail step=%lu status=0x%08X (PartialMdl bytes=%Iu)\n",
                       deviceCtx->CowInitStep, STATUS_INSUFFICIENT_RESOURCES, mdlBytes);
            OfFilterTeardownCow(Device);
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        MmInitializeMdl(deviceCtx->PartialMdl, (PVOID)(ULONG_PTR)(PAGE_SIZE - 1), OF_MAX_LOWER_RUN);
    }

    deviceCtx->CowInitStep = (ULONG)OfCowInitOverlayCreate;
    if (driverCtx->OverlayBackend == (ULONG)OfOverlayFile) {
        status = OfOverlayCreateFile(Device,
                                     driverCtx->SlotCount,
                                     driverCtx->OverlayExtents,
                                     driverCtx->OverlayExtentCount,
                                     driverCtx->OverlayFileSize,
                                     &deviceCtx->Overlay);
    } else {
        status = OfOverlayCreateRam(Device, driverCtx->SlotCount, &deviceCtx->Overlay);
    }
    if (!NT_SUCCESS(status)) {
        deviceCtx->CowInitStatus = status;
        deviceCtx->CowRefuseReason = OF_REFUSE_CONFIG;
        DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
                   "OfFilter: CoW init fail step=%lu status=0x%08X (OverlayCreate backend=%lu slots=%lu fileSize=%I64u extents=%lu)\n",
                   deviceCtx->CowInitStep,
                   status,
                   driverCtx->OverlayBackend,
                   driverCtx->SlotCount,
                   driverCtx->OverlayFileSize,
                   driverCtx->OverlayExtentCount);
        OfFilterTeardownCow(Device);
        return status;
    }

    {
        WDF_OBJECT_ATTRIBUTES reqAttributes;
        HANDLE threadHandle = NULL;

        deviceCtx->CowInitStep = (ULONG)OfCowInitLowerRequest;
        WDF_OBJECT_ATTRIBUTES_INIT(&reqAttributes);
        reqAttributes.ParentObject = Device;
        status = WdfRequestCreate(&reqAttributes,
                                  WdfDeviceGetIoTarget(Device),
                                  &deviceCtx->LowerRequest);
        if (!NT_SUCCESS(status)) {
            deviceCtx->LowerRequest = NULL;
            deviceCtx->CowInitStatus = status;
            deviceCtx->CowRefuseReason = OF_REFUSE_CONFIG;
            DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
                       "OfFilter: CoW init fail step=%lu status=0x%08X (LowerRequest)\n",
                       deviceCtx->CowInitStep, status);
            OfFilterTeardownCow(Device);
            return status;
        }

        OfFilterSetWorkerStop(deviceCtx, FALSE); /* non-paged: spinlock must not be taken in PAGE code */

        deviceCtx->CowInitStep = (ULONG)OfCowInitWorker;
        status = PsCreateSystemThread(&threadHandle,
                                      THREAD_ALL_ACCESS,
                                      NULL,
                                      NULL,
                                      NULL,
                                      OfFilterCowWorker,
                                      Device);
        if (!NT_SUCCESS(status)) {
            OfFilterSetWorkerStop(deviceCtx, TRUE); /* non-paged: spinlock must not be taken in PAGE code */
            deviceCtx->CowInitStatus = status;
            deviceCtx->CowRefuseReason = OF_REFUSE_CONFIG;
            DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
                       "OfFilter: CoW init fail step=%lu status=0x%08X (PsCreateSystemThread)\n",
                       deviceCtx->CowInitStep, status);
            OfFilterTeardownCow(Device);
            return status;
        }

        status = ObReferenceObjectByHandle(threadHandle,
                                           THREAD_ALL_ACCESS,
                                           *PsThreadType,
                                           KernelMode,
                                           (PVOID *)&deviceCtx->WorkerThread,
                                           NULL);
        ZwClose(threadHandle);
        if (!NT_SUCCESS(status)) {
            //
            // Cannot wait for the thread without a reference. Practically
            // impossible for our own handle; stop it and fail closed to
            // passthrough. The thread exits on its own once it sees
            // WorkerStop and an empty list.
            //
            deviceCtx->WorkerThread = NULL;
            OfFilterSetWorkerStop(deviceCtx, TRUE); /* non-paged: spinlock must not be taken in PAGE code */
            KeSetEvent(&deviceCtx->WorkEvent, IO_NO_INCREMENT, FALSE);
            deviceCtx->CowInitStatus = status;
            deviceCtx->CowRefuseReason = OF_REFUSE_CONFIG;
            DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
                       "OfFilter: CoW init fail step=%lu status=0x%08X (ObReferenceObjectByHandle)\n",
                       deviceCtx->CowInitStep, status);
            return status;
        }
    }

    deviceCtx->CowInitStep = (ULONG)OfCowInitDone;
    deviceCtx->CowInitStatus = STATUS_SUCCESS;
    deviceCtx->CowRefuseReason = OF_REFUSE_NONE;
    deviceCtx->CowEnabled = TRUE;
    g_OfTargetDevice = Device;
    g_OfTargetDeviceContext = deviceCtx;
    g_OfWorkQueue = deviceCtx->WorkQueue;
    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DEVICE,
                "CoW enabled MaxBlocks=%lu SlotCount=%lu VolumeSize=%!I64d!",
                maxBlocks,
                driverCtx->SlotCount,
                volumeSize);
    DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
               "OfFilter: CoW init OK step=%lu backend=%lu slots=%lu maxBlocks=%lu\n",
               deviceCtx->CowInitStep,
               driverCtx->OverlayBackend,
               driverCtx->SlotCount,
               maxBlocks);
    return STATUS_SUCCESS;
}

VOID
OfFilterTeardownCow(
    _In_ WDFDEVICE Device
    )
{
    PDEVICE_CONTEXT deviceCtx;

    PAGED_CODE();

    deviceCtx = OfFilterGetDeviceContext(Device);

    /* Ask deferred init to exit; wait before tearing down CoW state. */
    deviceCtx->CowInitCancel = TRUE;
    if (deviceCtx->CowInitThread != NULL) {
        KeWaitForSingleObject(deviceCtx->CowInitThread, Executive, KernelMode, FALSE, NULL);
        ObDereferenceObject(deviceCtx->CowInitThread);
        deviceCtx->CowInitThread = NULL;
        deviceCtx->CowInitDeferredStarted = FALSE;
    }

    //
    // Stop accepting work, let the worker drain everything already queued
    // (in order, against the overlay), then wait for it to exit. Only after
    // that is the overlay freed. A volume that was frozen never silently
    // reverts to passthrough: later reads/writes fail (CowTornDown).
    //
    {
        OfFilterSetWorkerStop(deviceCtx, TRUE); /* non-paged: spinlock must not be taken in PAGE code */
    }

    if (deviceCtx->WorkerThread != NULL) {
        KeSetEvent(&deviceCtx->WorkEvent, IO_NO_INCREMENT, FALSE);
        KeWaitForSingleObject(deviceCtx->WorkerThread, Executive, KernelMode, FALSE, NULL);
        ObDereferenceObject(deviceCtx->WorkerThread);
        deviceCtx->WorkerThread = NULL;
    }

    if (deviceCtx->CowEnabled) {
        deviceCtx->CowTornDown = TRUE;
    }
    deviceCtx->CowEnabled = FALSE;

    if (deviceCtx->LowerRequest != NULL) {
        WdfObjectDelete(deviceCtx->LowerRequest);
        deviceCtx->LowerRequest = NULL;
    }

    if (deviceCtx->Overlay != NULL) {
        OfOverlayDestroy(deviceCtx->Overlay);
        deviceCtx->Overlay = NULL;
    }

    if (deviceCtx->PartialMdl != NULL) {
        ExFreePoolWithTag(deviceCtx->PartialMdl, OF_POOL_TAG);
        deviceCtx->PartialMdl = NULL;
    }

    if (deviceCtx->ScratchMdl != NULL) {
        IoFreeMdl(deviceCtx->ScratchMdl);
        deviceCtx->ScratchMdl = NULL;
    }

    if (deviceCtx->ScratchBlock != NULL) {
        ExFreePoolWithTag(deviceCtx->ScratchBlock, OF_POOL_TAG);
        deviceCtx->ScratchBlock = NULL;
    }

    if (deviceCtx->BlockMapReady) {
        OfBlockMapDestroy(&deviceCtx->BlockMap);
        deviceCtx->BlockMapReady = FALSE;
    }

    /* After the map: its L2 tables may live in the reserve. */
    if (deviceCtx->L2Reserve != NULL) {
        ExFreePoolWithTag(deviceCtx->L2Reserve, OF_POOL_TAG);
        deviceCtx->L2Reserve = NULL;
        deviceCtx->L2ReserveTop = 0;
        deviceCtx->L2ReserveUsed = 0;
    }
}

static
NTSTATUS
OfFilterQueryVolumeSize(
    _In_ WDFDEVICE Device,
    _Out_ OF_U64 *SizeBytes
    )
{
    GET_LENGTH_INFORMATION lengthInfo;
    WDF_MEMORY_DESCRIPTOR output;
    ULONG_PTR bytesReturned = 0;
    NTSTATUS status;

    *SizeBytes = 0;
    RtlZeroMemory(&lengthInfo, sizeof(lengthInfo));
    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&output, &lengthInfo, sizeof(lengthInfo));

    status = WdfIoTargetSendIoctlSynchronously(WdfDeviceGetIoTarget(Device),
                                               NULL,
                                               IOCTL_DISK_GET_LENGTH_INFO,
                                               NULL,
                                               &output,
                                               NULL,
                                               &bytesReturned);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    *SizeBytes = (OF_U64)lengthInfo.Length.QuadPart;
    return STATUS_SUCCESS;
}

static
NTSTATUS
OfFilterQueryVolumeUniqueId(
    _In_ WDFDEVICE Device,
    _Out_writes_bytes_(OutputLength) PUCHAR Output,
    _In_ ULONG OutputLength,
    _Out_ PUSHORT UniqueIdLength
    )
{
    WDFIOTARGET target;
    WDF_MEMORY_DESCRIPTOR outputDescriptor;
    MOUNTDEV_UNIQUE_ID probe;
    PMOUNTDEV_UNIQUE_ID fullId = NULL;
    ULONG_PTR bytesReturned = 0;
    ULONG needed;
    NTSTATUS status;

    *UniqueIdLength = 0;
    RtlZeroMemory(Output, OutputLength);
    RtlZeroMemory(&probe, sizeof(probe));

    target = WdfDeviceGetIoTarget(Device);

    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&outputDescriptor, &probe, sizeof(probe));
    status = WdfIoTargetSendIoctlSynchronously(target,
                                               NULL,
                                               IOCTL_MOUNTDEV_QUERY_UNIQUE_ID,
                                               NULL,
                                               &outputDescriptor,
                                               NULL,
                                               &bytesReturned);

    if (status != STATUS_BUFFER_OVERFLOW && status != STATUS_BUFFER_TOO_SMALL) {
        if (!NT_SUCCESS(status)) {
            return status;
        }
        return STATUS_INVALID_DEVICE_REQUEST;
    }

    needed = FIELD_OFFSET(MOUNTDEV_UNIQUE_ID, UniqueId) + probe.UniqueIdLength;
    if (probe.UniqueIdLength == 0 || needed > OutputLength) {
        return STATUS_BUFFER_TOO_SMALL;
    }

    fullId = (PMOUNTDEV_UNIQUE_ID)ExAllocatePool2(POOL_FLAG_NON_PAGED,
                                                  needed,
                                                  OF_POOL_TAG);
    if (fullId == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    RtlZeroMemory(fullId, needed);
    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&outputDescriptor, fullId, needed);
    status = WdfIoTargetSendIoctlSynchronously(target,
                                               NULL,
                                               IOCTL_MOUNTDEV_QUERY_UNIQUE_ID,
                                               NULL,
                                               &outputDescriptor,
                                               NULL,
                                               &bytesReturned);
    if (!NT_SUCCESS(status)) {
        ExFreePoolWithTag(fullId, OF_POOL_TAG);
        return status;
    }

    if (fullId->UniqueIdLength == 0 ||
        fullId->UniqueIdLength > OF_UNIQUE_ID_MAX_BYTES) {
        ExFreePoolWithTag(fullId, OF_POOL_TAG);
        return STATUS_INVALID_PARAMETER;
    }

    RtlCopyMemory(Output, fullId->UniqueId, fullId->UniqueIdLength);
    *UniqueIdLength = fullId->UniqueIdLength;
    ExFreePoolWithTag(fullId, OF_POOL_TAG);
    return STATUS_SUCCESS;
}


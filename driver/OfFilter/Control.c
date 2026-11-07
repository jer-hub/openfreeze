/*++

Module Name:

    Control.c

Abstract:

    KMDF control device \\.\OfFilter (SYSTEM SDDL) + Phase 4/5 IOCTLs.

--*/

#include "Control.h"
#include "Device.h"
#include "Io.h"

extern PDEVICE_CONTEXT g_OfTargetDeviceContext;
extern WDFDEVICE g_OfTargetDevice;

/* ntddk.h does not prototype this; ntifs.h conflicts with our includes. */
NTKERNELAPI
ULONG
IoGetRequestorProcessId(
    _In_ PIRP Irp
    );

#ifdef ALLOC_PRAGMA
#pragma alloc_text(PAGE, OfFilterCreateControlDevice)
#pragma alloc_text(PAGE, OfFilterDeleteControlDevice)
#endif

static
VOID
OfFilterEvtControlDeviceControl(
    _In_ WDFQUEUE Queue,
    _In_ WDFREQUEST Request,
    _In_ size_t OutputBufferLength,
    _In_ size_t InputBufferLength,
    _In_ ULONG IoControlCode
    );

static
VOID
OfFilterEvtOverlayEventCancel(
    _In_ WDFREQUEST Request
    );

static
VOID
OfFilterEvtControlFileCreate(
    _In_ WDFDEVICE Device,
    _In_ WDFREQUEST Request,
    _In_ WDFFILEOBJECT FileObject
    );

static
VOID
OfFilterEvtControlFileCleanup(
    _In_ WDFFILEOBJECT FileObject
    );

static
BOOLEAN
OfFilterControlCallerIsOwner(
    _In_ WDFREQUEST Request,
    _In_ PDRIVER_CONTEXT DriverCtx
    );

NTSTATUS
OfFilterCreateControlDevice(
    _In_ WDFDRIVER Driver
    )
{
    PWDFDEVICE_INIT init = NULL;
    WDFDEVICE control = NULL;
    WDF_OBJECT_ATTRIBUTES attrs;
    WDF_IO_QUEUE_CONFIG qcfg;
    PDRIVER_CONTEXT driverCtx;
    NTSTATUS status;
    DECLARE_CONST_UNICODE_STRING(devName, OFFILTER_CONTROL_DEVICE_NAME);
    DECLARE_CONST_UNICODE_STRING(symName, OFFILTER_CONTROL_SYMLINK_NAME);
    DECLARE_CONST_UNICODE_STRING(sddl, OFFILTER_CONTROL_SDDL_STRING);

    PAGED_CODE();

    driverCtx = OfFilterGetDriverContext(Driver);
    if (driverCtx->ControlDevice != NULL) {
        return STATUS_SUCCESS;
    }

    init = WdfControlDeviceInitAllocate(Driver, &sddl);
    if (init == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    WdfDeviceInitSetExclusive(init, FALSE);
    status = WdfDeviceInitAssignName(init, &devName);
    if (!NT_SUCCESS(status)) {
        WdfDeviceInitFree(init);
        return status;
    }

    {
        WDF_FILEOBJECT_CONFIG fileCfg;
        WDF_FILEOBJECT_CONFIG_INIT(&fileCfg,
                                   OfFilterEvtControlFileCreate,
                                   WDF_NO_EVENT_CALLBACK,
                                   OfFilterEvtControlFileCleanup);
        WdfDeviceInitSetFileObjectConfig(init, &fileCfg, WDF_NO_OBJECT_ATTRIBUTES);
    }

    WDF_OBJECT_ATTRIBUTES_INIT(&attrs);
    status = WdfDeviceCreate(&init, &attrs, &control);
    if (!NT_SUCCESS(status)) {
        if (init != NULL) {
            WdfDeviceInitFree(init);
        }
        return status;
    }

    status = WdfDeviceCreateSymbolicLink(control, &symName);
    if (!NT_SUCCESS(status)) {
        WdfObjectDelete(control);
        return status;
    }

    /* Parallel: a pended GET_OVERLAY_EVENT must not block GET_STATUS / SET_MODE. */
    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&qcfg, WdfIoQueueDispatchParallel);
    qcfg.EvtIoDeviceControl = OfFilterEvtControlDeviceControl;
    status = WdfIoQueueCreate(control, &qcfg, WDF_NO_OBJECT_ATTRIBUTES, WDF_NO_HANDLE);
    if (!NT_SUCCESS(status)) {
        WdfObjectDelete(control);
        return status;
    }

    WdfControlFinishInitializing(control);
    driverCtx->ControlDevice = control;
    driverCtx->ControlOwnerPid = 0;
    driverCtx->ControlOwnerHandleCount = 0;
    DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
               "OfFilter: control device ready (\\\\.\\OfFilter)\n");
    return STATUS_SUCCESS;
}

VOID
OfFilterDeleteControlDevice(
    _In_ WDFDRIVER Driver
    )
{
    PDRIVER_CONTEXT driverCtx;

    PAGED_CODE();

    driverCtx = OfFilterGetDriverContext(Driver);
    if (driverCtx->ControlDevice != NULL) {
        WdfObjectDelete(driverCtx->ControlDevice);
        driverCtx->ControlDevice = NULL;
        driverCtx->ControlOwnerPid = 0;
        driverCtx->ControlOwnerHandleCount = 0;
    }
}

static
VOID
OfFilterEvtControlFileCreate(
    _In_ WDFDEVICE Device,
    _In_ WDFREQUEST Request,
    _In_ WDFFILEOBJECT FileObject
    )
{
    PDRIVER_CONTEXT driverCtx;
    LONG pid;
    LONG prior;

    UNREFERENCED_PARAMETER(FileObject);

    driverCtx = OfFilterGetDriverContext(WdfDeviceGetDriver(Device));
    pid = (LONG)(ULONG_PTR)PsGetCurrentProcessId();
    prior = InterlockedCompareExchange(&driverCtx->ControlOwnerPid, pid, 0);
    if (prior == 0 || prior == pid) {
        InterlockedIncrement(&driverCtx->ControlOwnerHandleCount);
        if (prior == 0) {
            DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
                       "OfFilter: control owner pid=%ld\n", pid);
        }
    }
    WdfRequestComplete(Request, STATUS_SUCCESS);
}

static
VOID
OfFilterEvtControlFileCleanup(
    _In_ WDFFILEOBJECT FileObject
    )
{
    PDRIVER_CONTEXT driverCtx;
    LONG pid;
    LONG left;

    driverCtx = OfFilterGetDriverContext(WdfDeviceGetDriver(WdfFileObjectGetDevice(FileObject)));
    pid = (LONG)(ULONG_PTR)PsGetCurrentProcessId();
    if (driverCtx->ControlOwnerPid != pid) {
        return;
    }
    left = InterlockedDecrement(&driverCtx->ControlOwnerHandleCount);
    if (left <= 0) {
        InterlockedExchange(&driverCtx->ControlOwnerHandleCount, 0);
        InterlockedExchange(&driverCtx->ControlOwnerPid, 0);
        DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
                   "OfFilter: control owner cleared (pid=%ld)\n", pid);
    }
}

static
BOOLEAN
OfFilterControlCallerIsOwner(
    _In_ WDFREQUEST Request,
    _In_ PDRIVER_CONTEXT DriverCtx
    )
{
    LONG reqPid;
    LONG owner;
    PIRP irp;

    irp = WdfRequestWdmGetIrp(Request);
    if (irp != NULL) {
        reqPid = (LONG)IoGetRequestorProcessId(irp);
    } else {
        reqPid = (LONG)(ULONG_PTR)PsGetCurrentProcessId();
    }
    owner = DriverCtx->ControlOwnerPid;
    if (owner == 0) {
        /* No create-path owner yet: claim on first SET. */
        InterlockedCompareExchange(&DriverCtx->ControlOwnerPid, reqPid, 0);
        owner = DriverCtx->ControlOwnerPid;
    }
    return (owner == reqPid) ? TRUE : FALSE;
}

static
VOID
OfFilterEvtOverlayEventCancel(
    _In_ WDFREQUEST Request
    )
{
    PDEVICE_CONTEXT deviceCtx = g_OfTargetDeviceContext;
    KIRQL irql;

    if (deviceCtx != NULL) {
        KeAcquireSpinLock(&deviceCtx->OverlayEventLock, &irql);
        if (deviceCtx->OverlayEventWaiter == Request) {
            deviceCtx->OverlayEventWaiter = NULL;
        }
        KeReleaseSpinLock(&deviceCtx->OverlayEventLock, irql);
    }
    WdfRequestComplete(Request, STATUS_CANCELLED);
}

static
VOID
OfFilterEvtControlDeviceControl(
    _In_ WDFQUEUE Queue,
    _In_ WDFREQUEST Request,
    _In_ size_t OutputBufferLength,
    _In_ size_t InputBufferLength,
    _In_ ULONG IoControlCode
    )
{
    PDEVICE_CONTEXT deviceCtx;
    PDRIVER_CONTEXT driverCtx;
    NTSTATUS status;
    WDFDEVICE volume;

    UNREFERENCED_PARAMETER(Queue);
    UNREFERENCED_PARAMETER(InputBufferLength);

    if (KeGetCurrentIrql() != PASSIVE_LEVEL) {
        WdfRequestComplete(Request, STATUS_INVALID_DEVICE_STATE);
        return;
    }

    deviceCtx = g_OfTargetDeviceContext;
    volume = g_OfTargetDevice;
    if (deviceCtx == NULL || volume == NULL || !deviceCtx->IsTargetVolume) {
        WdfRequestComplete(Request, STATUS_DEVICE_NOT_READY);
        return;
    }
    driverCtx = OfFilterGetDriverContext(WdfDeviceGetDriver(volume));

    if (IoControlCode == IOCTL_OF_GET_STATUS ||
        IoControlCode == IOCTL_OF_DEBUG_QUERY_COW) {
        POF_STATUS_V1 st;
        size_t outLen = 0;

        if (OutputBufferLength < sizeof(OF_STATUS_V1)) {
            WdfRequestComplete(Request, STATUS_BUFFER_TOO_SMALL);
            return;
        }
        status = WdfRequestRetrieveOutputBuffer(Request, sizeof(OF_STATUS_V1),
                                                (PVOID *)&st, &outLen);
        if (!NT_SUCCESS(status) || st == NULL) {
            WdfRequestComplete(Request, !NT_SUCCESS(status) ? status : STATUS_BUFFER_TOO_SMALL);
            return;
        }
        RtlZeroMemory(st, sizeof(*st));
        st->CowEnabled = deviceCtx->CowEnabled ? 1u : 0u;
        st->CowTornDown = deviceCtx->CowTornDown ? 1u : 0u;
        st->SlotsUsed = deviceCtx->BlockMapReady ? deviceCtx->BlockMap.SlotsUsed : 0;
        st->SlotCount = driverCtx->SlotCount;
        st->OverlayBackend = driverCtx->OverlayBackend;
        st->RefuseReason = deviceCtx->CowRefuseReason;
        st->CfgValid = deviceCtx->CfgValid;
        st->ThawNext = deviceCtx->ThawNext;
        st->CfgSeq = deviceCtx->CfgSeq;
        st->ThawedForN = deviceCtx->ThawedForN;
        st->InitStep = deviceCtx->CowInitStep;
        st->InitStatus = (ULONG)deviceCtx->CowInitStatus;
        WdfRequestCompleteWithInformation(Request, STATUS_SUCCESS, sizeof(*st));
        return;
    }

    if (IoControlCode == IOCTL_OF_SET_THAW_NEXT_BOOT ||
        IoControlCode == IOCTL_OF_SET_NEXT_BOOT_MODE) {
        PULONG inVal = NULL;
        POF_SET_NEXT_BOOT_MODE_IN modeIn = NULL;
        size_t inLen = 0;

        /* Phase 5: only the registered opener PID may SET on \\.\OfFilter. */
        if (!OfFilterControlCallerIsOwner(Request, driverCtx)) {
            WdfRequestComplete(Request, STATUS_ACCESS_DENIED);
            return;
        }

        if (IoControlCode == IOCTL_OF_SET_NEXT_BOOT_MODE) {
            status = WdfRequestRetrieveInputBuffer(Request, sizeof(*modeIn),
                                                   (PVOID *)&modeIn, &inLen);
            if (!NT_SUCCESS(status) || modeIn == NULL) {
                WdfRequestComplete(Request, !NT_SUCCESS(status) ? status : STATUS_BUFFER_TOO_SMALL);
                return;
            }
            status = OfFilterApplyNextBootMode(volume, modeIn->Mode, modeIn->ThawedForN);
            WdfRequestComplete(Request, status);
            return;
        }

        status = WdfRequestRetrieveInputBuffer(Request, sizeof(ULONG),
                                               (PVOID *)&inVal, &inLen);
        if (!NT_SUCCESS(status) || inVal == NULL) {
            WdfRequestComplete(Request, !NT_SUCCESS(status) ? status : STATUS_BUFFER_TOO_SMALL);
            return;
        }
        status = OfFilterApplyThawNextBoot(volume, (*inVal != 0) ? TRUE : FALSE);
        WdfRequestComplete(Request, status);
        return;
    }

    if (IoControlCode == IOCTL_OF_GET_OVERLAY_EVENT) {
        POF_OVERLAY_EVENT_V1 ev;
        size_t outLen = 0;
        ULONG used;
        ULONG total;
        ULONG curEvent = OF_OVERLAY_EVENT_NONE;
        KIRQL irql;
        WDFREQUEST prev;

        if (OutputBufferLength < sizeof(*ev)) {
            WdfRequestComplete(Request, STATUS_BUFFER_TOO_SMALL);
            return;
        }
        status = WdfRequestRetrieveOutputBuffer(Request, sizeof(*ev), (PVOID *)&ev, &outLen);
        if (!NT_SUCCESS(status) || ev == NULL) {
            WdfRequestComplete(Request, !NT_SUCCESS(status) ? status : STATUS_BUFFER_TOO_SMALL);
            return;
        }

        used = deviceCtx->BlockMapReady ? deviceCtx->BlockMap.SlotsUsed : 0;
        total = deviceCtx->BlockMapReady ? deviceCtx->BlockMap.SlotCount : 0;
        if (deviceCtx->OverlayFullLogged || (total > 0 && used >= total)) {
            curEvent = OF_OVERLAY_EVENT_FULL;
        } else if (total > 0 && used * 100u >= total * OF_OVERLAY_NEAR_FULL_PCT) {
            curEvent = OF_OVERLAY_EVENT_NEAR_FULL;
        }

        if (curEvent != OF_OVERLAY_EVENT_NONE) {
            RtlZeroMemory(ev, sizeof(*ev));
            ev->Event = curEvent;
            ev->SlotsUsed = used;
            ev->SlotCount = total;
            WdfRequestCompleteWithInformation(Request, STATUS_SUCCESS, sizeof(*ev));
            return;
        }

        /* Pend until near-full / full (one waiter). */
        KeAcquireSpinLock(&deviceCtx->OverlayEventLock, &irql);
        prev = deviceCtx->OverlayEventWaiter;
        deviceCtx->OverlayEventWaiter = Request;
        KeReleaseSpinLock(&deviceCtx->OverlayEventLock, irql);
        if (prev != NULL) {
            if (WdfRequestUnmarkCancelable(prev) != STATUS_CANCELLED) {
                WdfRequestComplete(prev, STATUS_CANCELLED);
            }
        }
        status = WdfRequestMarkCancelableEx(Request, OfFilterEvtOverlayEventCancel);
        if (!NT_SUCCESS(status)) {
            KeAcquireSpinLock(&deviceCtx->OverlayEventLock, &irql);
            if (deviceCtx->OverlayEventWaiter == Request) {
                deviceCtx->OverlayEventWaiter = NULL;
            }
            KeReleaseSpinLock(&deviceCtx->OverlayEventLock, irql);
            WdfRequestComplete(Request, status);
        }
        return;
    }

    WdfRequestComplete(Request, STATUS_INVALID_DEVICE_REQUEST);
}

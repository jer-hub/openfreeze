/*++

Module Name:

    Io.h

Abstract:

    I/O queue callbacks (passthrough and CoW).

--*/

#pragma once

#include "Driver.h"

EVT_WDF_IO_QUEUE_IO_READ OfFilterEvtIoRead;
EVT_WDF_IO_QUEUE_IO_WRITE OfFilterEvtIoWrite;
EVT_WDF_IO_QUEUE_IO_DEFAULT OfFilterEvtIoDefault;
EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL OfFilterEvtIoDeviceControl;

VOID
OfFilterForwardRequest(
    _In_ WDFREQUEST Request,
    _In_ WDFIOTARGET Target
    );

/* Persist ThawNextBoot in overlay config sector (file backend). PASSIVE. */
NTSTATUS
OfFilterApplyThawNextBoot(
    _In_ WDFDEVICE Device,
    _In_ BOOLEAN WantSet
    );

/* Persist next-boot mode + optional ThawedForN (OF_MODE_*). PASSIVE. */
NTSTATUS
OfFilterApplyNextBootMode(
    _In_ WDFDEVICE Device,
    _In_ ULONG Mode,
    _In_ ULONG ThawedForN
    );

/* Complete a pended overlay-event waiter (or stash level). DISPATCH ok. */
VOID
OfFilterSignalOverlayEvent(
    _In_ PDEVICE_CONTEXT DeviceCtx,
    _In_ ULONG Event
    );

#define OF_OVERLAY_NEAR_FULL_PCT 90u

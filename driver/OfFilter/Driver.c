/*++

Module Name:

    Driver.c

Abstract:

    DriverEntry, device-add dispatch, and parameter loading for OfFilter.

--*/

#include "Driver.h"
#include "Device.h"
#include "Control.h"
#include "Protect.h"
#include "driver.tmh"

#define OF_RAM_CHUNK_SLOTS_MIN 256u

DECLARE_CONST_UNICODE_STRING(OfFilterVolumeUniqueIdValueName, L"VolumeUniqueId");
DECLARE_CONST_UNICODE_STRING(OfFilterEnableCowValueName, L"EnableCow");
DECLARE_CONST_UNICODE_STRING(OfFilterOverlayBackendValueName, L"OverlayBackend");
DECLARE_CONST_UNICODE_STRING(OfFilterSlotCountValueName, L"SlotCount");
DECLARE_CONST_UNICODE_STRING(OfFilterPassthroughRangesValueName, L"PassthroughRanges");
DECLARE_CONST_UNICODE_STRING(OfFilterOverlayExtentsValueName, L"OverlayExtents");
DECLARE_CONST_UNICODE_STRING(OfFilterOverlayFileSizeValueName, L"OverlayFileSize");
DECLARE_CONST_UNICODE_STRING(OfFilterOverlayGenerationValueName, L"OverlayGeneration");

#ifdef ALLOC_PRAGMA
#pragma alloc_text(INIT, DriverEntry)
#pragma alloc_text(PAGE, OfFilterEvtDeviceAdd)
#pragma alloc_text(PAGE, OfFilterEvtDriverContextCleanup)
#pragma alloc_text(PAGE, OfFilterLoadParameters)
#endif

NTSTATUS
DriverEntry(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PUNICODE_STRING RegistryPath
    )
{
    WDF_DRIVER_CONFIG config;
    WDF_OBJECT_ATTRIBUTES attributes;
    WDFDRIVER driver;
    NTSTATUS status;

    WPP_INIT_TRACING(DriverObject, RegistryPath);

    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DRIVER, "%!FUNC! Entry");

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, DRIVER_CONTEXT);
    attributes.EvtCleanupCallback = OfFilterEvtDriverContextCleanup;

    WDF_DRIVER_CONFIG_INIT(&config, OfFilterEvtDeviceAdd);

    status = WdfDriverCreate(DriverObject,
                             RegistryPath,
                             &attributes,
                             &config,
                             &driver);
    if (!NT_SUCCESS(status)) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DRIVER,
                    "WdfDriverCreate failed %!STATUS!", status);
        WPP_CLEANUP(DriverObject);
        return status;
    }

    status = OfFilterLoadParameters(driver);
    if (!NT_SUCCESS(status)) {
        TraceEvents(TRACE_LEVEL_WARNING, TRACE_DRIVER,
                    "OfFilterLoadParameters returned %!STATUS! - using defaults",
                    status);
    }

    status = OfFilterCreateControlDevice(driver);
    if (!NT_SUCCESS(status)) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DRIVER,
                    "OfFilterCreateControlDevice failed %!STATUS!", status);
        /* Non-fatal for volume filter attach; ofctl control path unavailable. */
    }

    /* Phase 5: Ob/Cm self-protection (non-fatal if registration fails). */
    (void)OfFilterProtectStart(driver);

    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DRIVER, "%!FUNC! Exit");
    return STATUS_SUCCESS;
}

NTSTATUS
OfFilterEvtDeviceAdd(
    _In_ WDFDRIVER Driver,
    _Inout_ PWDFDEVICE_INIT DeviceInit
    )
{
    NTSTATUS status;

    PAGED_CODE();

    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DRIVER, "%!FUNC! Entry");
    status = OfFilterCreateDevice(Driver, DeviceInit);
    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DRIVER,
                "%!FUNC! Exit %!STATUS!", status);
    return status;
}

VOID
OfFilterEvtDriverContextCleanup(
    _In_ WDFOBJECT DriverObject
    )
{
    PAGED_CODE();

    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DRIVER, "%!FUNC! Entry");
    OfFilterProtectStop((WDFDRIVER)DriverObject);
    OfFilterDeleteControlDevice((WDFDRIVER)DriverObject);
    WPP_CLEANUP(WdfDriverWdmGetDriverObject((WDFDRIVER)DriverObject));
}

NTSTATUS
OfFilterLoadParameters(
    _In_ WDFDRIVER Driver
    )
{
    PDRIVER_CONTEXT driverCtx;
    WDFKEY parametersKey = NULL;
    WDFMEMORY memory = NULL;
    size_t size = 0;
    PUCHAR buffer;
    ULONG value;
    NTSTATUS status;

    PAGED_CODE();

    driverCtx = OfFilterGetDriverContext(Driver);
    RtlZeroMemory(driverCtx, sizeof(*driverCtx));
    driverCtx->SlotCount = OF_DEFAULT_SLOT_COUNT;
    driverCtx->OverlayBackend = 0; /* OfOverlayRam */

    status = WdfDriverOpenParametersRegistryKey(Driver,
                                                KEY_READ,
                                                WDF_NO_OBJECT_ATTRIBUTES,
                                                &parametersKey);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    status = WdfRegistryQueryMemory(parametersKey,
                                    &OfFilterVolumeUniqueIdValueName,
                                    NonPagedPoolNx,
                                    WDF_NO_OBJECT_ATTRIBUTES,
                                    &memory,
                                    NULL);
    if (NT_SUCCESS(status)) {
        buffer = (PUCHAR)WdfMemoryGetBuffer(memory, &size);
        if (buffer != NULL && size > 0 && size <= OF_UNIQUE_ID_MAX_BYTES) {
            RtlCopyMemory(driverCtx->UniqueId, buffer, size);
            driverCtx->UniqueIdLength = (USHORT)size;
            driverCtx->UniqueIdConfigured = TRUE;
            TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DRIVER,
                        "Loaded VolumeUniqueId (%hu bytes)",
                        driverCtx->UniqueIdLength);
        }
        WdfObjectDelete(memory);
        memory = NULL;
    }

    if (NT_SUCCESS(WdfRegistryQueryULong(parametersKey,
                                         &OfFilterEnableCowValueName,
                                         &value))) {
        driverCtx->EnableCow = (value != 0) ? TRUE : FALSE;
    }

    if (NT_SUCCESS(WdfRegistryQueryULong(parametersKey,
                                         &OfFilterOverlayBackendValueName,
                                         &value))) {
        driverCtx->OverlayBackend = value;
    }

    if (NT_SUCCESS(WdfRegistryQueryULong(parametersKey,
                                         &OfFilterSlotCountValueName,
                                         &value))) {
        if (value >= OF_RAM_CHUNK_SLOTS_MIN && value <= OF_MAX_SLOT_COUNT) {
            driverCtx->SlotCount = value;
        }
    }

    //
    // PassthroughRanges: REG_BINARY array of OF_RANGE (UINT64 offset, UINT64
    // length). Written by the ofranges helper before freezing. A malformed
    // value is ignored (no ranges) and logged.
    //
    status = WdfRegistryQueryMemory(parametersKey,
                                    &OfFilterPassthroughRangesValueName,
                                    PagedPool,
                                    WDF_NO_OBJECT_ATTRIBUTES,
                                    &memory,
                                    NULL);
    if (NT_SUCCESS(status)) {
        buffer = (PUCHAR)WdfMemoryGetBuffer(memory, &size);
        if (buffer != NULL && size > 0 && (size % sizeof(OF_RANGE)) == 0 &&
            size / sizeof(OF_RANGE) <= OF_MAX_PASSTHROUGH_RANGES) {
            RtlCopyMemory(driverCtx->PassthroughRaw, buffer, size);
            driverCtx->PassthroughRawCount = (ULONG)(size / sizeof(OF_RANGE));
        } else {
            TraceEvents(TRACE_LEVEL_ERROR, TRACE_DRIVER,
                        "PassthroughRanges malformed (%Iu bytes) - ignored", size);
            DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
                       "OfFilter: PassthroughRanges malformed (%Iu bytes) - ignored\n", size);
        }
        WdfObjectDelete(memory);
        memory = NULL;
    }
    
    driverCtx->OverlayExtentsLoaded = FALSE;
    driverCtx->OverlayExtentCount = 0;
    driverCtx->OverlayFileSize = 0;
    driverCtx->OverlayGeneration = 0;

    if (NT_SUCCESS(WdfRegistryQueryULong(parametersKey,
                                         &OfFilterOverlayGenerationValueName,
                                         &value))) {
        driverCtx->OverlayGeneration = value;
    }

    status = WdfRegistryQueryMemory(parametersKey,
                                    &OfFilterOverlayFileSizeValueName,
                                    PagedPool,
                                    WDF_NO_OBJECT_ATTRIBUTES,
                                    &memory,
                                    NULL);
    if (NT_SUCCESS(status)) {
        buffer = (PUCHAR)WdfMemoryGetBuffer(memory, &size);
        if (buffer != NULL && size == sizeof(OF_U64)) {
            RtlCopyMemory(&driverCtx->OverlayFileSize, buffer, sizeof(OF_U64));
        }
        WdfObjectDelete(memory);
        memory = NULL;
    }

    status = WdfRegistryQueryMemory(parametersKey,
                                    &OfFilterOverlayExtentsValueName,
                                    NonPagedPoolNx,
                                    WDF_NO_OBJECT_ATTRIBUTES,
                                    &memory,
                                    NULL);
    if (NT_SUCCESS(status)) {
        buffer = (PUCHAR)WdfMemoryGetBuffer(memory, &size);
        if (buffer != NULL && size > 0 &&
            (size % sizeof(OF_OVERLAY_EXTENT)) == 0 &&
            size / sizeof(OF_OVERLAY_EXTENT) <= OF_MAX_OVERLAY_EXTENTS) {
            RtlCopyMemory(driverCtx->OverlayExtents, buffer, size);
            driverCtx->OverlayExtentCount = (ULONG)(size / sizeof(OF_OVERLAY_EXTENT));
            driverCtx->OverlayExtentsLoaded = TRUE;
            TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DRIVER,
                        "Loaded OverlayExtents count=%lu fileSize=%!I64d! gen=%lu",
                        driverCtx->OverlayExtentCount,
                        driverCtx->OverlayFileSize,
                        driverCtx->OverlayGeneration);
        } else {
            TraceEvents(TRACE_LEVEL_ERROR, TRACE_DRIVER,
                        "OverlayExtents malformed (%Iu bytes) - ignored", size);
        }
        WdfObjectDelete(memory);
        memory = NULL;
    }
    status = STATUS_SUCCESS;
    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DRIVER,
                "PassthroughRanges raw=%lu", driverCtx->PassthroughRawCount);
    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DRIVER,
                "EnableCow=%!BOOLEAN! Backend=%lu SlotCount=%lu",
                driverCtx->EnableCow,
                driverCtx->OverlayBackend,
                driverCtx->SlotCount);

    WdfRegistryClose(parametersKey);
    return STATUS_SUCCESS;
}


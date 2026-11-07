/*++

Module Name:

    Driver.h

Abstract:

    Driver-wide types, globals, and entry-point declarations.

--*/

#pragma once

#include <ntddk.h>
#include <wdf.h>
#include <ntstrsafe.h>
#include <mountdev.h>
#include <ntddstor.h>
#include <ntdddisk.h>

#include "OfIoctl.h"
#include "OverlayMap.h"
#include "RangeList.h"
#include "Trace.h"

#define OF_POOL_TAG 'tFfO'

// Max unique-id bytes we persist / compare (MOUNTDEV_UNIQUE_ID is variable-length).
#define OF_UNIQUE_ID_MAX_BYTES 256

// RAM overlay cap in 4 KB slots. Default 65536 = 256 MB NonPagedPool,
// preallocated at device start. Max 262144 = 1 GB (test VM has 2-3 GB).
#define OF_DEFAULT_SLOT_COUNT 65536u
#define OF_MAX_SLOT_COUNT     262144u

//
// Debug builds only: turn CoW on for the matched volume on a running system
// (sent to \\.\C: by an admin). One way: there is no runtime disable; a reboot
// with EnableCow=0 discards the overlay. Lets the CoW path be exercised under
// Verifier/kd without a frozen-from-boot cycle.
//
/* IOCTL codes: shared/OfIoctl.h (included above). */

typedef struct _OF_DEBUG_COW_STATE {
    ULONG CowEnabled;
    ULONG CowTornDown;
    ULONG SlotsUsed;
    ULONG PendingWrites;
    LONGLONG BypassReads;
    LONGLONG WorkerReads;
    ULONGLONG OverlayFullCount;
} OF_DEBUG_COW_STATE, *POF_DEBUG_COW_STATE;

/* Extended query result; filled when the caller's buffer is this large.
   The first 40 bytes are OF_DEBUG_COW_STATE unchanged. */
typedef struct _OF_DEBUG_COW_STATE_V2 {
    OF_DEBUG_COW_STATE V1;
    ULONG PassthroughRangeCount;
    ULONG L2ReserveUsed;
    LONGLONG PassthroughWrites;
    LONGLONG PassthroughReads;
    ULONGLONG PassthroughBytes;  /* sum of range lengths */
    ULONG IsTargetVolume;        /* 1 if this volume matched VolumeUniqueId */
    ULONG RefuseReason;          /* 0=none, 1=NotTarget, 2=ConfigRefuse */
} OF_DEBUG_COW_STATE_V2, *POF_DEBUG_COW_STATE_V2;

/* RefuseReason values for OF_DEBUG_COW_STATE_V2. */
#define OF_REFUSE_NONE          0u
#define OF_REFUSE_NOT_TARGET    1u
#define OF_REFUSE_CONFIG        2u
#define OF_REFUSE_THAW_REQUESTED  3u
#define OF_REFUSE_CONFIG_CORRUPT  4u

/* CoW init progress for ofquery diagnostics (ConfigRefuse root-cause). */
typedef enum _OF_COW_INIT_STEP {
    OfCowInitNone = 0,
    OfCowInitVolumeSize = 1,
    OfCowInitPassthrough = 2,
    OfCowInitL2Reserve = 3,
    OfCowInitFileConfig = 4,
    OfCowInitValidateExtents = 5,
    OfCowInitConfigSector = 6,
    OfCowInitBlockMap = 7,
    OfCowInitScratch = 8,
    OfCowInitPartialMdl = 9,
    OfCowInitOverlayCreate = 10,
    OfCowInitLowerRequest = 11,
    OfCowInitWorker = 12,
    OfCowInitDone = 13
} OF_COW_INIT_STEP;

/* V3: appends init diagnostics after V2 (80 bytes). Total 88. */
typedef struct _OF_DEBUG_COW_STATE_V3 {
    OF_DEBUG_COW_STATE_V2 V2;
    ULONG InitStep;              /* OF_COW_INIT_STEP */
    ULONG InitStatus;            /* NTSTATUS of last init failure (0 on success) */
} OF_DEBUG_COW_STATE_V3, *POF_DEBUG_COW_STATE_V3;

/* V4: config-sector query fields after V3 (88). Total 100. */
typedef struct _OF_DEBUG_COW_STATE_V4 {
    OF_DEBUG_COW_STATE_V3 V3;
    ULONG CfgValid;
    ULONG ThawNext;
    ULONG CfgSeq;
} OF_DEBUG_COW_STATE_V4, *POF_DEBUG_COW_STATE_V4;

typedef struct _DRIVER_CONTEXT {
    BOOLEAN UniqueIdConfigured;
    USHORT UniqueIdLength;
    UCHAR UniqueId[OF_UNIQUE_ID_MAX_BYTES];

    BOOLEAN EnableCow;
    ULONG OverlayBackend;        /* 0 = RAM, 1 = File */
    ULONG SlotCount;

    /* File backend (OverlayBackend=1): extents + metadata from ofoverlay. */
    BOOLEAN OverlayExtentsLoaded;
    ULONG OverlayExtentCount;
    ULONG OverlayGeneration;
    OF_U64 OverlayFileSize;
    OF_OVERLAY_EXTENT OverlayExtents[OF_MAX_OVERLAY_EXTENTS];

    /* Raw PassthroughRanges from the registry (normalized per device at
       CoW start, once the volume size is known). */
    ULONG PassthroughRawCount;
    OF_RANGE PassthroughRaw[OF_MAX_PASSTHROUGH_RANGES];

    /* Phase 4: SYSTEM-only control device (\\.\OfFilter). */
    WDFDEVICE ControlDevice;

    /* Phase 5: first opener of \\.\OfFilter owns SET_* IOCTLs. */
    volatile LONG ControlOwnerPid;          /* 0 = none; else PID */
    volatile LONG ControlOwnerHandleCount;
} DRIVER_CONTEXT, *PDRIVER_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(DRIVER_CONTEXT, OfFilterGetDriverContext);

DRIVER_INITIALIZE DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD OfFilterEvtDeviceAdd;
EVT_WDF_OBJECT_CONTEXT_CLEANUP OfFilterEvtDriverContextCleanup;

NTSTATUS
OfFilterLoadParameters(
    _In_ WDFDRIVER Driver
    );

NTSTATUS
OfFilterCreateDevice(
    _In_ WDFDRIVER Driver,
    _Inout_ PWDFDEVICE_INIT DeviceInit
    );

NTSTATUS
OfFilterConfigureIoQueues(
    _In_ WDFDEVICE Device
    );


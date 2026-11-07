/*++

Module Name:

    Device.h

Abstract:

    Per-volume filter device context.

--*/

#pragma once

#include "Driver.h"
#include "BlockMap.h"
#include "Overlay.h"

/* Largest single lower read the worker issues (unmapped runs are split). */
#define OF_MAX_LOWER_RUN      (1024u * 1024u)
/* Requests the worker handles back to back before it yields. */
#define OF_WORKER_BATCH       64u
/* Preallocated block-map L2 tables (2 KB each, 512 KB total). */
#define OF_L2_RESERVE_COUNT   256u
#define OF_L2_BYTES           (OF_BLOCK_MAP_L2_ENTRIES * sizeof(OF_U32))

typedef struct _DEVICE_CONTEXT {
    BOOLEAN IsTargetVolume;
    BOOLEAN UniqueIdValid;
    BOOLEAN CowEnabled;
    USHORT UniqueIdLength;
    UCHAR UniqueId[OF_UNIQUE_ID_MAX_BYTES];

    OF_BLOCK_MAP BlockMap;
    BOOLEAN BlockMapReady;
    POF_OVERLAY Overlay;
    WDFWAITLOCK CowLock;
    PUCHAR ScratchBlock;         /* OF_BLOCK_SIZE RMW / lower I/O scratch */
    OF_U64 VolumeSizeBytes;

    /* Phase 2 worker: frozen writes/flushes (and reads that cannot bypass)
       are forwarded to WorkQueue (manual). One system thread pulls them with
       WdfIoQueueRetrieveNextRequest in arrival order at PASSIVE_LEVEL. */
    KSPIN_LOCK WorkLock;         /* guards WorkerStop + PendingWrites + forward */
    WDFQUEUE WorkQueue;          /* manual queue owned by the worker */
    KEVENT WorkEvent;            /* SynchronizationEvent, set after each forward */
    BOOLEAN WorkerStop;          /* set under WorkLock; no new work accepted */
    LONG PendingWrites;          /* writes forwarded and not yet finished by worker */
    PKTHREAD WorkerThread;       /* referenced */
    volatile LONG64 BypassReads; /* stats: reads sent straight down */
    volatile LONG64 WorkerReads; /* stats: reads served by the worker */
    WDFREQUEST LowerRequest;     /* preallocated, reused for every lower I/O */
    BOOLEAN CowTornDown;         /* was frozen, now torn down: fail I/O, never pass through */
    ULONG CowRefuseReason;      /* OF_REFUSE_*: why freeze was refused (queryable) */
    ULONG CowInitStep;          /* OF_COW_INIT_STEP: how far InitializeCow got */
    NTSTATUS CowInitStatus;     /* NTSTATUS of last InitializeCow failure (0=ok) */
    BOOLEAN OverlayFullLogged;
    OF_U64 OverlayFullCount;

    /* Overlay config sector (file backend). */
    ULONG CfgValid;
    ULONG ThawNext;
    ULONG CfgSeq;
    ULONG ThawedForN;            /* remaining ThawedForN boots (0 = none/sticky) */

    /* Phase 4: at most one pended GET_OVERLAY_EVENT waiter. */
    WDFREQUEST OverlayEventWaiter;
    KSPIN_LOCK OverlayEventLock;

    /* Deferred CoW init thread (poll until volume unlocked, then InitializeCow). */
    PKTHREAD CowInitThread;
    BOOLEAN CowInitDeferredStarted;
    volatile BOOLEAN CowInitCancel; /* ReleaseHardware asks deferred thread to exit */

    /* Passthrough ranges (pagefile/swapfile/hiberfil/bootstat extents).
       Changed only while not frozen (under CowLock); read lock-free while
       frozen. A request is passed through only if it lies wholly inside one
       range and none of its blocks is mapped (the map always wins). */
    OF_RANGE_LIST Passthrough;
    BOOLEAN PassthroughSetByIoctl;   /* IOCTL list overrides the registry */
    volatile LONG64 PassthroughWrites;
    volatile LONG64 PassthroughReads;

    /* Preallocated so the worker never allocates on the lower I/O path. */
    PMDL ScratchMdl;             /* describes ScratchBlock (non-paged) */
    PMDL PartialMdl;             /* target for IoBuildPartialMdl, OF_MAX_LOWER_RUN */

    /* Block-map L2 reserve: used first for L2 tables so mapping a block does
       not depend on pool allocation under memory pressure. */
    PUCHAR L2Reserve;
    OF_U32 L2ReserveTop;         /* entries left in L2ReserveFree */
    OF_U32 L2ReserveUsed;
    OF_U32 L2ReserveFree[OF_L2_RESERVE_COUNT];
} DEVICE_CONTEXT, *PDEVICE_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(DEVICE_CONTEXT, OfFilterGetDeviceContext);

EVT_WDF_DEVICE_PREPARE_HARDWARE OfFilterEvtDevicePrepareHardware;
EVT_WDF_DEVICE_RELEASE_HARDWARE OfFilterEvtDeviceReleaseHardware;

NTSTATUS
OfFilterInitializeCow(
    _In_ WDFDEVICE Device
    );

KSTART_ROUTINE OfFilterCowWorker;
KSTART_ROUTINE OfFilterDeferredCowInit;

// Non-paged (Io.c). Callers in PAGE code must use this instead of taking WorkLock.
VOID
OfFilterSetWorkerStop(
    _In_ PDEVICE_CONTEXT DeviceCtx,
    _In_ BOOLEAN Stop
    );

/* PASSIVE only. Normalizes Raw against the volume size and replaces the
   device list. Fails with STATUS_INVALID_DEVICE_STATE while frozen. */
NTSTATUS
OfFilterSetPassthroughRanges(
    _In_ WDFDEVICE Device,
    _In_reads_opt_(Count) const OF_RANGE *Raw,
    _In_ ULONG Count
    );

VOID
OfFilterTeardownCow(
    _In_ WDFDEVICE Device
    );

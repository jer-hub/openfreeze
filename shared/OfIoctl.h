/*++

Module Name:

    OfIoctl.h

Abstract:

    Shared IOCTL codes and structures for OfFilter.
    Keep the C# mirrors (ofquery / ofctl / OpenFreeze.Core) in sync.

--*/

#pragma once

#ifdef _KERNEL_MODE
#include <ntddk.h>
#else
#include <windows.h>
#include <winioctl.h>
#endif

/* Historical / volume path (\\.\C:) and control device (\\.\OfFilter). */
#define OF_IOCTL_DEVICE_TYPE 0x8F4Fu

#define IOCTL_OF_DEBUG_ENABLE_COW \
    CTL_CODE(OF_IOCTL_DEVICE_TYPE, 0x901, METHOD_BUFFERED, FILE_WRITE_ACCESS)
#define IOCTL_OF_DEBUG_QUERY_COW \
    CTL_CODE(OF_IOCTL_DEVICE_TYPE, 0x902, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_OF_SET_PASSTHROUGH_RANGES \
    CTL_CODE(OF_IOCTL_DEVICE_TYPE, 0x903, METHOD_BUFFERED, FILE_WRITE_ACCESS)
#define IOCTL_OF_SET_THAW_NEXT_BOOT \
    CTL_CODE(OF_IOCTL_DEVICE_TYPE, 0x904, METHOD_BUFFERED, FILE_WRITE_ACCESS)

/* Phase 4 control-device codes (same device type, 0xA0x function range). */
#define IOCTL_OF_GET_STATUS \
    CTL_CODE(OF_IOCTL_DEVICE_TYPE, 0xA01, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_OF_SET_NEXT_BOOT_MODE \
    CTL_CODE(OF_IOCTL_DEVICE_TYPE, 0xA02, METHOD_BUFFERED, FILE_WRITE_ACCESS)
#define IOCTL_OF_GET_OVERLAY_EVENT \
    CTL_CODE(OF_IOCTL_DEVICE_TYPE, 0xA03, METHOD_BUFFERED, FILE_ANY_ACCESS)

/* OF_SET_NEXT_BOOT_MODE input */
#define OF_MODE_FROZEN       0u
#define OF_MODE_THAWED       1u
#define OF_MODE_THAWED_FOR_N 2u

#pragma pack(push, 4)
typedef struct _OF_STATUS_V1 {
    ULONG CowEnabled;       /* 1 = frozen this boot */
    ULONG CowTornDown;
    ULONG SlotsUsed;
    ULONG SlotCount;        /* configured / derived */
    ULONG OverlayBackend;   /* 0=RAM 1=File */
    ULONG RefuseReason;     /* OF_REFUSE_* */
    ULONG CfgValid;
    ULONG ThawNext;
    ULONG CfgSeq;
    ULONG ThawedForN;       /* remaining ThawedForN boots (config Reserved0) */
    ULONG InitStep;
    ULONG InitStatus;       /* NTSTATUS */
} OF_STATUS_V1, *POF_STATUS_V1;

typedef struct _OF_SET_NEXT_BOOT_MODE_IN {
    ULONG Mode;             /* OF_MODE_* */
    ULONG ThawedForN;       /* used when Mode == OF_MODE_THAWED_FOR_N */
} OF_SET_NEXT_BOOT_MODE_IN, *POF_SET_NEXT_BOOT_MODE_IN;

#define OF_OVERLAY_EVENT_NONE       0u
#define OF_OVERLAY_EVENT_NEAR_FULL  1u
#define OF_OVERLAY_EVENT_FULL       2u

typedef struct _OF_OVERLAY_EVENT_V1 {
    ULONG Event;            /* OF_OVERLAY_EVENT_* */
    ULONG SlotsUsed;
    ULONG SlotCount;
} OF_OVERLAY_EVENT_V1, *POF_OVERLAY_EVENT_V1;
#pragma pack(pop)

#define OFFILTER_CONTROL_DEVICE_NAME L"\\Device\\OfFilter"
#define OFFILTER_CONTROL_SYMLINK_NAME L"\\DosDevices\\OfFilter"
#define OFFILTER_CONTROL_WIN32_NAME  L"\\\\.\\OfFilter"

/* SYSTEM-only control device SDDL (Phase 4). */
#define OFFILTER_CONTROL_SDDL_STRING L"D:P(A;;GA;;;SY)"

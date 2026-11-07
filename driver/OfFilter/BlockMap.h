/*++

Module Name:

    BlockMap.h

Abstract:

    Sparse two-level block->slot map and slot bitmap allocator.
    Pure logic: no kernel APIs. Kernel and user-mode tests supply allocators.

--*/

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_KERNEL_MODE)
#include <ntddk.h>
typedef ULONG OF_U32;
typedef ULONGLONG OF_U64;
typedef SIZE_T OF_SIZE;
#else
#include <windows.h>
typedef UINT32 OF_U32;
typedef UINT64 OF_U64;
typedef SIZE_T OF_SIZE;
#ifndef BOOLEAN
typedef unsigned char BOOLEAN;
#endif
#ifndef TRUE
#define TRUE 1
#define FALSE 0
#endif
#endif

#define OF_BLOCK_SHIFT          12u
#define OF_BLOCK_SIZE           (1u << OF_BLOCK_SHIFT)   /* 4096 */
#define OF_BLOCK_MAP_L2_ENTRIES 512u
#define OF_SLOT_INVALID         0xFFFFFFFFu

typedef enum _OF_STATUS {
    OfStatusOk = 0,
    OfStatusNoMemory = 1,
    OfStatusNotFound = 2,
    OfStatusFull = 3,
    OfStatusInvalidParameter = 4
} OF_STATUS;

typedef PVOID (*OF_ALLOC_FN)(_In_ OF_SIZE Size, _In_opt_ PVOID AllocContext);
typedef VOID (*OF_FREE_FN)(_In_opt_ PVOID Pointer, _In_opt_ PVOID AllocContext);

typedef struct _OF_BLOCK_MAP {
    OF_U32 MaxBlocks;
    OF_U32 SlotCount;
    OF_U32 SlotsUsed;
    OF_U32 L1Count;
    OF_U32 **L1;                 /* L1[i] -> L2 array of OF_U32 slot indices */
    OF_U32 *SlotBitmap;          /* bit i set => slot i allocated */
    OF_U32 SlotBitmapDwords;
    OF_U32 SearchHint;           /* first bitmap dword that may have a free bit */
    OF_ALLOC_FN Alloc;
    OF_FREE_FN Free;
    PVOID AllocContext;
} OF_BLOCK_MAP, *POF_BLOCK_MAP;

OF_STATUS
OfBlockMapInitialize(
    _Out_ POF_BLOCK_MAP Map,
    _In_ OF_U32 MaxBlocks,
    _In_ OF_U32 SlotCount,
    _In_ OF_ALLOC_FN Alloc,
    _In_ OF_FREE_FN Free,
    _In_opt_ PVOID AllocContext
    );

VOID
OfBlockMapDestroy(
    _Inout_ POF_BLOCK_MAP Map
    );

/* Returns OfStatusOk with *Slot, OfStatusNotFound if unmapped. */
OF_STATUS
OfBlockMapLookup(
    _In_ POF_BLOCK_MAP Map,
    _In_ OF_U32 BlockIndex,
    _Out_ OF_U32 *Slot
    );

/*
 * Ensure BlockIndex has a slot. If already mapped, returns existing.
 * On success *Created is TRUE when a new slot was allocated.
 */
OF_STATUS
OfBlockMapMapBlock(
    _Inout_ POF_BLOCK_MAP Map,
    _In_ OF_U32 BlockIndex,
    _Out_ OF_U32 *Slot,
    _Out_ BOOLEAN *Created
    );

/*
 * Remove the mapping for BlockIndex and release its slot. Used to roll back
 * a freshly created mapping when filling the slot failed, so a mapped block
 * never points at a slot with undefined content.
 * Returns OfStatusNotFound if the block was not mapped.
 */
OF_STATUS
OfBlockMapUnmapBlock(
    _Inout_ POF_BLOCK_MAP Map,
    _In_ OF_U32 BlockIndex
    );

/*
 * Atomic-write precheck for blocks [FirstBlock, LastBlock]: makes sure every
 * L2 table the range needs exists and that enough free slots remain for all
 * of its currently unmapped blocks. Changes no mapping and allocates no slot.
 * Returns OfStatusFull (not enough free slots), OfStatusNoMemory (an L2 table
 * could not be allocated) or OfStatusOk with *Needed = unmapped block count.
 * After OfStatusOk, mapping every block of the range cannot fail.
 */
OF_STATUS
OfBlockMapPrepareRange(
    _Inout_ POF_BLOCK_MAP Map,
    _In_ OF_U32 FirstBlock,
    _In_ OF_U32 LastBlock,
    _Out_ OF_U32 *Needed
    );

OF_U32
OfBlockMapSlotsUsed(
    _In_ POF_BLOCK_MAP Map
    );

#ifdef __cplusplus
}
#endif

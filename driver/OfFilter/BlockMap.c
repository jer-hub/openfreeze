/*++

Module Name:

    BlockMap.c

Abstract:

    Sparse two-level block map and first-fit slot bitmap. No kernel calls.

--*/

#include "BlockMap.h"

static
OF_U32 *
OfBlockMapEnsureL2(
    _Inout_ POF_BLOCK_MAP Map,
    _In_ OF_U32 L1Index
    )
{
    OF_U32 *l2;
    OF_SIZE bytes;

    if (L1Index >= Map->L1Count) {
        return NULL;
    }

    l2 = Map->L1[L1Index];
    if (l2 != NULL) {
        return l2;
    }

    bytes = (OF_SIZE)OF_BLOCK_MAP_L2_ENTRIES * sizeof(OF_U32);
    l2 = (OF_U32 *)Map->Alloc(bytes, Map->AllocContext);
    if (l2 == NULL) {
        return NULL;
    }

    {
        OF_U32 i;
        for (i = 0; i < OF_BLOCK_MAP_L2_ENTRIES; ++i) {
            l2[i] = OF_SLOT_INVALID;
        }
    }

    Map->L1[L1Index] = l2;
    return l2;
}

static
OF_STATUS
OfBlockMapAllocSlot(
    _Inout_ POF_BLOCK_MAP Map,
    _Out_ OF_U32 *Slot
    )
{
    OF_U32 dwordIndex;
    OF_U32 bit;

    if (Map->SlotsUsed >= Map->SlotCount) {
        return OfStatusFull;
    }

    for (dwordIndex = Map->SearchHint; dwordIndex < Map->SlotBitmapDwords; ++dwordIndex) {
        OF_U32 bits = Map->SlotBitmap[dwordIndex];
        if (bits == 0xFFFFFFFFu) {
            continue;
        }

        for (bit = 0; bit < 32u; ++bit) {
            OF_U32 mask = 1u << bit;
            OF_U32 slot = dwordIndex * 32u + bit;
            if (slot >= Map->SlotCount) {
                return OfStatusFull;
            }
            if ((bits & mask) == 0) {
                Map->SlotBitmap[dwordIndex] = bits | mask;
                Map->SlotsUsed += 1;
                Map->SearchHint = dwordIndex;
                *Slot = slot;
                return OfStatusOk;
            }
        }
    }

    return OfStatusFull;
}

OF_STATUS
OfBlockMapInitialize(
    _Out_ POF_BLOCK_MAP Map,
    _In_ OF_U32 MaxBlocks,
    _In_ OF_U32 SlotCount,
    _In_ OF_ALLOC_FN Alloc,
    _In_ OF_FREE_FN Free,
    _In_opt_ PVOID AllocContext
    )
{
    OF_SIZE l1Bytes;
    OF_SIZE bitmapBytes;
    OF_U32 i;

    if (Map == NULL || Alloc == NULL || Free == NULL ||
        MaxBlocks == 0 || SlotCount == 0) {
        return OfStatusInvalidParameter;
    }

    Map->MaxBlocks = MaxBlocks;
    Map->SlotCount = SlotCount;
    Map->SlotsUsed = 0;
    Map->L1Count = (MaxBlocks + OF_BLOCK_MAP_L2_ENTRIES - 1u) / OF_BLOCK_MAP_L2_ENTRIES;
    Map->L1 = NULL;
    Map->SlotBitmap = NULL;
    Map->SlotBitmapDwords = (SlotCount + 31u) / 32u;
    Map->SearchHint = 0;
    Map->Alloc = Alloc;
    Map->Free = Free;
    Map->AllocContext = AllocContext;

    l1Bytes = (OF_SIZE)Map->L1Count * sizeof(OF_U32 *);
    Map->L1 = (OF_U32 **)Alloc(l1Bytes, AllocContext);
    if (Map->L1 == NULL) {
        return OfStatusNoMemory;
    }
    for (i = 0; i < Map->L1Count; ++i) {
        Map->L1[i] = NULL;
    }

    bitmapBytes = (OF_SIZE)Map->SlotBitmapDwords * sizeof(OF_U32);
    Map->SlotBitmap = (OF_U32 *)Alloc(bitmapBytes, AllocContext);
    if (Map->SlotBitmap == NULL) {
        Free(Map->L1, AllocContext);
        Map->L1 = NULL;
        return OfStatusNoMemory;
    }
    for (i = 0; i < Map->SlotBitmapDwords; ++i) {
        Map->SlotBitmap[i] = 0;
    }

    return OfStatusOk;
}

VOID
OfBlockMapDestroy(
    _Inout_ POF_BLOCK_MAP Map
    )
{
    OF_U32 i;

    if (Map == NULL || Map->Free == NULL) {
        return;
    }

    if (Map->L1 != NULL) {
        for (i = 0; i < Map->L1Count; ++i) {
            if (Map->L1[i] != NULL) {
                Map->Free(Map->L1[i], Map->AllocContext);
                Map->L1[i] = NULL;
            }
        }
        Map->Free(Map->L1, Map->AllocContext);
        Map->L1 = NULL;
    }

    if (Map->SlotBitmap != NULL) {
        Map->Free(Map->SlotBitmap, Map->AllocContext);
        Map->SlotBitmap = NULL;
    }

    Map->MaxBlocks = 0;
    Map->SlotCount = 0;
    Map->SlotsUsed = 0;
    Map->L1Count = 0;
}

OF_STATUS
OfBlockMapLookup(
    _In_ POF_BLOCK_MAP Map,
    _In_ OF_U32 BlockIndex,
    _Out_ OF_U32 *Slot
    )
{
    OF_U32 l1Index;
    OF_U32 l2Index;
    OF_U32 *l2;

    if (Map == NULL || Slot == NULL || BlockIndex >= Map->MaxBlocks) {
        return OfStatusInvalidParameter;
    }

    l1Index = BlockIndex / OF_BLOCK_MAP_L2_ENTRIES;
    l2Index = BlockIndex % OF_BLOCK_MAP_L2_ENTRIES;
    l2 = Map->L1[l1Index];
    if (l2 == NULL || l2[l2Index] == OF_SLOT_INVALID) {
        return OfStatusNotFound;
    }

    *Slot = l2[l2Index];
    return OfStatusOk;
}

OF_STATUS
OfBlockMapMapBlock(
    _Inout_ POF_BLOCK_MAP Map,
    _In_ OF_U32 BlockIndex,
    _Out_ OF_U32 *Slot,
    _Out_ BOOLEAN *Created
    )
{
    OF_U32 l1Index;
    OF_U32 l2Index;
    OF_U32 *l2;
    OF_U32 newSlot;
    OF_STATUS status;

    if (Map == NULL || Slot == NULL || Created == NULL ||
        BlockIndex >= Map->MaxBlocks) {
        return OfStatusInvalidParameter;
    }

    *Created = FALSE;
    l1Index = BlockIndex / OF_BLOCK_MAP_L2_ENTRIES;
    l2Index = BlockIndex % OF_BLOCK_MAP_L2_ENTRIES;

    l2 = OfBlockMapEnsureL2(Map, l1Index);
    if (l2 == NULL) {
        return OfStatusNoMemory;
    }

    if (l2[l2Index] != OF_SLOT_INVALID) {
        *Slot = l2[l2Index];
        return OfStatusOk;
    }

    status = OfBlockMapAllocSlot(Map, &newSlot);
    if (status != OfStatusOk) {
        return status;
    }

    l2[l2Index] = newSlot;
    *Slot = newSlot;
    *Created = TRUE;
    return OfStatusOk;
}

OF_STATUS
OfBlockMapUnmapBlock(
    _Inout_ POF_BLOCK_MAP Map,
    _In_ OF_U32 BlockIndex
    )
{
    OF_U32 *l2;
    OF_U32 l2Index;
    OF_U32 slot;
    OF_U32 dwordIndex;

    if (Map == NULL || Map->L1 == NULL || BlockIndex >= Map->MaxBlocks) {
        return OfStatusInvalidParameter;
    }

    l2 = Map->L1[BlockIndex / OF_BLOCK_MAP_L2_ENTRIES];
    l2Index = BlockIndex % OF_BLOCK_MAP_L2_ENTRIES;
    if (l2 == NULL || l2[l2Index] == OF_SLOT_INVALID) {
        return OfStatusNotFound;
    }

    slot = l2[l2Index];
    l2[l2Index] = OF_SLOT_INVALID;

    if (slot < Map->SlotCount) {
        dwordIndex = slot / 32u;
        if ((Map->SlotBitmap[dwordIndex] & (1u << (slot % 32u))) != 0) {
            Map->SlotBitmap[dwordIndex] &= ~(1u << (slot % 32u));
            Map->SlotsUsed -= 1;
            if (dwordIndex < Map->SearchHint) {
                Map->SearchHint = dwordIndex;
            }
        }
    }

    return OfStatusOk;
}

OF_U32
OfBlockMapSlotsUsed(
    _In_ POF_BLOCK_MAP Map
    )
{
    if (Map == NULL) {
        return 0;
    }
    return Map->SlotsUsed;
}


OF_STATUS
OfBlockMapPrepareRange(
    _Inout_ POF_BLOCK_MAP Map,
    _In_ OF_U32 FirstBlock,
    _In_ OF_U32 LastBlock,
    _Out_ OF_U32 *Needed
    )
{
    OF_U32 block;
    OF_U32 need = 0;
    OF_U32 l1;

    if (Map == NULL || Needed == NULL || Map->L1 == NULL ||
        FirstBlock > LastBlock || LastBlock >= Map->MaxBlocks) {
        return OfStatusInvalidParameter;
    }
    *Needed = 0;

    for (l1 = FirstBlock / OF_BLOCK_MAP_L2_ENTRIES; l1 <= LastBlock / OF_BLOCK_MAP_L2_ENTRIES; ++l1) {
        if (OfBlockMapEnsureL2(Map, l1) == NULL) {
            return OfStatusNoMemory;
        }
    }

    for (block = FirstBlock; ; ++block) {
        if (Map->L1[block / OF_BLOCK_MAP_L2_ENTRIES][block % OF_BLOCK_MAP_L2_ENTRIES] == OF_SLOT_INVALID) {
            need += 1;
        }
        if (block == LastBlock) {
            break;
        }
    }

    *Needed = need;
    if (need > Map->SlotCount - Map->SlotsUsed) {
        return OfStatusFull;
    }
    return OfStatusOk;
}

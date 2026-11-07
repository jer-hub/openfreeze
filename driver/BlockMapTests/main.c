/*++

Module Name:

    main.c

Abstract:

    User-mode unit tests for BlockMap.c (no kernel APIs).

--*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "..\OfFilter\BlockMap.h"
#include "..\OfFilter\RangeList.h"
#include "..\OfFilter\OverlayMap.h"
#include "..\OfFilter\OfConfig.h"

static int g_Failures = 0;

#define TEST_CHECK(cond) \
    do { \
        if (!(cond)) { \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            g_Failures++; \
        } \
    } while (0)

static
PVOID
TestAlloc(
    _In_ OF_SIZE Size,
    _In_opt_ PVOID AllocContext
    )
{
    UNREFERENCED_PARAMETER(AllocContext);
    return malloc(Size);
}

static
VOID
TestFree(
    _In_opt_ PVOID Pointer,
    _In_opt_ PVOID AllocContext
    )
{
    UNREFERENCED_PARAMETER(AllocContext);
    free(Pointer);
}

static
void
TestLookupMissAndMap(void)
{
    OF_BLOCK_MAP map;
    OF_U32 slot = 0;
    BOOLEAN created = FALSE;
    OF_STATUS status;

    status = OfBlockMapInitialize(&map, 10000, 64, TestAlloc, TestFree, NULL);
    TEST_CHECK(status == OfStatusOk);

    status = OfBlockMapLookup(&map, 42, &slot);
    TEST_CHECK(status == OfStatusNotFound);

    status = OfBlockMapMapBlock(&map, 42, &slot, &created);
    TEST_CHECK(status == OfStatusOk);
    TEST_CHECK(created == TRUE);
    TEST_CHECK(slot == 0);

    created = FALSE;
    status = OfBlockMapMapBlock(&map, 42, &slot, &created);
    TEST_CHECK(status == OfStatusOk);
    TEST_CHECK(created == FALSE);
    TEST_CHECK(slot == 0);

    status = OfBlockMapLookup(&map, 42, &slot);
    TEST_CHECK(status == OfStatusOk);
    TEST_CHECK(slot == 0);
    TEST_CHECK(OfBlockMapSlotsUsed(&map) == 1);

    OfBlockMapDestroy(&map);
}

static
void
TestSparseAcrossL2(void)
{
    OF_BLOCK_MAP map;
    OF_U32 slotA = 0;
    OF_U32 slotB = 0;
    BOOLEAN created = FALSE;
    OF_STATUS status;
    OF_U32 farBlock = OF_BLOCK_MAP_L2_ENTRIES + 7;

    status = OfBlockMapInitialize(&map, farBlock + 10, 16, TestAlloc, TestFree, NULL);
    TEST_CHECK(status == OfStatusOk);

    status = OfBlockMapMapBlock(&map, 3, &slotA, &created);
    TEST_CHECK(status == OfStatusOk);

    created = FALSE;
    status = OfBlockMapMapBlock(&map, farBlock, &slotB, &created);
    TEST_CHECK(status == OfStatusOk);
    TEST_CHECK(created == TRUE);
    TEST_CHECK(slotA != slotB);
    TEST_CHECK(OfBlockMapSlotsUsed(&map) == 2);

    OfBlockMapDestroy(&map);
}

static
void
TestSlotExhaustion(void)
{
    OF_BLOCK_MAP map;
    OF_U32 slot = 0;
    BOOLEAN created = FALSE;
    OF_STATUS status;
    OF_U32 i;

    status = OfBlockMapInitialize(&map, 100, 4, TestAlloc, TestFree, NULL);
    TEST_CHECK(status == OfStatusOk);

    for (i = 0; i < 4; ++i) {
        created = FALSE;
        status = OfBlockMapMapBlock(&map, i, &slot, &created);
        TEST_CHECK(status == OfStatusOk);
        TEST_CHECK(created == TRUE);
    }

    created = FALSE;
    status = OfBlockMapMapBlock(&map, 50, &slot, &created);
    TEST_CHECK(status == OfStatusFull);
    TEST_CHECK(OfBlockMapSlotsUsed(&map) == 4);

    OfBlockMapDestroy(&map);
}

static
void
TestUnmapAndReuse(void)
{
    OF_BLOCK_MAP map;
    OF_U32 slot = 0;
    OF_U32 slot2 = 0;
    BOOLEAN created = FALSE;
    OF_STATUS status;
    OF_U32 i;

    status = OfBlockMapInitialize(&map, 1000, 40, TestAlloc, TestFree, NULL);
    TEST_CHECK(status == OfStatusOk);

    for (i = 0; i < 40; ++i) {
        status = OfBlockMapMapBlock(&map, i * 7, &slot, &created);
        TEST_CHECK(status == OfStatusOk && created == TRUE && slot == i);
    }
    TEST_CHECK(OfBlockMapMapBlock(&map, 999, &slot, &created) == OfStatusFull);

    /* Unmap a slot in the first bitmap dword; next allocation must reuse it. */
    TEST_CHECK(OfBlockMapLookup(&map, 5 * 7, &slot) == OfStatusOk && slot == 5);
    TEST_CHECK(OfBlockMapUnmapBlock(&map, 5 * 7) == OfStatusOk);
    TEST_CHECK(OfBlockMapLookup(&map, 5 * 7, &slot) == OfStatusNotFound);
    TEST_CHECK(OfBlockMapSlotsUsed(&map) == 39);
    TEST_CHECK(OfBlockMapUnmapBlock(&map, 5 * 7) == OfStatusNotFound);
    TEST_CHECK(OfBlockMapUnmapBlock(&map, 3) == OfStatusNotFound);
    TEST_CHECK(OfBlockMapUnmapBlock(&map, 1000) == OfStatusInvalidParameter);

    status = OfBlockMapMapBlock(&map, 999, &slot2, &created);
    TEST_CHECK(status == OfStatusOk && created == TRUE && slot2 == 5);
    TEST_CHECK(OfBlockMapSlotsUsed(&map) == 40);

    /* Other mappings untouched. */
    TEST_CHECK(OfBlockMapLookup(&map, 39 * 7, &slot) == OfStatusOk && slot == 39);
    OfBlockMapDestroy(&map);
}

static
void
TestBoundsAndInvalid(void)
{
    OF_BLOCK_MAP map;
    OF_U32 slot = 0;
    BOOLEAN created = FALSE;

    TEST_CHECK(OfBlockMapInitialize(&map, 0, 4, TestAlloc, TestFree, NULL) == OfStatusInvalidParameter);
    TEST_CHECK(OfBlockMapInitialize(&map, 4, 0, TestAlloc, TestFree, NULL) == OfStatusInvalidParameter);

    /* MaxBlocks not a multiple of L2 size: last block valid, one past is not. */
    TEST_CHECK(OfBlockMapInitialize(&map, OF_BLOCK_MAP_L2_ENTRIES * 2 + 3, 8, TestAlloc, TestFree, NULL) == OfStatusOk);
    TEST_CHECK(OfBlockMapMapBlock(&map, OF_BLOCK_MAP_L2_ENTRIES * 2 + 2, &slot, &created) == OfStatusOk);
    TEST_CHECK(OfBlockMapMapBlock(&map, OF_BLOCK_MAP_L2_ENTRIES * 2 + 3, &slot, &created) == OfStatusInvalidParameter);
    TEST_CHECK(OfBlockMapLookup(&map, OF_BLOCK_MAP_L2_ENTRIES * 2 + 3, &slot) == OfStatusInvalidParameter);
    TEST_CHECK(OfBlockMapLookup(&map, 0, NULL) == OfStatusInvalidParameter);
    OfBlockMapDestroy(&map);
}

/* Slot count not a multiple of 32: allocator must not hand out bits past SlotCount. */
static
void
TestOddSlotCountExhaustion(void)
{
    OF_BLOCK_MAP map;
    OF_U32 slot = 0;
    BOOLEAN created = FALSE;
    OF_U32 i;

    TEST_CHECK(OfBlockMapInitialize(&map, 5000, 37, TestAlloc, TestFree, NULL) == OfStatusOk);
    for (i = 0; i < 37; ++i) {
        TEST_CHECK(OfBlockMapMapBlock(&map, i, &slot, &created) == OfStatusOk && slot < 37);
    }
    TEST_CHECK(OfBlockMapMapBlock(&map, 4000, &slot, &created) == OfStatusFull);
    TEST_CHECK(OfBlockMapUnmapBlock(&map, 36) == OfStatusOk);
    TEST_CHECK(OfBlockMapMapBlock(&map, 4000, &slot, &created) == OfStatusOk && slot == 36);
    OfBlockMapDestroy(&map);
}

/* Failing allocator: L2 allocation failure must not leave a mapping behind. */
static int g_FailAfter = -1;
static PVOID FailingAlloc(_In_ OF_SIZE Size, _In_opt_ PVOID Ctx)
{
    UNREFERENCED_PARAMETER(Ctx);
    if (g_FailAfter == 0) { return NULL; }
    if (g_FailAfter > 0) { g_FailAfter--; }
    return malloc(Size);
}

static
void
TestAllocFailure(void)
{
    OF_BLOCK_MAP map;
    OF_U32 slot = 0;
    BOOLEAN created = FALSE;

    g_FailAfter = 2; /* L1 + bitmap succeed, first L2 fails */
    TEST_CHECK(OfBlockMapInitialize(&map, 10000, 8, FailingAlloc, TestFree, NULL) == OfStatusOk);
    TEST_CHECK(OfBlockMapMapBlock(&map, 10, &slot, &created) == OfStatusNoMemory);
    TEST_CHECK(OfBlockMapSlotsUsed(&map) == 0);
    TEST_CHECK(OfBlockMapLookup(&map, 10, &slot) == OfStatusNotFound);
    g_FailAfter = -1;
    TEST_CHECK(OfBlockMapMapBlock(&map, 10, &slot, &created) == OfStatusOk && created == TRUE);
    OfBlockMapDestroy(&map);

    g_FailAfter = 1; /* bitmap allocation fails */
    TEST_CHECK(OfBlockMapInitialize(&map, 10000, 8, FailingAlloc, TestFree, NULL) == OfStatusNoMemory);
    g_FailAfter = -1;
}

static OF_RANGE_LIST g_List;

static
void
TestRangeListNormalize(void)
{
    OF_RANGE raw[4];

    /* Unaligned edges shrink inward; overlapping and adjacent ranges merge. */
    raw[0].Offset = 4096 * 10 + 100; raw[0].Length = 4096 * 4;        /* -> [11,14) blocks */
    raw[1].Offset = 4096 * 14;       raw[1].Length = 4096 * 2;        /* adjacent -> [11,16) */
    raw[2].Offset = 4096 * 2;        raw[2].Length = 4096;            /* [2,3) */
    raw[3].Offset = 4096 * 5 + 1;    raw[3].Length = 4000;            /* no whole block: dropped */
    TEST_CHECK(OfRangeListBuild(&g_List, raw, 4, 4096ull * 1000) == OfStatusOk);
    TEST_CHECK(g_List.Count == 2);
    TEST_CHECK(g_List.Ranges[0].Offset == 4096 * 2 && g_List.Ranges[0].Length == 4096);
    TEST_CHECK(g_List.Ranges[1].Offset == 4096 * 11 && g_List.Ranges[1].Length == 4096 * 5);

    /* Clip to volume size; range past the end dropped. */
    raw[0].Offset = 4096 * 8; raw[0].Length = 4096 * 100;
    raw[1].Offset = 4096 * 50; raw[1].Length = 4096;
    TEST_CHECK(OfRangeListBuild(&g_List, raw, 2, 4096 * 10 + 512) == OfStatusOk);
    TEST_CHECK(g_List.Count == 1 && g_List.Ranges[0].Offset == 4096 * 8 && g_List.Ranges[0].Length == 4096 * 2);

    TEST_CHECK(OfRangeListBuild(&g_List, NULL, 0, 4096) == OfStatusOk && g_List.Count == 0);
    TEST_CHECK(OfRangeListBuild(&g_List, NULL, 1, 4096) == OfStatusInvalidParameter);
}

static
void
TestRangeListQueries(void)
{
    OF_RANGE raw[3];
    raw[0].Offset = 0x100000; raw[0].Length = 0x10000;
    raw[1].Offset = 0x300000; raw[1].Length = 0x2000;
    raw[2].Offset = 0x200000; raw[2].Length = 0x1000;
    TEST_CHECK(OfRangeListBuild(&g_List, raw, 3, 0x10000000) == OfStatusOk && g_List.Count == 3);

    TEST_CHECK(OfRangeListContains(&g_List, 0x100000, 0x10000) == TRUE);
    TEST_CHECK(OfRangeListContains(&g_List, 0x108000, 0x1000) == TRUE);
    TEST_CHECK(OfRangeListContains(&g_List, 0x10F000, 0x2000) == FALSE);   /* runs past end */
    TEST_CHECK(OfRangeListContains(&g_List, 0x0FF000, 0x2000) == FALSE);   /* starts before */
    TEST_CHECK(OfRangeListContains(&g_List, 0x200000, 0x1000) == TRUE);
    TEST_CHECK(OfRangeListContains(&g_List, 0x200000, 0x100001) == FALSE); /* spans gap */
    TEST_CHECK(OfRangeListContains(&g_List, 0x0, 0x1000) == FALSE);
    TEST_CHECK(OfRangeListContains(&g_List, 0x100000, 0) == FALSE);
    TEST_CHECK(OfRangeListContains(&g_List, ~(OF_U64)0 - 10, 100) == FALSE);

    TEST_CHECK(OfRangeListIntersects(&g_List, 0x0FF000, 0x2000) == TRUE);
    TEST_CHECK(OfRangeListIntersects(&g_List, 0x110000, 0x1000) == FALSE); /* touches end only */
    TEST_CHECK(OfRangeListIntersects(&g_List, 0x0, 0x100000) == FALSE);
    TEST_CHECK(OfRangeListIntersects(&g_List, 0x0, 0x100001) == TRUE);
    TEST_CHECK(OfRangeListIntersects(&g_List, 0x150000, 0x200000) == TRUE); /* covers middle range */
    TEST_CHECK(OfRangeListIntersects(&g_List, 0x400000, ~(OF_U64)0) == FALSE);
}

static
void
TestRangeListCapacity(void)
{
    static OF_RANGE raw[OF_MAX_PASSTHROUGH_RANGES + 8];
    OF_U32 i;
    /* Disjoint, non-adjacent: one too many fails closed (empty list). */
    for (i = 0; i < OF_MAX_PASSTHROUGH_RANGES + 1; ++i) {
        raw[i].Offset = (OF_U64)i * 0x4000; raw[i].Length = 0x1000;
    }
    TEST_CHECK(OfRangeListBuild(&g_List, raw, OF_MAX_PASSTHROUGH_RANGES + 1, 0x100000000ull) == OfStatusFull);
    TEST_CHECK(g_List.Count == 0);
    TEST_CHECK(OfRangeListBuild(&g_List, raw, OF_MAX_PASSTHROUGH_RANGES, 0x100000000ull) == OfStatusOk);
    TEST_CHECK(g_List.Count == OF_MAX_PASSTHROUGH_RANGES);
    /* Many adjacent pieces merge into one even past the raw limit. */
    for (i = 0; i < OF_MAX_PASSTHROUGH_RANGES + 8; ++i) {
        raw[i].Offset = (OF_U64)(OF_MAX_PASSTHROUGH_RANGES + 7 - i) * 0x1000; raw[i].Length = 0x1000;
    }
    TEST_CHECK(OfRangeListBuild(&g_List, raw, OF_MAX_PASSTHROUGH_RANGES + 8, 0x100000000ull) == OfStatusOk);
    TEST_CHECK(g_List.Count == 1 && g_List.Ranges[0].Offset == 0 &&
               g_List.Ranges[0].Length == (OF_U64)(OF_MAX_PASSTHROUGH_RANGES + 8) * 0x1000);
}

/* Atomic precheck: a multi-block write that does not fit changes nothing. */
static
void
TestPrepareRangeAtomic(void)
{
    OF_BLOCK_MAP map;
    OF_U32 slot = 0;
    OF_U32 need = 99;
    BOOLEAN created = FALSE;
    OF_U32 i;

    TEST_CHECK(OfBlockMapInitialize(&map, 4096, 8, TestAlloc, TestFree, NULL) == OfStatusOk);
    /* 6 used: blocks 0..5. */
    for (i = 0; i < 6; ++i) {
        TEST_CHECK(OfBlockMapMapBlock(&map, i, &slot, &created) == OfStatusOk && created == TRUE);
    }
    /* Range 4..7: 4,5 mapped, 6,7 unmapped -> needs 2, fits exactly. */
    TEST_CHECK(OfBlockMapPrepareRange(&map, 4, 7, &need) == OfStatusOk && need == 2);
    TEST_CHECK(OfBlockMapSlotsUsed(&map) == 6);
    /* Range 4..8 needs 3 > 2 free: full, nothing mapped. */
    TEST_CHECK(OfBlockMapPrepareRange(&map, 4, 8, &need) == OfStatusFull && need == 3);
    TEST_CHECK(OfBlockMapSlotsUsed(&map) == 6);
    for (i = 6; i <= 8; ++i) {
        TEST_CHECK(OfBlockMapLookup(&map, i, &slot) == OfStatusNotFound);
    }
    /* Range across an L2 boundary into an untouched table: L2 created, no slot used. */
    TEST_CHECK(OfBlockMapPrepareRange(&map, 510, 513, &need) == OfStatusFull && need == 4);
    TEST_CHECK(map.L1[1] != NULL && OfBlockMapSlotsUsed(&map) == 6);
    TEST_CHECK(OfBlockMapPrepareRange(&map, 511, 512, &need) == OfStatusOk && need == 2);
    /* After OK, mapping every block of the range succeeds. */
    TEST_CHECK(OfBlockMapMapBlock(&map, 511, &slot, &created) == OfStatusOk && created == TRUE);
    TEST_CHECK(OfBlockMapMapBlock(&map, 512, &slot, &created) == OfStatusOk && created == TRUE);
    TEST_CHECK(OfBlockMapSlotsUsed(&map) == 8);
    /* Fully mapped range needs nothing even when full. */
    TEST_CHECK(OfBlockMapPrepareRange(&map, 0, 5, &need) == OfStatusOk && need == 0);
    TEST_CHECK(OfBlockMapPrepareRange(&map, 5, 6, &need) == OfStatusFull && need == 1);
    /* Invalid arguments. */
    TEST_CHECK(OfBlockMapPrepareRange(&map, 7, 6, &need) == OfStatusInvalidParameter);
    TEST_CHECK(OfBlockMapPrepareRange(&map, 0, 4096, &need) == OfStatusInvalidParameter);
    TEST_CHECK(OfBlockMapPrepareRange(&map, 0, 1, NULL) == OfStatusInvalidParameter);
    OfBlockMapDestroy(&map);
}




static void
TestOverlayCapacityAndHeader(void)
{
    TEST_CHECK(OfOverlayComputeSlotCount(0) == 0);
    TEST_CHECK(OfOverlayComputeSlotCount(OF_OVERLAY_CONFIG_SECTOR_SIZE) == 0);
    TEST_CHECK(OfOverlayComputeSlotCount(OF_OVERLAY_CONFIG_SECTOR_SIZE + OF_BLOCK_SIZE) == 1);
    TEST_CHECK(OfOverlayComputeSlotCount(OF_OVERLAY_CONFIG_SECTOR_SIZE + 10ull * OF_BLOCK_SIZE) == 10);
    /* 8 GB default */
    {
        OF_U64 eightGb = 8ull * 1024ull * 1024ull * 1024ull;
        OF_U32 slots = OfOverlayComputeSlotCount(eightGb);
        TEST_CHECK(slots == OF_MAX_SLOT_COUNT); /* capped */
    }
}

static void
TestOverlayExtentMapping(void)
{
    OF_OVERLAY_EXTENT one;
    OF_U64 vol = 0;
    OF_U32 slots = 4;
    OF_U64 fileSize = OF_OVERLAY_CONFIG_SECTOR_SIZE + (OF_U64)slots * OF_BLOCK_SIZE;

    one.FileOffset = 0;
    one.VolumeOffset = 0x100000;
    one.Length = fileSize;

    TEST_CHECK(OfOverlayValidateExtents(&one, 1, fileSize, slots, 0) == OfStatusOk);
    TEST_CHECK(OfOverlayMapSlotToVolume(&one, 1, OF_OVERLAY_CONFIG_SECTOR_SIZE, slots, 0, &vol) == OfStatusOk);
    TEST_CHECK(vol == 0x100000 + OF_OVERLAY_CONFIG_SECTOR_SIZE);
    TEST_CHECK(OfOverlayMapSlotToVolume(&one, 1, OF_OVERLAY_CONFIG_SECTOR_SIZE, slots, 3, &vol) == OfStatusOk);
    TEST_CHECK(vol == 0x100000 + OF_OVERLAY_CONFIG_SECTOR_SIZE + 3ull * OF_BLOCK_SIZE);
    TEST_CHECK(OfOverlayMapSlotToVolume(&one, 1, OF_OVERLAY_CONFIG_SECTOR_SIZE, slots, 4, &vol) == OfStatusInvalidParameter);
}

static void
TestOverlaySplitExtentsAndValidation(void)
{
    OF_OVERLAY_EXTENT e[2];
    OF_U32 slots = 4;
    OF_U64 fileSize = OF_OVERLAY_CONFIG_SECTOR_SIZE + (OF_U64)slots * OF_BLOCK_SIZE;
    OF_U64 vol = 0;

    /* Config + first two slots in extent 0; last two slots in extent 1. */
    e[0].FileOffset = 0;
    e[0].VolumeOffset = 0x200000;
    e[0].Length = OF_OVERLAY_CONFIG_SECTOR_SIZE + 2ull * OF_BLOCK_SIZE;
    e[1].FileOffset = e[0].Length;
    e[1].VolumeOffset = 0x800000;
    e[1].Length = 2ull * OF_BLOCK_SIZE;

    TEST_CHECK(OfOverlayValidateExtents(e, 2, fileSize, slots, 0) == OfStatusOk);
    TEST_CHECK(OfOverlayMapSlotToVolume(e, 2, OF_OVERLAY_CONFIG_SECTOR_SIZE, slots, 0, &vol) == OfStatusOk);
    TEST_CHECK(vol == 0x200000 + OF_OVERLAY_CONFIG_SECTOR_SIZE);
    TEST_CHECK(OfOverlayMapSlotToVolume(e, 2, OF_OVERLAY_CONFIG_SECTOR_SIZE, slots, 2, &vol) == OfStatusOk);
    TEST_CHECK(vol == 0x800000);

    /* Overlap in file space must fail. */
    e[1].FileOffset = OF_BLOCK_SIZE;
    TEST_CHECK(OfOverlayValidateExtents(e, 2, fileSize, slots, 0) == OfStatusInvalidParameter);

    /* Misaligned volume offset must fail. */
    e[1].FileOffset = e[0].Length;
    e[1].VolumeOffset = 0x800001;
    TEST_CHECK(OfOverlayValidateExtents(e, 2, fileSize, slots, 0) == OfStatusInvalidParameter);
}


static void
TestOverlayCappedSlotsFullFileExtent(void)
{
    /* Regression for ConfigRefuse: ofoverlay writes SlotCount=OF_MAX_SLOT_COUNT
       for an 8 GB overlay.bin but stores one extent covering the whole file.
       ValidateExtents against the real file size must succeed; against the
       slot-implied size (config + slots*block ~= 1 GB) the full-file extent
       fails â€” that was OfOverlayCreateFile's old bug. */
    OF_OVERLAY_EXTENT one;
    OF_U64 eightGb = 8ull * 1024ull * 1024ull * 1024ull;
    OF_U32 slots = OfOverlayComputeSlotCount(eightGb);
    OF_U64 implied = OF_OVERLAY_CONFIG_SECTOR_SIZE + (OF_U64)slots * OF_BLOCK_SIZE;

    TEST_CHECK(slots == OF_MAX_SLOT_COUNT);
    TEST_CHECK(implied < eightGb);

    one.FileOffset = 0;
    one.VolumeOffset = 0x9B6468000ull; /* matches observed guest extent */
    one.Length = eightGb;

    TEST_CHECK(OfOverlayValidateExtents(&one, 1, eightGb, slots, 0) == OfStatusOk);
    TEST_CHECK(OfOverlayValidateExtents(&one, 1, implied, slots, 0) == OfStatusInvalidParameter);

    /* Exact capacity without cap: file = header + N blocks => N slots. */
    {
        OF_U32 n = 100;
        OF_U64 fs = OF_OVERLAY_CONFIG_SECTOR_SIZE + (OF_U64)n * OF_BLOCK_SIZE;
        TEST_CHECK(OfOverlayComputeSlotCount(fs) == n);
        one.Length = fs;
        TEST_CHECK(OfOverlayValidateExtents(&one, 1, fs, n, 0) == OfStatusOk);
    }
}

static void
TestOverlayMaxExtentsBlob(void)
{
    OF_OVERLAY_EXTENT e[OF_MAX_OVERLAY_EXTENTS];
    OF_U32 i;
    OF_U32 slots = OF_MAX_OVERLAY_EXTENTS;
    OF_U64 fileSize = OF_OVERLAY_CONFIG_SECTOR_SIZE + (OF_U64)slots * OF_BLOCK_SIZE;

    e[0].FileOffset = 0;
    e[0].VolumeOffset = 0x100000;
    e[0].Length = OF_OVERLAY_CONFIG_SECTOR_SIZE + OF_BLOCK_SIZE;
    for (i = 1; i < OF_MAX_OVERLAY_EXTENTS; ++i) {
        e[i].FileOffset = OF_OVERLAY_CONFIG_SECTOR_SIZE + (OF_U64)i * OF_BLOCK_SIZE;
        e[i].VolumeOffset = 0x100000 + e[i].FileOffset;
        e[i].Length = OF_BLOCK_SIZE;
    }

    TEST_CHECK(OfOverlayValidateExtents(e, OF_MAX_OVERLAY_EXTENTS, fileSize, slots, 0) == OfStatusOk);
    /* Registry blob size for max extents must be an exact multiple. */
    TEST_CHECK(((OF_MAX_OVERLAY_EXTENTS * sizeof(OF_OVERLAY_EXTENT)) % sizeof(OF_OVERLAY_EXTENT)) == 0);
}


/* Guest numbers from file_20261105_191831 ofoverlay_create / ofranges_set:
 * overlay extent file=0 vol=0x6E2465000 len=0x200000000 (8 GiB)
 * pagefile extent2 ends exactly where overlay begins (abut -> coalesce).
 * SlotCount capped at OF_MAX_SLOT_COUNT; slots use bytes (not LCNs/sectors)
 * starting after the 4 KB config header. */
static void
TestGuestFileBackendMapping(void)
{
    OF_OVERLAY_EXTENT one;
    OF_U64 eightGb = 8ull * 1024ull * 1024ull * 1024ull;
    OF_U32 slots = OfOverlayComputeSlotCount(eightGb);
    OF_U64 vol0, volLast, fileOffLast;
    OF_U32 s;
    OF_U64 markerish = 0x1000000ull; /* arbitrary early-volume offset, not in overlay */

    one.FileOffset = 0;
    one.VolumeOffset = 0x6E2465000ull;
    one.Length = eightGb;

    TEST_CHECK(slots == OF_MAX_SLOT_COUNT);
    TEST_CHECK(OfOverlayValidateExtents(&one, 1, eightGb, slots, 0) == OfStatusOk);

    TEST_CHECK(OfOverlayMapSlotToVolume(&one, 1, OF_OVERLAY_CONFIG_SECTOR_SIZE,
                                        slots, 0, &vol0) == OfStatusOk);
    /* slot0 file offset = 4 KB header; volume = extent.vol + 4 KB */
    TEST_CHECK(vol0 == 0x6E2465000ull + OF_OVERLAY_CONFIG_SECTOR_SIZE);
    TEST_CHECK(OfOverlayVolumeRangeInExtents(&one, 1, vol0, OF_BLOCK_SIZE) == TRUE);

    fileOffLast = OF_OVERLAY_CONFIG_SECTOR_SIZE + (OF_U64)(slots - 1u) * OF_BLOCK_SIZE;
    TEST_CHECK(OfOverlayMapSlotToVolume(&one, 1, OF_OVERLAY_CONFIG_SECTOR_SIZE,
                                        slots, slots - 1u, &volLast) == OfStatusOk);
    TEST_CHECK(volLast == one.VolumeOffset + fileOffLast);
    TEST_CHECK(OfOverlayVolumeRangeInExtents(&one, 1, volLast, OF_BLOCK_SIZE) == TRUE);

    /* Every slot lands inside overlay.bin's volume extent, never on
       "original data" locations outside it (e.g. low volume offsets). */
    for (s = 0; s < slots; s += (slots / 64u) ? (slots / 64u) : 1u) {
        OF_U64 v = 0;
        TEST_CHECK(OfOverlayMapSlotToVolume(&one, 1, OF_OVERLAY_CONFIG_SECTOR_SIZE,
                                            slots, s, &v) == OfStatusOk);
        TEST_CHECK(v >= one.VolumeOffset);
        TEST_CHECK(v + OF_BLOCK_SIZE <= one.VolumeOffset + one.Length);
        TEST_CHECK(OfOverlayVolumeRangeInExtents(&one, 1, v, OF_BLOCK_SIZE) == TRUE);
        TEST_CHECK(v != markerish);
        TEST_CHECK(OfOverlayVolumeRangeInExtents(&one, 1, markerish, OF_BLOCK_SIZE) == FALSE);
    }

    /* Outside the extent must fail the safety predicate. */
    TEST_CHECK(OfOverlayVolumeRangeInExtents(&one, 1, one.VolumeOffset - OF_BLOCK_SIZE, OF_BLOCK_SIZE) == FALSE);
    TEST_CHECK(OfOverlayVolumeRangeInExtents(&one, 1, one.VolumeOffset + one.Length, OF_BLOCK_SIZE) == FALSE);
}

/* ofranges pagefile + bootstat + overlay abut => coalesced ptRanges=3,
 * ptBytes = pagefile + bootstat + overlay (matches ofquery 10200616960). */
static void
TestGuestPassthroughOverlayAbutMerge(void)
{
    OF_RANGE raw[4];
    OF_U64 expectedBytes;
    OF_U32 i;
    OF_U64 sum = 0;

    /* Exact ranges from ofranges_set.txt of the 191831 run. */
    raw[0].Offset = 0x419E14000ull; raw[0].Length = 0x4DC6E000ull; /* pagefile 1 */
    raw[1].Offset = 0x6D00D3000ull; raw[1].Length = 0x12392000ull; /* pagefile 2 */
    raw[2].Offset = 0x3D8709000ull; raw[2].Length = 0x11000ull;    /* bootstat */
    raw[3].Offset = 0x6E2465000ull; raw[3].Length = 0x200000000ull; /* overlay */

    /* pagefile2 end == overlay start: abut merges. */
    TEST_CHECK(raw[1].Offset + raw[1].Length == raw[3].Offset);

    TEST_CHECK(OfRangeListBuild(&g_List, raw, 4, 0x4000000000ull) == OfStatusOk);
    TEST_CHECK(g_List.Count == 3);

    for (i = 0; i < g_List.Count; ++i) {
        sum += g_List.Ranges[i].Length;
    }
    expectedBytes = 0x4DC6E000ull + 0x12392000ull + 0x11000ull + 0x200000000ull;
    TEST_CHECK(sum == expectedBytes);
    TEST_CHECK(sum == 10200616960ull);

    /* Overlay volume range is covered by the merged list. */
    TEST_CHECK(OfRangeListContains(&g_List, 0x6E2465000ull, OF_BLOCK_SIZE) == TRUE);
    TEST_CHECK(OfRangeListContains(&g_List, 0x6E2465000ull + 0x100000ull, OF_BLOCK_SIZE) == TRUE);
}

static void
TestOverlayUniqueId(void)
{
    UCHAR a[] = { 1, 2, 3, 4 };
    UCHAR b[] = { 1, 2, 3, 4 };
    UCHAR c[] = { 1, 2, 3, 5 };
    TEST_CHECK(OfOverlayUniqueIdEqual(a, 4, b, 4) == TRUE);
    TEST_CHECK(OfOverlayUniqueIdEqual(a, 4, c, 4) == FALSE);
    TEST_CHECK(OfOverlayUniqueIdEqual(a, 4, b, 3) == FALSE);
    TEST_CHECK(OfOverlayUniqueIdEqual(NULL, 0, b, 4) == FALSE);
}

static
void
TestThawedForNConfig(void)
{
    unsigned char buf[4096];
    OF_U32 refuse = 0, valid = 0, thaw = 0, seq = 0, n = 0;
    OF_U32 outFlags = 0, outN = 0, outSeq = 0;
    BOOLEAN freeze;

    printf("TestThawedForNConfig\n");
    OfConfigBuild(buf, sizeof(buf), 5, OF_CONFIG_FLAG_THAW_NEXT, 2);
    freeze = OfConfigShouldFreeze(TRUE, TRUE, TRUE, buf, sizeof(buf),
                                  &refuse, &valid, &thaw, &seq, &n);
    TEST_CHECK(freeze == FALSE);
    TEST_CHECK(refuse == OF_REFUSE_THAW_REQUESTED);
    TEST_CHECK(valid == 1);
    TEST_CHECK(thaw == 1);
    TEST_CHECK(n == 2);
    TEST_CHECK(seq == 5);

    TEST_CHECK(OfConfigConsumeThawedForN(OF_CONFIG_FLAG_THAW_NEXT, 2, 5,
                                         &outFlags, &outN, &outSeq) == TRUE);
    TEST_CHECK(outN == 1);
    TEST_CHECK((outFlags & OF_CONFIG_FLAG_THAW_NEXT) != 0);
    TEST_CHECK(outSeq == 6);

    TEST_CHECK(OfConfigConsumeThawedForN(OF_CONFIG_FLAG_THAW_NEXT, 1, 6,
                                         &outFlags, &outN, &outSeq) == TRUE);
    TEST_CHECK(outN == 0);
    TEST_CHECK((outFlags & OF_CONFIG_FLAG_THAW_NEXT) == 0);
    TEST_CHECK(outSeq == 7);

    /* sticky-only: no consume */
    TEST_CHECK(OfConfigConsumeThawedForN(OF_CONFIG_FLAG_THAW_NEXT, 0, 9,
                                         &outFlags, &outN, &outSeq) == FALSE);

    /* Frozen sector */
    OfConfigBuild(buf, sizeof(buf), 1, 0, 0);
    freeze = OfConfigShouldFreeze(TRUE, TRUE, TRUE, buf, sizeof(buf),
                                  &refuse, &valid, &thaw, &seq, &n);
    TEST_CHECK(freeze == TRUE);
    TEST_CHECK(n == 0);
}

int
main(void)
{
    printf("BlockMapTests\n");
    TestLookupMissAndMap();
    TestSparseAcrossL2();
    TestSlotExhaustion();
    TestUnmapAndReuse();
    TestBoundsAndInvalid();
    TestOddSlotCountExhaustion();
    TestAllocFailure();
    TestRangeListNormalize();
    TestRangeListQueries();
    TestRangeListCapacity();
    TestPrepareRangeAtomic();
    TestOverlayCapacityAndHeader();
    TestOverlayExtentMapping();
    TestOverlaySplitExtentsAndValidation();
    TestOverlayCappedSlotsFullFileExtent();
    TestOverlayMaxExtentsBlob();
    TestGuestFileBackendMapping();
    TestGuestPassthroughOverlayAbutMerge();
    TestOverlayUniqueId();
    TestThawedForNConfig();

    if (g_Failures == 0) {
        printf("All tests passed.\n");
        return 0;
    }

    printf("%d assertion(s) failed.\n", g_Failures);
    return 1;
}


/*++

Module Name:

    RangeList.h

Abstract:

    Passthrough range list (volume byte ranges that bypass the overlay while
    frozen: pagefile.sys, swapfile.sys, hiberfil.sys, bootstat.dat).
    Pure logic: no kernel APIs, unit tested in BlockMapTests.

--*/

#pragma once

#include "BlockMap.h"

#ifdef __cplusplus
extern "C" {
#endif

#define OF_MAX_PASSTHROUGH_RANGES 1024u

typedef struct _OF_RANGE {
    OF_U64 Offset;               /* volume byte offset */
    OF_U64 Length;               /* bytes */
} OF_RANGE, *POF_RANGE;

typedef struct _OF_RANGE_LIST {
    OF_U32 Count;
    OF_U32 Reserved;
    OF_RANGE Ranges[OF_MAX_PASSTHROUGH_RANGES];   /* sorted, disjoint, 4 KB aligned */
} OF_RANGE_LIST, *POF_RANGE_LIST;

/*
 * Build a normalized list from raw (offset, length) pairs: each range is
 * shrunk inward to whole 4 KB blocks (a block shared with other data is never
 * passed through), clipped to VolumeSize, empty ranges dropped, then sorted
 * and merged. Returns OfStatusFull if more than OF_MAX_PASSTHROUGH_RANGES
 * remain after merging (List is left empty), OfStatusInvalidParameter on
 * bad arguments.
 */
OF_STATUS
OfRangeListBuild(
    _Out_ POF_RANGE_LIST List,
    _In_reads_opt_(Count) const OF_RANGE *Raw,
    _In_ OF_U32 Count,
    _In_ OF_U64 VolumeSize
    );

/* TRUE only if [Offset, Offset+Length) lies entirely inside one range. */
BOOLEAN
OfRangeListContains(
    _In_ const OF_RANGE_LIST *List,
    _In_ OF_U64 Offset,
    _In_ OF_U64 Length
    );

/* TRUE if [Offset, Offset+Length) touches any range. */
BOOLEAN
OfRangeListIntersects(
    _In_ const OF_RANGE_LIST *List,
    _In_ OF_U64 Offset,
    _In_ OF_U64 Length
    );

#ifdef __cplusplus
}
#endif

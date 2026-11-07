/*++

Module Name:

    RangeList.c

Abstract:

    Passthrough range list. Pure logic, see RangeList.h.

--*/

#include "RangeList.h"

#define OF_BLOCK_MASK ((OF_U64)OF_BLOCK_SIZE - 1u)

/* Insertion sort by offset: tiny lists, no CRT, no recursion. */
static
void
OfRangeSort(
    _Inout_updates_(Count) OF_RANGE *R,
    _In_ OF_U32 Count
    )
{
    OF_U32 i;
    for (i = 1; i < Count; ++i) {
        OF_RANGE key = R[i];
        OF_U32 j = i;
        while (j > 0 && R[j - 1].Offset > key.Offset) {
            R[j] = R[j - 1];
            --j;
        }
        R[j] = key;
    }
}

OF_STATUS
OfRangeListBuild(
    POF_RANGE_LIST List,
    const OF_RANGE *Raw,
    OF_U32 Count,
    OF_U64 VolumeSize
    )
{
    OF_U32 i;
    OF_U32 n = 0;
    OF_U32 out;

    if (List == 0) {
        return OfStatusInvalidParameter;
    }
    List->Count = 0;
    List->Reserved = 0;
    if (Count == 0) {
        return OfStatusOk;
    }
    if (Raw == 0) {
        return OfStatusInvalidParameter;
    }

    for (i = 0; i < Count; ++i) {
        OF_U64 start = Raw[i].Offset;
        OF_U64 end;

        if (Raw[i].Length == 0 || start >= VolumeSize) {
            continue;
        }
        end = (Raw[i].Length > VolumeSize - start) ? VolumeSize : start + Raw[i].Length;

        /* Shrink inward to whole blocks. */
        if (start & OF_BLOCK_MASK) {
            if (start > ~(OF_U64)0 - OF_BLOCK_SIZE) {
                continue;
            }
            start = (start + OF_BLOCK_MASK) & ~OF_BLOCK_MASK;
        }
        end &= ~OF_BLOCK_MASK;
        if (end <= start) {
            continue;
        }

        if (n >= OF_MAX_PASSTHROUGH_RANGES) {
            /* Too many before merging: sort+merge what we have to make room. */
            OF_U32 k;
            OfRangeSort(List->Ranges, n);
            out = 0;
            for (k = 0; k < n; ++k) {
                if (out > 0 && List->Ranges[k].Offset <=
                    List->Ranges[out - 1].Offset + List->Ranges[out - 1].Length) {
                    OF_U64 e = List->Ranges[k].Offset + List->Ranges[k].Length;
                    OF_U64 pe = List->Ranges[out - 1].Offset + List->Ranges[out - 1].Length;
                    if (e > pe) {
                        List->Ranges[out - 1].Length = e - List->Ranges[out - 1].Offset;
                    }
                } else {
                    List->Ranges[out++] = List->Ranges[k];
                }
            }
            n = out;
            if (n >= OF_MAX_PASSTHROUGH_RANGES) {
                List->Count = 0;
                return OfStatusFull;
            }
        }
        List->Ranges[n].Offset = start;
        List->Ranges[n].Length = end - start;
        ++n;
    }

    OfRangeSort(List->Ranges, n);
    out = 0;
    for (i = 0; i < n; ++i) {
        if (out > 0 && List->Ranges[i].Offset <=
            List->Ranges[out - 1].Offset + List->Ranges[out - 1].Length) {
            OF_U64 e = List->Ranges[i].Offset + List->Ranges[i].Length;
            OF_U64 pe = List->Ranges[out - 1].Offset + List->Ranges[out - 1].Length;
            if (e > pe) {
                List->Ranges[out - 1].Length = e - List->Ranges[out - 1].Offset;
            }
        } else {
            List->Ranges[out++] = List->Ranges[i];
        }
    }
    List->Count = out;
    return OfStatusOk;
}

/* Index of the last range with Offset <= Value, or Count if none. */
static
OF_U32
OfRangeFind(
    _In_ const OF_RANGE_LIST *List,
    _In_ OF_U64 Value
    )
{
    OF_U32 lo = 0;
    OF_U32 hi = List->Count;
    while (lo < hi) {
        OF_U32 mid = lo + (hi - lo) / 2u;
        if (List->Ranges[mid].Offset <= Value) {
            lo = mid + 1u;
        } else {
            hi = mid;
        }
    }
    return (lo == 0) ? List->Count : lo - 1u;
}

BOOLEAN
OfRangeListContains(
    const OF_RANGE_LIST *List,
    OF_U64 Offset,
    OF_U64 Length
    )
{
    OF_U32 i;
    if (List == 0 || List->Count == 0 || Length == 0 || Offset > ~(OF_U64)0 - Length) {
        return FALSE;
    }
    i = OfRangeFind(List, Offset);
    if (i == List->Count) {
        return FALSE;
    }
    return (Offset + Length <= List->Ranges[i].Offset + List->Ranges[i].Length) ? TRUE : FALSE;
}

BOOLEAN
OfRangeListIntersects(
    const OF_RANGE_LIST *List,
    OF_U64 Offset,
    OF_U64 Length
    )
{
    OF_U32 i;
    OF_U64 end;
    if (List == 0 || List->Count == 0 || Length == 0) {
        return FALSE;
    }
    end = (Offset > ~(OF_U64)0 - Length) ? ~(OF_U64)0 : Offset + Length;
    i = OfRangeFind(List, end - 1u);
    if (i == List->Count) {
        return FALSE;
    }
    return (List->Ranges[i].Offset + List->Ranges[i].Length > Offset) ? TRUE : FALSE;
}

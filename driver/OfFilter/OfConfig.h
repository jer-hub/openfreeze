/*++
Module Name: OfConfig.h
Abstract: Overlay config-sector layout, CRC32, and boot decision (no kernel APIs).
--*/

#pragma once

#include "OverlayMap.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Little-endian 'OFCF' */
#define OF_CONFIG_MAGIC           0x4643464Ful
#define OF_CONFIG_VERSION         1u
#define OF_CONFIG_FLAG_THAW_NEXT  0x00000001ul

#pragma pack(push, 1)
typedef struct _OF_CONFIG_SECTOR {
    OF_U32 Magic;
    OF_U32 Version;
    OF_U32 Size;       /* bytes covered by CRC (offset of Crc32 field) */
    OF_U32 Sequence;
    OF_U32 Flags;
    OF_U32 Reserved0;  /* ThawedForN remaining boots (0 = sticky-only / none) */
    OF_U32 Reserved1;
    OF_U32 Crc32;      /* CRC-32/IEEE over bytes [0, Size) with Crc32 zeroed */
} OF_CONFIG_SECTOR, *POF_CONFIG_SECTOR;
#pragma pack(pop)

/* RefuseReason extensions (also in Driver.h). */
#ifndef OF_REFUSE_NONE
#define OF_REFUSE_NONE            0u
#define OF_REFUSE_NOT_TARGET      1u
#define OF_REFUSE_CONFIG          2u
#endif
#define OF_REFUSE_THAW_REQUESTED  3u
#define OF_REFUSE_CONFIG_CORRUPT  4u

typedef enum _OF_CONFIG_PARSE {
    OfConfigParseAbsent = 0,   /* all-zero / no magic: today's behavior */
    OfConfigParseValid = 1,
    OfConfigParseCorrupt = 2
} OF_CONFIG_PARSE;

OF_U32
OfConfigCrc32(
    _In_reads_bytes_(Length) const void *Data,
    _In_ OF_SIZE Length
    );

/* Fill a fresh sector (zeros pad to OF_BLOCK_SIZE if BufSize >= 4096). */
void
OfConfigBuild(
    _Out_writes_bytes_(BufSize) void *Buffer,
    _In_ OF_SIZE BufSize,
    _In_ OF_U32 Sequence,
    _In_ OF_U32 Flags,
    _In_ OF_U32 ThawedForN
    );

OF_CONFIG_PARSE
OfConfigParse(
    _In_reads_bytes_(BufSize) const void *Buffer,
    _In_ OF_SIZE BufSize,
    _Out_opt_ POF_CONFIG_SECTOR OutHeader
    );

/*
 * Boot decision for file-backend freeze gate.
 * Returns TRUE if InitializeCow should proceed to enable CoW.
 * Returns FALSE to stay passthrough; *RefuseReason set to ThawRequested,
 * ConfigCorrupt, or Config (read failure).
 * Absent config + EnableCow => proceed (TRUE).
 * Non-file backend => proceed (TRUE) without consulting the sector.
 * ThawNext flag OR ThawedForN>0 => thaw this boot.
 */
BOOLEAN
OfConfigShouldFreeze(
    _In_ BOOLEAN EnableCow,
    _In_ BOOLEAN FileBackend,
    _In_ BOOLEAN ReadOk,
    _In_reads_bytes_(BufSize) const void *Buffer,
    _In_ OF_SIZE BufSize,
    _Out_ OF_U32 *RefuseReason,
    _Out_ OF_U32 *CfgValid,
    _Out_ OF_U32 *ThawNext,
    _Out_ OF_U32 *CfgSeq,
    _Out_ OF_U32 *ThawedForN
    );

/*
 * After a ThawedForN thaw boot: consume one count.
 * newN = oldN - 1 (floor 0). If newN==0 clear THAW_NEXT so the *next* boot freezes.
 * Sticky-only (oldN==0, flag set): leave unchanged (tool clears).
 * Returns TRUE if the sector should be rewritten (*OutFlags / *OutN / *OutSeq filled).
 */
BOOLEAN
OfConfigConsumeThawedForN(
    _In_ OF_U32 OldFlags,
    _In_ OF_U32 OldN,
    _In_ OF_U32 OldSeq,
    _Out_ OF_U32 *OutFlags,
    _Out_ OF_U32 *OutN,
    _Out_ OF_U32 *OutSeq
    );

#ifdef __cplusplus
}
#endif

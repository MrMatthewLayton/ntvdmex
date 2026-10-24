/* NTVDMEX -- An NTVDM replacement for Microsoft Windows
 *
 * DOS 8.3 aliases for names that have no short name.
 *
 * WHY THIS EXISTS. DOS sees one name per file, its 8.3 one. Win32 hands us that as
 * WIN32_FIND_DATA.cAlternateFileName -- when the volume keeps one. A NAS share (the s101
 * test machine's \\server\storage at Z:) and an NTFS volume with 8.3 generation turned off
 * keep none, and a long name that is not itself a legal 8.3 name ("Documentaries",
 * "Movie Featurettes") had no DOS name at all: DIR on Z: listed "Files", "Movies" and
 * "Music" and silently left the rest out. Here such an entry is given the alias Windows
 * would have made -- the first six characters that DOS allows, upper case, "~N", and the
 * first three of the extension, N counting up past every name already in the directory
 * -- and the host maps the alias back to the long name when a guest path uses it.
 *
 * A DOS_ALIAS_TABLE is one directory's assignment, built from ONE enumeration: every
 * entry is noted (DosAliasTableNote), then aliases are handed out in enumeration order
 * (DosAliasTableFinish). The same listing always gives the same aliases, so a name seen
 * in DIR is the name a later CD or TYPE resolves. Entries that have a short name, or
 * whose long name is already 8.3, are never given one and never change.
 *
 * Pure: no Win32, no host state; the caller owns the table and the enumeration.
 *
 *
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Matthew Layton
 */

#ifndef NTVDMEX_DOS_ALIAS_H
#define NTVDMEX_DOS_ALIAS_H

#include "../ntvdmex_types.h"

#define DOS_ALIAS_NAME_SIZE         13      /* "BASENAME.EXT" and its NUL */
#define DOS_ALIAS_BASE_LENGTH       8
#define DOS_ALIAS_EXTENSION_LENGTH  3
#define DOS_ALIAS_DIRECTORY_SIZE    300
#define DOS_ALIAS_MAX_TAKEN         4096    /* 8.3 names already in the directory */
#define DOS_ALIAS_MAX_NEEDY         2048    /* Entries given an alias */
#define DOS_ALIAS_POOL_SIZE         65536   /* Their long names */
#define DOS_ALIAS_MAX_NUMBER        999999  /* "~999999" still leaves one stem letter */

typedef struct _DOS_ALIAS_TABLE
{
    CHAR  Directory[DOS_ALIAS_DIRECTORY_SIZE];  /* The host directory, with its trailing '\' */
    INT   TakenCount;
    CHAR  Taken[DOS_ALIAS_MAX_TAKEN][DOS_ALIAS_NAME_SIZE];
    INT   NeedyCount;
    DWORD NeedyName[DOS_ALIAS_MAX_NEEDY];       /* Offset of the long name in Pool */
    CHAR  NeedyAlias[DOS_ALIAS_MAX_NEEDY][DOS_ALIAS_NAME_SIZE];
    DWORD PoolUsed;
    CHAR  Pool[DOS_ALIAS_POOL_SIZE];
    INT   IsOverflow;                           /* Some entries did not fit: they get no alias */
} DOS_ALIAS_TABLE, *PDOS_ALIAS_TABLE;

typedef const DOS_ALIAS_TABLE *PCDOS_ALIAS_TABLE;

/* Is `name` a legal DOS 8.3 name (either case)? "." and ".." are not names here. */
INT DosAliasIsShortName(_In_ PCSTR name);

/* Does a path component look like an alias -- a '~' followed by a digit? Cheap; a guest
 * path with none is never looked at further.
 */
INT DosAliasLooksLikeAlias(_In_ PCSTR component, _In_ INT length);

/* The alias for `longName` with number `number` ("DOCUME~1"), into `out`. */
VOID DosAliasMake(
    _In_ PCSTR longName,
    _In_ DWORD number,
    _Out_writes_(DOS_ALIAS_NAME_SIZE) PSTR out);

/* Start a table for `directory` (empty, not yet finished). */
VOID DosAliasTableBegin(_Out_ PDOS_ALIAS_TABLE table, _In_ PCSTR directory);

/* One directory entry: its long name and the short name the volume gave it ("" = none). */
VOID DosAliasTableNote(_Inout_ PDOS_ALIAS_TABLE table, _In_ PCSTR longName, _In_ PCSTR shortName);

/* Hand out the aliases, in the order the entries were noted. */
VOID DosAliasTableFinish(_Inout_ PDOS_ALIAS_TABLE table);

/* The alias given to `longName` (case-blind), or NULL if it has none. */
PCSTR DosAliasTableAliasOf(_In_ PCDOS_ALIAS_TABLE table, _In_ PCSTR longName);

/* The long name behind `alias` (case-blind, `length` characters), or NULL. */
PCSTR DosAliasTableLongOf(_In_ PCDOS_ALIAS_TABLE table, _In_ PCSTR alias, _In_ INT length);

/* Is `directory` this table's directory (case-blind)? */
INT DosAliasTableIsFor(_In_ PCDOS_ALIAS_TABLE table, _In_ PCSTR directory);

#endif /* NTVDMEX_DOS_ALIAS_H */

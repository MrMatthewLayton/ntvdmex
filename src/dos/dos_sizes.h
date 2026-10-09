/* NTVDMEX -- An NTVDM replacement for Microsoft Windows
 *
 * The sizes of DOS's name and path formats, shared by the DOS layer, the
 * LFN API and the host (#333). Defines only, so it can be included anywhere.
 *
 *
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Matthew Layton
 */

#ifndef NTVDMEX_DOS_DOS_SIZES_H
#define NTVDMEX_DOS_DOS_SIZES_H

#define DOS_FCB_NAME_SIZE           11      /* 8.3, blank-padded, no dot */
#define DOS_SHORT_NAME_SIZE         13      /* "NAME.EXT" and its NUL */
#define DOS_DOT_EXTENSION_LENGTH    4       /* ".EXT" */
#define DOS_DIRECTORY_MAX           63      /* The current directory, without "X:\" */
#define DOS_EXTENDER_ARGV0_MAX      64      /* DOS/4GW 1.97 copies argv[0] into 64 bytes */
#define DOS_LFN_PATH_BUFFER_SIZE    261     /* A path out (7147h, 7160h, 71AAh): RBIL's 261 bytes */
#define DOS_LFN_FIND_RECORD_SIZE    0x13E   /* A WIN32_FIND_DATA-shaped find record (714Eh/714Fh) */
#define DOS_LFN_FILETIME_SIZE       8       /* A FILETIME (71A7h) */

#endif /* NTVDMEX_DOS_DOS_SIZES_H */

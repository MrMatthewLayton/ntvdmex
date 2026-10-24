/* NTVDMEX -- An NTVDM replacement for Microsoft Windows
 *
 * The CD-ROM drive backed by an ISO image, presented the way MSCDEX presents one.  GH #241.
 *
 * Settings > Drives > CD-ROM: "Mount ISO Disk Image" with a path. The disc becomes a DOS
 * drive letter that INT 2Fh 15xxh reports (1500h, 150Bh, 150Ch, 150Dh ...) and that INT 21h
 * reads like any other drive.
 *
 * HOW THE LETTER IS MADE. Every INT 21h path call hands its string to Win32, which also
 * keeps each drive's current directory -- so the letter is made a real Win32 drive:
 * DefineDosDevice maps it onto a cache directory (%TEMP%\NTVDMEX-CD-<pid>), and the DOS
 * layer gives every path to HostCdromPrepare first. That creates the directories the path
 * walks through and copies the named file out of the image; a wildcard search lays out
 * the directory's entries with their sizes and dates, as sparse placeholders, and the
 * content follows when one is opened. Files are read-only, and the DOS layer refuses the
 * write-class calls (create, delete, rename, mkdir ...) with error 5.
 *   - The cost of this shape is a file's first open: it is copied out whole. A disc is
 *     read once per file per run.
 *   - The mapping is per logon session, not per process, so it is removed on every
 *     way out we own, and a stale one a crashed host left behind is removed at start.
 *
 * "A BLANK PATH = AN EMPTY DRIVE": the letter exists, MSCDEX reports it, and every read
 * answers 21, not ready.
 *
 * MSCDEX is Microsoft's "MS-DOS CD-ROM Extensions"; its INT 2Fh 15xxh interface is as
 * Ralf Brown's Interrupt List documents it (docs/inventory/mscdex.md; the spec itself is
 * not held -- docs/ref/SOURCES.md). The image is read by src/dos/dos_iso9660.c (ECMA-119).
 *
 *
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Matthew Layton
 */

#ifndef NTVDMEX_HOST_CDROM_H
#define NTVDMEX_HOST_CDROM_H

#include "host_state.h"

/* INT 2Fh AH=15h: the MSCDEX functions answered here (Ralf Brown's Interrupt List). */
#define CDROM_MULTIPLEX_ID              0x15
#define CDROM_FN_INSTALLATION_CHECK     0x00
#define CDROM_FN_COPYRIGHT_FILE         0x02
#define CDROM_FN_ABSTRACT_FILE          0x03
#define CDROM_FN_BIBLIOGRAPHIC_FILE     0x04
#define CDROM_FN_READ_VTOC              0x05
#define CDROM_FN_ABSOLUTE_READ          0x08
#define CDROM_FN_DRIVE_CHECK            0x0B
#define CDROM_FN_VERSION                0x0C
#define CDROM_FN_DRIVE_LETTERS          0x0D
#define CDROM_FN_DESCRIPTOR_PREFERENCE  0x0E
#define CDROM_FN_DIRECTORY_ENTRY        0x0F
#define CDROM_FN_DEVICE_REQUEST         0x10

#define CDROM_SIGNATURE                 0xADAD  /* 150Bh BX: MSCDEX is installed */
#define CDROM_VERSION                   0x0217  /* 150Ch BX: 2.23, the version MS-DOS 6.22 ships */
#define CDROM_DRIVE_SUPPORTED           0xFFFF  /* 150Bh AX: nonzero = a CD-ROM drive */
#define CDROM_ERR_INVALID_FUNCTION      0x01
#define CDROM_ERR_FILE_NOT_FOUND        0x02
#define CDROM_ERR_INVALID_DRIVE         0x0F    /* 15 */
#define CDROM_ERR_NOT_READY             0x15    /* 21 */
#define CDROM_DESCRIPTOR_PRIMARY        0x0100  /* 150Eh DX: DH=1 the primary descriptor, DL=0 */
#define CDROM_ENTRY_IS_ISO9660          0x0001  /* 150Fh AX: ISO 9660 (0 = High Sierra) */
#define CDROM_REQUEST_STATUS            3       /* Device request header: the status word */
#define CDROM_REQUEST_UNKNOWN_COMMAND   0x8103  /* Error | done | 03h unknown command */

/* What HostCdromInt2F did: not ours (pass through), or answered with CF clear / set. */
#define CDROM_PASS                      0
#define CDROM_DONE                      1
#define CDROM_DONE_ERROR                2

/* Mount the image the settings name, if the CD-ROM setting is "image", at Settings' letter
 * (`storedLetter`: 0 = automatic, 1..26 = A:..Z:; one Windows uses falls back). Logs what it did.
 */
VOID HostCdromMount(PSTR *cursorIo, PCSTR imagePath, INT isImageMode, DWORD storedLetter);

/* s101: the letter a mounted image takes (cdrom_policy.h DriveLetterPolicyChoose over the
 * drives Windows has now), -1 when none is free. A stored letter that is taken is logged.
 */
INT HostImageDriveLetter(DWORD storedLetter, DWORD avoidMask, PCSTR what);

/* s101: write DOS's CDS entry for `drive` (DosCdsBuild flags; 0 = the letter is gone). */
VOID HostDriveCdsWrite(INT drive, UINT flags);

/* Settings > Drives > CD-ROM changed while running: re-mount if the drive it names differs
 * from the one mounted (cdrom_policy.h), update the CDS entry and the DOS layer, log it.
 */
VOID HostCdromApply(PCSTR imagePath, INT isImageMode, DWORD storedLetter);

/* Undo the drive mapping and remove the cache. Safe to call more than once, from any path. */
VOID HostCdromUnmount(VOID);

/* The drive (0 = A:), or -1 when there is no image drive. */
INT HostCdromDrive(VOID);

/* The disc's size in 2048-byte sectors (0 = empty drive). */
DWORD HostCdromSectors(VOID);
VOID HostCdromLabel(_Out_writes_(DOS_CDROM_LABEL_SIZE) PSTR out);      /* "" with no disc */

/* DOS_MACHINE::PathPrepare: make what `path` names exist in the cache before Win32 opens it. */
VOID HostCdromPrepare(PVOID context, PCSTR path);

/* INT 2Fh with AH=15h: CDROM_PASS, CDROM_DONE or CDROM_DONE_ERROR (the caller sets CF). */
INT HostCdromInt2F(volatile BYTE *tib);

#endif

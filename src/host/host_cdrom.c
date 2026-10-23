/* NTVDMEX -- An NTVDM replacement for Microsoft Windows
 *
 * The ISO-backed CD-ROM drive and its MSCDEX interface.  GH #241.
 *
 * The function definitions of host_cdrom.h, which keeps their declarations and doc comments.
 *
 *
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Matthew Layton
 */

#include "host_cdrom.h"
#include <winioctl.h>
#include "log.h"
#include "main.h"   /* g_CdsSegment: a live mount adds its CDS entry */
#include "cdrom_policy.h"
#include "../dos/dos_iso9660.h"

#define CDROM_CACHE_PREFIX              "NTVDMEX-CD-"
#define CDROM_PATH_SIZE                 MAX_PATH
#define CDROM_DONE_MAX                  4096    /* Files copied out this run */
#define CDROM_COPY_CHUNK                65536u
#define CDROM_PATH_PARTS                32
#define CDROM_YEAR_BASE                 1900
#define CDROM_DEVICE_NAME_SIZE          3       /* "Q:" */
#define CDROM_FILE_ID_BYTES             (ISO_FILE_ID_SIZE + 1)
#define CDROM_DRIVE_LETTERS             26
#define CDROM_DIRECTORY_RECORD_BYTES    255
#define CDROM_LOG_LINE_SIZE             (CDROM_PATH_SIZE * 3)

typedef struct _CDROM_LISTING
{
    PCSTR HostDirectory;
} CDROM_LISTING;

static HANDLE     g_CdromImage = INVALID_HANDLE_VALUE;
static ISO_VOLUME g_CdromVolume;
static INT        g_CdromHasDisc;                 /* the image mounted as ISO 9660 */
static INT        g_CdromDriveIndex = -1;
static volatile LONG g_CdromMapped;               /* the drive mapping is in place */
static CHAR       g_CdromCache[CDROM_PATH_SIZE];  /* no trailing '\' */
static CHAR       g_CdromDevice[CDROM_DEVICE_NAME_SIZE];
static CHAR       g_CdromImagePath[CDROM_PATH_SIZE];  /* what is mounted, for HostCdromApply */
static PSTR       g_CdromDone[CDROM_DONE_MAX];    /* cache paths whose content is copied out */
static INT        g_CdromDoneCount;
static CRITICAL_SECTION g_CdromLock;
static INT        g_CdromLockReady;

/* No C runtime in the host: the two string tests this file needs. */
static INT CdromContains(PCSTR text, PCSTR part)
{
    INT start;

    for (start = 0; text[start]; ++start)
    {
        INT index = 0;

        while (part[index] && text[start + index] == part[index])
            ++index;

        if (!part[index])
            return 1;
    }

    return 0;
}

static INT CdromIsPattern(PCSTR text)
{
    for (; *text; ++text)
        if (*text == '*' || *text == '?')
            return 1;

    return 0;
}

static INT CdromReadSectors(PVOID context, DWORD sector, DWORD count, PBYTE buffer)
{
    LARGE_INTEGER position;
    DWORD wanted = count * ISO_SECTOR_SIZE;
    DWORD got = 0;

    (VOID)context;
    position.QuadPart = (LONGLONG)sector * ISO_SECTOR_SIZE;

    if (g_CdromImage == INVALID_HANDLE_VALUE || !SetFilePointerEx(g_CdromImage, position, NULL, FILE_BEGIN))
        return 0;

    return ReadFile(g_CdromImage, buffer, wanted, &got, NULL) && got == wanted;
}

/* A mapping a crashed host left behind: any letter whose target is one of our caches. */
static VOID CdromRemoveStaleMappings(VOID)
{
    INT letter;

    for (letter = 0; letter < CDROM_DRIVE_LETTERS; ++letter)
    {
        CHAR device[CDROM_DEVICE_NAME_SIZE];
        CHAR target[CDROM_PATH_SIZE];
        device[0] = (CHAR)('A' + letter);
        device[1] = ':';
        device[2] = 0;

        if (QueryDosDeviceA(device, target, sizeof target) && CdromContains(target, CDROM_CACHE_PREFIX))
            DefineDosDeviceA(DDD_REMOVE_DEFINITION, device, NULL);
    }
}

/* The letter for the ISO drive: CdromPolicyChooseLetter over the drives Windows has now. */
static INT CdromChooseLetter(VOID)
{
    DWORD drives = GetLogicalDrives();
    DWORD remote = 0;
    INT letter;

    for (letter = 0; letter < CDROM_DRIVE_LETTERS; ++letter)
    {
        CHAR root[] = "A:\\";
        root[0] = (CHAR)('A' + letter);

        if ((drives & (1u << letter)) && GetDriveTypeA(root) == DRIVE_REMOTE)
            remote |= 1u << letter;
    }

    return CdromPolicyChooseLetter(drives, remote);
}

/* The copied-out list names files in the cache; a re-mount empties the cache, so it goes too. */
static VOID CdromForgetDone(VOID)
{
    while (g_CdromDoneCount > 0)
        HeapFree(GetProcessHeap(), 0, g_CdromDone[--g_CdromDoneCount]);
}

VOID HostCdromMount(PSTR *cursorIo, PCSTR imagePath, INT isImageMode)
{
    PSTR cursor = *cursorIo;
    CHAR temporary[CDROM_PATH_SIZE];
    INT letter;

    if (!isImageMode)
        return;                                    /* physical: the host's own CD drives, as always */

    if (!g_CdromLockReady)
    {
        InitializeCriticalSection(&g_CdromLock);
        g_CdromLockReady = 1;
    }

    g_CdromHasDisc = 0;                            /* a re-mount (HostCdromApply) starts empty */
    CdromForgetDone();
    CdromRemoveStaleMappings();
    letter = CdromChooseLetter();

    if (letter < 0 || !GetTempPathA(sizeof temporary, temporary))
    {
        cursor = LogPut(cursor, "CDROM: no free drive letter or no temp directory -- no CD-ROM drive\r\n");
        *cursorIo = cursor;
        return;
    }

    lstrcpynA(g_CdromImagePath, imagePath ? imagePath : "", sizeof g_CdromImagePath);
    wsprintfA(g_CdromCache, "%s" CDROM_CACHE_PREFIX "%lu", temporary, GetCurrentProcessId());
    CreateDirectoryA(g_CdromCache, NULL);

    if (imagePath && imagePath[0])
    {
        g_CdromImage = CreateFileA(imagePath, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                                   FILE_ATTRIBUTE_NORMAL | FILE_FLAG_RANDOM_ACCESS, NULL);

        if (g_CdromImage != INVALID_HANDLE_VALUE && IsoMount(&g_CdromVolume, CdromReadSectors, NULL))
            g_CdromHasDisc = 1;
        else
        {
            cursor = LogPut(cursor, "CDROM: \"");
            cursor = LogPut(cursor, imagePath);
            cursor = LogPut(cursor, g_CdromImage == INVALID_HANDLE_VALUE ? "\" cannot be opened" : "\" is not an ISO 9660 image");
            cursor = LogPut(cursor, " -- the drive is EMPTY (reads answer not ready)\r\n");
        }
    }

    g_CdromDevice[0] = (CHAR)('A' + letter);
    g_CdromDevice[1] = ':';
    g_CdromDevice[2] = 0;

    if (!DefineDosDeviceA(0, g_CdromDevice, g_CdromCache))
    {
        cursor = LogPut(cursor, "CDROM: DefineDosDevice failed err=0x");
        cursor = LogHex(cursor, GetLastError());
        cursor = LogPut(cursor, " -- no CD-ROM drive\r\n");
        *cursorIo = cursor;
        return;
    }

    InterlockedExchange(&g_CdromMapped, 1);
    g_CdromDriveIndex = letter;
    cursor = LogPut(cursor, "  #241 CD-ROM ");
    cursor = LogPut(cursor, g_CdromDevice);
    cursor = LogPut(cursor, " = ");
    cursor = LogPut(cursor, g_CdromHasDisc ? g_CdromImagePath : "EMPTY (no image)");

    if (g_CdromHasDisc)
    {
        cursor = LogPut(cursor, ", volume sectors=0x");
        cursor = LogHex(cursor, g_CdromVolume.VolumeSectors);
    }

    cursor = LogPut(cursor, ", cache ");
    cursor = LogPut(cursor, g_CdromCache);
    cursor = LogPut(cursor, "\r\n");
    *cursorIo = cursor;
}

/* Delete what the cache holds -- read-only files first lose that bit. */
static VOID CdromRemoveTree(PCSTR directory)
{
    CHAR pattern[CDROM_PATH_SIZE];
    WIN32_FIND_DATAA findData;
    HANDLE find;

    wsprintfA(pattern, "%s\\*", directory);
    find = FindFirstFileA(pattern, &findData);

    if (find != INVALID_HANDLE_VALUE)
    {
        do
        {
            CHAR child[CDROM_PATH_SIZE];

            if (!lstrcmpA(findData.cFileName, ".") || !lstrcmpA(findData.cFileName, ".."))
                continue;

            wsprintfA(child, "%s\\%s", directory, findData.cFileName);
            SetFileAttributesA(child, FILE_ATTRIBUTE_NORMAL);

            if (findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                CdromRemoveTree(child);
            else
                DeleteFileA(child);
        }
        while (FindNextFileA(find, &findData));

        FindClose(find);
    }

    RemoveDirectoryA(directory);
}

VOID HostCdromUnmount(VOID)
{
    if (!InterlockedExchange(&g_CdromMapped, 0))
        return;

    DefineDosDeviceA(DDD_REMOVE_DEFINITION, g_CdromDevice, NULL);

    if (g_CdromImage != INVALID_HANDLE_VALUE)
    {
        CloseHandle(g_CdromImage);
        g_CdromImage = INVALID_HANDLE_VALUE;
    }

    CdromRemoveTree(g_CdromCache);
}

INT HostCdromDrive(VOID)
{
    return g_CdromMapped ? g_CdromDriveIndex : -1;
}

DWORD HostCdromSectors(VOID)
{
    return g_CdromHasDisc ? g_CdromVolume.VolumeSectors : 0;
}

VOID HostCdromLabel(PSTR out)
{
    if (g_CdromHasDisc)
        IsoVolumeLabel(&g_CdromVolume, out);
    else
        out[0] = 0;
}

/* One CDS entry, as StartupBuildDriveTables writes them: a redirector's for the CD-ROM,
 * an unused one for a letter that has gone.
 */
static VOID CdromCdsWrite(INT drive, INT isCdrom)
{
    BYTE cds[DOS_CDS_LEN];
    volatile BYTE *cdsBytes = (volatile BYTE *)((DWORD)g_CdsSegment << PARAGRAPH_SHIFT);
    UINT item;

    if (!g_CdsSegment || drive < 0 || drive >= DOS_LASTDRIVE)
        return;

    DosCdsBuild(cds, (UINT)drive, isCdrom ? (DOS_CDS_FLAG_PHYSICAL | DOS_CDS_FLAG_NETWORK) : 0, DOS_CTAB_SEG, DOS_DPBCHAIN_OFF);

    for (item = 0; item < DOS_CDS_LEN; ++item)
        cdsBytes[drive * DOS_CDS_LEN + item] = cds[item];
}

/* SETTINGS > DRIVES > CD-ROM, LIVE (s101 rig report: the ISO chosen in the dialog never
 * appeared -- the drive was mounted once, at start-up, and the letter the user then tried
 * was the PC's own empty CD drive). The floppy has been live since s84; the CD-ROM now is
 * too: the old drive goes, the new one is mounted, its CDS entry is written and the DOS
 * layer is told its letter. A program with a file open on the old disc loses it, as it
 * would if the disc were ejected.
 */
VOID HostCdromApply(PCSTR imagePath, INT isImageMode)
{
    CHAR lineBuffer[CDROM_LOG_LINE_SIZE];
    PSTR lineCursor = lineBuffer;
    INT oldDrive = HostCdromDrive();
    INT newDrive;

    if (!CdromPolicyNeedsRemount(g_CdromMapped != 0, g_CdromImagePath, isImageMode, imagePath))
        return;

    if (!g_CdromLockReady)
    {
        InitializeCriticalSection(&g_CdromLock);
        g_CdromLockReady = 1;
    }

    if (g_DosMachine)
    {
        g_DosMachine->CdromDrive = -1;                          /* the DOS layer stops using it first */
        g_DosMachine->PathPrepare = 0;
    }

    EnterCriticalSection(&g_CdromLock);
    HostCdromUnmount();
    g_CdromDriveIndex = -1;
    g_CdromImagePath[0] = 0;
    lineCursor = LogPut(lineCursor, "settings: CD-ROM changed --");

    if (isImageMode)
        HostCdromMount(&lineCursor, imagePath, isImageMode);
    else
        lineCursor = LogPut(lineCursor, " the PC's own CD drives\r\n");

    LeaveCriticalSection(&g_CdromLock);
    newDrive = HostCdromDrive();

    if (oldDrive >= 0 && oldDrive != newDrive)
        CdromCdsWrite(oldDrive, 0);

    if (newDrive >= 0)
        CdromCdsWrite(newDrive, 1);

    if (g_DosMachine)
    {
        g_DosMachine->CdromSectors = HostCdromSectors();
        HostCdromLabel(g_DosMachine->CdromLabel);
        g_DosMachine->PathPrepare = (newDrive >= 0) ? HostCdromPrepare : 0;
        g_DosMachine->PathPrepareContext = NULL;
        g_DosMachine->CdromDrive = newDrive;
    }

    LogAppend(LOG_PATH, lineBuffer, lineCursor);
}

/* The recording date (ECMA-119 9.1.5) as a file time. The recorded clock is local time,
 * so it is handed to DOS as the local time it says.
 */
static INT CdromFileTime(PCISO_ENTRY entry, FILETIME *fileTime)
{
    SYSTEMTIME systemTime;
    FILETIME local;

    if (!entry->Date[1] || !entry->Date[2])
        return 0;

    systemTime.wYear = (WORD)(CDROM_YEAR_BASE + entry->Date[0]);
    systemTime.wMonth = entry->Date[1];
    systemTime.wDay = entry->Date[2];
    systemTime.wHour = entry->Date[3];
    systemTime.wMinute = entry->Date[4];
    systemTime.wSecond = entry->Date[5];
    systemTime.wMilliseconds = 0;
    systemTime.wDayOfWeek = 0;
    return SystemTimeToFileTime(&systemTime, &local) && LocalFileTimeToFileTime(&local, fileTime);
}

static INT CdromIsDone(PCSTR hostPath)
{
    INT index;

    for (index = 0; index < g_CdromDoneCount; ++index)
        if (!lstrcmpiA(g_CdromDone[index], hostPath))
            return 1;

    return 0;
}

static VOID CdromNoteDone(PCSTR hostPath)
{
    PSTR copy;

    if (g_CdromDoneCount >= CDROM_DONE_MAX)
        return;

    copy = (PSTR)HeapAlloc(GetProcessHeap(), 0, (SIZE_T)lstrlenA(hostPath) + 1);

    if (copy)
    {
        lstrcpyA(copy, hostPath);
        g_CdromDone[g_CdromDoneCount++] = copy;
    }
}

static VOID CdromFinishFile(PCSTR hostPath, HANDLE file, PCISO_ENTRY entry)
{
    FILETIME fileTime;

    if (CdromFileTime(entry, &fileTime))
        SetFileTime(file, &fileTime, &fileTime, &fileTime);

    CloseHandle(file);
    SetFileAttributesA(hostPath, FILE_ATTRIBUTE_READONLY | (entry->IsHidden ? FILE_ATTRIBUTE_HIDDEN : 0));
}

/* A file's place in a directory listing: its size and date, no content yet (sparse where
 * the volume allows, so a 600 MB file costs nothing until it is opened).
 */
static VOID CdromPlaceholder(PCSTR hostPath, PCISO_ENTRY entry)
{
    HANDLE file;
    DWORD returned = 0;
    LARGE_INTEGER size;

    if (GetFileAttributesA(hostPath) != INVALID_FILE_ATTRIBUTES)
        return;

    file = CreateFileA(hostPath, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);

    if (file == INVALID_HANDLE_VALUE)
        return;

    DeviceIoControl(file, FSCTL_SET_SPARSE, NULL, 0, NULL, 0, &returned, NULL);
    size.QuadPart = entry->Size;
    SetFilePointerEx(file, size, NULL, FILE_BEGIN);
    SetEndOfFile(file);
    CdromFinishFile(hostPath, file, entry);
}

/* The whole file, out of the image. */
static VOID CdromExtract(PCSTR hostPath, PCISO_ENTRY entry)
{
    static BYTE chunk[CDROM_COPY_CHUNK];
    HANDLE file;
    DWORD offset = 0;

    if (CdromIsDone(hostPath))
        return;

    SetFileAttributesA(hostPath, FILE_ATTRIBUTE_NORMAL);     /* a placeholder is read-only too */
    file = CreateFileA(hostPath, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);

    if (file == INVALID_HANDLE_VALUE)
        return;

    while (offset < entry->Size)
    {
        DWORD written = 0;
        DWORD got = IsoReadFile(&g_CdromVolume, entry, offset, chunk, CDROM_COPY_CHUNK);

        if (!got || !WriteFile(file, chunk, got, &written, NULL) || written != got)
            break;

        offset += got;
    }

    CdromFinishFile(hostPath, file, entry);

    if (offset == entry->Size)
        CdromNoteDone(hostPath);
}

static VOID CdromDirectory(PCSTR hostPath, PCISO_ENTRY entry)
{
    if (CreateDirectoryA(hostPath, NULL) && entry->IsHidden)
        SetFileAttributesA(hostPath, FILE_ATTRIBUTE_HIDDEN);
}

static INT CdromListVisit(PVOID context, PCISO_ENTRY entry)
{
    CDROM_LISTING *listing = (CDROM_LISTING *)context;
    CHAR hostPath[CDROM_PATH_SIZE];

    wsprintfA(hostPath, "%s\\%s", listing->HostDirectory, entry->Name);

    if (entry->IsDirectory)
        CdromDirectory(hostPath, entry);
    else
        CdromPlaceholder(hostPath, entry);

    return 0;
}

VOID HostCdromPrepare(PVOID context, PCSTR path)
{
    CHAR fullPath[CDROM_PATH_SIZE];
    CHAR isoPath[CDROM_PATH_SIZE];
    CHAR hostPath[CDROM_PATH_SIZE];
    PSTR filePart;
    PSTR part[CDROM_PATH_PARTS];
    INT parts = 0;
    INT index;
    PSTR scan;
    ISO_ENTRY entry;
    DWORD length;

    (VOID)context;

    if (!g_CdromMapped || !g_CdromHasDisc)
        return;

    length = GetFullPathNameA(path, sizeof fullPath, fullPath, &filePart);

    if (!length || length >= sizeof fullPath || fullPath[1] != ':' || fullPath[2] != '\\'
        || ((fullPath[0] | 0x20) - 'a') != g_CdromDriveIndex)
        return;

    for (scan = fullPath + 3; *scan && parts < CDROM_PATH_PARTS;)
    {
        part[parts++] = scan;

        while (*scan && *scan != '\\')
            ++scan;

        if (*scan)
            *scan++ = 0;
    }

    EnterCriticalSection(&g_CdromLock);
    lstrcpyA(isoPath, "");
    lstrcpyA(hostPath, g_CdromCache);

    for (index = 0; index < parts; ++index)
    {
        INT isLast = (index == parts - 1);
        INT isPattern = CdromIsPattern(part[index]);

        if (isLast && isPattern)
        {
            /* A search: lay out the directory it searches. */
            CDROM_LISTING listing;

            if (IsoLookup(&g_CdromVolume, isoPath, &entry) && entry.IsDirectory)
            {
                listing.HostDirectory = hostPath;
                IsoEnumerate(&g_CdromVolume, &entry, CdromListVisit, &listing);
            }

            break;
        }

        lstrcatA(isoPath, "\\");
        lstrcatA(isoPath, part[index]);

        if (!IsoLookup(&g_CdromVolume, isoPath, &entry))
            break;                                  /* not on the disc: Win32 will say so */

        lstrcatA(hostPath, "\\");
        lstrcatA(hostPath, entry.Name);

        if (entry.IsDirectory)
            CdromDirectory(hostPath, &entry);
        else if (isLast)
            CdromExtract(hostPath, &entry);
        else
            break;                                  /* a file in the middle of a path */
    }

    LeaveCriticalSection(&g_CdromLock);
}

/* A far pointer's linear address in the VDM (the low megabyte is mapped at 0). */
static volatile BYTE *CdromGuest(DWORD segment, DWORD offset)
{
    return (volatile BYTE *)(ULONG_PTR)(((segment & WORD_MASK) << PARAGRAPH_SHIFT) + (offset & WORD_MASK));
}

static INT CdromFail(volatile BYTE *tib, WORD error)
{
    VDM_SET16(tib, VTIB_EAX, error);
    return CDROM_DONE_ERROR;
}

INT HostCdromInt2F(volatile BYTE *tib)
{
    BYTE function = (BYTE)(VDM_REG16(tib, VTIB_EAX) & BYTE_MASK);
    INT drive = HostCdromDrive();
    DWORD cx = VDM_REG16(tib, VTIB_ECX);
    volatile BYTE *buffer = CdromGuest(VDM_REG16(tib, VTIB_ES), VDM_REG16(tib, VTIB_EBX));

    if (drive < 0)
        return CDROM_PASS;                          /* no MSCDEX: AX=1500h finds nobody */

    switch (function)
    {
    case CDROM_FN_INSTALLATION_CHECK:               /* BX = drives, CX = the first */
        VDM_SET16(tib, VTIB_EBX, 1);
        VDM_SET16(tib, VTIB_ECX, (WORD)drive);
        return CDROM_DONE;

    case CDROM_FN_DRIVE_CHECK:                      /* CX = drive -> AX nonzero if a CD-ROM, BX=ADADh */
        VDM_SET16(tib, VTIB_EAX, (WORD)(cx == (DWORD)drive ? CDROM_DRIVE_SUPPORTED : 0));
        VDM_SET16(tib, VTIB_EBX, CDROM_SIGNATURE);
        return CDROM_DONE;

    case CDROM_FN_VERSION:
        VDM_SET16(tib, VTIB_EBX, CDROM_VERSION);
        return CDROM_DONE;

    case CDROM_FN_DRIVE_LETTERS:                    /* ES:BX <- one byte per drive (0 = A:) */
        buffer[0] = (BYTE)drive;
        return CDROM_DONE;

    case CDROM_FN_DEVICE_REQUEST:
        /* There is no device driver behind this drive -- so no audio, no IOCTL. Every
         * request is answered "unknown command", not left looking like success.
         */
        if (cx != (DWORD)drive)
            return CdromFail(tib, CDROM_ERR_INVALID_DRIVE);

        buffer[CDROM_REQUEST_STATUS] = (BYTE)(CDROM_REQUEST_UNKNOWN_COMMAND & BYTE_MASK);
        buffer[CDROM_REQUEST_STATUS + 1] = (BYTE)(CDROM_REQUEST_UNKNOWN_COMMAND >> BYTE_SHIFT);
        return CDROM_DONE;

    case CDROM_FN_DESCRIPTOR_PREFERENCE:            /* BX=0 get, BX=1 set; only the primary */
        if (cx != (DWORD)drive)
            return CdromFail(tib, CDROM_ERR_INVALID_DRIVE);

        if (VDM_REG16(tib, VTIB_EBX) == 0)
        {
            VDM_SET16(tib, VTIB_EDX, CDROM_DESCRIPTOR_PRIMARY);
            return CDROM_DONE;
        }

        return (VDM_REG16(tib, VTIB_EDX) == CDROM_DESCRIPTOR_PRIMARY) ? CDROM_DONE : CdromFail(tib, CDROM_ERR_INVALID_FUNCTION);

    default:
        break;
    }

    if (function == CDROM_FN_COPYRIGHT_FILE || function == CDROM_FN_ABSTRACT_FILE || function == CDROM_FN_BIBLIOGRAPHIC_FILE
        || function == CDROM_FN_READ_VTOC || function == CDROM_FN_ABSOLUTE_READ || function == CDROM_FN_DIRECTORY_ENTRY)
    {
        DWORD requested = (function == CDROM_FN_DIRECTORY_ENTRY) ? (cx & BYTE_MASK) : cx;

        if (requested != (DWORD)drive)
            return CdromFail(tib, CDROM_ERR_INVALID_DRIVE);

        if (!g_CdromHasDisc)
            return CdromFail(tib, CDROM_ERR_NOT_READY);
    }
    else
        return CDROM_PASS;                          /* 1501h, 1506h/07h, 1509h: not answered */

    if (function == CDROM_FN_COPYRIGHT_FILE || function == CDROM_FN_ABSTRACT_FILE || function == CDROM_FN_BIBLIOGRAPHIC_FILE)
    {
        CHAR identifier[CDROM_FILE_ID_BYTES];
        DWORD offset = (function == CDROM_FN_COPYRIGHT_FILE) ? ISO_PVD_COPYRIGHT_FILE
                     : (function == CDROM_FN_ABSTRACT_FILE) ? ISO_PVD_ABSTRACT_FILE : ISO_PVD_BIBLIOGRAPHIC_FILE;
        INT index;
        IsoFileIdentifier(&g_CdromVolume, offset, identifier);

        for (index = 0; index < CDROM_FILE_ID_BYTES; ++index)
            buffer[index] = (BYTE)identifier[index];

        return CDROM_DONE;
    }

    if (function == CDROM_FN_READ_VTOC)             /* DX = descriptor index -> AX = its type */
    {
        static BYTE descriptor[ISO_SECTOR_SIZE];
        INT type = IsoReadDescriptor(&g_CdromVolume, VDM_REG16(tib, VTIB_EDX), descriptor);
        DWORD index;

        if (type < 0)
            return CdromFail(tib, CDROM_ERR_NOT_READY);

        for (index = 0; index < ISO_SECTOR_SIZE; ++index)
            buffer[index] = descriptor[index];

        VDM_SET16(tib, VTIB_EAX, (WORD)(type == ISO_DESCRIPTOR_PRIMARY || type == ISO_DESCRIPTOR_TERMINATOR ? type : 0));
        return CDROM_DONE;
    }

    if (function == CDROM_FN_ABSOLUTE_READ)         /* SI:DI = first sector, DX = count -> ES:BX */
    {
        static BYTE sector[ISO_SECTOR_SIZE];
        DWORD first = (VDM_REG16(tib, VTIB_ESI) << WORD_SHIFT) | VDM_REG16(tib, VTIB_EDI);
        DWORD count = VDM_REG16(tib, VTIB_EDX);
        DWORD index;

        for (index = 0; index < count; ++index)
        {
            DWORD byteIndex;

            if (!CdromReadSectors(NULL, first + index, 1, sector))
                return CdromFail(tib, CDROM_ERR_NOT_READY);

            for (byteIndex = 0; byteIndex < ISO_SECTOR_SIZE; ++byteIndex)
                buffer[index * ISO_SECTOR_SIZE + byteIndex] = sector[byteIndex];
        }

        return CDROM_DONE;
    }

    /* 150Fh: CL = drive, ES:BX = the path, SI:DI <- its directory record; AX = 1 (ISO 9660). */
    {
        CHAR isoPath[CDROM_PATH_SIZE];
        ISO_ENTRY entry;
        volatile BYTE *output = CdromGuest(VDM_REG16(tib, VTIB_ESI), VDM_REG16(tib, VTIB_EDI));
        INT length = 0;
        INT index;
        volatile BYTE *name = buffer;

        if (name[0] && name[1] == ':')
            name += 2;                                /* "Q:\DIR\FILE" -> "\DIR\FILE" */

        while (name[length] && length < CDROM_PATH_SIZE - 1)
        {
            isoPath[length] = (CHAR)name[length];
            ++length;
        }

        isoPath[length] = 0;

        if (!IsoLookup(&g_CdromVolume, isoPath, &entry))
            return CdromFail(tib, CDROM_ERR_FILE_NOT_FOUND);

        for (index = 0; index < entry.RecordLength && index < CDROM_DIRECTORY_RECORD_BYTES; ++index)
            output[index] = entry.Record[index];

        VDM_SET16(tib, VTIB_EAX, CDROM_ENTRY_IS_ISO9660);
        return CDROM_DONE;
    }
}

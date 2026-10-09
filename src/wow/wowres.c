/* wowres.c -- ★ THE GUEST'S OWN RESOURCES, AS REAL Win32 OBJECTS. GH #128, s.43.
 *
 * The code of wowres.h (#335): its functions and state, in their original order. Part of
 * the host's single translation unit: #included by main.c straight after wowres.h. */

/* Forward declarations for the single translation unit (they were in wowres.h). */
/* ── ★★★★★ AND THE GROUP CAN BE NAMED. (session 47) ─────────────────────────
     MS Paint showed no icon at all -- Windows fell back to the generic
     application icon -- and the cause is the one session 45 already found for
     MENUS and did not carry across to icons:

       NOTEPAD.EXE   RT_GROUP_ICON  #1          <- an ordinal, so it worked
       PBRUSH.EXE    RT_GROUP_ICON  "PBRUSH"    <- a NAME, so it was refused

     ⚠ Read straight out of the two resource tables, not inferred: PBRUSH's
     RT_CURSOR entries are named too ("FLOOD", "CROSSH", "PICK", "TEXT",
     "SIDEAROW"), and so is its accelerator table. **A Win16 program is as likely
     to name a resource as to number it, and this host only understood numbers.**
   ⇒ one lookup each way, and the caller passes whichever the guest gave it. */
static HICON WowResIconAt(DWORD groupOffset, DWORD groupLength, PINT picked, INT width, INT height);

static PBYTE g_WowResImage  = NULL;      /* the application's file, verbatim */
static DWORD  g_WowResLength  = 0;
static CHAR   g_WowResPath[WOWRES_PATH_MAX];

static WORD WowResReadWord(DWORD offset)
{
    if (offset + WOW_WORD_BYTES > g_WowResLength) return 0;
    return (WORD)(g_WowResImage[offset] | (g_WowResImage[offset + 1] << BYTE_SHIFT));
}

static WOWRES_CACHE_ENTRY g_WowResCache[WOWRES_CACHE];
static INT g_WowResCacheCount = 0;

static INT WowResOpen(PCSTR path)
{
    HANDLE file;
    DWORD size = 0, bytesRead = 0;
    PBYTE image;
    INT index, slot;
    if (!path || !path[0]) return 0;
    for (slot = 0; slot < g_WowResCacheCount; ++slot)
        if (lstrcmpiA(g_WowResCache[slot].Path, path) == 0) {
            g_WowResImage = g_WowResCache[slot].Image; g_WowResLength = g_WowResCache[slot].Length;
            lstrcpynA(g_WowResPath, path, sizeof g_WowResPath);
            return g_WowResImage != NULL;
        }
    g_WowResImage = NULL; g_WowResLength = 0;
    for (index = 0; index < (INT)sizeof g_WowResPath - 1 && path[index]; ++index) g_WowResPath[index] = path[index];
    g_WowResPath[index] = 0;
    slot = (g_WowResCacheCount < WOWRES_CACHE) ? g_WowResCacheCount++ : WOWRES_CACHE - 1;
    lstrcpynA(g_WowResCache[slot].Path, path, sizeof g_WowResCache[slot].Path);
    g_WowResCache[slot].Image = NULL; g_WowResCache[slot].Length = 0;     /* a failure is cached too */
    file = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                    NULL, OPEN_EXISTING, 0, NULL);
    if (file == INVALID_HANDLE_VALUE) return 0;
    size = GetFileSize(file, NULL);
    if (size == INVALID_FILE_SIZE || size > WOWRES_MAX_FILE) { CloseHandle(file); return 0; }
    image = (PBYTE)HeapAlloc(GetProcessHeap(), 0, size);
    if (!image) { CloseHandle(file); return 0; }
    ReadFile(file, image, size, &bytesRead, NULL);
    CloseHandle(file);
    if (bytesRead < WOWRES_MZ_HEADER_SIZE || image[0] != 'M' || image[1] != 'Z') return 0;
    g_WowResCache[slot].Image = image; g_WowResCache[slot].Length = bytesRead;
    g_WowResImage = image; g_WowResLength = bytesRead;
    return 1;
}

/* Locate a resource by integer type and integer id. 0 = not found. */
static DWORD WowResFind(WORD type, WORD id, PDWORD length)
{
    DWORD header, resourceTable, position;
    WORD shift;
    if (!g_WowResImage) return 0;
    header = (DWORD)(g_WowResImage[WOWRES_MZ_NE_OFFSET] | (g_WowResImage[WOWRES_MZ_NE_OFFSET + 1] << BYTE_SHIFT)
              | (g_WowResImage[WOWRES_MZ_NE_OFFSET + 2] << WORD_SHIFT) | ((DWORD)g_WowResImage[WOWRES_MZ_NE_OFFSET + 3] << TOP_BYTE_SHIFT));
    if (header + WOWRES_NE_HEADER_SIZE > g_WowResLength || g_WowResImage[header] != 'N' || g_WowResImage[header + 1] != 'E') return 0;
    resourceTable = header + WowResReadWord(header + WOWRES_NE_RESOURCE_TABLE);
    if (resourceTable + WOW_WORD_BYTES > g_WowResLength) return 0;
    shift = WowResReadWord(resourceTable);
    if (shift > WOWRES_MAX_ALIGN_SHIFT) return 0;
    position = resourceTable + WOW_WORD_BYTES;
    while (position + WOWRES_TYPEINFO_SIZE <= g_WowResLength) {
        WORD typeId = WowResReadWord(position), count = WowResReadWord(position + WOWRES_TYPEINFO_COUNT), index;
        if (!typeId) break;
        position += WOWRES_TYPEINFO_SIZE;
        for (index = 0; index < count && position + WOWRES_NAMEINFO_SIZE <= g_WowResLength; ++index, position += WOWRES_NAMEINFO_SIZE) {
            /* Integer type and id both carry the high bit; a named one is an
               offset into the string pool and is not what this looks up. */
            if (typeId == (WORD)(WOWRES_INTEGER_ID | type) && WowResReadWord(position + WOWRES_NAMEINFO_ID) == (WORD)(WOWRES_INTEGER_ID | id)) {
                DWORD offset = (DWORD)WowResReadWord(position) << shift;
                DWORD resourceLength  = (DWORD)WowResReadWord(position + WOWRES_NAMEINFO_LENGTH) << shift;
                if (offset + resourceLength > g_WowResLength) return 0;
                if (length) *length = resourceLength;
                return offset;
            }
        }
    }
    return 0;
}

/* The first resource of a type, whatever its id -- see the accelerator note. */
static DWORD WowResFindAny(WORD type, PWORD idOutput, PDWORD length)
{
    DWORD header, resourceTable, position;
    WORD shift;
    if (!g_WowResImage) return 0;
    header = (DWORD)(g_WowResImage[WOWRES_MZ_NE_OFFSET] | (g_WowResImage[WOWRES_MZ_NE_OFFSET + 1] << BYTE_SHIFT)
              | (g_WowResImage[WOWRES_MZ_NE_OFFSET + 2] << WORD_SHIFT) | ((DWORD)g_WowResImage[WOWRES_MZ_NE_OFFSET + 3] << TOP_BYTE_SHIFT));
    if (header + WOWRES_NE_HEADER_SIZE > g_WowResLength || g_WowResImage[header] != 'N' || g_WowResImage[header + 1] != 'E') return 0;
    resourceTable = header + WowResReadWord(header + WOWRES_NE_RESOURCE_TABLE);
    if (resourceTable + WOW_WORD_BYTES > g_WowResLength) return 0;
    shift = WowResReadWord(resourceTable);
    if (shift > WOWRES_MAX_ALIGN_SHIFT) return 0;
    position = resourceTable + WOW_WORD_BYTES;
    while (position + WOWRES_TYPEINFO_SIZE <= g_WowResLength) {
        WORD typeId = WowResReadWord(position), count = WowResReadWord(position + WOWRES_TYPEINFO_COUNT), index;
        if (!typeId) break;
        position += WOWRES_TYPEINFO_SIZE;
        for (index = 0; index < count && position + WOWRES_NAMEINFO_SIZE <= g_WowResLength; ++index, position += WOWRES_NAMEINFO_SIZE) {
            if (typeId == (WORD)(WOWRES_INTEGER_ID | type)) {
                DWORD offset = (DWORD)WowResReadWord(position) << shift;
                DWORD resourceLength  = (DWORD)WowResReadWord(position + WOWRES_NAMEINFO_LENGTH) << shift;
                if (offset + resourceLength > g_WowResLength) return 0;
                if (idOutput) *idOutput = (WORD)(WowResReadWord(position + WOWRES_NAMEINFO_ID) & WOWRES_ID_MASK);
                if (length) *length = resourceLength;
                return offset;
            }
        }
    }
    return 0;
}

/* Parse the module's accelerator table. Returns the number of entries. */
static INT WowResAccelFirst(PWOWRES_ACCEL output, INT capacity, PWORD resourceId)
{
    DWORD length = 0, offset = WowResFindAny(WOWRES_RT_ACCEL, resourceId, &length);
    INT count = 0;
    if (!offset || !output) return 0;
    while (count < capacity && (DWORD)(count * WOWRES_ACCEL_ENTRY_SIZE + WOWRES_ACCEL_ENTRY_SIZE) <= length) {
        PCBYTE entry = g_WowResImage + offset + count * WOWRES_ACCEL_ENTRY_SIZE;
        output[count].Flags = entry[0];
        output[count].Key   = (WORD)(entry[WOWRES_ACCEL_FIELD_KEY] | (entry[WOWRES_ACCEL_FIELD_KEY + 1] << BYTE_SHIFT));
        output[count].Id    = (WORD)(entry[WOWRES_ACCEL_FIELD_ID] | (entry[WOWRES_ACCEL_FIELD_ID + 1] << BYTE_SHIFT));
        ++count;
        if (entry[0] & WOWRES_ACCEL_LAST) break;
    }
    return count;
}

/*
 * ── ★★★ A RESOURCE CAN BE NAMED RATHER THAN NUMBERED, AND MS PAINT'S IS ─────
 * `WowResFind` above looks up INTEGER ids, and said so. Notepad's menu is
 * `#0001`, so that was enough to give Notepad a working menu bar and the gap sat
 * there unexercised. PBRUSH.EXE registers `pbParent` with `MENU="PBrush2"` and
 * its resource table holds `MENU PBRUSH2` -- a NAMED resource -- so the lookup
 * could never match and Paint came up with no menu at all, silently.
 *
 * In an NE resource table a type or id word with the high bit CLEAR is not a
 * number: it is a byte OFFSET FROM THE START OF THE RESOURCE TABLE to a
 * length-prefixed (Pascal) string in the table's own string pool.
 * ⚠ THE COMPARISON IS CASE-INSENSITIVE, and that is not a nicety. The names are
 *   stored UPPER CASE (`PBRUSH2`) and the program asks for the case it wrote in
 *   its source (`PBrush2`); an exact compare finds nothing, which is exactly the
 *   failure that was on screen.
 * ⚠ AND THE STRING POOL IS BOUNDS-CHECKED like everything else here: the offset
 *   comes out of a file, so a corrupt one must fail to match rather than read
 *   past the image.
 */
static INT WowResNameIs(DWORD resourceTable, WORD idWord, PCSTR wanted)
{
    DWORD stringOffset;
    BYTE  nameLength;
    INT   index;
    if (idWord & WOWRES_INTEGER_ID) return 0;                  /* an integer, not a name    */
    stringOffset = resourceTable + idWord;
    if (stringOffset + 1 > g_WowResLength) return 0;
    nameLength = g_WowResImage[stringOffset];
    if (!nameLength || stringOffset + 1 + nameLength > g_WowResLength) return 0;
    for (index = 0; index < (INT)nameLength; ++index) {
        CHAR stored = (CHAR)g_WowResImage[stringOffset + 1 + index], expected = wanted[index];
        if (stored >= 'a' && stored <= 'z') stored = (CHAR)(stored - WOWRES_LOWER_TO_UPPER);
        if (expected >= 'a' && expected <= 'z') expected = (CHAR)(expected - WOWRES_LOWER_TO_UPPER);
        if (!expected || stored != expected) return 0;
    }
    return wanted[nameLength] == 0;                         /* and no trailing extra     */
}

/* Locate a resource of integer type `type` whose id is the NAME `name`.
   0 = not found. Mirrors WowResFind, which handles the numbered case. */
static DWORD WowResFindNamed(WORD type, PCSTR name, PDWORD length)
{
    DWORD header, resourceTable, position;
    WORD shift;
    if (!g_WowResImage || !name || !name[0]) return 0;
    header = (DWORD)(g_WowResImage[WOWRES_MZ_NE_OFFSET] | (g_WowResImage[WOWRES_MZ_NE_OFFSET + 1] << BYTE_SHIFT)
              | (g_WowResImage[WOWRES_MZ_NE_OFFSET + 2] << WORD_SHIFT) | ((DWORD)g_WowResImage[WOWRES_MZ_NE_OFFSET + 3] << TOP_BYTE_SHIFT));
    if (header + WOWRES_NE_HEADER_SIZE > g_WowResLength || g_WowResImage[header] != 'N' || g_WowResImage[header + 1] != 'E') return 0;
    resourceTable = header + WowResReadWord(header + WOWRES_NE_RESOURCE_TABLE);
    if (resourceTable + WOW_WORD_BYTES > g_WowResLength) return 0;
    shift = WowResReadWord(resourceTable);
    if (shift > WOWRES_MAX_ALIGN_SHIFT) return 0;
    position = resourceTable + WOW_WORD_BYTES;
    while (position + WOWRES_TYPEINFO_SIZE <= g_WowResLength) {
        WORD typeId = WowResReadWord(position), count = WowResReadWord(position + WOWRES_TYPEINFO_COUNT), index;
        if (!typeId) break;
        position += WOWRES_TYPEINFO_SIZE;
        for (index = 0; index < count && position + WOWRES_NAMEINFO_SIZE <= g_WowResLength; ++index, position += WOWRES_NAMEINFO_SIZE) {
            if (typeId != (WORD)(WOWRES_INTEGER_ID | type)) continue;
            if (!WowResNameIs(resourceTable, WowResReadWord(position + WOWRES_NAMEINFO_ID), name)) continue;
            {   DWORD offset = (DWORD)WowResReadWord(position) << shift;
                DWORD resourceLength  = (DWORD)WowResReadWord(position + WOWRES_NAMEINFO_LENGTH) << shift;
                if (offset + resourceLength > g_WowResLength) return 0;
                if (length) *length = resourceLength;
                return offset;
            }
        }
    }
    return 0;
}

/* Build one level of a menu, returning the offset just past it. `menu` may be
   NULL, which walks the template without building -- used to count items so an
   empty or unreadable menu is never attached to a window. */
static DWORD WowResMenuLevel(HMENU menu, DWORD position, DWORD end, INT depth, PINT itemCount)
{
    while (position + WOW_WORD_BYTES <= end) {
        WORD flags = WowResReadWord(position);
        WORD id = 0;
        CHAR text[WOWRES_MENU_TEXT_MAX];
        INT textLength = 0;
        position += WOW_WORD_BYTES;
        if (!(flags & WOWRES_MF_POPUP)) { id = WowResReadWord(position); position += WOW_WORD_BYTES; }
        while (position < end && g_WowResImage[position] && textLength < (INT)sizeof text - 1)
            text[textLength++] = (CHAR)g_WowResImage[position++];
        text[textLength] = 0;
        while (position < end && g_WowResImage[position]) ++position;          /* an over-long label */
        ++position;                                          /* the NUL */
        if (depth > WOWRES_MENU_MAX_DEPTH) return position;                      /* a bounded tree, always */
        if (flags & WOWRES_MF_POPUP) {
            HMENU submenu = menu ? CreatePopupMenu() : NULL;
            position = WowResMenuLevel(submenu, position, end, depth + 1, itemCount);
            if (menu && submenu) AppendMenuA(menu, MF_POPUP | MF_STRING,
                                         (UINT_PTR)submenu, text);
        } else if (!text[0] && !id) {
            if (menu) AppendMenuA(menu, MF_SEPARATOR, 0, NULL);
        } else {
            if (menu) AppendMenuA(menu, MF_STRING, id, text);
        }
        if (itemCount) ++*itemCount;
        if (flags & WOWRES_MF_END) return position;
    }
    return position;
}

/*
 * Build a real HMENU from the application's MENU resource `id`, or NULL.
 * ⚠ ONE MENU PER WINDOW. Win32 will not let two windows share an HMENU, so this
 *   builds a fresh one every time rather than caching -- a cached menu attached
 *   twice is a menu that vanishes from the first window.
 */
static HMENU WowResMenuAt(DWORD offset, DWORD length, PINT items)
{
    DWORD position;
    HMENU menu;
    INT itemCount = 0;
    if (items) *items = 0;
    if (!offset || length < WOWRES_MENU_HEADER_SIZE) return NULL;
    position = offset + WOWRES_MENU_HEADER_SIZE + WowResReadWord(offset + WOWRES_MENU_HEADER_EXTRA);                 /* version, then headerSize */
    menu = CreateMenu();
    if (!menu) return NULL;
    WowResMenuLevel(menu, position, offset + length, 0, &itemCount);
    if (items) *items = itemCount;
    if (!itemCount) { DestroyMenu(menu); return NULL; }
    return menu;
}

static HMENU WowResMenu(WORD id, PINT items)
{
    DWORD length = 0, offset = WowResFind(WOWRES_RT_MENU, id, &length);
    return WowResMenuAt(offset, length, items);
}

/* ★ The same menu, asked for by NAME -- see WowResFindNamed. MS Paint's is
   "PBrush2"; Notepad's is #1, and both paths end in the same builder. */
static HMENU WowResMenuByName(PCSTR name, PINT items)
{
    DWORD length = 0, offset = WowResFindNamed(WOWRES_RT_MENU, name, &length);
    return WowResMenuAt(offset, length, items);
}

static HICON WowResIconNamed(PCSTR name, PINT picked, INT width, INT height)
{
    DWORD groupLength = 0, groupOffset = WowResFindNamed(WOWRES_RT_GROUP_ICON, name, &groupLength);
    return WowResIconAt(groupOffset, groupLength, picked, width, height);
}

static HICON WowResIcon(WORD groupId, PINT picked, INT width, INT height)
{
    DWORD groupLength = 0, groupOffset = WowResFind(WOWRES_RT_GROUP_ICON, groupId, &groupLength);
    return WowResIconAt(groupOffset, groupLength, picked, width, height);
}

static HCURSOR WowResCursorAt(DWORD groupOffset, DWORD groupLength)
{
    DWORD cursorLength = 0, cursorOffset;
    WORD count, id;
    if (!groupOffset || groupLength < WOWRES_GROUP_HEADER_SIZE + WOWRES_GROUP_ENTRY_SIZE) return NULL;
    if (WowResReadWord(groupOffset + WOWRES_GROUP_TYPE) != WOWRES_GROUP_TYPE_CURSOR) return NULL;              /* type 2 = cursors */
    count = WowResReadWord(groupOffset + WOWRES_GROUP_COUNT);
    if (!count) return NULL;
    id = WowResReadWord(groupOffset + WOWRES_GROUP_HEADER_SIZE + WOWRES_GROUP_ENTRY_ID);
    if (!id) return NULL;
    cursorOffset = WowResFind(WOWRES_RT_CURSOR, id, &cursorLength);
    if (!cursorOffset || !cursorLength) return NULL;
    return (HCURSOR)CreateIconFromResourceEx(g_WowResImage + cursorOffset, cursorLength, FALSE,
                                             WOWRES_ICON_VERSION, 0, 0, LR_DEFAULTCOLOR);
}

static HCURSOR WowResCursorNamed(PCSTR name)
{
    DWORD groupLength = 0, groupOffset = WowResFindNamed(WOWRES_RT_GROUP_CURSOR, name, &groupLength);
    return WowResCursorAt(groupOffset, groupLength);
}

/* s89 (#216): the same, for a cursor group asked for by ordinal. */
static HCURSOR WowResCursor(WORD groupId)
{
    DWORD groupLength = 0, groupOffset = WowResFind(WOWRES_RT_GROUP_CURSOR, groupId, &groupLength);
    return WowResCursorAt(groupOffset, groupLength);
}

/* ── ★★★★ AND THE SIZE IS AN ARGUMENT, BECAUSE THE TASKBAR ASKS FOR A SMALL
     ONE. (session 47) ────────────────────────────────────────────────────────
     Measured against stock ntvdm running the same NOTEPAD.EXE on the same
     desktop, pixel for pixel out of one screenshot:

       our CAPTION icon   53 cyan-ish pixels   -- right
       our TASKBAR icon    0 cyan-ish pixels   -- 89 silver, 48 black, 23 grey
       stock's TASKBAR    59 cyan-ish pixels   -- right

     Same window, same `HICON`, two different renderings -- so the taskbar was
     not drawing the icon we built. A `WNDCLASSA` has one icon field, and when a
     window has no SMALL icon Windows produces one for itself; what came out was
     a washed-out monochrome version of the right picture.
   ⇒ Do not leave it to be derived. `CreateIconFromResourceEx` scales properly
     when it is told the size it is scaling to, so the class gets a real 16x16
     built from the same group as the 32x32, and neither the taskbar nor the
     caption has to guess. ⚠ 0 means "the system's default size", which is what
     the big icon still asks for -- passing 32 would ignore SM_CXICON. */
static HICON WowResIconAt(DWORD groupOffset, DWORD groupLength, PINT picked, INT width, INT height)
{
    DWORD iconLength = 0, iconOffset;
    WORD count, index, bestId = 0;
    INT bestBits = -1;
    if (picked) *picked = 0;
    if (!groupOffset || groupLength < WOWRES_GROUP_HEADER_SIZE) return NULL;
    if (WowResReadWord(groupOffset + WOWRES_GROUP_TYPE) != WOWRES_GROUP_TYPE_ICON) return NULL;              /* type 1 = icons */
    count = WowResReadWord(groupOffset + WOWRES_GROUP_COUNT);
    if (!count || WOWRES_GROUP_HEADER_SIZE + WOWRES_GROUP_ENTRY_SIZE * count > groupLength) return NULL;
    /* Richest colour depth wins -- the OS scales, so the only thing worth
       choosing between these is how much colour information there is. */
    for (index = 0; index < count; ++index) {
        DWORD entry = groupOffset + WOWRES_GROUP_HEADER_SIZE + (DWORD)index * WOWRES_GROUP_ENTRY_SIZE;
        INT bitCount = WowResReadWord(entry + WOWRES_GROUP_ENTRY_BITS);
        if (bitCount > bestBits) { bestBits = bitCount; bestId = WowResReadWord(entry + WOWRES_GROUP_ENTRY_ID); }
    }
    if (!bestId) return NULL;
    iconOffset = WowResFind(WOWRES_RT_ICON, bestId, &iconLength);
    if (!iconOffset || !iconLength) return NULL;
    if (picked) *picked = bestBits;
    return CreateIconFromResourceEx(g_WowResImage + iconOffset, iconLength, TRUE, WOWRES_ICON_VERSION,
                                    width, height, LR_DEFAULTCOLOR);
}

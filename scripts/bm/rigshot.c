/*
 * rigshot.c -- see the host's OWN WINDOW from the build machine.
 *
 * WHY THIS EXISTS. The host already has a screenshot (Capture > Take Screenshot),
 * and it captures g_vid.frame -- the GUEST FRAMEBUFFER. That is the right thing for
 * checking whether Doom's status bar renders, and it is useless for checking anything
 * ABOUT THE HOST: the caption, the status strip, the menu bar, a dialog. None of those
 * are in the guest's framebuffer. So every UI change this project has ever made has
 * been verified by a human walking to the box, which is why "the settings dialog has
 * never been opened by a human" sat open for a whole session.
 *
 * This is the missing half: BitBlt of the real desktop, written as a .bmp onto the
 * share, plus just enough remote poking to get a dialog on screen in the first place.
 *
 *   rigshot shot <out.bmp>    capture the whole screen
 *   rigshot cmd  <n>          PostMessage(WM_COMMAND, n) to the VDM window
 *                             (n=8 is IDM_FILE_SETTINGS -- see the enum in main.c)
 *   rigshot key  <vk> [times] synthesise a keypress into the foreground window
 *   rigshot click <x> <y>     synthesise a left click at a screen coordinate
 *   rigshot fg   <caption>    bring a window to the foreground by exact caption
 *   rigshot list              dump every top-level window caption (diagnostic)
 *   rigshot isne <dir>        classify every .exe in <dir> as NE / PE / LE / MZ
 *                             -- i.e. FIND THE 16-BIT WINDOWS PROGRAMS (GH #129/#128)
 *
 * ⚠ WHY KEYS AND NOT MESSAGES for switching tabs. TCM_SETCURSEL crosses a process
 *   boundary fine, but it does NOT raise TCN_SELCHANGE, and the notification that
 *   would (WM_NOTIFY) carries a POINTER that Windows will not marshal between
 *   processes -- it would be read in the wrong address space. A synthetic arrow key
 *   into the focused tab control changes the selection AND fires the notification, by
 *   the same path a user's finger does. Slower, and actually the thing being tested.
 *
 * XP-safe build: no-CRT (reuses src/runtime.c), subsystem 5.01, imports only
 * KERNEL32/USER32/GDI32. See scripts/build-rigshot.sh.
 */
#include <windows.h>

#define SHARE "C:\\Documents and Settings\\All Users\\Documents\\ntvdmex"
#define VDM_TITLE "Microsoft Windows XP Virtual DOS Machine"
#define LOGF SHARE "\\rigshot.txt"

/* ── tiny no-CRT helpers ─────────────────────────────────────────────────────── */
static int slen(const char *s) { int n = 0; while (s[n]) ++n; return n; }

static char *sput(char *p, const char *s) { while (*s) *p++ = *s++; *p = 0; return p; }

static int seq(const char *a, const char *b)
{
    while (*a && *a == *b) { ++a; ++b; }
    return *a == *b;
}

/* Parse a decimal or 0x-prefixed hex integer. */
static int satoi(const char *s)
{
    int v = 0, hex = 0;
    while (*s == ' ') ++s;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) { hex = 1; s += 2; }
    while (*s) {
        int d;
        if (*s >= '0' && *s <= '9')            d = *s - '0';
        else if (hex && *s >= 'a' && *s <= 'f') d = *s - 'a' + 10;
        else if (hex && *s >= 'A' && *s <= 'F') d = *s - 'A' + 10;
        else break;
        v = v * (hex ? 16 : 10) + d;
        ++s;
    }
    return v;
}

static void logline(const char *s)
{
    DWORD wr;
    HANDLE h = CreateFileA(LOGF, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    WriteFile(h, s, (DWORD)slen(s), &wr, NULL);
    WriteFile(h, "\r\n", 2, &wr, NULL);
    CloseHandle(h);
}

/* ── the desktop capture ─────────────────────────────────────────────────────── */
static int do_shot(const char *path)
{
    HDC scr = NULL, mem = NULL;
    HBITMAP bmp = NULL, old = NULL;
    BITMAPINFO bi;
    BYTE *bits = NULL, hdr[54];
    DWORD w, h, stride, imgsz, wr;
    HANDLE f;
    int ok = 0;

    w = (DWORD)GetSystemMetrics(SM_CXSCREEN);
    h = (DWORD)GetSystemMetrics(SM_CYSCREEN);
    if (!w || !h) { logline("shot: zero screen metrics"); return 1; }

    scr = GetDC(NULL);
    if (!scr) { logline("shot: GetDC(NULL) failed"); return 1; }
    mem = CreateCompatibleDC(scr);
    bmp = CreateCompatibleBitmap(scr, (int)w, (int)h);
    if (!mem || !bmp) { logline("shot: CreateCompatible* failed"); goto done; }
    old = (HBITMAP)SelectObject(mem, bmp);
    /* SRCCOPY of the screen DC: this is the composited desktop, so it picks up the
       VDM window, its menu, its status strip and any dialog on top of it. */
    if (!BitBlt(mem, 0, 0, (int)w, (int)h, scr, 0, 0, SRCCOPY)) {
        logline("shot: BitBlt failed"); goto done;
    }

    stride = ((w * 3u) + 3u) & ~3u;            /* DIB rows are 4-byte aligned */
    imgsz  = stride * h;
    bits   = (BYTE *)VirtualAlloc(NULL, imgsz, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!bits) { logline("shot: VirtualAlloc failed"); goto done; }

    { int i; for (i = 0; i < (int)sizeof bi; ++i) ((BYTE *)&bi)[i] = 0; }
    bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth       = (LONG)w;
    bi.bmiHeader.biHeight      = (LONG)h;      /* positive = bottom-up, as BMP wants */
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount    = 24;
    bi.bmiHeader.biCompression = BI_RGB;
    if (!GetDIBits(mem, bmp, 0, h, bits, &bi, DIB_RGB_COLORS)) {
        logline("shot: GetDIBits failed"); goto done;
    }

    /* Write the 14-byte BITMAPFILEHEADER by hand. Its declared alignment differs
       between compilers and one stray pad byte makes an unopenable file. */
    { int i; for (i = 0; i < 54; ++i) hdr[i] = 0; }
    hdr[0] = 'B'; hdr[1] = 'M';
    *(DWORD *)(hdr + 2)  = 54 + imgsz;         /* file size            */
    *(DWORD *)(hdr + 10) = 54;                 /* offset to the bits   */
    *(DWORD *)(hdr + 14) = sizeof(BITMAPINFOHEADER);
    *(LONG  *)(hdr + 18) = (LONG)w;
    *(LONG  *)(hdr + 22) = (LONG)h;
    *(WORD  *)(hdr + 26) = 1;
    *(WORD  *)(hdr + 28) = 24;
    *(DWORD *)(hdr + 34) = imgsz;

    f = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, NULL,
                    CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) { logline("shot: cannot create output"); goto done; }
    WriteFile(f, hdr,  54,    &wr, NULL);
    WriteFile(f, bits, imgsz, &wr, NULL);
    CloseHandle(f);
    ok = 1;

done:
    if (bits) VirtualFree(bits, 0, MEM_RELEASE);
    if (old)  SelectObject(mem, old);
    if (bmp)  DeleteObject(bmp);
    if (mem)  DeleteDC(mem);
    if (scr)  ReleaseDC(NULL, scr);
    { char m[320], *p = m;
      p = sput(p, ok ? "shot: wrote " : "shot: FAILED ");
      p = sput(p, path);
      logline(m); }
    return ok ? 0 : 1;
}

/* ── remote poking ───────────────────────────────────────────────────────────── */
static BOOL CALLBACK enum_cb(HWND h, LPARAM lp)
{
    char cap[256], line[400], *p = line;
    (void)lp;
    if (!IsWindowVisible(h)) return TRUE;
    cap[0] = 0;
    GetWindowTextA(h, cap, sizeof cap);
    if (!cap[0]) return TRUE;
    p = sput(p, "  win: ");
    p = sput(p, cap);
    logline(line);
    return TRUE;
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmdline, int show)
{
    char *a = GetCommandLineA();
    char verb[32], arg1[300], arg2[32];
    int i = 0, n = 0;
    (void)inst; (void)prev; (void)cmdline; (void)show;

    /* Skip argv[0], which may be quoted. */
    if (*a == '"') { ++a; while (*a && *a != '"') ++a; if (*a) ++a; }
    else           { while (*a && *a != ' ') ++a; }
    while (*a == ' ') ++a;

    for (i = 0; a[i] && a[i] != ' ' && i < 31; ++i) verb[i] = a[i];
    verb[i] = 0;
    a += i; while (*a == ' ') ++a;
    /* arg1 may be a quoted path (the share has spaces in it). */
    if (*a == '"') {
        ++a;
        for (i = 0; a[i] && a[i] != '"' && i < 299; ++i) arg1[i] = a[i];
        arg1[i] = 0; a += i; if (*a == '"') ++a;
    } else {
        for (i = 0; a[i] && a[i] != ' ' && i < 299; ++i) arg1[i] = a[i];
        arg1[i] = 0; a += i;
    }
    while (*a == ' ') ++a;
    for (i = 0; a[i] && a[i] != ' ' && i < 31; ++i) arg2[i] = a[i];
    arg2[i] = 0;

    if (seq(verb, "shot"))
        return do_shot(arg1[0] ? arg1 : (SHARE "\\rigshot.bmp"));

    if (seq(verb, "list")) {
        logline("list: visible top-level windows");
        EnumWindows(enum_cb, 0);
        return 0;
    }

    /* ── WHICH EXECUTABLES HERE ARE 16-BIT WINDOWS? ──────────────────────────────
         An .exe's real type is in its header, not its name: `MZ` at 0, then a
         LONG at 0x3C giving the offset of the second header -- `NE` (16-bit
         Windows / OS-2), `PE` (Win32), `LE`/`LX` (a DOS extender's linear image),
         or nothing at all (a plain DOS MZ). Windows dispatches an NE to WOW, i.e.
         to ntvdm.exe, which is why finding one is the prerequisite for measuring
         a Win16 launch. */
    if (seq(verb, "isne")) {
        WIN32_FIND_DATAA fd;
        char pat[320], *q; HANDLE h; int n_ne = 0, n_pe = 0, n_other = 0;
        q = sput(pat, arg1[0] ? arg1 : "C:\\WINDOWS\\system32");
        q = sput(q, "\\*.exe");
        { char m[400], *mp = m; mp = sput(mp, "isne: scanning "); sput(mp, pat); logline(m); }
        h = FindFirstFileA(pat, &fd);
        if (h == INVALID_HANDLE_VALUE) { logline("isne: nothing found"); return 1; }
        do {
            char full[512], *fp = full, line[600], *lp = line;
            BYTE hdr[4]; LONG lfa = 0; DWORD got = 0; HANDLE f;
            fp = sput(full, arg1[0] ? arg1 : "C:\\WINDOWS\\system32");
            fp = sput(fp, "\\"); sput(fp, fd.cFileName);
            f = CreateFileA(full, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                            NULL, OPEN_EXISTING, 0, NULL);
            if (f == INVALID_HANDLE_VALUE) continue;
            ReadFile(f, hdr, 2, &got, NULL);
            if (got == 2 && hdr[0] == 'M' && hdr[1] == 'Z') {
                SetFilePointer(f, 0x3C, NULL, FILE_BEGIN);
                ReadFile(f, &lfa, 4, &got, NULL);
                if (got == 4 && lfa > 0 && lfa < 0x10000000) {
                    SetFilePointer(f, lfa, NULL, FILE_BEGIN);
                    hdr[0] = hdr[1] = 0;
                    ReadFile(f, hdr, 2, &got, NULL);
                    if (got == 2 && hdr[0] == 'N' && hdr[1] == 'E') {
                        /* The one we are hunting. Report it loudly. */
                        lp = sput(line, "  NE  <-- 16-bit Windows: ");
                        sput(lp, fd.cFileName);
                        logline(line);
                        ++n_ne;
                    } else if (got == 2 && hdr[0] == 'P' && hdr[1] == 'E') ++n_pe;
                    else ++n_other;
                } else ++n_other;      /* plain DOS MZ, no second header */
            } else ++n_other;
            CloseHandle(f);
        } while (FindNextFileA(h, &fd));
        FindClose(h);
        { char m[200], *mp = m; char num[16];
          wsprintfA(num, "%d", n_ne);  mp = sput(m, "isne: NE=");    mp = sput(mp, num);
          wsprintfA(num, "%d", n_pe);  mp = sput(mp, "  PE=");       mp = sput(mp, num);
          wsprintfA(num, "%d", n_other); mp = sput(mp, "  other="); sput(mp, num);
          logline(m); }
        return 0;
    }

    if (seq(verb, "cmd")) {
        HWND w = FindWindowA(NULL, VDM_TITLE);
        char m[200], *p = m;
        if (!w) { logline("cmd: VDM window not found"); return 1; }
        PostMessageA(w, WM_COMMAND, (WPARAM)satoi(arg1), 0);
        p = sput(p, "cmd: posted WM_COMMAND "); p = sput(p, arg1);
        logline(m);
        return 0;
    }

    if (seq(verb, "fg")) {
        HWND w = FindWindowA(NULL, arg1);
        char m[400], *p = m;
        p = sput(p, w ? "fg: found " : "fg: NOT FOUND "); p = sput(p, arg1);
        logline(m);
        if (!w) return 1;
        /* SW_RESTORE, not SW_SHOW: a MINIMIZED window is still WS_VISIBLE, so it
           lists and finds normally and then captures as bare desktop. SW_SHOW leaves
           it minimized; only SW_RESTORE brings it back. Cost a confused screenshot. */
        ShowWindow(w, IsIconic(w) ? SW_RESTORE : SW_SHOW);
        SetForegroundWindow(w);
        return 0;
    }

    /* A real click, at a real screen coordinate, through the real hit-test. Used to
       switch tab pages: TCM_SETCURSEL would move the selection without raising
       TCN_SELCHANGE, so the page would not follow -- which would silently "verify"
       a tab control that does not work. */
    if (seq(verb, "click")) {
        char m[200], *p = m;
        SetCursorPos(satoi(arg1), satoi(arg2));
        Sleep(80);
        mouse_event(MOUSEEVENTF_LEFTDOWN, 0, 0, 0, 0);
        Sleep(60);
        mouse_event(MOUSEEVENTF_LEFTUP, 0, 0, 0, 0);
        p = sput(p, "click: "); p = sput(p, arg1);
        p = sput(p, ","); p = sput(p, arg2);
        logline(m);
        return 0;
    }

    if (seq(verb, "key")) {
        int vk = satoi(arg1);
        char m[200], *p = m;
        n = arg2[0] ? satoi(arg2) : 1;
        for (i = 0; i < n; ++i) {
            keybd_event((BYTE)vk, 0, 0, 0);
            Sleep(40);
            keybd_event((BYTE)vk, 0, KEYEVENTF_KEYUP, 0);
            Sleep(120);
        }
        p = sput(p, "key: sent vk="); p = sput(p, arg1);
        p = sput(p, " times="); p = sput(p, arg2[0] ? arg2 : "1");
        logline(m);
        return 0;
    }

    logline("rigshot: unknown verb (shot|cmd|key|fg|list)");
    return 2;
}

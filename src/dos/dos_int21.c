/* dos_int21.c -- see dos_int21.h. Faithful port of the INT 21h handlers from
 * tools/vdmhost/vdmhost.c; AH=48/49/4A delegate to the shared dos_mcb.h allocator. */
#include "dos_int21.h"
#include "dos_mcb.h"
#include "dos_layout.h"
#include "log.h"          /* zput / zhex */

/* Copy an ASCIIZ string out of V86 memory (seg:off) into a host buffer. */
static void v86_str(DWORD seg, DWORD off, char *dst, int max)
{
    const volatile BYTE *s = (const volatile BYTE *)((seg << 4) + (off & 0xFFFF));
    int i;
    for (i = 0; i < max - 1 && s[i]; ++i) dst[i] = (char)s[i];
    dst[i] = 0;
}

void dos_int21_init(dos_machine_t *m, uint16_t first_mcb)
{
    int i;
    for (i = 0; i < 24; ++i) m->fh[i] = 0;
    m->first_mcb = first_mcb;
    m->dta_seg = DOS_PSP_SEG;
    m->dta_off = 0x0080;
    m->out_len = 0;
    m->exit_code = 0;
    m->conout = 0;
    m->conctx = 0;
    m->conin = 0;
    m->cinctx = 0;
    m->coninnb = 0;
    m->conpeek = 0;
}

int dos_int21(dos_machine_t *m)
{
    volatile BYTE *tib = m->tib;
    char *tp = m->tp;
    volatile WORD *pfl;
    DWORD ah;
    int cont = 1;

    #define R_AX VDM_REG(tib, VTIB_EAX)
    #define R_BX VDM_REG(tib, VTIB_EBX)
    #define R_CX VDM_REG(tib, VTIB_ECX)
    #define R_DX VDM_REG(tib, VTIB_EDX)
    #define R_DS VDM_REG(tib, VTIB_DS)
    #define R_ES VDM_REG(tib, VTIB_ES)
    #define R_SI VDM_REG(tib, VTIB_ESI)
    #define SETAX(v)    (R_AX = (R_AX & 0xFFFF0000u) | ((DWORD)(v) & 0xFFFF))
    #define SET16(r, v) ((r)  = ((r)  & 0xFFFF0000u) | ((DWORD)(v) & 0xFFFF))
    #define OKCF()      (*pfl &= (WORD)~1)
    #define ERRCF()     (*pfl |= 1)
    #define SETZF()     (*pfl |= 0x40)
    #define CLRZF()     (*pfl &= (WORD)~0x40)
    #define OUTC(c)     do { uint8_t _ch = (uint8_t)(c); \
        if (m->out_len < m->out_cap - 1) m->out[m->out_len++] = (char)_ch; \
        if (m->conout) m->conout(m->conctx, _ch); } while (0)

    /* CF is returned via the FLAGS the INT pushed on the V86 stack (SS:SP+4): the
       handler's IRET restores FLAGS from there, so the live EFlags get clobbered. */
    pfl = (volatile WORD *)(((VDM_REG(tib, VTIB_SS) & 0xFFFF) << 4)
                            + (((VDM_REG(tib, VTIB_ESP) & 0xFFFF) + 4) & 0xFFFF));
    ah = (R_AX >> 8) & 0xFF;

    if (ah == 0x4C) {                           /* terminate */
        m->exit_code = (int)(R_AX & 0xFF);      /* DOS errorlevel */
        tp = zput(tp, "  ==> DOS terminate (AH=4Ch), exit code AL=0x");
        tp = zhex(tp, R_AX & 0xFF); tp = zput(tp, "\r\n");
        cont = 0;
    } else if (ah == 0x02) {                    /* print char DL */
        OUTC(R_DX & 0xFF); OKCF();
    } else if (ah == 0x01 || ah == 0x07 || ah == 0x08) {   /* read char (01 echoes) */
        int c = m->conin ? m->conin(m->cinctx) : 0;
        if (ah == 0x01) OUTC(c);                /* AH=01: echo                     */
        SETAX((R_AX & 0xFF00) | (c & 0xFF)); OKCF();
    } else if (ah == 0x0A) {                    /* buffered input DS:DX */
        volatile BYTE *buf = (volatile BYTE *)((R_DS << 4) + (R_DX & 0xFFFF));
        int maxn = buf[0], n = 0, c;
        while (n < maxn - 1) {
            c = m->conin ? m->conin(m->cinctx) : 0x0D;
            if (c == 0x0D) break;
            if (c == 0x08) { if (n > 0) { --n; OUTC(0x08); OUTC(' '); OUTC(0x08); } continue; }
            buf[2 + n++] = (BYTE)c; OUTC(c);
        }
        buf[1] = (BYTE)n; buf[2 + n] = 0x0D;
        OUTC(0x0D); OUTC(0x0A);
        OKCF();
    } else if (ah == 0x0B) {                    /* check input status */
        int ready = m->conpeek ? m->conpeek(m->cinctx) : 0;
        SETAX((R_AX & 0xFF00) | (ready ? 0xFF : 0x00));   /* FFh = char waiting */
        OKCF();
    } else if (ah == 0x06) {                    /* direct console I/O (DL=FF -> read) */
        if ((R_DX & 0xFF) == 0xFF) {            /* input: non-blocking, ZF=1 if none */
            int c = m->coninnb ? m->coninnb(m->cinctx) : -1;
            if (c >= 0) { SETAX((R_AX & 0xFF00) | (c & 0xFF)); CLRZF(); }
            else        { SETAX(R_AX & 0xFF00); SETZF(); }
        } else {                                /* output: write DL, AL=DL */
            OUTC(R_DX & 0xFF); SETAX((R_AX & 0xFF00) | (R_DX & 0xFF));
        }
        OKCF();
    } else if (ah == 0x09) {                    /* print $-string DS:DX */
        const volatile BYTE *s = (const volatile BYTE *)((R_DS << 4) + (R_DX & 0xFFFF));
        int k; for (k = 0; k < 1024 && *s != '$'; ++k, ++s) OUTC(*s);
        OKCF();
    } else if (ah == 0x40) {                    /* write: BX=handle CX=cnt DS:DX=buf */
        DWORD h = R_BX & 0xFFFF, cnt = R_CX & 0xFFFF;
        const char *b = (const char *)((R_DS << 4) + (R_DX & 0xFFFF));
        if (h == 1 || h == 2) { DWORD k; for (k = 0; k < cnt; ++k) OUTC(b[k]); SETAX(cnt); OKCF(); }
        else if (h < 24 && m->fh[h]) { DWORD w = 0; WriteFile(m->fh[h], b, cnt, &w, NULL); SETAX(w); OKCF(); }
        else { SETAX(6); ERRCF(); }
    } else if (ah == 0x3C || ah == 0x3D) {      /* create / open: DS:DX=ASCIIZ name */
        char fn[300]; DWORD slot; HANDLE f;
        v86_str(R_DS, R_DX, fn, sizeof(fn));
        if (ah == 0x3C)
            f = CreateFileA(fn, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ,
                            NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        else {
            DWORD mode = R_AX & 3;
            DWORD acc = (mode == 1) ? GENERIC_WRITE
                      : (mode == 2) ? (GENERIC_READ | GENERIC_WRITE) : GENERIC_READ;
            f = CreateFileA(fn, acc, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                            FILE_ATTRIBUTE_NORMAL, NULL);
        }
        if (f != INVALID_HANDLE_VALUE) {
            for (slot = 5; slot < 24 && m->fh[slot]; ++slot) {}
            if (slot < 24) { m->fh[slot] = f; SETAX(slot); OKCF(); }
            else { CloseHandle(f); SETAX(4); ERRCF(); }
        } else { SETAX(2); ERRCF(); }
        tp = zput(tp, "  INT21 AH=0x"); tp = zhex(tp, ah);
        tp = zput(tp, " ["); tp = zput(tp, fn); tp = zput(tp, "] -> AX=0x");
        tp = zhex(tp, R_AX & 0xFFFF); tp = zput(tp, (*pfl & 1) ? " (err)\r\n" : "\r\n");
    } else if (ah == 0x3E) {                    /* close: BX=handle */
        DWORD h = R_BX & 0xFFFF;
        if (h >= 5 && h < 24 && m->fh[h]) { CloseHandle(m->fh[h]); m->fh[h] = 0; }
        OKCF();
    } else if (ah == 0x3F) {                    /* read: BX=handle CX=cnt -> DS:DX */
        DWORD h = R_BX & 0xFFFF, cnt = R_CX & 0xFFFF, rd = 0;
        void *b = (void *)((R_DS << 4) + (R_DX & 0xFFFF));
        if (h >= 5 && h < 24 && m->fh[h]) { ReadFile(m->fh[h], b, cnt, &rd, NULL); SETAX(rd); OKCF(); }
        else if (h == 0) { SETAX(0); OKCF(); }  /* stdin: EOF for now */
        else { SETAX(6); ERRCF(); }
    } else if (ah == 0x42) {                    /* lseek: AL=org BX=h CX:DX=off */
        DWORD h = R_BX & 0xFFFF, meth = R_AX & 0xFF;
        LONG dist = (LONG)(((R_CX & 0xFFFF) << 16) | (R_DX & 0xFFFF));
        if (h >= 5 && h < 24 && m->fh[h]) {
            DWORD np = SetFilePointer(m->fh[h], dist, NULL, meth);
            SETAX(np & 0xFFFF);
            R_DX = (R_DX & 0xFFFF0000u) | ((np >> 16) & 0xFFFF); OKCF();
        } else { SETAX(6); ERRCF(); }
    } else if (ah == 0x30) {                    /* get DOS version -> 5.0 */
        SETAX(0x0005); OKCF();
    } else if (ah == 0x44) {                    /* IOCTL (C-runtime isatty etc.) */
        BYTE al = (BYTE)(R_AX & 0xFF);
        WORD bx = (WORD)(R_BX & 0xFFFF);
        if (al == 0x00)        { SET16(R_DX, (bx < 5) ? 0x80D3 : 0x0002); OKCF(); }
        else if (al == 0x06 || al == 0x07) { SETAX((R_AX & 0xFF00) | 0xFF); OKCF(); }
        else                   { OKCF(); }
        tp = zput(tp, "  INT21 AH=44 ioctl AL=0x"); tp = zhex(tp, al);
        tp = zput(tp, " BX=0x"); tp = zhex(tp, bx); tp = zput(tp, "\r\n");
    } else if (ah == 0x63) {                    /* get DBCS lead-byte table */
        if ((R_AX & 0xFF) == 0) { SET16(R_DS, DOS_HDLR_SEG); SET16(R_SI, DOS_DBCS_OFF); }
        SETAX(R_AX & 0xFF00); OKCF();
        tp = zput(tp, "  INT21 AH=63 DBCS lead-byte table\r\n");
    } else if (ah == 0x25) {                    /* set interrupt vector: AL=int DS:DX */
        DWORD v = (R_AX & 0xFF) * 4;
        *(volatile WORD *)(v)     = (WORD)(R_DX & 0xFFFF);
        *(volatile WORD *)(v + 2) = (WORD)(R_DS & 0xFFFF);
        OKCF();
    } else if (ah == 0x35) {                    /* get interrupt vector: AL=int -> ES:BX */
        DWORD v = (R_AX & 0xFF) * 4;
        SET16(R_BX, *(volatile WORD *)(v));
        SET16(R_ES, *(volatile WORD *)(v + 2));
        OKCF();
    } else if (ah == 0x48) {                    /* allocate BX paras -> AX=seg (err: BX=max) */
        uint16_t want = (uint16_t)(R_BX & 0xFFFF), seg = 0, max = 0;
        int err = dos_alloc(NULL, m->first_mcb, want, &seg, &max);
        if (err) { SET16(R_AX, err); SET16(R_BX, max); ERRCF(); }
        else     { SET16(R_AX, seg); OKCF(); }
        tp = zput(tp, "  INT21 AH=48 alloc 0x"); tp = zhex(tp, want);
        tp = zput(tp, (*pfl & 1) ? " -> err max=0x" : " -> seg=0x");
        tp = zhex(tp, (*pfl & 1) ? max : (R_AX & 0xFFFF)); tp = zput(tp, "\r\n");
    } else if (ah == 0x49) {                    /* free block: ES=segment */
        int err = dos_free(NULL, (uint16_t)(R_ES & 0xFFFF));
        if (err) { SET16(R_AX, err); ERRCF(); } else OKCF();
        tp = zput(tp, "  INT21 AH=49 free seg=0x"); tp = zhex(tp, R_ES & 0xFFFF);
        tp = zput(tp, (*pfl & 1) ? " (err)\r\n" : "\r\n");
    } else if (ah == 0x4A) {                    /* resize: ES=block BX=new paras */
        uint16_t want = (uint16_t)(R_BX & 0xFFFF), max = 0;
        int err = dos_resize(NULL, (uint16_t)(R_ES & 0xFFFF), want, &max);
        if (err) { SET16(R_AX, err); if (err == 8) SET16(R_BX, max); ERRCF(); }
        else OKCF();
        tp = zput(tp, "  INT21 AH=4A resize seg=0x"); tp = zhex(tp, R_ES & 0xFFFF);
        tp = zput(tp, " -> 0x"); tp = zhex(tp, want);
        tp = zput(tp, (*pfl & 1) ? " (err)\r\n" : "\r\n");
    } else if (ah == 0x51 || ah == 0x62) {      /* get current PSP -> BX */
        SET16(R_BX, DOS_PSP_SEG); OKCF();
    } else if (ah == 0x50) {                    /* set current PSP (single process) */
        OKCF();
    } else if (ah == 0x1A) {                    /* set DTA = DS:DX */
        m->dta_seg = (WORD)(R_DS & 0xFFFF); m->dta_off = (WORD)(R_DX & 0xFFFF); OKCF();
    } else if (ah == 0x2F) {                    /* get DTA -> ES:BX */
        SET16(R_ES, m->dta_seg); SET16(R_BX, m->dta_off); OKCF();
    } else if (ah == 0x19) {                    /* get current drive -> AL (C: = 2) */
        SETAX((R_AX & 0xFF00) | 0x02); OKCF();
    } else if (ah == 0x0E) {                    /* select drive -> AL = #drives */
        SETAX((R_AX & 0xFF00) | 0x03); OKCF();
    } else if (ah == 0x0D) {                    /* disk reset (flush) -> nop */
        OKCF();
    } else if (ah == 0x33) {                    /* get/set Ctrl-Break -> off */
        if ((R_AX & 0xFF) == 0) SET16(R_DX, 0);
        OKCF();
    } else if (ah == 0x2A) {                    /* get date: CX=yr DH=mon DL=day AL=dow */
        SYSTEMTIME t; GetLocalTime(&t);
        SET16(R_CX, t.wYear);
        SET16(R_DX, ((t.wMonth & 0xFF) << 8) | (t.wDay & 0xFF));
        SETAX((R_AX & 0xFF00) | (t.wDayOfWeek & 0xFF));
        OKCF();
    } else if (ah == 0x2C) {                    /* get time: CH=hr CL=min DH=sec DL=cs */
        SYSTEMTIME t; GetLocalTime(&t);
        SET16(R_CX, ((t.wHour & 0xFF) << 8) | (t.wMinute & 0xFF));
        SET16(R_DX, ((t.wSecond & 0xFF) << 8) | ((t.wMilliseconds / 10) & 0xFF));
        OKCF();
    } else if (ah == 0x2B || ah == 0x2D) {      /* set date/time -> report success */
        SETAX(R_AX & 0xFF00);                   /* AL=0 = ok */
        OKCF();
    } else {                                    /* unhandled service */
        tp = zput(tp, "  INT21 AH=0x"); tp = zhex(tp, ah); tp = zput(tp, " unhandled\r\n");
        ERRCF();
    }

    m->tp = tp;
    #undef R_AX
    #undef R_BX
    #undef R_CX
    #undef R_DX
    #undef R_DS
    #undef R_ES
    #undef R_SI
    #undef SETAX
    #undef SET16
    #undef OKCF
    #undef ERRCF
    #undef SETZF
    #undef CLRZF
    #undef OUTC
    return cont;
}

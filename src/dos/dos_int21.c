/* dos_int21.c -- see dos_int21.h. Faithful port of the INT 21h handlers from
 * tools/vdmhost/vdmhost.c; AH=48/49/4A delegate to the shared dos_mcb.h allocator. */
#include "dos_int21.h"
#include "dos_mcb.h"
#include "dos_layout.h"
#include "dos_fh.h"       /* the handle table's two rules -- allocation + classification */
#include "dos_err.h"      /* AH=59h class/action/locus, measured on the oracle */
#include "log.h"          /* zput / zhex */
#include "dos_ctab.h"     /* CP437 tables dumped from the 6.22 oracle */

/* Set when the caller is servicing INT 21h for a client that is still in PROTECTED
   mode (a DPMI client), so CF/ZF go to the live VTIB_EFLAGS instead of a pushed V86
   FLAGS frame that does not exist there. See the pfl assignment below. */
int g_dos_int21_pm = 0;
void dos_int21_set_pm(int on) { g_dos_int21_pm = on ? 1 : 0; }

/* Does MS-DOS 6.22 provide a MEANINGFUL service at this AH?  GH #27.
 *
 * This is the line between "we are missing something" and "DOS has nothing here
 * either", and the two need opposite behaviour: the first must fail loudly so a
 * no-op is never mistaken for success, the second must stay silent so we do not
 * invent an error real DOS never reports.
 *
 * THE BOUNDARY IS MEASURED, NOT REMEMBERED (tools/dostest/p_defs.asm, run against
 * the 6.22 oracle).  Every probed AH from 6Dh upward -- 6Dh, 6Eh, 6Fh, 70h, 71h,
 * 72h, 74h, 80h, A0h, E0h -- returns with AX unchanged, CF clear and every
 * poisoned output register still holding its poison: nothing happened at all.
 * 6Ch is the highest that does anything.
 *
 * The exceptions inside the table were measured the same way and carry the same
 * do-nothing signature, so they belong on the quiet side: 18h, 1Dh, 1Eh and 20h
 * are DOS's documented internal null functions, 61h is reserved, 6Bh is a null
 * function from DOS 5 on.
 *
 * A CAUTION FOR WHOEVER EXTENDS THIS: "AX came back unchanged" is NOT on its own
 * a reliable test for absence.  AH=54h (get verify flag) returns AL=0 when verify
 * is off, which is indistinguishable from the AL=0 that went in, and AH=2Ch
 * leaves AX alone while writing CX and DX.  The signature that actually works is
 * EVERY output register still poisoned.
 */
static int dos622_defines(uint8_t ah)
{
    if (ah > 0x6C) return 0;
    switch (ah) {
    case 0x18: case 0x1D: case 0x1E: case 0x20:   /* internal null functions */
    case 0x61:                                    /* reserved                */
    case 0x6B:                                    /* null function (DOS 5+)  */
        return 0;
    default:
        return 1;
    }
}

/* MS-DOS 6.22 country block for country 1 (USA), INT 21h AH=38h.  GH #38.
 *
 * TRANSCRIBED FROM THE ORACLE, byte for byte (tools/dostest/p_ctry.asm):
 *   [0-1]  0000     date format, 0 = USA month/day/year
 *   [2-6]  "$"      currency symbol, ASCIIZ in 5 bytes
 *   [7-8]  ","      thousands separator      [9-10]  "."  decimal separator
 *   [11-12] "-"     date separator           [13-14] ":"  time separator
 *   [15]   00       currency format          [16]    02   digits after decimal
 *   [17]   00       time format, 0 = 12-hour
 *   [18-21]         FAR pointer to DOS's case-map routine -- filled in at run time
 *   [22-23] ","     data-list separator
 *
 * THE LENGTH IS MEASURED, NOT ASSUMED. The probe poisoned the destination with
 * 0xEE first: real DOS writes exactly 24 bytes and leaves everything past them
 * alone. The commonly quoted "34-byte block" would have had us zeroing 10 bytes
 * of the caller's memory that DOS never touches.
 */
static const uint8_t ctry_us[24] = {
    0x00, 0x00,
    0x24, 0x00, 0x00, 0x00, 0x00,
    0x2C, 0x00,   0x2E, 0x00,   0x2D, 0x00,   0x3A, 0x00,
    0x00,   0x02,   0x00,
    0x00, 0x00, 0x00, 0x00,               /* case-map FAR ptr, patched below */
    0x2C, 0x00
};

/* ---- INT 21h 4Eh/4Fh find-first/find-next.  GH #29. --------------------------
 *
 * DTA BLOCK LAYOUT, read off the oracle byte for byte (tools/dostest/p_find.asm).
 * The dump cross-checks itself: the size field came back 0xD575 = 54645, which is
 * COMMAND.COM's exact byte count.
 *
 *   [0]      drive number            [1-11]  11-byte search template
 *   [12]     search attributes       [13-14] directory entry number
 *   [15-16]  starting cluster        [17-20] reserved (DOS search state)
 *   [21]     attribute of the file found
 *   [22-23]  time      [24-25] date      [26-29] size (dword)
 *   [30-42]  filename, ASCIIZ, 13 bytes
 *
 * The block is 43 bytes: byte 43 came back still poisoned, so nothing beyond it
 * may be written.
 *
 * DOS keeps its own search state in the first 21 bytes.  We keep a Win32 handle
 * instead and stash the slot number there, so a program that saves and restores
 * its DTA between calls -- which real programs do -- resumes the right search.
 */
#define DOS_FIND_MAGIC 0x4E

static int dta_match(DWORD attr, uint16_t mask)
{
    /* DOS's rule is "normal files always match; these extras only if asked". */
    if ((attr & FILE_ATTRIBUTE_DIRECTORY) && !(mask & 0x10)) return 0;
    if ((attr & FILE_ATTRIBUTE_HIDDEN)    && !(mask & 0x02)) return 0;
    if ((attr & FILE_ATTRIBUTE_SYSTEM)    && !(mask & 0x04)) return 0;
    return 1;
}

static void dta_fill(volatile BYTE *d, const WIN32_FIND_DATAA *fd)
{
    FILETIME lf;
    WORD fdate = 0, ftime = 0;
    const char *nm = fd->cAlternateFileName[0] ? fd->cAlternateFileName : fd->cFileName;
    int k;
    if (FileTimeToLocalFileTime(&fd->ftLastWriteTime, &lf))
        FileTimeToDosDateTime(&lf, &fdate, &ftime);   /* DOS times are LOCAL */
    d[21] = (BYTE)(fd->dwFileAttributes & 0x3F);
    d[22] = (BYTE)(ftime & 0xFF);  d[23] = (BYTE)(ftime >> 8);
    d[24] = (BYTE)(fdate & 0xFF);  d[25] = (BYTE)(fdate >> 8);
    d[26] = (BYTE)( fd->nFileSizeLow        & 0xFF);
    d[27] = (BYTE)((fd->nFileSizeLow >> 8)  & 0xFF);
    d[28] = (BYTE)((fd->nFileSizeLow >> 16) & 0xFF);
    d[29] = (BYTE)((fd->nFileSizeLow >> 24) & 0xFF);
    for (k = 0; k < 12 && nm[k]; ++k) {
        char ch = nm[k];
        if (ch >= 'a' && ch <= 'z') ch = (char)(ch - 32);   /* DOS reports 8.3 upper */
        d[30 + k] = (BYTE)ch;
    }
    d[30 + k] = 0;
}

/* ---- The FCB interface (AH=0Fh-24h, 27h-29h).  GH #36. ----------------------
 *
 * The pre-1983 file API.  Little 6.22-era software uses it, but TREE.COM does,
 * and it is 19 of the 103 services 6.22 defines.
 *
 * MEASURED ON THE ORACLE (tools/dostest/p_fcb.asm), and the first fact matters
 * more than the rest: **FCB calls report success in AL (00 ok, FF fail), and the
 * CARRY FLAG IS UNDEFINED** -- a successful open came back with CF=1.  Anything
 * that treats carry as the result here is reading noise.
 *
 * An opened FCB comes back with bytes 0-31 filled and the current-record and
 * random-record fields (32-36) LEFT ALONE:
 *   [0] drive  [1-8] name  [9-11] ext  [12-13] current block
 *   [14-15] record size (128)  [16-19] file size  [20-21] date  [22-23] time
 *   [24-31] DOS's own workspace -- we keep our handle slot there
 *   [32] current record  [33-36] random record
 *
 * AH=11h/12h put a 33-byte directory entry in the DTA (that dump cross-checked
 * itself: the size field read 54645, COMMAND.COM's exact length):
 *   [0] drive  [1-11] name+ext  [12] attribute  [13-22] reserved
 *   [23-24] time  [25-26] date  [27-28] starting cluster  [29-32] size
 */
#define FCB_MAGIC 0x46

static volatile BYTE *fcb_at(DWORD seg, DWORD off)
{
    volatile BYTE *f = (volatile BYTE *)((seg << 4) + (off & 0xFFFF));
    return (f[0] == 0xFF) ? f + 7 : f;      /* skip an extended FCB's prefix */
}

/* Build "D:NAME.EXT" from an FCB's drive/name/extension fields. */
static void fcb_name(const volatile BYTE *f, char *out)
{
    int i, n = 0;
    if (f[0]) { out[n++] = (char)('A' + f[0] - 1); out[n++] = ':'; }
    for (i = 1; i <= 8 && f[i] != ' '; ++i) out[n++] = (char)f[i];
    if (f[9] != ' ') {
        out[n++] = '.';
        for (i = 9; i <= 11 && f[i] != ' '; ++i) out[n++] = (char)f[i];
    }
    out[n] = 0;
}

/* ── A DOS FILENAME ENDS AT A TERMINATOR, NOT ONLY AT A NUL. ─────────────────────
     The set is DOS's own, and we already publish it to guests as the AH=65h AL=05
     "filename terminator" table (dos_ctab.h): every control character and the space
     (0x00-0x20), plus the punctuation that separates a path from what follows.
     '.' is not here -- it is the extension separator and the caller handles it.
   ⚠ WHY THIS IS NOT COSMETIC. A command line is terminated by 0x0D, and the name
     builder below used to copy that CR straight into the FCB. COMMAND.COM matches
     its internal command table by comparing the entry's characters and then checking
     that the NEXT byte of the FCB is blank -- so `ver` parsed to "VER\r    " and
     missed, while `ver ` parsed to "VER \r   " and hit purely because the user had
     typed the blank we should have supplied. That is why every internal command was
     "Bad command or file name" until you put a space after it. */
static int fcb_name_ends(unsigned char c)
{
    if (c <= 0x20) return 1;                    /* NUL, CR, TAB, space, any control */
    return c == '"' || c == '/' || c == '\\' || c == '[' || c == ']' || c == ':'
        || c == '|' || c == '<'  || c == '>'  || c == '+' || c == '=' || c == ';'
        || c == ',';
}

static void fcb_put_name(volatile BYTE *d, const char *nm)
{
    int i = 0, k;
    for (k = 0; k < 11; ++k) d[k] = ' ';
    /* "." AND ".." ARE NAMES, NOT EXTENSIONS. The rule below ends the name at the
       first '.', which for these two directory entries ends it at character zero and
       leaves eleven blanks -- DIR then printed an empty column where the oracle
       shows "." and "..". DOS stores them literally in the name field. */
    if (nm[0] == '.') {
        d[0] = '.';
        if (nm[1] == '.' && (nm[2] == 0 || nm[2] == '.')) d[1] = '.';
        if (nm[1] == 0 || nm[1] == '.') return;
    }
    for (k = 0; k < 8 && !fcb_name_ends((unsigned char)nm[i]) && nm[i] != '.'; ++k, ++i)
        d[k] = (BYTE)(nm[i] >= 'a' && nm[i] <= 'z' ? nm[i] - 32 : nm[i]);
    while (!fcb_name_ends((unsigned char)nm[i]) && nm[i] != '.') ++i;
    if (nm[i] == '.') ++i;
    for (k = 8; k < 11 && !fcb_name_ends((unsigned char)nm[i]); ++k, ++i)
        d[k] = (BYTE)(nm[i] >= 'a' && nm[i] <= 'z' ? nm[i] - 32 : nm[i]);
}

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
    for (i = 0; i < DOS_MAX_FILES; ++i) m->fh[i] = 0;
    for (i = 0; i < 8; ++i) m->find_h[i] = 0;
    m->last_err = 0;
    m->verify = 0;
    m->child_rc = 0;
    m->fcb_find = 0;
    m->switch_char = '/';   /* oracle-confirmed 6.22 default */
    m->psp_seg = DOS_PSP_SEG;
    m->exec_pending = 0;
    m->first_mcb = first_mcb;
    m->dta_seg = DOS_PSP_SEG;
    m->dta_off = 0x0080;
    m->out_len = 0; m->out_trunc = 0;
    m->line_active = 0; m->line_n = 0; m->line_seg = 0; m->line_off = 0;
    m->std_open = 0x1F;                     /* stdin/stdout/stderr/aux/prn all open */
    { int _i; for (_i = 0; _i < 32; ++_i) { m->unimpl21[_i] = 0; m->noop21[_i] = 0; } }
    m->exit_code = 0;
    /* GH #28: default to 6.22 so we match the oracle. It is also the friendlier
       lie -- most version checks are floor checks, and real 6.22 tools refuse to
       run at all under a lower number ("Incorrect DOS version" from MEM.EXE was
       the first thing the evidence pass hit). */
    m->ver_major = 6; m->ver_minor = 22;
    /* Oracle-confirmed 6.22 defaults: 5800h -> AX=0000 (first fit),
       5802h -> AL=00 (UMBs not linked). */
    m->alloc_strat = 0; m->umb_link = 0;
    m->sysvars_seg = 0; m->sysvars_off = 0;
    m->conout = 0;
    m->conctx = 0;
    m->conin = 0;
    m->cinctx = 0;
    m->coninnb = 0;
    m->conpeek = 0;
}

/* See the header. The comment at AH=30h has promised this function since GH #28;
   COMMAND.COM is what finally needed it. */
void dos_int21_set_version(dos_machine_t *m, uint8_t major, uint8_t minor)
{
    if (!m || !major) return;                   /* major 0 is not a DOS version */
    m->ver_major = major; m->ver_minor = minor;
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
    #define R_DI VDM_REG(tib, VTIB_EDI)
    #define SETAX(v)    (R_AX = (R_AX & 0xFFFF0000u) | ((DWORD)(v) & 0xFFFF))
    #define SET16(r, v) ((r)  = ((r)  & 0xFFFF0000u) | ((DWORD)(v) & 0xFFFF))
    #define OKCF()      (*pfl &= (WORD)~1)
    #define ERRCF()     (*pfl |= 1)
    #define SETZF()     (*pfl |= 0x40)
    #define CLRZF()     (*pfl &= (WORD)~0x40)
    /* Dropping output silently once cost a wrong conclusion: a probe's dump was
       cut mid-line, the harness saw fewer results than it asked for, and the
       missing rows read as agreement. Record the drop so the log can say so. */
    /* ── AH=02h/09h WRITE TO STANDARD OUTPUT, NOT TO THE SCREEN. ─────────────────
         That distinction is invisible until something redirects, and then it is the
         whole feature: ECHO does not use AH=40h, it prints with AH=02h, so a shell
         doing `echo hello > hi.txt` sends the text through here. With this macro
         hard-wired to the console sink the redirect was accepted, the file was
         created, the text went to the SCREEN, and hi.txt was left empty at 0 bytes.
         Fixing AH=40h alone did not move it -- measured, twice -- because ECHO never
         goes near AH=40h.
         A bound handle 1 is a file (see the note there); an unbound one is the
         console, which is the ordinary case and behaves exactly as before. */
    #define OUTC(c)     do { uint8_t _ch = (uint8_t)(c); \
        if (m->fh[1]) { DWORD _w = 0; WriteFile(m->fh[1], &_ch, 1, &_w, NULL); } \
        else { \
            if (m->out_len < m->out_cap - 1) m->out[m->out_len++] = (char)_ch; \
            else m->out_trunc = 1; \
            if (m->conout) m->conout(m->conctx, _ch); \
        } } while (0)

    /* CF is returned via the FLAGS the INT pushed on the V86 stack (SS:SP+4): the
       handler's IRET restores FLAGS from there, so the live EFlags get clobbered.
       ► NOT IN PROTECTED MODE. A DPMI client's INT 21h is serviced by the host with the
         guest still in PM, where SS holds a SELECTOR -- so `SS<<4` is not the stack at
         all (0x1f -> linear 0x1f0) and this would both fail to return CF and scribble
         on low memory. There is no pushed-FLAGS frame to honour there either: the PM
         dispatcher resumes the client by advancing EIP past the BOP, so the live
         VTIB_EFLAGS *is* what the client sees. Point at its low word, which carries
         CF/ZF. Set by the PM caller via dos_int21_set_pm(). */
    pfl = g_dos_int21_pm
        ? (volatile WORD *)(tib + VTIB_EFLAGS)
        : (volatile WORD *)(((VDM_REG(tib, VTIB_SS) & 0xFFFF) << 4)
                            + (((VDM_REG(tib, VTIB_ESP) & 0xFFFF) + 4) & 0xFFFF));
    ah = (R_AX >> 8) & 0xFF;

    /* ── EVERY CALL, WHEN ASKED. ────────────────────────────────────────────────
         Most handlers here trace only what they think is interesting, which is fine
         until the question is "what does the guest do BETWEEN two calls we can see".
         COMMAND.COM accepts `ver ` and rejects `ver`, with a provably identical line
         buffer apart from one space -- so the answer is in the calls it makes after
         reading the line, and those are exactly the ones nothing prints. Two traces
         differing by one space is a DIFFERENTIAL experiment, which beats reasoning
         about a parser we cannot see.
       ⚠ AH=0Ah is excluded: it now polls via `retry`, so tracing it would bury the
         log in thousands of identical lines -- it prints its completed line instead.
         Gated by a flag file so no other run pays for this. */
    if (m->trace_all && ah != 0x0A) {
        tp = zput(tp, "  21:"); tp = zhexb(tp, (unsigned)ah);
        tp = zput(tp, "/");     tp = zhexb(tp, (unsigned)(R_AX & 0xFF));
        tp = zput(tp, " bx="); tp = zhexb(tp, (unsigned)((R_BX >> 8) & 0xFF));
        tp = zhexb(tp, (unsigned)(R_BX & 0xFF));
        tp = zput(tp, " dx="); tp = zhexb(tp, (unsigned)((R_DX >> 8) & 0xFF));
        tp = zhexb(tp, (unsigned)(R_DX & 0xFF));
        tp = zput(tp, "\r\n");
    }

    if (ah == 0x4C) {                           /* terminate */
        m->exit_code = (int)(R_AX & 0xFF);      /* DOS errorlevel */
        tp = zput(tp, "  ==> DOS terminate (AH=4Ch), exit code AL=0x");
        tp = zhex(tp, R_AX & 0xFF); tp = zput(tp, "\r\n");
        cont = 0;
    } else if (ah == 0x00) {                    /* terminate (CP/M style, = INT 20h) */
        /* Skyroads exits through this one, so "unhandled" was both wrong and misleading:
           we returned an error and let the guest run on into nowhere. It is just 4Ch with
           an exit code of 0. Logging the call site because WHY a game terminates is the
           question, and the CS tells you whether it was the program or something we
           vectored it into. */
        m->exit_code = 0;
        tp = zput(tp, "  ==> DOS terminate (AH=00h) from CS:IP=0x");
        tp = zhex(tp, VDM_REG(tib, VTIB_CS) & 0xFFFF); tp = zput(tp, ":0x");
        tp = zhex(tp, VDM_REG(tib, VTIB_EIP) & 0xFFFF);
        tp = zput(tp, " ivt8=0x");
        { const volatile BYTE *z = (const volatile BYTE *)0;
          DWORD s8 = (DWORD)z[0x22] | ((DWORD)z[0x23] << 8);
          DWORD o8 = (DWORD)z[0x20] | ((DWORD)z[0x21] << 8);
          DWORD sc = (DWORD)z[0x72] | ((DWORD)z[0x73] << 8);
          DWORD oc = (DWORD)z[0x70] | ((DWORD)z[0x71] << 8);
          tp = zhex(tp, s8); tp = zput(tp, ":0x"); tp = zhex(tp, o8);
          tp = zput(tp, " ivt1C=0x"); tp = zhex(tp, sc);
          tp = zput(tp, ":0x"); tp = zhex(tp, oc); }
        tp = zput(tp, "\r\n");
        cont = 0;
    } else if (ah == 0x02) {                    /* print char DL */
        OUTC(R_DX & 0xFF); OKCF();
    } else if (ah == 0x01 || ah == 0x07 || ah == 0x08) {   /* read char (01 echoes) */
        /* Poll, do not block. If no key is waiting we ask the host to re-run this INT
           rather than parking the exec thread -- see `retry` in dos_int21.h. */
        int c = m->coninnb ? m->coninnb(m->cinctx) : -1;
        if (c < 0) { m->retry = 1; }
        else {
            if (ah == 0x01) OUTC(c);            /* AH=01: echo                     */
            SETAX((R_AX & 0xFF00) | (c & 0xFF)); OKCF();
        }
    } else if (ah == 0x0A) {                    /* buffered input DS:DX */
        /* ── THE LAST INPUT CALL THAT PARKED THE EXEC THREAD, AND IT DEADLOCKS A SHELL.
             This used to sit in a loop on the BLOCKING m->conin until it had a whole
             line. AH=01/07/08 and INT 16h were both fixed years ago to poll via
             `retry` (see the note on that field), and the reason is spelled out
             there: blocking in C stops the GUEST dead. For a game that meant a frozen
             screen. For COMMAND.COM it means never running at all, because the thing
             it is waiting for CANNOT ARRIVE while it waits:
                 COMMAND.COM -> INT 21h AH=0Ah -> we block on the BIOS key ring
                 ...the BIOS key ring is filled by the guest's own INT 09h ISR
                 ...which cannot run, because we are blocked inside its INT 21h call.
             Measured exactly that way: the shell printed its banner and prompt, then
             40 scancodes went into the FIFO and IRQ1 was attempted 691 times, EVERY
             one refused as `not_in_exec`. The keys were there the whole time and the
             guest was never running to take them.
           ► SO COLLECT THE LINE ACROSS RETRIES. The characters accumulate in the
             GUEST's buffer (untouched between retries) and we keep only our position
             in it; a different DS:DX is a different call, not a continuation. Each
             retry leaves EIP on the BOP, so the guest re-executes the INT and gets to
             run its ISRs in between -- which is what a real DOS does, since the BIOS
             spins in the guest with interrupts enabled. */
        volatile BYTE *buf = (volatile BYTE *)((R_DS << 4) + (R_DX & 0xFFFF));
        int maxn = buf[0], c;
        if (!m->line_active || m->line_seg != (uint16_t)(R_DS & 0xFFFF)
                            || m->line_off != (uint16_t)(R_DX & 0xFFFF)) {
            m->line_active = 1; m->line_n = 0;
            m->line_seg = (uint16_t)(R_DS & 0xFFFF);
            m->line_off = (uint16_t)(R_DX & 0xFFFF);
        }
        for (;;) {
            if (m->line_n >= maxn - 1) break;           /* buffer full -> take it as a line */
            c = m->coninnb ? m->coninnb(m->cinctx) : 0x0D;
            if (c < 0) { m->retry = 1; break; }         /* nothing yet -> let the guest run */
            if (c == 0x0D) { m->line_active = 0; break; }
            if (c == 0x08) {                            /* backspace: rub it out on screen */
                if (m->line_n > 0) { --m->line_n; OUTC(0x08); OUTC(' '); OUTC(0x08); }
                continue;
            }
            if (c == 0x00) continue;                    /* extended key: no ASCII, ignore  */
            buf[2 + m->line_n++] = (BYTE)c; OUTC(c);
        }
        if (!m->retry) {
            buf[1] = (BYTE)m->line_n; buf[2 + m->line_n] = 0x0D;
            OUTC(0x0D); OUTC(0x0A);
            m->line_active = 0;
            /* WHAT THE SHELL ACTUALLY RECEIVES. `echo hi` works while a bare `ver`
               comes back "Bad command or file name" -- and the difference between
               them is a SPACE, i.e. whether the command word ends at a delimiter or
               at our terminator. That points straight at these bytes, so print them
               rather than reason about them. */
            if (m->trace_all) { int k;
              tp = zput(tp, "  INT21 AH=0A line max="); tp = zhexb(tp, (unsigned)maxn);
              tp = zput(tp, " n="); tp = zhexb(tp, (unsigned)m->line_n);
              tp = zput(tp, " [");
              for (k = 0; k < m->line_n + 1 && k < 64; ++k) {
                  tp = zhexb(tp, buf[2 + k]); tp = zput(tp, " ");
              }
              tp = zput(tp, "]\r\n"); }
            OKCF();
        }
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
        /* ── HANDLES 0-4 ARE TABLE ENTRIES, NOT A SPECIAL CASE. ──────────────────
             DOS pre-opens stdin/stdout/stderr/aux/prn as ordinary slots in the same
             handle table as everything else, and that is precisely WHY redirection
             works: the shell opens the file, dup2s it over handle 1, runs the
             command, then dup2s the saved copy back. Treating 1 and 2 as "the
             console, always" accepted the redirect and then ignored it -- measured,
             `echo hello world > hi.txt` printed to the screen and left an EMPTY
             hi.txt on disk, which is the worst of both.
             So: a BOUND handle is a file, whatever its number; only an unbound low
             handle is the console. */
        if (dos_fh_is_file((void *const *)m->fh, h)) { DWORD w = 0; WriteFile(m->fh[h], b, cnt, &w, NULL); SETAX(w); OKCF(); }
        /* ⚠ ANY device slot, not just 1 and 2 -- after AH=45h the console can be
             sitting in slot 5. Handles 3/4 stay out: they are AUX and PRN, which
             AH=04h/05h already accept and discard. A duplicate loses which device
             it was, so a dup of AUX would print here; nothing does that, and the
             alternative is a per-slot identity byte we have no caller for. */
        else if (dos_fh_is_device((void *const *)m->fh, m->std_open, h) && h != 3 && h != 4)
             { DWORD k; for (k = 0; k < cnt; ++k) OUTC(b[k]); SETAX(cnt); OKCF(); }
        else { SETAX(6); ERRCF(); }
    } else if (ah == 0x3C || ah == 0x3D) {      /* create / open: DS:DX=ASCIIZ name */
        /* ── DOS HANDS OUT THE LOWEST FREE HANDLE, AND THAT IS HOW `>` WORKS. ─────
             COMMAND.COM does not redirect with dup2. It CLOSES handle 1 and then
             creates the target, relying on the new file landing in the slot the
             console just vacated -- measured, the trace is `3Ch create` followed
             immediately by `40h write to handle 1` with no 45h/46h anywhere.
             Allocating from 5 upwards, as this did, makes that impossible: the file
             got handle 5, handle 1 was still the console, so the text went to the
             screen and the file stayed 0 bytes. Two earlier fixes (AH=40h, then
             AH=02h) were aimed at the write end and neither moved it, because the
             write end was never wrong -- the HANDLE NUMBER was. */
        /* ── WE DO NOT EMULATE SHARE.EXE, SO WE MUST NOT ENFORCE IT. (session 37) ──
             FILE_SHARE_READ here means a second open of a file this VDM already holds
             for writing fails with ERROR_SHARING_VIOLATION -- a lock bare DOS does not
             have, reported back as DOS error 2 "file not found", which sends the guest
             looking for a file that is there. The protected-mode twin of this call cost
             the GDI.EXE wall exactly that way. Share everything; the access mode below
             still comes from the guest. */
        char fn[300]; DWORD slot; HANDLE f;
        DWORD shr = FILE_SHARE_READ | FILE_SHARE_WRITE;
        v86_str(R_DS, R_DX, fn, sizeof(fn));
        if (ah == 0x3C)
            f = CreateFileA(fn, GENERIC_READ | GENERIC_WRITE, shr,
                            NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        else {
            DWORD mode = R_AX & 7;
            DWORD acc = (mode == 1) ? GENERIC_WRITE
                      : (mode == 2) ? (GENERIC_READ | GENERIC_WRITE) : GENERIC_READ;
            f = CreateFileA(fn, acc, shr, NULL, OPEN_EXISTING,
                            FILE_ATTRIBUTE_NORMAL, NULL);
        }
        if (f != INVALID_HANDLE_VALUE) {
            slot = dos_fh_alloc((void *const *)m->fh, m->std_open);
            if (slot < DOS_MAX_FILES) { m->fh[slot] = f; SETAX(slot); OKCF(); }
            else { CloseHandle(f); SETAX(4); ERRCF(); }
        } else { SETAX(2); ERRCF(); }
        tp = zput(tp, "  INT21 AH=0x"); tp = zhex(tp, ah);
        tp = zput(tp, " ["); tp = zput(tp, fn); tp = zput(tp, "] -> AX=0x");
        tp = zhex(tp, R_AX & 0xFFFF); tp = zput(tp, (*pfl & 1) ? " (err)\r\n" : "\r\n");
    } else if (ah == 0x3E) {                    /* close: BX=handle */
        DWORD h = R_BX & 0xFFFF;
        /* Any BOUND handle closes, including a low one the shell redirected -- see
           the note at AH=40h. An unbound 0-4 is the console and closing it is a no-op. */
        if (dos_fh_is_file((void *const *)m->fh, h)) { CloseHandle(m->fh[h]); m->fh[h] = 0; }
        else dos_fh_set_device(&m->std_open, h, 0);         /* free the device slot */
        OKCF();
    } else if (ah == 0x3F) {                    /* read: BX=handle CX=cnt -> DS:DX */
        DWORD h = R_BX & 0xFFFF, cnt = R_CX & 0xFFFF, rd = 0;
        void *b = (void *)((R_DS << 4) + (R_DX & 0xFFFF));
        if (dos_fh_is_file((void *const *)m->fh, h)) {      /* bound -> a file, even if low */
            /* ► LOG THE FILE POSITION, THE COUNT AND THE FIRST BYTES. A DOS extender
                 loading an executable is doing nothing but seek+read, so if the image it
                 ends up with is wrong, the first question is whether WE handed it the
                 right bytes -- and that is answerable offline by comparing these lines
                 against the file. Without the position a short or misplaced read is
                 indistinguishable from a correct one. */
            DWORD pos = SetFilePointer(m->fh[h], 0, NULL, FILE_CURRENT);
            ReadFile(m->fh[h], b, cnt, &rd, NULL); SETAX(rd); OKCF();
            tp = zput(tp, "  INT21 AH=3F h="); tp = zhex(tp, h);
            tp = zput(tp, " pos=0x"); tp = zhex(tp, pos);
            tp = zput(tp, " cnt=0x"); tp = zhex(tp, cnt);
            tp = zput(tp, " got=0x"); tp = zhex(tp, rd);
            tp = zput(tp, " -> 0x"); tp = zhex(tp, (DWORD)(ULONG_PTR)b);
            tp = zput(tp, " first="); tp = zdump(tp, (const BYTE *)b, (rd >= 8) ? 8 : 0);
            tp = zput(tp, "\r\n");
        }
        else if (dos_fh_is_device((void *const *)m->fh, m->std_open, h))
             { SETAX(0); OKCF(); }              /* an unredirected device: EOF for now */
        else { SETAX(6); ERRCF(); }
    } else if (ah == 0x42) {                    /* lseek: AL=org BX=h CX:DX=off */
        DWORD h = R_BX & 0xFFFF, meth = R_AX & 0xFF;
        LONG dist = (LONG)(((R_CX & 0xFFFF) << 16) | (R_DX & 0xFFFF));
        /* ── A BOUND HANDLE IS A FILE, WHATEVER ITS NUMBER. (GH #133) ─────────
             This read `h >= 5`, and that is how `>>` was broken while `>` worked:
             the shell redirects stdout by closing handle 1 and opening the
             target into the slot it vacates, then seeks to end-of-file before
             appending. Excluding handles below 5 refused that seek with error 6
             on a handle that IS a file -- the last survivor of #133, after the
             create and both write paths had been fixed.
             Oracle, tools/dostest/p_redir.asm on MS-DOS 6.22:
               CASE=int21.42.end.on.h1 SIG=AX,DX,CF AX=0004 DX=0000 CF=0
             i.e. real DOS seeks handle 1 to the end and reports 4 bytes. */
        if (dos_fh_is_file((void *const *)m->fh, h)) {
            DWORD np = SetFilePointer(m->fh[h], dist, NULL, meth);
            SETAX(np & 0xFFFF);
            R_DX = (R_DX & 0xFFFF0000u) | ((np >> 16) & 0xFFFF); OKCF();
        } else { SETAX(6); ERRCF(); }
    } else if (ah == 0x30) {                    /* get DOS version */
        /* AL=major, AH=minor, BH=OEM, BL:CX=24-bit serial.  GH #28.
           BX and CX were never written before, so a caller saw whatever it had
           left in them and read that as our OEM number and serial.  Values
           confirmed against the 6.22 oracle: BH=0xFF (generic MS-DOS), serial 0.
           The version itself is configurable -- see dos_int21_set_version(). */
        SETAX((uint16_t)((m->ver_minor << 8) | m->ver_major));
        SET16(R_BX, 0xFF00);                    /* BH=OEM 0xFF, BL=serial high */
        SET16(R_CX, 0x0000);                    /* serial low                  */
        OKCF();
    } else if (ah == 0x4E || ah == 0x4F) {      /* find first / find next */
        volatile BYTE *d = (volatile BYTE *)((m->dta_seg << 4) + m->dta_off);
        WIN32_FIND_DATAA fd;
        uint16_t mask;
        int slot = -1, ok = 0;
        if (ah == 0x4E) {
            char pat[300];
            v86_str(R_DS, R_DX, pat, sizeof(pat));
            mask = (uint16_t)(R_CX & 0xFFFF);
            for (slot = 0; slot < 8 && m->find_h[slot]; ++slot) {}
            if (slot >= 8) { slot = 0;                       /* recycle the oldest */
                             FindClose(m->find_h[0]); m->find_h[0] = 0; }
            { HANDLE hf = FindFirstFileA(pat, &fd);
              if (hf == INVALID_HANDLE_VALUE) {
                  DWORD e = GetLastError();
                  /* ORACLE-CONFIRMED, and not what memory suggests: a pattern
                     that matches nothing inside an EXISTING directory is
                     AX=18 "no more files", not AX=2 "file not found". A missing
                     directory is AX=3. */
                  SETAX(e == ERROR_PATH_NOT_FOUND ? 3 : 18);
                  ERRCF();
              } else {
                  m->find_h[slot] = hf;
                  ok = 1;
                  while (!dta_match(fd.dwFileAttributes, mask)) {
                      if (!FindNextFileA(hf, &fd)) { ok = 0; break; }
                  }
                  /* Fill DOS's private search area deterministically.  It is
                     DOS-private, but leaving the caller's bytes lying in it
                     means the DTA differs run to run for no reason; real 6.22
                     puts the EXPANDED 11-byte search template there (a "*.*"
                     search reads back as eleven '?'), so do the same. */
                  { const char *q = pat; int bi = 0, ei, dot = 0;
                    for (bi = 0; bi < 11; ++bi) d[1 + bi] = ' ';
                    /* skip any path, match on the final component only */
                    { const char *r2 = pat;
                      for (; *r2; ++r2) if (*r2 == '\\' || *r2 == '/' || *r2 == ':') q = r2 + 1; }
                    for (bi = 0; bi < 8 && q[dot] && q[dot] != '.'; ++dot) {
                        if (q[dot] == '*') { while (bi < 8) d[1 + bi++] = '?'; 
                                             while (q[dot] && q[dot] != '.') ++dot; break; }
                        d[1 + bi++] = (BYTE)(q[dot] >= 'a' && q[dot] <= 'z'
                                             ? q[dot] - 32 : q[dot]);
                    }
                    if (q[dot] == '.') ++dot;
                    for (ei = 0; ei < 3 && q[dot]; ++dot) {
                        if (q[dot] == '*') { while (ei < 3) d[9 + ei++] = '?'; break; }
                        d[9 + ei++] = (BYTE)(q[dot] >= 'a' && q[dot] <= 'z'
                                             ? q[dot] - 32 : q[dot]);
                    } }
                  d[0] = 3;                                  /* drive C:        */
                  d[12] = (BYTE)(mask & 0xFF);
                  d[13] = 0; d[14] = 0; d[15] = 0; d[16] = 0;
                  d[17] = 0; d[18] = 0;
                  d[19] = DOS_FIND_MAGIC;
                  d[20] = (BYTE)slot;
              }
            }
        } else {                                             /* 4Fh: continue   */
            mask = (uint16_t)d[12];
            if (d[19] == DOS_FIND_MAGIC && d[20] < 8 && m->find_h[d[20]]) {
                slot = d[20];
                ok = 1;
                do {
                    if (!FindNextFileA(m->find_h[slot], &fd)) { ok = 0; break; }
                } while (!dta_match(fd.dwFileAttributes, mask));
            } else {
                SETAX(18); ERRCF();                          /* no search live  */
            }
        }
        if (slot >= 0 && ok) {
            dta_fill(d, &fd);
            /* DIR renders blank names, one impossible size repeated, and a 1980-ish
               date -- i.e. it is reading fields we did not put where it looks. Print
               the DTA we hand back, whole, and let the bytes settle it. */
            if (m->trace_all) { int q;
              tp = zput(tp, "  INT21 AH=4E/4F dta="); tp = zhexb(tp, (unsigned)((m->dta_seg >> 8) & 0xFF));
              tp = zhexb(tp, (unsigned)(m->dta_seg & 0xFF)); tp = zput(tp, ":");
              tp = zhexb(tp, (unsigned)((m->dta_off >> 8) & 0xFF));
              tp = zhexb(tp, (unsigned)(m->dta_off & 0xFF));
              tp = zput(tp, " [");
              for (q = 0; q < 44; ++q) { tp = zhexb(tp, (unsigned)d[q]); tp = zput(tp, " "); }
              tp = zput(tp, "]\r\n"); }
            SETAX(0); OKCF();                                /* oracle: AX=0000 */
        } else if (slot >= 0 && m->find_h[slot] && !ok) {
            FindClose(m->find_h[slot]); m->find_h[slot] = 0;
            d[19] = 0;
            SETAX(18); ERRCF();                              /* no more files   */
        }
    } else if ((ah >= 0x0F && ah <= 0x17) || (ah >= 0x21 && ah <= 0x24)
               || (ah >= 0x27 && ah <= 0x29)) {  /* ---- the FCB interface ---- */
        volatile BYTE *f = fcb_at(R_DS, R_DX);
        char nm[300];
        #define FCB_OK()   SETAX((R_AX & 0xFF00) | 0x00)
        #define FCB_FAIL() SETAX((R_AX & 0xFF00) | 0xFF)
        /* CF is undefined for these on real DOS; leave it as the guest set it. */
        if (ah == 0x0F || ah == 0x16) {         /* open / create */
            HANDLE fh2; DWORD slot;
            fcb_name(f, nm);
            fh2 = CreateFileA(nm, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, NULL,
                              (ah == 0x16) ? CREATE_ALWAYS : OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, NULL);
            if (fh2 == INVALID_HANDLE_VALUE) FCB_FAIL();
            else {
                FILETIME ft, lf; WORD fdt = 0, ftm = 0;
                DWORD sz = GetFileSize(fh2, NULL);
                slot = dos_fh_alloc((void *const *)m->fh, m->std_open);
                if (slot >= DOS_MAX_FILES) { CloseHandle(fh2); FCB_FAIL(); }
                else {
                    m->fh[slot] = fh2;
                    if (GetFileTime(fh2, NULL, NULL, &ft)
                        && FileTimeToLocalFileTime(&ft, &lf))
                        FileTimeToDosDateTime(&lf, &fdt, &ftm);
                    f[12] = 0; f[13] = 0;
                    f[14] = 128; f[15] = 0;         /* oracle: record size 128 */
                    f[16] = (BYTE)(sz & 0xFF);        f[17] = (BYTE)((sz >> 8) & 0xFF);
                    f[18] = (BYTE)((sz >> 16) & 0xFF); f[19] = (BYTE)((sz >> 24) & 0xFF);
                    f[20] = (BYTE)(fdt & 0xFF); f[21] = (BYTE)(fdt >> 8);
                    f[22] = (BYTE)(ftm & 0xFF); f[23] = (BYTE)(ftm >> 8);
                    /* DOS replaces a "default drive" 0 with the drive it
                       actually resolved -- measured: the oracle returns 01 when
                       run from A:, DOSBox 03 from C:. We were leaving the
                       caller's 0 in place. */
                    if (!f[0]) {
                        char cw[300];
                        DWORD cn = GetCurrentDirectoryA(sizeof(cw), cw);
                        if (cn >= 2 && cw[1] == ':')
                            f[0] = (BYTE)((cw[0] | 0x20) - 'a' + 1);
                    }
                    f[24] = FCB_MAGIC; f[25] = (BYTE)slot;
                    FCB_OK();
                }
            }
        } else if (ah == 0x10) {                /* close */
            if (f[24] == FCB_MAGIC && f[25] < DOS_MAX_FILES && m->fh[f[25]]) {
                CloseHandle(m->fh[f[25]]); m->fh[f[25]] = 0; f[24] = 0; FCB_OK();
            } else FCB_FAIL();
        } else if (ah == 0x11 || ah == 0x12) {  /* find first / find next */
            volatile BYTE *d = (volatile BYTE *)((m->dta_seg << 4) + m->dta_off);
            WIN32_FIND_DATAA fd;
            int got = 0;
            /* An extended FCB carries its search attribute in the byte just
               before the part fcb_at() returns; a normal one asks for ordinary
               files only.  WITHOUT THIS FILTER the search returns "." first --
               measured: our DTA came back with a blank name where the oracle had
               COMMAND.COM, because "." has no 8.3 name to put in the field. */
            uint16_t fmask = (f != (volatile BYTE *)((R_DS << 4) + (R_DX & 0xFFFF)))
                           ? (uint16_t)f[-1] : 0;
            /* ── A VOLUME-LABEL SEARCH IS NOT A FILE SEARCH. ────────────────────
                 Attribute 08h means "return the volume label and nothing else", and
                 it is how DIR fills in its header line. There is no file on disk to
                 match, so FindFirstFile cannot answer it -- we used to run the
                 ordinary search and hand back whatever came first, which is why DIR
                 announced `Volume in drive C is COMMAND COM`, the first file in the
                 directory wearing the label's clothes.
                 The label is 11 bytes in the name+ext field, NOT an 8.3 name, so it
                 is padded raw rather than through fcb_put_name. */
            if (fmask == 0x08) {
                if (ah == 0x11) {
                    char vol[128]; int vi;
                    volatile BYTE *e;
                    int ext = (f != (volatile BYTE *)((R_DS << 4) + (R_DX & 0xFFFF))) ? 7 : 0;
                    vol[0] = 0;
                    if (!GetVolumeInformationA("C:\\", vol, sizeof vol,
                                               NULL, NULL, NULL, NULL, 0) || !vol[0]) {
                        if (m->fcb_find) { FindClose(m->fcb_find); m->fcb_find = 0; }
                        m->last_err = 18;
                        FCB_FAIL();                       /* no label: DIR says so   */
                        goto fcb_done;
                    }
                    e = d + ext;
                    if (ext) { int q; d[0] = 0xFF; for (q = 1; q <= 5; ++q) d[q] = 0; d[6] = 0x08; }
                    e[0] = 3;                             /* drive C:                */
                    for (vi = 0; vi < 11; ++vi) {
                        char ch = vol[vi] ? vol[vi] : ' ';
                        if (!vol[vi]) { e[1 + vi] = ' '; continue; }
                        e[1 + vi] = (BYTE)(ch >= 'a' && ch <= 'z' ? ch - 32 : ch);
                    }
                    e[12] = 0x08;                         /* attribute: volume label */
                    { int q; for (q = 13; q <= 32; ++q) e[q] = 0; }
                    if (m->fcb_find) { FindClose(m->fcb_find); m->fcb_find = 0; }
                    FCB_OK();
                } else { m->last_err = 18; FCB_FAIL(); }  /* 12h: only ever one label */
                goto fcb_done;
            }
            if (ah == 0x11) {
                HANDLE hf;
                fcb_name(f, nm);
                if (m->fcb_find) { FindClose(m->fcb_find); m->fcb_find = 0; }
                hf = FindFirstFileA(nm, &fd);
                if (hf != INVALID_HANDLE_VALUE) {
                    m->fcb_find = hf; got = 1;
                    while (!dta_match(fd.dwFileAttributes, fmask)) {
                        if (!FindNextFileA(hf, &fd)) { got = 0; break; }
                    }
                }
            } else if (m->fcb_find) {
                got = 1;
                do {
                    if (!FindNextFileA(m->fcb_find, &fd)) { got = 0; break; }
                } while (!dta_match(fd.dwFileAttributes, fmask));
                if (!got) { FindClose(m->fcb_find); m->fcb_find = 0; }
            }
            /* ── A FAILED SEARCH MUST SAY WHY, OR THE LAST FAILURE SPEAKS FOR IT. ──
                 The extended error (AH=59h) is only recorded where CF comes back set,
                 and FCB calls deliberately leave CF alone -- so an exhausted search
                 left `last_err` holding whatever failed previously. In a shell that
                 is COMMAND.COM's own startup probe: it asks AH=48h for 0xFFFF
                 paragraphs to learn the largest block, which fails with code 8.
                 DIR then ends its listing, asks AH=59h why, is told "insufficient
                 memory", and prints exactly that instead of its summary line. The
                 listing was RIGHT and the epitaph was three commands stale.
                 18 = "no more files", which is what DOS reports here. */
            if (!got) { m->last_err = 18; FCB_FAIL(); }
            else {
                const char *bn = fd.cAlternateFileName[0] ? fd.cAlternateFileName
                                                          : fd.cFileName;
                FILETIME lf; WORD fdt = 0, ftm = 0;
                int k;
                /* ── AN EXTENDED SEARCH RETURNS AN EXTENDED RESULT. ──────────────
                     We already skip the 7-byte prefix on the way IN (fcb_at), and
                     then wrote the answer back in the SHORT layout regardless -- so
                     a caller that searched with an extended FCB read every field
                     seven bytes early. DIR does exactly that (it must, to see
                     directories and the volume label), which is why its listing came
                     out with blank names, one impossible size repeated down the
                     column, and a volume label of "COM" -- the tail of COMMAND.COM
                     read as an 11-byte label.
                     The prefix is FFh, five reserved bytes, then the attribute of
                     the file found; the ordinary result follows it unchanged. */
                int ext = (f != (volatile BYTE *)((R_DS << 4) + (R_DX & 0xFFFF))) ? 7 : 0;
                volatile BYTE *e = d + ext;
                if (FileTimeToLocalFileTime(&fd.ftLastWriteTime, &lf))
                    FileTimeToDosDateTime(&lf, &fdt, &ftm);
                if (ext) {
                    d[0] = 0xFF;
                    for (k = 1; k <= 5; ++k) d[k] = 0;
                    d[6] = (BYTE)(fd.dwFileAttributes & 0x3F);
                }
                e[0] = 3;                                 /* drive C:          */
                fcb_put_name(e + 1, bn);
                e[12] = (BYTE)(fd.dwFileAttributes & 0x3F);
                for (k = 13; k <= 22; ++k) e[k] = 0;
                e[23] = (BYTE)(ftm & 0xFF); e[24] = (BYTE)(ftm >> 8);
                e[25] = (BYTE)(fdt & 0xFF); e[26] = (BYTE)(fdt >> 8);
                e[27] = 0; e[28] = 0;                     /* starting cluster  */
                e[29] = (BYTE)( fd.nFileSizeLow        & 0xFF);
                e[30] = (BYTE)((fd.nFileSizeLow >> 8)  & 0xFF);
                e[31] = (BYTE)((fd.nFileSizeLow >> 16) & 0xFF);
                e[32] = (BYTE)((fd.nFileSizeLow >> 24) & 0xFF);
                FCB_OK();
            }
            fcb_done: ;
        } else if (ah == 0x13) {                /* delete (wildcards allowed) */
            WIN32_FIND_DATAA fd; HANDLE hf; int any = 0;
            fcb_name(f, nm);
            hf = FindFirstFileA(nm, &fd);
            if (hf != INVALID_HANDLE_VALUE) {
                do {
                    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
                    if (DeleteFileA(fd.cFileName)) any = 1;
                } while (FindNextFileA(hf, &fd));
                FindClose(hf);
            }
            if (any) FCB_OK(); else FCB_FAIL();
        } else if (ah == 0x17) {                /* rename: new name at f[17..27] */
            char to[300];
            char save[16]; int k;
            fcb_name(f, nm);
            for (k = 0; k < 12; ++k) save[k] = (char)f[k];
            { volatile BYTE tmp[12]; tmp[0] = f[0];
              for (k = 1; k < 12; ++k) tmp[k] = f[16 + k];
              fcb_name(tmp, to); }
            if (MoveFileA(nm, to)) FCB_OK(); else FCB_FAIL();
            (void)save;
        } else if (ah == 0x14 || ah == 0x15 || ah == 0x21 || ah == 0x22
                   || ah == 0x27 || ah == 0x28) {         /* record I/O */
            volatile BYTE *d = (volatile BYTE *)((m->dta_seg << 4) + m->dta_off);
            DWORD recsz = (DWORD)f[14] | ((DWORD)f[15] << 8);
            DWORD blk   = (DWORD)f[12] | ((DWORD)f[13] << 8);
            DWORD rec, count = 1, done = 0, k;
            BYTE buf[512];
            if (!recsz) recsz = 128;
            if (recsz > sizeof(buf)) recsz = sizeof(buf);
            if (ah == 0x14 || ah == 0x15) rec = blk * 128 + f[32];
            else rec = (DWORD)f[33] | ((DWORD)f[34] << 8)
                     | ((DWORD)f[35] << 16) | ((DWORD)f[36] << 24);
            if (ah == 0x27 || ah == 0x28) count = R_CX & 0xFFFF;
            if (f[24] != FCB_MAGIC || f[25] >= DOS_MAX_FILES || !m->fh[f[25]]) SETAX((R_AX & 0xFF00) | 1);
            else {
                HANDLE hh = m->fh[f[25]];
                DWORD n = 0;
                SetFilePointer(hh, (LONG)(rec * recsz), NULL, FILE_BEGIN);
                for (k = 0; k < count; ++k) {
                    if (ah == 0x14 || ah == 0x21 || ah == 0x27) {
                        DWORD j;
                        if (!ReadFile(hh, buf, recsz, &n, NULL) || n == 0) break;
                        for (j = 0; j < recsz; ++j)
                            d[done * recsz + j] = (j < n) ? buf[j] : 0;
                        ++done;
                        if (n < recsz) break;
                    } else {
                        DWORD j;
                        for (j = 0; j < recsz; ++j) buf[j] = d[done * recsz + j];
                        if (!WriteFile(hh, buf, recsz, &n, NULL)) break;
                        ++done;
                    }
                }
                if (ah == 0x27 || ah == 0x28) SET16(R_CX, (uint16_t)done);
                /* AL: 0 = all done, 1 = end of file / nothing transferred,
                   3 = a partial final record. */
                if (done == count) SETAX((R_AX & 0xFF00) | 0);
                else if (!done)    SETAX((R_AX & 0xFF00) | 1);
                else               SETAX((R_AX & 0xFF00) | 3);
                if (ah == 0x14 || ah == 0x15) {           /* advance sequentially */
                    DWORD nr = rec + done;
                    f[12] = (BYTE)((nr / 128) & 0xFF); f[13] = (BYTE)((nr / 128) >> 8);
                    f[32] = (BYTE)(nr % 128);
                } else {
                    DWORD nr = rec + done;
                    f[33] = (BYTE)(nr & 0xFF);         f[34] = (BYTE)((nr >> 8) & 0xFF);
                    f[35] = (BYTE)((nr >> 16) & 0xFF); f[36] = (BYTE)((nr >> 24) & 0xFF);
                }
            }
        } else if (ah == 0x23) {                /* get file size, in records */
            HANDLE fh3;
            fcb_name(f, nm);
            fh3 = CreateFileA(nm, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
            if (fh3 == INVALID_HANDLE_VALUE) FCB_FAIL();
            else {
                DWORD sz = GetFileSize(fh3, NULL);
                DWORD recsz = (DWORD)f[14] | ((DWORD)f[15] << 8);
                DWORD recs;
                CloseHandle(fh3);
                if (!recsz) recsz = 128;
                recs = (sz + recsz - 1) / recsz;
                f[33] = (BYTE)(recs & 0xFF);         f[34] = (BYTE)((recs >> 8) & 0xFF);
                f[35] = (BYTE)((recs >> 16) & 0xFF); f[36] = (BYTE)((recs >> 24) & 0xFF);
                FCB_OK();
            }
        } else if (ah == 0x24) {                /* set random record from current */
            DWORD nr = ((DWORD)f[12] | ((DWORD)f[13] << 8)) * 128 + f[32];
            f[33] = (BYTE)(nr & 0xFF);         f[34] = (BYTE)((nr >> 8) & 0xFF);
            f[35] = (BYTE)((nr >> 16) & 0xFF); f[36] = (BYTE)((nr >> 24) & 0xFF);
            OKCF();
        } else if (ah == 0x29) {                /* parse a filename into an FCB */
            char in[300];
            volatile BYTE *dst = (volatile BYTE *)((R_ES << 4) + (R_DI & 0xFFFF));
            int i2 = 0, wild = 0, k;
            v86_str(R_DS, R_SI, in, sizeof(in));
            while (in[i2] == ' ' || in[i2] == 9) ++i2;
            dst[0] = 0;
            if (in[i2] && in[i2 + 1] == ':') {
                char dch = in[i2];
                dst[0] = (BYTE)((dch >= 'a' ? dch - 32 : dch) - 'A' + 1);
                i2 += 2;
            }
            fcb_put_name(dst + 1, in + i2);
            for (k = 1; k <= 11; ++k) if (dst[k] == '?' || dst[k] == '*') wild = 1;
            for (k = 12; k <= 15; ++k) dst[k] = 0;
            SETAX((R_AX & 0xFF00) | (wild ? 1 : 0));
            SET16(R_SI, (uint16_t)((R_SI & 0xFFFF) + i2));
            OKCF();
            /* THE CALL COMMAND.COM'S DISPATCH TURNS ON. `ver ` runs and `ver` does
               not, and the traces diverge on the instruction after the third of
               these -- so print what went in and what came out, both. Reasoning
               about it from the handler's source has already produced two wrong
               models this session. */
            if (m->trace_all) { int q;
              tp = zput(tp, "  INT21 AH=29 al="); tp = zhexb(tp, (unsigned)(R_AX & 0xFF));
              tp = zput(tp, " ds:si="); tp = zhexb(tp, (unsigned)((R_DS >> 8) & 0xFF));
              tp = zhexb(tp, (unsigned)(R_DS & 0xFF)); tp = zput(tp, ":");
              tp = zhexb(tp, (unsigned)(((R_SI & 0xFFFF) >> 8) & 0xFF));
              tp = zhexb(tp, (unsigned)(R_SI & 0xFF));
              tp = zput(tp, " in=[");
              for (q = 0; q < 12 && in[q]; ++q) tp = zhexb(tp, (unsigned)(BYTE)in[q]), tp = zput(tp, " ");
              tp = zput(tp, "] fcb=[");
              for (q = 0; q < 12; ++q) tp = zhexb(tp, (unsigned)dst[q]), tp = zput(tp, " ");
              tp = zput(tp, "]\r\n"); }
        } else FCB_FAIL();
        #undef FCB_OK
        #undef FCB_FAIL
    } else if (ah == 0x4B) {                    /* EXEC: load and run a program */
        uint8_t al4b = (uint8_t)(R_AX & 0xFF);
        if (al4b == 0x00 || al4b == 0x01) {
            const volatile BYTE *pb =
                (const volatile BYTE *)((R_ES << 4) + (R_BX & 0xFFFF));
            v86_str(R_DS, R_DX, m->exec_path, sizeof(m->exec_path));
            m->exec_env      = (uint16_t)(pb[0] | (pb[1] << 8));
            m->exec_tail_off = (uint16_t)(pb[2] | (pb[3] << 8));
            m->exec_tail_seg = (uint16_t)(pb[4] | (pb[5] << 8));
            m->exec_fcb1_off = (uint16_t)(pb[6] | (pb[7] << 8));
            m->exec_fcb1_seg = (uint16_t)(pb[8] | (pb[9] << 8));
            m->exec_fcb2_off = (uint16_t)(pb[10] | (pb[11] << 8));
            m->exec_fcb2_seg = (uint16_t)(pb[12] | (pb[13] << 8));
            m->exec_mode = al4b;
            m->exec_pending = 1;                /* the host does the rest */
            OKCF();
        } else {
            /* AL=03 loads an overlay into a caller-supplied segment with no PSP
               and no transfer of control.  Not wired up; say so. */
            tp = zput(tp, "  INT21 AH=4B AL=0x"); tp = zhexb(tp, al4b);
            tp = zput(tp, " UNIMPLEMENTED (overlay load)\r\n");
            m->unimpl21[0x4B >> 3] |= (uint8_t)(1u << (0x4B & 7));
            SETAX(1); ERRCF();
        }
    } else if (ah == 0x1B || ah == 0x1C) {      /* allocation info for a drive */
        /* AL=sectors/cluster, DS:BX -> media descriptor byte, CX=bytes/sector,
           DX=total clusters.  All disk geometry, so the probe compares only CF.
           NOTE this call RETURNS A SEGMENT IN DS -- which is what broke the
           probe's own output until probe_capture learned to restore it. */
        DWORD spc = 0, bps = 0, freec = 0, totc = 0;
        char root[4]; char *rp = 0;
        uint8_t dl1b = (uint8_t)(R_DX & 0xFF);
        if (ah == 0x1C && dl1b) { root[0] = (char)('A' + dl1b - 1); root[1] = ':';
                                  root[2] = '\\'; root[3] = 0; rp = root; }
        if (GetDiskFreeSpaceA(rp, &spc, &bps, &freec, &totc)) {
            volatile BYTE *md = (volatile BYTE *)((DOS_CTAB_SEG << 4) + DOS_MEDIA_OFF);
            *md = 0xF8;                          /* fixed disk */
            SETAX((R_AX & 0xFF00) | (spc & 0xFF));
            SET16(R_DS, DOS_CTAB_SEG); SET16(R_BX, DOS_MEDIA_OFF);
            SET16(R_CX, (uint16_t)bps);
            SET16(R_DX, totc > 0xFFFF ? 0xFFFF : totc);
            OKCF();
        } else { SETAX((R_AX & 0xFF00) | 0xFF); ERRCF(); }
    } else if (ah == 0x1F || ah == 0x32) {      /* get drive parameter block */
        /* DPB contents are disk geometry and its address is host-specific; AL is
           the comparable part -- 00 for a valid drive, FF otherwise (measured). */
        uint8_t dl32 = (ah == 0x1F) ? 0 : (uint8_t)(R_DX & 0xFF);
        DWORD spc = 0, bps = 0, freec = 0, totc = 0;
        char root[4]; char *rp = 0;
        if (dl32) { root[0] = (char)('A' + dl32 - 1); root[1] = ':';
                    root[2] = '\\'; root[3] = 0; rp = root; }
        if (dl32 > 26 || !GetDiskFreeSpaceA(rp, &spc, &bps, &freec, &totc)) {
            SETAX((R_AX & 0xFF00) | 0xFF);
        } else {
            volatile BYTE *d = (volatile BYTE *)((DOS_CTAB_SEG << 4) + DOS_DPB_OFF);
            int k; uint8_t shift = 0;
            while ((1u << shift) < spc && shift < 15) ++shift;
            for (k = 0; k < 33; ++k) d[k] = 0;
            d[0] = (BYTE)(dl32 ? dl32 - 1 : 2);   /* 0-based drive */
            d[1] = 0;
            d[2] = (BYTE)(bps & 0xFF); d[3] = (BYTE)(bps >> 8);
            d[4] = (BYTE)(spc - 1);
            d[5] = shift;
            d[6] = 1; d[7] = 0;                   /* reserved sectors  */
            d[8] = 2;                             /* number of FATs    */
            d[9] = 0x00; d[10] = 0x02;            /* root entries 512  */
            d[13] = (BYTE)((totc + 1) & 0xFF); d[14] = (BYTE)(((totc + 1) >> 8) & 0xFF);
            d[23] = 0xF8;                         /* media descriptor  */
            d[25] = 0xFF; d[26] = 0xFF; d[27] = 0xFF; d[28] = 0xFF;  /* no next DPB */
            SET16(R_DS, DOS_CTAB_SEG); SET16(R_BX, DOS_DPB_OFF);
            SETAX(R_AX & 0xFF00);
        }
        OKCF();
    } else if (ah == 0x37) {                    /* get/set the SWITCH character */
        uint8_t al37 = (uint8_t)(R_AX & 0xFF);
        if (al37 == 0x00) {                     /* oracle: DL = '/' */
            SET16(R_DX, (uint16_t)((R_DX & 0xFF00) | m->switch_char));
            SETAX(R_AX & 0xFF00); OKCF();
        } else if (al37 == 0x01) {
            m->switch_char = (uint8_t)(R_DX & 0xFF);
            SETAX(R_AX & 0xFF00); OKCF();
        } else { SETAX((R_AX & 0xFF00) | 0xFF); OKCF(); }
    } else if (ah == 0x66) {                    /* get/set global code page */
        uint8_t al66 = (uint8_t)(R_AX & 0xFF);
        if (al66 == 0x01) {                     /* oracle: BX=DX=437 */
            SET16(R_BX, 437); SET16(R_DX, 437); OKCF();
        } else if (al66 == 0x02) {
            OKCF();                             /* accept; we have only 437 */
        } else { SETAX(1); ERRCF(); }
    } else if (ah == 0x26 || ah == 0x55) {      /* create a PSP / child PSP */
        /* Copy our PSP to the segment in DX and fix up the fields that must
           differ. 55h additionally takes the child's memory top in SI. */
        volatile BYTE *src = (volatile BYTE *)(DOS_PSP_SEG << 4);
        volatile BYTE *dst = (volatile BYTE *)((R_DX & 0xFFFF) << 4);
        int k;
        for (k = 0; k < 256; ++k) dst[k] = src[k];
        dst[0x16] = (BYTE)(DOS_PSP_SEG & 0xFF);       /* parent PSP segment */
        dst[0x17] = (BYTE)(DOS_PSP_SEG >> 8);
        if (ah == 0x55) {
            dst[0x02] = (BYTE)(R_SI & 0xFF);          /* memory top          */
            dst[0x03] = (BYTE)((R_SI >> 8) & 0xFF);
        }
        OKCF();
    } else if (ah == 0x31) {                    /* terminate and stay resident */
        /* We run one program at a time, so nothing can stay resident behind it.
           Say so rather than pretend: a TSR that believes it installed and did
           not is the silent-failure class #27 exists to remove. */
        tp = zput(tp, "  INT21 AH=31 TSR: residency NOT honoured (single-program host)\r\n");
        m->unimpl21[0x31 >> 3] |= (uint8_t)(1u << (0x31 & 7));
        m->exit_code = (int)(R_AX & 0xFF);
        cont = 0;
    } else if (ah == 0x53) {                    /* translate a BPB into a DPB */
        /* ⚠ TESTED AS THE CAUSE OF THE COMMAND.COM EXIT, AND REFUTED. XP's own
             COMMAND.COM calls this during init and terminates shortly after,
             printing nothing, and this was the ONLY unimplemented call in the whole
             run -- which made it the tempting answer rather than the proven one.
             Answering SUCCESS with a zeroed DPB was tried: the shell still exits, at
             the SAME CS:IP (0x95eb:0x03ce), after the same 32 ms. So 53h is not what
             stops it, and reporting success here would have bought nothing at the
             price of a call that lies. Reverted deliberately.
             What COMMAND.COM asks for and does not get is INT 2Fh AX=122Eh, the five
             DOS error-message table addresses (DL=00/02/04/06/08) -- see the widened
             BOP2F log. That is the next thing to chase, and "died after" is still not
             "died because": prove it before implementing it. */
        tp = zput(tp, "  INT21 AH=53 BPB->DPB UNIMPLEMENTED (no installable "
                      "block drivers)\r\n");
        m->unimpl21[0x53 >> 3] |= (uint8_t)(1u << (0x53 & 7));
        SETAX(1); ERRCF();
    } else if (ah == 0x5E) {                    /* network machine name / printer */
        uint8_t al5e = (uint8_t)(R_AX & 0xFF);
        if (al5e == 0x00) {                     /* oracle: AX=0, CF=0 */
            volatile BYTE *d = (volatile BYTE *)((R_DS << 4) + (R_DX & 0xFFFF));
            int k; for (k = 0; k < 16; ++k) d[k] = 0;
            SETAX(0); SET16(R_CX, 0); OKCF();
        } else { SETAX(1); ERRCF(); }
    } else if (ah == 0x5F) {                    /* network redirection list */
        tp = zput(tp, "  INT21 AH=5F network redirection: no redirector present\r\n");
        SETAX(1); ERRCF();                      /* invalid function */
    } else if (ah == 0x64) {                    /* set device driver lookahead */
        OKCF();                                 /* internal; accepted, no effect */
    } else if (ah == 0x03) {                    /* AUX input  */
        SETAX((R_AX & 0xFF00) | 0x1A);          /* no serial attached -> EOF   */
        OKCF();
    } else if (ah == 0x04 || ah == 0x05) {      /* AUX / printer output        */
        OKCF();                                 /* accepted and discarded      */
    } else if (ah == 0x0C) {                    /* flush input, then run AL     */
        /* AL names the input function to perform after flushing. Anything else
           is just a flush. Re-dispatching is the whole point of the call. */
        uint8_t fn = (uint8_t)(R_AX & 0xFF);
        while (m->conpeek && m->conpeek(m->cinctx) && m->coninnb)
            (void)m->coninnb(m->cinctx);
        if (fn == 0x01 || fn == 0x06 || fn == 0x07 || fn == 0x08 || fn == 0x0A) {
            SETAX((uint16_t)(fn << 8));
            m->retry = 1;                       /* re-enter with AH = that fn  */
        } else OKCF();
    } else if (ah == 0x2E) {                    /* set verify flag */
        m->verify = (uint8_t)(R_AX & 0xFF); OKCF();
    } else if (ah == 0x54) {                    /* get verify flag */
        SETAX((R_AX & 0xFF00) | m->verify); OKCF();
    } else if (ah == 0x34) {                    /* get InDOS flag -> ES:BX */
        SET16(R_ES, DOS_HDLR_SEG); SET16(R_BX, DOS_INDOS_OFF); OKCF();
    } else if (ah == 0x5D && ((R_AX & 0xFF) == 0x08 || (R_AX & 0xFF) == 0x09)) {
        /* 5D08h/5D09h set and flush the network redirector's sharing retry
           counts. COMMAND.COM calls both at startup. There is no redirector
           here, so accept and ignore -- that is what DOS does on a machine with
           no network, and refusing would make the shell think something failed. */
        OKCF();
    } else if (ah == 0x5D && (R_AX & 0xFF) == 0x06) {   /* get swappable data area */
        SET16(R_DS, DOS_HDLR_SEG); SET16(R_SI, DOS_SDA_OFF);
        SET16(R_CX, DOS_SDA_LEN); SET16(R_DX, DOS_SDA_LEN);
        tp = zput(tp, "  INT21 AH=5D06 SDA (minimal: crit-err + InDOS only)\r\n");
        OKCF();
    } else if (ah == 0x39 || ah == 0x3A) {      /* mkdir / rmdir */
        char fn[300];
        int ok2;
        v86_str(R_DS, R_DX, fn, sizeof(fn));
        ok2 = (ah == 0x39) ? (int)CreateDirectoryA(fn, NULL)
                           : (int)RemoveDirectoryA(fn);
        if (ok2) OKCF();
        else {
            /* Oracle: mkdir over an existing name is 5 (access denied); rmdir of
               something absent is 3 (path not found). */
            DWORD e = GetLastError();
            SETAX((uint16_t)(e == ERROR_ALREADY_EXISTS ? 5
                           : e == ERROR_PATH_NOT_FOUND ? 3
                           : e == ERROR_FILE_NOT_FOUND ? 3 : 5));
            ERRCF();
        }
    } else if (ah == 0x41) {                    /* delete file */
        char fn[300];
        v86_str(R_DS, R_DX, fn, sizeof(fn));
        if (DeleteFileA(fn)) OKCF();
        else { SETAX(2); ERRCF(); }             /* oracle: absent -> AX=2 */
    } else if (ah == 0x43) {                    /* get/set file attributes */
        char fn[300];
        uint8_t al43 = (uint8_t)(R_AX & 0xFF);
        v86_str(R_DS, R_DX, fn, sizeof(fn));
        if (al43 == 0x00) {
            DWORD a = GetFileAttributesA(fn);
            if (a == 0xFFFFFFFFu) { SETAX(2); ERRCF(); }
            else { SET16(R_CX, (uint16_t)(a & 0x3F)); SETAX((uint16_t)(a & 0x3F)); OKCF(); }
        } else if (al43 == 0x01) {
            DWORD a = (DWORD)(R_CX & 0x3F);
            if (!a) a = FILE_ATTRIBUTE_NORMAL;
            if (SetFileAttributesA(fn, a)) OKCF();
            else { SETAX(2); ERRCF(); }
        } else {
            tp = zput(tp, "  INT21 AH=43 AL=0x"); tp = zhexb(tp, al43);
            tp = zput(tp, " UNIMPLEMENTED subfunction\r\n");
            m->unimpl21[0x43 >> 3] |= (uint8_t)(1u << (0x43 & 7));
            SETAX(1); ERRCF();
        }
    } else if (ah == 0x45 || ah == 0x46) {      /* dup / dup2 */
        /* ── ★ A DEVICE IS DUPLICABLE, AND THAT IS THE WHOLE POINT OF 45h. ────
             This refused anything that was not a FILE, so `dup(1)` -- the first
             step of every save-redirect-restore sequence a shell performs --
             came back error 6 and the restore could never happen. Found by
             running tools/dostest/p_redir.asm on the rig against the same probe
             on the oracle; the two disagreed on one line:
               oracle : CASE=int21.45.dup.stdout AX=0005 CF=0
               NTVDMEX: CASE=int21.45.dup.stdout AX=0006 CF=1
             The failure was not even visible as itself: the probe's LATER output
             vanished into the test file, because with stdout never restored the
             next open took handle 1. A wrong answer that eats the evidence of
             itself is why this is measured against an oracle rather than read.
           Duplicating a device produces another handle ON THAT DEVICE -- no
           Win32 handle exists to duplicate, so the copy is a device slot too. */
        DWORD src = R_BX & 0xFFFF, dst;
        int src_dev = dos_fh_is_device((void *const *)m->fh, m->std_open, src);
        if (!src_dev && !dos_fh_is_file((void *const *)m->fh, src)) { SETAX(6); ERRCF(); }
        else {
            HANDLE nh = 0;
            if (!src_dev
                && !DuplicateHandle(GetCurrentProcess(), m->fh[src],
                                    GetCurrentProcess(), &nh, 0, FALSE,
                                    DUPLICATE_SAME_ACCESS)) { SETAX(6); ERRCF(); }
            else if (ah == 0x45) {
                dst = dos_fh_alloc((void *const *)m->fh, m->std_open);
                if (dst >= DOS_MAX_FILES) {
                    if (nh) CloseHandle(nh); SETAX(4); ERRCF();
                } else if (src_dev && !dos_fh_set_device(&m->std_open, dst, 1)) {
                    /* Past the device mask. Refuse LOUDLY rather than hand back a
                       slot that would read as a file -- see DOS_DEV_SLOTS. */
                    tp = zput(tp, "  INT21 AH=45 device dup past slot 0x");
                    tp = zhex(tp, DOS_DEV_SLOTS); tp = zput(tp, " -- refused\r\n");
                    SETAX(4); ERRCF();
                } else { m->fh[dst] = nh; SETAX(dst); OKCF(); }
            } else {
                dst = R_CX & 0xFFFF;
                if (dst >= DOS_MAX_FILES) { if (nh) CloseHandle(nh); SETAX(6); ERRCF(); }
                else if (src_dev && !dos_fh_set_device(&m->std_open, dst, 1)) {
                    tp = zput(tp, "  INT21 AH=46 device dup2 past slot 0x");
                    tp = zhex(tp, DOS_DEV_SLOTS); tp = zput(tp, " -- refused\r\n");
                    SETAX(4); ERRCF();
                }
                else { if (m->fh[dst]) CloseHandle(m->fh[dst]);
                       m->fh[dst] = nh;                 /* 0 when src is a device */
                       if (!src_dev) dos_fh_set_device(&m->std_open, dst, 0);
                       OKCF(); }
            }
        }
    } else if (ah == 0x4D) {                    /* get child return code */
        SETAX(m->child_rc); m->child_rc = 0;    /* DOS clears it after reading */
        OKCF();
    } else if (ah == 0x56) {                    /* rename: DS:DX -> ES:DI */
        char from[300], to[300];
        v86_str(R_DS, R_DX, from, sizeof(from));
        v86_str(R_ES, R_DI, to,   sizeof(to));
        if (MoveFileA(from, to)) OKCF();
        else { DWORD e = GetLastError();
               SETAX((uint16_t)(e == ERROR_ALREADY_EXISTS ? 5 : 2)); ERRCF(); }
    } else if (ah == 0x57) {                    /* get/set file date and time */
        DWORD h57 = R_BX & 0xFFFF;
        uint8_t al57 = (uint8_t)(R_AX & 0xFF);
        if (!dos_fh_is_file((void *const *)m->fh, h57)) { SETAX(6); ERRCF(); }
        else if (al57 == 0x00) {
            FILETIME ft, lf; WORD fdate = 0, ftime = 0;
            if (GetFileTime(m->fh[h57], NULL, NULL, &ft)
                && FileTimeToLocalFileTime(&ft, &lf)
                && FileTimeToDosDateTime(&lf, &fdate, &ftime)) {
                SET16(R_CX, ftime); SET16(R_DX, fdate); OKCF();
            } else { SETAX(6); ERRCF(); }
        } else if (al57 == 0x01) {
            FILETIME ft, lf;
            if (DosDateTimeToFileTime((WORD)(R_DX & 0xFFFF), (WORD)(R_CX & 0xFFFF), &lf)
                && LocalFileTimeToFileTime(&lf, &ft)
                && SetFileTime(m->fh[h57], NULL, NULL, &ft)) OKCF();
            else { SETAX(6); ERRCF(); }
        } else { SETAX(1); ERRCF(); }
    } else if (ah == 0x5A || ah == 0x5B) {      /* create temp / create new */
        char fn[300]; HANDLE f; DWORD slot;
        v86_str(R_DS, R_DX, fn, sizeof(fn));
        if (ah == 0x5A) {                       /* DS:DX is a DIRECTORY path;
                                                   DOS appends a generated name
                                                   and hands it back in place. */
            int k = 0; static unsigned seq = 0;
            const char *hexd = "0123456789ABCDEF";
            while (fn[k] && k < 280) ++k;
            if (k && fn[k-1] != '\\' && fn[k-1] != '/') fn[k++] = '\\';
            { unsigned v = (unsigned)(GetTickCount() + (seq++ * 0x1234u));
              int j; for (j = 0; j < 8; ++j) fn[k + j] = hexd[(v >> (28 - j*4)) & 0xF]; }
            fn[k + 8] = 0;
            { volatile BYTE *d = (volatile BYTE *)((R_DS << 4) + (R_DX & 0xFFFF));
              int j = 0; while (fn[j]) { d[j] = (BYTE)fn[j]; ++j; } d[j] = 0; }
        }
        f = CreateFileA(fn, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, NULL,
                        CREATE_NEW, (DWORD)(R_CX & 0x3F) ? (DWORD)(R_CX & 0x3F)
                                                         : FILE_ATTRIBUTE_NORMAL, NULL);
        if (f == INVALID_HANDLE_VALUE) {
            /* Oracle: create-new over an existing file is error 80 (file exists),
               not 5 -- measured, and not the obvious guess. */
            DWORD e = GetLastError();
            SETAX((uint16_t)(e == ERROR_FILE_EXISTS || e == ERROR_ALREADY_EXISTS ? 80 : 3));
            ERRCF();
        } else {
            slot = dos_fh_alloc((void *const *)m->fh, m->std_open);
            if (slot < DOS_MAX_FILES) { m->fh[slot] = f; SETAX(slot); OKCF(); }
            else { CloseHandle(f); SETAX(4); ERRCF(); }
        }
    } else if (ah == 0x5C) {                    /* lock / unlock a byte range */
        DWORD h5c = R_BX & 0xFFFF;
        DWORD off = ((DWORD)(R_CX & 0xFFFF) << 16) | (DWORD)(R_DX & 0xFFFF);
        DWORD len = ((DWORD)(R_SI & 0xFFFF) << 16) | (DWORD)(R_DI & 0xFFFF);
        if (!dos_fh_is_file((void *const *)m->fh, h5c)) { SETAX(6); ERRCF(); }
        else {
            BOOL ok5 = ((R_AX & 0xFF) == 0)
                     ? LockFile(m->fh[h5c], off, 0, len, 0)
                     : UnlockFile(m->fh[h5c], off, 0, len, 0);
            if (ok5) OKCF(); else { SETAX(0x21); ERRCF(); }   /* 33 = lock violation */
        }
    } else if (ah == 0x67) {                    /* set maximum handle count */
        /* We keep a fixed DOS_MAX_FILES-entry table, so anything up to that succeeds.
           NOTE the oracle FAILED this with AX=8 (insufficient memory) when asked
           for 30 -- that is a property of ITS memory state at that moment, not a
           rule about DOS, which is why the probe treats the result as
           informational rather than comparable. */
        if ((R_BX & 0xFFFF) <= DOS_MAX_FILES) OKCF();
        else { SETAX(8); ERRCF(); }
    } else if (ah == 0x68 || ah == 0x6A) {      /* commit file (flush) */
        DWORD h68 = R_BX & 0xFFFF;
        if (!dos_fh_is_file((void *const *)m->fh, h68)) { SETAX(6); ERRCF(); }
        else { FlushFileBuffers(m->fh[h68]); OKCF(); }
    } else if (ah == 0x6C) {                    /* extended open/create */
        /* BX=mode, CX=attributes, DX=action, DS:SI=name.
           action: bits 0-3 if it exists (0 fail, 1 open, 2 truncate),
                   bits 4-7 if it does not (0 fail, 1 create).
           CX on return says what happened: 1 opened, 2 created, 3 truncated. */
        char fn[300]; HANDLE f; DWORD slot, disp;
        uint16_t act = (uint16_t)(R_DX & 0xFFFF);
        uint16_t exists = act & 0x0F, missing = (act >> 4) & 0x0F;
        DWORD mode = R_BX & 3;
        DWORD acc = (mode == 1) ? GENERIC_WRITE
                  : (mode == 2) ? (GENERIC_READ | GENERIC_WRITE) : GENERIC_READ;
        v86_str(R_DS, R_SI, fn, sizeof(fn));
        if      (exists == 2 && missing == 1) disp = CREATE_ALWAYS;
        else if (exists == 1 && missing == 1) disp = OPEN_ALWAYS;
        else if (exists == 0 && missing == 1) disp = CREATE_NEW;
        else if (exists == 2 && missing == 0) disp = TRUNCATE_EXISTING;
        else                                  disp = OPEN_EXISTING;
        f = CreateFileA(fn, acc, FILE_SHARE_READ, NULL, disp,
                        (DWORD)(R_CX & 0x3F) ? (DWORD)(R_CX & 0x3F)
                                             : FILE_ATTRIBUTE_NORMAL, NULL);
        if (f == INVALID_HANDLE_VALUE) { SETAX(2); ERRCF(); }
        else {
            uint16_t res = (disp == CREATE_NEW) ? 2
                         : (disp == TRUNCATE_EXISTING || disp == CREATE_ALWAYS) ? 3 : 1;
            if (disp == OPEN_ALWAYS && GetLastError() != ERROR_ALREADY_EXISTS) res = 2;
            slot = dos_fh_alloc((void *const *)m->fh, m->std_open);
            if (slot < DOS_MAX_FILES) { m->fh[slot] = f; SETAX(slot); SET16(R_CX, res); OKCF(); }
            else { CloseHandle(f); SETAX(4); ERRCF(); }
        }
    } else if (ah == 0x59) {                    /* get extended error */
        /* Four answers, not one: extended code (AX), class (BH), suggested
           action (BL) and locus (CH).  The pairings are MEASURED, by provoking
           each failure on the oracle and asking (tools/dostest/p_err.asm):
             codes 2, 3, 18  -> BX=0803, CH=02   (not-found family)
             code  6         -> BX=0704, CH=01   (bad handle)
           CL is left ALONE -- the oracle returns it still holding the caller's
           value, so writing it would be an invention. */
        uint16_t e = m->last_err, bx59 = 0;
        uint8_t ch59 = 0;
        /* The table moved to src/dos/dos_err.h so the off-VM battery can pin it
           (tools/dostest/err_test.c) and so there is exactly one place a row can
           be added. Rows 5 (access denied) and 0x50 (file exists) were provoked
           and measured in session 52; before that both fell into the UNMEASURED
           arm below. */
        if (!dos_err_classify(e, &bx59, &ch59)) {
            /* Rather than fabricate a class for a code we have not provoked on
               real DOS, say so. Extend p_err.asm and dos_err.h together. */
            tp = zput(tp, "  INT21 AH=59 class/action/locus UNMEASURED for code 0x");
            tp = zhex(tp, e); tp = zput(tp, "\r\n");
        }
        SETAX(e);
        SET16(R_BX, bx59);
        R_CX = (R_CX & 0xFFFF00FFu) | (((DWORD)ch59 & 0xFF) << 8);
        OKCF();
    } else if (ah == 0x60) {                    /* truename: DS:SI -> ES:DI */
        char in[300], out[300];
        DWORD n;
        v86_str(R_DS, R_SI, in, sizeof(in));
        n = GetFullPathNameA(in, sizeof(out), out, NULL);
        if (n == 0 || n >= sizeof(out)) { SETAX(3); ERRCF(); }
        else {
            volatile BYTE *d = (volatile BYTE *)((R_ES << 4) + (R_DI & 0xFFFF));
            int k = 0;
            /* Oracle: a RELATIVE name resolves against the current directory and
               comes back fully qualified and UPPER CASE, and existence is not
               required -- "SUB\\FILE.TXT" became "C:\\SUB\\FILE.TXT" with no such dir. */
            while (out[k] && k < 127) {
                char ch = out[k];
                if (ch >= 'a' && ch <= 'z') ch = (char)(ch - 32);
                d[k] = (BYTE)ch; ++k;
            }
            d[k] = 0;
            OKCF();
        }
    } else if (ah == 0x65) {                    /* get extended country info */
        uint8_t al65 = (uint8_t)(R_AX & 0xFF);
        if (al65 == 0x01) {
            /* Oracle layout: [0]=1 id, [1-2]=size 0x26, [3-4]=country,
               [5-6]=code page, [7-40]=34-byte country block.  41 bytes total.
               NOTE the block here is the 34-byte form (24 meaningful + 10 zero),
               where AH=38h writes only 24 -- measured, not assumed. */
            volatile BYTE *d = (volatile BYTE *)((R_ES << 4) + (R_DI & 0xFFFF));
            uint16_t cap = (uint16_t)(R_CX & 0xFFFF), k;
            uint8_t blk[41];
            int j;
            for (j = 0; j < 41; ++j) blk[j] = 0;
            blk[0] = 0x01; blk[1] = 0x26; blk[2] = 0x00;
            blk[3] = 0x01; blk[4] = 0x00;                /* country 1  */
            blk[5] = 0xB5; blk[6] = 0x01;                /* code page 437 */
            for (j = 0; j < 24; ++j) blk[7 + j] = ctry_us[j];
            blk[7 + 18] = (uint8_t)(DOS_CASEMAP_OFF & 0xFF);
            blk[7 + 19] = (uint8_t)(DOS_CASEMAP_OFF >> 8);
            blk[7 + 20] = (uint8_t)(DOS_HDLR_SEG & 0xFF);
            blk[7 + 21] = (uint8_t)(DOS_HDLR_SEG >> 8);
            for (k = 0; k < 41 && k < cap; ++k) d[k] = blk[k];
            SETAX(0x01B5); OKCF();                       /* oracle: AX = code page */
        } else if (al65 >= 0x02 && al65 <= 0x07) {
            /* Table subfunctions: ES:DI gets a 5-byte descriptor -- the
               subfunction id, then a FAR POINTER to the table itself.  ATTRIB
               wants AL=07 and COMMAND.COM AL=04, which is why AL=01 alone was
               not enough. Offsets from dos_layout.h; contents in dos_ctab.h,
               dumped from the oracle rather than synthesised. */
            volatile BYTE *d = (volatile BYTE *)((R_ES << 4) + (R_DI & 0xFFFF));
            uint16_t off = 0;
            switch (al65) {
            case 0x02: off = DOS_CTAB_UPPER;   break;
            case 0x04: off = DOS_CTAB_FNUPPER; break;
            case 0x05: off = DOS_CTAB_FNTERM;  break;
            case 0x06: off = DOS_CTAB_COLLATE; break;
            case 0x07: off = DOS_CTAB_DBCS;    break;
            default:   off = DOS_CTAB_UPPER;   break;   /* AL=03, same shape */
            }
            d[0] = al65;
            d[1] = (BYTE)(off & 0xFF);        d[2] = (BYTE)(off >> 8);
            d[3] = (BYTE)(DOS_CTAB_SEG & 0xFF); d[4] = (BYTE)(DOS_CTAB_SEG >> 8);
            SETAX(0x01B5); OKCF();
        } else {
            tp = zput(tp, "  INT21 AH=65 AL=0x"); tp = zhex(tp, al65);
            tp = zput(tp, " UNIMPLEMENTED subfunction\r\n");
            m->unimpl21[0x65 >> 3] |= (uint8_t)(1u << (0x65 & 7));
            SETAX(1); ERRCF();
        }
    } else if (ah == 0x69) {                    /* get/set volume serial number */
        uint8_t al69 = (uint8_t)(R_AX & 0xFF);
        if (al69 == 0x00) {
            /* Oracle layout: [0-1] NOT WRITTEN (came back poisoned), [2-5]
               serial dword, [6-16] 11-byte label, [17-24] 8-byte fs type. */
            volatile BYTE *d = (volatile BYTE *)((R_DS << 4) + (R_DX & 0xFFFF));
            char label[64], fstype[32];
            DWORD serial = 0, maxc = 0, flags = 0;
            int k;
            for (k = 0; k < 64; ++k) label[k] = 0;
            for (k = 0; k < 32; ++k) fstype[k] = 0;
            if (GetVolumeInformationA(NULL, label, sizeof(label), &serial,
                                      &maxc, &flags, fstype, sizeof(fstype))) {
                d[2] = (BYTE)( serial        & 0xFF);
                d[3] = (BYTE)((serial >> 8)  & 0xFF);
                d[4] = (BYTE)((serial >> 16) & 0xFF);
                d[5] = (BYTE)((serial >> 24) & 0xFF);
                for (k = 0; k < 11; ++k) d[6 + k]  = (BYTE)(label[k] ? label[k] : ' ');
                for (k = 0; k < 8;  ++k) d[17 + k] = (BYTE)(fstype[k] ? fstype[k] : ' ');
                OKCF();
            } else { SETAX(0x0F); ERRCF(); }
        } else {
            tp = zput(tp, "  INT21 AH=69 AL=0x"); tp = zhex(tp, al69);
            tp = zput(tp, " UNIMPLEMENTED (set serial)\r\n");
            m->unimpl21[0x69 >> 3] |= (uint8_t)(1u << (0x69 & 7));
            SETAX(1); ERRCF();
        }
    } else if (ah == 0x47) {                    /* get current directory -> DS:SI */
        /* DOS returns the path WITHOUT the drive letter and WITHOUT a leading
           backslash, ASCIIZ.  Oracle at the root writes exactly ONE byte -- the
           terminating NUL -- and leaves the rest of the caller's 64-byte buffer
           untouched, so we must not pad it.
           Wanted by four of the five real 6.22 tools we ran (TREE, ATTRIB,
           XCOPY, COMMAND.COM), which is why it came first.  GH #32. */
        char cwd[300];
        DWORD n = GetCurrentDirectoryA(sizeof(cwd), cwd);
        uint8_t dl47 = (uint8_t)(R_DX & 0xFF);
        uint8_t curdrv = (n >= 2 && cwd[1] == ':')
                       ? (uint8_t)((cwd[0] | 0x20) - 'a' + 1) : 3;
        /* ── ★ 0xF0 IS krnl386 TALKING TO ntvdm, AND WE ARE ntvdm. (#128, s37) ──
             krnl386 keeps a per-drive byte table at its DGROUP 0x2a2 and, for any
             drive flagged there, re-issues this call through seg1:0x0834, which is
             literally `mov dl,0xF0 / call <dispatcher>`. 0xF0 is not a drive under
             any DOS convention -- it is a sentinel between the two halves of one
             product, and the 16-bit half is readable, so answering it is
             implementing a protocol rather than guessing at one. It is what stops
             WOWEXEC.EXE resolving: the path build at seg1:0x1f55 needs a current
             directory, this call fails, and `jae` at seg1:0x1fd5 takes the error
             exit before the PATH search is ever reached.
           ⚠ Only the EXACT sentinel, never "any invalid drive". A DOS program that
             passes garbage in DL still gets the error DOS gives it -- turning that
             into a plausible answer would be the "runs but lies" class. */
        if (dl47 == 0xF0) {
            tp = zput(tp, "  INT21 AH=47 drive 0xF0 (WOW sentinel) -> current drive\r\n");
            dl47 = 0;
        }
        /* ── ★ AND PER-DRIVE CURRENT DIRECTORIES ARE REAL DOS BEHAVIOUR. ─────────
             This answered only for the drive we happened to be on and returned
             "invalid drive" for every other, with a note saying so. Win32 keeps a
             current directory per drive too -- that is what the hidden `=C:`
             environment variables are -- and GetFullPathNameA("X:") reads it. So
             ask for the drive the caller named, and keep the honest refusal for a
             drive that genuinely is not there (GetLogicalDrives), which is the
             error DOS itself returns. GH #32. */
        if (n && dl47 && dl47 != curdrv && dl47 <= 26) {
            if (GetLogicalDrives() & (1u << (dl47 - 1))) {
                char spec[4]; spec[0] = (char)('A' + dl47 - 1); spec[1] = ':';
                spec[2] = 0;
                n = GetFullPathNameA(spec, sizeof(cwd), cwd, NULL);
            } else n = 0;
        }
        if (n == 0 || n >= sizeof(cwd)) {
            tp = zput(tp, "  INT21 AH=47 drive 0x"); tp = zhex(tp, dl47);
            tp = zput(tp, " -> invalid drive\r\n");
            SETAX(0x0F); ERRCF();
        } else {
            volatile BYTE *dst = (volatile BYTE *)((R_DS << 4) + (R_SI & 0xFFFF));
            const char *p47 = cwd;
            int k = 0;
            if (cwd[1] == ':') p47 += 2;              /* drop "C:"            */
            if (*p47 == '\\' || *p47 == '/') ++p47;   /* drop the separator   */
            while (p47[k] && k < 63) { dst[k] = (BYTE)p47[k]; ++k; }
            dst[k] = 0;
            SETAX(0x0100); OKCF();                    /* oracle: AX=0100      */
        }
    } else if (ah == 0x3B) {                    /* chdir: DS:DX = ASCIIZ path */
        char fn[300];
        v86_str(R_DS, R_DX, fn, sizeof(fn));
        if (SetCurrentDirectoryA(fn)) { OKCF(); }
        else { SETAX(3); ERRCF(); }             /* oracle: AX=0003, CF=1      */
    } else if (ah == 0x36) {                    /* get free disk space: DL = drive */
        /* AX=sectors/cluster BX=free clusters CX=bytes/sector DX=total clusters.
           AN INVALID DRIVE RETURNS AX=FFFF WITH CARRY CLEAR -- oracle-confirmed,
           and easy to get wrong: it is not a CF error.  Counts are 16-bit in the
           DOS interface, so a large volume has to be clamped rather than wrapped. */
        uint8_t dl36 = (uint8_t)(R_DX & 0xFF);
        DWORD spc = 0, bps = 0, freec = 0, totc = 0;
        char root[4]; char *rp = 0;
        if (dl36) { root[0] = (char)('A' + dl36 - 1); root[1] = ':'; root[2] = '\\';
                    root[3] = 0; rp = root; }
        if (dl36 <= 26 && GetDiskFreeSpaceA(rp, &spc, &bps, &freec, &totc)) {
            SETAX((uint16_t)spc);
            SET16(R_BX, freec > 0xFFFF ? 0xFFFF : freec);
            SET16(R_CX, (uint16_t)bps);
            SET16(R_DX, totc  > 0xFFFF ? 0xFFFF : totc);
            OKCF();
        } else {
            SETAX(0xFFFF); OKCF();
        }
    } else if (ah == 0x38) {                    /* get/set country information */
        uint8_t al38 = (uint8_t)(R_AX & 0xFF);
        uint16_t want = (al38 == 0xFF) ? (uint16_t)(R_BX & 0xFFFF)
                                       : (uint16_t)(al38 ? al38 : 1);
        if ((R_DX & 0xFFFF) == 0xFFFF) {        /* DX=FFFF selects SET, not GET */
            tp = zput(tp, "  INT21 AH=38 SET country UNIMPLEMENTED\r\n");
            m->unimpl21[0x38 >> 3] |= (uint8_t)(1u << (0x38 & 7));
            SETAX(2); ERRCF();
        } else if (want == 1) {                 /* USA -- the only block we have */
            volatile BYTE *b = (volatile BYTE *)((R_DS << 4) + (R_DX & 0xFFFF));
            int k;
            for (k = 0; k < 24; ++k) b[k] = ctry_us[k];
            b[18] = (BYTE)(DOS_CASEMAP_OFF & 0xFF);
            b[19] = (BYTE)(DOS_CASEMAP_OFF >> 8);
            b[20] = (BYTE)(DOS_HDLR_SEG & 0xFF);
            b[21] = (BYTE)(DOS_HDLR_SEG >> 8);
            SETAX(1); SET16(R_BX, 1); OKCF();
        } else {
            /* We only have measured data for country 1. Inventing a block for
               another country would be exactly the from-memory guess the
               programme forbids, so say so rather than fabricate one. */
            tp = zput(tp, "  INT21 AH=38 country 0x"); tp = zhex(tp, want);
            tp = zput(tp, " UNIMPLEMENTED (only country 1 measured)\r\n");
            m->unimpl21[0x38 >> 3] |= (uint8_t)(1u << (0x38 & 7));
            SETAX(2); ERRCF();
        }
    } else if (ah == 0x58) {                    /* get/set memory allocation strategy */
        uint8_t al58 = (uint8_t)(R_AX & 0xFF);
        if (al58 == 0x00)      { SETAX(m->alloc_strat); OKCF(); }
        else if (al58 == 0x01) { m->alloc_strat = (uint8_t)(R_BX & 0xFF); OKCF(); }
        else if (al58 == 0x02) { SETAX((R_AX & 0xFF00) | m->umb_link); OKCF(); }
        else if (al58 == 0x03) { m->umb_link = (uint8_t)(R_BX & 0xFF); OKCF(); }
        else {
            tp = zput(tp, "  INT21 AH=58 AL=0x"); tp = zhex(tp, al58);
            tp = zput(tp, " UNIMPLEMENTED subfunction\r\n");
            m->unimpl21[0x58 >> 3] |= (uint8_t)(1u << (0x58 & 7));
            SETAX(1); ERRCF();
        }
    } else if (ah == 0x52) {                    /* get list of lists -> ES:BX */
        /* The word at ES:BX-2 is the first MCB segment, and that is the field a
           memory walker actually follows -- it is filled in truthfully from our
           own MCB chain. The rest of SysVars is a stub, so it is ZEROED rather
           than left as whatever was in memory: a walker that follows a garbage
           DPB or SFT pointer wanders off into nonsense, which is the silent
           failure #27 exists to remove, whereas a null pointer stops it. */
        if (m->sysvars_seg) {
            SET16(R_ES, m->sysvars_seg);
            SET16(R_BX, m->sysvars_off);
            tp = zput(tp, "  INT21 AH=52 list-of-lists (MCB head only; rest stubbed)\r\n");
            OKCF();
        } else {
            tp = zput(tp, "  INT21 AH=52 UNIMPLEMENTED (no SysVars planted)\r\n");
            m->unimpl21[0x52 >> 3] |= (uint8_t)(1u << (0x52 & 7));
            SETAX(1); ERRCF();
        }
    } else if (ah == 0x44) {                    /* IOCTL (C-runtime isatty etc.) */
        BYTE al = (BYTE)(R_AX & 0xFF);
        WORD bx = (WORD)(R_BX & 0xFFFF);
        if (al == 0x00)        { SET16(R_DX, (bx < 5) ? 0x80D3 : 0x0002); OKCF(); }
        else if (al == 0x06 || al == 0x07) { SETAX((R_AX & 0xFF00) | 0xFF); OKCF(); }
        /* ── ★★ THE DRIVE-CLASSIFICATION TRIO. (GH #32, #128, session 37) ─────────
             AL = 08h "is this block device removable", 09h "is it remote", 0Eh "get
             the logical drive map". BL is the drive (0 = default, 1 = A) and all
             three answer in REGISTERS -- no buffer anywhere near them.
           ⚠ They used to fall into the `else { OKCF(); }` below, which is the worst
             answer available: carry clear, meaning success, with the registers the
             caller happened to be holding. That is the "runs but lies" class, and it
             is what krnl386 uses to decide whether a drive is local. It probes every
             drive with 44/08, 44/09 and 44/0E in a loop (measured: once per drive,
             descending), and with all three lying it marked drive C: in its own
             per-drive flag table at DGROUP 0x2a2 -- which routes every later
             INT 21h AH=47h for C: through a pre-handler that forces CF, which makes
             its path canonicaliser fail, which makes LoadModule("WOWEXEC.EXE")
             report "file not found" without ever opening a file.
           ▸ Answered from the host, which is where the guest's drives really are.
           ▸ NOT yet checked against the MS-DOS 6.22 oracle -- the register contract
             here is from the documented interface, not from a run. Worth a panel
             (#24) since the whole point of M9 is that we do not write these from
             memory. The DX bits beyond 12 are the ones to confirm. */
        else if (al == 0x08 || al == 0x09 || al == 0x0E) {
            BYTE drv = (BYTE)(bx & 0xFF);            /* 0 = default drive           */
            UINT ty = 0;
            if (!drv) {
                char cw[300];
                DWORD n = GetCurrentDirectoryA(sizeof(cw), cw);
                drv = (n >= 2 && cw[1] == ':') ? (BYTE)((cw[0] | 0x20) - 'a' + 1) : 3;
            }
            if (drv >= 1 && drv <= 26 && (GetLogicalDrives() & (1u << (drv - 1)))) {
                char root[4]; root[0] = (char)('A' + drv - 1); root[1] = ':';
                root[2] = '\\'; root[3] = 0;
                ty = GetDriveTypeA(root);
            }
            if (!ty || ty == 1) { SETAX(0x000F); ERRCF(); }   /* invalid drive      */
            else if (al == 0x08) {
                /* AX = 0 removable, 1 fixed. A CD is removable media. */
                SETAX((ty == DRIVE_REMOVABLE || ty == DRIVE_CDROM) ? 0 : 1); OKCF();
            } else if (al == 0x09) {
                /* DX = the device attribute word; bit 12 = the drive is remote.
                   Nothing else in it is load-bearing for the callers we have. */
                SET16(R_DX, (ty == DRIVE_REMOTE) ? 0x1000 : 0x0000); OKCF();
            } else {
                /* AL = 0 when only one letter maps to the block device, which is
                   true of every drive we can see (we do not emulate a SUBST). */
                SETAX(R_AX & 0xFF00); OKCF();
            }
            tp = zput(tp, "  INT21 AH=44 AL=0x"); tp = zhex(tp, al);
            tp = zput(tp, " drive 0x"); tp = zhex(tp, drv);
            tp = zput(tp, " type "); tp = zhex(tp, ty);
            tp = zput(tp, " -> AX=0x"); tp = zhex(tp, R_AX & 0xFFFF);
            tp = zput(tp, " DX=0x"); tp = zhex(tp, R_DX & 0xFFFF);
            tp = zput(tp, (*pfl & 1) ? " (err)\r\n" : "\r\n");
        }
        else                   { OKCF(); }
        if (al != 0x08 && al != 0x09 && al != 0x0E) {
            tp = zput(tp, "  INT21 AH=44 ioctl AL=0x"); tp = zhex(tp, al);
            tp = zput(tp, " BX=0x"); tp = zhex(tp, bx); tp = zput(tp, "\r\n");
        }
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
        SET16(R_BX, m->psp_seg); OKCF();
    } else if (ah == 0x50) {                    /* set current PSP */
        m->psp_seg = (uint16_t)(R_BX & 0xFFFF); OKCF();
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
    } else if (ah == 0x33) {                    /* get/set Ctrl-Break, get true version */
        uint8_t al33 = (uint8_t)(R_AX & 0xFF);
        if (al33 == 0x00) { SET16(R_DX, 0); OKCF(); }          /* get: break off  */
        else if (al33 == 0x01) { OKCF(); }                     /* set: accepted   */
        else if (al33 == 0x05) { SET16(R_DX, 3); OKCF(); }     /* boot drive = C: */
        else if (al33 == 0x06) {                               /* get TRUE version */
            /* BL=major BH=minor DL=revision DH=flags.  DH bit 3 = DOS in ROM,
               bit 4 = DOS in HMA; we are in neither, so 0.  Note the oracle
               reports DH=0x10 because that image boots DOS=HIGH -- DH is a
               property of the host's configuration, not of the version, which
               is why the probes do not compare it. */
            SET16(R_BX, (uint16_t)((m->ver_minor << 8) | m->ver_major));
            SET16(R_DX, 0x0000);
            OKCF();
        } else {                                               /* unknown subfn   */
            tp = zput(tp, "  INT21 AH=33 AL=0x"); tp = zhex(tp, al33);
            tp = zput(tp, " UNIMPLEMENTED subfunction\r\n");
            m->unimpl21[0x33 >> 3] |= (uint8_t)(1u << (0x33 & 7));
            ERRCF();
        }
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
    } else if (!dos622_defines(ah)) {
        /* MS-DOS 6.22 has nothing here, and what IT does is the specification:
           return with AX unchanged and CF clear, touching nothing.  Measured on
           the oracle (tools/dostest/p_defs.asm) -- AH=6Dh..E0h, plus the
           documented null functions, all come back with every poisoned output
           register intact.  Failing loudly here would be US inventing an error
           that real DOS does not report, which breaks programs that probe for
           an extension by calling it and checking CF. */
        tp = zput(tp, "  INT21 AH=0x"); tp = zhex(tp, ah);
        tp = zput(tp, " undefined on 6.22 -- no-op (matches DOS)\r\n");
        m->noop21[(ah & 0xFF) >> 3] |= (uint8_t)(1u << (ah & 7));
        OKCF();
    } else {                                    /* unhandled service */
        /* GH #27. Recorded as well as logged, so the STAGE2 block can list every
           service a run actually wanted -- that list is the to-do list.
           Reaching HERE means 6.22 defines a real service at this AH and we have
           not written it yet.  CF=1 is right for that: a quiet "success" would
           tell the program its request worked when nothing happened.  Functions
           DOS does not define are handled above and stay silent, matching DOS. */
        tp = zput(tp, "  INT21 AH=0x"); tp = zhexb(tp, (unsigned)ah);
        tp = zput(tp, " AL=0x"); tp = zhexb(tp, (unsigned)(R_AX & 0xFF));
        tp = zput(tp, " UNIMPLEMENTED\r\n");
        m->unimpl21[(ah & 0xFF) >> 3] |= (uint8_t)(1u << (ah & 7));
        ERRCF();
    }

    /* GH #34: remember the last failure for AH=59h. Done HERE, once, rather
       than at each of the ~20 error sites -- CF and AX are already exactly what
       the guest is about to see. 59h itself is excluded so reading the error
       does not overwrite it. */
    if (ah != 0x59 && (*pfl & 1)) m->last_err = (uint16_t)(R_AX & 0xFFFF);

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

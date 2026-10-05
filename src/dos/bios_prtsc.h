/* bios_prtsc.h -- the BIOS's default INT 05h (Print Screen), as a byte sequencer. #274.
 *
 * The guest half is bios_kbdact.asm's `p5`: a loop that asks the host (BOP 09h) for the
 * next byte and prints it through INT 17h, so a hooked INT 17h sees every byte. This is
 * the host half: WHICH byte comes next, and whether the printer's answer ends the job.
 * Pure, so tests/unit/prtsc_test.c can run it; the host supplies the screen reader.
 *
 * ── THE CONTRACT, AND WHERE IT COMES FROM. ─────────────────────────────────────────────
 *   IBM PC/AT Technical Reference, BIOS listing PRINT_SCREEN (and RBIL INT 05h,
 *   MEMORY.LST 0050:0000):
 *     • 0050:0000 = 01h while printing; a second INT 05h meanwhile returns at once.
 *       00h when the screen has gone out, FFh if the printer reported an error.
 *     • Columns and the active page from INT 10h AH=0Fh; rows = 25 on the AT -- the
 *       EGA/VGA-era routine takes 0040:0084 + 1, which is what we use (0 -> 25).
 *     • First the "initial" line feed / carriage return, then every cell of every row,
 *       read with INT 10h AH=08h at that position (a NUL cell prints as a space), each
 *       row followed by line feed / carriage return. The cursor is saved first (AH=03h)
 *       and put back at the end, error or not.
 *     • After each SCREEN byte, INT 17h's AH is tested against 29h (bit 0 time-out,
 *       bit 3 I/O error, bit 5 out of paper); any of them ends the job with FFh. The
 *       line-end bytes are not tested.
 *   ⚠ UNMEASURED, FROM THE LISTING AS RECALLED: the line-end order is LF then CR
 *     ("WILL NOW SEND INITIAL LF,CR TO PRINTER"), and only screen bytes are tested.
 *     p_kbd3 records the first bytes INT 17h receives so PCem's AMI BIOS settles it.
 *   ⛔ OURS KEEPS THAT STATUS HOST-SIDE (main.c g_prtsc_status), NOT AT 0050:0000:
 *     that byte is DOS_HDLR_SEG:0000, the first byte of our INT 21h stub. See prtsc_bop.
 *   ⚠ OURS READS THE SCREEN THROUGH OUR OWN INT 10h, HOST-SIDE, not through the guest's
 *     INT 10h vector: a program that hooked INT 10h would see the IBM routine's 2000
 *     AH=02h/08h calls and does not see ours. INT 17h -- the half a program actually
 *     hooks to capture a print-out -- is the guest's.
 */
#ifndef BIOS_PRTSC_H
#define BIOS_PRTSC_H

#include <stdint.h>

#define PRTSC_STATUS_LIN  0x500u    /* 0050:0000 */
#define PRTSC_BUSY        0x01
#define PRTSC_OK          0x00
#define PRTSC_ERR         0xFF
#define PRTSC_AH_ERRMASK  0x29      /* INT 17h AH: time-out | I/O error | out of paper */

enum { PRTSC_EMIT = 0, PRTSC_DONE = 1, PRTSC_ERROR = 2 };
enum { PRTSC_P_LF0, PRTSC_P_CR0, PRTSC_P_CELL, PRTSC_P_LF, PRTSC_P_CR, PRTSC_P_END };

typedef struct {
    uint8_t  active;
    uint8_t  cols, rows, page;
    uint8_t  row, col, phase;
    uint8_t  judge;                 /* the last byte out was a screen cell: test its AH */
    uint16_t cursor;                /* AH=03h's DX, put back at the end                 */
} bios_prtsc;

/* `rows84` is 0040:0084 (rows - 1; 0 on a BIOS that never set it). */
static inline void bios_prtsc_begin(bios_prtsc *s, uint8_t cols, uint8_t rows84,
                                    uint8_t page, uint16_t cursor)
{
    s->active = 1;
    s->cols = cols; s->rows = (uint8_t)(rows84 ? rows84 + 1 : 25); s->page = page;
    s->row = s->col = 0; s->phase = PRTSC_P_LF0; s->judge = 0; s->cursor = cursor;
}

/* The next byte for INT 17h. `ah` is what INT 17h answered for the PREVIOUS byte
   (ignored for the first one and after a line-end byte). PRTSC_EMIT -> *out is the
   byte; PRTSC_DONE / PRTSC_ERROR -> the job is over and `active` is clear. `readc`
   returns the character at (row, col) of the active page. */
static inline int bios_prtsc_step(bios_prtsc *s, uint8_t ah,
                                  uint8_t (*readc)(void *ctx, uint8_t row, uint8_t col),
                                  void *ctx, uint8_t *out)
{
    uint8_t c;
    if (!s->active) return PRTSC_DONE;
    if (s->judge && (ah & PRTSC_AH_ERRMASK)) { s->active = 0; return PRTSC_ERROR; }
    s->judge = 0;
    switch (s->phase) {
    case PRTSC_P_LF0: *out = 0x0A; s->phase = PRTSC_P_CR0; return PRTSC_EMIT;
    case PRTSC_P_CR0:
        *out = 0x0D;
        s->phase = (s->rows && s->cols) ? PRTSC_P_CELL : PRTSC_P_END;
        return PRTSC_EMIT;
    case PRTSC_P_CELL:
        c = readc(ctx, s->row, s->col);
        *out = c ? c : (uint8_t)' ';
        s->judge = 1;
        if (++s->col >= s->cols) s->phase = PRTSC_P_LF;
        return PRTSC_EMIT;
    case PRTSC_P_LF: *out = 0x0A; s->phase = PRTSC_P_CR; return PRTSC_EMIT;
    case PRTSC_P_CR:
        *out = 0x0D;
        s->col = 0;
        s->phase = (++s->row >= s->rows) ? PRTSC_P_END : PRTSC_P_CELL;
        return PRTSC_EMIT;
    default:
        s->active = 0;
        return PRTSC_DONE;
    }
}

#endif /* BIOS_PRTSC_H */

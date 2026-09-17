/* vdd_video.c -- see vdd_video.h.  Text mode 3 + graphics mode 13h over the
 * shared video aperture (vmem), with the DAC palette, on the VDD bus.  Pure C. */
#include "vdd_video.h"
#include "vga_font_8x16.h"
#include "vga_font_8x8.h"
#include "vga_font_8x14.h"
#include "vga_defaults.h"

/* ⚠ THE ega16 TABLE THAT WAS HERE IS GONE, and so is the ega64_rgb() that replaced
   it. Both hardcoded the sixteen colours a 16-colour mode renders with, which is
   exactly the assumption this file had to stop making: those colours are
   dac[vpal[i]], and both halves belong to the guest. The defaults now come from
   vga_defaults.h, MEASURED per mode; tools/gen-vgadefs.py refuses to generate it
   unless mode 10h's measured DAC matches what ega64_rgb() computed entry for entry,
   so retiring the formula shifted no colour anywhere. */

/* DAC component (0..63) -> 8-bit; pack/unpack a palette entry. */
/* ⚠ This is <<2, so a guest writing 0x3F gets 252 and not 255 -- it disagrees by a
   few counts with the (v<<2)|(v>>4) the generated defaults use. Pre-existing, and
   left alone deliberately: it is on the path of every guest that programs a palette,
   so it wants its own before/after rather than riding along with this one. */
static uint32_t dac_pack(uint8_t r, uint8_t g, uint8_t b)
{ return 0xFF000000u | ((uint32_t)(r << 2) << 16) | ((uint32_t)(g << 2) << 8) | (uint32_t)(b << 2); }

/* ── ★★ THE ATTRIBUTE CONTROLLER, WHICH IS WHERE A 16-COLOUR PIXEL GETS ITS COLOUR.
     A 4-bit pixel does NOT index the DAC. It indexes one of the AC's sixteen palette
     registers (st->vpal), and THAT six-bit value indexes the DAC. We stored vpal --
     INT 10h AH=10h has always written it -- and then rendered straight from a fixed
     ega16 table, so nothing a guest did to the palette had any effect. Lemmings sets
     its own palette, which is exactly why its menu came out structurally perfect and
     the wrong colours. */

/* ── WHAT A MODE SET LEAVES BEHIND IS PER MODE. ─────────────────────────────────
     There is no single default AC palette and no single default DAC; the BIOS has a
     table for each mode and they genuinely differ:

         00h-03h, 10h, 12h   AC 00 01 02 03 04 05 14 07 38..3F   DAC = the EGA 64
         0Dh, 0Eh            AC 00 01 02 03 04 05 06 07 10..17   DAC = CGA 16, x4
         13h                 AC 00..0F (identity)                DAC = the VGA 256
         04h/05h/06h/07h/0Fh/11h  each different again

     All of it is measured by tools/dostest/vgadefs.asm, which sets each mode and
     reads the registers BACK -- the AC through 0x3C1, the DAC through 0x3C7/0x3C9 --
     on genuine MS-DOS 6.22. tools/gen-vgadefs.py turns that dump into
     vga_defaults.h. Bit 7 of AL ("do not clear the buffer") changes none of it; the
     probe measures 8Dh and 90h to prove that rather than assume it.

   ⚠⚠ THE SINGLE TABLE THAT WAS HERE WAS INFERRED FROM A PICTURE, AND BOTH HALVES OF
     THE INFERENCE WERE WRONG. Card 11 reprogrammed DAC 0..15 to a ramp in mode 0Dh,
     left the AC alone, and read the bars off the screen: bar 6 followed the ramp, so
     index 6 was concluded to be 6 rather than the 0x14 every reference quotes, and
     bars 8..15 showed the EGA brights, so the high eight were concluded to be
     0x38..0x3F. In mode 0Dh the high eight are really 0x10..0x17 -- and the card
     could not tell, because mode 0Dh's default DAC repeats the same sixteen colours
     four times across 0..0x3F, so 0x10..0x17 and 0x38..0x3F hold identical values.
     A bar's colour is a reading of the whole chain and cannot say which link differs.
   ⚠ And the card ran in 0Dh while the guest that prompted it plays in 10h, where
     index 6 IS 0x14. Lemmings carries that exact table inside VGALEMMI.EXE and uses
     it to choose which DAC entries to program, so with 6 there its colour 6 was
     written to DAC 0x14 and read back from DAC 0x06. */
static int vga_defaults_row(uint8_t mode)
{
    unsigned i;
    for (i = 0; i < VGA_DEFAULT_MODES; ++i)
        if (VGA_DEFAULT_BY_MODE[0][i] == mode) return (int)i;
    /* A mode the BIOS has no table for: a VESA mode, or one nobody defines. Above
       13h means 256 colours, so 13h's defaults; below it, mode 3's. st->mkind is
       not set yet at the point this runs, so the mode number is all there is. */
    for (i = 0; i < VGA_DEFAULT_MODES; ++i)
        if (VGA_DEFAULT_BY_MODE[0][i] == (mode >= 0x13 ? 0x13 : 0x03)) return (int)i;
    return 0;
}

static void vga_defaults_for(uint8_t mode,
                             const unsigned char **ac, const unsigned long **dac)
{
    int i = vga_defaults_row(mode);
    *ac  = VGA_AC_DEFAULT [VGA_DEFAULT_BY_MODE[1][i]];
    *dac = VGA_DAC_DEFAULT[VGA_DEFAULT_BY_MODE[2][i]];
}

/* ── ★ A MODE SET REPROGRAMS THE CRTC, and not doing so lets one screen inherit the
     geometry of the one before it. Lemmings' gameplay sets Offset=22 (44 bytes to
     the line, for a 352-pixel-wide scrolling window); the mode 10h screen AFTER it
     was then drawn 44 bytes to the line instead of 80 and came out as diagonal
     noise. The BIOS writes all 25 registers on every mode set -- these values are
     measured per mode by tools/dostest/vgadefs.asm.
   ▶ THE VERTICAL TIMING IS NOW APPLIED TOO, and it had to be: the BIOS sets these
     registers, the guest does not, so a mode set is the ONLY place a BIOS-set mode
     ever learns its own geometry. Leaving it out was what kept 0x3DA on the old
     two-case guess -- and that guess is wrong by 45 lines in 640x350 (which is
     Lemmings' MENU; its gameplay is 0Dh). The renderer still takes its picture size from the mode
     table; what these feed is the CRT timing, which is a different question.
   ⚠ Line Compare's three registers are loaded here as well, so it holds the BIOS's
     all-ones "no split" rather than a zero we merely happen to treat as inert. */
static void crtc_lc_update(video_state *st);
static void crtc_vt_update(video_state *st);
static void load_default_crtc(video_state *st)
{
    const unsigned char *c = VGA_CRTC_DEFAULT[VGA_DEFAULT_BY_MODE[3][vga_defaults_row(st->mode)]];
    st->crtc_index = 0;
    st->crtc_offset = c[VGA_CRTC_OFFSET];
    st->crtc_start = (uint32_t)(((unsigned)c[VGA_CRTC_START_HI] << 8) | c[VGA_CRTC_START_LO]);
    st->crtc_start_live = (uint16_t)st->crtc_start;   /* the display follows at once */
    st->crtc_start_pend = 0;
    st->crtc_off_seen = 0;
    st->crtc_overflow   = c[0x07];
    st->crtc_maxscan    = c[0x09];
    st->crtc_lc_low     = c[0x18];
    st->crtc_vtotal_lo  = c[0x06];
    st->crtc_vde_lo     = c[0x12];
    st->crtc_vbs_lo     = c[0x15];
    crtc_lc_update(st);
    crtc_vt_update(st);
    st->dirty = 1;
}

/* ── ★★★ THE RENDER PALETTE IS DERIVED, NOT STORED. ─────────────────────────────
     pal[] is what the 8-bit framebuffer indexes; dac[] is what the guest programmed.
     In a 16-colour mode they differ, because the hardware puts the Attribute
     Controller between them:
         pixel (4 bits) -> vpal[pixel] -> 6-bit DAC index -> dac[]
     Rebuilding pal[0..15] through that chain is what makes a guest's palette
     actually appear. Mode 13h bypasses the AC (its 8-bit pixel goes straight to the
     DAC), so there pal[] is simply dac[].

   ⚠ THE FIRST CUT OF THIS WAS WRONG IN A WAY WORTH RECORDING: it wrote a COMPUTED
     EGA colour into pal[0..15] and never consulted dac[] at all. That made the test
     card pass -- the card uses the default DAC -- while leaving Lemmings exactly as
     broken, because Lemmings programs the DAC and then points the AC at it. It also
     clobbered the guest's DAC entries, since pal[] and dac[] were the same array.
     A derived table has to derive from something; substituting a plausible value for
     the real one is the same mistake as a stepped-over call returning a sentinel. */
static void pal_split_note(video_state *st, const uint32_t *prev);
static int  vid_beam(const video_state *st, uint64_t *now, uint32_t *frame_us,
                     uint32_t *vtotal, uint32_t *vdisp, uint32_t *vblank,
                     uint32_t *frame_no, uint32_t *line);   /* fwd: defined with status_in */
static void pal_refresh(video_state *st)
{
    int i;
    uint32_t prev[256];
    for (i = 0; i < 256; ++i) prev[i] = st->pal[i];
    /* A packed 8bpp VESA mode indexes the DAC directly, exactly as mode 13h does.
       (s74b) It used to fall through to the attribute-controller path below because a
       4F02 mode set never touches mkind, so pixels 0..15 went through the EGA remap
       (6 -> DAC 0x14, 8..15 -> 0x38..0x3F) and a VESA guest's first sixteen colours
       were somebody else's. */
    if (st->mkind == VID_KIND_LINEAR8 || (st->in_vesa && st->vesa_bpp <= 8)) {
        for (i = 0; i < 256; ++i) st->pal[i] = st->dac[i];
    } else {
        for (i = 0; i < 16; ++i) {
            uint8_t v = (uint8_t)(st->vpal[i] & 0x3F);
            /* AR14 Color Select supplies the high DAC bits on a real VGA; zero by
               default, which makes this the identity. */
            if (st->attr_mode & 0x80)
                v = (uint8_t)((v & 0x0F) | ((st->attr_cse & 0x03) << 4));
            v = (uint8_t)(v | ((st->attr_cse & 0x0C) << 4));
            st->pal[i] = st->dac[v];
        }
        for (i = 16; i < 256; ++i) st->pal[i] = st->dac[i];
    }
    pal_split_note(st, prev);
    st->dirty = 1;
}

/* ── ★★★★ A PALETTE WRITE MID-FRAME IS A RASTER SPLIT, NOT A NEW PALETTE. ─────────
     s70, the user's "flickers between the right and wrong colours" (Lemmings #1/#2).
     Lemmings' HP-mode timer tick is calibrated to land 320 scanlines after the
     retrace -- pixel row 160, the top of the toolbar -- and the tick's first act is
     to push one set of DAC entries 16..23 (guest ds:2668); after the retrace it
     pushes another (ds:2650). The level is drawn in one set, the toolbar in the
     other, and the beam position is what separates them. The presenter took ONE
     palette per frame, so whichever set the snapshot caught coloured the whole
     screen, and the alternation between the two was the flicker. (The real-DOS
     oracle under QEMU shows one set everywhere too: its default 0x3DA model makes
     the retrace wait return at once, so the two writes land back to back. The
     oracle is not truth for a raster effect; the game's own tables and timing are.)
   ► So per entry: the value at the START of the frame (`pal_base`), the value written
     MID-frame and the row it was written at (`pal_split`/`pal_split_row`), stamped
     with the frame number so a guest that stops splitting is back to one palette a
     frame later. Writes during blanking or on the first two rows are the frame's
     base (Lemmings' post-retrace push lands on row 0). The first write of a new
     frame rebases: what the palette held before it IS what the DAC held at the
     frame start. ntvdd_frame_pal_at resolves a row; the presenter and the capture
     apply it, video_test pins it with a fake clock. */
#define VID_SPLIT_STICK 12u   /* rows: IRQ jitter measured at +-5 rows (s70), with margin */
static void pal_split_note(video_state *st, const uint32_t *prev)
{
    uint64_t now; uint32_t frame_us, vtotal, vdisp, vblank, frame_no, line, row;
    int i, split;
    if (!vid_beam(st, &now, &frame_us, &vtotal, &vdisp, &vblank, &frame_no, &line)
        || !st->gh || !vdisp) {
        for (i = 0; i < 256; ++i) if (st->pal[i] != prev[i]) st->pal_base[i] = st->pal[i];
        return;
    }
    if (frame_no != st->pal_frame_no) {           /* first write of this frame: rebase */
        for (i = 0; i < 256; ++i) st->pal_base[i] = prev[i];
        st->pal_frame_no = frame_no;
    }
    row   = (line < vdisp) ? (uint32_t)(((uint64_t)line * st->gh) / vdisp) : 0xFFFFu;
    split = (row != 0xFFFFu && row >= 2u);
    for (i = 0; i < 256; ++i) {
        if (st->pal[i] == prev[i]) continue;
        if (split) {
            /* ── THE BOUNDARY IS STICKY (s70, the user's HP run). The tick that writes
                 this split is an IRQ we deliver with ~100-300 us of jitter, so its row
                 wandered 160..169 frame to frame (heartbeat `lastrow`). Drawn faithfully
                 that is a ten-row band flickering between palettes -- worse than the
                 single-palette picture it replaced. A real PC's IRQ lands within a
                 microsecond of the same row every frame. So a mid-frame write within
                 VID_SPLIT_STICK rows of this entry's live split keeps the OLD row: the
                 boundary stays where the guest first put it, and only a genuinely
                 different row (a new effect) moves it. */
            uint32_t old = st->pal_split_row[i];
            int live = old && (uint32_t)(frame_no - st->pal_split_frame[i]) <= 2u;
            uint32_t d = old > row ? old - row : row - old;
            st->pal_split[i]       = st->pal[i];
            st->pal_split_row[i]   = (live && d <= VID_SPLIT_STICK) ? (uint16_t)old : (uint16_t)row;
            st->pal_split_frame[i] = frame_no;
            st->pal_split_notes++;
        } else {
            st->pal_base[i] = st->pal[i];
        }
    }
}

void vdd_video_frame_touch(video_state *st)
{
    uint64_t now; uint32_t frame_us, vtotal, vdisp, vblank, frame_no, line;
    st->frame.palette_base  = st->pal_base;
    st->frame.palette_split = st->pal_split;
    st->frame.split_row     = st->pal_split_row;
    st->frame.split_frame   = st->pal_split_frame;
    st->frame.frame_no = vid_beam(st, &now, &frame_us, &vtotal, &vdisp, &vblank, &frame_no, &line)
                       ? frame_no : st->pal_frame_no;
}

/* A mode set reloads the DAC and the AC palette. Real hardware does this, and
   without it a program that reprogrammed the palette leaves the NEXT program (or
   the text screen it returns to) drawn in its colours -- usually near-black, so
   text mode looks dead. */
static void load_default_palette(video_state *st)
{
    int i;
    /* The guest asked us not to (AH=12h BL=31h). Leave both the DAC and the
       attribute palette exactly as it left them. */
    const unsigned char *ac; const unsigned long *dc;
    for (i = 0; i < 256; ++i) st->pal_split_row[i] = 0;   /* a mode set ends any raster split */
    if (st->def_pal_off) return;
    st->pal_resets++;
    /* ⚠ dac_hi_since_reset ON ITS OWN CANNOT REPORT ANYTHING. The counters are
       printed after the guest has exited, and a guest exits through a mode set back
       to text -- so "since the last reset" is always "since a moment after the last
       thing the guest drew", and the answer is always zero. It read zero for
       Lemmings and was taken as evidence that a mode set had wiped a palette the
       game never rewrote; the disassembly says the game sets the mode and THEN
       writes the palette, which is the only order that can work on real hardware.
       Carry the running maximum too, so the epoch that had the writes survives. */
    if (st->dac_hi_since_reset > st->dac_hi_max)
        st->dac_hi_max = st->dac_hi_since_reset;
    st->dac_hi_since_reset = 0;
    vga_defaults_for(st->mode, &ac, &dc);
    for (i = 0; i < 16; ++i)   st->vpal[i] = ac[i];
    st->vpal[16] = 0;
    st->attr_ff = st->attr_index = st->attr_mode = st->attr_cse = 0;
    for (i = 0; i < 256; ++i)  st->dac[i] = 0xFF000000u | (uint32_t)dc[i];
    pal_refresh(st);
}

/* ── 0x3C0 / 0x3C1: index and data on ONE port, alternating. ─────────────────────
     Write to 0x3C0 and the flip-flop decides whether it lands in the index or the
     data half. The flip-flop is reset by READING 0x3DA -- every guest does that read
     first, which is why status_in resets it and why claiming 0x3DA was already
     necessary for this to work at all.
   ⚠ Bit 5 of the INDEX is "video enable" and is not part of the register number: a
     guest programming the palette clears it and sets it again when it has finished.
     Masking it off the index (0x1F) is the difference between writing register 0 and
     writing register 32. */
static void attr_out(void *self, uint16_t port, uint8_t w, uint32_t v)
{
    video_state *st = (video_state *)self;
    uint8_t val = (uint8_t)(v & 0xFF);
    (void)w;
    if (port == 0x3C1) return;                       /* data port is read-only       */
    if (!st->attr_ff) { st->attr_index = val; st->attr_ff = 1; return; }
    st->attr_ff = 0;
    switch (st->attr_index & 0x1F) {
    case 0x00: case 0x01: case 0x02: case 0x03:
    case 0x04: case 0x05: case 0x06: case 0x07:
    case 0x08: case 0x09: case 0x0A: case 0x0B:
    case 0x0C: case 0x0D: case 0x0E: case 0x0F:
        st->vpal[st->attr_index & 0x0F] = (uint8_t)(val & 0x3F);
        st->ac_port_writes++;
        pal_refresh(st);
        break;
    case 0x10:
        /* ── ★ BIT 3 IS BLINK ENABLE, AND IT IS THE HARDWARE'S ANSWER, NOT OURS. ──
             We stored this register and kept a PRIVATE st->blink beside it that only
             INT 10h 1003h could move -- so a program that turns blink off the usual
             way, by writing the attribute controller directly, was ignored and every
             character with attribute bit 7 went on blinking.
             Found in QBasic: its dialogs mark the accelerator letter with bit 7, so
             `Files` rendered as `iles` and `Help` as `elp` every other half-second --
             the letter was there, it was blinking. Two layers with their own copy of
             one fact, disagreeing; AR10 is now the single source. */
        st->attr_mode = val;
        st->blink = (uint8_t)((val >> 3) & 1);
        pal_refresh(st);
        break;
    case 0x11: st->vpal[16] = st->overscan = (uint8_t)(val & 0x3F); st->dirty = 1; break;
    case 0x14: st->attr_cse = val;   pal_refresh(st); break;
    default: break;                                  /* 12h plane enable, 13h pan    */
    }
}

static void attr_in(void *self, uint16_t port, uint8_t w, uint32_t *v)
{
    video_state *st = (video_state *)self;
    uint8_t idx = (uint8_t)(st->attr_index & 0x1F);
    (void)w;
    if (port == 0x3C0) { *v = st->attr_index; return; }   /* index reads back at 3C0 */
    switch (idx) {
    case 0x10: *v = st->attr_mode; break;
    case 0x11: *v = st->vpal[16];  break;
    case 0x14: *v = st->attr_cse;  break;
    default:   *v = (idx < 16) ? st->vpal[idx] : 0; break;
    }
}

/* The standard BIOS mode set.  Dimensions are the modes' documented geometry;
   what makes them right for US is that the renderer now honours them instead of
   forcing 80x25 text.  CGA modes 4/5/6 are marked UNSUPPORTED rather than
   approximated: they use a two-bank interleaved layout at B800 that shares
   nothing with the planar path, and quietly showing a text screen instead is the
   silent failure GH #27 exists to remove. */
static const struct { uint8_t mode, kind, cols, rows; uint16_t w, h; } vid_modes[] = {
    { 0x00, VID_KIND_TEXT,    40, 25,   320, 400 },
    { 0x01, VID_KIND_TEXT,    40, 25,   320, 400 },
    { 0x02, VID_KIND_TEXT,    80, 25,   640, 400 },
    { 0x03, VID_KIND_TEXT,    80, 25,   640, 400 },
    { 0x04, VID_KIND_CGA,     40, 25,   320, 200 },   /* CGA 4-colour   */
    { 0x05, VID_KIND_CGA,     40, 25,   320, 200 },   /* 4-colour, grey */
    { 0x06, VID_KIND_CGA,     80, 25,   640, 200 },   /* CGA 2-colour   */
    { 0x07, VID_KIND_TEXT,    80, 25,   640, 400 },   /* MDA mono text  */
    { 0x0D, VID_KIND_PLANAR,  40, 25,   320, 200 },
    { 0x0E, VID_KIND_PLANAR,  80, 25,   640, 200 },
    { 0x0F, VID_KIND_PLANAR,  80, 25,   640, 350 },
    { 0x10, VID_KIND_PLANAR,  80, 25,   640, 350 },
    { 0x11, VID_KIND_PLANAR,  80, 30,   640, 480 },
    { 0x12, VID_KIND_PLANAR,  80, 30,   640, 480 },
    { 0x13, VID_KIND_LINEAR8, 40, 25,   320, 200 },
};

static uint8_t *cell(video_state *st, int r, int c)   /* -> char byte of (r,c)    */
{ return st->vmem + VID_TEXT_OFF + (r * st->cols + c) * 2; }

static void clear_text(video_state *st, uint8_t attr)
{
    int n = st->cols * st->rows, i;
    uint8_t *t = st->vmem + VID_TEXT_OFF;
    for (i = 0; i < n; ++i) { t[i*2] = ' '; t[i*2+1] = attr; }
}

/* The number of text rows the loaded font gives on a 400-line VGA text screen. */
static uint8_t text_rows_for(uint8_t cell_h)
{ return (uint8_t)(cell_h ? (400 / cell_h) : 25); }

/* ── THE BDA DESCRIBES THE DISPLAY, AND A TEXT APPLICATION BELIEVES IT. ──────────
     0040:0049 mode, 004A columns, 004C page size, 004E page offset, 0050 the cursor
     per page, 0060 the cursor shape, 0062 active page, 0063 the CRTC base, 0084 rows
     minus one, 0085 the character height, 0087-0089 the EGA/VGA info bytes. All of
     it read as ZERO before this: rows-1 = 0 is a one-row screen to a program that
     sizes itself from 0040:0084, and the video-info bytes said "no EGA/VGA" to one
     that checks before it asks for 50 lines. Cheap enough to redo after every INT
     10h call and every frame -- a few dozen byte writes -- and correct by
     construction, since it is derived rather than maintained. */
void vdd_video_bda_sync(video_state *st)
{
    uint8_t *b = st->bda;
    unsigned psize;
    if (!b) return;
    b[0x49] = st->mode;
    b[0x4A] = st->cols; b[0x4B] = 0;
    if (st->mkind == VID_KIND_TEXT) {
        psize = ((unsigned)st->cols * st->rows * 2u + 0xFFu) & ~0xFFu;
        if (psize < 0x800u) psize = 0x800u;
    } else {
        /* ── THE GRAPHICS PAGE SIZE IS PER MODE, and this was a flat 0x2000 for all
             of them. Measured on 6.22: mode 06h is 0x4000 and mode 12h is 0xA000,
             where we said 0x2000 either way. A program that pages by adding this to
             its offset lands inside the previous page. 06h/12h/13h are the
             oracle-verified rows (p_video.asm); the rest are the standard VGA BIOS
             table and are marked unverified in docs/PARITY.md. */
        switch (st->mode) {
        case 0x04: case 0x05: case 0x06: psize = 0x4000u; break;  /* 06h verified   */
        case 0x0D:                       psize = 0x2000u; break;
        case 0x0E:                       psize = 0x4000u; break;
        case 0x0F: case 0x10:            psize = 0x8000u; break;
        case 0x11: case 0x12:            psize = 0xA000u; break;  /* 12h verified   */
        case 0x13:                       psize = 0x2000u; break;  /* 13h verified   */
        default:                         psize = 0x2000u; break;
        }
    }
    b[0x4C] = (uint8_t)psize; b[0x4D] = (uint8_t)(psize >> 8);
    { unsigned poff = (unsigned)st->page * psize;
      b[0x4E] = (uint8_t)poff; b[0x4F] = (uint8_t)(poff >> 8); }
    { unsigned pg = st->page & 7;
      b[0x50 + pg * 2] = st->cur_col; b[0x51 + pg * 2] = st->cur_row; }
    {   /* 0040:0060 follows the same rule as AH=03h: no text cursor in graphics. */
        uint16_t shp = (st->mkind == VID_KIND_TEXT) ? st->cur_shape : 0;
        b[0x60] = (uint8_t)shp; b[0x61] = (uint8_t)(shp >> 8); }
    b[0x62] = st->page;
    { unsigned crtc = (st->mode == 0x07) ? 0x3B4u : 0x3D4u;
      b[0x63] = (uint8_t)crtc; b[0x64] = (uint8_t)(crtc >> 8); }
    b[0x84] = (uint8_t)(st->rows ? st->rows - 1 : 24);
    b[0x85] = st->cell_h; b[0x86] = 0;
    b[0x87] = 0x60;                                    /* 256K, EGA/VGA active, cursor emulation on */
    b[0x88] = 0x09;                                    /* feature/switch bits: enhanced colour */
    /* 0089: bit 0 = VGA active; bits 7,4 = scan lines (0,0 = 350; 0,1 = 400; 1,0 = 200). */
    b[0x89] = (uint8_t)((st->gh == 200) ? 0x81 : (st->gh == 350) ? 0x01 : 0x11);
}

static void scroll_up(video_state *st, int lines, int top, int left,
                      int bot, int right, uint8_t attr)
{
    int r, c;
    if (lines <= 0 || lines > (bot - top + 1)) {
        for (r = top; r <= bot; ++r)
            for (c = left; c <= right; ++c) { uint8_t *p = cell(st, r, c); p[0]=' '; p[1]=attr; }
        return;
    }
    for (r = top; r <= bot - lines; ++r)
        for (c = left; c <= right; ++c) {
            uint8_t *d = cell(st, r, c), *s = cell(st, r + lines, c);
            d[0] = s[0]; d[1] = s[1];
        }
    for (r = bot - lines + 1; r <= bot; ++r)
        for (c = left; c <= right; ++c) { uint8_t *p = cell(st, r, c); p[0]=' '; p[1]=attr; }
}

static void advance(video_state *st)
{
    if (++st->cur_col >= st->cols) {
        st->cur_col = 0;
        if (++st->cur_row >= st->rows) {
            scroll_up(st, 1, 0, 0, st->rows - 1, st->cols - 1, 0x07);
            st->cur_row = st->rows - 1;
        }
    }
}

static void teletype(video_state *st, uint8_t ch)
{
    switch (ch) {
    case 0x0D: st->cur_col = 0; break;
    case 0x0A:
        if (++st->cur_row >= st->rows) {
            scroll_up(st, 1, 0, 0, st->rows - 1, st->cols - 1, 0x07);
            st->cur_row = st->rows - 1;
        }
        break;
    case 0x08: if (st->cur_col) st->cur_col--; break;
    case 0x07: break;
    default:
        cell(st, st->cur_row, st->cur_col)[0] = ch;
        advance(st);
    }
}

/* --- VESA VBE 2.0 (banked, packed-256) ----------------------------------- */
/* supported modes: {VBE number, width, height} (all 8bpp packed) */
/* ── ★★ THE MODE LIST IS THE INTERFACE, AND A GUEST FILTERS ON IT. (s74) ──────────
     This was three 8bpp modes. heaven7 calls 4F00, walks the list we publish, asks
     4F01 about every entry -- we answered OK for all three -- and then printed its
     own "VESA error" WITHOUT EVER CALLING 4F02. Measured, in that order. A guest that
     wants direct colour will not settle for 640x400x8 however cheerfully we describe
     it, so the fix is the list and the ModeInfoBlock behind it, not the answer code.
     Numbers are the VBE standard assignments; 0x11x direct-colour modes are what
     anything from the late 90s actually asks for. Every entry here must fit in
     VID_VESA_VRAM -- see the note there. */
static const struct { uint16_t num, w, h; uint8_t bpp; } vesa_modes[] = {
    /* packed-pixel 256-colour */
    { 0x100, 640, 400,  8 }, { 0x101, 640, 480,  8 },
    { 0x103, 800, 600,  8 }, { 0x105, 1024, 768, 8 },
    /* 1024x768 arrived with NTVDD_FRAME_MAXW/H (s74b): the presenter's snapshot and
       this list are now sized from ONE number, so a mode cannot be offered that the
       presenter would drop. 0x105 is the 1024x768x8 every VESA game's setup lists. */
    /* ── 320x240, THE MODE HEAVEN7 ASKS FOR BY DEFAULT. (s74b) ───────────────────
         Not a VBE-numbered mode; it is an OEM mode, and the numbers below are the S3
         Trio's (0x151 8bpp, 0x160 15bpp, 0x170 16bpp), which is the set DOSBox and
         every game of the period expects. heaven7's render table is (320,176)
         (512,280) (640,352) (800,440) -- one letterboxed picture per screen height,
         and 320x176 belongs to 320x240, its default. Without this mode it fell back
         to 640x480 and drew its 320x176 picture in the bottom quarter of the screen,
         which read as "the geometry is wrong" for a whole session. A guest picks by
         XResolution/YResolution from 4F01, so the number itself is not load-bearing. */
    { 0x151, 320, 240,  8 }, { 0x160, 320, 240, 15 }, { 0x170, 320, 240, 16 },
    /* direct colour: 15/16/24bpp at the resolutions VRAM can back */
    { 0x10D, 320, 200, 15 }, { 0x10E, 320, 200, 16 }, { 0x10F, 320, 200, 24 },
    { 0x110, 640, 480, 15 }, { 0x111, 640, 480, 16 }, { 0x112, 640, 480, 24 },
    { 0x113, 800, 600, 15 }, { 0x114, 800, 600, 16 }, { 0x115, 800, 600, 24 },
    { 0x116, 1024, 768, 15 }, { 0x117, 1024, 768, 16 }, { 0x118, 1024, 768, 24 },
    { 0x107, 1280, 1024, 8 },
    { 0x119, 1280, 1024, 15 }, { 0x11A, 1280, 1024, 16 }, { 0x11B, 1280, 1024, 24 },
};
/* ── VESA TEXT MODES (VBE 1.2 §, mode numbers 108h..10Ch). (s74b) ─────────────────
     132-column text is what editors and file managers of the period ask for. A VESA
     text mode is an ordinary text mode with a different geometry: it lives at B800,
     goes through VID_KIND_TEXT and every INT 10h text service unchanged, and the
     renderer already sizes the frame from cols x 8 and rows x cell_h. Only the
     bookkeeping is new: 4F01 answers in characters, 4F02 lands in text kind, 4F03
     remembers the VESA number (a standard mode set forgets it). */
static const struct { uint16_t num; uint8_t cols, rows, cell_h; } vesa_text_modes[] = {
    { 0x108,  80, 60,  8 }, { 0x109, 132, 25, 16 }, { 0x10A, 132, 43,  8 },
    { 0x10B, 132, 50,  8 }, { 0x10C, 132, 60,  8 },
};
static int vesa_find_text(uint16_t num, uint8_t *cols, uint8_t *rows, uint8_t *cell_h)
{
    unsigned i;
    for (i = 0; i < sizeof(vesa_text_modes)/sizeof(vesa_text_modes[0]); ++i)
        if (vesa_text_modes[i].num == (num & 0x3FFF)) {
            *cols = vesa_text_modes[i].cols; *rows = vesa_text_modes[i].rows;
            *cell_h = vesa_text_modes[i].cell_h; return 1;
        }
    return 0;
}
/* bytes per pixel as VBE counts them: 15bpp occupies 2 bytes, like 16. */
static uint32_t vesa_bypp(uint8_t bpp) { return bpp <= 8 ? 1u : bpp <= 16 ? 2u : bpp <= 24 ? 3u : 4u; }
/* Byte offset into vesa_vram of the pixel shown top-left: the 4F07 display start at
   the 4F06 logical pitch. (0,0) at the mode's own pitch until a guest moves it. */
static uint32_t vesa_origin(const video_state *st)
{
    return (uint32_t)st->vesa_start_y * st->vesa_stride
         + (uint32_t)st->vesa_start_x * vesa_bypp(st->vesa_bpp);
}
/* Record a VESA mode query and its answer -- see vesa_q[] in vdd_video.h. */
static void vesa_note(video_state *st, uint8_t fn, uint16_t mode, int ok)
{
    unsigned k;
    for (k = 0; k < st->vesa_qn; ++k)                  /* collapse repeats */
        if (st->vesa_q[k] == mode && st->vesa_q_fn[k] == fn) return;
    if (st->vesa_qn >= sizeof(st->vesa_q)/sizeof(st->vesa_q[0])) return;
    k = st->vesa_qn++;
    st->vesa_q[k] = mode; st->vesa_q_ok[k] = (uint8_t)(ok ? 1 : 0); st->vesa_q_fn[k] = fn;
}

static int vesa_find(uint16_t num, uint16_t *w, uint16_t *h, uint8_t *bpp)
{
    unsigned i;
    for (i = 0; i < sizeof(vesa_modes)/sizeof(vesa_modes[0]); ++i)
        if (vesa_modes[i].num == (num & 0x3FFF)) {
            uint32_t need = (uint32_t)vesa_modes[i].w * vesa_modes[i].h
                          * vesa_bypp(vesa_modes[i].bpp);
            /* ⚠ A MODE WE CANNOT STORE IS NOT A MODE WE SUPPORT. Answering 4F01 for
                 geometry that does not fit VID_VESA_VRAM invites a 4F02 we would have
                 to fail, or worse, blits off the end of the buffer. Checked here so
                 the list and the answer can never disagree. */
            if (need > VID_VESA_VRAM) return 0;
            *w = vesa_modes[i].w; *h = vesa_modes[i].h;
            if (bpp) *bpp = vesa_modes[i].bpp;
            return 1;
        }
    return 0;
}
static void wr16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void wr32(uint8_t *p, uint32_t v) { p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); p[2]=(uint8_t)(v>>16); p[3]=(uint8_t)(v>>24); }

/* sync the live A0000 window into vesa_vram[current bank]. */
static void vesa_sync(video_state *st)
{
    uint32_t off = (uint32_t)st->vesa_bank * VID_VESA_WIN; unsigned i;
    /* ⚠ AN LFB GUEST NEVER WRITES A0000, so copying that window into vram would
         paint a stale (usually blank) 64KB hole over the frame it just drew. */
    if (st->vesa_lfb) return;
    if (off + VID_VESA_WIN > VID_VESA_VRAM) return;
    for (i = 0; i < VID_VESA_WIN; ++i) st->vesa_vram[off + i] = st->vmem[i];
}

/* ── DIRECT COLOUR -> ARGB, ONCE PER FRAME. (s74) ─────────────────────────────────
     The frame contract has a bpp field, but every consumer of it indexed a palette --
     so a 15/16/24bpp mode has to be converted somewhere, and this is the only place
     that knows the guest's pixel format. Channels are expanded by REPLICATING the
     high bits into the low ones (5 bits -> 8 as (v<<3)|(v>>2)), not by shifting and
     leaving zeros: a white pixel must come out 0xFF, and 0x1F<<3 is 0xF8, which is a
     visibly grey white and the classic giveaway of a lazy 5-to-8 expansion. */
/* Bounding offsets of everything non-zero in the framebuffer. Answers, without any
   assumption about stride or origin, WHERE the guest put its pixels. */
static void vesa_scan_written(video_state *st)
{
    uint32_t i, lo = 0xFFFFFFFFu, hi = 0, nz = 0;
    uint32_t end = vesa_origin(st) + st->vesa_stride * st->vesa_h;
    if (!end || end > VID_VESA_VRAM) end = VID_VESA_VRAM;
    for (i = 0; i < end; ++i)
        if (st->vesa_vram[i]) { if (lo == 0xFFFFFFFFu) lo = i; hi = i; ++nz; }
    st->vram_lo = (lo == 0xFFFFFFFFu) ? 0 : lo;
    st->vram_hi = hi; st->vram_nz = nz;
}

static void vesa_to_argb(video_state *st)
{
    uint32_t y, x, w = st->vesa_w, h = st->vesa_h, pitch = st->vesa_stride;
    uint32_t bypp = vesa_bypp(st->vesa_bpp), org = vesa_origin(st);
    if (!w || !h || !pitch) return;
    if (w > VID_VESA_MAXW || h > VID_VESA_MAXH) return;   /* cannot happen: vesa_find caps it */
    for (y = 0; y < h; ++y) {
        const uint8_t *src = st->vesa_vram + org + y * pitch;  /* from the 4F07 start */
        uint32_t *dst = st->vesa_argb + y * w;
        if (org + y * pitch + w * bypp > VID_VESA_VRAM) break;
        for (x = 0; x < w; ++x) {
            uint32_t r, g, b;
            if (st->vesa_bpp == 15) {
                uint32_t v = (uint32_t)src[x*2] | ((uint32_t)src[x*2+1] << 8);
                r = (v >> 10) & 0x1F; g = (v >> 5) & 0x1F; b = v & 0x1F;
                r = (r << 3) | (r >> 2); g = (g << 3) | (g >> 2); b = (b << 3) | (b >> 2);
            } else if (st->vesa_bpp == 16) {
                uint32_t v = (uint32_t)src[x*2] | ((uint32_t)src[x*2+1] << 8);
                r = (v >> 11) & 0x1F; g = (v >> 5) & 0x3F; b = v & 0x1F;
                r = (r << 3) | (r >> 2); g = (g << 2) | (g >> 4); b = (b << 3) | (b >> 2);
            } else {                                   /* 24bpp, B G R in memory */
                b = src[x*3]; g = src[x*3+1]; r = src[x*3+2];
            }
            dst[x] = 0xFF000000u | (r << 16) | (g << 8) | b;
        }
    }
}

/* ── SAVE / RESTORE STATE: one block for INT 10h AH=1Ch and VBE 4F04 (§4.7). (s74b)
     AH=1Ch used to report 3 blocks (192 bytes) and then write 768 bytes of DAC into the
     caller's buffer -- the Heretic MCB overrun again, in a different function -- and
     4F04 did not exist. A guest treats the buffer as opaque, so the layout is ours:
        +0   'NTVS'  +4 mask  +6 version
        +8   mode, in_vesa, vesa_mode(2), vesa_bpp, vesa_lfb, dacwidth, bank(2),
             stride(4) @+20, start_x(2) @+24, start_y(2) @+26
        +32  DAC, 256 x (R,G,B) at 8 bits each -- the 8-bit width would lose precision
             at 6 bits, and AH=1Ch's own 6-bit format is produced from this on the way out
        +800 the attribute controller (vpal[17], AR10, AR14, overscan), text cursor
             (row, col, shape), page, CRTC start/offset
     896 bytes = 14 blocks, reported for every mask; the spec permits over-reporting
     and a guest allocates from the answer, which is the one thing that must hold.
     Restore re-enters the saved mode with the DON'T-CLEAR bit -- frame buffer memory
     is explicitly not part of the state -- then puts the registers back over it. */
#define VID_STATE_BYTES  896u
#define VID_STATE_BLOCKS (VID_STATE_BYTES / 64u)
static void int10(void *self, ntvdd_regs *r);
static void vesa(video_state *st, ntvdd_regs *r);
static void vid_state_save(video_state *st, uint8_t *b, uint16_t mask)
{
    unsigned i;
    for (i = 0; i < VID_STATE_BYTES; ++i) b[i] = 0;
    b[0] = 'N'; b[1] = 'T'; b[2] = 'V'; b[3] = 'S'; wr16(b + 4, mask); wr16(b + 6, 1);
    b[8] = st->mode; b[9] = st->in_vesa; wr16(b + 10, st->vesa_mode);
    b[12] = st->vesa_bpp; b[13] = st->vesa_lfb; b[14] = st->vesa_dacwidth;
    wr16(b + 16, st->vesa_bank); wr32(b + 20, st->vesa_stride);
    wr16(b + 24, st->vesa_start_x); wr16(b + 26, st->vesa_start_y);
    for (i = 0; i < 256; ++i) {
        b[32 + i*3]     = (uint8_t)(st->dac[i] >> 16);
        b[32 + i*3 + 1] = (uint8_t)(st->dac[i] >> 8);
        b[32 + i*3 + 2] = (uint8_t)(st->dac[i]);
    }
    for (i = 0; i < 17; ++i) b[800 + i] = st->vpal[i];
    b[817] = st->attr_mode; b[818] = st->attr_cse; b[819] = st->overscan;
    b[820] = st->cur_row; b[821] = st->cur_col; wr16(b + 822, st->cur_shape);
    b[824] = st->page; wr16(b + 826, st->crtc_start); b[828] = st->crtc_offset;
}
/* 1 = restored, 0 = not a buffer we wrote (the caller answers AH=02). */
static int vid_state_load(video_state *st, const uint8_t *b)
{
    unsigned i; ntvdd_regs m;
    if (b[0] != 'N' || b[1] != 'T' || b[2] != 'V' || b[3] != 'S') return 0;
    for (i = 0; i < sizeof m; ++i) ((uint8_t *)&m)[i] = 0;
    if (b[9]) {                                   /* back into the VESA mode, no clear */
        uint16_t bx = (uint16_t)(0x8000u | (b[13] ? 0x4000u : 0u) | (b[10] | (b[11] << 8)));
        s_ah(&m, 0x4F); s_al(&m, 0x02); s_bx(&m, bx); vesa(st, &m);
        st->vesa_dacwidth = b[14]; st->vesa_bank = (uint16_t)(b[16] | (b[17] << 8));
        st->vesa_stride   = (uint32_t)b[20] | ((uint32_t)b[21] << 8) | ((uint32_t)b[22] << 16) | ((uint32_t)b[23] << 24);
        st->vesa_start_x  = (uint16_t)(b[24] | (b[25] << 8));
        st->vesa_start_y  = (uint16_t)(b[26] | (b[27] << 8));
    } else {                                      /* a standard mode, bit 7 = no clear */
        s_ah(&m, 0x00); s_al(&m, (uint8_t)(b[8] | 0x80)); int10(st, &m);
    }
    for (i = 0; i < 256; ++i)
        st->dac[i] = 0xFF000000u | ((uint32_t)b[32 + i*3] << 16)
                   | ((uint32_t)b[32 + i*3 + 1] << 8) | (uint32_t)b[32 + i*3 + 2];
    for (i = 0; i < 17; ++i) st->vpal[i] = b[800 + i];
    st->attr_mode = b[817]; st->attr_cse = b[818]; st->overscan = b[819];
    st->cur_row = b[820]; st->cur_col = b[821]; st->cur_shape = (uint16_t)(b[822] | (b[823] << 8));
    st->page = b[824]; st->crtc_start = (uint16_t)(b[826] | (b[827] << 8)); st->crtc_offset = b[828];
    pal_refresh(st); st->dirty = 1;
    return 1;
}

/* INT 10h AX=4Fxx. Always returns AX=0x004F (supported+ok) for what we handle. */
static void vesa(video_state *st, ntvdd_regs *r)
{
    uint8_t al = r_al(r); unsigned i;
    if (al < 0x16) {                              /* inventory: see vesa_calls[]   */
        uint8_t bl = (uint8_t)(r_bx(r) & 0xFF);
        st->vesa_calls[al]++;
        st->vesa_bl[al] |= (uint16_t)(bl == 0x80 ? 0x8000u : bl < 15 ? (1u << bl) : 0u);
    }
    switch (al) {
    case 0x00: {                                  /* return controller info       */
        /* ⚠ THE CALLER'S BLOCK IS 256 BYTES UNLESS IT PRESET "VBE2". (s74) VBE 2.0
             §4.3: a VbeInfoBlock is 256 bytes for a VBE 1.x caller and 512 only when
             the caller wrote "VBE2" into the signature first; the OEM string and mode
             list go in the reserved area at +34 (that is what it is for). We used to
             put the OEM string at +0x100 and the mode list at +0x120 REGARDLESS.
             Heretic allocates exactly 16 paragraphs for this call, so +0x100 was the
             MCB of the next block: its signature became 'N' (of "NTVDMEX VESA"), the
             chain walk stopped there, ~480 KB above went invisible, and its next DOS
             allocation died with "I_AllocLow: DOS alloc of 1024 failed, 256 free".
             Doom never probes VESA, which is why only Heretic paid. */
        uint8_t *b = (uint8_t *)vdd_map_flat(st->bus, r->es, (uint16_t)(uint16_t)r->edi);
        int vbe2 = (b[0]=='V' && b[1]=='B' && b[2]=='E' && b[3]=='2');
        const unsigned OEM = 0x22, MODES = 0x40;  /* both inside the reserved area */
        for (i = 0; i < (vbe2 ? 512u : 256u); ++i) b[i] = 0;
        b[0]='V'; b[1]='E'; b[2]='S'; b[3]='A';
        wr16(b + 4, 0x0200);                      /* VBE 2.0                      */
        wr32(b + 6, ((uint32_t)r->es << 16) | (((uint16_t)r->edi + OEM) & 0xFFFF));    /* OEM string */
        wr32(b + 10, 0);                          /* capabilities                 */
        wr32(b + 14, ((uint32_t)r->es << 16) | (((uint16_t)r->edi + MODES) & 0xFFFF));  /* mode list  */
        wr16(b + 18, VID_VESA_VRAM / 0x10000);    /* total memory in 64KB units   */
        if (vbe2) {                               /* VBE 2.0 fields, only for a 2.0 caller */
            wr16(b + 20, 0x0100);                 /* OEM software rev             */
            wr32(b + 22, ((uint32_t)r->es << 16) | (((uint16_t)r->edi + OEM) & 0xFFFF)); /* vendor  */
            wr32(b + 26, ((uint32_t)r->es << 16) | (((uint16_t)r->edi + OEM) & 0xFFFF)); /* product */
            wr32(b + 30, ((uint32_t)r->es << 16) | (((uint16_t)r->edi + OEM) & 0xFFFF)); /* rev     */
        }
        { const char *o = "NTVDMEX VESA"; for (i = 0; o[i]; ++i) b[OEM + i] = (uint8_t)o[i]; b[OEM+i]=0; }
        for (i = 0; i < sizeof(vesa_modes)/sizeof(vesa_modes[0]); ++i)
            wr16(b + MODES + i*2, vesa_modes[i].num);
        { unsigned t;
          for (t = 0; t < sizeof(vesa_text_modes)/sizeof(vesa_text_modes[0]); ++t, ++i)
              wr16(b + MODES + i*2, vesa_text_modes[t].num); }
        wr16(b + MODES + i*2, 0xFFFF);            /* mode-list terminator         */
        s_ax(r, 0x004F);
        break; }
    case 0x01: {                                  /* return mode info             */
        uint16_t w, h; uint8_t mbpp = 8;
        { uint8_t tc, tr, th;
          if (vesa_find_text(r_cx(r), &tc, &tr, &th)) {   /* a TEXT mode: answer in characters */
              uint8_t *b = (uint8_t *)vdd_map_flat(st->bus, r->es, (uint16_t)r->edi);
              unsigned page = (unsigned)tc * tr * 2u;
              vesa_note(st, 0x01, r_cx(r), 1);
              for (i = 0; i < 256; ++i) b[i] = 0;
              wr16(b + 0, 0x000F);              /* supported|opt info|BIOS output|colour; bit 4 clear = TEXT */
              b[2] = 0x07; b[3] = 0x00;         /* WinA r/w/exists; WinB none    */
              wr16(b + 4, 32); wr16(b + 6, 32); /* the 32 KB colour-text window  */
              wr16(b + 8, 0xB800); wr16(b + 10, 0);
              wr32(b + 12, 0);
              wr16(b + 16, (uint16_t)(tc * 2u)); /* bytes per character row       */
              wr16(b + 18, tc); wr16(b + 20, tr); /* X/Y resolution IN CHARACTERS  */
              b[22] = 8; b[23] = th;            /* char cell                     */
              b[24] = 1;                        /* planes                        */
              b[25] = 4;                        /* bits per pixel (attribute)    */
              b[26] = 1;                        /* NumberOfBanks                 */
              b[27] = 0;                        /* MemoryModel 0 = text          */
              b[28] = 0;                        /* BankSize                      */
              b[29] = (uint8_t)(page ? (0x8000u / page) - 1u : 0u);   /* image pages */
              b[30] = 1;                        /* Reserved = 1                  */
              s_ax(r, 0x004F);
              break;
          } }
        vesa_note(st, 0x01, r_cx(r), vesa_find(r_cx(r), &w, &h, &mbpp));
        if (vesa_find(r_cx(r), &w, &h, &mbpp)) {
            uint8_t *b = (uint8_t *)vdd_map_flat(st->bus, r->es, (uint16_t)(uint16_t)r->edi);
            uint32_t bypp = vesa_bypp(mbpp), pitch = (uint32_t)w * bypp;
            for (i = 0; i < 256; ++i) b[i] = 0;
            wr16(b + 0, 0x009B);                  /* attrs: supported|color|graphics */
            b[2] = 0x07; b[3] = 0x00;             /* WinA r/w/exists; WinB none    */
            wr16(b + 4, 64); wr16(b + 6, 64);     /* granularity / size (KB)       */
            wr16(b + 8, 0xA000); wr16(b + 10, 0); /* WinA seg / WinB seg           */
            wr32(b + 12, 0);                      /* WinFuncPtr (use INT 10h 4F05) */
            wr16(b + 16, (uint16_t)pitch);        /* bytes per scan line           */
            wr16(b + 18, w); wr16(b + 20, h);     /* X / Y resolution              */
            b[22] = 8; b[23] = 16;                /* char cell                     */
            b[24] = 1; b[25] = mbpp;              /* planes / bits per pixel       */
            /* ⚠ MEMORY MODEL IS NOT A CONSTANT. It was 4 ("packed pixel", i.e. a
                 palette index) for every mode, which is a lie for direct colour --
                 a guest reads this byte to decide whether the bytes it writes are
                 indices or channels. VBE 2.0 §4.4: 04h packed pixel, 06h direct colour. */
            b[27] = (uint8_t)(mbpp > 8 ? 6 : 4);
            /* ⚠⚠ NumberOfBanks IS **NOT** "how many 64KB windows the mode needs".
                 I wrote that from memory and it is the wrong CONCEPT, not just a wrong
                 number -- it computed 22 for 640x480x24. VBE 2.0 §4.4: banks are the
                 groups the SCAN LINES are divided into (the CGA/Hercules interleave),
                 and "for modes that don't have scanline banks (such as VGA modes
                 0Dh-13h), this field should be set to 1". BankSize likewise 0.
                 Caught by reading the spec, not by any guest -- heaven7 never looks. */
            b[26] = 1;                            /* +26 NumberOfBanks: no scanline banks */
            /* ⚠⚠⚠ AND THE OFFSETS FROM HERE WERE OFF BY ONE, ALSO FROM MEMORY. The
                 VBE 2.0 ModeInfoBlock runs +26 NumberOfBanks, +27 MemoryModel,
                 +28 BankSize, +29 NumberOfImagePages, +30 Reserved(=1). I had written
                 NumberOfImagePages at +28 and "reserved must be 1" at +29 -- so the
                 image-page count was landing in BankSize, a 1 was landing in
                 NumberOfImagePages (which is a count MINUS ONE, i.e. claiming two
                 pages of a mode we have one page of VRAM for), and Reserved at +30 was
                 left 0 when the spec says it is always 1 in this version.
                 Three fields wrong, none of which any guest we have would have caught. */
            b[28] = 0;                            /* +28 BankSize: no scanline banks   */
            b[29] = 0;                            /* +29 NumberOfImagePages = total-1  */
            b[30] = 1;                            /* +30 Reserved: always 1 in VBE 2.0 */
            /* ── DIRECT-COLOUR FIELD LAYOUT (offsets 31..38). A guest cannot pack a
                 pixel without these, and it will not trust a mode that leaves them
                 zero. 15bpp is 5:5:5 with one byte unused, 16bpp is 5:6:5, 24bpp is
                 8:8:8 -- all little-endian, blue in the low bits, which is what every
                 PC VBE implementation does. */
            if (mbpp == 15)      { b[31]=5; b[32]=10; b[33]=5; b[34]=5; b[35]=5; b[36]=0;
                                   b[37]=1; b[38]=15; }
            else if (mbpp == 16) { b[31]=5; b[32]=11; b[33]=6; b[34]=5; b[35]=5; b[36]=0;
                                   b[37]=0; b[38]=0; }
            else if (mbpp == 24) { b[31]=8; b[32]=16; b[33]=8; b[34]=8;  b[35]=8; b[36]=0;
                                   b[37]=0; b[38]=0; }
            b[39] = 0;                            /* DirectColorModeInfo: no prog ramp */
            /* ── ★★★ LINEAR FRAMEBUFFER. (s74) Attribute bit 7 says a mode HAS one and
                 PhysBasePtr says where; with them 0 the whole mode list reads as
                 "banked only". heaven7 enumerated all twelve modes we published,
                 including 320x200x16 and 640x480x16, and refused every one without
                 ever calling 4F02 -- measured twice, before and after the list grew.
                 A 2000-era demo will not paginate a 64KB window to raytrace. */
            /* ⚠ 0x9B ALREADY CARRIES D7 (LFB available) -- D0|D1|D3|D4|D7. An earlier
                 note here claimed bit 7 was 0 and OR'd 0x80 in; that was a no-op and
                 the claim was wrong. What was actually missing was PhysBasePtr, which
                 D7 is worthless without. VBE 2.0 §4.4 D7/D6 table: D7=1,D6=0 means
                 "both windowed and linear", which is exactly what we now provide. */
            wr32(b + 40, VID_VESA_LFB_PHYS);            /* PhysBasePtr              */
            /* VBE 2.0 adds the linear-mode geometry at +50; a guest that drives the
               LFB reads these rather than the banked ones. Same numbers here because
               our pitch does not change between the two. */
            wr16(b + 50, (uint16_t)pitch);              /* LinBytesPerScanLine      */
            b[52] = 0; b[53] = 0;                       /* Lin/Bnk NumberOfImagePages */
            if (mbpp > 8) { b[54]=b[31]; b[55]=b[32]; b[56]=b[33]; b[57]=b[34];
                            b[58]=b[35]; b[59]=b[36]; b[60]=b[37]; b[61]=b[38]; }
            s_ax(r, 0x004F);
        } else s_ax(r, 0x014F);
        break; }
    case 0x02: {                                  /* set VBE mode                 */
        uint16_t w, h; uint8_t mbpp = 8;
        { uint8_t tc, tr, th;
          if (vesa_find_text(r_bx(r), &tc, &tr, &th)) {
              /* A VESA text mode = mode 3 with a different geometry. Go through the
                 standard mode set so everything a text mode resets is reset (font,
                 palette, CRTC, cursor, blink), honouring D15 as AL bit 7, then apply
                 the geometry. vesa_text_mode is what 4F03 reports until a standard
                 mode set clears it (that path zeroes it below). */
              ntvdd_regs m; unsigned k;
              for (k = 0; k < sizeof m; ++k) ((uint8_t *)&m)[k] = 0;
              vesa_note(st, 0x02, r_bx(r), 1);
              st->vesa_set_bx = r_bx(r); st->vesa_set_seen = 1; st->vesa_set_ok = 1;
              s_ah(&m, 0x00); s_al(&m, (uint8_t)(0x03 | ((r_bx(r) & 0x8000) ? 0x80 : 0x00)));
              int10(st, &m);
              st->cols = tc; st->rows = tr; st->cell_h = th;
              st->gw = (uint16_t)(tc * VID_CELL_W); st->gh = (uint16_t)(tr * th);
              if (!(r_bx(r) & 0x8000)) clear_text(st, 0x07);
              st->vesa_text_mode = (uint16_t)(r_bx(r) & 0x3FFF);
              st->dirty = 1; s_ax(r, 0x004F);
              break;
          } }
        vesa_note(st, 0x02, r_bx(r), vesa_find(r_bx(r), &w, &h, &mbpp));
        st->vesa_set_bx = r_bx(r); st->vesa_set_seen = 1;
        st->vesa_set_ok = (uint8_t)(vesa_find(r_bx(r), &w, &h, &mbpp) ? 1 : 0);
        if (vesa_find(r_bx(r), &w, &h, &mbpp)) {
            uint32_t n;
            st->in_vesa = 1; st->vesa_mode = r_bx(r) & 0x3FFF; st->vesa_w = w; st->vesa_h = h;
            /* Bit 14 of the mode = "use the linear framebuffer". It matters beyond
               bookkeeping: an LFB guest never touches A0000, so vesa_sync must stop
               copying that window over the picture (see vesa_sync). */
            st->vesa_lfb = (uint8_t)((r_bx(r) & 0x4000) ? 1 : 0);
            st->vesa_bpp = mbpp;
            st->vesa_stride = (uint32_t)w * vesa_bypp(mbpp);
            st->vesa_start_x = st->vesa_start_y = 0;   /* a mode set shows page 1 */
            st->vesa_dacwidth = 6;                     /* §4.11: any mode set -> 6 bits */
            st->vesa_bank = 0;
            /* ── ★★★ D15 = "DON'T CLEAR DISPLAY MEMORY". (VBE 2.0 §4.5) ──────────────
                 We cleared unconditionally. This is the SAME defect as the standard
                 BIOS one already on the books -- "AL bit 7 on a mode set = DO NOT
                 CLEAR VRAM" -- just on the VESA function instead, and it was found the
                 same way it should have been the first time: by reading the spec.
                 A guest that sets a mode to change geometry while keeping its picture
                 gets a black screen from us otherwise. */
            if (!(r_bx(r) & 0x8000)) {
                for (n = 0; n < VID_VESA_VRAM; ++n) st->vesa_vram[n] = 0;
                for (n = 0; n < VID_VESA_WIN; ++n) st->vmem[n] = 0;
            }
            pal_refresh(st);                           /* identity DAC path, see pal_refresh */
            st->dirty = 1; s_ax(r, 0x004F);
        } else s_ax(r, 0x014F);
        break; }
    case 0x04: {                                  /* save/restore state (§4.7)     */
        uint8_t dl = (uint8_t)(r_dx(r) & 0xFF);
        if (dl == 0x00) { s_bx(r, VID_STATE_BLOCKS); s_ax(r, 0x004F); break; }
        if (dl == 0x01 || dl == 0x02) {
            uint8_t *sb = (uint8_t *)vdd_map_flat(st->bus, r->es, (uint16_t)r->ebx);
            if (dl == 0x01) { vid_state_save(st, sb, r_cx(r)); s_ax(r, 0x004F); }
            else s_ax(r, vid_state_load(st, sb) ? 0x004F : 0x024F);
            break;
        }
        s_ax(r, 0x014F);
        break; }
    case 0x03:                                    /* get the current VBE mode      */
        s_bx(r, (uint16_t)(st->in_vesa ? st->vesa_mode : st->vesa_text_mode ? st->vesa_text_mode : st->mode));
        s_ax(r, 0x004F);
        break;
    /* ── 4F06 / 4F07: THE LOGICAL SCREEN, AND WHICH PART OF IT IS SHOWN. (s74b) ──
         VBE 2.0 §4.9/§4.10. Both of these used to be accepted and IGNORED: 4F06 kept a
         private `vesa_scanline` the presenter never read (and assumed bytes == pixels,
         true only at 8bpp), and 4F07 stored a start the presenter never applied. So a
         guest that draws page 2 and calls 4F07 to show it -- the standard VESA
         double-buffer -- got 004F back and saw page 1 forever. `vesa_stride` is the
         single truth for bytes-per-line and `vesa_origin()` for the displayed start;
         the presenter reads both.
       ► Failure codes are the spec's: AH=02 for a length or start that does not fit,
         AH=03 outside a VESA mode ("invalid in current video mode"). "Fail and make no
         changes" -- §4.10 -- so nothing is written until the request has been checked. */
    case 0x06: {                                  /* get/set logical scan length   */
        uint8_t  bl   = (uint8_t)(r_bx(r) & 0xFF);
        uint32_t bypp = vesa_bypp(st->vesa_bpp);
        uint32_t minb = (uint32_t)st->vesa_w * bypp;                 /* the mode's own pitch */
        uint32_t maxb = st->vesa_h ? VID_VESA_VRAM / st->vesa_h : 0; /* longest line that still holds h rows */
        maxb -= maxb % bypp;                                         /* whole pixels          */
        if (!st->in_vesa || !minb || !maxb) { s_ax(r, 0x034F); break; }
        if (bl == 0x00 || bl == 0x02) {
            uint32_t want = r_cx(r);
            if (bl == 0x00) want *= bypp;                            /* pixels -> bytes       */
            want = (want + bypp - 1) / bypp * bypp;                  /* "next larger value"   */
            if (want < minb || want > maxb) { s_ax(r, 0x024F); break; }
            st->vesa_stride = want;
            /* a start that no longer leaves a full page at the new pitch is reset,
               which is what a BIOS that re-latches its CRTC offset does in effect */
            if (vesa_origin(st) + (uint32_t)(st->vesa_h - 1) * want + minb > VID_VESA_VRAM)
                st->vesa_start_x = st->vesa_start_y = 0;
            st->dirty = 1;
        } else if (bl == 0x03) {                  /* get maximum                   */
            s_bx(r, (uint16_t)maxb); s_cx(r, (uint16_t)(maxb / bypp));
            s_dx(r, (uint16_t)(VID_VESA_VRAM / maxb)); s_ax(r, 0x004F);
            break;
        } else if (bl != 0x01) { s_ax(r, 0x014F); break; }
        s_bx(r, (uint16_t)st->vesa_stride);
        s_cx(r, (uint16_t)(st->vesa_stride / bypp));
        s_dx(r, (uint16_t)(VID_VESA_VRAM / st->vesa_stride));
        s_ax(r, 0x004F);
        break; }
    case 0x07: {                                  /* get/set display start         */
        uint8_t bl = (uint8_t)(r_bx(r) & 0xFF);
        if (!st->in_vesa) { s_ax(r, 0x034F); break; }
        if (bl == 0x01) {                         /* get                           */
            s_cx(r, st->vesa_start_x); s_dx(r, st->vesa_start_y); s_bx(r, 0);
            s_ax(r, 0x004F);
        } else if (bl == 0x00 || bl == 0x80) {    /* set (80h: during retrace)     */
            uint32_t bypp = vesa_bypp(st->vesa_bpp);
            uint32_t org  = (uint32_t)r_dx(r) * st->vesa_stride + (uint32_t)r_cx(r) * bypp;
            if (r_cx(r) > st->vesa_07_maxx) st->vesa_07_maxx = r_cx(r);   /* inventory */
            if (r_dx(r) > st->vesa_07_maxy) st->vesa_07_maxy = r_dx(r);
            /* the whole displayed page must exist: "if the requested Display Start
               coordinates do not allow for a full page of video memory ... fail" */
            if (org + (uint32_t)(st->vesa_h - 1) * st->vesa_stride
                    + (uint32_t)st->vesa_w * bypp > VID_VESA_VRAM) { st->vesa_07_rej++; s_ax(r, 0x024F); break; }
            st->vesa_start_x = r_cx(r); st->vesa_start_y = r_dx(r);
            st->dirty = 1;
            s_ax(r, 0x004F);
        } else s_ax(r, 0x014F);                   /* VBE 3.0 sub-functions: not here */
        break; }
    case 0x08: {                                  /* get/set DAC palette width     */
        /* VBE 2.0 §4.11: BL=00 set (BH = wanted bits), BL=01 get; BH out = current.
           "If the hardware cannot select the requested width, the NEXT LOWER value it
           can is selected" -- we have 6 and 8, so 10 -> 8 and 7 -> 6 (this used to send
           anything but exactly 8 to 6). AH=03 in a direct-colour mode; the width is
           reset to 6 by any mode set (done in 4F02 and the standard AH=00). */
        uint8_t bl8 = (uint8_t)(r_bx(r) & 0xFF);
        if (!st->in_vesa || st->vesa_bpp > 8) { s_ax(r, 0x034F); break; }
        if (bl8 == 0x00) {
            uint8_t w8 = (uint8_t)((r_bx(r) >> 8) & 0xFF);
            st->vesa_dacwidth = (uint8_t)(w8 >= 8 ? 8 : 6);
        } else if (bl8 != 0x01) { s_ax(r, 0x014F); break; }
        if (!st->vesa_dacwidth) st->vesa_dacwidth = 6;
        s_bx(r, (uint16_t)((r_bx(r) & 0x00FF) | ((uint16_t)st->vesa_dacwidth << 8)));
        s_ax(r, 0x004F);
        break; }
    case 0x09: {                                  /* get/set palette data          */
        /* VBE 2.0 §4.12: BL=00 set, 01 get, 02/03 secondary palette (none here ->
           AH=02), 80h set during retrace; DX=first, CX=count, ES:DI = quads laid out
           B,G,R,align in memory. The DAC width set by 4F08 decides how far to shift.
           ⚠ (s74b) A set never called pal_refresh(), so the presenter kept showing the
             OLD palette until some other DAC path happened to refresh it. */
        uint8_t bl9 = (uint8_t)(r_bx(r) & 0xFF);
        uint16_t first = r_dx(r), n = r_cx(r), i;
        uint8_t *t = (uint8_t *)vdd_map_flat(st->bus, r->es, (uint16_t)r->edi);
        uint8_t sh = (uint8_t)(st->vesa_dacwidth == 8 ? 0 : 2);
        if (bl9 == 0x02 || bl9 == 0x03) { s_ax(r, 0x024F); break; }
        if (bl9 != 0x00 && bl9 != 0x01 && bl9 != 0x80) { s_ax(r, 0x014F); break; }
        if ((uint32_t)first + n > 256) { s_ax(r, 0x024F); break; }   /* fail, change nothing */
        for (i = 0; i < n && (first + i) < 256; ++i) {
            if (bl9 == 0x00 || bl9 == 0x80)
                st->dac[first + i] = 0xFF000000u
                    | ((uint32_t)(t[i*4+2] << sh) << 16)
                    | ((uint32_t)(t[i*4+1] << sh) << 8)
                    |  (uint32_t)(t[i*4+0] << sh);
            else {
                uint32_t v = st->dac[first + i];
                t[i*4+0] = (uint8_t)((v & 0xFF) >> sh);
                t[i*4+1] = (uint8_t)(((v >> 8) & 0xFF) >> sh);
                t[i*4+2] = (uint8_t)(((v >> 16) & 0xFF) >> sh);
                t[i*4+3] = 0;
            }
        }
        if (bl9 != 0x01) pal_refresh(st);         /* the presenter reads st->pal */
        st->dirty = 1;
        s_ax(r, 0x004F);
        break; }
    case 0x0A:                                    /* protected-mode interface      */
        /* There is no PM bank-switching stub to hand out. Report NOT SUPPORTED
           (AH=01) rather than returning 4F00 with a null pointer, which a client
           would call straight into. */
        s_ax(r, 0x014F);
        VID_UNIMPL_SET(st->unimpl_fn, 0x4F);
        break;
    case 0x10: {                                  /* VBE/PM: display power (DPMS)  */
        /* VBE/PM 1.0. BL=00 report: BL=version 10h (BCD), BH=states supported
           (bit0 standby, bit1 suspend, bit2 off, bit3 reduced-on); BL=01 set state
           BH; BL=02 get state -> BH. ⚠ From the published interface (what Bochs and
           DOSBox answer), not from a VESA PDF in docs/ref -- there is no oracle for
           it. We claim all four states and remember the one set; nothing blanks,
           which is what a monitor with no power management would show too. */
        uint8_t bl = (uint8_t)(r_bx(r) & 0xFF), bh = (uint8_t)((r_bx(r) >> 8) & 0xFF);
        if (bl == 0x00)      s_bx(r, (uint16_t)((0x0Fu << 8) | 0x10));
        else if (bl == 0x01) { if (bh & ~0x0Fu) { s_ax(r, 0x024F); break; } st->vesa_pm_state = bh; }
        else if (bl == 0x02) s_bx(r, (uint16_t)(((uint16_t)st->vesa_pm_state << 8) | bl));
        else { s_ax(r, 0x014F); break; }
        s_ax(r, 0x004F);
        break; }
    case 0x15:                                    /* DDC / display identification  */
        s_ax(r, 0x014F);                          /* no monitor EDID to report     */
        VID_UNIMPL_SET(st->unimpl_fn, 0x4F);
        break;
    case 0x05: {                                  /* window control (bank switch) */
        /* VBE 2.0 §4.8: BH = 00h set / 01h get, BL = WINDOW (00h A, 01h B), DX = window
           position in granularity units (64 KB here, as 4F01 says).
           ⚠ (s74b) This read BL as the set/get selector. Window A's number is 0, so a
             SET of window A (BH=00,BL=00) happened to work -- and a GET of window A
             (BH=01,BL=00) was treated as a set to whatever DX held. A guest that reads
             the bank back to restore it later flipped its own window to junk. Caught
             by the spec audit, not by a guest; there is no oracle for VESA yet.
           Window B is advertised as absent (WinBAttributes=0) so BL=01 fails; in an LFB
           mode the spec requires AH=03. */
        uint8_t bh = (uint8_t)((r_bx(r) >> 8) & 0xFF), bl = (uint8_t)(r_bx(r) & 0xFF);
        if (!st->in_vesa || st->vesa_lfb) { s_ax(r, 0x034F); break; }
        if (bl != 0x00) { s_ax(r, 0x014F); break; }
        if (bh == 0x00) {                         /* set window A                  */
            uint32_t off = (uint32_t)r_dx(r) * VID_VESA_WIN; unsigned k;
            if (off + VID_VESA_WIN > VID_VESA_VRAM) { s_ax(r, 0x024F); break; }
            vesa_sync(st);                        /* flush current bank            */
            st->vesa_bank = r_dx(r);
            for (k = 0; k < VID_VESA_WIN; ++k) st->vmem[k] = st->vesa_vram[off + k];
            st->dirty = 1;
        } else if (bh == 0x01) {                  /* get window A                  */
            s_dx(r, st->vesa_bank);
        } else { s_ax(r, 0x014F); break; }
        s_ax(r, 0x004F);
        break; }
    default: s_ax(r, 0x014F); break;              /* unsupported sub-function      */
    }
}

/* Render an 8x16 glyph into the mode-12h bit-planes at text cell (col,row), so
   BIOS character output (INT 10h AH=09/0A/0E) shows up in graphics mode --
   QuickBASIC draws SCREEN 12 text exactly this way (LOCATE -> AH=02, then AH=09).
   fg/bg are 4-bit colour indices; each glyph row maps to one byte per plane. */
static void glyph_12h(video_state *st, int col, int row, uint8_t ch, uint8_t fg, uint8_t bg)
{
    const uint8_t *gl = vga_font_8x16[ch];
    int gy, p;
    if (col < 0 || row < 0 || col >= (VID_G12_W / 8) || (row * 16 + 15) >= VID_G12_H) return;
    for (gy = 0; gy < 16; ++gy) {
        uint8_t bits = gl[gy];
        uint32_t off = (uint32_t)(row * 16 + gy) * (VID_G12_W / 8) + (uint32_t)col;
        for (p = 0; p < 4; ++p) {
            uint8_t fgb = ((fg >> p) & 1) ? 0xFF : 0x00;
            uint8_t bgb = ((bg >> p) & 1) ? 0xFF : 0x00;
            st->plane[p][off] = (uint8_t)((bits & fgb) | ((uint8_t)~bits & bgb));
        }
    }
}

/* INT 10h text + mode + palette services. */
static void int10(void *self, ntvdd_regs *r)
{
    video_state *st = (video_state *)self;
    uint8_t ah = r_ah(r), al = r_al(r);
    st->dirty = 1;
    switch (ah) {
    case 0x00:                                        /* set video mode          */
        /* ── ▶ AL BIT 7 = "DO NOT CLEAR VIDEO MEMORY", AND IT IS NOT DECORATION. ────
             Standard since the EGA: a mode set with bit 7 reprograms the card but
             LEAVES DISPLAY MEMORY ALONE. We masked the bit off to get the mode number
             and then threw it away, so every mode set wiped all four 64KB planes.
           ⚠ THAT IS THE MISSING LEMMINGS TOOLBAR. The game composes its skill-button
             panel into OFF-SCREEN VRAM at 0xF91F..0xFFFE, then sets its mode with
             `mov ax,0x008D` (0x0FB3) -- and the menu with `mov ax,0x0090` (0x4BEF) --
             exactly so that cache survives. We cleared it, and the panel blit at
             0x7626 then copied 1760 bytes of zeroes onto the screen, faithfully.
             MEASURED: the blit writes with wm=1 and lat=00000000 while the cache's
             own last write left 0x00/0x24/0x7F/0x00 there -- the copy was never wrong,
             its source had been destroyed underneath it.
           ▶ Everything else a mode set does still happens; only the erase is skipped,
             which is the whole difference the bit describes. */
        {   int noclear = (al & 0x80) != 0;
        st->mode = al & 0x7F; st->in_vesa = 0;        /* a standard mode leaves VESA */
        st->vesa_text_mode = 0;                       /* ...including a VESA text mode */
        st->vesa_dacwidth = 6;                        /* §4.11: any mode set -> 6 bits */
        st->cur_row = st->cur_col = 0; st->page = 0;
        st->cur_shape = 0x0607;                       /* the BIOS resets the shape too */
        st->blink = 1;                                /* ...and re-enables blink (AR10 bit 3) */
        st->attr_mode = (uint8_t)(st->attr_mode | 0x08u);   /* the register agrees    */
        st->user_font_on = 0;                         /* the ROM font comes back with the mode */
        /* The cell height the mode's BIOS font gives: 8x16 in the VGA text modes and
           the 480-line graphics modes, 8x14 at 350 lines, 8x8 at 200. */
        st->cell_h = (uint8_t)((st->mode <= 0x03 || st->mode == 0x07 ||
                                st->mode == 0x11 || st->mode == 0x12) ? 16
                             : (st->mode == 0x0F || st->mode == 0x10) ? 14 : 8);
        load_default_palette(st);                     /* HW reloads the DAC on mode set */
        load_default_crtc(st);                        /* ...and reprograms the CRTC     */
        {   unsigned mi; const void *found = 0;
            for (mi = 0; mi < sizeof(vid_modes)/sizeof(vid_modes[0]); ++mi)
                if (vid_modes[mi].mode == st->mode) { found = &vid_modes[mi]; break; }
            if (!found) {                             /* a mode nobody defines   */
                VID_UNIMPL_SET(st->unimpl_mode, st->mode);
                st->mkind = VID_KIND_TEXT;
                st->cols = VID_COLS; st->rows = VID_ROWS;
                st->gw = VID_FB_W; st->gh = VID_FB_H;
                if (!noclear) clear_text(st, 0x07);
            } else {
                st->mkind = vid_modes[mi].kind;
                st->cols  = vid_modes[mi].cols;
                st->rows  = vid_modes[mi].rows;
                st->gw    = vid_modes[mi].w;
                st->gh    = vid_modes[mi].h;
                if (st->mkind == VID_KIND_UNSUP) {
                    /* Say so; do not paint a text screen and let the program
                       draw into a layout that is not there. */
                    VID_UNIMPL_SET(st->unimpl_mode, st->mode);
                    st->mkind = VID_KIND_TEXT;
                    st->cols = VID_COLS; st->rows = VID_ROWS;
                    st->gw = VID_FB_W;  st->gh = VID_FB_H;
                    if (!noclear) clear_text(st, 0x07);
                } else if (st->mkind == VID_KIND_LINEAR8) {
                    int i; if (!noclear) for (i = 0; i < VID_G13_W * VID_G13_H; ++i) st->vmem[i] = 0;
                } else if (st->mkind == VID_KIND_PLANAR) {
                    int pl; uint32_t i;
                    if (!noclear)
                        for (pl = 0; pl < 4; ++pl)
                            for (i = 0; i < VID_PLANE_SIZE; ++i) st->plane[pl][i] = 0;
                } else if (st->mkind == VID_KIND_CGA) {
                    int i;
                    st->cga_bpp = (uint8_t)(st->mode == 0x06 ? 1 : 2);
                    st->cga_pal = 0;
                    if (!noclear) for (i = 0; i < 16384; ++i) st->vmem[VID_TEXT_OFF + i] = 0;
                } else {
                    if (!noclear) clear_text(st, 0x07);
                }
            }
            if (st->mode_qn < 8) {
                st->mode_q[st->mode_qn].mode = st->mode;
                st->mode_q[st->mode_qn].kind = st->mkind;
                st->mode_q[st->mode_qn].cols = st->cols;
                st->mode_q[st->mode_qn].rows = st->rows;
                st->mode_q[st->mode_qn].w    = st->gw;
                st->mode_q[st->mode_qn].h    = st->gh;
                st->mode_qn++;
            }
        } }
        break;
    case 0x01: st->cur_shape = r_cx(r); break;
    case 0x02: st->cur_row = (uint8_t)(r_dx(r) >> 8); st->cur_col = (uint8_t)(r_dx(r) & 0xFF); break;
    case 0x03:
        /* THERE IS NO TEXT CURSOR IN A GRAPHICS MODE, and the BIOS says so: CX comes
           back 0000 in 06h/12h/13h, where we were still handing out the text
           underline shape 0607. Measured, p_video.asm int10.03.<mode>. The stored
           shape is left alone so returning to a text mode restores it. */
        s_dx(r, (uint16_t)((st->cur_row << 8) | st->cur_col));
        s_cx(r, (uint16_t)(st->mkind == VID_KIND_TEXT ? st->cur_shape : 0));
        break;
    case 0x05: st->page = al; break;
    case 0x06:
        scroll_up(st, al, (uint8_t)(r_cx(r) >> 8), (uint8_t)(r_cx(r) & 0xFF),
                  (uint8_t)(r_dx(r) >> 8), (uint8_t)(r_dx(r) & 0xFF), (uint8_t)(r_bx(r) >> 8));
        break;
    case 0x08: { uint8_t *p = cell(st, st->cur_row, st->cur_col);
                 s_ax(r, (uint16_t)((p[1] << 8) | p[0])); } break;
    case 0x09:
    case 0x0A: {
        uint16_t n = r_cx(r); uint8_t attr = (uint8_t)(r_bx(r) & 0xFF);
        int c = st->cur_col, rr = st->cur_row; if (!n) n = 1;
        while (n-- && rr < st->rows) {
            uint8_t *p = cell(st, rr, c); p[0] = al; if (ah == 0x09) p[1] = attr;
            if (st->mkind == VID_KIND_PLANAR)         /* draw the glyph as pixels */
                glyph_12h(st, c, rr, al, (uint8_t)(attr & 0x0F), (uint8_t)((attr >> 4) & 0x0F));
            if (++c >= st->cols) { c = 0; if (++rr >= st->rows) break; }
        }
        break; }
    case 0x0C:                                        /* write graphics pixel    */
        if (st->mkind == VID_KIND_LINEAR8) {
            uint32_t x = r_cx(r), y = r_dx(r);
            if (x < VID_G13_W && y < VID_G13_H) st->vmem[y * VID_G13_W + x] = al;
        } else if (st->mkind == VID_KIND_PLANAR) {    /* planar: set 4-bit colour */
            uint32_t x = r_cx(r), y = r_dx(r);
            if (x < VID_G12_W && y < VID_G12_H) {
                uint32_t byte = y * (VID_G12_W / 8) + (x >> 3);
                uint8_t  bit = (uint8_t)(0x80 >> (x & 7)), p;
                for (p = 0; p < 4; ++p) {
                    if (al & (1 << p)) st->plane[p][byte] |= bit;
                    else               st->plane[p][byte] &= (uint8_t)~bit;
                }
            }
        }
        break;
    case 0x0E:                                        /* teletype                */
        if (st->mkind == VID_KIND_PLANAR && al >= 0x20) { /* graphics: glyph + advance */
            glyph_12h(st, st->cur_col, st->cur_row, al, (uint8_t)(r_bx(r) & 0x0F), 0);
            advance(st);
        } else teletype(st, al);
        break;
    case 0x0F:
        /* BH is the active page; BL IS NOT DEFINED BY THIS CALL and the real BIOS
           leaves it alone -- we were zeroing the whole of BX and taking the caller's
           BL with it. Measured: the oracle returns the probe's poison in BL. */
        s_ax(r, (uint16_t)((st->cols << 8) | st->mode));
        s_bx(r, (uint16_t)((st->page << 8) | (r_bx(r) & 0xFF)));
        break;
    case 0x10:                                        /* palette / DAC            */
        if (al == 0x10) {                             /* set one DAC register     */
            uint16_t idx = r_bx(r);
            st->dac_block[((idx & 0xFF) >> 4) & 15]++;
            st->dac[idx & 0xFF] = dac_pack((uint8_t)(r_dx(r) >> 8) & 0x3F,
                                           (uint8_t)(r_cx(r) >> 8) & 0x3F,
                                           (uint8_t)(r_cx(r) & 0x3F));
            pal_refresh(st);
        } else if (al == 0x12) {                      /* set block of DAC regs    */
            uint16_t first = r_bx(r), n = r_cx(r), i;
            uint8_t *t = (uint8_t *)vdd_map_flat(st->bus, r->es, (uint16_t)r_dx(r));
            for (i = 0; i < n && (first + i) < 256; ++i)
                { st->dac[first + i] = dac_pack(t[i*3] & 0x3F, t[i*3+1] & 0x3F, t[i*3+2] & 0x3F);
                  st->dac_block[((first + i) >> 4) & 15]++;
                  if (((first + i) & 0xF0) == 0x30) st->dac_hi_since_reset++; }
            st->dac_writes += n;
            pal_refresh(st);
        } else if (al == 0x00) {                      /* set one palette register */
            uint8_t reg = (uint8_t)((r_bx(r) >> 8) & 0xFF);
            if (reg < 17) { st->vpal[reg] = (uint8_t)(r_bx(r) & 0x3F); st->ac_bios_writes++; }
            pal_refresh(st);   /* AH=10h stored vpal and rendered from ega16: inert until now */
        } else if (al == 0x01) {                      /* set the border           */
            st->vpal[16] = st->overscan = (uint8_t)((r_bx(r) >> 8) & 0x3F);
        } else if (al == 0x02) {                      /* set all 16 + border      */
            uint8_t *t = (uint8_t *)vdd_map_flat(st->bus, r->es, (uint16_t)r_dx(r));
            int i; for (i = 0; i < 17; ++i) st->vpal[i] = (uint8_t)(t[i] & 0x3F);
            st->overscan = st->vpal[16];
            st->ac_bios_writes += 17;
            pal_refresh(st);
        } else if (al == 0x03) {                      /* blink vs bright background */
            /* The BIOS's job here is to write AR10 bit 3; keep both in step so a
               guest that sets it through the BIOS and then READS the register back
               sees what it asked for. Oracle-measured (p_video.asm ar10.blink.*):
               BL=0 leaves AR10 bit 3 clear, BL=1 sets it. */
            st->blink = (uint8_t)(r_bx(r) & 1);
            st->attr_mode = (uint8_t)((st->attr_mode & ~0x08u) | (st->blink ? 0x08u : 0u));
        } else if (al == 0x07) {                      /* get one palette register */
            uint8_t reg = (uint8_t)((r_bx(r) >> 8) & 0xFF);
            s_bx(r, (uint16_t)((r_bx(r) & 0xFF00) | (reg < 17 ? st->vpal[reg] : 0)));
        } else if (al == 0x08) {                      /* get the border           */
            s_bx(r, (uint16_t)((r_bx(r) & 0x00FF) | ((uint16_t)st->vpal[16] << 8)));
        } else if (al == 0x09) {                      /* get all 16 + border      */
            uint8_t *t = (uint8_t *)vdd_map_flat(st->bus, r->es, (uint16_t)r_dx(r));
            int i; for (i = 0; i < 17; ++i) t[i] = st->vpal[i];
        } else if (al == 0x13) {                      /* select DAC page / mode   */
            st->dac_page = (uint8_t)(r_bx(r) >> 8);
        } else if (al == 0x15) {                      /* get one DAC register     */
            uint32_t v = st->dac[r_bx(r) & 0xFF];
            s_dx(r, (uint16_t)(((v >> 18) & 0x3F) << 8));
            s_cx(r, (uint16_t)(((((v >> 10) & 0x3F)) << 8) | ((v >> 2) & 0x3F)));
        } else if (al == 0x17) {                      /* get block of DAC regs    */
            uint16_t first = r_bx(r), n = r_cx(r), i;
            uint8_t *t = (uint8_t *)vdd_map_flat(st->bus, r->es, (uint16_t)r_dx(r));
            for (i = 0; i < n && (first + i) < 256; ++i) {
                uint32_t v = st->dac[first + i];
                t[i*3] = (uint8_t)((v >> 18) & 0x3F);
                t[i*3+1] = (uint8_t)((v >> 10) & 0x3F);
                t[i*3+2] = (uint8_t)((v >> 2) & 0x3F);
            }
        } else if (al == 0x1A) {                      /* get DAC page state       */
            s_bx(r, (uint16_t)((st->dac_page << 8) | 0));
        } else if (al == 0x1B) {                      /* convert to grey scale    */
            uint16_t first = r_bx(r), n = r_cx(r), i;
            for (i = 0; i < n && (first + i) < 256; ++i) {
                uint32_t v = st->dac[first + i];
                uint32_t g = ((((v >> 16) & 0xFF) * 30) + (((v >> 8) & 0xFF) * 59)
                              + ((v & 0xFF) * 11)) / 100;
                st->dac[first + i] = 0xFF000000u | (g << 16) | (g << 8) | g;
            }
            pal_refresh(st);
        } else {
            VID_UNIMPL_SET(st->unimpl_fn, 0x10);      /* name it, do not ignore it */
        }
        break;
    case 0x07: {                                       /* scroll window DOWN      */
        /* 06h scrolled up and 07h fell through to the unimplemented default, so
           any program scrolling downwards silently did nothing. */
        uint8_t n = al, top = (uint8_t)(r_cx(r) >> 8), lft = (uint8_t)(r_cx(r) & 0xFF);
        uint8_t bot = (uint8_t)(r_dx(r) >> 8), rgt = (uint8_t)(r_dx(r) & 0xFF);
        uint8_t attr = (uint8_t)(r_bx(r) >> 8);
        int rr, cc, k;
        if (!n || n > (bot - top + 1)) {               /* 0 or oversized = clear  */
            for (rr = top; rr <= bot; ++rr)
                for (cc = lft; cc <= rgt; ++cc)
                    { uint8_t *p2 = cell(st, rr, cc); p2[0] = ' '; p2[1] = attr; }
        } else {
            for (k = 0; k < n; ++k) {
                for (rr = bot; rr > top; --rr)
                    for (cc = lft; cc <= rgt; ++cc) {
                        uint8_t *d2 = cell(st, rr, cc), *s2 = cell(st, rr - 1, cc);
                        d2[0] = s2[0]; d2[1] = s2[1];
                    }
                for (cc = lft; cc <= rgt; ++cc)
                    { uint8_t *p2 = cell(st, top, cc); p2[0] = ' '; p2[1] = attr; }
            }
        }
        break; }
    case 0x0B:                                         /* set background / palette */
        if ((r_bx(r) >> 8) == 0x00) st->overscan = (uint8_t)(r_bx(r) & 0xFF);
        else                        st->cga_pal  = (uint8_t)(r_bx(r) & 0x01);
        break;
    case 0x0D: {                                       /* READ a pixel            */
        uint16_t x = r_cx(r), y = r_dx(r);
        uint8_t v = 0;
        if (st->mkind == VID_KIND_LINEAR8) {
            if (x < st->gw && y < st->gh) v = st->vmem[y * st->gw + x];
        } else if (st->mkind == VID_KIND_PLANAR) {
            uint32_t byi = y * (st->gw / 8) + (x >> 3);
            uint8_t  msk = (uint8_t)(0x80 >> (x & 7));
            if (byi < VID_PLANE_SIZE)
                v = (uint8_t)(((st->plane[0][byi] & msk) ? 1 : 0)
                            | ((st->plane[1][byi] & msk) ? 2 : 0)
                            | ((st->plane[2][byi] & msk) ? 4 : 0)
                            | ((st->plane[3][byi] & msk) ? 8 : 0));
        }
        s_ax(r, (uint16_t)((r_ax(r) & 0xFF00) | v));
        break; }
    case 0x1C: {                                       /* save / restore state    */
        /* CX is a bitmask of what to save; ES:BX is the buffer. Same state block as
           VBE 4F04 -- see vid_state_save(). ⚠ This used to report 3 blocks and then
           write 768 bytes: a guest that allocated what it was told got its next MCB
           overwritten. The size reported is now the size written. */
        uint8_t al1c = al;
        if (al1c == 0x00)      { s_bx(r, VID_STATE_BLOCKS); s_ax(r, (uint16_t)((r_ax(r) & 0xFF00) | 0x1C)); }
        else if (al1c == 0x01 || al1c == 0x02) {
            uint8_t *buf = (uint8_t *)vdd_map_flat(st->bus, r->es, (uint16_t)r->ebx);
            if (al1c == 0x01) vid_state_save(st, buf, r_cx(r));
            else              (void)vid_state_load(st, buf);   /* a foreign buffer: no-op */
            s_ax(r, (uint16_t)((r_ax(r) & 0xFF00) | 0x1C));
        }
        break; }
    case 0x13: {                                       /* write string ES:BP      */
        uint16_t n = r_cx(r), i; uint8_t mode = al, attr = (uint8_t)(r_bx(r) & 0xFF);
        uint8_t *s = (uint8_t *)vdd_map_flat(st->bus, r->es, (uint16_t)r->ebp);
        st->cur_row = (uint8_t)(r_dx(r) >> 8); st->cur_col = (uint8_t)(r_dx(r) & 0xFF);
        for (i = 0; i < n; ++i) {
            uint8_t ch = *s++; if (mode & 0x02) attr = *s++;
            cell(st, st->cur_row, st->cur_col)[0] = ch;
            cell(st, st->cur_row, st->cur_col)[1] = attr;
            advance(st);
        }
        break; }
    case 0x11:                                         /* character generator     */
        st->int10_11_calls++;                          /* did the guest ASK at all? */
        if (al == 0x30) {                              /* get font info -> ES:BP  */
            /* THE POINTER IS THE POINT. BH selects which table the caller wants, and the
               answer is returned in ES:BP with CX = bytes per character. Returning only
               CX/DL (as we used to) leaves the caller drawing from whatever ES:BP already
               held -- which is why Skyroads' "ROAD COMPLETED" came out as glyph-shaped
               noise. BH: 0/1 = the INT 1Fh / INT 43h vectors, 2 = 8x14, 3 = 8x8 lower,
               4 = 8x8 upper (chars 128-255), 5 = 9x14 alt, 6 = 8x16, 7 = 9x16 alt. We hold
               two real tables and answer every code from the nearer of the two. */
            uint8_t bh = (uint8_t)((r_bx(r) >> 8) & 0xFF);
            uint16_t seg = VDD_FONT8X16_SEG, off = 0, bpc = 16;
            switch (bh) {
            case 0x03: seg = VDD_FONT8X8_SEG;  off = 0;       bpc = 8;  break;
            case 0x00:                                        /* INT 1Fh: 8x8 upper half */
            case 0x04: seg = VDD_FONT8X8_SEG;  off = 128 * 8; bpc = 8;  break;
            case 0x02:                                        /* ROM 8x14 / 9x14 alt     */
            case 0x05: seg = VDD_FONT8X14_SEG; off = 0;       bpc = 14; break;
            case 0x01:                                        /* INT 43h: the CURRENT font */
                /* Whatever the active mode actually draws with -- 8x8 in the 200-line
                   graphics modes, 8x16 in text. We used to answer this (and 8x14) with the
                   8x16 table while reporting CX=14, so a caller striding by 14 through
                   16-byte glyphs drifted 2 bytes per character and drew shredded text.
                   cell_h is that answer, and it follows a 1112h/1111h font change too. */
                if      (st->cell_h == 8)  { seg = VDD_FONT8X8_SEG;  off = 0; bpc = 8;  }
                else if (st->cell_h == 14) { seg = VDD_FONT8X14_SEG; off = 0; bpc = 14; }
                else                       { seg = VDD_FONT8X16_SEG; off = 0; bpc = 16; }
                break;
            default:   seg = VDD_FONT8X16_SEG; off = 0;       bpc = 16; break;
            }
            r->es = seg; r->ebp = off;
            /* ── ★ CX IS THE ON-SCREEN FONT'S HEIGHT, NOT THE REQUESTED TABLE'S. ──
                 The classic gotcha in this call, and we had it backwards: BH selects
                 which TABLE ES:BP points at, but CX reports the height of the font
                 the screen is CURRENTLY drawing with. Measured on 6.22: BH=0 asks for
                 the 8x8 upper half and CX still comes back 16 in mode 3. We answered
                 8, contradicting our OWN BDA byte at 0040:0085 two lines of probe
                 output earlier -- and a 43/50-line editor sizes the screen from CX. */
            s_cx(r, st->cell_h ? st->cell_h : bpc);    /* on-screen bytes/character */
            s_dx(r, (uint16_t)(st->rows ? st->rows - 1 : 24));  /* DL = rows-1     */
            if (st->font_qn < 4) {                     /* record the request + answer */
                st->font_q[st->font_qn].al  = al;
                st->font_q[st->font_qn].bh  = bh;
                st->font_q[st->font_qn].seg = seg;
                st->font_q[st->font_qn].off = off;
                st->font_q[st->font_qn].cx  = bpc;
                st->font_qn++;
            }
        } else if ((al & 0x0F) <= 0x04 && (al <= 0x04 || (al >= 0x10 && al <= 0x14))) {
            /* ── AL=x0 LOADS THE CALLER'S OWN GLYPHS, AND NOW THEY GET DRAWN. ──
                 This used to accept the call, mark it unimplemented and keep
                 drawing from the ROM table -- so a program that loaded a custom
                 character set saw the stock font and no error at all. That is
                 the silent-wrong-output class: the call succeeded, the screen
                 was wrong, and nothing said so.
                 ES:BP = the table, CX = how many characters, DX = the first
                 character, BH = bytes per character, BL = the font block.
                 AL=x1/x2/x3/x4 select ROM fonts, which is a request to go BACK
                 to our own tables -- so they clear the override rather than
                 leaving a stale user font in place. (GH #52) */
            if ((al & 0x0F) == 0x00) {
                uint16_t fseg = r->es, foff = (uint16_t)(r->ebp & 0xFFFF);
                uint16_t cnt = (uint16_t)r_cx(r), first = (uint16_t)r_dx(r);
                uint8_t  bpc = (uint8_t)((r_bx(r) >> 8) & 0xFF);
                const uint8_t *src = (const uint8_t *)vdd_map_flat(st->bus, fseg, foff);
                if (!src || bpc == 0 || bpc > VID_CELL_H) {
                    /* Cannot represent it -- a cell is VID_CELL_H tall. Say so
                       rather than store something the renderer would misread. */
                    VID_UNIMPL_SET(st->unimpl_fn, 0x11);
                } else {
                    unsigned i, y;
                    if (!st->user_font_on) {       /* seed from ROM so characters
                                                      the caller does NOT supply
                                                      still draw as themselves */
                        unsigned c2;
                        for (c2 = 0; c2 < 256; ++c2)
                            for (y = 0; y < VID_CELL_H; ++y)
                                st->user_font[c2 * VID_CELL_H + y] = vga_font_8x16[c2][y];
                    }
                    for (i = 0; i < cnt && (first + i) < 256; ++i) {
                        unsigned ch2 = first + i;
                        for (y = 0; y < VID_CELL_H; ++y)
                            st->user_font[ch2 * VID_CELL_H + y] =
                                (y < bpc) ? src[i * bpc + y] : 0;
                    }
                    st->user_font_rows = bpc;
                    st->user_font_on = 1;
                    st->dirty = 1;
                    /* AL=10h (not 00h) also reprograms the CRTC for the new height:
                       the cell becomes the font's height and the row count follows. */
                    if (al == 0x10 && st->mkind == VID_KIND_TEXT) {
                        st->cell_h = bpc;
                        st->rows = text_rows_for(bpc);
                    }
                }
            } else {
                /* ── ★ AL=x1/x2/x4 SELECT A ROM FONT, AND WITH 1x THAT IS A ROW COUNT. ──
                     1112h is THE 50-line call: load the 8x8 ROM font and, because AL
                     has bit 4 set, recompute the CRTC -- 400 lines / 8 = 50 rows.
                     1111h is 8x14 (28 rows) and 1114h 8x16 (25). The 0x variants only
                     change the glyphs, which is what the BIOS documents and what a
                     caller that follows 1102h with its own CRTC programming expects. */
                uint8_t h = (uint8_t)(((al & 0x0F) == 0x02) ? 8 : ((al & 0x0F) == 0x01) ? 14 : 16);
                st->user_font_on = 0;              /* back to the ROM tables */
                if ((al & 0x10) && st->mkind == VID_KIND_TEXT) {
                    st->cell_h = h;
                    st->rows = text_rows_for(h);
                    if (st->cur_row >= st->rows) st->cur_row = (uint8_t)(st->rows - 1);
                }
                st->dirty = 1;
            }
            s_dx(r, (uint16_t)(st->rows ? st->rows - 1 : 24));
        } else if (al >= 0x20 && al <= 0x24) {
            /* Set the graphics-mode font pointer used by INT 43h / INT 1Fh. */
            s_dx(r, (uint16_t)(st->rows ? st->rows - 1 : 24));
        } else {
            VID_UNIMPL_SET(st->unimpl_fn, 0x11);
        }
        break;
    case 0x12: {                                       /* alternate function sel  */
        uint8_t bl = (uint8_t)(r_bx(r) & 0xFF);
        if (bl == 0x10) { s_bx(r, 0x0003); s_cx(r, 0x0009); }  /* color / 256K, switches */
        /* ── ★★ BL=31h: DEFAULT PALETTE LOADING. NOT A NO-OP. ────────────────────
             A mode set normally reloads the DAC and the attribute palette, which
             destroys any colours the program has already installed. A game that
             wants to keep them says so with this call BEFORE setting the mode --
             and that is exactly what Lemmings does.
           ⚠ WE USED TO ACCEPT IT AND IGNORE IT. The `else` arm below answers
             AL=12h ("supported") to every unhandled BL, so this looked handled and
             did nothing: measured, Lemmings wrote 752 entries into DAC 0x30..0x3F,
             a mode set wiped them (palresets=5, hi_since_reset=0), and it never
             wrote them again -- because on real hardware it did not have to. Its
             terrain then rendered in the leftover default EGA brights, which is the
             cyan-and-green "garbling" this whole thread has been chasing.
             The same "unimplemented call still answers" shape as ever: a sentinel
             that reads as success is worse than a refusal. */
        else if (bl == 0x31) {
            st->def_pal_off = (uint8_t)((r_ax(r) & 0xFF) ? 1 : 0);
            s_ax(r, (uint16_t)((r_ax(r) & 0xFF00) | 0x12));
        }
        else            { s_ax(r, (uint16_t)((r_ax(r) & 0xFF00) | 0x12)); } /* supported */
        break; }
    case 0x1A:                                         /* get display combination */
        s_ax(r, (uint16_t)((r_ax(r) & 0xFF00) | 0x1A));/* AL=1A: function present */
        s_bx(r, 0x0008);                               /* BL=08 active=VGA colour */
        break;
    case 0x1B: {                                       /* functionality/state info */
        uint8_t *b = (uint8_t *)vdd_map_flat(st->bus, r->es, (uint16_t)r->edi);
        unsigned i;
        for (i = 0; i < 64; ++i) b[i] = 0;
        /* static functionality table kept inside the 64-byte block (reserved tail
           at 0x2E) so we never write past the caller's buffer. */
        wr32(b + 0, ((uint32_t)r->es << 16) | (((uint16_t)r->edi + 0x2E) & 0xFFFF));
        b[4] = (uint8_t)st->mode;                      /* current mode            */
        wr16(b + 5, st->cols);                         /* columns on screen       */
        b[0x22] = (uint8_t)(st->rows ? st->rows : 25); /* character rows          */
        wr16(b + 0x23, st->cell_h ? st->cell_h : 16);  /* bytes per character     */
        b[0x25] = 0x08;                                /* active DCC = VGA colour */
        wr16(b + 0x27, 256);                           /* number of colours       */
        b[0x29] = 8;                                   /* number of pages         */
        b[0x2A] = 0;                                   /* scan lines (0 = 200)    */
        b[0x2B] = 0; b[0x2C] = 0; b[0x2D] = 0x21;      /* char blocks / misc      */
        /* static functionality table (16 bytes) -- modes 0..0x1F all supported   */
        b[0x2E + 0] = 0xFF; b[0x2E + 1] = 0xFF; b[0x2E + 2] = 0xFF; b[0x2E + 3] = 0xFF;
        b[0x2E + 7] = 0x07;                            /* scan lines 200/350/400  */
        b[0x2E + 8] = 8; b[0x2E + 9] = 8;              /* char blocks             */
        b[0x2E + 0x0A] = 0xFF; b[0x2E + 0x0B] = 0x07;  /* capability bits         */
        s_ax(r, (uint16_t)((r_ax(r) & 0xFF00) | 0x1B));/* AL=1B: supported        */
        break; }
    case 0x4F: vesa(st, r); break;                     /* VESA VBE 2.0            */
    default:                                           /* unimplemented function  */
        VID_UNIMPL_SET(st->unimpl_fn, ah);
        st->dirty = 0;
        break;
    }
    vdd_video_bda_sync(st);                            /* the BDA follows every call */
}

/* DAC palette ports 3C7 (read index) / 3C8 (write index) / 3C9 (data). */
static void dac_out(void *self, uint16_t port, uint8_t w, uint32_t v)
{
    video_state *st = (video_state *)self; uint8_t val = (uint8_t)v; (void)w;
    if (port == 0x3C8) { st->dac_widx = val; st->dac_comp = 0; }
    else if (port == 0x3C7) { st->dac_ridx = val; st->dac_comp = 0; }
    else if (port == 0x3C9) {
        st->dac_latch[st->dac_comp++] = val & 0x3F;
        if (st->dac_comp >= 3) {
            st->dac[st->dac_widx] = dac_pack(st->dac_latch[0], st->dac_latch[1], st->dac_latch[2]);
            st->dac_block[(st->dac_widx >> 4) & 15]++;
            if ((st->dac_widx & 0xF0) == 0x30) st->dac_hi_since_reset++;
            st->dac_widx++; st->dac_comp = 0; st->dac_writes++;
            {   uint64_t now; uint32_t frame_us, vtotal, vdisp, vblank, frame_no, line; uint16_t row = 0xFFFE;
                if (vid_beam(st, &now, &frame_us, &vtotal, &vdisp, &vblank, &frame_no, &line) && st->gh && vdisp)
                    row = (line >= vblank) ? 0xFFFF : (uint16_t)(((uint64_t)line * st->gh) / vdisp);
                st->dac_last_row = row;
                st->dac_row_hist[row == 0xFFFF ? 3 : row == 0xFFFE ? 0 : row < 2 ? 0 : row < 160 ? 1 : 2]++;
            }
            /* pal[] is DERIVED from dac[] -- see pal_refresh. Without this a guest
               could reprogram the DAC and see nothing change, which is precisely the
               half of the Lemmings bug that survived the first fix. */
            pal_refresh(st);
        }
    }
}
static void dac_in(void *self, uint16_t port, uint8_t w, uint32_t *v)
{
    video_state *st = (video_state *)self; uint32_t p; (void)w;
    if (port == 0x3C8) { *v = st->dac_widx; return; }
    if (port != 0x3C9) { *v = 0xFF; return; }
    p = st->dac[st->dac_ridx];
    switch (st->dac_comp) {
    case 0: *v = ((p >> 16) & 0xFF) >> 2; break;      /* R 8->6                  */
    case 1: *v = ((p >> 8) & 0xFF) >> 2; break;       /* G                       */
    default:*v = (p & 0xFF) >> 2; st->dac_ridx++; break;/* B, then advance        */
    }
    if (++st->dac_comp >= 3) st->dac_comp = 0;
}

/* --- VGA planar write engine (mode 12h: Sequencer 3C4/5 + GC 3CE/F) ------- */
static uint8_t vga_ror(uint8_t v, uint8_t n)
{ n &= 7; return n ? (uint8_t)((v >> n) | (v << (8 - n))) : v; }
static uint8_t vga_alu(uint8_t op, uint8_t v, uint8_t lat)
{ switch (op & 3) { case 1: return (uint8_t)(v & lat); case 2: return (uint8_t)(v | lat);
                    case 3: return (uint8_t)(v ^ lat); default: return v; } }

static void vga_planar_write_1(video_state *st, uint32_t off, uint8_t cpu)
{
    uint8_t alu = (uint8_t)((st->func_rotate >> 3) & 3), bm = st->bit_mask; int p;
    if (off >= VID_PLANE_SIZE) return;
    st->dirty = 1;
    switch (st->write_mode & 3) {
    case 1:                                       /* copy latches -> planes        */
        for (p = 0; p < 4; ++p) if (st->map_mask & (1<<p)) st->plane[p][off] = st->latch[p];
        return;
    case 2:                                       /* CPU bit p -> plane p           */
        for (p = 0; p < 4; ++p) {
            uint8_t val = (uint8_t)((cpu & (1<<p)) ? 0xFF : 0x00);
            uint8_t r = vga_alu(alu, val, st->latch[p]);
            r = (uint8_t)((r & bm) | (st->latch[p] & (uint8_t)~bm));
            if (st->map_mask & (1<<p)) st->plane[p][off] = r;
        }
        return;
    case 3: {                                     /* set/reset masked by rot(cpu)&bm */
        uint8_t data = vga_ror(cpu, st->func_rotate), mask = (uint8_t)(data & bm);
        for (p = 0; p < 4; ++p) {
            uint8_t val = (uint8_t)((st->set_reset & (1<<p)) ? 0xFF : 0x00);
            uint8_t r = (uint8_t)((val & mask) | (st->latch[p] & (uint8_t)~mask));
            if (st->map_mask & (1<<p)) st->plane[p][off] = r;
        }
        return; }
    default: {                                    /* write mode 0                   */
        uint8_t data = vga_ror(cpu, st->func_rotate);
        st->w_ensr_hist[st->enable_sr & 0x0F]++;
        st->w_alu_hist[alu & 3]++;
        for (p = 0; p < 4; ++p) {
            uint8_t val = (st->enable_sr & (1<<p)) ? (uint8_t)((st->set_reset & (1<<p)) ? 0xFF : 0x00) : data;
            uint8_t r = vga_alu(alu, val, st->latch[p]);
            r = (uint8_t)((r & bm) | (st->latch[p] & (uint8_t)~bm));
            if (p == 3 && (st->map_mask & 8)) {
                if (st->enable_sr & 8) { st->w_p3_sr++; if (r) st->w_p3_nz++; }
                else                     st->w_p3_data++;
            }
            if (st->map_mask & (1<<p)) st->plane[p][off] = r;
        }
        return; }
    }
}

/* ── THE CACHE WITNESS. A linear scan over ten slots, entered only for accesses
     above VID_CACHE_LO, so its cost falls on nothing that draws the screen. It is
     deliberately NOT a hash: the whole point is that "this pc never appeared" must
     mean the pc never ran, and a hashed table cannot say that. See vdd_video.h. */
static void csite_note(video_state *st, uint32_t off, int wr)
{
    uint32_t pc, i;
    if (off < VID_CACHE_LO || !st->guest_pc) return;
    pc = st->guest_pc();
    st->csite_seq++;
    for (i = 0; i < VID_CSITES; ++i) {
        vid_csite *c = &st->csite[i];
        if (c->n) {
            if (c->pc != pc || c->wr != (uint8_t)wr) continue;
            if (off < c->lo) c->lo = off;
            if (off > c->hi) c->hi = off;
        } else {
            c->pc = pc; c->wr = (uint8_t)wr; c->lo = c->hi = off;
            c->first = st->csite_seq;
        }
        c->n++; c->last = st->csite_seq;
        return;
    }
    st->csite_lost++;
}

/* The watchpoint wrapper. The engine above is left exactly as it was so that the
   instrument cannot change what it measures; this only records around it. */
void vga_planar_write(video_state *st, uint32_t off, uint8_t cpu)
{
    vid_watch_rec r; int p;
    if (off > st->planar_hi_water) st->planar_hi_water = off;
    if (st->guest_pc) {
        uint32_t pc = st->guest_pc();
        vid_wsite *w = &st->wsite[VID_WSITE_HASH(pc)];
        if (!w->n)            { w->pc = pc; w->lo = w->hi = off; w->n = 1; }
        else if (w->pc == pc) { if (off < w->lo) w->lo = off;
                                if (off > w->hi) w->hi = off; w->n++; }
        else                  st->wsite_lost++;
    }
    csite_note(st, off, 1);
    if (off != st->watch_off) { vga_planar_write_1(st, off, cpu); return; }
    r.pc = st->guest_pc ? st->guest_pc() : 0;
    r.wmode = (uint8_t)(st->write_mode & 3); r.map_mask = st->map_mask;
    r.ensr = st->enable_sr; r.set_reset = st->set_reset;
    r.frot = st->func_rotate; r.bit_mask = st->bit_mask; r.cpu = cpu;
    for (p = 0; p < 4; ++p) r.latch[p] = st->latch[p];
    vga_planar_write_1(st, off, cpu);
    for (p = 0; p < 4; ++p)
        r.after[p] = (off < VID_PLANE_SIZE) ? st->plane[p][off] : 0;
    if (st->watch_n < VID_WATCH_MAX) st->watch[st->watch_n] = r;
    st->watch_last = r;
    st->watch_n++;
}

uint8_t vga_planar_read(video_state *st, uint32_t off)
{
    int p;
    if (off > st->planar_hi_water) st->planar_hi_water = off;
    if (st->guest_pc) {
        uint32_t pc = st->guest_pc();
        vid_wsite *w = &st->rsite[VID_WSITE_HASH(pc)];
        if (!w->n)            { w->pc = pc; w->lo = w->hi = off; w->n = 1; }
        else if (w->pc == pc) { if (off < w->lo) w->lo = off;
                                if (off > w->hi) w->hi = off; w->n++; }
        else                  st->rsite_lost++;
        if (st->read_mode & 1) {
            vid_wsite *c = &st->rsite1[VID_WSITE_HASH(pc)];
            if (!c->n)            { c->pc = pc; c->lo = c->hi = off; c->n = 1; }
            else if (c->pc == pc) { if (off < c->lo) c->lo = off;
                                    if (off > c->hi) c->hi = off; c->n++; }
            else                  st->rsite1_lost++;
        }
    }
    csite_note(st, off, 0);
    if (off >= VID_PLANE_SIZE) return 0xFF;
    for (p = 0; p < 4; ++p) st->latch[p] = st->plane[p][off];   /* load latches    */
    st->rmode_hist[st->read_mode & 1]++;
    if (!(st->read_mode & 1))
        return st->plane[st->read_map & 3][off];                /* read mode 0     */
    /* ── READ MODE 1: COLOUR COMPARE. One bit per pixel, set where that pixel's
         colour matches GR2 in every plane GR7 selects. GR7 is "Color DON'T Care" and
         reads backwards: a SET bit means the plane DOES take part. With GR7 = 0 no
         plane is compared, so every pixel matches and the read is 0xFF -- which is the
         hardware's answer, not a failure, and worth not "fixing". */
    {   uint8_t r = 0; int b;
        for (b = 0; b < 8; ++b) {
            int match = 1;
            for (p = 0; p < 4; ++p) {
                if (!((st->col_dontcare >> p) & 1)) continue;
                if (((st->latch[p] >> b) & 1) != ((st->col_compare >> p) & 1)) { match = 0; break; }
            }
            if (match) r = (uint8_t)(r | (1 << b));
        }
        /* Record what we ANSWERED, not just that we were asked -- see vdd_video.h. */
        if (st->guest_pc) {
            uint32_t pc2 = st->guest_pc();
            unsigned h = VID_WSITE_HASH(pc2);
            if (st->rsite1[h].pc == pc2) {
                if (!r)            st->rsite1_zero[h]++;
                else if (r == 0xFF) st->rsite1_ones[h]++;
            }
        }
        return r; }
}

int vdd_video_planar_active(const video_state *st) { return st->mkind == VID_KIND_PLANAR; }

/* CRT timings, shared by the 0x3DA status read and the present scheduler. */
#define VID_VBL_HZ_HI     60        /* 640x480 modes                                */
#define VID_VBL_HZ_LO     70        /* 320x200 / 720x400 modes                      */
#define VID_VTOTAL_HI    525        /* scanlines per frame incl. blanking, 480-line */
#define VID_VTOTAL_LO    449        /*                                    400-line  */
#define VID_VACTIVE_HI   480
#define VID_VACTIVE_LO   400
#define VID_HACTIVE_PCT   80        /* % of a scanline that is active (rest = hblank) */

/* See the header. Phase within the frame, in permille, against the point where the
   active picture ends (480/525 = 914, 400/449 = 891). The window is the last ~12%
   of the active period: late enough that a guest released by the PREVIOUS retrace
   has finished its drawing, early enough to be a distinct instant every frame. */
#define VID_PRESENT_WINDOW_PM 120
/* A poll this long after the previous one means the guest went away to draw. Its own
   draw is 1-4 ms (BOUNCEBX 2.4); a poll loop iterates in well under 50 us. */
#define VID_PRESENT_GAP_US    400
static int vga_vtiming(const video_state *st, uint32_t *total, uint32_t *active,
                       uint32_t *blank_start);
/* The mode's frame period in microseconds -- the same 60/70 Hz choice the retrace
   model makes -- for the host's Auto fallback floor. 1/60 s when there is no clock. */
uint32_t vdd_video_frame_us(const video_state *st)
{
    uint32_t t, a, b; int tall = (st->gh > VID_VACTIVE_LO);
    if (vga_vtiming(st, &t, &a, &b)) tall = (t >= 500u);
    return 1000000u / (uint32_t)(tall ? VID_VBL_HZ_HI : VID_VBL_HZ_LO);
}
int vdd_video_present_ready(video_state *st)
{
    uint32_t frame_us, pm, t, a, b;
    int act, tall;
    if (!st->time_us) return 1;                 /* no clock: present every tick   */
    /* Same geometry the 0x3DA read uses, and for the same reason: 914/891 permille
       are the two BIOS cases, and 640x350 is neither -- its picture ends at 350 of
       449 lines, 780 permille. Presenting at 891 there meant building the frame 1.6ms
       into the blanking interval rather than at the end of the picture. */
    tall = (st->gh > VID_VACTIVE_LO);
    act  = tall ? 914 : 891;
    if (vga_vtiming(st, &t, &a, &b)) { tall = (t >= 500u); act = (int)(a * 1000u / t); }
    frame_us = 1000000u / (uint32_t)(tall ? VID_VBL_HZ_HI : VID_VBL_HZ_LO);
    if (!frame_us) return 1;
    pm  = (uint32_t)((st->time_us() % frame_us) * 1000u / frame_us);
    return (int)pm >= act - VID_PRESENT_WINDOW_PM && (int)pm < act;
}

/* Sequencer ports 3C4 (index) / 3C5 (data) -- Map Mask (SR2). */
/* ── ATTRIBUTE ONLY THE BYTES THAT CHANGED, NOT THE WHOLE APERTURE. ─────────────────
     The A0000 aperture is one flat buffer -- the page trap is deliberately not armed,
     because arming it makes the interpreter the CPU and collapses the run -- so a guest
     write lands there with no record of which plane the map mask had selected. This
     used to copy the ENTIRE aperture into the outgoing plane on every mask change, on
     the assumption that an unchained program fills one whole plane before moving to the
     next.

     Doom does not. It updates in dirty boxes: measured, the map mask changes about 516
     times per frame, four planes x ~129 boxes, each write touching a few columns. So
     every "snapshot" copied 16000 bytes of which only a handful belonged to that plane,
     and the other 15,900-odd were whatever the PREVIOUS plane had left behind. All four
     planes therefore converged on the same picture -- measured, they ended a run
     reporting an identical 48,031 non-zero bytes -- and the frame came out with every
     even column equal to the one after it. On screen that reads as Doom at half
     horizontal resolution, which is not a thing Doom can do: its own low-detail mode
     leaves the status bar alone, and the doubling was in the status bar too.

     The aperture does carry the information, just not in its addresses: a byte that
     CHANGED since the last flush was written under the mask that is now going out.
     So keep a shadow of the aperture and attribute the differences. That is exact
     without a page trap, and it is not more expensive than the copy it replaces --
     comparing dwords, an untouched region costs a quarter of the reads a copy did. */
/* ── DE-INTERLEAVE MODE Y BY ATTRIBUTING EACH CHANGED RUN TO THE SELECTED PLANE. ────
     A0000 is one flat buffer -- the page trap is deliberately not armed, because arming
     it makes the interpreter the CPU and collapses the run -- so a guest write lands
     there carrying no record of which plane the map mask had selected. It has to be
     recovered afterwards, from a shadow of the aperture.

   ► WHAT THE USER'S PLAY SESSION HANDED US, AND IT IS THE WHOLE DESIGN. "The intro
     screen is 320x200 until the menu shows, then it degrades... I played through the
     first level and got to the score screen, which went back to correct 320x200, and
     then degraded again on the next level."
     Title and intermission are FULL-SCREEN blits: one mask change per plane, the whole
     plane written under it. Menu, demo and gameplay are DIRTY-BOX updates: ~516 mask
     changes a frame, a few columns each. So the rule that copies the WHOLE aperture
     into the outgoing plane is exactly right for the first and exactly wrong for the
     second -- which is precisely the split seen on the screen, and confirms the model.

   ► THE RULE: copy the aperture over the CHANGED EXTENT WITHIN EACH GRANULE. A whole-
     plane write changes every granule end to end, so the copy is the whole plane and
     the full-screen case stays exact. A box changes only the granules its columns fall
     in, and only the span within them, so the rest of each plane keeps its own data.
     Sizing the granule is the entire trick, and each wrong answer was measured on
     captured frames (even-column match: 0.08 is a real 320-wide picture, 1.000 is
     every even column equal to the next, i.e. half horizontal resolution):

       whole aperture      1.000  correct only for full-screen writers
       changed bytes       0.08   full resolution but STREAKED -- a byte rewritten with
                                  the value it already held is still a write and the
                                  shadow cannot see it, and Doom's textures are full of
                                  equal neighbours
       changed span (all)  0.90   one global min..max spans nearly the whole page
       whole 16B granules  0.78   copying the WHOLE granule over-attributes: a plane
                                  byte is FOUR screen pixels wide, so 16 bytes span 64

     Per-granule min..max is the one that is tight in both directions: it never copies
     beyond the outermost change in a granule, and it carries the same-valued bytes
     between two changes, which is what kills the streaks.
   ⚠ Do not change the rule or the granule without measuring even-column match on a
     captured frame. Four plausible variants have already made it worse. */
/* A guest store to the aperture is a CONTIGUOUS RUN of plane bytes -- a row segment of
   whatever box is being updated. Find those runs in the diff and copy each one whole.
   Bytes inside a run that happen to be unchanged (the same value written again, which
   the shadow cannot see) come along with it; bytes outside stay with their own plane.
   MODEY_GAP is how many unchanged dwords may sit inside one run before it is treated as
   two: it is the only tuning constant here, and it trades streaks (too small) against
   over-attribution (too large). */
/* ► IT IS A KNOB, AND THE HUMAN IS THE INSTRUMENT. There is no good point on this
     curve -- six rules have been measured and every one trades resolution against
     stale streaks -- so the value is read from `modey.txt` on the share rather than
     compiled in, and a play session can walk it without a rebuild. Measured
     even-column match on Doom's 3D view (0.08 = a real 320-wide picture, 1.000 = every
     even column equal to the next):
         gap 0     tightest, most detail, most streaking
         gap 2     ~0.55, the shipped default
         gap huge  1.000, the old whole-aperture behaviour: coherent, half resolution
     A run of guest stores is contiguous, so this is how many unchanged dwords may sit
     inside one before it is treated as two. */
#define MODEY_GAP_DEFAULT 2u

static void modey_copy(video_state *st, const int *sel, int nsel, uint32_t lo, uint32_t hi)
{
    uint32_t i;
    for (i = lo; i < hi; ++i) {
        uint8_t b = st->vmem[i];
        int k;
        st->yshadow[i] = b;
        for (k = 0; k < nsel; ++k) st->yplane[sel[k]][i] = b;
    }
    st->ynz[0] += hi - lo;                              /* bytes attributed, for STAGE2 */
}

static void modey_flush(video_state *st)
{
    uint32_t i, run_lo = 0, run_hi = 0, gap = 0;
    const uint32_t *src32, *shd32;
    int p, sel[4], nsel = 0, in_run = 0;
    if (st->chain4 || st->mkind != VID_KIND_LINEAR8 || !st->vmem) return;
    for (p = 0; p < 4; ++p) if (st->y_mask & (1u << p)) sel[nsel++] = p;
    if (!nsel) return;
    src32 = (const uint32_t *)st->vmem;
    shd32 = (const uint32_t *)st->yshadow;

    /* Dword-at-a-time scan. Most of the aperture is untouched between two adjacent
       mask changes -- and in Doom there are ~3,800 of those a second -- so the reject
       path is the one that has to be cheap. */
    for (i = 0; i < VID_Y_PLANE / 4; ++i) {
        if (src32[i] != shd32[i]) {
            if (!in_run) { in_run = 1; run_lo = i; }
            run_hi = i + 1; gap = 0;
        } else if (in_run && ++gap > st->modey_gap) {
            modey_copy(st, sel, nsel, run_lo * 4u, run_hi * 4u);
            in_run = 0;
        }
    }
    if (in_run) modey_copy(st, sel, nsel, run_lo * 4u, run_hi * 4u);
    st->dirty = 1;
}

/* CRTC: only the registers unchained page-flipping needs. 0x0C/0x0D are the
   display START address (how a mode-Y program flips pages) and 0x13 the logical
   line width. Everything else is accepted and ignored -- this VDD does not model
   CRTC timing and pretending to would be worse than not. */
static void vga_idx_data(uint8_t *index, uint8_t w, uint32_t v,
                         void (*setdata)(void *, uint32_t), void *ctx)
{
    *index = (uint8_t)v;
    if (w == 2) setdata(ctx, (v >> 8) & 0xFF);
}

static void crtc_set_data(void *self, uint32_t v);

/* The cursor's CRTC address (0x0E/0x0F) as the BIOS path implies it: cells from the
   start of video memory, so the display start is added back in. */
static uint16_t crtc_cursor_of(const video_state *st)
{ return (uint16_t)(st->crtc_start + (unsigned)st->cur_row * st->cols + st->cur_col); }
/* ...and the reverse: a guest wrote 0x0E/0x0F, so derive row/col from it. A value
   off the visible page (some guests park the cursor at 0x7FFF to hide it) is left
   where it is -- hidden by being out of range, as it is on the card. */
static void crtc_cursor_apply(video_state *st)
{
    unsigned pos = st->crtc_cursor;
    unsigned base = st->crtc_start;
    st->dirty = 1;
    if (!st->cols || !st->rows) return;
    if (pos < base) { st->cur_row = st->rows; return; }          /* off-page: hidden  */
    pos -= base;
    if (pos >= (unsigned)st->cols * st->rows) { st->cur_row = st->rows; return; }
    st->cur_row = (uint8_t)(pos / st->cols);
    st->cur_col = (uint8_t)(pos % st->cols);
    vdd_video_bda_sync(st);
}

/* Reassemble the ten-bit Line Compare from its three registers. */
static void crtc_lc_update(video_state *st)
{
    st->crtc_line_compare = (uint16_t)(st->crtc_lc_low
                            | (((uint16_t)(st->crtc_overflow >> 4) & 1u) << 8)
                            | (((uint16_t)(st->crtc_maxscan  >> 6) & 1u) << 9));
}

/* A mode set writes 0x06, 0x12 and 0x15 among the rest; only once all three have
   arrived is the vertical timing a complete statement rather than one register of
   the old mode's geometry beside two of the new one's. */
/* ── ▶ DERIVED ONCE PER CRTC WRITE, NOT ONCE PER 0x3DA READ. ──────────────────────
     The vertical geometry is a function of five registers. Recomputing it inside
     status_in meant reassembling three 10-bit values out of scattered bits, running
     three validity tests and a division ON EVERY POLL -- and a guest polls this port
     harder than it does anything else: Lemmings reads 0x3DA 73.8 MILLION times in a
     45-second run, 1.6M/s, and in its menu phase it does essentially nothing else.
     MEASURED: that put the per-poll cost up from 16.0ns to 20.0ns off-VM, and on the
     rig the guest got through 77.0M polls per run before and 73.6M after -- 4.4% fewer
     in the same wall time, paid by every guest that waits on retrace.
     The inputs change only when the guest writes the CRTC, so the answer is cached
     there and the hot path just reads it. Same numbers, none of the arithmetic. */
static void crtc_vt_recompute(video_state *st)
{
    uint32_t ov, ms, vt, vde, vbs;
    st->vt_valid = 0;
    if (!st->crtc_vt_seen) return;
    ov = st->crtc_overflow; ms = st->crtc_maxscan;
    vt  = (uint32_t)st->crtc_vtotal_lo | ((ov >> 0 & 1u) << 8) | ((ov >> 5 & 1u) << 9);
    vde = (uint32_t)st->crtc_vde_lo    | ((ov >> 1 & 1u) << 8) | ((ov >> 6 & 1u) << 9);
    vbs = (uint32_t)st->crtc_vbs_lo    | ((ov >> 3 & 1u) << 8) | ((ms >> 5 & 1u) << 9);
    vt += 2; vde += 1;
    if (vt < 100u || vt > 1200u) return;
    if (vde == 0u || vde > vt)   return;
    if (vbs < vde || vbs >= vt)  return;
    st->vt_total = (uint16_t)vt;
    st->vt_active = (uint16_t)vde;
    st->vt_blank  = (uint16_t)vbs;
    st->vt_valid  = 1;
}

static void crtc_vt_update(video_state *st)
{
    if (st->crtc_vtotal_lo && st->crtc_vde_lo && st->crtc_vbs_lo) st->crtc_vt_seen = 1;
    crtc_vt_recompute(st);
}

/* ── THE CRT'S VERTICAL GEOMETRY, FROM THE REGISTERS THE GUEST WROTE. ──────────────
     Each of the three quantities is a 10-bit value split across three registers: the
     low byte of its own, plus one bit in Overflow (0x07) and -- for Blank Start only
     -- one in Maximum Scan Line (0x09). That scattering is why it is worth composing
     in one place instead of at each use.

       Vertical Total        0x06 + ov bit0 + ov bit5   (+2 = scanlines per frame)
       Vertical Display End  0x12 + ov bit1 + ov bit6   (+1 = active scanlines)
       Vertical Blank Start  0x15 + ov bit3 + maxscan bit5

   ▶ WHY NOT KEEP THE TWO CONSTANTS. They were right for every mode the BIOS sets
     except 0Fh/10h, and wrong there by 45 lines -- checked against the per-mode CRTC
     tables in vga_defaults.h, which were read back off a real card. A guest that
     programs its own timing (Mode X and friends) was never covered at all.
   ▶ Returns 0 and touches nothing when the guest has not programmed the CRTC, or
     when what it programmed is not a plausible screen: the caller then keeps the old
     constants. An off-VM test that sets gh directly takes this path, which is why
     the existing battery is unaffected. */
/* Read back what crtc_vt_recompute() worked out. The validity rules -- a plausible
   screen: blanking after the picture and inside the frame -- live there, because a
   half-written mode set must be rejected ONCE, not re-rejected 73 million times. */
static int vga_vtiming(const video_state *st, uint32_t *total, uint32_t *active,
                       uint32_t *blank_start)
{
    if (!st->vt_valid) return 0;
    *total = st->vt_total; *active = st->vt_active; *blank_start = st->vt_blank;
    return 1;
}

static void crtc_out(void *self, uint16_t port, uint8_t w, uint32_t v)
{
    video_state *st = (video_state *)self;
    if (port == 0x3D4) { vga_idx_data(&st->crtc_index, w, v, crtc_set_data, st); return; }
    crtc_set_data(st, v);
}
static void crtc_set_data(void *self, uint32_t v)
{
    video_state *st = (video_state *)self;
    switch (st->crtc_index) {
    /* ── THE START ADDRESS IS SIXTEEN BITS WRITTEN AS TWO REGISTERS, so between the
         two writes it holds a value the guest never asked for -- half of the old
         address and half of the new. Real hardware survives that because the address
         counter LOADS FROM THESE REGISTERS AT THE VERTICAL RETRACE, not continuously,
         and a guest that flips pages during retrace is therefore never seen torn.
         We rendered from them directly, so a frame built between the two writes
         showed a garbage address -- a whole-screen flicker on any guest that scrolls
         or page-flips, which is every scrolling game. crtc_start_half counts how
         often a frame was built mid-pair; crtc_start_live is what the renderer uses. */
    case 0x0C: st->crtc_start = (uint16_t)((st->crtc_start & 0x00FF) | ((uint16_t)(v & 0xFF) << 8));
               st->crtc_seen = 1; st->crtc_start_pend ^= 1; st->dirty = 1; break;
    case 0x0D: st->crtc_start = (uint16_t)((st->crtc_start & 0xFF00) | (v & 0xFF));
               st->crtc_seen = 1; st->crtc_start_pend ^= 1;
               if (!st->crtc_start_pend) st->crtc_start_writes++;
               st->dirty = 1; break;
    case 0x13: st->crtc_offset = (uint8_t)v; st->crtc_off_seen = 1;                                   st->dirty = 1; break;
    /* Line Compare, and the two registers that carry its top two bits. */
    case 0x07: st->crtc_overflow = (uint8_t)v; crtc_lc_update(st); crtc_vt_update(st); st->dirty = 1; break;
    case 0x09: st->crtc_maxscan  = (uint8_t)v; crtc_lc_update(st); crtc_vt_update(st); st->dirty = 1; break;
    case 0x18: st->crtc_lc_low   = (uint8_t)v; crtc_lc_update(st); st->dirty = 1; break;
    /* Vertical timing -- see vga_vtiming(). Low bytes only; 0x07/0x09 carry the
       high bits and are latched above for Line Compare already. */
    case 0x06: st->crtc_vtotal_lo = (uint8_t)v; crtc_vt_update(st); st->dirty = 1; break;
    case 0x12: st->crtc_vde_lo    = (uint8_t)v; crtc_vt_update(st); st->dirty = 1; break;
    case 0x15: st->crtc_vbs_lo    = (uint8_t)v; crtc_vt_update(st); st->dirty = 1; break;
    /* ── THE TEXT CURSOR, PROGRAMMED DIRECTLY. ────────────────────────────────────
         0x0A/0x0B are Cursor Start/End (the same CH/CL INT 10h AH=01h takes, bit 5
         of Start = off) and 0x0E/0x0F the cursor's address in character cells from
         the start of video memory. Full-screen editors and every CRT unit (Turbo
         Pascal's, QB's runtime) position the cursor this way instead of through the
         BIOS, and these registers fell into `default:` -- so the cursor sat wherever
         the last INT 10h left it, which in an editor is nowhere near the text. */
    case 0x0A: st->cur_shape = (uint16_t)((st->cur_shape & 0x00FF) | ((uint16_t)(v & 0x3F) << 8));
               st->dirty = 1; vdd_video_bda_sync(st); break;
    case 0x0B: st->cur_shape = (uint16_t)((st->cur_shape & 0xFF00) | (v & 0x1F));
               st->dirty = 1; vdd_video_bda_sync(st); break;
    case 0x0E: st->crtc_cursor = (uint16_t)((st->crtc_cursor & 0x00FF) | ((uint16_t)(v & 0xFF) << 8));
               crtc_cursor_apply(st); break;
    case 0x0F: st->crtc_cursor = (uint16_t)((st->crtc_cursor & 0xFF00) | (v & 0xFF));
               crtc_cursor_apply(st); break;
    default: break;
    }
}
static void crtc_in(void *self, uint16_t port, uint8_t w, uint32_t *v)
{
    video_state *st = (video_state *)self; (void)w;
    if (port == 0x3D4) { *v = st->crtc_index; return; }
    switch (st->crtc_index) {
    case 0x0C: *v = (uint8_t)(st->crtc_start >> 8); break;
    case 0x0D: *v = (uint8_t)(st->crtc_start & 0xFF); break;
    case 0x13: *v = st->crtc_offset; break;
    case 0x0A: *v = (uint8_t)((st->cur_shape >> 8) & 0x3F); break;
    case 0x0B: *v = (uint8_t)(st->cur_shape & 0x1F); break;
    /* Read back what the BIOS path set, in the hardware's own units. */
    case 0x0E: *v = (uint8_t)(crtc_cursor_of(st) >> 8); break;
    case 0x0F: *v = (uint8_t)(crtc_cursor_of(st) & 0xFF); break;
    default:   *v = 0; break;
    }
}

/* ── A 16-BIT `OUT` TO A VGA INDEX PORT WRITES INDEX **AND** DATA. ──────────────────
     The index and data registers of the sequencer, the graphics controller and the CRTC
     are adjacent by design precisely so that one word OUT can set both -- `outpw(0x3C4,
     index | value<<8)` is the idiom every DOS graphics programmer uses, and Watcom
     compiles it to `mov eax,0x102 / out dx,ax`.
     These handlers ignored `w` and treated the whole word as an index, THROWING THE
     DATA BYTE AWAY. Doom's mode-Y frame blit selects each plane with exactly that
     instruction:
         19f8f:  mov edx,0x3c4 / mov eax,0x102 / out dx,ax    ; map mask := plane 0
     so the map mask never changed, the de-interleave saw one plane's bytes where four
     should have been, and every even screen column came out identical to the one after
     it -- measured at 1.000 across whole frames, status bar included. It reads as "Doom
     at half resolution", which is not a thing Doom can do: its low-detail mode leaves
     the status bar alone.
   ► THIS IS ALSO WHY CLAIMING THE CRTC REGRESSED DOOM THREE TIMES (sessions 19-20,
     "mechanism UNKNOWN"). Doom page-flips with `mov edx,0x3d4 / out dx,ax` -- the same
     idiom. Claiming 0x3D4 while dropping the data byte breaks the flip outright, which
     is strictly worse than not claiming it and inferring the page from the data. */
static void seq_set_data(void *self, uint32_t v);

static void seq_out(void *self, uint16_t port, uint8_t w, uint32_t v)
{
    video_state *st = (video_state *)self;
    if (port == 0x3C4) { vga_idx_data(&st->seq_index, w, v, seq_set_data, st); return; }
    seq_set_data(st, v);
}
static void seq_set_data(void *self, uint32_t v)
{
    video_state *st = (video_state *)self;
    if (st->seq_index == 2) {
        /* Which map-mask values does this program actually use, and how often? The
           de-interleave is built entirely on the assumption that an unchained program
           selects ONE plane at a time and changes the mask between planes; nothing has
           ever checked that against a real one. A 16-entry histogram costs nothing and
           turns "the frame comes out doubled" into "plane 1 was never selected". */
        st->mask_hist[v & 0x0F]++;
        /* ► THE PAIR, NOT THE TWO HISTOGRAMS SEPARATELY. "write mode 1 happens 120
             times" and "mask 0x0F happens 44 times" cannot be combined by the reader:
             a latch copy through a SINGLE-plane mask is served correctly by per-plane
             backing, one through an ALL-plane mask is not, and only the pairing says
             which Doom actually does. */
        st->mw_hist[(st->write_mode & 3) * 16 + (v & 0x0F)]++;
        /* A mask change is the moment the outgoing plane's data is complete. */
        /* Flush BEFORE the mask moves: everything written since the last flush
           belongs to the mask that is now going out. With host-supplied per-plane
           backing there is nothing to flush -- the write already went to the right
           plane -- and all that is needed is to point the window at the new one. */
        /* ► ACCOUNT FOR EVERY WRITE THAT DOES NOT REACH ymap_select. 8.6% of a run's
             map-mask writes did not move the window and no counter said why. These two
             are the only ways a write can be dropped here, and a dropped mask change
             strands the window on the plane the PREVIOUS mask chose -- so the next
             store lands in the wrong plane, which is what a four-way collapse is made
             of. `chain4` in particular is a live suspect: the guest may change the mask
             while chained and expect the change to hold once it unchains. */
        if (st->chain4)                                   st->mask_skip_chain4++;
        else if ((uint8_t)(v & 0x0F) == st->y_mask)       st->mask_skip_same++;
        if (!st->chain4) {
            /* ⚠ WITH HOST BACKING, CALL ON EVERY WRITE -- NOT ONLY ON A CHANGE. Once the
                 host follows GR4 (the read plane) the window can have MOVED since the
                 last map-mask write, so "the mask value is unchanged" no longer implies
                 "the window is where the writes need it". The host early-returns when it
                 already is, so the extra calls cost a compare; the alternative is a
                 store landing in the plane the last READ selected.
                 The fallback de-interleave path has no such window and keeps the skip. */
            if (st->ymap_select)                        st->ymap_select(st->ymap_ctx, (int)(v & 0x0F));
            else if ((uint8_t)(v & 0x0F) != st->y_mask) modey_flush(st);
            if ((uint8_t)(v & 0x0F) != st->y_mask)      st->dirty = 1;
        }
        st->map_mask = (uint8_t)(v & 0x0F);
        st->y_mask   = st->map_mask;
    }
    else if (st->seq_index == 4) {                 /* Memory Mode: bit 3 = Chain-4 */
        uint8_t c4 = (uint8_t)((v >> 3) & 1);
        if (c4 != st->chain4) {
            st->chain4 = c4; st->y_mask = st->map_mask;
            st->chain4_sel++;
            if (st->ymap_select) st->ymap_select(st->ymap_ctx, c4 ? -1 : (int)st->map_mask);
            st->dirty = 1;
        }
    }
}
static void seq_in(void *self, uint16_t port, uint8_t w, uint32_t *v)
{
    video_state *st = (video_state *)self; (void)w;
    *v = (port == 0x3C4) ? st->seq_index : (st->seq_index == 2 ? st->map_mask : 0);
}
/* Graphics Controller ports 3CE (index) / 3CF (data). */
static void gc_set_data(void *self, uint32_t v);

static void gc_out(void *self, uint16_t port, uint8_t w, uint32_t v)
{
    video_state *st = (video_state *)self;
    if (port == 0x3CE) { vga_idx_data(&st->gc_index, w, v, gc_set_data, st); return; }
    gc_set_data(st, v);
}
static void gc_set_data(void *self, uint32_t v)
{
    video_state *st = (video_state *)self;
    switch (st->gc_index) {
    case 0: st->set_reset   = (uint8_t)(v & 0x0F); break;
    case 1: st->enable_sr   = (uint8_t)(v & 0x0F); break;
    /* GR2 and GR7 are the two halves of read mode 1 and used to fall into default:,
       i.e. be dropped. See read_mode in the header. */
    case 2: st->col_compare  = (uint8_t)(v & 0x0F); break;
    case 7: st->col_dontcare = (uint8_t)(v & 0x0F); break;
    case 3: st->func_rotate = (uint8_t)(v & 0x1F); break;
    /* ── GR4 IS THE READ PLANE, AND THE REMAP PATH CANNOT SEE READS AT ALL. ──────────
         In the `st->plane[]` interpreter path a guest read is served by us and honours
         this register (see the read-mode-0 return). With host-supplied per-plane backing
         A0000 is a REAL mapped section, so a guest read never reaches this file and
         returns whatever plane the WRITE MASK last selected. Read plane and write plane
         are independent on the hardware, so any guest that sets them apart -- Doom's
         `I_ReadScreen` cycles GR4 with the write mask irrelevant -- gets the wrong bytes,
         SILENTLY. Every exclusion so far in the status-bar hunt has been about writes.
       ► Count the pairing, not the register. `gr4_hist` alone cannot say whether GR4
         ever DISAGREED with the mapped plane, and disagreement is the entire defect;
         the host compares against `g_ycur` in the hook. */
    case 4:
        st->read_map = (uint8_t)(v & 3);
        st->gr4_hist[v & 3]++;
        if (st->ymap_readmap) st->ymap_readmap(st->ymap_ctx, (int)(v & 3));
        break;
    case 5:
        /* ► COUNT THE WRITE MODES. Per-plane backing can only serve write mode 0, where
             a guest store is a plain byte into the selected plane. WRITE MODE 1 is a
             LATCH COPY: reading an address loads all four planes into the VGA's latches
             and the next store writes all four back at once. That is the standard mode-Y
             trick for moving a region inside video memory without touching the CPU bus
             four times -- and with A0000 pointing at ONE plane it collapses, because the
             guest can only read and write the plane that happens to be mapped.
             If a program uses it, the mapping approach cannot serve it and the fact has
             to be visible rather than inferred. */
        st->wmode_hist[v & 3]++;
        st->write_mode  = (uint8_t)(v & 3);
        st->read_mode   = (uint8_t)((v >> 3) & 1);   /* ⚠ bit 3 used to be masked off */
        if (st->ymap_wmode) st->ymap_wmode(st->ymap_ctx, (int)(v & 3));
        break;
    case 8: st->bit_mask    = (uint8_t)v;          break;
    default: break;
    }
}
static void gc_in(void *self, uint16_t port, uint8_t w, uint32_t *v)
{
    video_state *st = (video_state *)self; (void)w;
    if (port == 0x3CE) { *v = st->gc_index; return; }
    switch (st->gc_index) {
    case 0: *v = st->set_reset; break;  case 1: *v = st->enable_sr; break;
    case 2: *v = st->col_compare; break; case 7: *v = st->col_dontcare; break;
    case 3: *v = st->func_rotate; break; case 4: *v = st->read_map; break;
    case 5: *v = (uint8_t)(st->write_mode | (st->read_mode << 3)); break;
    case 8: *v = st->bit_mask; break;
    default: *v = 0; break;
    }
}

/* Input Status Register 1 (3DA/3BA) -- bit 3 = vertical retrace, bit 0 = display
   disabled (set during EITHER horizontal or vertical blanking). A read also resets
   the attribute-controller flip-flop.

   ▶ THIS USED TO TOGGLE BOTH BITS ON EVERY READ. That guaranteed a "wait until set,
     then wait until clear" loop finished within two reads, so no guest could ever
     spin here forever -- the right call when the alternative was a hang, and the
     comment that lived here said plainly "we have no real CRT timing".
     But it also meant the bits had NO RELATIONSHIP TO TIME. `WAIT &H3DA,8` -- which
     is the entire frame clock of a great deal of DOS graphics code -- returned
     immediately, so those programs ran as fast as we could execute them instead of
     at ~60-70 Hz. Measured live on the physical box: BOUNCEBX tore instead of
     animating, MATRIX_2 outran MATRIX_1 (stock ntvdm has that pair the other way
     round), and CAVE ran "way too fast" in SCREEN 13.
   ▶ We DO have a timebase now -- `host_pit_sync()` has derived guest clocks from
     QueryPerformanceCounter since session 11 -- so derive the bits from it.
     `st->time_us` is NULL off-VM by default, which keeps the old toggle for tests
     that do not care about timing.
   ▶ The retrace bit is asserted for the whole VERTICAL BLANKING interval rather
     than just the 2-line sync pulse. A 2-line window is 0.4% of a frame, and a
     guest that does any work between polls would miss it and wait an extra frame;
     the blanking interval (~9%) is the forgiving reading and is what emulators
     conventionally report. */
/* ── WHERE IS THE BEAM? One answer for everyone who asks. ──────────────────────
     status_in derived the frame timing inline; the raster-split bookkeeping needs
     the same numbers at every DAC write, so it lives here. Returns 0 with no clock
     (the off-VM default). `now` is the model's microseconds; `frame_us` the frame
     period; `vtotal`/`vdisp`/`vblank` the line counts (total, display end, blank
     start); `frame_no` = now / frame_us; `line` = the scanline the beam is on. */
static int vid_beam(const video_state *st, uint64_t *now, uint32_t *frame_us,
                    uint32_t *vtotal, uint32_t *vdisp, uint32_t *vblank,
                    uint32_t *frame_no, uint32_t *line)
{
    int tall; uint32_t t, a, b; uint64_t in_frame;
    if (!st->time_us) return 0;
    /* 480-line modes run at 60 Hz, the 200/400-line ones at 70 Hz. Mode 13h is
       320x200 displayed as 400 scanlines, so it belongs with the 70 Hz group -- key
       the choice off the DISPLAYED height, not the mode number. */
    tall    = (st->gh > VID_VACTIVE_LO);
    *vtotal = tall ? VID_VTOTAL_HI  : VID_VTOTAL_LO;
    *vblank = tall ? VID_VACTIVE_HI : VID_VACTIVE_LO;
    *vdisp  = *vblank;
    /* Prefer the geometry the guest programmed. `vblank` is BLANK START, not display
       end -- they differ by 6 lines in the BIOS modes and by 45 in 640x350, and it is
       blanking, not the end of the picture, that raises the status bit. */
    if (vga_vtiming(st, &t, &a, &b)) {
        *vtotal = t; *vdisp = a; *vblank = b;
        /* 449-line modes are the 70 Hz family, 525-line the 60 Hz one. Keyed off the
           measured total rather than the displayed height, so a 350-line mode is no
           longer forced to pick a side of a 400-line fence. */
        tall = (t >= 500u);
    }
    *frame_us = 1000000u / (uint32_t)(tall ? VID_VBL_HZ_HI : VID_VBL_HZ_LO);
    *now      = st->time_us();
    *frame_no = (uint32_t)(*now / *frame_us);
    in_frame  = *now % (uint64_t)*frame_us;
    /* SCALE FIRST, DIVIDE ONCE -- see status_in for why line_us must not be an integer. */
    *line     = (uint32_t)((in_frame * (uint64_t)*vtotal) / *frame_us);
    return 1;
}

static void status_out(void *self, uint16_t port, uint8_t w, uint32_t v)
{ (void)self; (void)port; (void)w; (void)v; }     /* feature ctrl: ignore */
#define VID_HBL_DEBT_MAX 16u   /* lines: further apart than this, a poll is not counting lines */
static void status_in(void *self, uint16_t port, uint8_t w, uint32_t *v)
{
    video_state *st = (video_state *)self; (void)port; (void)w;
    uint64_t now, in_frame;
    /* ⚠ LOAD-BEARING, AND NOT A VBLANK CONCERN. Reading this port resets the
         Attribute Controller's index/data flip-flop, and every guest relies on it
         before touching 0x3C0. See attr_out. */
    st->attr_ff = 0;
    uint32_t frame_us, line, u_vtotal, u_vdisp, u_vblank, frame_no;
    int vtotal, vactive, in_vbl, in_hbl;

    if (!vid_beam(st, &now, &frame_us, &u_vtotal, &u_vdisp, &u_vblank, &frame_no, &line)) {
        st->retrace ^= 0x09;                        /* no clock injected: old behaviour */
        *v = st->retrace;
        return;
    }
    vtotal = (int)u_vtotal; vactive = (int)u_vblank; (void)u_vdisp;
    /* Bracket the polling in the model's own microseconds -- see the header. */
    if (!st->t3da_first) st->t3da_first = now;
    if (st->t3da_last) {
        uint64_t d = now - st->t3da_last;
        st->present_gap_us = (d > 0xFFFFFFFFull) ? 0xFFFFFFFFu : (uint32_t)d;
        if (!d) st->dt3da_zero++;
        else {
            unsigned b = 0;
            uint64_t t = d;
            while (b < 7 && t >= 4) { t >>= 2; b++; }
            st->dt3da_hist[b]++;
            if (d > (uint64_t)st->dt3da_max)
                st->dt3da_max = (d > 0xFFFFFFFFull) ? 0xFFFFFFFFu : (uint32_t)d;
        }
    }
    st->t3da_last = now;
    /* ── SCALE FIRST, DIVIDE ONCE. A scanline is not a whole number of microseconds:
         at 70 Hz it is 14285/449 = 31.8us, and taking `line_us = frame_us / vtotal`
         truncated that to 31. Lines then ran 2.6% fast -- a frame's worth of them
         reached 460 on a 449-line screen, which is what the clamp below was quietly
         absorbing, and it put every line boundary up to 12 lines out by the bottom of
         the picture. Multiplying into the frame before dividing keeps the fraction and
         needs no clamp: `line` cannot exceed vtotal-1 because in_frame < frame_us.
         The remainder is the position WITHIN the line, on the same scale. */
    in_frame = now % (uint64_t)frame_us;
    {   uint64_t pos = in_frame * (uint64_t)vtotal;
        line   = (uint32_t)(pos / frame_us);
        in_hbl = ((uint64_t)(pos % frame_us) * 100u >= (uint64_t)frame_us * VID_HACTIVE_PCT);
    }
    in_vbl = (line >= (uint32_t)vactive);
    /* bit 3 = vertical retrace; bit 0 = display disabled (h- OR v-blank). Bit 0 is a
       DIFFERENT signal on a real card -- it changes per scanline, not per frame -- so
       toggling the two together, as we used to, was doubly wrong.
       The owed-blank rule below is what makes a scanline counter exact. */
    /* ── ★★★★ A BLANK THAT PASSED BETWEEN TWO POLLS HAPPENED, WHETHER OR NOT A SAMPLE
         LANDED IN IT. Bit 0 is what a scanline COUNTER reads: Lemmings' "High
         Performance PC" calibration does `wait while set; wait while clear` 320 times
         against the 8254 and uses the elapsed clocks as its game tick (guest
         CS:1602..1643), and the tick is then what places its raster palette split on
         the screen -- row 160 when the count is exactly 320 lines. On real silicon an
         `in` is a microsecond and every 6.4us hblank is sampled. Here any host stall
         during the ~10 ms window (a lock, a present, a capture) skips whole lines:
         measured reloads 0x2F00..0x38CD across one evening -- 320 to 383 lines -- and
         at 383 the toolbar's top 31 rows were drawn in the level's palette.
       ► So if the guest's previous poll was in an EARLIER line and saw the display
         active, the blanking of that earlier line went unobserved; report it ONCE and
         let the next poll read the true phase. The rule cannot fire within a line (two
         reads at one instant still agree, the property the timed model was built for)
         and does not fire when the guest saw the blank itself, so a fast poller sees
         exactly what it saw before. A poller slower than a whole line cannot count
         lines on real hardware either and is not helped. (s69 shipped this and s69
         reverted it with the PIT change it was bundled with; on its own it was right.) */
    {   uint64_t abs_line = (now / (uint64_t)frame_us) * (uint64_t)vtotal + line;
        int bit0 = (in_vbl || in_hbl);
        /* ► ONE blank owed per straddle, and only for a poll that is plausibly counting
             lines (s69's rule, bounded, s70). Two generalisations were tried and
             measured wrong in video_test: repaying every line crossed as a debt of
             synthetic pulses either swallowed real blanks in the guest's `wait while
             set` phase (a 330us stall came back wholly unrepaid) or, drained the other
             way round, broke the plain case. A two-phase poll loop cannot be fed more
             than one blank per line boundary it crosses. So the residual stands: a
             host stall of N lines inside a count is repaid ONE line; the rest is
             real time the guest lost. With the interpreter port path now syncing the
             PIT (main.c iio_out) the rig measured 320.7..322.7 lines on four runs.
           ► A poll more than VID_HBL_DEBT_MAX lines after the previous one is not a
             line counter (the attribute flip-flop reset before a palette write,
             once a frame) and owes nothing. */
        if (!bit0 && st->p3da_have_last && !st->p3da_last_bit0 && !in_vbl && !st->p3da_last_vbl
            && abs_line > st->p3da_last_line
            && abs_line - st->p3da_last_line <= VID_HBL_DEBT_MAX) {
            bit0 = 1; st->p3da_hbl_owed++;
        }
        st->p3da_last_line = abs_line; st->p3da_last_bit0 = (uint8_t)bit0;
        st->p3da_last_vbl = (uint8_t)in_vbl;
        st->p3da_have_last = 1;
        *v = (uint32_t)((in_vbl ? 0x08u : 0u) | (bit0 ? 0x01u : 0u));
        if (st->p3da_ring_on) {          /* debug only -- see the note in the header */
            st->p3da_ring_us[st->p3da_ring_n & (VID_P3DA_RING - 1)] = (uint32_t)now;
            st->p3da_ring_v [st->p3da_ring_n & (VID_P3DA_RING - 1)] = (uint8_t)*v;
            st->p3da_ring_n++;
        }
    }
    st->retrace = (uint8_t)*v;                      /* keep it observable in dumps    */
    /* Count the edge the GUEST sees, not the one the model produces: a clear->set
       transition between two of its own reads is exactly one completed
       `WAIT &H3DA,8`, so edges/second IS the guest's frame rate. */
    st->p3da_reads++;
    if (in_vbl && !st->vbl_prev) st->vbl_edges++;
    st->vbl_prev = (uint8_t)in_vbl;
    /* ── RAISE THE PRESENT FROM THE GUEST'S FRAME (see present_hook in the header).
         Same window as vdd_video_present_ready -- the last VID_PRESENT_WINDOW_PM of
         the frame before blanking, when a retrace-paced guest has finished drawing
         and is parked here polling -- expressed in lines so no second clock read is
         paid on a path that runs millions of times a second. A poller that skips
         the window (one read per frame, at the edge) gets the edge instead: the
         frame it drew is complete by then too. Once per frame_no either way. */
    if (st->present_hook && st->present_frame != frame_no) {
        uint32_t win_lines = (uint32_t)vtotal * VID_PRESENT_WINDOW_PM / 1000u;
        /* ► THE FIRST POLL AFTER A GAP IS THE BEST MOMENT OF ALL. A retrace-paced
             guest leaves this port to DRAW and comes back to wait: the first read
             after it was away (>= VID_PRESENT_GAP_US) means the frame is complete
             and the guest is parked -- with most of the frame still ahead for the
             render, which blocks its polls while it runs. Firing in the window
             instead (first cut) put that render right before the retrace the guest
             was waiting for, and it missed one frame in twenty (BOUNCEBX 1798 ->
             1697 edges, dtmax 2.4 -> 13.5 ms). The window and the edge remain for
             a guest that never leaves the port. */
        int gap = st->present_gap_us >= VID_PRESENT_GAP_US;
        if (gap || in_vbl || (line + win_lines >= (uint32_t)vactive)) {
            st->present_frame = frame_no;
            st->present_hook_fires++;
            if (gap) st->present_hook_gap++;
            st->present_hook(st->present_ctx);
        }
    }
    st->present_gap_us = 0;
}

/* Copy both character generators into guest-visible memory so the pointer handed out by
   INT 10h AH=11h AL=30h resolves to real glyph data. The 8x8 table is DERIVED from the 8x16
   one -- each output row is the OR of the two rows it replaces, which keeps thin horizontal
   strokes that plain decimation would drop. It is a faithful-enough 8x8, not the authentic
   IBM ROM design; if a game's text ever looks subtly wrong in shape rather than garbled,
   this is the thing to replace with real 8x8 glyph data. */
void vdd_video_install_fonts(video_state *st)
{
    uint8_t *f16, *f8, *f14;
    unsigned c, y;
    if (!st || !st->bus) return;           /* called before the VDD joined the bus */
    f16 = (uint8_t *)vdd_map_flat(st->bus, VDD_FONT8X16_SEG, 0);
    f8  = (uint8_t *)vdd_map_flat(st->bus, VDD_FONT8X8_SEG, 0);
    f14 = (uint8_t *)vdd_map_flat(st->bus, VDD_FONT8X14_SEG, 0);
    if (!f16 || !f8 || !f14) return;
    /* All three are the REAL ROM tables now. The 8x8 used to be manufactured here by
       OR-ing adjacent row pairs of the 8x16 -- which squashes a 16-row glyph into 6 and
       fills in every counter, so 'A' came out solid and 'E' came out as noise. Skyroads
       asks for this exact table (BH=3 and BH=4, measured) and draws its own text from the
       pointer we return, so that hack WAS the game's garbled text. Never derive a font. */
    for (c = 0; c < 256; ++c) {
        for (y = 0; y < 16; ++y) f16[c * 16 + y] = vga_font_8x16[c][y];
        for (y = 0; y < 8;  ++y) f8 [c * 8  + y] = vga_font_8x8 [c][y];
        for (y = 0; y < 14; ++y) f14[c * 14 + y] = vga_font_8x14[c][y];
    }
}

/* B8000 window hook (for the off-VM test; the live host maps the aperture RAM
   so direct writes never trap -- the renderer just reads vmem each frame). */
static uint8_t vid_rd(void *self, uint32_t off)
{ video_state *st = (video_state *)self; return st->vmem[VID_TEXT_OFF + off]; }
static void vid_wr(void *self, uint32_t off, uint8_t v)
{ video_state *st = (video_state *)self; st->vmem[VID_TEXT_OFF + off] = v; st->dirty = 1; }

/* One character's glyph rows: the loaded user font wins (GH #52), otherwise the ROM
   table for the cell height in force -- 8x8 after a 1112h, 8x14 after 1111h, 8x16
   otherwise. One predictable branch in the ordinary case. */
static const uint8_t *glyph_rows(const video_state *st, uint8_t ch)
{
    if (st->user_font_on)  return &st->user_font[ch * VID_CELL_H];
    if (st->cell_h == 8)   return vga_font_8x8[ch];
    if (st->cell_h == 14)  return vga_font_8x14[ch];
    return vga_font_8x16[ch];
}

/* ── ONE CELL. The attribute byte's top bit is BLINK OR BRIGHT BACKGROUND, and the
     Attribute Controller (AR10 bit 3, INT 10h AX=1003h) decides which. Blink is the
     power-on default. We always masked the bit off -- `(attr >> 4) & 7` -- so a
     program that turned blink OFF to get sixteen background colours (every text-mode
     UI with a light-grey dialog on a bright panel) got the dark eight, and one that
     left blink ON and used it never blinked. `st->blink_off` is the phase for this
     render, set once per frame by vdd_video_render from the injected clock. */
static void render_cell(video_state *st, int r, int c, uint8_t ch, uint8_t attr)
{
    int gy, gx;
    int cell_h = st->cell_h ? st->cell_h : VID_CELL_H;
    int stride = st->cols * VID_CELL_W;                /* 320 in a 40-column mode  */
    uint8_t fg = attr & 0x0F, bg;
    const uint8_t *gl = glyph_rows(st, ch);
    if (st->blink) {
        bg = (uint8_t)((attr >> 4) & 0x07);
        if ((attr & 0x80) && st->blink_off) fg = bg;  /* off phase: the glyph hides */
    } else {
        bg = (uint8_t)((attr >> 4) & 0x0F);           /* sixteen backgrounds      */
    }
    for (gy = 0; gy < cell_h; ++gy) {
        uint8_t bits = gl[gy];
        uint8_t *row = &st->fb[(r*cell_h + gy) * stride + c*VID_CELL_W];
        for (gx = 0; gx < VID_CELL_W; ++gx) row[gx] = (bits & (0x80 >> gx)) ? fg : bg;
    }
}

static void draw_hw_cursor(video_state *st);

/* ── THE TEXT SCREEN AS THE GUEST WROTE IT, not as we drew it. ───────────────────
     An instrument for exactly one question, and it is a question screenshots cannot
     answer: when something is missing from the display, did the guest never PUT it
     there, or did we never DRAW it? Chasing QBasic's empty file list from captured
     images cost three wrong guesses -- blink, the search API, the DTA names -- two
     of which were about the picture rather than the data.
     Rows of characters, then the attribute of each cell in hex, straight out of the
     text VRAM the renderer itself reads. */
int vdd_video_text_snapshot(video_state *st, char *out, int cap)
{
    static const char hexd[] = "0123456789abcdef";
    int r, c, n = 0;
    if (!out || cap < 16) return 0;
    for (r = 0; r < st->rows; ++r) {
        for (c = 0; c < st->cols && n < cap - 2; ++c) {
            uint8_t ch = cell(st, r, c)[0];
            out[n++] = (ch >= 32 && ch < 127) ? (char)ch : '.';
        }
        if (n < cap - 2) out[n++] = '\n';
    }
    if (n < cap - 8) { const char *h = "--attr--\n"; while (*h && n < cap - 2) out[n++] = *h++; }
    for (r = 0; r < st->rows; ++r) {
        for (c = 0; c < st->cols && n < cap - 3; ++c) {
            uint8_t a = cell(st, r, c)[1];
            out[n++] = hexd[(a >> 4) & 0xF]; out[n++] = hexd[a & 0xF];
        }
        if (n < cap - 2) out[n++] = '\n';
    }
    out[n] = 0;
    return n;
}

void vdd_video_render(video_state *st)                 /* text glyph render        */
{
    int r, c;
    /* Text blinks at half the cursor rate: 32 frames on, 32 off at 60 Hz. */
    st->blink_off = (uint8_t)((st->blink && st->time_us)
                              ? ((st->time_us() % 1066000u) >= 533000u) : 0);
    for (r = 0; r < st->rows; ++r)
        for (c = 0; c < st->cols; ++c) {
            uint8_t *p = cell(st, r, c);
            render_cell(st, r, c, p[0], p[1]);
        }
    draw_hw_cursor(st);
}

void vdd_video_text_cursor(video_state *st, int col, int row,
                           uint16_t and_mask, uint16_t xor_mask)
{
    uint8_t *p, ch, attr;
    if (st->mkind != VID_KIND_TEXT) return;
    if (col < 0 || row < 0 || col >= st->cols || row >= st->rows) return;
    p    = cell(st, row, col);
    ch   = (uint8_t)((p[0] & (and_mask & 0xFF)) ^ (xor_mask & 0xFF));
    attr = (uint8_t)((p[1] & (and_mask >> 8)) ^ (xor_mask >> 8));
    render_cell(st, row, col, ch, attr);
    /* The hardware cursor is drawn by the CRTC over whatever the cell holds, so it
       stays on top of the pointer when the two share a cell. */
    if (row == st->cur_row && col == st->cur_col) draw_hw_cursor(st);
}

static void draw_hw_cursor(video_state *st)
{
    int gy, gx;
    int cell_h = st->cell_h ? st->cell_h : VID_CELL_H;
    int stride = st->cols * VID_CELL_W;
    /* ── THE TEXT CURSOR: SHAPE FROM THE GUEST, SCALED, BLINK FROM THE CLOCK. ────
         This used to be two hard-coded scan lines, always lit. Two things were wrong
         with that and only one of them is cosmetic:
           * A REAL CURSOR BLINKS. On VGA the CRTC blinks it at the vertical rate
             divided by 32 -- 16 frames lit, 16 dark, about 1.9 Hz. A steady block is
             the one thing every DOS user would notice instantly.
           * THE GUEST CHOOSES THE SHAPE, and says so in INT 10h AH=01h CX: CH is the
             first scan line, CL the last. It is also how a program HIDES the cursor
             -- bit 5 of CH, or a start line past the end -- so ignoring CX means a
             full-screen editor that turned the cursor off gets one anyway, now
             blinking at it. Honouring the shape and honouring the hide are the same
             piece of code, which is why they arrive together.
         The phase comes from st->time_us, the injected clock the CRT timebase already
         uses, so this stays pure C and off-VM testable: with no clock injected the
         cursor is simply steady, which is what the existing battery expects. */
    if (st->cur_row < st->rows && st->cur_col < st->cols) {
        unsigned start, end;
        int hidden, lit = 1;
        vdd_cursor_lines(st->cur_shape, (unsigned)cell_h, &start, &end, &hidden);
        if (st->cursor_blink && st->time_us) {
            /* 16 frames on / 16 off at 60 Hz = a 533 ms period, lit for the first
               half. Integer maths only; no floating point in a VDD. */
            uint64_t ph = st->time_us() % 533000u;
            lit = (ph < 266500u);
        }
        if (!hidden && lit) {
            uint8_t fg = cell(st, st->cur_row, st->cur_col)[1] & 0x0F;
            for (gy = (int)start; gy <= (int)end; ++gy)
                for (gx = 0; gx < VID_CELL_W; ++gx)
                    st->fb[(st->cur_row*cell_h + gy) * stride
                           + st->cur_col*VID_CELL_W + gx] = fg;
        }
    }
}

/* ── ★ CURSOR EMULATION. See the note in vdd_video.h for WHY. ────────────────────
     The rule is the VGA BIOS's own (IBM's, as carried by Bochs/SeaBIOS), reproduced
     rather than approximated -- an approximation here shows up as a cursor a pixel
     or two out of place on every DOS prompt in existence:

         CH &= 0x3f;  CL &= 0x1f;
         if (cell > 8 && CL < 8 && CH < 0x20) {
             CH = (CL == CH + 1) ? ((CL + 1) * cell / 8) - 2
                                 : ((CH + 1) * cell / 8) - 1;
             CL = ((CL + 1) * cell / 8) - 1;
         }

     ⚠ THE `CL == CH + 1` BRANCH IS THE ONE THAT MATTERS and it looks like a special
       case for nothing. It is not: a two-line cursor (6-7) is DOS's UNDERLINE, and
       scaling both ends the ordinary way would give 13-15, a three-line smear. The
       branch keeps it two lines tall at the bottom of the cell.
     ⚠ `CL < 8` is what stops a shape that ALREADY knows about 16-line cells from
       being scaled twice; `CH < 0x20` leaves the hide bit alone. */
void vdd_cursor_lines(uint16_t shape, unsigned cell_h,
                      unsigned *start, unsigned *end, int *hidden)
{
    unsigned ch = (shape >> 8) & 0x3Fu;
    unsigned cl =  shape       & 0x1Fu;
    *hidden = (ch & 0x20u) != 0;                 /* CH bit 5: cursor off        */
    ch &= 0x1Fu;
    if (cell_h > 8u && cl < 8u && !*hidden) {
        ch = (cl == ch + 1u) ? ((cl + 1u) * cell_h / 8u) - 2u
                             : ((ch + 1u) * cell_h / 8u) - 1u;
        cl = ((cl + 1u) * cell_h / 8u) - 1u;
    }
    if (ch >= cell_h) ch = cell_h - 1u;
    if (cl >= cell_h) cl = cell_h - 1u;
    if (ch > cl) *hidden = 1;                    /* the other "off" idiom       */
    *start = ch; *end = cl;
}

/* combine the 4 bit-planes into fb (16-colour indices) -- mode 12h. */
/* CGA modes 4/5/6 at B800.  The layout is the reason these were left out before:
   rows INTERLEAVE between two 8 KB banks -- even rows from offset 0, odd rows
   from 0x2000 -- and pixels are 2 bits (modes 4/5) or 1 bit (mode 6), packed
   high-bit-first.  Nothing about that is shared with the planar path, which is
   why approximating it with a text screen was never going to work. */
static void render_cga(video_state *st)
{
    const uint8_t *src = st->vmem + VID_TEXT_OFF;
    int gw = st->gw, gh = st->gh, y, x;
    int per = st->cga_bpp == 1 ? 8 : 4;             /* pixels per byte          */
    /* Mode 5's palette is the grey/brown variant; 4's default is cyan/magenta. */
    static const uint8_t pal4[2][4] = { { 0, 11, 13, 15 }, { 0, 10, 12, 14 } };
    for (y = 0; y < gh; ++y) {
        const uint8_t *row = src + ((y & 1) ? 0x2000 : 0) + (y >> 1) * (gw / per);
        uint8_t *out = &st->fb[y * gw];
        for (x = 0; x < gw; ++x) {
            uint8_t b = row[x / per];
            if (st->cga_bpp == 1)
                out[x] = (uint8_t)((b >> (7 - (x & 7))) & 1 ? 15 : 0);
            else
                out[x] = pal4[st->cga_pal & 1][(b >> (6 - 2 * (x & 3))) & 3];
        }
    }
}

/* ── ★★★ THE PLANAR RENDERER HAS TO READ THE CRTC. (s64) ─────────────────────────
     This drew every frame from address 0 with a stride of gw/8, which is correct for
     exactly one thing: a stationary full-screen picture. That is what the test card
     draws, what the mode-12h demos draw, and what QBasic's BUBBLES draws -- so the
     whole planar suite passed while Lemmings' GAMEPLAY was garbled, because Lemmings
     SCROLLS. Measured on the rig, mid-level: `crtc_seen=01 crtc_start=0x000002c2`.
     The game had panned 706 bytes into the buffer and we were still rendering from
     the top of it.
   ⚠ ANIMATION IS NOT SCROLLING, and that distinction is why every other guest missed
     this. BUBBLES animates by REDRAWING PIXELS -- its start address never leaves 0.
     A scrolling game leaves the pixels alone and moves the WINDOW over them. Nothing
     but Lemmings, in everything tested, does the second.
   ★ render_modey() has honoured both registers for ages -- Doom needed the start
     address to page-flip. The capability existed in one renderer and not its sibling,
     purely because nothing had yet asked the sibling for it.
   ⚠ THE OFFSET REGISTER COUNTS IN 2-BYTE UNITS, and its reset value of 40 is right
     for 12h and wrong for 0Dh -- hence crtc_off_seen: use it only when the guest has
     actually written it, and fall back to the mode's natural stride otherwise.
   ⚠ Both values come from a guest register, so every plane index is wrapped into the
     plane rather than trusted: a mid-scroll write must not read off the end. */
static void render_planar(video_state *st)
{
    int y, xb, b;
    int gw = st->gw ? st->gw : VID_G12_W;
    int gh = st->gh ? st->gh : VID_G12_H;
    uint32_t bytes = (uint32_t)(gw / 8);
    uint32_t pitch = (st->crtc_off_seen && st->crtc_offset)
                     ? (uint32_t)st->crtc_offset * 2u : bytes;
    /* The LATCHED start address, not the register pair -- see crtc_out case 0x0C. */
    uint32_t base  = st->crtc_seen ? (uint32_t)st->crtc_start_live : 0u;
    /* ── SPLIT SCREEN. Below Line Compare the address generator restarts at 0, which
         is how a scrolling game pins a status panel to the bottom of the screen while
         the level pans behind it. Lemmings does exactly this, and without it the panel
         is drawn from the scrolled address -- the striped band under an otherwise
         correct level.
       ⚠ LINE COMPARE COUNTS SCANLINES, NOT OUR ROWS. A 200-line mode is displayed as
         400 scanlines (double-scanned), so a value that looks past the bottom of the
         picture is really in the doubled space -- halve it rather than ignoring it.
         A value that is still past the end after that means "no split", which is the
         power-on state (all ones) and must stay inert. */
    uint32_t split = (uint32_t)gh;                 /* gh = no split */
    if (st->crtc_line_compare) {
        uint32_t lc = st->crtc_line_compare;
        if (lc >= (uint32_t)gh && (lc / 2u) < (uint32_t)gh) lc /= 2u;
        if (lc < (uint32_t)gh) split = lc;
    }
    if (!pitch) pitch = bytes;
    for (y = 0; y < gh; ++y) {
        uint32_t row = (y < (int)split) ? base + (uint32_t)y * pitch
                                        : (uint32_t)(y - (int)split) * pitch;
        uint8_t *out = &st->fb[y * gw];
        for (xb = 0; xb < (int)bytes; ++xb) {
            uint32_t o = (row + (uint32_t)xb) % (uint32_t)VID_PLANE_SIZE;
            uint8_t p0 = st->plane[0][o], p1 = st->plane[1][o];
            uint8_t p2 = st->plane[2][o], p3 = st->plane[3][o];
            for (b = 0; b < 8; ++b) {
                uint8_t m = (uint8_t)(0x80 >> b);
                out[xb*8 + b] = (uint8_t)(((p0&m)?1:0) | ((p1&m)?2:0) | ((p2&m)?4:0) | ((p3&m)?8:0));
            }
        }
    }
}

/* Render the current mode into st->frame each tick (always, so direct A0000
   writes show and the client stays refreshed). Does NOT blit -- the host presents
   st->frame outside the bus lock so the slow blit never starves the V86 thread. */
/* Combine the snapshotted planes. pitch/start come from the CRTC in 2-byte units,
   which is how a mode-Y program page-flips. Masked so a mid-flip value cannot
   index outside the plane. */
/* WHICH PAGE IS ON SCREEN, without the CRTC. A mode-Y program page-flips by
   pointing the CRTC start at one of the 16000-byte pages, and we cannot watch that
   register (above). But the pages are in our snapshot, so pick the one that
   actually holds a picture: count non-zero bytes per page in plane 0 and take the
   busiest. Exact for a title/menu screen, which is what this is for; a game
   double-buffering two equally-busy pages may pick either, and that is a known
   limit rather than a surprise. */
static uint32_t modey_page(const video_state *st)
{
    /* ► PAGES ARE 0x4000 APART, NOT 16000. A 320x200 mode-Y page OCCUPIES 16000
         bytes per plane, but programs align the pages to 0x4000 so the page
         address is a shift rather than a multiply -- Doom's pagestart[] is
         0, 0x4000, 0x8000. Detecting on a 16000 stride put the start 384 bytes
         (16384-16000) below the real page base, which is 4.8 rows: the frame came
         out VERTICALLY ROTATED by ~5 rows, with the bottom of the picture stitched
         onto the top. Measured -- the largest row-to-row discontinuity in the
         captured frame sits at y=5. */
    uint32_t page = 0, bestn = 0, pg;
    for (pg = 0; pg < 4; ++pg) {
        uint32_t base = pg * 0x4000u, i, n = 0;
        if (base + 16000u > VID_Y_PLANE) break;
        for (i = 0; i < 16000u; i += 8) if (st->yplane[0][base + i]) ++n;
        if (n > bestn) { bestn = n; page = pg; }
    }
    return page * 0x4000u;
}

static void render_modey(video_state *st)
{
    uint32_t pitch = (uint32_t)(st->crtc_offset ? st->crtc_offset : 40) * 2u;
    /* ► READ THE PAGE FLIP; DO NOT GUESS IT. modey_page() picks the busiest page out
         of the snapshot, and its own commentary admits the limit: "a game
         double-buffering two equally-busy pages may pick either". Doom double-buffers
         every frame, so the guess alternated and the picture came out streaked with
         bands of the other buffer. It only ever existed because we could not watch the
         register -- claiming 0x3D4 regressed Doom three times for reasons recorded as
         UNKNOWN. The reason was that Doom flips with ONE 16-BIT WRITE,
             19fd4: mov edx,0x3d4 / add eax,0xc / out dx,ax
         and the handler took the whole word as an index and dropped the data, so
         claiming the port broke the flip outright -- strictly worse than not claiming
         it. With index+data writes honoured, the register is the answer. */
    uint32_t start = st->crtc_seen ? st->crtc_start : modey_page(st);
    /* ► THE SELECTED PLANE IS READ LIVE. Its most recent bytes are in the aperture
         and nowhere else -- once a static screen stops changing the mask, no further
         flush ever comes, and that plane's columns would render as whatever was last
         snapshotted. Only a SINGLE-plane mask can be attributed this way; with more
         bits set the aperture belongs to no one plane, so fall back to the snapshots. */
    /* Flush first, then render from the planes ONLY. There is no live-aperture read
       any more: under box updates the aperture is a mixture of whichever planes were
       written most recently, so reading it for the selected plane pulls in another
       plane's pixels -- which is the same error as the whole-aperture copy, wearing a
       different hat. modey_flush() has already moved everything that was written. */
    if (!st->ymap_plane) modey_flush(st);
    { int y, x2;
      const uint8_t *pl[4];
      for (x2 = 0; x2 < 4; ++x2)
          pl[x2] = st->ymap_plane ? st->ymap_plane(st->ymap_ctx, x2) : st->yplane[x2];
      for (y = 0; y < st->gh; ++y) {
          uint32_t row = start + (uint32_t)y * pitch;
          uint8_t *dst = st->fb + (uint32_t)y * st->gw;
          for (x2 = 0; x2 < st->gw; ++x2)
              dst[x2] = pl[x2 & 3][(row + ((uint32_t)x2 >> 2)) & (VID_Y_PLANE - 1u)];
      } }
}

static void vid_frame(void *self)
{
    video_state *st = (video_state *)self;
    if (!st->vmem) return;
    /* ── THE START ADDRESS IS LATCHED ONCE PER FRAME, as the hardware's address
         counter loads it at the vertical retrace. Rendering straight from the
         register pair means a frame built between the two byte writes of a page flip
         shows half the old address and half the new. crtc_start_half counts the
         frames latched mid-pair -- real hardware tears there too, so this is a
         diagnostic and not a guarantee; a guest avoids it by writing both halves
         inside the retrace.
       ⚠ MEASURED ON LEMMINGS: 941 completed pairs, crtc_start_half = 0. So this is
         NOT the cause of the flicker it was written to explain. Kept because it is
         what the hardware does and the hazard is real for other guests. */
    st->crtc_start_live = (uint16_t)st->crtc_start;
    if (st->crtc_start_pend) st->crtc_start_half++;
    if (st->in_vesa) {                                 /* VESA: sync window -> vram */
        vesa_sync(st);
        st->frame.w = st->vesa_w; st->frame.h = st->vesa_h;
        if (st->vesa_bpp > 8) {                        /* direct colour -> ARGB */
            vesa_scan_written(st);
            vesa_to_argb(st);
            st->frame.bpp = 32;
            st->frame.stride = (uint32_t)st->vesa_w * 4;
            st->frame.pixels = (const uint8_t *)st->vesa_argb;
            st->frame.palette = 0;                     /* contract: NULL unless bpp==8 */
        } else {
            st->frame.bpp = 8;
            /* ⚠ THE STRIDE IS THE MODE'S PITCH, NOT ITS WIDTH. Equal for 8bpp, and
                 that equality is why this read `= st->vesa_w` and nobody noticed.
                 And the pitch is the 4F06 LOGICAL one, the origin the 4F07 start:
                 a page flip is nothing more than this pointer moving. */
            st->frame.stride = st->vesa_stride ? st->vesa_stride : st->vesa_w;
            st->frame.pixels = st->vesa_vram + vesa_origin(st); st->frame.palette = st->pal;
        }
    } else if (st->mkind == VID_KIND_LINEAR8 && !st->chain4) {  /* mode Y */
        /* ⚠ NO SNAPSHOT HERE. It used to capture the "live" plane at present time,
             but present time is an ARBITRARY moment: it can land mid-write, and
             which planes get overwritten then depends purely on timing. That made
             the picture NON-DETERMINISTIC -- the same binary produced a perfect
             title screen on one run and a coarse, blocky one on the next (observed
             directly on the physical screen; my own analysis missed it because I
             only ever inspected the richest captured frame, which hid the bad runs).
             The mask-change snapshot is well defined -- the outgoing plane is
             complete by then -- so rely on that alone. */
        render_modey(st);
        st->frame.w = st->gw; st->frame.h = st->gh; st->frame.bpp = 8;
        st->frame.stride = st->gw; st->frame.pixels = st->fb; st->frame.palette = st->pal;
    } else if (st->mkind == VID_KIND_LINEAR8) {        /* graphics: vmem is the FB */
        st->frame.w = st->gw; st->frame.h = st->gh; st->frame.bpp = 8;
        st->frame.stride = st->gw; st->frame.pixels = st->vmem; st->frame.palette = st->pal;
    } else if (st->mkind == VID_KIND_CGA) {            /* CGA: de-interleave -> fb */
        render_cga(st);
        st->frame.w = st->gw; st->frame.h = st->gh; st->frame.bpp = 8;
        st->frame.stride = st->gw; st->frame.pixels = st->fb; st->frame.palette = st->pal;
    } else if (st->mkind == VID_KIND_PLANAR) {         /* planar: combine -> fb    */
        render_planar(st);
        st->frame.w = st->gw; st->frame.h = st->gh; st->frame.bpp = 8;
        st->frame.stride = st->gw; st->frame.pixels = st->fb; st->frame.palette = st->pal;
    } else {                                           /* text: render glyphs      */
        /* Geometry now follows the MODE, not a fixed 80x25 -- a 40-column mode
           renders 320 pixels wide instead of pretending to be 640. */
        vdd_video_render(st);
        st->frame.w = (uint16_t)(st->cols * VID_CELL_W);
        st->frame.h = (uint16_t)(st->rows * (st->cell_h ? st->cell_h : VID_CELL_H));
        vdd_video_bda_sync(st);                        /* cursor moved by teletype etc. */
        st->frame.bpp = 8;
        st->frame.stride = st->frame.w;
        st->frame.pixels = st->fb; st->frame.palette = st->pal;
    }
    st->dirty = 0;
}

void vdd_video_putc(video_state *st, uint8_t ch) { teletype(st, ch); st->dirty = 1; }

void vdd_video_reset(void *self)
{
    video_state *st = (video_state *)self;
    st->mode = 3; st->cols = VID_COLS; st->rows = VID_ROWS;
    st->mkind = VID_KIND_TEXT; st->gw = VID_FB_W; st->gh = VID_FB_H;
    st->cell_h = VID_CELL_H; st->blink = 1; st->user_font_on = 0;
    st->attr_mode = (uint8_t)(st->attr_mode | 0x08u);
    st->crtc_cursor = 0;
    st->mode_qn = 0;
    st->cur_row = st->cur_col = 0; st->cur_shape = 0x0607; st->page = 0;
    st->dac_widx = st->dac_ridx = st->dac_comp = 0;
    st->seq_index = st->gc_index = 0;
    st->map_mask = 0x0F; st->bit_mask = 0xFF; st->write_mode = 0;
    st->chain4 = 1; st->y_mask = 0x0F;
    load_default_crtc(st);                      /* mode 3's CRTC, measured not assumed */
    st->set_reset = st->enable_sr = st->func_rotate = st->read_map = 0;
    st->read_mode = st->col_compare = st->col_dontcare = 0;
    st->latch[0] = st->latch[1] = st->latch[2] = st->latch[3] = 0;
    st->in_vesa = 0; st->vesa_mode = 0; st->vesa_bank = 0;
    load_default_palette(st);
    if (st->vmem) clear_text(st, 0x07);
    vdd_video_bda_sync(st);
    st->dirty = 1;
}

int vdd_video_init(vdd_bus *b, void *self)
{
    video_state *st = (video_state *)self;
    st->bus = b;
    /* Disarmed before the reset, so no offset a guest can produce matches. */
    st->watch_off = 0xFFFFFFFFu;
    vdd_video_reset(st);
    st->modey_gap = MODEY_GAP_DEFAULT;
    if (vdd_claim_mem(b, VID_TEXT_BASE, 0x8000, vid_rd, vid_wr, st)) return -1;
    if (vdd_claim_int(b, 0x10, int10, st)) return -1;
    if (vdd_claim_ports(b, 0x3C4, 0x3C5, seq_in, seq_out, st)) return -1;  /* Sequencer */
    /* ⚠ THE OLD WARNING HERE ("DO NOT CLAIM CRTC 0x3D4/0x3D5", three regressions,
         mechanism UNKNOWN) IS RESOLVED, not ignored. The mechanism was that Doom
         page-flips with ONE 16-BIT WRITE and these handlers dropped the data byte, so
         claiming the port broke the flip outright -- worse than not claiming it. See
         seq_out()/vga_idx_data(). */
    if (vdd_claim_ports(b, 0x3C0, 0x3C1, attr_in, attr_out, st)) return -1; /* Attribute */
    if (vdd_claim_ports(b, 0x3C7, 0x3C9, dac_in, dac_out, st)) return -1;  /* DAC       */
    if (vdd_claim_ports(b, 0x3CE, 0x3CF, gc_in, gc_out, st)) return -1;    /* Graphics  */
    /* CRTC. Claimed at last -- see render_modey() for why three earlier attempts
       regressed Doom and why that cause is gone. */
    if (vdd_claim_ports(b, 0x3D4, 0x3D5, crtc_in, crtc_out, st)) return -1; /* CRTC     */
    if (vdd_claim_ports(b, 0x3DA, 0x3DA, status_in, status_out, st)) return -1; /* InpStatus1 */
    if (vdd_on_frame(b, vid_frame, st)) return -1;
    return 0;
}

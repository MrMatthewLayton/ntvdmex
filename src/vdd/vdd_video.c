/* vdd_video.c -- see vdd_video.h.  Text mode 3 + graphics mode 13h over the
 * shared video aperture (vmem), with the DAC palette, on the VDD bus.  Pure C. */
#include "vdd_video.h"
#include "vga_font_8x16.h"
#include "vga_font_8x8.h"
#include "vga_font_8x14.h"

/* 16-colour EGA/CGA text palette as 0xAARRGGBB. */
static const uint32_t ega16[16] = {
    0xFF000000, 0xFF0000AA, 0xFF00AA00, 0xFF00AAAA,
    0xFFAA0000, 0xFFAA00AA, 0xFFAA5500, 0xFFAAAAAA,
    0xFF555555, 0xFF5555FF, 0xFF55FF55, 0xFF55FFFF,
    0xFFFF5555, 0xFFFF55FF, 0xFFFFFF55, 0xFFFFFFFF
};

/* DAC component (0..63) -> 8-bit; pack/unpack a palette entry. */
static uint32_t dac_pack(uint8_t r, uint8_t g, uint8_t b)
{ return 0xFF000000u | ((uint32_t)(r << 2) << 16) | ((uint32_t)(g << 2) << 8) | (uint32_t)(b << 2); }

/* Load the default DAC palette: 16 EGA colours in 0..15, a 240-entry grey ramp
   above.  Real VGA reloads this on every INT 10h AH=00 mode set; without it a
   graphics program that reprogrammed the DAC (e.g. SCREEN 13) leaves later text
   drawn in *its* palette -- usually near-black, so text mode looks dead. */
static void load_default_palette(video_state *st)
{
    int i;
    for (i = 0; i < 16; ++i)   st->pal[i] = ega16[i];
    for (i = 16; i < 256; ++i) st->pal[i] = 0xFF000000u | (uint32_t)(i * 0x010101u);
}

static uint8_t *cell(video_state *st, int r, int c)   /* -> char byte of (r,c)    */
{ return st->vmem + VID_TEXT_OFF + (r * st->cols + c) * 2; }

static void clear_text(video_state *st, uint8_t attr)
{
    int n = st->cols * st->rows, i;
    uint8_t *t = st->vmem + VID_TEXT_OFF;
    for (i = 0; i < n; ++i) { t[i*2] = ' '; t[i*2+1] = attr; }
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
static const struct { uint16_t num, w, h; } vesa_modes[] = {
    { 0x100, 640, 400 }, { 0x101, 640, 480 }, { 0x103, 800, 600 },
};
static int vesa_find(uint16_t num, uint16_t *w, uint16_t *h)
{
    unsigned i;
    for (i = 0; i < sizeof(vesa_modes)/sizeof(vesa_modes[0]); ++i)
        if (vesa_modes[i].num == (num & 0x3FFF)) { *w = vesa_modes[i].w; *h = vesa_modes[i].h; return 1; }
    return 0;
}
static void wr16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void wr32(uint8_t *p, uint32_t v) { p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); p[2]=(uint8_t)(v>>16); p[3]=(uint8_t)(v>>24); }

/* sync the live A0000 window into vesa_vram[current bank]. */
static void vesa_sync(video_state *st)
{
    uint32_t off = (uint32_t)st->vesa_bank * VID_VESA_WIN; unsigned i;
    if (off + VID_VESA_WIN > VID_VESA_VRAM) return;
    for (i = 0; i < VID_VESA_WIN; ++i) st->vesa_vram[off + i] = st->vmem[i];
}

/* INT 10h AX=4Fxx. Always returns AX=0x004F (supported+ok) for what we handle. */
static void vesa(video_state *st, ntvdd_regs *r)
{
    uint8_t al = r_al(r); unsigned i;
    switch (al) {
    case 0x00: {                                  /* return controller info       */
        uint8_t *b = (uint8_t *)vdd_map_flat(st->bus, r->es, (uint16_t)(uint16_t)r->edi);
        for (i = 0; i < 256; ++i) b[i] = 0;
        b[0]='V'; b[1]='E'; b[2]='S'; b[3]='A';
        wr16(b + 4, 0x0200);                      /* VBE 2.0                      */
        wr32(b + 6, ((uint32_t)r->es << 16) | (((uint16_t)r->edi + 0x100) & 0xFFFF));   /* OEM string */
        wr32(b + 10, 0);                          /* capabilities                 */
        wr32(b + 14, ((uint32_t)r->es << 16) | (((uint16_t)r->edi + 0x120) & 0xFFFF));  /* mode list  */
        wr16(b + 18, VID_VESA_VRAM / 0x10000);    /* total memory in 64KB units   */
        { const char *o = "NTVDMEX VESA"; for (i = 0; o[i]; ++i) b[0x100 + i] = (uint8_t)o[i]; b[0x100+i]=0; }
        for (i = 0; i < sizeof(vesa_modes)/sizeof(vesa_modes[0]); ++i)
            wr16(b + 0x120 + i*2, vesa_modes[i].num);
        wr16(b + 0x120 + i*2, 0xFFFF);            /* mode-list terminator         */
        s_ax(r, 0x004F);
        break; }
    case 0x01: {                                  /* return mode info             */
        uint16_t w, h;
        if (vesa_find(r_cx(r), &w, &h)) {
            uint8_t *b = (uint8_t *)vdd_map_flat(st->bus, r->es, (uint16_t)(uint16_t)r->edi);
            for (i = 0; i < 256; ++i) b[i] = 0;
            wr16(b + 0, 0x009B);                  /* attrs: supported|color|graphics */
            b[2] = 0x07; b[3] = 0x00;             /* WinA r/w/exists; WinB none    */
            wr16(b + 4, 64); wr16(b + 6, 64);     /* granularity / size (KB)       */
            wr16(b + 8, 0xA000); wr16(b + 10, 0); /* WinA seg / WinB seg           */
            wr32(b + 12, 0);                      /* WinFuncPtr (use INT 10h 4F05) */
            wr16(b + 16, w);                      /* bytes per scan line           */
            wr16(b + 18, w); wr16(b + 20, h);     /* X / Y resolution              */
            b[22] = 8; b[23] = 16;                /* char cell                     */
            b[24] = 1; b[25] = 8;                 /* planes / bpp                  */
            b[26] = 1; b[27] = 4;                 /* banks / memory model (packed) */
            wr32(b + 40, 0);                      /* PhysBasePtr: no LFB (banked)  */
            s_ax(r, 0x004F);
        } else s_ax(r, 0x014F);
        break; }
    case 0x02: {                                  /* set VBE mode                 */
        uint16_t w, h;
        if (vesa_find(r_bx(r), &w, &h)) {
            uint32_t n;
            st->in_vesa = 1; st->vesa_mode = r_bx(r) & 0x3FFF; st->vesa_w = w; st->vesa_h = h;
            st->vesa_bank = 0;
            for (n = 0; n < VID_VESA_VRAM; ++n) st->vesa_vram[n] = 0;
            for (n = 0; n < VID_VESA_WIN; ++n) st->vmem[n] = 0;
            st->dirty = 1; s_ax(r, 0x004F);
        } else s_ax(r, 0x014F);
        break; }
    case 0x03:                                    /* get current mode             */
        s_bx(r, st->in_vesa ? st->vesa_mode : 0); s_ax(r, 0x004F); break;
    case 0x05:                                    /* window control (bank switch) */
        if ((r_bx(r) & 0xFF) == 0) {              /* BL=0 set window A             */
            vesa_sync(st);                        /* flush current bank            */
            st->vesa_bank = r_dx(r);
            { uint32_t off = (uint32_t)st->vesa_bank * VID_VESA_WIN; unsigned k;
              if (off + VID_VESA_WIN <= VID_VESA_VRAM)
                  for (k = 0; k < VID_VESA_WIN; ++k) st->vmem[k] = st->vesa_vram[off + k]; }
            st->dirty = 1;
        } else if ((r_bx(r) & 0xFF) == 1) {       /* BL=1 get window               */
            s_dx(r, st->vesa_bank);
        }
        s_ax(r, 0x004F);
        break;
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
        st->mode = al & 0x7F; st->in_vesa = 0;        /* a standard mode leaves VESA */
        st->cur_row = st->cur_col = 0; st->page = 0;
        load_default_palette(st);                     /* HW reloads the DAC on mode set */
        if (st->mode == 0x13) {                       /* 320x200x256 graphics    */
            int i; for (i = 0; i < VID_G13_W * VID_G13_H; ++i) st->vmem[i] = 0;
        } else if (st->mode == 0x12) {                /* 640x480x16 planar       */
            int p, i;
            for (p = 0; p < 4; ++p) for (i = 0; i < VID_PLANE_SIZE; ++i) st->plane[p][i] = 0;
            st->cols = 80; st->rows = 30;             /* 80x30 text cells (8x16) */
        } else {                                      /* text                    */
            /* Only 13h and 12h are branched on above, so EVERY other mode lands
               here and silently becomes 80x25 text -- mode 0 gives 80x25 rather
               than 40x25, and mode 11h gives a text screen while the program
               writes pixels into A0000. Record anything that is not a mode we
               genuinely implement, so the log names it (GH #27, #39). */
            if (st->mode != 0x03 && st->mode != 0x02)
                VID_UNIMPL_SET(st->unimpl_mode, st->mode);
            st->cols = VID_COLS; st->rows = VID_ROWS; clear_text(st, 0x07);
        }
        break;
    case 0x01: st->cur_shape = r_cx(r); break;
    case 0x02: st->cur_row = (uint8_t)(r_dx(r) >> 8); st->cur_col = (uint8_t)(r_dx(r) & 0xFF); break;
    case 0x03: s_dx(r, (uint16_t)((st->cur_row << 8) | st->cur_col)); s_cx(r, st->cur_shape); break;
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
            if (st->mode == 0x12)                     /* draw the glyph as pixels */
                glyph_12h(st, c, rr, al, (uint8_t)(attr & 0x0F), (uint8_t)((attr >> 4) & 0x0F));
            if (++c >= st->cols) { c = 0; if (++rr >= st->rows) break; }
        }
        break; }
    case 0x0C:                                        /* write graphics pixel    */
        if (st->mode == 0x13) {
            uint32_t x = r_cx(r), y = r_dx(r);
            if (x < VID_G13_W && y < VID_G13_H) st->vmem[y * VID_G13_W + x] = al;
        } else if (st->mode == 0x12) {                /* planar: set 4-bit colour */
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
        if (st->mode == 0x12 && al >= 0x20) {         /* graphics: draw glyph + advance */
            glyph_12h(st, st->cur_col, st->cur_row, al, (uint8_t)(r_bx(r) & 0x0F), 0);
            advance(st);
        } else teletype(st, al);
        break;
    case 0x0F: s_ax(r, (uint16_t)((st->cols << 8) | st->mode)); s_bx(r, (uint16_t)(st->page << 8)); break;
    case 0x10:                                        /* palette / DAC            */
        if (al == 0x10) {                             /* set one DAC register     */
            uint16_t idx = r_bx(r);
            st->pal[idx & 0xFF] = dac_pack((uint8_t)(r_dx(r) >> 8) & 0x3F,
                                           (uint8_t)(r_cx(r) >> 8) & 0x3F,
                                           (uint8_t)(r_cx(r) & 0x3F));
        } else if (al == 0x12) {                      /* set block of DAC regs    */
            uint16_t first = r_bx(r), n = r_cx(r), i;
            uint8_t *t = (uint8_t *)vdd_map_flat(st->bus, r->es, (uint16_t)r_dx(r));
            for (i = 0; i < n && (first + i) < 256; ++i)
                st->pal[first + i] = dac_pack(t[i*3] & 0x3F, t[i*3+1] & 0x3F, t[i*3+2] & 0x3F);
        }
        break;
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
                   16-byte glyphs drifted 2 bytes per character and drew shredded text. */
                if (st->mode == 0x13 || st->mode == 0x04 || st->mode == 0x05 ||
                    st->mode == 0x06 || st->mode == 0x0D) {
                    seg = VDD_FONT8X8_SEG;  off = 0; bpc = 8;
                } else {
                    seg = VDD_FONT8X16_SEG; off = 0; bpc = 16;
                }
                break;
            default:   seg = VDD_FONT8X16_SEG; off = 0;       bpc = 16; break;
            }
            r->es = seg; r->ebp = off;
            s_cx(r, bpc);                              /* bytes per character     */
            s_dx(r, (uint16_t)(st->rows ? st->rows - 1 : 24));  /* DL = rows-1     */
            if (st->font_qn < 4) {                     /* record the request + answer */
                st->font_q[st->font_qn].al  = al;
                st->font_q[st->font_qn].bh  = bh;
                st->font_q[st->font_qn].seg = seg;
                st->font_q[st->font_qn].off = off;
                st->font_q[st->font_qn].cx  = bpc;
                st->font_qn++;
            }
        }
        break;
    case 0x12: {                                       /* alternate function sel  */
        uint8_t bl = (uint8_t)(r_bx(r) & 0xFF);
        if (bl == 0x10) { s_bx(r, 0x0003); s_cx(r, 0x0009); }  /* color / 256K, switches */
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
        wr16(b + 0x23, 16);                            /* bytes per character     */
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
            st->pal[st->dac_widx] = dac_pack(st->dac_latch[0], st->dac_latch[1], st->dac_latch[2]);
            st->dac_widx++; st->dac_comp = 0; st->dirty = 1;
        }
    }
}
static void dac_in(void *self, uint16_t port, uint8_t w, uint32_t *v)
{
    video_state *st = (video_state *)self; uint32_t p; (void)w;
    if (port == 0x3C8) { *v = st->dac_widx; return; }
    if (port != 0x3C9) { *v = 0xFF; return; }
    p = st->pal[st->dac_ridx];
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

void vga_planar_write(video_state *st, uint32_t off, uint8_t cpu)
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
        for (p = 0; p < 4; ++p) {
            uint8_t val = (st->enable_sr & (1<<p)) ? (uint8_t)((st->set_reset & (1<<p)) ? 0xFF : 0x00) : data;
            uint8_t r = vga_alu(alu, val, st->latch[p]);
            r = (uint8_t)((r & bm) | (st->latch[p] & (uint8_t)~bm));
            if (st->map_mask & (1<<p)) st->plane[p][off] = r;
        }
        return; }
    }
}

uint8_t vga_planar_read(video_state *st, uint32_t off)
{
    int p;
    if (off >= VID_PLANE_SIZE) return 0xFF;
    for (p = 0; p < 4; ++p) st->latch[p] = st->plane[p][off];   /* load latches    */
    return st->plane[st->read_map & 3][off];                    /* read mode 0     */
}

int vdd_video_planar_active(const video_state *st) { return st->mode == 0x12; }

/* Sequencer ports 3C4 (index) / 3C5 (data) -- Map Mask (SR2). */
static void seq_out(void *self, uint16_t port, uint8_t w, uint32_t v)
{
    video_state *st = (video_state *)self; (void)w;
    if (port == 0x3C4) st->seq_index = (uint8_t)v;
    else if (st->seq_index == 2) st->map_mask = (uint8_t)(v & 0x0F);
}
static void seq_in(void *self, uint16_t port, uint8_t w, uint32_t *v)
{
    video_state *st = (video_state *)self; (void)w;
    *v = (port == 0x3C4) ? st->seq_index : (st->seq_index == 2 ? st->map_mask : 0);
}
/* Graphics Controller ports 3CE (index) / 3CF (data). */
static void gc_out(void *self, uint16_t port, uint8_t w, uint32_t v)
{
    video_state *st = (video_state *)self; (void)w;
    if (port == 0x3CE) { st->gc_index = (uint8_t)v; return; }
    switch (st->gc_index) {
    case 0: st->set_reset   = (uint8_t)(v & 0x0F); break;
    case 1: st->enable_sr   = (uint8_t)(v & 0x0F); break;
    case 3: st->func_rotate = (uint8_t)(v & 0x1F); break;
    case 4: st->read_map    = (uint8_t)(v & 3);    break;
    case 5: st->write_mode  = (uint8_t)(v & 3);    break;
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
    case 3: *v = st->func_rotate; break; case 4: *v = st->read_map; break;
    case 5: *v = st->write_mode; break; case 8: *v = st->bit_mask; break;
    default: *v = 0; break;
    }
}

/* Input Status Register 1 (3DA/3BA). Graphics demos pace their animation by
   polling bit 3 (vertical retrace) and bit 0 (display enable). We have no real
   CRT timing, so toggle both bits on every read: any "wait until set then wait
   until clear" retrace loop completes within two reads, so the animation
   advances (a read of 3DA also resets the attribute-controller flip-flop). */
static void status_out(void *self, uint16_t port, uint8_t w, uint32_t v)
{ (void)self; (void)port; (void)w; (void)v; }     /* feature ctrl: ignore */
static void status_in(void *self, uint16_t port, uint8_t w, uint32_t *v)
{
    video_state *st = (video_state *)self; (void)port; (void)w;
    st->retrace ^= 0x09;                            /* toggle retrace(3) + disp-en(0) */
    *v = st->retrace;
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

void vdd_video_render(video_state *st)                 /* text glyph render        */
{
    int r, c, gy, gx;
    for (r = 0; r < st->rows; ++r)
        for (c = 0; c < st->cols; ++c) {
            uint8_t *p = cell(st, r, c);
            uint8_t ch = p[0], attr = p[1];
            uint8_t fg = attr & 0x0F, bg = (uint8_t)((attr >> 4) & 0x07);
            const uint8_t *gl = vga_font_8x16[ch];
            for (gy = 0; gy < VID_CELL_H; ++gy) {
                uint8_t bits = gl[gy];
                uint8_t *row = &st->fb[(r*VID_CELL_H + gy) * VID_FB_W + c*VID_CELL_W];
                for (gx = 0; gx < VID_CELL_W; ++gx) row[gx] = (bits & (0x80 >> gx)) ? fg : bg;
            }
        }
    if (st->cur_row < st->rows && st->cur_col < st->cols) {   /* underline cursor */
        uint8_t fg = cell(st, st->cur_row, st->cur_col)[1] & 0x0F;
        for (gy = VID_CELL_H - 2; gy < VID_CELL_H; ++gy)
            for (gx = 0; gx < VID_CELL_W; ++gx)
                st->fb[(st->cur_row*VID_CELL_H + gy) * VID_FB_W + st->cur_col*VID_CELL_W + gx] = fg;
    }
}

/* combine the 4 bit-planes into fb (16-colour indices) -- mode 12h. */
static void render_planar(video_state *st)
{
    int y, xb, b;
    for (y = 0; y < VID_G12_H; ++y) {
        uint32_t row = y * (VID_G12_W / 8);
        uint8_t *out = &st->fb[y * VID_G12_W];
        for (xb = 0; xb < VID_G12_W / 8; ++xb) {
            uint8_t p0 = st->plane[0][row+xb], p1 = st->plane[1][row+xb];
            uint8_t p2 = st->plane[2][row+xb], p3 = st->plane[3][row+xb];
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
static void vid_frame(void *self)
{
    video_state *st = (video_state *)self;
    if (!st->vmem) return;
    if (st->in_vesa) {                                 /* VESA: sync window -> vram */
        vesa_sync(st);
        st->frame.w = st->vesa_w; st->frame.h = st->vesa_h; st->frame.bpp = 8;
        st->frame.stride = st->vesa_w; st->frame.pixels = st->vesa_vram; st->frame.palette = st->pal;
    } else if (st->mode == 0x13) {                     /* graphics: vmem is the FB */
        st->frame.w = VID_G13_W; st->frame.h = VID_G13_H; st->frame.bpp = 8;
        st->frame.stride = VID_G13_W; st->frame.pixels = st->vmem; st->frame.palette = st->pal;
    } else if (st->mode == 0x12) {                     /* planar: combine -> fb    */
        render_planar(st);
        st->frame.w = VID_G12_W; st->frame.h = VID_G12_H; st->frame.bpp = 8;
        st->frame.stride = VID_G12_W; st->frame.pixels = st->fb; st->frame.palette = st->pal;
    } else {                                           /* text: render glyphs      */
        vdd_video_render(st);
        st->frame.w = VID_FB_W; st->frame.h = VID_FB_H; st->frame.bpp = 8;
        st->frame.stride = VID_FB_W; st->frame.pixels = st->fb; st->frame.palette = st->pal;
    }
    st->dirty = 0;
}

void vdd_video_putc(video_state *st, uint8_t ch) { teletype(st, ch); st->dirty = 1; }

void vdd_video_reset(void *self)
{
    video_state *st = (video_state *)self;
    st->mode = 3; st->cols = VID_COLS; st->rows = VID_ROWS;
    st->cur_row = st->cur_col = 0; st->cur_shape = 0x0607; st->page = 0;
    st->dac_widx = st->dac_ridx = st->dac_comp = 0;
    st->seq_index = st->gc_index = 0;
    st->map_mask = 0x0F; st->bit_mask = 0xFF; st->write_mode = 0;
    st->set_reset = st->enable_sr = st->func_rotate = st->read_map = 0;
    st->latch[0] = st->latch[1] = st->latch[2] = st->latch[3] = 0;
    st->in_vesa = 0; st->vesa_mode = 0; st->vesa_bank = 0;
    load_default_palette(st);
    if (st->vmem) clear_text(st, 0x07);
    st->dirty = 1;
}

int vdd_video_init(vdd_bus *b, void *self)
{
    video_state *st = (video_state *)self;
    st->bus = b;
    vdd_video_reset(st);
    if (vdd_claim_mem(b, VID_TEXT_BASE, 0x8000, vid_rd, vid_wr, st)) return -1;
    if (vdd_claim_int(b, 0x10, int10, st)) return -1;
    if (vdd_claim_ports(b, 0x3C4, 0x3C5, seq_in, seq_out, st)) return -1;  /* Sequencer */
    if (vdd_claim_ports(b, 0x3C7, 0x3C9, dac_in, dac_out, st)) return -1;  /* DAC       */
    if (vdd_claim_ports(b, 0x3CE, 0x3CF, gc_in, gc_out, st)) return -1;    /* Graphics  */
    if (vdd_claim_ports(b, 0x3DA, 0x3DA, status_in, status_out, st)) return -1; /* InpStatus1 */
    if (vdd_on_frame(b, vid_frame, st)) return -1;
    return 0;
}

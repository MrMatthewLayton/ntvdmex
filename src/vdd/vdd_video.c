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
    case 0x03:                                    /* get the current VBE mode      */
        s_bx(r, (uint16_t)(st->in_vesa ? st->vesa_mode : st->mode));
        s_ax(r, 0x004F);
        break;
    case 0x06:                                    /* get/set logical scan length   */
        /* BL=0 set in pixels, 1 get, 2 set in bytes, 3 get max. Returns BX=bytes
           per scan line, CX=pixels, DX=number of scan lines. */
        if ((r_bx(r) & 0xFF) == 0 && r_cx(r)) st->vesa_scanline = r_cx(r);
        if (!st->vesa_scanline) st->vesa_scanline = st->vesa_w;
        s_bx(r, st->vesa_scanline);               /* 8bpp: bytes == pixels         */
        s_cx(r, st->vesa_scanline);
        s_dx(r, (uint16_t)(st->vesa_scanline ? VID_VESA_VRAM / st->vesa_scanline : 0));
        s_ax(r, 0x004F);
        break;
    case 0x07:                                    /* get/set display start         */
        if ((r_bx(r) & 0xFF) == 0x01) {           /* BL=1 get                      */
            s_cx(r, st->vesa_start_x); s_dx(r, st->vesa_start_y); s_bx(r, 0);
        } else {                                  /* BL=0/80h set                  */
            st->vesa_start_x = r_cx(r); st->vesa_start_y = r_dx(r);
            st->dirty = 1;
        }
        s_ax(r, 0x004F);
        break;
    case 0x08:                                    /* get/set DAC palette width     */
        if ((r_bx(r) & 0xFF) == 0x00) {           /* BL=0 set                      */
            uint8_t w8 = (uint8_t)((r_bx(r) >> 8) & 0xFF);
            st->vesa_dacwidth = (uint8_t)(w8 == 8 ? 8 : 6);
        }
        if (!st->vesa_dacwidth) st->vesa_dacwidth = 6;
        s_bx(r, (uint16_t)((r_bx(r) & 0x00FF) | ((uint16_t)st->vesa_dacwidth << 8)));
        s_ax(r, 0x004F);
        break;
    case 0x09: {                                  /* get/set palette data          */
        /* BL=0 set, 1 get; DX=first, CX=count, ES:DI = quads (B,G,R,align).
           The DAC width set by 4F08 decides how far to shift. */
        uint8_t bl9 = (uint8_t)(r_bx(r) & 0xFF);
        uint16_t first = r_dx(r), n = r_cx(r), i;
        uint8_t *t = (uint8_t *)vdd_map_flat(st->bus, r->es, (uint16_t)r->edi);
        uint8_t sh = (uint8_t)(st->vesa_dacwidth == 8 ? 0 : 2);
        for (i = 0; i < n && (first + i) < 256; ++i) {
            if (bl9 == 0x00 || bl9 == 0x80)
                st->pal[first + i] = 0xFF000000u
                    | ((uint32_t)(t[i*4+2] << sh) << 16)
                    | ((uint32_t)(t[i*4+1] << sh) << 8)
                    |  (uint32_t)(t[i*4+0] << sh);
            else {
                uint32_t v = st->pal[first + i];
                t[i*4+0] = (uint8_t)((v & 0xFF) >> sh);
                t[i*4+1] = (uint8_t)(((v >> 8) & 0xFF) >> sh);
                t[i*4+2] = (uint8_t)(((v >> 16) & 0xFF) >> sh);
                t[i*4+3] = 0;
            }
        }
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
    case 0x15:                                    /* DDC / display identification  */
        s_ax(r, 0x014F);                          /* no monitor EDID to report     */
        VID_UNIMPL_SET(st->unimpl_fn, 0x4F);
        break;
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
        {   unsigned mi; const void *found = 0;
            for (mi = 0; mi < sizeof(vid_modes)/sizeof(vid_modes[0]); ++mi)
                if (vid_modes[mi].mode == st->mode) { found = &vid_modes[mi]; break; }
            if (!found) {                             /* a mode nobody defines   */
                VID_UNIMPL_SET(st->unimpl_mode, st->mode);
                st->mkind = VID_KIND_TEXT;
                st->cols = VID_COLS; st->rows = VID_ROWS;
                st->gw = VID_FB_W; st->gh = VID_FB_H;
                clear_text(st, 0x07);
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
                    clear_text(st, 0x07);
                } else if (st->mkind == VID_KIND_LINEAR8) {
                    int i; for (i = 0; i < VID_G13_W * VID_G13_H; ++i) st->vmem[i] = 0;
                } else if (st->mkind == VID_KIND_PLANAR) {
                    int pl, i;
                    for (pl = 0; pl < 4; ++pl)
                        for (i = 0; i < VID_PLANE_SIZE; ++i) st->plane[pl][i] = 0;
                } else if (st->mkind == VID_KIND_CGA) {
                    int i;
                    st->cga_bpp = (uint8_t)(st->mode == 0x06 ? 1 : 2);
                    st->cga_pal = 0;
                    for (i = 0; i < 16384; ++i) st->vmem[VID_TEXT_OFF + i] = 0;
                } else {
                    clear_text(st, 0x07);
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
        } else if (al == 0x00) {                      /* set one palette register */
            uint8_t reg = (uint8_t)((r_bx(r) >> 8) & 0xFF);
            if (reg < 17) st->vpal[reg] = (uint8_t)(r_bx(r) & 0x3F);
        } else if (al == 0x01) {                      /* set the border           */
            st->vpal[16] = st->overscan = (uint8_t)((r_bx(r) >> 8) & 0x3F);
        } else if (al == 0x02) {                      /* set all 16 + border      */
            uint8_t *t = (uint8_t *)vdd_map_flat(st->bus, r->es, (uint16_t)r_dx(r));
            int i; for (i = 0; i < 17; ++i) st->vpal[i] = (uint8_t)(t[i] & 0x3F);
            st->overscan = st->vpal[16];
        } else if (al == 0x03) {                      /* blink vs bright background */
            st->blink = (uint8_t)(r_bx(r) & 1);
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
            uint32_t v = st->pal[r_bx(r) & 0xFF];
            s_dx(r, (uint16_t)(((v >> 18) & 0x3F) << 8));
            s_cx(r, (uint16_t)(((((v >> 10) & 0x3F)) << 8) | ((v >> 2) & 0x3F)));
        } else if (al == 0x17) {                      /* get block of DAC regs    */
            uint16_t first = r_bx(r), n = r_cx(r), i;
            uint8_t *t = (uint8_t *)vdd_map_flat(st->bus, r->es, (uint16_t)r_dx(r));
            for (i = 0; i < n && (first + i) < 256; ++i) {
                uint32_t v = st->pal[first + i];
                t[i*3] = (uint8_t)((v >> 18) & 0x3F);
                t[i*3+1] = (uint8_t)((v >> 10) & 0x3F);
                t[i*3+2] = (uint8_t)((v >> 2) & 0x3F);
            }
        } else if (al == 0x1A) {                      /* get DAC page state       */
            s_bx(r, (uint16_t)((st->dac_page << 8) | 0));
        } else if (al == 0x1B) {                      /* convert to grey scale    */
            uint16_t first = r_bx(r), n = r_cx(r), i;
            for (i = 0; i < n && (first + i) < 256; ++i) {
                uint32_t v = st->pal[first + i];
                uint32_t g = ((((v >> 16) & 0xFF) * 30) + (((v >> 8) & 0xFF) * 59)
                              + ((v & 0xFF) * 11)) / 100;
                st->pal[first + i] = 0xFF000000u | (g << 16) | (g << 8) | g;
            }
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
        /* CX is a bitmask of what to save; BX:0 is the buffer. We report the
           size in BX (blocks of 64 bytes) and accept save/restore of the DAC,
           which is the part programs actually use. */
        uint8_t al1c = al;
        if (al1c == 0x00)      { s_bx(r, 3); s_ax(r, (uint16_t)((r_ax(r) & 0xFF00) | 0x1C)); }
        else if (al1c == 0x01 || al1c == 0x02) {
            uint8_t *buf = (uint8_t *)vdd_map_flat(st->bus, r->es, (uint16_t)r->ebx);
            int i;
            if (al1c == 0x01) for (i = 0; i < 256; ++i)
                { buf[i*3] = (uint8_t)((st->pal[i] >> 18) & 0x3F);
                  buf[i*3+1] = (uint8_t)((st->pal[i] >> 10) & 0x3F);
                  buf[i*3+2] = (uint8_t)((st->pal[i] >> 2) & 0x3F); }
            else for (i = 0; i < 256; ++i)
                st->pal[i] = dac_pack(buf[i*3] & 0x3F, buf[i*3+1] & 0x3F, buf[i*3+2] & 0x3F);
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
                }
            } else {
                st->user_font_on = 0;              /* back to the ROM tables */
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
int vdd_video_present_ready(video_state *st)
{
    uint32_t frame_us, pm;
    int act;
    if (!st->time_us) return 1;                 /* no clock: present every tick   */
    frame_us = 1000000u / (uint32_t)((st->gh > VID_VACTIVE_LO) ? VID_VBL_HZ_HI : VID_VBL_HZ_LO);
    if (!frame_us) return 1;
    pm  = (uint32_t)((st->time_us() % frame_us) * 1000u / frame_us);
    act = (st->gh > VID_VACTIVE_LO) ? 914 : 891;
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
    case 0x0C: st->crtc_start = (uint16_t)((st->crtc_start & 0x00FF) | ((uint16_t)(v & 0xFF) << 8));
               st->crtc_seen = 1; st->dirty = 1; break;
    case 0x0D: st->crtc_start = (uint16_t)((st->crtc_start & 0xFF00) | (v & 0xFF));
               st->crtc_seen = 1; st->dirty = 1; break;
    case 0x13: st->crtc_offset = (uint8_t)v;                                                          st->dirty = 1; break;
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
    case 3: *v = st->func_rotate; break; case 4: *v = st->read_map; break;
    case 5: *v = st->write_mode; break; case 8: *v = st->bit_mask; break;
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
static void status_out(void *self, uint16_t port, uint8_t w, uint32_t v)
{ (void)self; (void)port; (void)w; (void)v; }     /* feature ctrl: ignore */
static void status_in(void *self, uint16_t port, uint8_t w, uint32_t *v)
{
    video_state *st = (video_state *)self; (void)port; (void)w;
    uint64_t now, in_frame;
    uint32_t frame_us, line_us, line, dot_us;
    int tall, vtotal, vactive, in_vbl, in_hbl;

    if (!st->time_us) {                             /* no clock injected: old behaviour */
        st->retrace ^= 0x09;
        *v = st->retrace;
        return;
    }
    /* 480-line modes run at 60 Hz, the 200/400-line ones at 70 Hz. Mode 13h is
       320x200 displayed as 400 scanlines, so it belongs with the 70 Hz group -- key
       the choice off the DISPLAYED height, not the mode number. */
    tall    = (st->gh > VID_VACTIVE_LO);
    vtotal  = tall ? VID_VTOTAL_HI  : VID_VTOTAL_LO;
    vactive = tall ? VID_VACTIVE_HI : VID_VACTIVE_LO;
    frame_us = 1000000u / (uint32_t)(tall ? VID_VBL_HZ_HI : VID_VBL_HZ_LO);
    line_us  = frame_us / (uint32_t)vtotal;
    if (!line_us) line_us = 1;                      /* never divide by zero          */

    now      = st->time_us();
    in_frame = now % (uint64_t)frame_us;
    line     = (uint32_t)(in_frame / line_us);
    dot_us   = (uint32_t)(in_frame % line_us);
    if (line >= (uint32_t)vtotal) line = (uint32_t)vtotal - 1;  /* rounding guard     */

    in_vbl = (line >= (uint32_t)vactive);
    in_hbl = (dot_us * 100u >= line_us * VID_HACTIVE_PCT);
    /* bit 3 = vertical retrace; bit 0 = display disabled (h- OR v-blank). Bit 0 is a
       DIFFERENT signal on a real card -- it changes per scanline, not per frame --
       so toggling the two together, as we used to, was doubly wrong. */
    *v = (uint32_t)((in_vbl ? 0x08u : 0u) | ((in_vbl || in_hbl) ? 0x01u : 0u));
    st->retrace = (uint8_t)*v;                      /* keep it observable in dumps    */
    /* Count the edge the GUEST sees, not the one the model produces: a clear->set
       transition between two of its own reads is exactly one completed
       `WAIT &H3DA,8`, so edges/second IS the guest's frame rate. */
    st->p3da_reads++;
    if (in_vbl && !st->vbl_prev) st->vbl_edges++;
    st->vbl_prev = (uint8_t)in_vbl;
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
            /* GH #52: a loaded user font wins; otherwise the ROM table, which
               is the ordinary case and one predictable branch. */
            const uint8_t *gl = st->user_font_on ? &st->user_font[ch * VID_CELL_H]
                                                 : vga_font_8x16[ch];
            for (gy = 0; gy < VID_CELL_H; ++gy) {
                uint8_t bits = gl[gy];
                uint8_t *row = &st->fb[(r*VID_CELL_H + gy) * VID_FB_W + c*VID_CELL_W];
                for (gx = 0; gx < VID_CELL_W; ++gx) row[gx] = (bits & (0x80 >> gx)) ? fg : bg;
            }
        }
    /* ── THE TEXT CURSOR: SHAPE FROM THE GUEST, BLINK FROM THE CLOCK. ────────────
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
        unsigned start = (st->cur_shape >> 8) & 0x1F;    /* CH bits 0-4: first line  */
        unsigned end   =  st->cur_shape       & 0x1F;    /* CL bits 0-4: last line   */
        int hidden     = ((st->cur_shape >> 8) & 0x20) != 0;   /* CH bit 5: cursor off */
        int lit = 1;
        if (st->cur_shape == 0) { start = VID_CELL_H - 2; end = VID_CELL_H - 1; }
        if (start >= VID_CELL_H) start = VID_CELL_H - 2;
        if (end   >= VID_CELL_H) end   = VID_CELL_H - 1;
        if (start > end) hidden = 1;                     /* the other "off" idiom    */
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
                    st->fb[(st->cur_row*VID_CELL_H + gy) * VID_FB_W
                           + st->cur_col*VID_CELL_W + gx] = fg;
        }
    }
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

static void render_planar(video_state *st)
{
    int y, xb, b;
    int gw = st->gw ? st->gw : VID_G12_W;
    int gh = st->gh ? st->gh : VID_G12_H;
    for (y = 0; y < gh; ++y) {
        uint32_t row = y * (gw / 8);
        uint8_t *out = &st->fb[y * gw];
        for (xb = 0; xb < gw / 8; ++xb) {
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
    if (st->in_vesa) {                                 /* VESA: sync window -> vram */
        vesa_sync(st);
        st->frame.w = st->vesa_w; st->frame.h = st->vesa_h; st->frame.bpp = 8;
        st->frame.stride = st->vesa_w; st->frame.pixels = st->vesa_vram; st->frame.palette = st->pal;
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
        st->frame.h = (uint16_t)(st->rows * VID_CELL_H);
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
    st->mode_qn = 0;
    st->cur_row = st->cur_col = 0; st->cur_shape = 0x0607; st->page = 0;
    st->dac_widx = st->dac_ridx = st->dac_comp = 0;
    st->seq_index = st->gc_index = 0;
    st->map_mask = 0x0F; st->bit_mask = 0xFF; st->write_mode = 0;
    st->chain4 = 1; st->y_mask = 0x0F;
    st->crtc_index = 0; st->crtc_offset = 40; st->crtc_start = 0;
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
    st->modey_gap = MODEY_GAP_DEFAULT;
    if (vdd_claim_mem(b, VID_TEXT_BASE, 0x8000, vid_rd, vid_wr, st)) return -1;
    if (vdd_claim_int(b, 0x10, int10, st)) return -1;
    if (vdd_claim_ports(b, 0x3C4, 0x3C5, seq_in, seq_out, st)) return -1;  /* Sequencer */
    /* ⚠ THE OLD WARNING HERE ("DO NOT CLAIM CRTC 0x3D4/0x3D5", three regressions,
         mechanism UNKNOWN) IS RESOLVED, not ignored. The mechanism was that Doom
         page-flips with ONE 16-BIT WRITE and these handlers dropped the data byte, so
         claiming the port broke the flip outright -- worse than not claiming it. See
         seq_out()/vga_idx_data(). */
    if (vdd_claim_ports(b, 0x3C7, 0x3C9, dac_in, dac_out, st)) return -1;  /* DAC       */
    if (vdd_claim_ports(b, 0x3CE, 0x3CF, gc_in, gc_out, st)) return -1;    /* Graphics  */
    /* CRTC. Claimed at last -- see render_modey() for why three earlier attempts
       regressed Doom and why that cause is gone. */
    if (vdd_claim_ports(b, 0x3D4, 0x3D5, crtc_in, crtc_out, st)) return -1; /* CRTC     */
    if (vdd_claim_ports(b, 0x3DA, 0x3DA, status_in, status_out, st)) return -1; /* InpStatus1 */
    if (vdd_on_frame(b, vid_frame, st)) return -1;
    return 0;
}

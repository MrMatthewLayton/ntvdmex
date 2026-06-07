/* vdd_video.c -- see vdd_video.h.  Text mode 3 + graphics mode 13h over the
 * shared video aperture (vmem), with the DAC palette, on the VDD bus.  Pure C. */
#include "vdd_video.h"
#include "vga_font_8x16.h"

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
        if (st->mode == 0x13) {                       /* 320x200x256 graphics    */
            int i; for (i = 0; i < VID_G13_W * VID_G13_H; ++i) st->vmem[i] = 0;
        } else if (st->mode == 0x12) {                /* 640x480x16 planar       */
            int p, i;
            for (p = 0; p < 4; ++p) for (i = 0; i < VID_PLANE_SIZE; ++i) st->plane[p][i] = 0;
        } else {                                      /* text                    */
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
    case 0x0E: teletype(st, al); break;
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
    case 0x4F: vesa(st, r); break;                     /* VESA VBE 2.0            */
    default: st->dirty = 0; break;
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

static void vid_frame(void *self)
{
    video_state *st = (video_state *)self;
    if (!st->dirty || !st->vmem) return;
    if (st->in_vesa) {                                 /* VESA: sync window -> vram */
        vesa_sync(st);
        st->frame.w = st->vesa_w; st->frame.h = st->vesa_h; st->frame.bpp = 8;
        st->frame.stride = st->vesa_w; st->frame.pixels = st->vesa_vram; st->frame.palette = st->pal;
        vdd_present(st->bus, &st->frame); st->dirty = 0; return;
    }
    if (st->mode == 0x13) {                            /* graphics: vmem is the FB */
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
    vdd_present(st->bus, &st->frame);
    st->dirty = 0;
}

void vdd_video_putc(video_state *st, uint8_t ch) { teletype(st, ch); st->dirty = 1; }

void vdd_video_reset(void *self)
{
    video_state *st = (video_state *)self;
    int i;
    st->mode = 3; st->cols = VID_COLS; st->rows = VID_ROWS;
    st->cur_row = st->cur_col = 0; st->cur_shape = 0x0607; st->page = 0;
    st->dac_widx = st->dac_ridx = st->dac_comp = 0;
    st->in_vesa = 0; st->vesa_mode = 0; st->vesa_bank = 0;
    for (i = 0; i < 16; ++i)  st->pal[i] = ega16[i];
    for (i = 16; i < 256; ++i) st->pal[i] = 0xFF000000u | (uint32_t)(i * 0x010101u); /* grey ramp */
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
    if (vdd_claim_ports(b, 0x3C7, 0x3C9, dac_in, dac_out, st)) return -1;
    if (vdd_on_frame(b, vid_frame, st)) return -1;
    return 0;
}

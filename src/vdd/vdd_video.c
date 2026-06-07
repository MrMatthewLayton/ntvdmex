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

/* INT 10h text + mode + palette services. */
static void int10(void *self, ntvdd_regs *r)
{
    video_state *st = (video_state *)self;
    uint8_t ah = r_ah(r), al = r_al(r);
    st->dirty = 1;
    switch (ah) {
    case 0x00:                                        /* set video mode          */
        st->mode = al & 0x7F;
        st->cur_row = st->cur_col = 0; st->page = 0;
        if (st->mode == 0x13) {                       /* 320x200x256 graphics    */
            int i; for (i = 0; i < VID_G13_W * VID_G13_H; ++i) st->vmem[i] = 0;
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

static void vid_frame(void *self)
{
    video_state *st = (video_state *)self;
    if (!st->dirty || !st->vmem) return;
    if (st->mode == 0x13) {                            /* graphics: vmem is the FB */
        st->frame.w = VID_G13_W; st->frame.h = VID_G13_H; st->frame.bpp = 8;
        st->frame.stride = VID_G13_W; st->frame.pixels = st->vmem; st->frame.palette = st->pal;
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

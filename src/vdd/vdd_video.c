/* vdd_video.c -- see vdd_video.h.  Text-mode (mode 3) VGA model + INT 10h text
 * services + cell renderer, on the VDD bus.  Pure C, no <windows.h>. */
#include "vdd_video.h"
#include "vga_font_8x16.h"

/* 16-colour EGA/CGA text palette as 0xAARRGGBB. */
static const uint32_t ega16[16] = {
    0xFF000000, 0xFF0000AA, 0xFF00AA00, 0xFF00AAAA,
    0xFFAA0000, 0xFFAA00AA, 0xFFAA5500, 0xFFAAAAAA,
    0xFF555555, 0xFF5555FF, 0xFF55FF55, 0xFF55FFFF,
    0xFFFF5555, 0xFFFF55FF, 0xFFFFFF55, 0xFFFFFFFF
};

static uint8_t *cell(video_state *st, int r, int c)   /* -> char byte of (r,c) */
{ return &st->vram[(r * st->cols + c) * 2]; }

static void clear_screen(video_state *st, uint8_t attr)
{
    int n = st->cols * st->rows, i;
    for (i = 0; i < n; ++i) { st->vram[i*2] = ' '; st->vram[i*2+1] = attr; }
}

/* scroll [top..bot]x[left..right] up by `lines` (0 = clear whole window), fill
   the vacated rows with spaces of `attr`. */
static void scroll_up(video_state *st, int lines, int top, int left,
                      int bot, int right, uint8_t attr)
{
    int r, c;
    if (lines <= 0 || lines > (bot - top + 1)) {     /* clear the region        */
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

static void advance(video_state *st)                 /* teletype cursor advance */
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
    case 0x0D: st->cur_col = 0; break;                /* CR                      */
    case 0x0A:                                        /* LF                      */
        if (++st->cur_row >= st->rows) {
            scroll_up(st, 1, 0, 0, st->rows - 1, st->cols - 1, 0x07);
            st->cur_row = st->rows - 1;
        }
        break;
    case 0x08: if (st->cur_col) st->cur_col--; break; /* BS                      */
    case 0x07: break;                                 /* BEL                     */
    default:
        cell(st, st->cur_row, st->cur_col)[0] = ch;   /* keep existing attr      */
        advance(st);
    }
}

/* INT 10h text-mode services. */
static void int10(void *self, ntvdd_regs *r)
{
    video_state *st = (video_state *)self;
    uint8_t ah = r_ah(r), al = r_al(r);
    st->dirty = 1;
    switch (ah) {
    case 0x00:                                        /* set video mode          */
        st->mode = al & 0x7F;
        st->cols = VID_COLS; st->rows = VID_ROWS;
        st->cur_row = st->cur_col = 0; st->page = 0;
        clear_screen(st, 0x07);
        break;
    case 0x01: st->cur_shape = r_cx(r); break;        /* set cursor shape        */
    case 0x02: st->cur_row = (uint8_t)(r_dx(r) >> 8); /* set cursor pos DH,DL    */
               st->cur_col = (uint8_t)(r_dx(r) & 0xFF); break;
    case 0x03:                                        /* get cursor pos/shape    */
        s_dx(r, (uint16_t)((st->cur_row << 8) | st->cur_col));
        s_cx(r, st->cur_shape);
        break;
    case 0x05: st->page = al; break;                  /* set active page         */
    case 0x06:                                        /* scroll up               */
        scroll_up(st, al, (uint8_t)(r_cx(r) >> 8), (uint8_t)(r_cx(r) & 0xFF),
                  (uint8_t)(r_dx(r) >> 8), (uint8_t)(r_dx(r) & 0xFF),
                  (uint8_t)(r_bx(r) >> 8));
        break;
    case 0x08:                                        /* read char+attr @ cursor */
        { uint8_t *p = cell(st, st->cur_row, st->cur_col);
          s_ax(r, (uint16_t)((p[1] << 8) | p[0])); }
        break;
    case 0x09:                                        /* write char+attr xCX     */
    case 0x0A: {                                       /* write char only xCX     */
        uint16_t n = r_cx(r); if (!n) n = 1;
        uint8_t attr = (uint8_t)(r_bx(r) & 0xFF);     /* BL                      */
        int c = st->cur_col, rr = st->cur_row;
        while (n-- && rr < st->rows) {
            uint8_t *p = cell(st, rr, c);
            p[0] = al; if (ah == 0x09) p[1] = attr;
            if (++c >= st->cols) { c = 0; if (++rr >= st->rows) break; }
        }
        break; }
    case 0x0E: teletype(st, al); break;               /* teletype output         */
    case 0x0F:                                         /* get video mode          */
        s_ax(r, (uint16_t)((st->cols << 8) | st->mode));
        s_bx(r, (uint16_t)(st->page << 8));
        break;
    case 0x13: {                                       /* write string ES:BP      */
        uint16_t n = r_cx(r), i;
        uint8_t mode = al, attr = (uint8_t)(r_bx(r) & 0xFF);
        uint8_t *s = (uint8_t *)vdd_map_flat(st->bus, r->es, (uint16_t)r->ebp);
        st->cur_row = (uint8_t)(r_dx(r) >> 8); st->cur_col = (uint8_t)(r_dx(r) & 0xFF);
        for (i = 0; i < n; ++i) {
            uint8_t ch = *s++;
            if (mode & 0x02) attr = *s++;             /* attr interleaved        */
            cell(st, st->cur_row, st->cur_col)[0] = ch;
            cell(st, st->cur_row, st->cur_col)[1] = attr;
            advance(st);
        }
        break; }
    default: st->dirty = 0; break;                     /* unhandled: no-op        */
    }
}

/* B8000 window read/write (direct screen access). */
static uint8_t vid_rd(void *self, uint32_t off)
{ video_state *st = (video_state *)self; return st->vram[off]; }
static void vid_wr(void *self, uint32_t off, uint8_t v)
{ video_state *st = (video_state *)self; st->vram[off] = v; st->dirty = 1; }

void vdd_video_render(video_state *st)
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
                for (gx = 0; gx < VID_CELL_W; ++gx)
                    row[gx] = (bits & (0x80 >> gx)) ? fg : bg;
            }
        }
    /* a simple underline cursor in the cursor cell's fg colour */
    if (st->cur_row < st->rows && st->cur_col < st->cols) {
        uint8_t fg = cell(st, st->cur_row, st->cur_col)[1] & 0x0F;
        for (gy = VID_CELL_H - 2; gy < VID_CELL_H; ++gy)
            for (gx = 0; gx < VID_CELL_W; ++gx)
                st->fb[(st->cur_row*VID_CELL_H + gy) * VID_FB_W + st->cur_col*VID_CELL_W + gx] = fg;
    }
}

static void vid_frame(void *self)
{
    video_state *st = (video_state *)self;
    if (!st->dirty) return;
    vdd_video_render(st);
    st->frame.w = VID_FB_W; st->frame.h = VID_FB_H; st->frame.bpp = 8;
    st->frame.stride = VID_FB_W; st->frame.pixels = st->fb; st->frame.palette = st->pal;
    vdd_present(st->bus, &st->frame);
    st->dirty = 0;
}

void vdd_video_reset(void *self)
{
    video_state *st = (video_state *)self;
    int i;
    st->mode = 3; st->cols = VID_COLS; st->rows = VID_ROWS;
    st->cur_row = st->cur_col = 0; st->cur_shape = 0x0607; st->page = 0;
    for (i = 0; i < 16; ++i)  st->pal[i] = ega16[i];
    for (i = 16; i < 256; ++i) st->pal[i] = 0xFF000000u;
    clear_screen(st, 0x07);
    st->dirty = 1;
}

int vdd_video_init(vdd_bus *b, void *self)
{
    video_state *st = (video_state *)self;
    st->bus = b;
    vdd_video_reset(st);
    if (vdd_claim_mem(b, VID_TEXT_BASE, VID_WIN_SIZE, vid_rd, vid_wr, st)) return -1;
    if (vdd_claim_int(b, 0x10, int10, st)) return -1;
    if (vdd_on_frame(b, vid_frame, st)) return -1;
    return 0;
}

/*
 * vdd_video.h -- the video VDD, text mode 3 first.  (M3 slice-4, ADR-0008)
 *
 * Owns the emulated VGA text screen: it traps the B8000 colour-text window,
 * services the INT 10h text subset, keeps the 80x25 cell grid + cursor, and
 * renders the grid (8x16 font, 16-colour EGA palette) into an 8bpp ntvdd_frame
 * that it hands to present_ddraw via the bus.  Pure C, no <windows.h>: all state
 * is explicit and effects go through the bus, so it is unit-tested off-VM.
 *
 * Graphics modes (13h linear, VESA) build on this same frame-sink path later.
 */
#ifndef NTVDMEX_VDD_VIDEO_H
#define NTVDMEX_VDD_VIDEO_H

#include "vdd_bus.h"

#define VID_TEXT_BASE   0xB8000u        /* colour-text aperture (page 0)        */
#define VID_WIN_SIZE    0x8000u         /* 32KB text window                     */
#define VID_COLS        80
#define VID_ROWS        25
#define VID_CELL_W      8               /* font cell pixels                     */
#define VID_CELL_H      16
#define VID_FB_W        (VID_COLS * VID_CELL_W)   /* 640                        */
#define VID_FB_H        (VID_ROWS * VID_CELL_H)   /* 400                        */

typedef struct video_state {
    vdd_bus *bus;
    uint8_t  mode;                      /* current video mode (3 = text 80x25)  */
    uint8_t  cols, rows;
    uint8_t  cur_row, cur_col;          /* cursor (page 0)                      */
    uint16_t cur_shape;                 /* CX from INT 10h AH=01                 */
    uint8_t  page;                      /* active display page                  */
    uint8_t  vram[VID_WIN_SIZE];        /* B8000 window: char/attr cell pairs   */
    uint32_t pal[256];                  /* ARGB; [0..15] = EGA text colours      */
    uint8_t  fb[VID_FB_W * VID_FB_H];   /* rendered framebuffer (palette idx)   */
    int      dirty;                     /* a write happened since last present   */
    ntvdd_frame frame;
} video_state;

int  vdd_video_init(vdd_bus *b, void *self);
void vdd_video_reset(void *self);
static inline ntvdd vdd_video_device(video_state *st)
{ ntvdd d; d.name = "video"; d.init = vdd_video_init; d.reset = vdd_video_reset;
  d.shutdown = 0; d.self = st; return d; }

/* Render the cell grid into st->fb (exposed for the off-VM battery). */
void vdd_video_render(video_state *st);

#endif /* NTVDMEX_VDD_VIDEO_H */

/*
 * vdd_video.h -- the video VDD: text mode 3 + graphics mode 13h.  (M3, ADR-0008)
 *
 * Owns the emulated VGA display. The guest's video memory aperture (A0000-BFFFF,
 * 128KB) is mapped as RAM, so both INT 10h/console writes AND direct-framebuffer
 * writes land in the same `vmem`; the VDD renders it each frame into an ntvdd_frame
 * for present_ddraw:
 *   - text mode 3: B8000 cell grid -> 8x16 font, 16-colour EGA palette (640x400)
 *   - mode 13h:    A0000 linear 320x200x256, palette indices straight to present
 * The DAC ports (3C7-3C9) + INT 10h AH=10h drive the 256-entry palette. Pure C,
 * no <windows.h>: set `st->vmem` (host: 0xA0000 absolute; test: a 128KB buffer)
 * before vdd_bus_add(), and the whole VDD is exercised off-VM.
 */
#ifndef NTVDMEX_VDD_VIDEO_H
#define NTVDMEX_VDD_VIDEO_H

#include "vdd_bus.h"

#define VID_APERTURE_BASE 0xA0000u      /* video memory window base               */
#define VID_APERTURE_SIZE 0x20000u      /* A0000-BFFFF (128KB)                    */
#define VID_TEXT_BASE     0xB8000u      /* colour-text page 0                     */
#define VID_TEXT_OFF      (VID_TEXT_BASE - VID_APERTURE_BASE)  /* 0x18000         */
#define VID_COLS          80
#define VID_ROWS          25
#define VID_CELL_W        8
#define VID_CELL_H        16
#define VID_FB_W          (VID_COLS * VID_CELL_W)   /* 640 (text render target)   */
#define VID_FB_H          (VID_ROWS * VID_CELL_H)   /* 400                        */
#define VID_G13_W         320
#define VID_G13_H         200

typedef struct video_state {
    vdd_bus *bus;
    uint8_t *vmem;                      /* the 128KB aperture (A0000); caller-set  */
    uint8_t  mode;                      /* 0x03 text, 0x13 graphics                */
    uint8_t  cols, rows;
    uint8_t  cur_row, cur_col;
    uint16_t cur_shape;
    uint8_t  page;
    uint32_t pal[256];                  /* ARGB palette ([0..15]=EGA for text)     */
    /* DAC (ports 3C7/3C8/3C9) write/read state */
    uint8_t  dac_widx, dac_ridx, dac_comp, dac_latch[3];
    uint8_t  fb[VID_FB_W * VID_FB_H];   /* text glyph render target                */
    int      dirty;
    ntvdd_frame frame;
} video_state;

int  vdd_video_init(vdd_bus *b, void *self);
void vdd_video_reset(void *self);
static inline ntvdd vdd_video_device(video_state *st)
{ ntvdd d; d.name = "video"; d.init = vdd_video_init; d.reset = vdd_video_reset;
  d.shutdown = 0; d.self = st; return d; }

void vdd_video_render(video_state *st);                /* text glyph render        */
void vdd_video_putc(video_state *st, uint8_t ch);      /* console teletype sink    */

#endif /* NTVDMEX_VDD_VIDEO_H */

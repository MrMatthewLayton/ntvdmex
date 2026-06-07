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
#define VID_G12_W         640           /* mode 12h: 640x480x16 planar            */
#define VID_G12_H         480
#define VID_PLANE_SIZE    (VID_G12_W * VID_G12_H / 8)   /* 38400 bytes/plane      */

/* VESA VBE 2.0 (banked, packed-256). A0000 is the 64KB window onto vesa_vram. */
#define VID_VESA_WIN      0x10000u      /* 64KB banked window                     */
#define VID_VESA_VRAM     0x80000u      /* 512KB total emulated VRAM (8 banks)    */
#define VID_FB_MAX        (640 * 480)   /* largest glyph/planar render target     */

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
    /* VESA VBE state */
    uint8_t  in_vesa;                   /* a VESA mode is active                   */
    uint16_t vesa_mode, vesa_w, vesa_h; /* current VESA mode + resolution          */
    uint16_t vesa_bank;                 /* current 64KB window bank (4F05)         */
    uint8_t  vesa_vram[VID_VESA_VRAM];  /* full packed-256 framebuffer             */
    uint8_t  plane[4][VID_PLANE_SIZE];  /* mode 12h: 4 bit-planes (640x480x16)     */
    uint8_t  fb[VID_FB_MAX];            /* text glyph / planar render target        */
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

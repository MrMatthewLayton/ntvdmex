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
    /* VGA planar write engine (Sequencer 3C4/3C5 + Graphics Controller 3CE/3CF) */
    uint8_t  seq_index, gc_index;
    uint8_t  map_mask;     /* SR2: planes enabled for writes (reset 0x0F)          */
    uint8_t  set_reset;    /* GR0                                                  */
    uint8_t  enable_sr;    /* GR1                                                  */
    uint8_t  func_rotate;  /* GR3: bits0-2 rotate count, bits3-4 ALU               */
    uint8_t  read_map;     /* GR4: plane read in read-mode 0                       */
    uint8_t  write_mode;   /* GR5 bits0-1                                          */
    uint8_t  bit_mask;     /* GR8 (reset 0xFF)                                     */
    uint8_t  latch[4];     /* per-plane read latches                               */
    uint8_t  retrace;      /* Input Status 1 (3DA): toggled so vsync polls advance  */
    uint32_t int10_11_calls;/* INT 10h AH=11h (character generator) calls -- see below */
    /* WHAT the guest asked for, not just that it asked. BH selects the table and the
       answer's CX (bytes per character) is what the caller strides by -- so a wrong CX
       misaligns every glyph after the first, which looks exactly like garbled text.
       Recorded per call so the log says which table Skyroads wants. */
    struct { uint8_t al, bh; uint16_t seg, off, cx; } font_q[4];
    uint8_t  font_qn;
    uint8_t  fb[VID_FB_MAX];            /* text glyph / planar render target        */
    int      dirty;
    /* GH #27: every unimplemented path must announce itself. An INT 10h function
       we do not handle, or a mode number we do not support, used to be a silent
       no-op -- the program carried on drawing into a screen that was never set
       up, and the only symptom was "the display looks wrong". These bitmaps are
       drained into the STAGE2 block so a run yields a to-do list instead. */
    uint8_t  unimpl_fn[32];             /* INT 10h AH values seen but unhandled     */
    uint8_t  unimpl_mode[32];           /* mode numbers requested but unsupported   */
    ntvdd_frame frame;
} video_state;

#define VID_UNIMPL_SET(bm, n)  ((bm)[((n) & 0xFF) >> 3] |= (uint8_t)(1u << ((n) & 7)))
#define VID_UNIMPL_GET(bm, n)  (((bm)[((n) & 0xFF) >> 3] >> ((n) & 7)) & 1u)

int  vdd_video_init(vdd_bus *b, void *self);
void vdd_video_reset(void *self);
static inline ntvdd vdd_video_device(video_state *st)
{ ntvdd d; d.name = "video"; d.init = vdd_video_init; d.reset = vdd_video_reset;
  d.shutdown = 0; d.self = st; return d; }

void vdd_video_render(video_state *st);                /* text glyph render        */
void vdd_video_putc(video_state *st, uint8_t ch);      /* console teletype sink    */

/* Planar A0000 access (mode 12h): the host calls these from the memory-write trap
   so direct framebuffer writes run through the VGA write-modes into the 4 planes.
   `off` is the byte offset within the A0000 window. */
void    vga_planar_write(video_state *st, uint32_t off, uint8_t cpu);
uint8_t vga_planar_read (video_state *st, uint32_t off);   /* loads latches        */
int     vdd_video_planar_active(const video_state *st);    /* 1 in mode 12h        */

/* WHERE THE BIOS FONTS LIVE IN GUEST MEMORY.
   INT 10h AH=11h AL=30h hands the caller a POINTER to the character generator, and plenty of
   DOS games take it and render text themselves rather than going through the BIOS. We were
   answering that call with the metrics (CX, DL) but never setting ES:BP -- so the caller drew
   from whatever ES:BP happened to hold, which is exactly the glyph-shaped noise Skyroads put
   on screen in place of "ROAD COMPLETED". The tables therefore need a real address the guest
   can read. The B0000 half of the text aperture is already mapped as RAM and is untouched by
   a VGA game (our text output lives at B8000), so the fonts go there. */
#define VDD_FONT8X16_SEG 0xB000       /* 256 chars * 16 bytes = 0x1000 */
#define VDD_FONT8X8_SEG  0xB100       /* 256 chars *  8 bytes = 0x0800 */
#define VDD_FONT8X14_SEG 0xB180       /* 256 chars * 14 bytes = 0x0E00 (B1800..B25FF) */

/* `int10_11_calls` is counted so the next round is not another guess: the font-pointer fix
   assumed the guest asks for its glyphs with INT 10h AH=11h, and the text is still garbled.
   If it stays at zero, Skyroads never asks -- it is reading a font from a hard-coded ROM
   address (F000:FA6E is the classic one) or carrying its own, and the fix was aimed at the
   wrong thing. The end-of-run STAGE2 summary reports it. */

/* Publish both fonts into guest memory. Call once at start-up. */
void vdd_video_install_fonts(video_state *st);

#endif /* NTVDMEX_VDD_VIDEO_H */

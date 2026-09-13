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
/* ── ★ A VGA BIT-PLANE IS 64KB, NOT "AS MUCH AS THE VISIBLE SCREEN NEEDS". ───────
     This was VID_G12_W * VID_G12_H / 8 = 38400, which is exactly mode 12h's visible
     area. The aperture it serves is A0000-AFFFF, so a guest can address 65536 bytes
     per plane, and the 27136 above the old bound were NOT THERE: vga_planar_read
     returned 0xFF for them and vga_planar_write dropped the write, silently.
   ⚠ That is not an obscure corner. Off-screen VRAM is where DOS games keep sprite
     and background caches and where they page-flip to, and a source read back as
     0xFF is eight pixels of colour 15. Lemmings' level-briefing preview is blitted
     from up there, which is why a 52x17-byte rectangle of it came out solid green.
     With the plane the size the hardware makes it, the bounds check in the engine
     can no longer fail at all -- off is lin - 0xA0000, so it is 16-bit by
     construction. */
#define VID_PLANE_SIZE    65536u                        /* bytes/plane, as on a VGA */
#define VID_Y_PLANE       65536u                        /* mode-Y plane = full 64K */

/* VESA VBE 2.0 (banked, packed-256). A0000 is the 64KB window onto vesa_vram. */
#define VID_VESA_WIN      0x10000u      /* 64KB banked window                     */
#define VID_VESA_VRAM     0x80000u      /* 512KB total emulated VRAM (8 banks)    */
#define VID_FB_MAX        (640 * 480)   /* largest glyph/planar render target     */

/* How a mode is rendered.  Before this, only 13h and 12h were branched on and
   EVERY other mode silently became 80x25 text -- so mode 0 gave 80 columns
   instead of 40, and mode 11h gave a text screen while the program wrote pixels
   into A0000 (GH #39). */
#define VID_KIND_TEXT     0
#define VID_KIND_PLANAR   1             /* 4-plane EGA/VGA, 16 colours             */
#define VID_KIND_LINEAR8  2             /* mode 13h: one byte per pixel            */
#define VID_KIND_CGA      3             /* modes 4/5: 320x200x4, 6: 640x200x2      */
#define VID_KIND_UNSUP    4

/* One recorded planar write -- see `watch` in video_state. */
#define VID_WATCH_MAX 32
typedef struct {
    uint32_t pc;                        /* guest (CS<<16)|IP at the write            */
    uint8_t  wmode, map_mask, ensr, set_reset, frot, bit_mask, cpu;
    uint8_t  latch[4];                  /* the latches the write combined with       */
    uint8_t  after[4];                  /* the four plane bytes it left behind       */
} vid_watch_rec;

/* ── WHO WRITES THE SCREEN, AND WHERE. ──────────────────────────────────────────
     A watchpoint answers "how was THIS byte made" and needs the byte's address; a
     scrolling guest moves its addresses about, so picking one is guesswork. This is
     the other half: every planar write is counted against the guest CS:IP that made
     it, with the lowest and highest VRAM offset that site touched. A missing piece
     of a picture is then either a site that never ran or a site writing somewhere
     unexpected, and both are visible without knowing an address in advance.
   ⚠ DIRECT-MAPPED, not searched: this is on the path of every planar write, of which
     a run makes millions. One index and one compare. A pc that collides with another
     is counted in wsite_lost rather than silently attributed to the wrong routine --
     a histogram that lies about attribution is worse than one with a gap in it. */
/* ⚠ 256 SLOTS AND A MIXED HASH, because the first cut used 64 slots indexed by
   (pc>>2) and lost 568574 reads of 860000 to collisions -- the pcs of a blitter's
   unrolled bodies are only a few bytes apart, so the low bits alone put them all in
   the same slot and the table reported two sites where there were dozens. */
#define VID_WSITES 256
#define VID_WSITE_HASH(pc) ((((pc) >> 1) ^ ((pc) >> 7) ^ ((pc) >> 13)) & (VID_WSITES - 1))
typedef struct { uint32_t pc, n, lo, hi; } vid_wsite;

/* ── THE OFF-SCREEN SPRITE-CACHE WITNESS. ─────────────────────────────────────────
     VID_CACHE_LO is a floor, not a measurement of any particular guest: it is chosen
     to sit ABOVE every screen page a 16-colour mode can have (0x8000 covers 640x480
     planar) and above everything the measured Lemmings run touches for drawing
     (0xDABF at the busiest), while including its panel cache at 0xF91F..0xFFFE. A
     guest that caches sprites lower than this is simply not covered -- which is a
     reported gap, not a wrong answer, because `lost` and the range fields say what
     the table did see. Direction is part of the key: the same routine reading and
     writing the cache is two facts, and the toolbar question is about reads. */
#define VID_CACHE_LO 0xF000u
#define VID_CSITES   10
typedef struct { uint32_t pc, n, lo, hi, first, last; uint8_t wr; } vid_csite;

typedef struct video_state {
    vdd_bus *bus;
    uint8_t *vmem;                      /* the 128KB aperture (A0000); caller-set  */
    uint8_t  mode;                      /* 0x03 text, 0x13 graphics                */
    uint8_t  cols, rows;
    uint16_t gw, gh;                    /* graphics resolution of the current mode  */
    uint8_t  mkind;                     /* VID_KIND_* below                          */
    uint8_t  cga_bpp;                   /* 2 for modes 4/5, 1 for mode 6             */
    uint8_t  cga_pal;                   /* AH=0Bh BH=1: which 4-colour CGA palette   */
    uint8_t  overscan;                  /* AH=0Bh BH=0: border/background colour     */
    uint8_t  blink;                     /* AH=10h AL=03: blink vs bright background  */
    uint8_t  dac_page;                  /* AH=10h AL=13: DAC page state              */
    uint8_t  vpal[17];                  /* the 16 EGA palette registers + border     */
    uint16_t vesa_scanline;             /* 4F06 logical scan line length, pixels     */
    uint16_t vesa_start_x, vesa_start_y;/* 4F07 display start                        */
    uint8_t  vesa_dacwidth;             /* 4F08 bits per DAC primary (6 or 8)        */
    uint8_t  cur_row, cur_col;
    uint16_t cur_shape;                 /* INT 10h AH=01 CX: start/end scan lines    */
    uint8_t  cursor_blink;              /* host setting: blink it, as a real CRTC does */
    uint8_t  page;
    /* ── TWO TABLES, AND CONFLATING THEM IS A BUG. ────────────────────────────
         dac[]  is the DAC: 256 ARGB entries, written by port 0x3C9 and the INT 10h
                palette calls. It is what the GUEST programs.
         pal[]  is the RENDER palette: what the framebuffer's 8-bit values index.
         In a 16-colour mode they are NOT the same table -- the chain is
             pixel(4 bits) -> vpal[pixel] (Attribute Controller) -> DAC index -> dac[]
         so pal[0..15] is a DERIVED view of dac[], rebuilt by pal_refresh(). In 13h
         the AC is bypassed and pal[] is simply dac[]. */
    uint32_t dac[256];                  /* the DAC as the guest programmed it      */
    uint32_t pal[256];                  /* ARGB palette the framebuffer indexes    */
    /* Raster-split bookkeeping over pal[] (see ntvdd_frame and pal_split_note):
       what each entry was at the start of the current frame, what it was set to
       mid-frame and at which row, stamped with the frame number. */
    uint32_t pal_base[256];
    uint32_t pal_split[256];
    uint16_t pal_split_row[256];
    uint32_t pal_split_frame[256];
    uint32_t pal_frame_no;              /* frame the bookkeeping last rebased on   */
    uint32_t pal_split_notes;           /* mid-frame palette writes seen (STAGE2)  */
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
    /* ── ★★ READ MODE 1 IS COLOUR COMPARE, AND IT IS HOW A GAME ASKS THE HARDWARE
         "WHICH OF THESE EIGHT PIXELS ARE SOLID?". A read in mode 1 does not return a
         plane; it returns one BIT PER PIXEL, set where the pixel's 4-bit colour
         matches GR2 (Color Compare) in every plane GR7 (Color Don't Care) says to
         look at. That is pixel-perfect terrain collision in one instruction, which is
         exactly what a platform game does with it.
       ⚠ NONE OF THIS EXISTED: GR5 bit 3 was masked off with `v & 3`, so the read mode
         was thrown away, and GR2 and GR7 fell into `default:` and were dropped. Every
         colour-compare read therefore came back as a raw plane byte -- a plausible
         wrong answer, so the guest ran on and its lemmings walked off the terrain. */
    uint8_t  read_mode;    /* GR5 bit 3: 0 = plane select, 1 = colour compare       */
    uint8_t  col_compare;  /* GR2                                                   */
    uint8_t  col_dontcare; /* GR7: bit n SET = plane n takes part in the compare    */
    uint32_t rmode_hist[2];/* reads served in each mode                             */
    /* CRTC start address: what the guest has written so far, and what the display is
       actually using. They differ between the two byte writes of a page flip -- see
       crtc_out case 0x0C for why rendering from the first is a flicker. */
    uint16_t crtc_start_live;
    uint8_t  crtc_start_pend;   /* 0x0C written, 0x0D not yet                       */
    uint32_t crtc_start_writes; /* completed pairs                                   */
    uint32_t crtc_start_half;   /* frames built mid-pair -- would have been torn      */
    /* ── ATTRIBUTE CONTROLLER (0x3C0/0x3C1). ──────────────────────────────────
         In every 16-colour planar and text mode the 4-bit pixel value indexes
         THESE registers (vpal, above), and the result indexes the DAC. Not
         claiming 0x3C0 meant a guest's palette writes went nowhere -- right
         shapes, wrong colours, static. That is what ailed Lemmings.
       ⚠ INDEX AND DATA SHARE ONE PORT and alternate; the flip-flop is reset by
         READING 0x3DA (see status_in), which is why that read is load-bearing
         and not merely a vblank poll. */
    uint8_t  attr_ff;      /* 0 = next 3C0 write is the index, 1 = the data        */
    uint8_t  attr_index;   /* last index written, including bit5 (video enable)    */
    uint8_t  attr_mode;    /* AR10 Mode Control                                    */
    uint8_t  attr_cse;     /* AR14 Color Select                                    */
    /* ── UNCHAINED ("mode Y") SUPPORT, snapshot-based. ────────────────────────
         Clearing Sequencer reg 4 bit 3 unchains mode 13h: 320x200 becomes four
         planes, pixel (x,y) in plane (x&3) at y*80 + x/4, and programs page-flip
         via the CRTC start address. Doom's DOS build does this.
         We CANNOT demultiplex by trapping A000 -- measured, arming that page trap
         collapsed a Doom run from ~553k log lines to ~55k, because with the trap
         armed the interpreter becomes the CPU and a 32-bit client rewriting the
         framebuffer every frame cannot afford it.
         So: snapshot instead. The guest changes the plane mask only a handful of
         times per frame, and that write goes through a PORT (already cheap to
         intercept). On each mask change we copy the aperture into the planes the
         OLD mask selected. Exact for a program that fills a whole plane before
         switching -- which is what a full-screen blit does -- and approximate for
         one that interleaves planes mid-scan. Documented as an approximation
         because it is one. */
    uint8_t  chain4;       /* SR4 bit 3: 1 = chained (plain 13h), 0 = mode Y       */
    uint8_t  y_mask;       /* map mask in force since the last snapshot            */
    uint8_t  crtc_index;   /* 3D4 index latch                                      */
    uint8_t  crtc_offset;  /* CRTC 0x13: logical line width in 2-byte units        */
    uint16_t crtc_start;   /* CRTC 0x0C/0x0D: display start -- the page-flip reg   */
    uint8_t  yplane[4][VID_Y_PLANE];   /* de-interleaved mode-Y planes             */
    uint8_t  yshadow[VID_Y_PLANE];     /* the aperture as of the last plane flush   */
    uint8_t  crtc_seen;                /* the guest has written a CRTC start address */
    /* ⚠ SEPARATE FROM crtc_seen ON PURPOSE. crtc_offset RESETS TO 40, which is the
         right stride for mode 12h (40 * 2 = 80 bytes a line) and wrong for 0Dh (which
         wants 20 * 2 = 40). So the default cannot be trusted as a value -- only an
         explicit write to CRTC index 0x13 means "this is the stride". */
    uint8_t  crtc_off_seen;            /* the guest has written CRTC Offset (0x13)   */
    /* ── SPLIT SCREEN (Line Compare). ─────────────────────────────────────────
         When the scanline counter reaches Line Compare the address generator
         RESETS TO 0, so everything below that line comes from the start of video
         memory regardless of the pan. It is how a scrolling game keeps a
         stationary status panel -- which is exactly what Lemmings does.
       ⚠ The value is TEN BITS spread over three registers: 0x18 low eight,
         Overflow (0x07) bit 4 = bit 8, Maximum Scan Line (0x09) bit 6 = bit 9.
         Reading only 0x18 gives a value that is silently wrong past line 255. */
    uint16_t crtc_line_compare;
    uint8_t  crtc_overflow;            /* 0x07, for line-compare bit 8            */
    uint8_t  crtc_maxscan;             /* 0x09, bit 6 = line-compare bit 9        */
    uint8_t  crtc_lc_low;              /* 0x18, line-compare bits 0-7             */
    /* ▶ THE VERTICAL TIMING THE GUEST ITSELF PROGRAMMED, so 0x3DA stops being a
         guess keyed off the displayed height. The old model had exactly two cases,
         "400-line" and "480-line", and asserted blanking from line 400 or 480.
         Checked against the MEASURED per-mode tables in vga_defaults.h that is
         right within 8 lines for every mode EXCEPT 0Fh/10h (640x350), where real
         blanking starts at line 355 of 449 and we said 400 -- so we reported a
         10.9% blanking interval where the card gives 20.9%, less than half.
         A two-case table has nowhere to put a 350-line mode.
       ⚠ 640x350 is Lemmings' MENU, not its gameplay: measured from the BDA in a dump
         of the real game, gameplay is mode 0Dh, 320x200, page size 0x2000. Do not
         reach for this fix to explain a gameplay symptom.
         These are the low bytes; the high bits live in crtc_overflow (0x07) and
         crtc_maxscan (0x09), which we already latch for Line Compare. Composed by
         vga_vtiming(), which falls back to the old constants when the guest has not
         programmed the CRTC (off-VM tests set gh directly and never touch it). */
    uint8_t  crtc_vtotal_lo;           /* 0x06, Vertical Total bits 0-7           */
    uint8_t  crtc_vde_lo;              /* 0x12, Vertical Display End bits 0-7     */
    uint8_t  crtc_vbs_lo;              /* 0x15, Vertical Blank Start bits 0-7     */
    uint8_t  crtc_vt_seen;             /* guest has written 0x06 AND 0x12 AND 0x15 */
    /* ▶ The geometry those five registers DESCRIBE, worked out once per CRTC write by
         crtc_vt_recompute(). 0x3DA is the most-read port there is -- Lemmings polls it
         1.6 MILLION times a second -- so reassembling three scattered 10-bit fields and
         revalidating them on every read was 4.4% of that guest's poll budget for an
         answer that only changes when the guest programs the CRTC. vt_valid = 0 means
         "not a plausible screen", and the caller keeps the old two-case constants. */
    uint16_t vt_total, vt_active, vt_blank;
    uint8_t  vt_valid;
    uint32_t modey_gap;                /* mode-Y run coalescing slack, in dwords     */
    /* ── OPTIONAL: PER-PLANE BACKING SUPPLIED BY THE HOST. ───────────────────────
         When these are set, the guest's A0000 window IS whichever plane the map mask
         selects -- the host swaps the mapping on a mask change -- so a guest write
         lands in the right plane and there is nothing to de-interleave afterwards.
         `select` is called with the NEW mask, or -1 for chained/linear; `plane`
         returns a host-side view of a plane, valid whatever is mapped at A0000.
         Left null, the VDD falls back to modey_flush()'s heuristic. */
    void    *ymap_ctx;
    void   (*ymap_select)(void *ctx, int mask);
    void   (*ymap_wmode)(void *ctx, int wmode);   /* GC write mode changed */
    void   (*ymap_readmap)(void *ctx, int plane); /* GR4 read-plane changed -- see gc_set_data */
    uint8_t *(*ymap_plane)(void *ctx, int p);
    uint8_t  write_mode;   /* GR5 bits0-1                                          */
    uint8_t  bit_mask;     /* GR8 (reset 0xFF)                                     */
    uint8_t  latch[4];     /* per-plane read latches                               */
    uint8_t  retrace;      /* Input Status 1 (3DA): legacy toggle, used only if no clock */
    /* CRT TIMEBASE (GH #55 follow-up). Host sets this to a microsecond clock so
       0x3DA reports vertical retrace ON A REAL PERIOD instead of alternating per
       read. Without it, `WAIT &H3DA,8` returns instantly and every program that
       paces on vblank runs unbounded -- measured on BOUNCEBX/MATRIX_2/CAVE.
       NULL (the off-VM default) keeps the old toggle, so tests that do not care
       about timing are unaffected; the video battery injects a fake clock. */
    uint64_t (*time_us)(void);
    /* WHAT THE GUEST ACTUALLY SEES on 0x3DA. `vbl_edges` counts clear->set
       transitions as observed BY THE GUEST's own reads, which is its real frame
       rate: one `WAIT &H3DA,8` completes per edge. Without this, "is it paced?"
       is a matter of opinion about how fast the screen looks; with it, it is
       edges/second against an expected 60 or 70. `p3da_reads` is the poll count,
       so the two together also say how hard the guest is spinning. */
    uint32_t vbl_edges;
    uint32_t p3da_reads;
    uint8_t  vbl_prev;
    /* ▶ IS THE TIMEBASE UNDER THE RETRACE ACTUALLY WALL-CLOCK TIME? `vbl_edges` says
         the guest completes N frames a second, but that is only a statement about the
         CARD if the clock beneath it runs at real speed. Both of the obvious readings
         of a low edge count -- "the guest is missing vblanks" and "the model is slow"
         -- predict the same vbl_edges, so that counter cannot separate them and no
         amount of staring at it will.
         These can. t3da_first/t3da_last bracket the polling in MODEL microseconds,
         read from the SAME clock status_in derives the bits from, so the two cannot
         disagree about their units; the caller compares that span against the run's
         real length. A model span 20x shorter than the run says the retrace is slow
         because the CLOCK is slow, and the video model is innocent.
         dt3da_max is the largest gap between two consecutive polls: a guest can only
         legitimately miss a vblank if it was away longer than one blanking interval,
         so if this stays well under a frame the guest missed nothing. */
    uint64_t t3da_first, t3da_last;
    /* The guest's previous poll of bit 0: which scanline (absolute, frames included)
       and what it read. A poll in a LATER line whose predecessor saw the display
       active is owed the blanking that passed between them -- see status_in. */
    uint64_t p3da_last_line;
    uint8_t  p3da_last_bit0, p3da_have_last;
    uint32_t p3da_hbl_owed;   /* blanks reported by that rule (STAGE2)               */
    uint32_t p3da_hbl_debt;   /* blanks still owed: whole lines crossed unsampled     */
    uint8_t  p3da_synth_hi;   /* a synthetic blank is half-way through (1 reported)   */
    uint8_t  p3da_last_vbl;   /* the previous poll was in vertical blanking          */
    /* ── ⚠ DEBUG SCAFFOLDING, OFF UNLESS ASKED FOR. ─────────────────────────────────
         The last VID_P3DA_RING polls: model microseconds and the byte returned. A
         scanline-counting loop is ~1000 polls in 85 million, invisible in any
         histogram, so the host dumps this ring when the guest latches the 8254 --
         which is how such a loop ends (Lemmings' calibration, s69).
       ⚠⚠ `ring_on` GATES IT, AND DEFAULTS TO 0, because both halves are the kind of
         cost this project has been bitten by: the stores sit on 0x3DA, the single
         hottest path in the program (85 MILLION reads in a Lemmings run, and the
         port trap is the measured ceiling), and the dump is ~43 log_append calls
         made from iio_out -- i.e. FILE I/O UNDER g_lock from inside the planar
         interpreter. I shipped both to the user in s69 by default; a build a person
         plays on must not carry an instrument that heavy. `cfg\pitlatch.flag`. */
#define VID_P3DA_RING 1024
    uint8_t  p3da_ring_on;
    uint32_t p3da_ring_us[VID_P3DA_RING];
    uint8_t  p3da_ring_v[VID_P3DA_RING];
    uint32_t p3da_ring_n;
    uint32_t dt3da_max;
    uint32_t dt3da_zero;   /* polls across which the clock did not advance at all */
    /* A MAXIMUM IS ONE EVENT AND CANNOT CARRY A RATE. dt3da_max says the guest was
       once away for seconds; what decides the frame rate is how OFTEN it is away
       longer than a blanking interval (~1.9ms), because each of those can step over
       a whole vblank unseen. Buckets are powers of four in microseconds, so the
       window and the frame period land in known ones: [5] is 1024-4095us (straddles
       the ~1.9ms window) and [6]/[7] are longer than a 14.3ms frame outright. */
    uint32_t dt3da_hist[8];
    uint32_t int10_11_calls;/* INT 10h AH=11h (character generator) calls -- see below */
    /* WHAT the guest asked for, not just that it asked. BH selects the table and the
       answer's CX (bytes per character) is what the caller strides by -- so a wrong CX
       misaligns every glyph after the first, which looks exactly like garbled text.
       Recorded per call so the log says which table Skyroads wants. */
    struct { uint8_t al, bh; uint16_t seg, off, cx; } font_q[4];
    /* ── GH #52: A USER-LOADED TEXT FONT, AND THE RENDERER READS IT. ──────────
         INT 10h AH=11h AL=00h/10h loads a caller-supplied character generator.
         We used to accept those calls, announce them as unimplemented and draw
         from the ROM table anyway -- so a program that loaded its own glyphs got
         the stock ones and no error. `user_font_rows` is bytes per character as
         the caller declared it; rows beyond it are blanked, because a cell is
         VID_CELL_H tall whatever the font supplies.
         `user_font_on` stays 0 until a load actually lands, so the ordinary case
         costs one branch and draws from the ROM exactly as before. */
    uint8_t  user_font[256 * 16];
    uint8_t  user_font_rows;
    uint8_t  user_font_on;
    uint8_t  font_qn;
    /* Every mode set, with what it RESOLVED TO. Added after a mode-table change
       silently altered mode 12h rendering: the STAGE2 line said "modes
       unsupported: none" and the picture was still wrong, so "which modes were
       refused" was not the question -- "what did each accepted mode become" was. */
    struct { uint8_t mode, kind, cols, rows; uint16_t w, h; } mode_q[8];
    uint8_t  mode_qn;
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
    /* Mode-Y de-interleave instrumentation. `plane-nonzero` in STAGE2 has always
       counted st->plane[] -- the 16-colour PLANAR array -- which mode Y never touches,
       so it reported four zeroes for every unchained run ever made. These are the
       arrays that actually carry a mode-Y frame. */
    uint32_t mask_hist[16];             /* map-mask values written, by value          */
    uint32_t wmode_hist[4];             /* GC write modes selected (1 = LATCH COPY)   */
    /* ── WHAT THE REGISTERS HELD AT *WRITE* TIME, not at end of run. ──────────
         wmode_hist/mask_hist above count register PROGRAMMING; a snapshot of
         enable_sr taken when the guest exits says nothing about what it held
         during the thousands of writes before that. These are incremented inside
         vga_planar_write, so they are a history rather than a final state. */
    uint32_t w_ensr_hist[16];           /* Enable Set/Reset live at each write     */
    uint32_t w_alu_hist[4];             /* GR3 ALU function live at each write      */
    uint32_t w_p3_sr;                   /* writes where plane 3 took set/reset      */
    uint32_t w_p3_nz;                   /* ...of which stored a NON-ZERO byte       */
    uint32_t w_p3_data;                 /* writes where plane 3 took the CPU byte   */
    uint32_t ac_port_writes;            /* palette registers written via 0x3C0       */
    uint32_t ac_bios_writes;            /* ...and via INT 10h AH=10h                 */
    uint32_t dac_writes;                /* DAC entries written (3C9 + INT 10h)       */
    /* s70 instrument: WHERE ON THE SCREEN the guest writes its DAC. Bands: rows 0-1,
       2-159, 160+ (toolbar), vertical blanking. dac_last_row = the last write's row
       (0xFFFF = blanking, 0xFFFE = no clock). */
    uint32_t dac_row_hist[4];
    uint16_t dac_last_row;
    /* Which DAC entries the guest actually programs, in 16-entry blocks. The whole
       Lemmings palette question is "does it write 0x38..0x3F", and a total count
       cannot answer that. */
    uint32_t dac_block[16];
    uint8_t  def_pal_off;               /* INT 10h AH=12h BL=31h: suppress the reload */
    uint32_t pal_resets;                /* load_default_palette() calls              */
    uint32_t dac_hi_since_reset;        /* block-3 DAC writes since the last reset   */
    /* ⚠ ...and the running maximum over all reset epochs. The counter above is read
       after the guest has exited, and a guest exits through a mode set, so on its
       own it reads zero for every guest that ever ran. */
    uint32_t dac_hi_max;
    /* ── A VRAM WATCHPOINT: EVERY WRITE TO ONE BYTE, WITH THE REGISTERS THAT MADE IT.
         A histogram says which idioms a run used; it cannot say which idiom produced
         the wrong pixel, because every idiom is in the histogram. This records the
         whole write -- mode, map mask, Enable Set/Reset, Set/Reset, ALU, bit mask,
         the CPU byte, the latches, and the four plane bytes afterwards -- for one
         chosen offset, plus the guest CS:IP so the routine responsible can be found
         in a disassembly. Armed from cfg/vwatch.txt; off by default.
       ⚠ BOUNDED ON PURPose: a guest-driven trace outruns the guest. The first
         VID_WATCH_MAX writes are kept and the LAST one separately, because the last
         write is the one that decides what is on screen. */
    /* The highest VRAM byte offset the guest has touched, read or written. Answers
       "does this guest use off-screen VRAM?" for any guest, which is what the old
       38400-byte plane made unanswerable -- everything above it read back as 0xFF
       and looked like the guest's own data. */
    uint32_t planar_hi_water;
    vid_wsite wsite[VID_WSITES];        /* write sites, by guest CS:IP               */
    uint32_t wsite_lost;                /* writes whose pc collided with another      */
    /* And the same for READS. A picture that is missing something is as often a copy
       whose SOURCE was never read as a write that never happened -- with off-screen
       VRAM in play, "who reads up there" is the question that separates the two. */
    vid_wsite rsite[VID_WSITES];
    uint32_t rsite_lost;
    /* Colour-compare reads get their OWN table. They are a tiny minority of reads and
       would be buried in the one above, and they are the interesting ones: a read mode
       1 site is a guest asking "where is the ground", so its pc is a routine worth
       disassembling and its address range says WHICH copy of the level it trusts. */
    vid_wsite rsite1[VID_WSITES];
    uint32_t rsite1_lost;
    /* ▶ WHAT THE COMPARE ACTUALLY ANSWERED, per site. A colour-compare read is the only
         VRAM read whose RESULT is a decision rather than a pixel, and Lemmings has one
         site (guest 0x7A3A) that reads two bytes, counts the set bits and branches on
         whether at least 8 of 16 pixels are terrain. A count of reads says that site
         ran; it cannot say the game could SEE anything. If `zero` is essentially equal
         to `n` at that site, every pixel it asked about came back "not terrain" -- which
         is a lemming walking into thin air, and is indistinguishable, in every counter
         we had before this, from a site that is working perfectly. */
    uint32_t rsite1_zero[VID_WSITES];   /* compares that returned 0x00 -- nothing matched */
    uint32_t rsite1_ones[VID_WSITES];   /* compares that returned 0xFF -- everything did  */
    /* ── ▶ WHO TOUCHES THE OFF-SCREEN SPRITE CACHE -- A LINEAR TABLE, NOT A HASH. ──
         The three tables above are 256 single-slot hashes, so a site whose pc collides
         with a busier one is dropped into a `_lost` counter and NEVER APPEARS. On the
         Lemmings run that cost 249,630 reads, which makes those tables unable to answer
         the one question the toolbar bug turns on: does the routine that copies the
         composed panel onto the screen RUN AT ALL? An absence there is indistinguishable
         from a collision, and reasoning from it is exactly the trap the site histograms
         have already sprung twice.
         This table cannot collide: it is a short LINEAR scan, and it only admits
         accesses above VID_CACHE_LO -- a region no other site in the measured run comes
         near (the busiest top out at 0xDABF), so ten slots is generous rather than tight.
         `lost` counts accesses that found the table full, and a nonzero value invalidates
         only the CLAIM OF COMPLETENESS, never the entries themselves.
       ▶ AND THE ORDER THEY CAME IN. A panel that is composed and then blitted looks
         identical, by count, to one that is blitted and then composed -- but the second
         puts an empty cache on the screen, which is the reported symptom. Counts cannot
         tell those apart and no number of them will. `first`/`last` are ticks of a
         counter that advances only on cache accesses, so comparing the compositor's
         first write against the blitter's first read settles the ordering outright. */
    vid_csite csite[VID_CSITES];
    uint32_t  csite_lost;
    uint32_t  csite_seq;                /* ticks once per cache-region access          */
    uint32_t watch_off;                 /* VRAM byte offset watched; ~0u = disarmed  */
    uint32_t watch_n;                   /* writes seen (may exceed what is recorded) */
    vid_watch_rec watch[VID_WATCH_MAX];
    vid_watch_rec watch_last;
    uint32_t (*guest_pc)(void);         /* host hook: (CS<<16)|IP, 0 if unavailable  */
    uint32_t mw_hist[64];               /* (write mode, map mask) pairs -- see seq_out */
    uint32_t mask_skip_chain4;          /* map-mask writes dropped: chained            */
    uint32_t mask_skip_same;            /* map-mask writes dropped: value unchanged    */
    uint32_t gr4_hist[4];               /* GR4 read-plane values written, by value      */
    uint32_t chain4_sel;                /* ymap_select calls made by a CHAIN4 change,
                                           not by a map-mask write -- the map-mask
                                           identity has to subtract these or it will
                                           show a residual that is not a lost write   */
    uint32_t ysnap[4];                  /* snapshots taken into each mode-Y plane      */
    uint32_t ynz[4];                    /* busiest snapshot each plane ever received   */
} video_state;

#define VID_UNIMPL_SET(bm, n)  ((bm)[((n) & 0xFF) >> 3] |= (uint8_t)(1u << ((n) & 7)))
#define VID_UNIMPL_GET(bm, n)  (((bm)[((n) & 0xFF) >> 3] >> ((n) & 7)) & 1u)

int  vdd_video_init(vdd_bus *b, void *self);
void vdd_video_reset(void *self);
/* Stamp the frame with the current frame number and the raster-split arrays. The
   host calls this under its lock right before it snapshots the frame; tests call it
   before resolving colours with ntvdd_frame_pal_at. */
void vdd_video_frame_touch(video_state *st);
static inline ntvdd vdd_video_device(video_state *st)
{ ntvdd d; d.name = "video"; d.init = vdd_video_init; d.reset = vdd_video_reset;
  d.shutdown = 0; d.self = st; return d; }

void vdd_video_render(video_state *st);                /* text glyph render        */
/* ── ★ CURSOR EMULATION: AN 8-LINE SHAPE ON A 16-LINE CELL. ─────────────────────
     DOS asks for its cursor in SCAN LINES, and it asks in the units of the machine
     it was written for -- an 8-line character cell, where an underline is lines 6-7
     and the insert-mode block is 0-7. Our cell is 16 lines (an 8x16 VGA font), so
     honouring those numbers literally puts the underline HALFWAY UP THE CELL, which
     is what "ABC123-" instead of "ABC123_" looks like. It was our own default doing
     it too: cur_shape starts at 0x0607.
   ► Real VGA BIOSes solve this with CURSOR EMULATION, and this is their rule (the
     IBM/Bochs/SeaBIOS one, followed exactly rather than approximated): when the cell
     is taller than 8 and the request is in 8-line units, scale it up. Which is also
     what gives MS-DOS's insert/overwrite cursors for free -- 6-7 becomes 14-15, a
     bottom underline, and 0-7 becomes 1-15, a full block -- because those are the
     two shapes DOS sets when you press Insert.
   Pure arithmetic on the shape word, so tools/dostest/video_test.c can pin the exact
   shapes DOS uses. `hidden` is set for the two idioms that mean "no cursor". */
void vdd_cursor_lines(uint16_t shape, unsigned cell_h,
                      unsigned *start, unsigned *end, int *hidden);
void vdd_video_putc(video_state *st, uint8_t ch);      /* console teletype sink    */

/* Planar A0000 access (mode 12h): the host calls these from the memory-write trap
   so direct framebuffer writes run through the VGA write-modes into the 4 planes.
   `off` is the byte offset within the A0000 window. */
void    vga_planar_write(video_state *st, uint32_t off, uint8_t cpu);
uint8_t vga_planar_read (video_state *st, uint32_t off);   /* loads latches        */
int     vdd_video_planar_active(const video_state *st);    /* 1 in mode 12h        */
/* 1 when the emulated CRT is in the tail of its active period -- the guest has
   finished drawing this frame and is parked waiting for retrace, so a snapshot
   taken now is a WHOLE frame. Presenting at an arbitrary phase is what makes a
   program that erases-then-redraws (BOUNCEBX) tear: catch it between the two and
   the object is simply missing. Returns 1 unconditionally with no clock injected. */
int     vdd_video_present_ready(video_state *st);

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

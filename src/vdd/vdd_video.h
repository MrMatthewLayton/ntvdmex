/*
 * vdd_video.h -- the video VDD: text mode 3 + graphics mode 13h.  (M3, ADR-0008)
 *
 * Owns the emulated VGA display. The guest's video memory aperture (A0000-BFFFF,
 * 128KB) is mapped as RAM, so both INT 10h/console writes AND direct-framebuffer
 * writes land in the same `vmem`; the VDD renders it each frame into an NTVDD_FRAME
 * for present_ddraw:
 *   - text mode 3: B8000 cell grid -> 8x16 font, 16-colour EGA palette (640x400)
 *   - mode 13h:    A0000 linear 320x200x256, palette indices straight to present
 * The DAC ports (3C7-3C9) + INT 10h AH=10h drive the 256-entry palette. Pure C,
 * no <windows.h>: set `st->VideoMemory` (host: 0xA0000 absolute; test: a 128KB buffer)
 * before VddBusAdd(), and the whole VDD is exercised off-VM.
 */
#ifndef NTVDMEX_VDD_VIDEO_H
#define NTVDMEX_VDD_VIDEO_H

#include "vdd_bus.h"

#define VIDEO_APERTURE_BASE 0xA0000u      /* video memory window base               */
#define VIDEO_APERTURE_SIZE 0x20000u      /* A0000-BFFFF (128KB)                    */
#define VIDEO_TEXT_BASE     0xB8000u      /* colour-text page 0                     */
#define VIDEO_TEXT_OFFSET      (VIDEO_TEXT_BASE - VIDEO_APERTURE_BASE)  /* 0x18000         */
#define VIDEO_COLUMNS          80
#define VIDEO_ROWS          25
#define VIDEO_CELL_WIDTH        8
#define VIDEO_CELL_HEIGHT        16
#define VIDEO_FRAME_WIDTH          (VIDEO_COLUMNS * VIDEO_CELL_WIDTH)   /* 640 (text render target)   */
#define VIDEO_FRAME_HEIGHT          (VIDEO_ROWS * VIDEO_CELL_HEIGHT)   /* 400                        */
#define VIDEO_MODE13_WIDTH         320
#define VIDEO_MODE13_HEIGHT         200
#define VIDEO_MODE12_WIDTH         640           /* mode 12h: 640x480x16 planar            */
#define VIDEO_MODE12_HEIGHT         480
/* ── ★ A VGA BIT-PLANE IS 64KB, NOT "AS MUCH AS THE VISIBLE SCREEN NEEDS". ───────
     This was VIDEO_MODE12_WIDTH * VIDEO_MODE12_HEIGHT / 8 = 38400, which is exactly mode 12h's visible
     area. The aperture it serves is A0000-AFFFF, so a guest can address 65536 bytes
     per plane, and the 27136 above the old bound were NOT THERE: VddVideoPlanarRead
     returned 0xFF for them and VddVideoPlanarWrite dropped the write, silently.
   ⚠ That is not an obscure corner. Off-screen VRAM is where DOS games keep sprite
     and background caches and where they page-flip to, and a source read back as
     0xFF is eight pixels of colour 15. Lemmings' level-briefing preview is blitted
     from up there, which is why a 52x17-byte rectangle of it came out solid green.
     With the plane the size the hardware makes it, the bounds check in the engine
     can no longer fail at all -- off is lin - 0xA0000, so it is 16-bit by
     construction. */
#define VIDEO_PLANE_SIZE    65536u                        /* bytes/plane, as on a VGA */
#define VIDEO_Y_PLANE_SIZE       65536u                        /* mode-Y plane = full 64K */

/* VESA VBE 2.0 (banked, packed-256). A0000 is the 64KB window onto VesaVram. */
#define VIDEO_VESA_WINDOW      0x10000u      /* 64KB banked window                     */
/* ── ★ VRAM HAS TO HOLD THE DEEPEST MODE WE ADVERTISE, NOT THE SHALLOWEST. (s74)
     512KB was exactly enough for 800x600x8 and nothing else. Advertising hi-colour
     means 800x600x24 = 1,440,000 bytes, so a mode the guest is allowed to SET must
     have somewhere to live -- a mode list that promises more than VRAM can back is
     a promise the first blit breaks. 2MB covers every mode in vesa_modes[]. */
#define VIDEO_VESA_VRAM     0x400000u     /* 4MB: enough for 1024x768x24 (s74b)      */
/* ── THE LFB'S ADVERTISED PHYSICAL ADDRESS. ───────────────────────────────────────
     4F01 reports this as PhysBasePtr and the guest asks DPMI 0800 to map it; the
     host answers with the host VA of VesaVram, which is exactly the convention
     0501 already uses (it hands the client a host VA as its linear address). Both
     halves must agree on the number, so it lives here rather than twice.
     0xE0000000 is where a PCI video aperture normally sits, so it cannot collide
     with anything the guest has a right to expect at a lower address. */
#define VIDEO_VESA_LFB_PHYSICAL 0xE0000000u
#define VIDEO_VESA_MAX_WIDTH     NTVDD_FRAME_MAX_WIDTH   /* widest mode advertised = what the presenter shows */
#define VIDEO_VESA_MAX_HEIGHT     NTVDD_FRAME_MAX_HEIGHT
#define VIDEO_FRAME_MAX        (NTVDD_FRAME_MAX_WIDTH * NTVDD_FRAME_MAX_HEIGHT)   /* glyph/planar render target: 132 cols x 60 rows fits */

/* How a mode is rendered.  Before this, only 13h and 12h were branched on and
   EVERY other mode silently became 80x25 text -- so mode 0 gave 80 columns
   instead of 40, and mode 11h gave a text screen while the program wrote pixels
   into A0000 (GH #39). */
#define VIDEO_KIND_TEXT     0
#define VIDEO_KIND_PLANAR   1             /* 4-plane EGA/VGA, 16 colours             */
#define VIDEO_KIND_LINEAR8  2             /* mode 13h: one byte per pixel            */
#define VIDEO_KIND_CGA      3             /* modes 4/5: 320x200x4, 6: 640x200x2      */
#define VIDEO_KIND_UNSUPPORTED    4

/* One recorded planar write -- see `Watch` in VIDEO_STATE. */
#define VIDEO_WATCH_MAX 32
typedef struct _VIDEO_WATCH_RECORD {
    UINT32 Pc;                        /* guest (CS<<16)|IP at the write            */
    BYTE  WriteMode, MapMask, EnableSetReset, SetReset, FunctionRotate, BitMask, Cpu;
    BYTE  Latch[4];                  /* the latches the write combined with       */
    BYTE  After[4];                  /* the four plane bytes it left behind       */
} VIDEO_WATCH_RECORD, *PVIDEO_WATCH_RECORD;
typedef const VIDEO_WATCH_RECORD *PCVIDEO_WATCH_RECORD;

/* ── WHO WRITES THE SCREEN, AND WHERE. ──────────────────────────────────────────
     A watchpoint answers "how was THIS byte made" and needs the byte's address; a
     scrolling guest moves its addresses about, so picking one is guesswork. This is
     the other half: every planar write is counted against the guest CS:IP that made
     it, with the lowest and highest VRAM offset that site touched. A missing piece
     of a picture is then either a site that never ran or a site writing somewhere
     unexpected, and both are visible without knowing an address in advance.
   ⚠ DIRECT-MAPPED, not searched: this is on the path of every planar write, of which
     a run makes millions. One index and one compare. A pc that collides with another
     is counted in WriteSitesLost rather than silently attributed to the wrong routine --
     a histogram that lies about attribution is worse than one with a gap in it. */
/* ⚠ 256 SLOTS AND A MIXED HASH, because the first cut used 64 slots indexed by
   (pc>>2) and lost 568574 reads of 860000 to collisions -- the pcs of a blitter's
   unrolled bodies are only a few bytes apart, so the low bits alone put them all in
   the same slot and the table reported two sites where there were dozens. */
#define VIDEO_SITES 256
#define VIDEO_SITE_HASH(pc) ((((pc) >> 1) ^ ((pc) >> 7) ^ ((pc) >> 13)) & (VIDEO_SITES - 1))
typedef struct _VIDEO_SITE { UINT32 Pc, Count, Low, High; } VIDEO_SITE, *PVIDEO_SITE;
typedef const VIDEO_SITE *PCVIDEO_SITE;

/* ── THE OFF-SCREEN SPRITE-CACHE WITNESS. ─────────────────────────────────────────
     VIDEO_CACHE_LOW is a floor, not a measurement of any particular guest: it is chosen
     to sit ABOVE every screen page a 16-colour mode can have (0x8000 covers 640x480
     planar) and above everything the measured Lemmings run touches for drawing
     (0xDABF at the busiest), while including its panel cache at 0xF91F..0xFFFE. A
     guest that caches sprites lower than this is simply not covered -- which is a
     reported gap, not a wrong answer, because `lost` and the range fields say what
     the table did see. Direction is part of the key: the same routine reading and
     writing the cache is two facts, and the toolbar question is about reads. */
#define VIDEO_CACHE_LOW 0xF000u
#define VIDEO_CACHE_SITES   10
typedef struct _VIDEO_CACHE_SITE { UINT32 Pc, Count, Low, High, First, Last; BYTE IsWrite; } VIDEO_CACHE_SITE, *PVIDEO_CACHE_SITE;
typedef const VIDEO_CACHE_SITE *PCVIDEO_CACHE_SITE;

/* INT 10h AH=11h calls as asked (VIDEO_STATE.FontQueries) and mode sets as resolved
   (VIDEO_STATE.ModeQueries). */
typedef struct _VIDEO_FONT_QUERY { BYTE Al, Bh; WORD Segment, Offset, Cx; } VIDEO_FONT_QUERY;
typedef struct _VIDEO_MODE_QUERY { BYTE Mode, Kind, Columns, Rows; WORD Width, Height; } VIDEO_MODE_QUERY;

typedef struct _VIDEO_STATE {
    PVDD_BUS Bus;
    BYTE *VideoMemory;                      /* the 128KB aperture (A0000); caller-set  */
    BYTE  Mode;                      /* 0x03 text, 0x13 graphics                */
    BYTE  Columns, Rows;
    /* ── THE CELL HEIGHT IS THE GUEST'S, NOT A COMPILE-TIME 16. ───────────────────
         A VGA text mode has 400 scan lines and the BIOS divides them by whatever font
         is loaded: 8x16 gives 25 rows, 8x14 gives 28, 8x8 gives 50 -- and INT 10h
         AX=1112h (load the ROM 8x8) is how edit.com, QBasic, DOSSHELL and every
         "43/50 line" option get there. With VIDEO_CELL_HEIGHT hard-wired to 16 that call
         cleared the user font and changed nothing else, so a 50-row screen was drawn
         as 25 rows of 16-line glyphs over a buffer the guest laid out as 50. */
    BYTE  CellHeight;                    /* scan lines per text row: 8, 14 or 16     */
    BYTE  IsBlinkOffPhase;                 /* this render: blinking cells are in their off phase */
    /* The BIOS DATA AREA fields that describe the display (0040:0049..008A). A text
       application reads them rather than asking: rows-1 at 0040:0084 is how QBasic
       and edit.com size their screen, 0040:004A the columns, 0040:0050 the cursor.
       NULL in the off-VM battery unless a test wires a buffer; the host points it at
       guest linear 0x400, as the input VDD does for the keyboard ring. Nothing wrote
       any of these before, so every one read as zero. */
    BYTE *BiosData;
    WORD GraphicsWidth, GraphicsHeight;                    /* graphics resolution of the current mode  */
    BYTE  ModeKind;                     /* VID_KIND_* below                          */
    BYTE  CgaBpp;                   /* 2 for modes 4/5, 1 for mode 6             */
    BYTE  CgaPalette;                   /* AH=0Bh BH=1: which 4-colour CGA palette   */
    BYTE  Overscan;                  /* AH=0Bh BH=0: border/background colour     */
    BYTE  IsBlink;                     /* AH=10h AL=03: blink vs bright background  */
    BYTE  DacPage;                  /* AH=10h AL=13: DAC page state              */
    BYTE  PaletteRegisters[17];                  /* the 16 EGA palette registers + border     */
    WORD VesaStartX, VesaStartY;/* 4F07 display start (pixels, rows); the      */
                                        /* 4F06 logical pitch lives in VesaStride    */
    /* ── THE VESA DISPLAY START ON THE HARDWARE'S SCHEDULE (#226), the same three stages
         as the CRTC start (CrtcStart -> StartVs -> CrtcStartLive, see vid_latch):
         the start as a BYTE offset into VesaVram as the "register" holds it, as loaded
         at the last retrace start, and as the displayed frame uses it. A byte offset
         because VBE 3.0's 4F07 BL=02h/82h hand one over directly; BL=00h/80h derive it
         from (x, y) at the current pitch. VesaStartX/Y stay what BL=01h reports. */
    UINT32 VesaOrigin, VesaOriginVs, VesaOriginLive;
    /* ── 4F07 BL=80h/82h "SET DISPLAY START DURING VERTICAL RETRACE" MUST NOT RETURN
         BEFORE THE RETRACE. The INT 10h handler runs under the host lock, so it cannot
         spin there; it computes when the call completes, in the model's microseconds
         (st->TimeUs), and the HOST honours it after releasing the lock -- see
         VddVideoInt10WaitUs(). 0 = the last call completes at once. */
    UINT64 Int10WaitUntil;
    UINT32 Vesa07Vbl;               /* 1 + frame number whose retrace last released a
                                           BL=80h/82h call: one release per retrace       */
    UINT32 Vesa07Waits;             /* BL=80h/82h calls that had to wait (STAGE2)     */
    UINT64 Vesa07WaitUs;           /* ...and the total they were asked to wait        */
    BYTE  VesaDacWidth;             /* 4F08 bits per DAC primary (6 or 8) -- the */
                                        /* RAMDAC's width: 3C9h, AH=10h and 4F09 all */
                                        /* obey it (#226); any mode set resets it to 6 */
    BYTE  CursorRow, CursorColumn;
    WORD CursorShape;                 /* INT 10h AH=01 CX: start/end scan lines    */
    BYTE  IsCursorBlink;              /* host setting: blink it, as a real CRTC does */
    BYTE  Page;
    /* ── #252: THE BIOS KEEPS ONE CURSOR PER PAGE (0040:0050, eight words). CursorRow/
         CursorColumn stay THE ACTIVE PAGE's (everything that draws the cursor reads them);
         PageRow/PageColumn hold the other seven, swapped in and out by AH=05h. */
    BYTE  PageRow[8], PageColumn[8];
    /* AH=12h state a later call or mode set acts on (#252):
         ScanSelect   BL=30h: 0 = 200, 1 = 350, 2 = 400 lines for the next TEXT mode set
         IsGreySum   BL=33h: sum the DAC to grey on mode-set / AH=10h DAC loads
         IsCursorEmulationOff BL=34h: CGA cursor emulation off -- AH=01h CX taken literally
         IsVideoOff    BL=32h: CPU addressing of video memory disabled (recorded only)  */
    BYTE  ScanSelect, IsGreySum, IsCursorEmulationOff, IsVideoOff;
    /* ── TWO TABLES, AND CONFLATING THEM IS A BUG. ────────────────────────────
         dac[]  is the DAC: 256 ARGB entries, written by port 0x3C9 and the INT 10h
                palette calls. It is what the GUEST programs.
         pal[]  is the RENDER palette: what the framebuffer's 8-bit values index.
         In a 16-colour mode they are NOT the same table -- the chain is
             pixel(4 bits) -> vpal[pixel] (Attribute Controller) -> DAC index -> dac[]
         so pal[0..15] is a DERIVED view of dac[], rebuilt by pal_refresh(). In 13h
         the AC is bypassed and pal[] is simply dac[]. */
    UINT32 Dac[256];                  /* the DAC as the guest programmed it      */
    UINT32 Palette[256];                  /* ARGB palette the framebuffer indexes    */
    /* Raster-split bookkeeping over pal[] (see NTVDD_FRAME and pal_split_note):
       what each entry was at the start of the current frame, what it was set to
       mid-frame and at which row, stamped with the frame number. */
    UINT32 PaletteBase[256];
    UINT32 PaletteSplit[256];
    WORD PaletteSplitRow[256];
    UINT32 PaletteSplitFrame[256];
    UINT32 PaletteFrameNumber;              /* frame the bookkeeping last rebased on   */
    UINT32 PaletteSplitNotes;           /* mid-frame palette writes seen (STAGE2)  */
    /* DAC (ports 3C7/3C8/3C9) write/read state */
    BYTE  DacWriteIndex, DacReadIndex, DacComponent, DacLatch[3];
    /* VESA VBE state */
    BYTE  IsVesa;                   /* a VESA mode is active                   */
    WORD VesaMode, VesaWidth, VesaHeight; /* current VESA mode + resolution          */
    WORD VesaBank;                 /* current 64KB window bank (4F05)         */
    BYTE  VesaBpp;                  /* 8/15/16/24 -- bits per pixel of the mode */
    UINT32 VesaStride;               /* bytes per scan line of the current mode  */
    BYTE  IsVesaLfb;                  /* the mode was set with bit 14: LFB in use  */
    WORD VesaModeFlags;           /* D14|D15 of the last 4F02 (4F03 returns them, #226) */
    BYTE  IsModeSetNoClear;           /* the last mode set (AH=00h AL bit 7 / 4F02h D15) did
                                           not clear memory: BDA 40:87h bit 7 (#226)    */
    /* Where the guest ACTUALLY wrote in the framebuffer, in raw buffer offsets --
       independent of how WE choose to interpret stride/origin. The only way to tell
       "the demo put its picture there" from "we are reading the buffer wrong". */
    UINT32 VramLow, VramHigh, VramNonZero;
    /* Hi-colour frames are handed to the presenter as 32-bit ARGB: the frame
       contract has a bpp field but every consumer indexed a palette, so a direct
       colour mode has to be converted somewhere. Here is the only place that knows
       the guest's pixel format, so here is where it converts. */
    UINT32 VesaArgb[(UINT32)VIDEO_VESA_MAX_WIDTH * VIDEO_VESA_MAX_HEIGHT];
    BYTE  VesaVram[VIDEO_VESA_VRAM];  /* full packed-256 framebuffer             */
    BYTE  Planes[4][VIDEO_PLANE_SIZE];  /* mode 12h: 4 bit-planes (640x480x16)     */
    /* VGA planar write engine (Sequencer 3C4/3C5 + Graphics Controller 3CE/3CF) */
    BYTE  SequencerIndex, GcIndex;
    BYTE  MapMask;     /* SR2: planes enabled for writes (reset 0x0F)          */
    BYTE  SetReset;    /* GR0                                                  */
    BYTE  EnableSetReset;    /* GR1                                                  */
    BYTE  FunctionRotate;  /* GR3: bits0-2 rotate count, bits3-4 ALU               */
    BYTE  ReadMap;     /* GR4: plane read in read-mode 0                       */
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
    BYTE  ReadMode;    /* GR5 bit 3: 0 = plane select, 1 = colour compare       */
    BYTE  ColorCompare;  /* GR2                                                   */
    BYTE  ColorDontCare; /* GR7: bit n SET = plane n takes part in the compare    */
    UINT32 ReadModeHistogram[2];/* reads served in each mode                             */
    /* CRTC start address: what the guest has written so far, and what the display is
       actually using. They differ between the two byte writes of a page flip -- see
       crtc_out case 0x0C for why rendering from the first is a flicker. */
    WORD CrtcStartLive;
    BYTE  IsCrtcStartPending;   /* 0x0C written, 0x0D not yet                       */
    UINT32 CrtcStartWrites; /* completed pairs                                   */
    UINT32 CrtcStartHalf;   /* frames built mid-pair -- would have been torn      */
    /* ── WHAT THE DISPLAYED FRAME USES, ON THE HARDWARE'S SCHEDULE (s83, Mario). ─────
         The address counter loads the start address at the START OF VERTICAL RETRACE;
         pel panning (AR13) is taken as the next frame's picture begins. A smooth
         scroller writes the start during display, waits for retrace, then writes the
         pan -- both land on the SAME next frame. We used to take the start whenever the
         host drew and ignored AR13, so Mario moved in 4-pixel jumps, a frame out of
         step. See vid_latch(). */
    WORD StartVs;          /* start address as loaded at the last retrace start */
    BYTE  DisplayPan;          /* AR13 as the displayed frame uses it               */
    UINT64 LatchTime;           /* when vid_latch last ran (0 = never / reset)       */
    /* Guest pacing (s83, #221): retrace periods between successive completed start-
       address pairs. A smooth 70 Hz scroller is all 1s; a 2 is a frame the guest missed. */
    UINT32 StartPreviousFrame;  /* frame number of the previous completed pair       */
    UINT32 StartGapHistogram[5]; /* 0, 1, 2, 3, 4+ frames between pairs               */
    /* ── ATTRIBUTE CONTROLLER (0x3C0/0x3C1). ──────────────────────────────────
         In every 16-colour planar and text mode the 4-bit pixel value indexes
         THESE registers (vpal, above), and the result indexes the DAC. Not
         claiming 0x3C0 meant a guest's palette writes went nowhere -- right
         shapes, wrong colours, static. That is what ailed Lemmings.
       ⚠ INDEX AND DATA SHARE ONE PORT and alternate; the flip-flop is reset by
         READING 0x3DA (see status_in), which is why that read is load-bearing
         and not merely a vblank poll. */
    BYTE  AttributeFlipFlop;      /* 0 = next 3C0 write is the index, 1 = the data        */
    BYTE  AttributeIndex;   /* last index written, including bit5 (video enable)    */
    BYTE  AttributeMode;    /* AR10 Mode Control                                    */
    BYTE  AttributeColorSelect;     /* AR14 Color Select                                    */
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
    BYTE  IsChain4;       /* SR4 bit 3: 1 = chained (plain 13h), 0 = mode Y       */
    BYTE  YMask;       /* map mask in force since the last snapshot            */
    BYTE  CrtcIndex;   /* 3D4 index latch                                      */
    BYTE  CrtcOffset;  /* CRTC 0x13: logical line width in 2-byte units        */
    WORD CrtcStart;   /* CRTC 0x0C/0x0D: display start -- the page-flip reg   */
    WORD CrtcCursor;  /* CRTC 0x0E/0x0F as last written by the guest          */
    BYTE  YPlanes[4][VIDEO_Y_PLANE_SIZE];   /* de-interleaved mode-Y planes             */
    BYTE  YShadow[VIDEO_Y_PLANE_SIZE];     /* the aperture as of the last plane flush   */
    BYTE  IsCrtcSeen;                /* the guest has written a CRTC start address */
    /* ⚠ SEPARATE FROM IsCrtcSeen ON PURPOSE. CrtcOffset RESETS TO 40, which is the
         right stride for mode 12h (40 * 2 = 80 bytes a line) and wrong for 0Dh (which
         wants 20 * 2 = 40). So the default cannot be trusted as a value -- only an
         explicit write to CRTC index 0x13 means "this is the stride". */
    BYTE  IsCrtcOffsetSeen;            /* the guest has written CRTC Offset (0x13)   */
    /* ── SPLIT SCREEN (Line Compare). ─────────────────────────────────────────
         When the scanline counter reaches Line Compare the address generator
         RESETS TO 0, so everything below that line comes from the start of video
         memory regardless of the pan. It is how a scrolling game keeps a
         stationary status panel -- which is exactly what Lemmings does.
       ⚠ The value is TEN BITS spread over three registers: 0x18 low eight,
         Overflow (0x07) bit 4 = bit 8, Maximum Scan Line (0x09) bit 6 = bit 9.
         Reading only 0x18 gives a value that is silently wrong past line 255. */
    WORD CrtcLineCompare;
    BYTE  CrtcOverflow;            /* 0x07, for line-compare bit 8            */
    BYTE  CrtcMaxScan;             /* 0x09, bit 6 = line-compare bit 9        */
    BYTE  CrtcLineCompareLow;              /* 0x18, line-compare bits 0-7             */
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
         These are the low bytes; the high bits live in CrtcOverflow (0x07) and
         CrtcMaxScan (0x09), which we already latch for Line Compare. Composed by
         vga_vtiming(), which falls back to the old constants when the guest has not
         programmed the CRTC (off-VM tests set gh directly and never touch it). */
    BYTE  CrtcVerticalTotalLow;           /* 0x06, Vertical Total bits 0-7           */
    BYTE  CrtcVerticalDisplayEndLow;              /* 0x12, Vertical Display End bits 0-7     */
    BYTE  CrtcVerticalBlankStartLow;              /* 0x15, Vertical Blank Start bits 0-7     */
    BYTE  IsCrtcVerticalTimingSeen;             /* guest has written 0x06 AND 0x12 AND 0x15 */
    /* ▶ The geometry those five registers DESCRIBE, worked out once per CRTC write by
         crtc_vt_recompute(). 0x3DA is the most-read port there is -- Lemmings polls it
         1.6 MILLION times a second -- so reassembling three scattered 10-bit fields and
         revalidating them on every read was 4.4% of that guest's poll budget for an
         answer that only changes when the guest programs the CRTC. IsVerticalTimingValid = 0 means
         "not a plausible screen", and the caller keeps the old two-case constants. */
    WORD VerticalTotal, VerticalActive, VerticalBlank;
    BYTE  IsVerticalTimingValid;
    /* #325: the register file describes the mode on screen -- a measured register set
       was loaded at the mode set, or the guest programmed a geometry register (CR01 /
       CR07 / CR09 / CR12 / CR17) since. Only then is the displayed size read from it. */
    BYTE  IsGeometryRegistersOk;
    UINT32 ModeYGap;                /* mode-Y run coalescing slack, in dwords     */
    /* ── OPTIONAL: PER-PLANE BACKING SUPPLIED BY THE HOST. ───────────────────────
         When these are set, the guest's A0000 window IS whichever plane the map mask
         selects -- the host swaps the mapping on a mask change -- so a guest write
         lands in the right plane and there is nothing to de-interleave afterwards.
         `YMapSelect` is called with the NEW mask, or -1 for chained/linear; `YMapPlane`
         returns a host-side view of a plane, valid whatever is mapped at A0000.
         Left null, the VDD falls back to modey_flush()'s heuristic. */
    PVOID YMapContext;
    VOID   (*YMapSelect)(PVOID ctx, INT mask);
    VOID   (*YMapWriteMode)(PVOID ctx, INT wmode);   /* GC write mode changed */
    VOID   (*YMapReadMap)(PVOID ctx, INT Planes); /* GR4 read-plane changed -- see gc_set_data */
    BYTE *(*YMapPlane)(PVOID ctx, INT p);
    BYTE  WriteMode;   /* GR5 bits0-1                                          */
    BYTE  BitMask;     /* GR8 (reset 0xFF)                                     */
    BYTE  Latch[4];     /* per-plane read latches                               */
    BYTE  Retrace;      /* Input Status 1 (3DA): legacy toggle, used only if no clock */
    /* CRT TIMEBASE (GH #55 follow-up). Host sets this to a microsecond clock so
       0x3DA reports vertical retrace ON A REAL PERIOD instead of alternating per
       read. Without it, `WAIT &H3DA,8` returns instantly and every program that
       paces on vblank runs unbounded -- measured on BOUNCEBX/MATRIX_2/CAVE.
       NULL (the off-VM default) keeps the old toggle, so tests that do not care
       about timing are unaffected; the video battery injects a fake clock. */
    UINT64 (*TimeUs)(VOID);
    /* WHAT THE GUEST ACTUALLY SEES on 0x3DA. `VblEdges` counts clear->set
       transitions as observed BY THE GUEST's own reads, which is its real frame
       rate: one `WAIT &H3DA,8` completes per edge. Without this, "is it paced?"
       is a matter of opinion about how fast the screen looks; with it, it is
       edges/second against an expected 60 or 70. `Port3DaReads` is the poll count,
       so the two together also say how hard the guest is spinning. */
    UINT32 VblEdges;
    UINT32 Port3DaReads;
    BYTE  VblPrevious;
    /* ── ★ THE PRESENT, RAISED BY THE GUEST'S OWN FRAME (s73, "Auto" UI tick). ──────
         The host used to discover the present window by SAMPLING it from a timer --
         a 15 ms tick looking for a ~2 ms phase window in a 16.7 ms frame, so it hit
         about two frames in three and at an arbitrary offset (BOUNCEBX: 1798 guest
         frames, ~1200 presents, visibly choppy where stock is smooth -- stock has ONE
         clock for the retrace and the repaint). The guest's 0x3DA poll already
         computes where the beam is; when it enters the present window -- or, failing
         that, the retrace itself -- this hook fires ONCE per frame and the host
         presents as a consequence of the guest's frame, not on its own stopwatch.
         NULL = nobody listening (off-VM, or a fixed tick). */
    VOID   (*PresentHook)(PVOID ctx);
    PVOID PresentContext;
    UINT32 PresentFrame;      /* frame_no the hook last fired for              */
    UINT32 PresentHookFires; /* how often it fired (the report compares to edges) */
    UINT32 PresentHookGap;   /* ...of which on "first poll after drawing"        */
    UINT32 PresentGapUs;     /* this poll's distance from the previous one       */
    /* ▶ IS THE TIMEBASE UNDER THE RETRACE ACTUALLY WALL-CLOCK TIME? `VblEdges` says
         the guest completes N frames a second, but that is only a statement about the
         CARD if the clock beneath it runs at real speed. Both of the obvious readings
         of a low edge count -- "the guest is missing vblanks" and "the model is slow"
         -- predict the same VblEdges, so that counter cannot separate them and no
         amount of staring at it will.
         These can. Time3DaFirst/Time3DaLast bracket the polling in MODEL microseconds,
         read from the SAME clock status_in derives the bits from, so the two cannot
         disagree about their units; the caller compares that span against the run's
         real length. A model span 20x shorter than the run says the retrace is slow
         because the CLOCK is slow, and the video model is innocent.
         Dt3DaMax is the largest gap between two consecutive polls: a guest can only
         legitimately miss a vblank if it was away longer than one blanking interval,
         so if this stays well under a frame the guest missed nothing. */
    UINT64 Time3DaFirst, Time3DaLast;
    /* The guest's previous poll of bit 0: which scanline (absolute, frames included)
       and what it read. A poll in a LATER line whose predecessor saw the display
       active is owed the blanking that passed between them -- see status_in. */
    UINT64 Port3DaLastLine;
    BYTE  Port3DaLastBit0, IsPort3DaHaveLast;
    UINT32 Port3DaHblOwed;   /* blanks reported by that rule (STAGE2)               */
    BYTE  IsPort3DaLastVbl;   /* the previous poll was in vertical blanking          */
    /* #225: the same rule for bit 3, ONLY while the host's CPU throttle is on. A
       throttled guest is held for milliseconds at a time and a vertical retrace is
       ~1.5 ms, so it slept through most of them (Skyroads at 486DX2-66: 13 edges/s
       where it draws 35). A real slow CPU polls continuously and never misses one.
       Set by the host (IsVblOweOn); off = byte-identical to before. */
    BYTE  IsVblOweOn;
    UINT32 Port3DaLastFrame; /* frame_no of the previous poll                        */
    UINT32 Port3DaVblOwed;   /* retraces reported by that rule (STAGE2)             */
    /* ── ⚠ DEBUG SCAFFOLDING, OFF UNLESS ASKED FOR. ─────────────────────────────────
         The last VIDEO_PORT_3DA_RING polls: model microseconds and the byte returned. A
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
#define VIDEO_PORT_3DA_RING 1024
    BYTE  IsPort3DaRingOn;
    UINT32 Port3DaRingUs[VIDEO_PORT_3DA_RING];
    BYTE  Port3DaRingValue[VIDEO_PORT_3DA_RING];
    UINT32 Port3DaRingCount;
    UINT32 Dt3DaMax;
    UINT32 Dt3DaZero;   /* polls across which the clock did not advance at all */
    /* A MAXIMUM IS ONE EVENT AND CANNOT CARRY A RATE. Dt3DaMax says the guest was
       once away for seconds; what decides the frame rate is how OFTEN it is away
       longer than a blanking interval (~1.9ms), because each of those can step over
       a whole vblank unseen. Buckets are powers of four in microseconds, so the
       window and the frame period land in known ones: [5] is 1024-4095us (straddles
       the ~1.9ms window) and [6]/[7] are longer than a 14.3ms frame outright. */
    UINT32 Dt3DaHistogram[8];
    UINT32 Int10Ah11Calls;/* INT 10h AH=11h (character generator) calls -- see below */
    /* WHAT the guest asked for, not just that it asked. BH selects the table and the
       answer's CX (bytes per character) is what the caller strides by -- so a wrong CX
       misaligns every glyph after the first, which looks exactly like garbled text.
       Recorded per call so the log says which table Skyroads wants. */
    VIDEO_FONT_QUERY FontQueries[4];
    /* ── GH #52: A USER-LOADED TEXT FONT, AND THE RENDERER READS IT. ──────────
         INT 10h AH=11h AL=00h/10h loads a caller-supplied character generator.
         We used to accept those calls, announce them as unimplemented and draw
         from the ROM table anyway -- so a program that loaded its own glyphs got
         the stock ones and no error. `UserFontRows` is bytes per character as
         the caller declared it; rows beyond it are blanked, because a cell is
         VIDEO_CELL_HEIGHT tall whatever the font supplies.
         `IsUserFontOn` stays 0 until a load actually lands, so the ordinary case
         costs one branch and draws from the ROM exactly as before. */
    BYTE  UserFont[256 * 16];
    BYTE  UserFontRows;
    BYTE  IsUserFontOn;
    BYTE  FontQueryCount;
    /* Every mode set, with what it RESOLVED TO. Added after a mode-table change
       silently altered mode 12h rendering: the STAGE2 line said "modes
       unsupported: none" and the picture was still wrong, so "which modes were
       refused" was not the question -- "what did each accepted mode become" was. */
    VIDEO_MODE_QUERY ModeQueries[8];
    BYTE  ModeQueryCount;
    BYTE  FrameBuffer[VIDEO_FRAME_MAX];            /* text glyph / planar render target        */
    INT      IsDirty;
    /* GH #27: every unimplemented path must announce itself. An INT 10h function
       we do not handle, or a mode number we do not support, used to be a silent
       no-op -- the program carried on drawing into a screen that was never set
       up, and the only symptom was "the display looks wrong". These bitmaps are
       drained into the STAGE2 block so a run yields a to-do list instead. */
    BYTE  UnimplementedFunctions[32];             /* INT 10h AH values seen but unhandled     */
    BYTE  UnimplementedModes[32];           /* mode numbers requested but unsupported   */
    /* ── WHICH VESA MODES THE GUEST ASKED ABOUT, AND WHETHER WE HAD THEM. (s74)
         `UnimplementedModes` is a 256-bit map indexed by mode number, so it cannot hold a
         VBE mode at all (they start at 0x100). heaven7 asks 4F01 twice, gets 0x014F
         twice and prints "VESA error" -- and nothing in the log said WHICH modes,
         which is the only fact needed to decide what to implement. Do not guess a
         guest's expectation; record it. */
    /* ⚠⚠ 12 SLOTS WAS EXACTLY THE NUMBER OF MODES WE PUBLISH, so heaven7's twelve
         4F01 queries filled the ring and the 4F02 that FOLLOWED THEM -- the one call
         the whole instrument existed to catch -- was silently dropped. The log then
         read "the guest never set a mode" while it was demonstrably rendering. A
         ring sized to the thing it observes will always lose the last event, so the
         SET is now recorded on its own and cannot be crowded out by queries. */
    WORD VesaQueries[32];                /* modes passed to 4F01 / 4F02              */
    BYTE  VesaQueryOk[32];             /* 1 = we answered 0x004F, 0 = 0x014F       */
    BYTE  VesaQueryFunction[32];             /* 0x01 or 0x02 -- which call asked         */
    WORD VesaSetBx;               /* the raw BX of the last 4F02 (mode|flags)  */
    BYTE  IsVesaSetOk;               /* and whether we accepted it                */
    BYTE  IsVesaSetSeen;
    BYTE  VesaQueryCount;                   /* how many recorded (capped)               */
    /* Every 4Fxx call by sub-function, and which BL values it came with -- the
       corpus inventory: what guests ACTUALLY ask of VESA. Bits 0..14 = BL 0..14,
       bit 15 = BL 80h (the "during retrace" variants). Costs two adds per call. */
    UINT32 VesaCalls[0x16];
    WORD VesaBl[0x16];
    WORD VesaTextMode;            /* 0x108..0x10C while a VESA TEXT mode is set (4F03 reports it); 0 otherwise */
    BYTE  VesaPmState;             /* 4F10 VBE/PM: 0 on 1 standby 2 suspend 4 off 8 reduced-on */
    WORD Vesa07MaxX, Vesa07MaxY;/* largest display start a guest asked for  */
    UINT32 Vesa07Rejected;               /* 4F07 sets refused (would not fit)         */
    /* ── #53: THE PORTS THE 4F0Ah PROTECTED-MODE CODE DRIVES (01CEh index, 01CFh data;
         see vbe_pm.asm). VbeIndex = the selected index, VbeStartLow the low word of a
         display start awaiting its high word. The counters are the STAGE2 evidence that
         a client is calling the PM block instead of INT 10h: banks set, starts set, and
         writes refused (no VESA mode, out of range). */
    WORD VbeIndex, VbeStartLow;
    UINT32 VbePmBankCount, VbePmStartCount, VbePmRejected;
    NTVDD_FRAME Frame;
    /* Mode-Y de-interleave instrumentation. `plane-nonzero` in STAGE2 has always
       counted st->Planes[] -- the 16-colour PLANAR array -- which mode Y never touches,
       so it reported four zeroes for every unchained run ever made. These are the
       arrays that actually carry a mode-Y frame. */
    UINT32 MaskHistogram[16];             /* map-mask values written, by value          */
    UINT32 WriteModeHistogram[4];             /* GC write modes selected (1 = LATCH COPY)   */
    /* ── WHAT THE REGISTERS HELD AT *WRITE* TIME, not at end of run. ──────────
         WriteModeHistogram/MaskHistogram above count register PROGRAMMING; a snapshot of
         EnableSetReset taken when the guest exits says nothing about what it held
         during the thousands of writes before that. These are incremented inside
         VddVideoPlanarWrite, so they are a history rather than a final state. */
    UINT32 WriteEnableSetResetHistogram[16];           /* Enable Set/Reset live at each write     */
    UINT32 WriteAluHistogram[4];             /* GR3 ALU function live at each write      */
    UINT32 WritePlane3SetReset;                   /* writes where plane 3 took set/reset      */
    UINT32 WritePlane3NonZero;                   /* ...of which stored a NON-ZERO byte       */
    UINT32 WritePlane3Data;                 /* writes where plane 3 took the CPU byte   */
    UINT32 AcPortWrites;            /* palette registers written via 0x3C0       */
    UINT32 AcBiosWrites;            /* ...and via INT 10h AH=10h                 */
    UINT32 DacWrites;                /* DAC entries written (3C9 + INT 10h)       */
    /* s70 instrument: WHERE ON THE SCREEN the guest writes its DAC. Bands: rows 0-1,
       2-159, 160+ (toolbar), vertical blanking. DacLastRow = the last write's row
       (0xFFFF = blanking, 0xFFFE = no clock). */
    UINT32 DacRowHistogram[4];
    WORD DacLastRow;
    /* Which DAC entries the guest actually programs, in 16-entry blocks. The whole
       Lemmings palette question is "does it write 0x38..0x3F", and a total count
       cannot answer that. */
    UINT32 DacBlock[16];
    BYTE  IsDefaultPaletteOff;               /* INT 10h AH=12h BL=31h: suppress the reload */
    UINT32 PaletteResets;                /* load_default_palette() calls              */
    UINT32 DacHighSinceReset;        /* block-3 DAC writes since the last reset   */
    /* ⚠ ...and the running maximum over all reset epochs. The counter above is read
       after the guest has exited, and a guest exits through a mode set, so on its
       own it reads zero for every guest that ever ran. */
    UINT32 DacHighMax;
    /* ── A VRAM WATCHPOINT: EVERY WRITE TO ONE BYTE, WITH THE REGISTERS THAT MADE IT.
         A histogram says which idioms a run used; it cannot say which idiom produced
         the wrong pixel, because every idiom is in the histogram. This records the
         whole write -- mode, map mask, Enable Set/Reset, Set/Reset, ALU, bit mask,
         the CPU byte, the latches, and the four plane bytes afterwards -- for one
         chosen offset, plus the guest CS:IP so the routine responsible can be found
         in a disassembly. Armed from cfg/vwatch.txt; off by default.
       ⚠ BOUNDED ON PURPose: a guest-driven trace outruns the guest. The first
         VIDEO_WATCH_MAX writes are kept and the LAST one separately, because the last
         write is the one that decides what is on screen. */
    /* The highest VRAM byte offset the guest has touched, read or written. Answers
       "does this guest use off-screen VRAM?" for any guest, which is what the old
       38400-byte plane made unanswerable -- everything above it read back as 0xFF
       and looked like the guest's own data. */
    UINT32 PlanarHighWater;
    VIDEO_SITE WriteSites[VIDEO_SITES];        /* write sites, by guest CS:IP               */
    UINT32 WriteSitesLost;                /* writes whose pc collided with another      */
    /* And the same for READS. A picture that is missing something is as often a copy
       whose SOURCE was never read as a write that never happened -- with off-screen
       VRAM in play, "who reads up there" is the question that separates the two. */
    VIDEO_SITE ReadSites[VIDEO_SITES];
    UINT32 ReadSitesLost;
    /* Colour-compare reads get their OWN table. They are a tiny minority of reads and
       would be buried in the one above, and they are the interesting ones: a read mode
       1 site is a guest asking "where is the ground", so its pc is a routine worth
       disassembling and its address range says WHICH copy of the level it trusts. */
    VIDEO_SITE CompareSites[VIDEO_SITES];
    UINT32 CompareSitesLost;
    /* ▶ WHAT THE COMPARE ACTUALLY ANSWERED, per site. A colour-compare read is the only
         VRAM read whose RESULT is a decision rather than a pixel, and Lemmings has one
         such site, which decides from two bytes whether at least 8 of 16 pixels are
         terrain. A count of reads says that site
         ran; it cannot say the game could SEE anything. If `zero` is essentially equal
         to `n` at that site, every pixel it asked about came back "not terrain" -- which
         is a lemming walking into thin air, and is indistinguishable, in every counter
         we had before this, from a site that is working perfectly. */
    UINT32 CompareSitesZero[VIDEO_SITES];   /* compares that returned 0x00 -- nothing matched */
    UINT32 CompareSitesOnes[VIDEO_SITES];   /* compares that returned 0xFF -- everything did  */
    /* ── ▶ WHO TOUCHES THE OFF-SCREEN SPRITE CACHE -- A LINEAR TABLE, NOT A HASH. ──
         The three tables above are 256 single-slot hashes, so a site whose pc collides
         with a busier one is dropped into a `_lost` counter and NEVER APPEARS. On the
         Lemmings run that cost 249,630 reads, which makes those tables unable to answer
         the one question the toolbar bug turns on: does the routine that copies the
         composed panel onto the screen RUN AT ALL? An absence there is indistinguishable
         from a collision, and reasoning from it is exactly the trap the site histograms
         have already sprung twice.
         This table cannot collide: it is a short LINEAR scan, and it only admits
         accesses above VIDEO_CACHE_LOW -- a region no other site in the measured run comes
         near (the busiest top out at 0xDABF), so ten slots is generous rather than tight.
         `lost` counts accesses that found the table full, and a nonzero value invalidates
         only the CLAIM OF COMPLETENESS, never the entries themselves.
       ▶ AND THE ORDER THEY CAME IN. A panel that is composed and then blitted looks
         identical, by count, to one that is blitted and then composed -- but the second
         puts an empty cache on the screen, which is the reported symptom. Counts cannot
         tell those apart and no number of them will. `first`/`last` are ticks of a
         counter that advances only on cache accesses, so comparing the compositor's
         first write against the blitter's first read settles the ordering outright. */
    VIDEO_CACHE_SITE CacheSites[VIDEO_CACHE_SITES];
    UINT32  CacheSitesLost;
    UINT32  CacheSequence;                /* ticks once per cache-region access          */
    UINT32 WatchOffset;                 /* VRAM byte offset watched; ~0u = disarmed  */
    UINT32 WatchCount;                   /* writes seen (may exceed what is recorded) */
    VIDEO_WATCH_RECORD Watch[VIDEO_WATCH_MAX];
    VIDEO_WATCH_RECORD WatchLast;
    UINT32 (*GuestPc)(VOID);         /* host hook: (CS<<16)|IP, 0 if unavailable  */
    UINT32 ModeMaskHistogram[64];               /* (write mode, map mask) pairs -- see seq_out */
    UINT32 MaskSkipChain4;          /* map-mask writes dropped: chained            */
    UINT32 MaskSkipSame;            /* map-mask writes dropped: value unchanged    */
    UINT32 Gr4Histogram[4];               /* GR4 read-plane values written, by value      */
    UINT32 Chain4Transfers;              /* #184: chain-4 <-> unchained moves of the 64K */
    UINT32 Chain4Selects;                /* YMapSelect calls made by a CHAIN4 change,
                                           not by a map-mask write -- the map-mask
                                           identity has to subtract these or it will
                                           show a residual that is not a lost write   */
    UINT32 YSnapshots[4];                  /* snapshots taken into each mode-Y plane      */
    UINT32 YNonZero[4];                    /* busiest snapshot each plane ever received   */
    /* ── ★★★★★ THE REGISTER FILE. (docs/inventory/vga.md, step 1) ───────────────────
         Every Sequencer / CRTC / Graphics Controller / Attribute Controller index as
         the guest wrote it, plus the external registers, plus a WRITE COUNT per index.
       ► WHY, in the user's words: *"Doom, Wolf3D, Mario and Skyroads all use mode 13h,
         but they all use it differently. Those differences were not identified and
         implemented against until the apps asked for them. Yet on real period-correct
         hardware they all work."* They work because the hardware IS these registers and
         the picture is what they make. This host had no register file at all: it had a
         mode number and a special case per guest that failed. The inventory measured
         41 of 71 registers modelled, and every absent one describes geometry, address
         generation or panning -- which is exactly what differs between those four.
       ⚠ THIS IS CAPTURE ONLY AND RENDERS NOTHING. The derived fields above
         (MapMask, IsChain4, CrtcStart, WriteMode ...) remain the sole authority for
         every picture, so step 1 cannot change one. What it changes is that
         "which registers do our guests actually program, and with what?" has an
         answer -- for the first time -- and that the oracle has something to diff.
         Steps 3-5 of the plan move the authority here, one guest at a time.
       ⚠ A WRITE COUNT IS NOT A VALUE. `*_w[i] == 0` means the guest never touched
         index i, so the value beside it is OUR reset default and a statement about us,
         not about the guest. Read the two together or the table lies. */
    BYTE  SequencerRegisters[8];                /* SR0-SR4 (8 decoded)                        */
    BYTE  CrtcRegisters[32];              /* CR00-CR18 (32 decoded)                     */
    BYTE  GcRegisters[16];                /* GR0-GR8 (16 decoded)                       */
    BYTE  AttributeRegisters[32];              /* AR00-AR14 (32 decoded)                     */
    UINT32 SequencerWrites[8], CrtcWrites[32], GcWrites[16], AttributeWrites[32];
    /* External/general registers. Ports 3C2/3C3/3C6/3CA/3CC were claimed by NOBODY
       before this, so a write vanished and a read returned the bus's 0xFF. */
    BYTE  MiscOutput;                  /* 3C2 write / 3CC read -- clock + sync polarity */
    BYTE  FeatureControl;                 /* 3?A write / 3CA read                        */
    BYTE  VgaEnable;                /* 3C3                                         */
    BYTE  DacMask;                  /* 3C6 -- ANDed with every pixel; fades use it  */
    UINT32 MiscWrites, FeatureWrites, VgaEnableWrites, DacMaskWrites;
    /* CR00-CR07 writes refused because CR11 bit 7 (write protect) was set --
       real hardware refuses them and we used to accept them. Counted rather than
       silent: an absence in a report means nothing. */
    UINT32 CrtcWriteProtectRefused;
    /* #187: Input Status 0 bit 7, the vertical-retrace interrupt latch. Armed while
       CR11 bit 5 = 0 (enable, active low) and bit 4 = 1 (not clearing); set by the
       first retrace start after VintArmTime; cleared by writing CR11 bit 4 = 0. */
    BYTE  IsVintArmed, IsVintPending;
    UINT64 VintArmTime;
    /* ── #266: THE GRAPHICS FONT VECTORS AND THE CGA COLOUR SELECT. ──────────────────
         i43_* is what INT 43h holds (the graphics-mode character table) and i1f_* what
         INT 1Fh holds (the 8x8 table's upper half, characters 80h-FFh): the mode set and
         INT 10h AH=11h AL=20h-24h set them, AH=11h AL=30h BH=0/1 answers with them, and
         the IVT is written to agree whenever they change (vid_set_vec). `IsGraphicsFontUser` /
         `IsInt1FUser` are set when the CALLER supplied the table (AL=21h / 20h): the glyph
         services then draw from guest memory at that pointer instead of our ROM copy --
         exactly the table the vector names, which for the ROM case is the same bytes.
         `CgaSelect` shadows 0040:0066 (the CGA colour-select byte AH=0Bh maintains) for a
         run with no BDA wired (the off-VM battery); with a BDA the byte itself is read. */
    WORD Int43Segment, Int43Offset, Int1FSegment, Int1FOffset;
    BYTE  IsGraphicsFontUser, IsInt1FUser;
    BYTE  CgaSelect;
    /* SR1 bit 5 "screen off" was in force for the frame last composed (#266): the
       frame went out black, and frame_touch must not re-arm a raster split over it. */
    BYTE  IsBlanked;
} VIDEO_STATE, *PVIDEO_STATE;
typedef const VIDEO_STATE *PCVIDEO_STATE;

/* ── #266: THE PURE HALVES OF INT 10h AH=0Bh AND AH=11h AL=21h-24h, for video_test.c. ──
     VddCgaColourSelect   the new 0040:0066 byte: BH=0 replaces bits 0-4 with BL's
                             (background/border colour + bit 4 = intensity), BH=1 bit 5
                             with BL bit 0 (palette 0 green/red/brown, 1 cyan/magenta/white).
     VddCgaBackgroundAr           an IRGB colour from BL as an attribute-controller value in the
                             CGA-compatible DAC layout the BIOS loads for modes 04h-06h:
                             BL bit 3 (intensity) becomes bit 4, i.e. 08h-0Fh -> 10h-17h.
     VddCgaPaletteAr          AR01-AR03 for modes 04h/05h from the 0066 byte: 2/4/6 or 3/5/7,
                             plus 10h when bit 4 (intensity) is set. With the mode set's
                             0066 = 30h that is 13h/15h/17h -- the MEASURED mode 04h table.
     VddGraphicsFontRows       AH=11h AL=21h-24h BL: 0 = DL rows, 1 = 14, 2 = 25, 3 = 43, and
                             anything else 25 (SeaVGABIOS's default arm; IBM unmeasured). */
BYTE VddCgaColourSelect(_In_ BYTE current66, _In_ BYTE bh, _In_ BYTE bl);
BYTE VddCgaBackgroundAr(_In_ BYTE bl);
VOID    VddCgaPaletteAr(_In_ BYTE select66, _Out_writes_(3) BYTE attributes[3]);
BYTE VddGraphicsFontRows(_In_ BYTE bl, _In_ BYTE dl);
/* ── #266: ONE 64-BYTE VIDEO PARAMETER TABLE ENTRY, IN THE IBM VGA BIOS's LAYOUT. ──
     00 columns, 01 rows-1, 02 character height, 03 page (regen) size word, 05 SR1-SR4,
     09 Misc Output, 0A CR00-CR18, 23 AR00-AR13, 37 GR0-GR8. `tableIndex` is the TABLE index
     (0-1Ch: 04h-07h, 0Dh/0Eh, 11h = 0Fh, 12h = 10h, 17h = 0+/1+, 18h = 2+/3+, 19h = 7+,
     1Ah-1Ch = 11h-13h), not a mode number. Returns 1 when filled from the measured mode
     table, 0 when the entry is left zero (a mode nobody measured, or a reserved slot). */
INT     VddVideoParameterEntry(_In_ BYTE tableIndex, _Out_writes_(64) BYTE entry[64]);

#define VIDEO_UNIMPLEMENTED_SET(bm, n)  ((bm)[((n) & 0xFF) >> 3] |= (BYTE)(1u << ((n) & 7)))
#define VIDEO_UNIMPLEMENTED_GET(bm, n)  (((bm)[((n) & 0xFF) >> 3] >> ((n) & 7)) & 1u)

INT  VddVideoInitialize(_In_ PVDD_BUS bus, _In_ PVOID context);
VOID VddVideoReset(_In_ PVOID context);
/* Stamp the frame with the current frame number and the raster-split arrays. The
   host calls this under its lock right before it snapshots the frame; tests call it
   before resolving colours with VddFramePaletteAt. */
VOID VddVideoFrameTouch(_Inout_ PVIDEO_STATE state);
static inline NTVDD_DEVICE VddVideoDevice(_In_ PVIDEO_STATE state)
{ NTVDD_DEVICE device; device.Name = "video"; device.Initialize = VddVideoInitialize; device.Reset = VddVideoReset;
  device.Shutdown = 0; device.Context = state; return device; }

VOID VddVideoRender(_Inout_ PVIDEO_STATE state);                /* text glyph render        */
/* The text screen as the GUEST wrote it (characters, then attributes in hex) --
   the instrument that separates "never listed" from "never drawn". */
INT  VddVideoTextSnapshot(_In_ PVIDEO_STATE state, _Out_writes_(capacity) PSTR output, _In_ INT capacity);
/* ── THE INT 33h TEXT CURSOR IS A CELL, NOT A SPRITE. ───────────────────────────────
     In a text mode the mouse driver has no pixels to draw an arrow with; it shows the
     pointer by REWRITING THE ATTRIBUTE of the character cell under it -- AND-ed with
     the screen mask, XOR-ed with the cursor mask (INT 33h 0Ah; the defaults 77FFh /
     7700h invert foreground and background). The host used to stamp its 16x16 arrow
     sprite into the text frame, which is what "a graphical mouse cursor over a text
     interface" looks like. Call after VddVideoRender, with the cell the pointer is
     in; the masks are the driver's (low byte = character, high byte = attribute). */
VOID VddVideoTextCursor(_Inout_ PVIDEO_STATE state, _In_ INT column, _In_ INT row,
                        _In_ WORD andMask, _In_ WORD xorMask);
/* CGA 4-colour (modes 04h/05h): the frame value render_cga gives each 2-bit pixel value
   0..3 under the current palette select. The INT 33h graphics cursor maps back through it
   (GH #264) -- one table, so the cursor and the renderer cannot disagree. */
const BYTE *VddVideoCga4Map(_In_ PCVIDEO_STATE state);
/* Write the display's BDA fields from the current state (see `bda` above). */
VOID VddVideoBdaSync(_Inout_ PVIDEO_STATE state);
/* #183: microseconds until 3DAh bit 3 reads `want_set` (0 = now, UINT32_MAX = unknown). */
UINT32 VddVideoUsToRetrace(_Inout_ PVIDEO_STATE state, _In_ INT wantSet);
/* ── ★ CURSOR EMULATION: AN 8-LINE SHAPE ON A 16-LINE CELL. ─────────────────────
     DOS asks for its cursor in SCAN LINES, and it asks in the units of the machine
     it was written for -- an 8-line character cell, where an underline is lines 6-7
     and the insert-mode block is 0-7. Our cell is 16 lines (an 8x16 VGA font), so
     honouring those numbers literally puts the underline HALFWAY UP THE CELL, which
     is what "ABC123-" instead of "ABC123_" looks like. It was our own default doing
     it too: CursorShape starts at 0x0607.
   ► Real VGA BIOSes solve this with CURSOR EMULATION, and this is their rule (the
     IBM/Bochs/SeaBIOS one, followed exactly rather than approximated): when the cell
     is taller than 8 and the request is in 8-line units, scale it up. Which is also
     what gives MS-DOS's insert/overwrite cursors for free -- 6-7 becomes 14-15, a
     bottom underline, and 0-7 becomes 1-15, a full block -- because those are the
     two shapes DOS sets when you press Insert.
   Pure arithmetic on the shape word, so tests/unit/video_test.c can pin the exact
   shapes DOS uses. `isHidden` is set for the two idioms that mean "no cursor". */
VOID VddCursorLines(_In_ WORD shape, _In_ UINT cellHeight,
                    _Out_ UINT *start, _Out_ UINT *end, _Out_ INT *isHidden);
VOID VddVideoPutChar(_Inout_ PVIDEO_STATE state, _In_ BYTE character);      /* console teletype sink    */

/* Planar A0000 access (mode 12h): the host calls these from the memory-write trap
   so direct framebuffer writes run through the VGA write-modes into the 4 planes.
   `offset` is the byte offset within the A0000 window. */
VOID    VddVideoPlanarWrite(_Inout_ PVIDEO_STATE state, _In_ UINT32 offset, _In_ BYTE cpu);
BYTE VddVideoPlanarRead (_Inout_ PVIDEO_STATE state, _In_ UINT32 offset);   /* loads latches        */
INT     VddVideoIsPlanarActive(_In_ PCVIDEO_STATE state);    /* 1 in mode 12h        */
/* 1 when the emulated CRT is in the tail of its active period -- the guest has
   finished drawing this frame and is parked waiting for retrace, so a snapshot
   taken now is a WHOLE frame. Presenting at an arbitrary phase is what makes a
   program that erases-then-redraws (BOUNCEBX) tear: catch it between the two and
   the object is simply missing. Returns 1 unconditionally with no clock injected. */
INT     VddVideoIsPresentReady(_Inout_ PVIDEO_STATE state);
UINT32 VddVideoFrameUs(_In_ PCVIDEO_STATE state);   /* 16667 or 14286 (s73) */
/* ── #226: AN INT 10h CALL THAT MUST WAIT FOR THE BEAM. ──────────────────────────────
     VBE 4F07h BL=80h (and 3.0's 82h) is "set display start DURING VERTICAL RETRACE":
     the call is not complete until the retrace has begun, and a guest that calls it
     once a frame is paced by it -- that is the whole VESA vsync idiom (heaven7 makes
     ~16,000 such calls a run). The VDD cannot spin inside int10(): the host delivers
     INT 10h under its lock, and holding that for up to a frame would stall the UI,
     the presenter and IRQ delivery. So the handler applies the start to the latch
     schedule, stamps st->Int10WaitUntil, and returns.
   ► THE HOST'S HALF: after VddBusDeliverInterrupt(..., 0x10, ...) and HOST_UNLOCK(),
       while (VddVideoInt10WaitUs(&g_vid)) { spin / yield, keep g_dpmi_iter alive }
     before advancing the guest past the INT. Returns the microseconds still to wait
     on st->TimeUs's clock (0 = done, and the stamp is cleared), so a stale stamp can
     never park a guest. Without that loop the call behaves as it always did -- it
     returns at once -- and only the pacing is lost; the start itself is still shown
     from the retrace the call names (vid_latch). 0 always with no clock (off-VM). */
UINT32 VddVideoInt10WaitUs(_Inout_ PVIDEO_STATE state);

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
/* #53: the VBE 2.0 protected-mode interface block (vbe_pm.h, < 256 bytes) right after the
   fonts, for the same reason they are here: real-mode-addressable RAM a VGA game leaves
   alone. 4F0Ah returns B260:0000; VddVideoInstallFonts writes it, and 4F0Ah writes it
   again on every call so a guest that scribbled on it gets a good copy. */
#define VDD_VBEPM_SEG    0xB260       /* B2600..B26FF */
/* #273: the real-mode WinFuncPtr stub (vbe_rm.asm, 34 bytes) in the same 256 bytes, after
   the 186-byte block: B260:00C0. vbe_pm_install writes both. */
#define VDD_VBERM_OFF    0x00C0
/* #266: the VGA BIOS's pointer tables, after the VBE block and for the same reason --
   B270:0000 the Video Save Pointer table (0040:00A8 points here; 7 far pointers),
   B270:0020 the secondary save pointer table (VGA, 1Ah bytes), B270:0040 the display
   combination code table (36 bytes), B270:0080 the 29 x 64-byte video parameter table.
   Ends at B2EC0. VddVideoInstallFonts writes them and the 0040:00A8 pointer. */
#define VDD_VIDTAB_SEG   0xB270
#define VDD_SAVEPTR_OFF  0x0000
#define VDD_SAVEPTR2_OFF 0x0020
#define VDD_DCC_OFF      0x0040
#define VDD_VPARAM_OFF   0x0080
#define VDD_VPARAM_N     29
/* #265: THE INT 33h DRIVER'S OWN DATA, the 512 bytes after #266's tables (B2F00..B30FF;
   s92: both branches first claimed B270). 2Ch and 2Dh
   answer with ES:SI INTO the driver (the acceleration-profile block, a profile's name)
   and 34h with ES:DX at the MOUSE.INI name, so those bytes need a real-mode address a
   guest can read -- the same reason the fonts and the VBE block are here. Not owned by
   the video model: mouse_int33 (main.c) rewrites it on every call that hands it out. */
#define VDD_MOUSE_SEG    0xB2F0       /* B2F00..B30FF */
#define VDD_MOUSE_ACC    0x0000       /* the 144h-byte profile block (i33_driver.h)    */
#define VDD_MOUSE_INI    0x0150       /* "MOUSE.INI", ASCIIZ                            */

/* `Int10Ah11Calls` is counted so the next round is not another guess: the font-pointer fix
   assumed the guest asks for its glyphs with INT 10h AH=11h, and the text is still garbled.
   If it stays at zero, Skyroads never asks -- it is reading a font from a hard-coded ROM
   address (F000:FA6E is the classic one) or carrying its own, and the fix was aimed at the
   wrong thing. The end-of-run STAGE2 summary reports it. */

/* Publish both fonts into guest memory. Call once at start-up. */
VOID VddVideoInstallFonts(_Inout_ PVIDEO_STATE state);
/* #324: the text cell's width in frame pixels -- 9 (VGA text) or 8 (SR01 bit 0, VESA text). */
INT  VddVideoTextCellWidth(_In_ PCVIDEO_STATE state);
/* #325: the displayed size of a graphics mode as the CRTC is programmed (table size if not trusted). */
VOID VddVideoGeometry(_In_ PCVIDEO_STATE state, _Out_ INT *width, _Out_ INT *height);
/* #321: re-copy the glyph tables after a font change and redraw; 0 = not mapped yet. */
INT  VddVideoRefreshFonts(_Inout_ PVIDEO_STATE state);

/* ── THE REGISTER FILE, AS TEXT. (docs/inventory/vga.md) ─────────────────────────
     Writes `STAGE2: VGAREG ...` lines into `output` and returns the bytes written.
     Every index with its value and, separately, the list of indices the GUEST
     actually wrote -- the second list is the evidence the inventory is built on and
     the first is meaningless without it (see the write-count warning in the struct). */
INT VddVideoRegistersDump(_In_ PCVIDEO_STATE state, _Out_writes_(capacity) PSTR output, _In_ INT capacity);

#endif /* NTVDMEX_VDD_VIDEO_H */

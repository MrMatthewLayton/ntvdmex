/* vdd_video.c -- see vdd_video.h.  Text mode 3 + graphics mode 13h over the
 * shared video aperture (vmem), with the DAC palette, on the VDD bus.  Pure C. */
#include "vdd_video.h"
#include "vga_modedefs.h"
#include "vga_font.h"

/* #322: the one copy of each table -- filled at start-up (src/host/sysfont.h). */
BYTE vga_font_8x8[256][8];
BYTE vga_font_8x14[256][14];
BYTE vga_font_8x16[256][16];
#include "vga_defaults.h"
#include "vbe_pm.h"

static VOID VideoLoadModeDefinition(PVIDEO_STATE st, BYTE mode);

/* ⚠ THE ega16 TABLE THAT WAS HERE IS GONE, and so is the ega64_rgb() that replaced
   it. Both hardcoded the sixteen colours a 16-colour mode renders with, which is
   exactly the assumption this file had to stop making: those colours are
   dac[vpal[i]], and both halves belong to the guest. The defaults now come from
   vga_defaults.h, MEASURED per mode; tools/gen/gen-vgadefs.py refuses to generate it
   unless mode 10h's measured DAC matches what ega64_rgb() computed entry for entry,
   so retiring the formula shifted no colour anywhere. */

/* DAC component (0..63) -> 8-bit; pack/unpack a palette entry. (This was dac_pack();
   it is VideoDacPackWidth() below now, which also knows the 4F08h width.) */
/* ⚠ This is <<2, so a guest writing 0x3F gets 252 and not 255 -- it disagrees by a
   few counts with the (v<<2)|(v>>4) the generated defaults use. Pre-existing, and
   left alone deliberately: it is on the path of every guest that programs a palette,
   so it wants its own before/after rather than riding along with this one. It is also
   what makes the width switch lossless: v<<2 >>2 is v again. */

/* ── ★★ THE DAC WIDTH IS A PROPERTY OF THE RAMDAC, SO EVERY PATH INTO IT OBEYS IT. (#226)
     VBE 4F08h switches the DAC between 6 and 8 bits per primary. We accepted the switch
     and honoured it in 4F09h ONLY: port 3C9h still stored `value & 3Fh` and read back
     `>> 2`, so a guest that did what VBE 2.0 §4.11/§4.12 tell it to -- check
     Capabilities D0, set 8 bits, then load its palette the usual way, through the
     ports -- lost the top two bits of every primary (80h became 00h) with a 004Fh in
     hand. Capabilities D0 = 1 was a promise kept for one function out of three.
   ► THE REPRESENTATION DOES NOT CHANGE. dac[] has always held 8 bits per primary; a
     6-bit write is stored as v<<2 (dac_pack) and read back >>2. That is how a
     switchable RAMDAC behaves too: in 6-bit mode the value occupies the top six bits
     of an 8-bit register, so switching the width re-interprets what is there rather
     than rescaling it (6-bit 3Fh reads back FCh at 8 bits). So the renderer, pal[],
     the raster split and the 4F04/AH=1Ch state block are width-blind by construction,
     and only the two conversions below know the width.
   ► THE RESET IS THE SPEC'S: "The DAC palette width is assumed to be reset to the
     standard VGA value of 6 bits per primary color during any mode set" (VBE 2.0
     §4.11) -- INT 10h AH=00h, 4F02h (both arms) and power-on all put it back to 6.
     A width of 0 (a state nothing initialised) reads as 6. */
static INT VideoDacIs8(PCVIDEO_STATE state) { return state->VesaDacWidth == 8; }
/* one primary as the guest wrote it -> the 8-bit component dac[] holds */
static BYTE VideoDacTo8(PCVIDEO_STATE state, BYTE value)
{ return VideoDacIs8(state) ? value : (BYTE)((value & 0x3Fu) << 2); }
/* ...and back, for a read */
static BYTE VideoDacFrom8(PCVIDEO_STATE state, BYTE component)
{ return VideoDacIs8(state) ? component : (BYTE)(component >> 2); }
static UINT32 VideoDacPackWidth(PCVIDEO_STATE state, BYTE red, BYTE green, BYTE blue)
{
    return 0xFF000000u | ((UINT32)VideoDacTo8(state, red) << 16)
                       | ((UINT32)VideoDacTo8(state, green) << 8) | (UINT32)VideoDacTo8(state, blue);
}

/* ── ★★ THE ATTRIBUTE CONTROLLER, WHICH IS WHERE A 16-COLOUR PIXEL GETS ITS COLOUR.
     A 4-bit pixel does NOT index the DAC. It indexes one of the AC's sixteen palette
     registers (st->PaletteRegisters), and THAT six-bit value indexes the DAC. We stored vpal --
     INT 10h AH=10h has always written it -- and then rendered straight from a fixed
     ega16 table, so nothing a guest did to the palette had any effect. Lemmings sets
     its own palette, which is exactly why its menu came out structurally perfect and
     the wrong colours. */

/* ── WHAT A MODE SET LEAVES BEHIND IS PER MODE. ─────────────────────────────────
     There is no single default AC palette and no single default DAC; the BIOS has a
     table for each mode and they genuinely differ:

         00h-03h, 10h, 12h   AC 00 01 02 03 04 05 14 07 38..3F   DAC = the EGA 64
         0Dh, 0Eh            AC 00 01 02 03 04 05 06 07 10..17   DAC = CGA 16, x4
         13h                 AC 00..0F (identity)                DAC = the VGA 256
         04h/05h/06h/07h/0Fh/11h  each different again

     All of it is measured by tests/probes/dos/vgadefs.asm, which sets each mode and
     reads the registers BACK -- the AC through 0x3C1, the DAC through 0x3C7/0x3C9 --
     on genuine MS-DOS 6.22. tools/gen/gen-vgadefs.py turns that dump into
     vga_defaults.h. Bit 7 of AL ("do not clear the buffer") changes none of it; the
     probe measures 8Dh and 90h to prove that rather than assume it.

   ⚠⚠ THE SINGLE TABLE THAT WAS HERE WAS INFERRED FROM A PICTURE, AND BOTH HALVES OF
     THE INFERENCE WERE WRONG. Card 11 reprogrammed DAC 0..15 to a ramp in mode 0Dh,
     left the AC alone, and read the bars off the screen: bar 6 followed the ramp, so
     index 6 was concluded to be 6 rather than the 0x14 every reference quotes, and
     bars 8..15 showed the EGA brights, so the high eight were concluded to be
     0x38..0x3F. In mode 0Dh the high eight are really 0x10..0x17 -- and the card
     could not tell, because mode 0Dh's default DAC repeats the same sixteen colours
     four times across 0..0x3F, so 0x10..0x17 and 0x38..0x3F hold identical values.
     A bar's colour is a reading of the whole chain and cannot say which link differs.
   ⚠ And the card ran in 0Dh while the guest that prompted it plays in 10h, where
     index 6 IS 0x14. Lemmings carries that exact table inside VGALEMMI.EXE and uses
     it to choose which DAC entries to program, so with 6 there its colour 6 was
     written to DAC 0x14 and read back from DAC 0x06. */
static INT VideoDefaultsRow(BYTE mode)
{
    UINT index;
    for (index = 0; index < VGA_DEFAULT_MODES; ++index)
        if (VGA_DEFAULT_BY_MODE[0][index] == mode) return (INT)index;
    /* A mode the BIOS has no table for: a VESA mode, or one nobody defines. Above
       13h means 256 colours, so 13h's defaults; below it, mode 3's. st->ModeKind is
       not set yet at the point this runs, so the mode number is all there is. */
    for (index = 0; index < VGA_DEFAULT_MODES; ++index)
        if (VGA_DEFAULT_BY_MODE[0][index] == (mode >= 0x13 ? 0x13 : 0x03)) return (INT)index;
    return 0;
}

static VOID VideoDefaultsFor(BYTE mode,
                             const BYTE **attributes, const unsigned long **dacTable)   /* the generated VGA_DAC_DEFAULT type: ULONG is 32 bits off-VM */
{
    INT index = VideoDefaultsRow(mode);
    *attributes  = VGA_AC_DEFAULT [VGA_DEFAULT_BY_MODE[1][index]];
    *dacTable = VGA_DAC_DEFAULT[VGA_DEFAULT_BY_MODE[2][index]];
}

/* ── ★ A MODE SET REPROGRAMS THE CRTC, and not doing so lets one screen inherit the
     geometry of the one before it. Lemmings' gameplay sets Offset=22 (44 bytes to
     the line, for a 352-pixel-wide scrolling window); the mode 10h screen AFTER it
     was then drawn 44 bytes to the line instead of 80 and came out as diagonal
     noise. The BIOS writes all 25 registers on every mode set -- these values are
     measured per mode by tests/probes/dos/vgadefs.asm.
   ▶ THE VERTICAL TIMING IS NOW APPLIED TOO, and it had to be: the BIOS sets these
     registers, the guest does not, so a mode set is the ONLY place a BIOS-set mode
     ever learns its own geometry. Leaving it out was what kept 0x3DA on the old
     two-case guess -- and that guess is wrong by 45 lines in 640x350 (which is
     Lemmings' MENU; its gameplay is 0Dh). The renderer still takes its picture size from the mode
     table; what these feed is the CRT timing, which is a different question.
   ⚠ Line Compare's three registers are loaded here as well, so it holds the BIOS's
     all-ones "no split" rather than a zero we merely happen to treat as inert. */
static VOID VideoCrtcLineCompareUpdate(PVIDEO_STATE st);
static VOID VideoCrtcVerticalTimingUpdate(PVIDEO_STATE st);
static VOID VideoLatch(PVIDEO_STATE st, INT at_frame);   /* display start/pan schedule (s83) */
static UINT64 VideoVesaVblRelease(PVIDEO_STATE st, INT *now_in_vbl);   /* 4F07 BL=80h (#226) */
static INT VideoBeam(PCVIDEO_STATE st, UINT64 *now, UINT32 *frame_us,
                    UINT32 *vtotal, UINT32 *vdisp, UINT32 *vblank,
                    UINT32 *frame_no, UINT32 *line);
static VOID VideoLoadDefaultCrtc(PVIDEO_STATE state)
{
    const BYTE *crtc = VGA_CRTC_DEFAULT[VGA_DEFAULT_BY_MODE[3][VideoDefaultsRow(state->Mode)]];
    state->CrtcIndex = 0;
    state->CrtcOffset = crtc[VGA_CRTC_OFFSET];
    state->CrtcStart = (UINT32)(((UINT)crtc[VGA_CRTC_START_HI] << 8) | crtc[VGA_CRTC_START_LO]);
    state->CrtcStartLive = (WORD)state->CrtcStart;   /* the display follows at once */
    state->StartVs        = (WORD)state->CrtcStart;
    state->LatchTime         = 0;                           /* VideoLatch: take everything as-is */
    state->IsCrtcStartPending = 0;
    state->IsCrtcOffsetSeen = 0;
    state->CrtcOverflow   = crtc[0x07];
    state->CrtcMaxScan    = crtc[0x09];
    state->CrtcLineCompareLow     = crtc[0x18];
    state->CrtcVerticalTotalLow  = crtc[0x06];
    state->CrtcVerticalDisplayEndLow     = crtc[0x12];
    state->CrtcVerticalBlankStartLow     = crtc[0x15];
    VideoCrtcLineCompareUpdate(state);
    VideoCrtcVerticalTimingUpdate(state);
    state->IsDirty = 1;
}

/* ── ★★★ THE RENDER PALETTE IS DERIVED, NOT STORED. ─────────────────────────────
     pal[] is what the 8-bit framebuffer indexes; dac[] is what the guest programmed.
     In a 16-colour mode they differ, because the hardware puts the Attribute
     Controller between them:
         pixel (4 bits) -> vpal[pixel] -> 6-bit DAC index -> dac[]
     Rebuilding pal[0..15] through that chain is what makes a guest's palette
     actually appear. Mode 13h bypasses the AC (its 8-bit pixel goes straight to the
     DAC), so there pal[] is simply dac[].

   ⚠ THE FIRST CUT OF THIS WAS WRONG IN A WAY WORTH RECORDING: it wrote a COMPUTED
     EGA colour into pal[0..15] and never consulted dac[] at all. That made the test
     card pass -- the card uses the default DAC -- while leaving Lemmings exactly as
     broken, because Lemmings programs the DAC and then points the AC at it. It also
     clobbered the guest's DAC entries, since pal[] and dac[] were the same array.
     A derived table has to derive from something; substituting a plausible value for
     the real one is the same mistake as a stepped-over call returning a sentinel. */
static VOID VideoPaletteSplitNote(PVIDEO_STATE state, const UINT32 *prev);
static INT  VideoBeam(PCVIDEO_STATE st, UINT64 *now, UINT32 *frame_us,
                     UINT32 *vtotal, UINT32 *vdisp, UINT32 *vblank,
                     UINT32 *frame_no, UINT32 *line);   /* fwd: defined with VideoStatusIn */
static VOID VideoPaletteRefresh(PVIDEO_STATE state)
{
    INT index;
    UINT32 prev[256];
    for (index = 0; index < 256; ++index) prev[index] = state->Palette[index];
    /* A packed 8bpp VESA mode indexes the DAC directly, exactly as mode 13h does.
       (s74b) It used to fall through to the attribute-controller path below because a
       4F02 mode set never touches mkind, so pixels 0..15 went through the EGA remap
       (6 -> DAC 0x14, 8..15 -> 0x38..0x3F) and a VESA guest's first sixteen colours
       were somebody else's. */
    if (state->ModeKind == VIDEO_KIND_LINEAR8 || (state->IsVesa && state->VesaBpp <= 8)) {
        for (index = 0; index < 256; ++index) state->Palette[index] = state->Dac[index];
    } else {
        for (index = 0; index < 16; ++index) {
            BYTE value = (BYTE)(state->PaletteRegisters[index] & 0x3F);
            /* AR14 Color Select supplies the high DAC bits on a real VGA; zero by
               default, which makes this the identity. */
            if (state->AttributeMode & 0x80)
                value = (BYTE)((value & 0x0F) | ((state->AttributeColorSelect & 0x03) << 4));
            value = (BYTE)(value | ((state->AttributeColorSelect & 0x0C) << 4));
            state->Palette[index] = state->Dac[value];
        }
        for (index = 16; index < 256; ++index) state->Palette[index] = state->Dac[index];
    }
    VideoPaletteSplitNote(state, prev);
    state->IsDirty = 1;
}

/* ── ★★★★ A PALETTE WRITE MID-FRAME IS A RASTER SPLIT, NOT A NEW PALETTE. ─────────
     s70, the user's "flickers between the right and wrong colours" (Lemmings #1/#2).
     Lemmings' HP-mode timer tick is calibrated to land 320 scanlines after the
     retrace -- pixel row 160, the top of the toolbar -- and the tick's first act is
     to push one set of DAC entries 16..23 (guest ds:2668); after the retrace it
     pushes another (ds:2650). The level is drawn in one set, the toolbar in the
     other, and the beam position is what separates them. The presenter took ONE
     palette per frame, so whichever set the snapshot caught coloured the whole
     screen, and the alternation between the two was the flicker. (The real-DOS
     oracle under QEMU shows one set everywhere too: its default 0x3DA model makes
     the retrace wait return at once, so the two writes land back to back. The
     oracle is not truth for a raster effect; the game's own tables and timing are.)
   ► So per entry: the value at the START of the frame (`PaletteBase`), the value written
     MID-frame and the row it was written at (`PaletteSplit`/`PaletteSplitRow`), stamped
     with the frame number so a guest that stops splitting is back to one palette a
     frame later. Writes during blanking or on the first two rows are the frame's
     base (Lemmings' post-retrace push lands on row 0). The first write of a new
     frame rebases: what the palette held before it IS what the DAC held at the
     frame start. VddFramePaletteAt resolves a row; the presenter and the capture
     apply it, video_test pins it with a fake clock. */
#define VIDEO_SPLIT_STICK 12u   /* rows: IRQ jitter measured at +-5 rows (s70), with margin */
static VOID VideoPaletteSplitNote(PVIDEO_STATE state, const UINT32 *prev)
{
    UINT64 now; UINT32 frameUs, verticalTotal, verticalDisplay, verticalBlank, frameNumber, line, row;
    INT index, split;
    if (!VideoBeam(state, &now, &frameUs, &verticalTotal, &verticalDisplay, &verticalBlank, &frameNumber, &line)
        || !state->GraphicsHeight || !verticalDisplay) {
        for (index = 0; index < 256; ++index) if (state->Palette[index] != prev[index]) state->PaletteBase[index] = state->Palette[index];
        return;
    }
    if (frameNumber != state->PaletteFrameNumber) {           /* first write of this frame: rebase */
        for (index = 0; index < 256; ++index) state->PaletteBase[index] = prev[index];
        state->PaletteFrameNumber = frameNumber;
    }
    row   = (line < verticalDisplay) ? (UINT32)(((UINT64)line * state->GraphicsHeight) / verticalDisplay) : 0xFFFFu;
    split = (row != 0xFFFFu && row >= 2u);
    for (index = 0; index < 256; ++index) {
        if (state->Palette[index] == prev[index]) continue;
        if (split) {
            /* ── THE BOUNDARY IS STICKY (s70, the user's HP run). The tick that writes
                 this split is an IRQ we deliver with ~100-300 us of jitter, so its row
                 wandered 160..169 frame to frame (heartbeat `lastrow`). Drawn faithfully
                 that is a ten-row band flickering between palettes -- worse than the
                 single-palette picture it replaced. A real PC's IRQ lands within a
                 microsecond of the same row every frame. So a mid-frame write within
                 VIDEO_SPLIT_STICK rows of this entry's live split keeps the OLD row: the
                 boundary stays where the guest first put it, and only a genuinely
                 different row (a new effect) moves it. */
            UINT32 oldRow = state->PaletteSplitRow[index];
            INT live = oldRow && (UINT32)(frameNumber - state->PaletteSplitFrame[index]) <= 2u;
            UINT32 distance = oldRow > row ? oldRow - row : row - oldRow;
            state->PaletteSplit[index]       = state->Palette[index];
            state->PaletteSplitRow[index]   = (live && distance <= VIDEO_SPLIT_STICK) ? (WORD)oldRow : (WORD)row;
            state->PaletteSplitFrame[index] = frameNumber;
            state->PaletteSplitNotes++;
        } else {
            state->PaletteBase[index] = state->Palette[index];
        }
    }
}

VOID VddVideoFrameTouch(PVIDEO_STATE state)
{
    UINT64 now; UINT32 frameUs, verticalTotal, verticalDisplay, verticalBlank, frameNumber, line;
    state->Frame.PaletteBase  = state->PaletteBase;
    state->Frame.PaletteSplit = state->PaletteSplit;
    state->Frame.SplitRow     = state->IsBlanked ? 0 : state->PaletteSplitRow;   /* SR1.5 (#266) */
    state->Frame.SplitFrame   = state->PaletteSplitFrame;
    state->Frame.FrameNumber = VideoBeam(state, &now, &frameUs, &verticalTotal, &verticalDisplay, &verticalBlank, &frameNumber, &line)
                       ? frameNumber : state->PaletteFrameNumber;
}

/* AH=10h AL=1Bh's sum (30% red, 59% green, 11% blue), over DAC entries [first,
   first+n) -- and since #252 also what AH=12h BL=33h's summing applies to a mode
   set's palette and to AH=10h AL=10h/12h loads. */
static VOID VideoDacGrey(PVIDEO_STATE state, UINT first, UINT count)
{
    UINT index;
    for (index = 0; index < count && (first + index) < 256; ++index) {
        UINT32 value = state->Dac[first + index];
        UINT32 grey = ((((value >> 16) & 0xFF) * 30) + (((value >> 8) & 0xFF) * 59)
                      + ((value & 0xFF) * 11)) / 100;
        state->Dac[first + index] = 0xFF000000u | (grey << 16) | (grey << 8) | grey;
    }
}

/* A mode set reloads the DAC and the AC palette. Real hardware does this, and
   without it a program that reprogrammed the palette leaves the NEXT program (or
   the text screen it returns to) drawn in its colours -- usually near-black, so
   text mode looks dead. */
static VOID VideoLoadDefaultPalette(PVIDEO_STATE state)
{
    INT index;
    /* The guest asked us not to (AH=12h BL=31h). Leave both the DAC and the
       attribute palette exactly as it left them. */
    const BYTE *attributes; const unsigned long *dacTable;   /* not ULONG: see VideoDefaultsFor */
    for (index = 0; index < 256; ++index) state->PaletteSplitRow[index] = 0;   /* a mode set ends any raster split */
    if (state->IsDefaultPaletteOff) return;
    state->PaletteResets++;
    /* ⚠ dac_hi_since_reset ON ITS OWN CANNOT REPORT ANYTHING. The counters are
       printed after the guest has exited, and a guest exits through a mode set back
       to text -- so "since the last reset" is always "since a moment after the last
       thing the guest drew", and the answer is always zero. It read zero for
       Lemmings and was taken as evidence that a mode set had wiped a palette the
       game never rewrote; the port trace shows the game sets the mode and THEN
       writes the palette, which is the only order that can work on real hardware.
       Carry the running maximum too, so the epoch that had the writes survives. */
    if (state->DacHighSinceReset > state->DacHighMax)
        state->DacHighMax = state->DacHighSinceReset;
    state->DacHighSinceReset = 0;
    VideoDefaultsFor(state->Mode, &attributes, &dacTable);
    for (index = 0; index < 16; ++index)   state->PaletteRegisters[index] = attributes[index];
    state->PaletteRegisters[16] = 0;
    state->AttributeFlipFlop = state->AttributeIndex = state->AttributeMode = state->AttributeColorSelect = 0;
    for (index = 0; index < 256; ++index)  state->Dac[index] = 0xFF000000u | (UINT32)dacTable[index];
    if (state->IsGreySum) VideoDacGrey(state, 0, 256);           /* AH=12h BL=33h (#252) */
    VideoPaletteRefresh(state);
}

/* ── 0x3C0 / 0x3C1: index and data on ONE port, alternating. ─────────────────────
     Write to 0x3C0 and the flip-flop decides whether it lands in the index or the
     data half. The flip-flop is reset by READING 0x3DA -- every guest does that read
     first, which is why VideoStatusIn resets it and why claiming 0x3DA was already
     necessary for this to work at all.
   ⚠ Bit 5 of the INDEX is "video enable" and is not part of the register number: a
     guest programming the palette clears it and sets it again when it has finished.
     Masking it off the index (0x1F) is the difference between writing register 0 and
     writing register 32. */
static VOID VideoAttributeOut(PVOID context, WORD port, BYTE width, UINT32 value)
{
    PVIDEO_STATE state = (PVIDEO_STATE)context;
    BYTE byteValue = (BYTE)(value & 0xFF);
    (VOID)width;
    if (port == 0x3C1) return;                       /* data port is read-only       */
    if (!state->AttributeFlipFlop) { state->AttributeIndex = byteValue; state->AttributeFlipFlop = 1; return; }
    state->AttributeFlipFlop = 0;
    if ((state->AttributeIndex & 0x1F) == 0x13) VideoLatch(state, 0);   /* pel panning: see VideoLatch */
    state->AttributeRegisters[state->AttributeIndex & 0x1F] = byteValue;
    state->AttributeWrites  [state->AttributeIndex & 0x1F]++;
    switch (state->AttributeIndex & 0x1F) {
    case 0x00: case 0x01: case 0x02: case 0x03:
    case 0x04: case 0x05: case 0x06: case 0x07:
    case 0x08: case 0x09: case 0x0A: case 0x0B:
    case 0x0C: case 0x0D: case 0x0E: case 0x0F:
        state->PaletteRegisters[state->AttributeIndex & 0x0F] = (BYTE)(byteValue & 0x3F);
        state->AcPortWrites++;
        VideoPaletteRefresh(state);
        break;
    case 0x10:
        /* ── ★ BIT 3 IS BLINK ENABLE, AND IT IS THE HARDWARE'S ANSWER, NOT OURS. ──
             We stored this register and kept a PRIVATE st->IsBlink beside it that only
             INT 10h 1003h could move -- so a program that turns blink off the usual
             way, by writing the attribute controller directly, was ignored and every
             character with attribute bit 7 went on blinking.
             Found in QBasic: its dialogs mark the accelerator letter with bit 7, so
             `Files` rendered as `iles` and `Help` as `elp` every other half-second --
             the letter was there, it was blinking. Two layers with their own copy of
             one fact, disagreeing; AR10 is now the single source. */
        state->AttributeMode = byteValue;
        state->IsBlink = (BYTE)((byteValue >> 3) & 1);
        VideoPaletteRefresh(state);
        break;
    case 0x11: state->PaletteRegisters[16] = state->Overscan = (BYTE)(byteValue & 0x3F); state->IsDirty = 1; break;
    case 0x14: state->AttributeColorSelect = byteValue;   VideoPaletteRefresh(state); break;
    default: break;                                  /* 12h plane enable, 13h pan    */
    }
}

static VOID VideoAttributeIn(PVOID context, WORD port, BYTE width, UINT32 *value)
{
    PVIDEO_STATE state = (PVIDEO_STATE)context;
    BYTE index = (BYTE)(state->AttributeIndex & 0x1F);
    (VOID)width;
    if (port == 0x3C0) { *value = state->AttributeIndex; return; }   /* index reads back at 3C0 */
    switch (index) {
    case 0x10: *value = state->AttributeMode; break;
    case 0x11: *value = state->PaletteRegisters[16];  break;
    case 0x14: *value = state->AttributeColorSelect;  break;
    default:   *value = (index < 16) ? state->PaletteRegisters[index] : state->AttributeRegisters[index]; break;  /* AR12/AR13 */
    }
}

/* The standard BIOS mode set.  Dimensions are the modes' documented geometry;
   what makes them right for US is that the renderer now honours them instead of
   forcing 80x25 text.  CGA modes 4/5/6 are marked UNSUPPORTED rather than
   approximated: they use a two-bank interleaved layout at B800 that shares
   nothing with the planar path, and quietly showing a text screen instead is the
   silent failure GH #27 exists to remove. */
typedef struct _VIDEO_MODE_ENTRY { BYTE Mode, Kind, Columns, Rows; WORD Width, Height; } VIDEO_MODE_ENTRY;
static const VIDEO_MODE_ENTRY g_VideoModes[] = {
    { 0x00, VIDEO_KIND_TEXT,    40, 25,   320, 400 },
    { 0x01, VIDEO_KIND_TEXT,    40, 25,   320, 400 },
    { 0x02, VIDEO_KIND_TEXT,    80, 25,   640, 400 },
    { 0x03, VIDEO_KIND_TEXT,    80, 25,   640, 400 },
    { 0x04, VIDEO_KIND_CGA,     40, 25,   320, 200 },   /* CGA 4-colour   */
    { 0x05, VIDEO_KIND_CGA,     40, 25,   320, 200 },   /* 4-colour, grey */
    { 0x06, VIDEO_KIND_CGA,     80, 25,   640, 200 },   /* CGA 2-colour   */
    { 0x07, VIDEO_KIND_TEXT,    80, 25,   640, 400 },   /* MDA mono text  */
    { 0x0D, VIDEO_KIND_PLANAR,  40, 25,   320, 200 },
    { 0x0E, VIDEO_KIND_PLANAR,  80, 25,   640, 200 },
    { 0x0F, VIDEO_KIND_PLANAR,  80, 25,   640, 350 },
    { 0x10, VIDEO_KIND_PLANAR,  80, 25,   640, 350 },
    { 0x11, VIDEO_KIND_PLANAR,  80, 30,   640, 480 },
    { 0x12, VIDEO_KIND_PLANAR,  80, 30,   640, 480 },
    { 0x13, VIDEO_KIND_LINEAR8, 40, 25,   320, 200 },
};

/* ── #252: THREE WAYS TO NAME A TEXT CELL, because a page is three things. ──────────
     pcell  page N's (r,c): where the character services write when the caller names
            a page (AH=09h/0Ah/02h... BH).
     cell   the ACTIVE page's (r,c): teletype, scroll, read -- what AH=05h selected.
     dcell  the DISPLAYED (r,c): from the CRTC start address, which is what the card
            shows. AH=05h moves it to the active page, and so does a guest that writes
            CR0C/CR0D itself (a text-mode page flip or smooth scroll).
   All three used to be one function that always meant page 0 at B800:0 -- so 05h
   flipped the BDA and nothing else, and a write to page 1 landed on page 0. The text
   window is 32 KB (B800-BFFF); an address past it wraps, as the card's does. */
static UINT VideoPageSize(PCVIDEO_STATE state);
static BYTE *VideoPageCell(PVIDEO_STATE state, INT page, INT row, INT column)
{
    UINT offset = (UINT)(page & 7) * VideoPageSize(state) + (UINT)(row * state->Columns + column) * 2u;
    return state->VideoMemory + VIDEO_TEXT_OFFSET + (offset & 0x7FFFu);
}
static BYTE *VideoCell(PVIDEO_STATE state, INT row, INT column)   /* -> char byte of (row,column) */
{ return VideoPageCell(state, state->Page, row, column); }
static BYTE *VideoDisplayCell(PVIDEO_STATE state, INT row, INT column)
{
    UINT offset = (UINT)state->CrtcStartLive * 2u + (UINT)(row * state->Columns + column) * 2u;
    return state->VideoMemory + VIDEO_TEXT_OFFSET + (offset & 0x7FFFu);
}

/* ── ONE BACKING STORE FOR A BIT-PLANE, WHOEVER WRITES IT. (s74b) ─────────────────
     The mode-12h engine wrote its four planes into st->Planes[] and VideoRenderPlanar read
     them back -- fine for a real-mode guest, whose pixel loop runs in the host
     interpreter and reaches the engine. A PROTECTED-MODE guest runs natively: its
     `memcpy` to A0000 lands wherever the host has mapped that window, which under the
     mode-Y remap is the SELECTED PLANE'S OWN SECTION (ymap_plane) -- the map-mask port
     handler moves the window on every mask write whenever chain-4 is off, mode 12h
     included. So Hexen's and Heretic's 640x480 loaders were written, in full, into
     memory nothing rendered: `planar hi_water=0`, black screen, for two guests.
     When the host supplies plane sections, the engine, the BIOS pixel services and
     the renderer all use them; without a host (the off-VM harness) st->Planes[] as
     before. The sections are MODEY_WIN = VIDEO_PLANE_SIZE bytes, so nothing changes size. */
static BYTE *VideoPlaneBytes(PVIDEO_STATE state, INT plane)
{ return state->YMapPlane ? state->YMapPlane(state->YMapContext, plane & 3) : state->Planes[plane & 3]; }

static VOID VideoClearText(PVIDEO_STATE state, BYTE attribute)
{
    INT count = state->Columns * state->Rows, index;
    BYTE *text = state->VideoMemory + VIDEO_TEXT_OFFSET;
    for (index = 0; index < count; ++index) { text[index*2] = ' '; text[index*2+1] = attribute; }
}

/* The number of text rows the loaded font gives on a 400-line VGA text screen. */
static BYTE VideoTextRowsFor(BYTE cellHeight)
{ return (BYTE)(cellHeight ? (400 / cellHeight) : 25); }

/* ── THE BDA DESCRIBES THE DISPLAY, AND A TEXT APPLICATION BELIEVES IT. ──────────
     0040:0049 mode, 004A columns, 004C page size, 004E page offset, 0050 the cursor
     per page, 0060 the cursor shape, 0062 active page, 0063 the CRTC base, 0084 rows
     minus one, 0085 the character height, 0087-0089 the EGA/VGA info bytes. All of
     it read as ZERO before this: rows-1 = 0 is a one-row screen to a program that
     sizes itself from 0040:0084, and the video-info bytes said "no EGA/VGA" to one
     that checks before it asks for 50 lines. Cheap enough to redo after every INT
     10h call and every frame -- a few dozen byte writes -- and correct by
     construction, since it is derived rather than maintained. */
/* The BIOS page size, 0040:004C -- and (#252) the stride AH=05h moves the CRTC start
   by and the character services address a page at. One function, so the BDA, the
   display and the writes cannot disagree about where page N is. */
static UINT VideoModePageSize(BYTE mode, BYTE kind, UINT columns, UINT rows)
{
    UINT pageSize;
    if (kind == VIDEO_KIND_TEXT) {
        pageSize = (columns * rows * 2u + 0xFFu) & ~0xFFu;
        if (pageSize < 0x800u) pageSize = 0x800u;
    } else {
        /* ── THE GRAPHICS PAGE SIZE IS PER MODE, and this was a flat 0x2000 for all
             of them. Measured on 6.22: mode 06h is 0x4000 and mode 12h is 0xA000,
             where we said 0x2000 either way. A program that pages by adding this to
             its offset lands inside the previous page. 06h/12h/13h are the
             oracle-verified rows (p_video.asm); the rest are the standard VGA BIOS
             table and are marked unverified in docs/inventory/video-bios.md. */
        switch (mode) {
        case 0x04: case 0x05: case 0x06: pageSize = 0x4000u; break;  /* 06h verified   */
        case 0x0D:                       pageSize = 0x2000u; break;
        case 0x0E:                       pageSize = 0x4000u; break;
        case 0x0F: case 0x10:            pageSize = 0x8000u; break;
        case 0x11: case 0x12:            pageSize = 0xA000u; break;  /* 12h verified   */
        case 0x13:                       pageSize = 0x2000u; break;  /* 13h verified   */
        default:                         pageSize = 0x2000u; break;
        }
    }
    return pageSize;
}
static UINT VideoPageSize(PCVIDEO_STATE state)
{ return VideoModePageSize(state->Mode, state->ModeKind, state->Columns, state->Rows); }

/* The cell height a BIOS mode set gives each standard mode: 8x16 in the VGA text modes
   and the 480-line graphics modes, 8x14 at 350 lines, 8x8 at 200. AH=00h and the
   parameter table (#266) both read it here, so they cannot disagree. */
static BYTE VideoModeCellHeight(BYTE mode)
{
    return (BYTE)((mode <= 0x03 || mode == 0x07 || mode == 0x11 || mode == 0x12) ? 16
                   : (mode == 0x0F || mode == 0x10) ? 14 : 8);
}

/* ── #266: THE VECTORS THE VIDEO BIOS OWNS, KEPT IN THE IVT. INT 43h (the graphics
     character table) and INT 1Fh (8x8 characters 80h-FFh) are pointers a program
     READS -- to draw text itself, or to find the font the BIOS will draw with -- and
     nothing in src/ wrote either: the IVT held whatever the VDM started with, which
     on the rig is the real machine's BIOS, not the tables our INT 10h draws from.
     Written through the bus so the off-VM battery's flat memory gets them too. */
static VOID VideoSetVector(PVIDEO_STATE state, BYTE vector, WORD segment, WORD offset)
{
    BYTE *value;
    if (!state->Bus) return;
    value = (BYTE *)VddMapFlat(state->Bus, 0, (WORD)(vector * 4u));
    if (!value) return;
    value[0] = (BYTE)offset; value[1] = (BYTE)(offset >> 8);
    value[2] = (BYTE)segment; value[3] = (BYTE)(segment >> 8);
}
/* INT 43h -> our ROM copy of the font a cell of `height` lines draws with. */
static VOID VideoInt43Rom(PVIDEO_STATE state, BYTE height)
{
    state->Int43Segment = height == 8 ? VDD_FONT8X8_SEG : height == 14 ? VDD_FONT8X14_SEG : VDD_FONT8X16_SEG;
    state->Int43Offset = 0;
    state->IsGraphicsFontUser = 0;
    VideoSetVector(state, 0x43, state->Int43Segment, state->Int43Offset);
}
/* INT 1Fh -> the upper half of our 8x8 table (power-on; AH=11h AL=20h replaces it). */
static VOID VideoInt1FRom(PVIDEO_STATE state)
{
    state->Int1FSegment = VDD_FONT8X8_SEG; state->Int1FOffset = 128 * 8; state->IsInt1FUser = 0;
    VideoSetVector(state, 0x1F, state->Int1FSegment, state->Int1FOffset);
}

/* ── #266: AH=0Bh's arithmetic, from the CGA it emulates. 0040:0066 is the CGA's colour
     select register (3D9h): bits 0-3 the background (border in text, the 320x200
     background, the 640x200 foreground), bit 4 the intensity of palette colours 1-3,
     bit 5 the palette. A VGA does not decode 3D9h; the BIOS keeps the byte in the BDA
     and turns it into attribute-controller values (vdd_video.h). The arithmetic is the
     one DOSBox's INT10_SetBackgroundBorder/SetColorSelect carry; what pins it is that
     it reproduces the MEASURED mode 04h table from the mode set's own 0066 = 30h:
     AR01-03 = 13h/15h/17h (vga_modedefs.h, PCem's IBM ROM). */
BYTE VddCgaColourSelect(BYTE current66, BYTE bh, BYTE bl)
{
    if (bh == 0) return (BYTE)((current66 & 0xE0u) | (bl & 0x1Fu));
    return (BYTE)((current66 & 0xDFu) | ((bl & 1u) << 5));
}
BYTE VddCgaBackgroundAr(BYTE bl)
{ return (BYTE)(((bl << 1) & 0x10u) | (bl & 0x07u)); }
VOID VddCgaPaletteAr(BYTE select66, BYTE attributes[3])
{
    BYTE value = (BYTE)((select66 & 0x10u) | 0x02u | ((select66 >> 5) & 1u));
    attributes[0] = value; attributes[1] = (BYTE)(value + 2u); attributes[2] = (BYTE)(value + 4u);
}
BYTE VddGraphicsFontRows(BYTE bl, BYTE dl)
{
    switch (bl) {
    case 0x00: return dl;
    case 0x01: return 14;
    case 0x03: return 43;
    default:   return 25;               /* 02h, and SeaVGABIOS's answer to the rest */
    }
}

/* One AC palette register as the BIOS writes it: the register file AND the shadow the
   renderer reads (vpal; 11h = vpal[16]/overscan). Not a guest write: attr_w untouched. */
static VOID VideoAttributeBiosSet(PVIDEO_STATE state, BYTE index, BYTE value)
{
    value = (BYTE)(value & 0x3F);
    state->AttributeRegisters[index & 0x1F] = value;
    if (index < 16) state->PaletteRegisters[index] = value;
    else if (index == 0x11) state->PaletteRegisters[16] = state->Overscan = value;
    state->AcBiosWrites++;
}

VOID VddVideoBdaSync(PVIDEO_STATE state)
{
    BYTE *bda = state->BiosData;
    UINT pageSize;
    if (!bda) return;
    bda[0x49] = state->Mode;
    bda[0x4A] = state->Columns; bda[0x4B] = 0;
    pageSize = VideoPageSize(state);
    bda[0x4C] = (BYTE)pageSize; bda[0x4D] = (BYTE)(pageSize >> 8);
    { UINT pageOffset = (UINT)state->Page * pageSize;
      bda[0x4E] = (BYTE)pageOffset; bda[0x4F] = (BYTE)(pageOffset >> 8); }
    /* All eight cursors (#252) -- the active page's from cur_row/cur_col, the rest
       from the per-page store. Only the active slot used to be written, so a guest
       reading another page's cursor from the BDA read whatever was left there. */
    { UINT page;
      for (page = 0; page < 8; ++page) {
          INT isActive = (page == (UINT)(state->Page & 7));
          bda[0x50 + page * 2] = isActive ? state->CursorColumn : state->PageColumn[page];
          bda[0x51 + page * 2] = isActive ? state->CursorRow : state->PageRow[page];
      } }
    {   /* 0040:0060 follows the same rule as AH=03h: no text cursor in graphics. */
        WORD shape = (state->ModeKind == VIDEO_KIND_TEXT) ? state->CursorShape : 0;
        bda[0x60] = (BYTE)shape; bda[0x61] = (BYTE)(shape >> 8); }
    bda[0x62] = state->Page;
    { UINT crtc = (state->Mode == 0x07) ? 0x3B4u : 0x3D4u;
      bda[0x63] = (BYTE)crtc; bda[0x64] = (BYTE)(crtc >> 8); }
    bda[0x84] = (BYTE)(state->Rows ? state->Rows - 1 : 24);
    bda[0x85] = state->CellHeight; bda[0x86] = 0;
    /* 256K, EGA/VGA active, cursor emulation on; bit 7 = the last mode set (AH=00h AL
       bit 7, or 4F02h D15) did not clear memory -- see VideoInt10 AH=00h and vesa 4F02h. */
    bda[0x87] = (BYTE)(0x60 | (state->IsModeSetNoClear ? 0x80 : 0x00)
                        | (state->IsCursorEmulationOff ? 0x01 : 0x00));      /* bit 0: 12h BL=34h (#252) */
    bda[0x88] = 0x09;                                    /* feature/switch bits: enhanced colour */
    /* 0089: bit 0 = VGA active; bits 7,4 = scan lines (0,0 = 350; 0,1 = 400; 1,0 = 200). */
    bda[0x89] = (BYTE)((state->GraphicsHeight == 200) ? 0x81 : (state->GraphicsHeight == 350) ? 0x01 : 0x11);
}

/* ══ #252: THE CHARACTER SERVICES IN A GRAPHICS MODE DRAW, IN THAT MODE'S LAYOUT. ══
     AH=09h/0Ah/0Eh/13h used to write (char, attr) pairs through cell() -- B800:0 --
     in EVERY mode. In mode 13h that is memory the screen does not show (text
     vanished); in the CGA modes 04h-06h it IS the frame buffer, so the pairs came out
     as pixel noise; and the planar modes alone got a glyph, always 8x16 at a 640-pixel
     stride, which is right for 11h/12h only. A VGA BIOS draws the ROM glyph for the
     mode's own cell (8x8 at 200 lines, 8x14 at 350, 8x16 at 480 -- cell_h) into the
     mode's own memory:
       13h      one byte a pixel, `GraphicsWidth` bytes a line, foreground BL, background 0;
       04h/05h  two bits a pixel across the two interleaved CGA banks (even lines at
                B800:0, odd at B800:2000), 80 bytes a line, colour BL&3;
       06h      one bit a pixel, same banks, colour BL&1;
       0Dh-12h  one bit a pixel per PLANE, gw/8 bytes a line, page N at N*pagesize,
                colour BL&0Fh written plane by plane (bits 4-6 are NOT a background
                -- the old planar path took one from them).
     BL bit 7 = XOR the glyph onto what is there, in every mode but 13h (p_vidtxt:
     PCem's IBM VGA ROM is the authority). Background pixels are written 0 otherwise.
     Measured against PCem's IBM VGA ROM, DOSBox-X and SeaVGABIOS by p_vidtxt.asm. */
static const BYTE *VideoGraphicsFont(PCVIDEO_STATE state, BYTE character, INT *height)
{
    /* #266: a CALLER-SUPPLIED table (AH=11h AL=21h -> INT 43h, AL=20h -> INT 1Fh) is
       drawn from where the vector points, as the BIOS draws: SeaVGABIOS's get_font_data
       takes INT 1Fh for characters 80h-FFh of an 8-line cell, INT 43h for the rest. The
       ROM case is unchanged -- our vectors point at copies of these same tables. */
    if (state->Bus && (state->IsGraphicsFontUser || state->IsInt1FUser) && state->CellHeight >= 1 && state->CellHeight <= 16) {
        const BYTE *table = 0;
        *height = state->CellHeight;
        if (*height == 8 && character >= 0x80 && state->IsInt1FUser)
            table = (const BYTE *)VddMapFlat(state->Bus, state->Int1FSegment,
                                              (WORD)(state->Int1FOffset + (character - 0x80u) * 8u));
        else if (state->IsGraphicsFontUser)
            table = (const BYTE *)VddMapFlat(state->Bus, state->Int43Segment,
                                              (WORD)(state->Int43Offset + (UINT)character * (UINT)*height));
        if (table) return table;
    }
    *height = state->CellHeight == 14 ? 14 : state->CellHeight == 16 ? 16 : 8;
    return *height == 8 ? vga_font_8x8[character] : *height == 14 ? vga_font_8x14[character] : vga_font_8x16[character];
}

/* One glyph row's worth of pixels at character cell (col,row) of page pg -- the
   address arithmetic for every graphics kind lives here and in VideoGraphicsRowAddress, so the
   draw, the scroll and the read-back cannot disagree. */
static VOID VideoGraphicsGlyph(PVIDEO_STATE state, INT page, INT column, INT row, BYTE character, BYTE colour)
{
    INT height, glyphY, plane, isXor = (colour & 0x80) != 0;
    const BYTE *glyph = VideoGraphicsFont(state, character, &height);
    /* A VESA mode: our ModeInfoBlock says D2 = 0, "BIOS TTY output not supported",
       and the A0000 window is one bank of a bigger picture -- draw nothing. */
    /* ...nor in an UNCHAINED 256-colour mode (mode Y: chain-4 off): A0000 is then the
       plane-mapped window and a chained byte store would land in one plane. */
    if (state->IsVesa || (state->ModeKind == VIDEO_KIND_LINEAR8 && !state->IsChain4)) return;
    if (column < 0 || row < 0 || column >= state->Columns || row >= state->Rows) return;
    if (state->ModeKind == VIDEO_KIND_LINEAR8) {
        UINT32 width = state->GraphicsWidth ? state->GraphicsWidth : VIDEO_MODE13_WIDTH;
        for (glyphY = 0; glyphY < height; ++glyphY) {
            UINT32 offset = (UINT32)(row * height + glyphY) * width + (UINT32)column * 8u;
            INT glyphX;
            if (offset + 8u > VIDEO_APERTURE_SIZE) return;
            for (glyphX = 0; glyphX < 8; ++glyphX)
                state->VideoMemory[offset + glyphX] = (glyph[glyphY] & (0x80 >> glyphX)) ? colour : 0;
        }
    } else if (state->ModeKind == VIDEO_KIND_PLANAR) {
        UINT32 bytesPerRow = (UINT32)(state->GraphicsWidth ? state->GraphicsWidth : VIDEO_MODE12_WIDTH) / 8u;
        UINT32 base = (UINT32)(page & 7) * VideoPageSize(state);
        for (glyphY = 0; glyphY < height; ++glyphY) {
            UINT32 offset = base + (UINT32)(row * height + glyphY) * bytesPerRow + (UINT32)column;
            if (offset >= VIDEO_PLANE_SIZE) return;
            for (plane = 0; plane < 4; ++plane) {
                BYTE foreground = (BYTE)(((colour >> plane) & 1) ? glyph[glyphY] : 0);
                if (isXor) VideoPlaneBytes(state,plane)[offset] ^= foreground; else VideoPlaneBytes(state,plane)[offset] = foreground;
            }
        }
    } else if (state->ModeKind == VIDEO_KIND_CGA) {
        BYTE *memory = state->VideoMemory + VIDEO_TEXT_OFFSET;
        for (glyphY = 0; glyphY < height; ++glyphY) {
            INT line = row * height + glyphY;
            UINT32 offset = ((line & 1) ? 0x2000u : 0u) + (UINT32)(line >> 1) * 80u;
            BYTE bits = glyph[glyphY];
            if (state->CgaBpp == 1) {
                BYTE value = (BYTE)((colour & 1) ? bits : 0);
                offset += (UINT32)column;
                if (isXor) memory[offset] ^= value; else memory[offset] = value;
            } else {
                WORD value = 0; INT bit;
                for (bit = 0; bit < 8; ++bit)              /* 8 pixels -> 16 bits, MSB first */
                    if (bits & (0x80 >> bit)) value |= (WORD)((colour & 3u) << (14 - 2 * bit));
                offset += (UINT32)column * 2u;
                if (isXor) { memory[offset] ^= (BYTE)(value >> 8); memory[offset + 1] ^= (BYTE)value; }
                else   { memory[offset]  = (BYTE)(value >> 8); memory[offset + 1]  = (BYTE)value; }
            }
        }
    }
    state->IsDirty = 1;
}

/* AH=08h in a graphics mode: there is no character code in memory, so the BIOS reads
   the cell's pixels back (non-zero = foreground) and looks the pattern up in the font
   it draws with. No match = 0. */
static BYTE VideoGraphicsReadChar(PVIDEO_STATE state, INT page, INT column, INT row)
{
    BYTE pattern[16];
    INT height, glyphY, candidate;
    (VOID)VideoGraphicsFont(state, 0, &height);
    if (state->IsVesa || column < 0 || row < 0 || column >= state->Columns || row >= state->Rows) return 0;
    for (glyphY = 0; glyphY < height; ++glyphY) {
        INT line = row * height + glyphY, glyphX;
        BYTE bits = 0;
        if (state->ModeKind == VIDEO_KIND_LINEAR8) {
            UINT32 width = state->GraphicsWidth ? state->GraphicsWidth : VIDEO_MODE13_WIDTH, offset = (UINT32)line * width + (UINT32)column * 8u;
            if (offset + 8u > VIDEO_APERTURE_SIZE) return 0;
            for (glyphX = 0; glyphX < 8; ++glyphX) if (state->VideoMemory[offset + glyphX]) bits |= (BYTE)(0x80 >> glyphX);
        } else if (state->ModeKind == VIDEO_KIND_PLANAR) {
            UINT32 bytesPerRow = (UINT32)(state->GraphicsWidth ? state->GraphicsWidth : VIDEO_MODE12_WIDTH) / 8u;
            UINT32 offset = (UINT32)(page & 7) * VideoPageSize(state) + (UINT32)line * bytesPerRow + (UINT32)column;
            INT plane;
            if (offset >= VIDEO_PLANE_SIZE) return 0;
            for (plane = 0; plane < 4; ++plane) bits |= VideoPlaneBytes(state, plane)[offset];
        } else if (state->ModeKind == VIDEO_KIND_CGA) {
            const BYTE *memory = state->VideoMemory + VIDEO_TEXT_OFFSET + ((line & 1) ? 0x2000u : 0u) + (UINT32)(line >> 1) * 80u;
            if (state->CgaBpp == 1) bits = memory[column];
            else {
                WORD value = (WORD)((memory[column * 2] << 8) | memory[column * 2 + 1]);
                for (glyphX = 0; glyphX < 8; ++glyphX) if ((value >> (14 - 2 * glyphX)) & 3u) bits |= (BYTE)(0x80 >> glyphX);
            }
        } else return 0;
        pattern[glyphY] = bits;
    }
    for (candidate = 0; candidate < 256; ++candidate) {
        INT glyphHeight; const BYTE *glyph = VideoGraphicsFont(state, (BYTE)candidate, &glyphHeight);
        for (glyphY = 0; glyphY < height && glyph[glyphY] == pattern[glyphY]; ++glyphY) ;
        if (glyphY == height) return (BYTE)candidate;
    }
    return 0;
}

/* The bytes of one pixel line y (0..gh-1) for character columns [left, right], and
   how many: the unit VideoGraphicsScroll moves. Planar returns plane 0's; the caller adds
   the same offset into the other three. */
static BYTE *VideoGraphicsRowAddress(PVIDEO_STATE state, INT plane, INT line, INT left, INT right, UINT32 *count)
{
    if (state->ModeKind == VIDEO_KIND_LINEAR8) {
        UINT32 width = state->GraphicsWidth ? state->GraphicsWidth : VIDEO_MODE13_WIDTH, offset = (UINT32)line * width + (UINT32)left * 8u;
        *count = (UINT32)(right - left + 1) * 8u;
        return (offset + *count <= VIDEO_APERTURE_SIZE) ? state->VideoMemory + offset : 0;
    }
    if (state->ModeKind == VIDEO_KIND_PLANAR) {
        UINT32 bytesPerRow = (UINT32)(state->GraphicsWidth ? state->GraphicsWidth : VIDEO_MODE12_WIDTH) / 8u;
        UINT32 offset = (UINT32)(state->Page & 7) * VideoPageSize(state) + (UINT32)line * bytesPerRow + (UINT32)left;
        *count = (UINT32)(right - left + 1);
        return (offset + *count <= VIDEO_PLANE_SIZE) ? VideoPlaneBytes(state, plane) + offset : 0;
    }
    if (state->ModeKind == VIDEO_KIND_CGA) {
        UINT32 bytesPerPixelGroup = state->CgaBpp == 1 ? 1u : 2u;
        UINT32 offset = ((line & 1) ? 0x2000u : 0u) + (UINT32)(line >> 1) * 80u + (UINT32)left * bytesPerPixelGroup;
        *count = (UINT32)(right - left + 1) * bytesPerPixelGroup;
        return (offset + *count <= 0x4000u) ? state->VideoMemory + VIDEO_TEXT_OFFSET + offset : 0;
    }
    *count = 0; return 0;
}

/* AH=06h/07h and the teletype's scroll in a graphics mode: whole character rows of
   the window move by `lines` (up or down), and the rows uncovered are filled with
   colour `fill` -- BH for 06h/07h, 0 for the teletype. lines = 0 (or more than the
   window) clears it. */
static VOID VideoGraphicsScroll(PVIDEO_STATE state, INT lines, INT top, INT left, INT bottom, INT right,
                       BYTE fill, INT isUp)
{
    INT cellHeight = state->CellHeight == 14 ? 14 : state->CellHeight == 16 ? 16 : 8;
    INT line, firstLine, lastLine, plane, planeCount = state->ModeKind == VIDEO_KIND_PLANAR ? 4 : 1;
    if (state->IsVesa || (state->ModeKind == VIDEO_KIND_LINEAR8 && !state->IsChain4)) return;   /* as VideoGraphicsGlyph */
    if (right >= state->Columns) right = state->Columns - 1;
    if (bottom >= state->Rows) bottom = state->Rows - 1;
    if (left > right || top > bottom) return;
    if (lines <= 0 || lines > bottom - top + 1) lines = bottom - top + 1;
    firstLine = top * cellHeight; lastLine = (bottom + 1) * cellHeight - 1;
    for (plane = 0; plane < planeCount; ++plane) {
        BYTE fillByte;
        if (state->ModeKind == VIDEO_KIND_PLANAR)    fillByte = (BYTE)(((fill >> plane) & 1) ? 0xFF : 0x00);
        else if (state->ModeKind == VIDEO_KIND_CGA)  fillByte = (BYTE)(state->CgaBpp == 1 ? ((fill & 1) ? 0xFF : 0)
                                                                            : (fill & 3) * 0x55);
        else                                 fillByte = fill;
        for (line = isUp ? firstLine : lastLine; isUp ? (line <= lastLine) : (line >= firstLine); line += isUp ? 1 : -1) {
            INT sourceLine = isUp ? line + lines * cellHeight : line - lines * cellHeight;
            UINT32 count, sourceCount, byteIndex;
            BYTE *destination = VideoGraphicsRowAddress(state, plane, line, left, right, &count), *source;
            if (!destination) continue;
            source = (sourceLine >= firstLine && sourceLine <= lastLine) ? VideoGraphicsRowAddress(state, plane, sourceLine, left, right, &sourceCount) : 0;
            for (byteIndex = 0; byteIndex < count; ++byteIndex) destination[byteIndex] = source ? source[byteIndex] : fillByte;
        }
    }
    state->IsDirty = 1;
}

static VOID VideoScrollUp(PVIDEO_STATE state, INT lines, INT top, INT left,
                      INT bottom, INT right, BYTE attribute)
{
    INT row, column;
    if (state->ModeKind != VIDEO_KIND_TEXT) { VideoGraphicsScroll(state, lines, top, left, bottom, right, attribute, 1); return; }
    if (lines <= 0 || lines > (bottom - top + 1)) {
        for (row = top; row <= bottom; ++row)
            for (column = left; column <= right; ++column) { BYTE *cellPointer = VideoCell(state, row, column); cellPointer[0]=' '; cellPointer[1]=attribute; }
        return;
    }
    for (row = top; row <= bottom - lines; ++row)
        for (column = left; column <= right; ++column) {
            BYTE *destination = VideoCell(state, row, column), *source = VideoCell(state, row + lines, column);
            destination[0] = source[0]; destination[1] = source[1];
        }
    for (row = bottom - lines + 1; row <= bottom; ++row)
        for (column = left; column <= right; ++column) { BYTE *cellPointer = VideoCell(state, row, column); cellPointer[0]=' '; cellPointer[1]=attribute; }
}

/* The teletype's scroll fill: attribute 07h in text (as before), colour 0 -- the
   background -- in a graphics mode. */
static BYTE VideoTeletypeFill(PCVIDEO_STATE state) { return (BYTE)(state->ModeKind == VIDEO_KIND_TEXT ? 0x07 : 0x00); }

static VOID VideoAdvance(PVIDEO_STATE state)
{
    if (++state->CursorColumn >= state->Columns) {
        state->CursorColumn = 0;
        if (++state->CursorRow >= state->Rows) {
            VideoScrollUp(state, 1, 0, 0, state->Rows - 1, state->Columns - 1, VideoTeletypeFill(state));
            state->CursorRow = state->Rows - 1;
        }
    }
}

/* AH=0Eh, and DOS console output (VddVideoPutChar), on the ACTIVE page. `colour` is
   BL -- the glyph's foreground in a graphics mode, unused in text (the cell keeps
   its attribute). DOS's CON driver calls 0Eh with BL=07h, so that is what
   VddVideoPutChar passes. */
static VOID VideoTeletypeChar(PVIDEO_STATE state, BYTE character, BYTE colour)
{
    switch (character) {
    case 0x0D: state->CursorColumn = 0; break;
    case 0x0A:
        if (++state->CursorRow >= state->Rows) {
            VideoScrollUp(state, 1, 0, 0, state->Rows - 1, state->Columns - 1, VideoTeletypeFill(state));
            state->CursorRow = state->Rows - 1;
        }
        break;
    case 0x08: if (state->CursorColumn) state->CursorColumn--; break;
    case 0x07: break;
    default:
        if (state->ModeKind == VIDEO_KIND_TEXT) VideoCell(state, state->CursorRow, state->CursorColumn)[0] = character;
        else VideoGraphicsGlyph(state, state->Page, state->CursorColumn, state->CursorRow, character, colour);
        VideoAdvance(state);
    }
}
static VOID VideoTeletype(PVIDEO_STATE state, BYTE character) { VideoTeletypeChar(state, character, 0x07); }

/* --- VESA VBE 2.0 (banked, packed-256) ----------------------------------- */
/* supported modes: {VBE number, width, height} (all 8bpp packed) */
/* ── ★★ THE MODE LIST IS THE INTERFACE, AND A GUEST FILTERS ON IT. (s74) ──────────
     This was three 8bpp modes. heaven7 calls 4F00, walks the list we publish, asks
     4F01 about every entry -- we answered OK for all three -- and then printed its
     own "VESA error" WITHOUT EVER CALLING 4F02. Measured, in that order. A guest that
     wants direct colour will not settle for 640x400x8 however cheerfully we describe
     it, so the fix is the list and the ModeInfoBlock behind it, not the answer code.
     Numbers are the VBE standard assignments; 0x11x direct-colour modes are what
     anything from the late 90s actually asks for. Every entry here must fit in
     VIDEO_VESA_VRAM -- see the note there. */
typedef struct _VIDEO_VESA_MODE { WORD Number, Width, Height; BYTE Bpp; } VIDEO_VESA_MODE;
static const VIDEO_VESA_MODE g_VideoVesaModes[] = {
    /* packed-pixel 256-colour */
    { 0x100, 640, 400,  8 }, { 0x101, 640, 480,  8 },
    { 0x103, 800, 600,  8 }, { 0x105, 1024, 768, 8 },
    /* 1024x768 arrived with NTVDD_FRAME_MAX_WIDTH/H (s74b): the presenter's snapshot and
       this list are now sized from ONE number, so a mode cannot be offered that the
       presenter would drop. 0x105 is the 1024x768x8 every VESA game's setup lists. */
    /* ── 320x240, THE MODE HEAVEN7 ASKS FOR BY DEFAULT. (s74b) ───────────────────
         Not a VBE-numbered mode; it is an OEM mode, and the numbers below are the S3
         Trio's (0x151 8bpp, 0x160 15bpp, 0x170 16bpp), which is the set DOSBox and
         every game of the period expects. heaven7's render table is (320,176)
         (512,280) (640,352) (800,440) -- one letterboxed picture per screen height,
         and 320x176 belongs to 320x240, its default. Without this mode it fell back
         to 640x480 and drew its 320x176 picture in the bottom quarter of the screen,
         which read as "the geometry is wrong" for a whole session. A guest picks by
         XResolution/YResolution from 4F01, so the number itself is not load-bearing. */
    { 0x151, 320, 240,  8 }, { 0x160, 320, 240, 15 }, { 0x170, 320, 240, 16 },
    /* direct colour: 15/16/24bpp at the resolutions VRAM can back */
    { 0x10D, 320, 200, 15 }, { 0x10E, 320, 200, 16 }, { 0x10F, 320, 200, 24 },
    { 0x110, 640, 480, 15 }, { 0x111, 640, 480, 16 }, { 0x112, 640, 480, 24 },
    { 0x113, 800, 600, 15 }, { 0x114, 800, 600, 16 }, { 0x115, 800, 600, 24 },
    { 0x116, 1024, 768, 15 }, { 0x117, 1024, 768, 16 }, { 0x118, 1024, 768, 24 },
    { 0x107, 1280, 1024, 8 },
    { 0x119, 1280, 1024, 15 }, { 0x11A, 1280, 1024, 16 }, { 0x11B, 1280, 1024, 24 },
};
/* ── VESA TEXT MODES (VBE 1.2 §, mode numbers 108h..10Ch). (s74b) ─────────────────
     132-column text is what editors and file managers of the period ask for. A VESA
     text mode is an ordinary text mode with a different geometry: it lives at B800,
     goes through VIDEO_KIND_TEXT and every INT 10h text service unchanged, and the
     renderer already sizes the frame from cols x 8 and rows x cell_h. Only the
     bookkeeping is new: 4F01 answers in characters, 4F02 lands in text kind, 4F03
     remembers the VESA number (a standard mode set forgets it). */
typedef struct _VIDEO_VESA_TEXT_MODE { WORD Number; BYTE Columns, Rows, CellHeight; } VIDEO_VESA_TEXT_MODE;
static const VIDEO_VESA_TEXT_MODE g_VideoVesaTextModes[] = {
    { 0x108,  80, 60,  8 }, { 0x109, 132, 25, 16 }, { 0x10A, 132, 43,  8 },
    { 0x10B, 132, 50,  8 }, { 0x10C, 132, 60,  8 },
};
static INT VideoVesaFindText(WORD modeNumber, BYTE *columns, BYTE *rows, BYTE *cellHeight)
{
    UINT index;
    for (index = 0; index < sizeof(g_VideoVesaTextModes)/sizeof(g_VideoVesaTextModes[0]); ++index)
        if (g_VideoVesaTextModes[index].Number == (modeNumber & 0x3FFF)) {
            *columns = g_VideoVesaTextModes[index].Columns; *rows = g_VideoVesaTextModes[index].Rows;
            *cellHeight = g_VideoVesaTextModes[index].CellHeight; return 1;
        }
    return 0;
}
/* bytes per pixel as VBE counts them: 15bpp occupies 2 bytes, like 16. */
static UINT32 VideoVesaBytesPerPixel(BYTE bitsPerPixel) { return bitsPerPixel <= 8 ? 1u : bitsPerPixel <= 16 ? 2u : bitsPerPixel <= 24 ? 3u : 4u; }
/* Byte offset into vesa_vram of the pixel shown top-left, as the start REGISTER holds
   it (4F07 at the 4F06 pitch; 0 after a mode set). What is actually on screen is
   vesa_org_live -- the same value once the retrace has loaded it (VideoLatch). */
static UINT32 VideoVesaOrigin(PCVIDEO_STATE state) { return state->VesaOrigin; }
/* (x, y) at the current pitch -> the byte offset the register holds. */
static UINT32 VideoVesaXyOrigin(PCVIDEO_STATE state, UINT32 column, UINT32 line)
{ return line * state->VesaStride + column * VideoVesaBytesPerPixel(state->VesaBpp); }
/* §4.10: "if the requested Display Start coordinates do not allow for a full page of
   video memory ... the Function call should fail and no changes should be made". */
static INT VideoVesaOriginFits(PCVIDEO_STATE state, UINT32 origin)
{
    UINT64 end = (UINT64)origin + (UINT64)(state->VesaHeight ? state->VesaHeight - 1u : 0u) * state->VesaStride
                 + (UINT64)state->VesaWidth * VideoVesaBytesPerPixel(state->VesaBpp);
    return end <= VIDEO_VESA_VRAM;
}

/* ── ★ THE CRT A VESA MODE RUNS ON (#226). ────────────────────────────────────────────
     The retrace model (VideoBeam) took its geometry from the CRTC registers the LAST
     STANDARD MODE left behind -- a 4F02h programs none of them -- so a 640x480 VESA
     mode set from text mode "ran" at text mode's 449 lines and 70 Hz, and from mode
     12h at 60 Hz. Harmless while nothing in a VESA mode asked the beam anything but
     3DAh; 4F07h BL=80h now waits for it, so the answer must be the mode's own.
   ► THE VESA MODES' OWN TIMINGS, not the VGA CRTC's: a VBE BIOS programs its own
     (extended) timing for these, and the published ones are the VESA DMT set at
     60 Hz -- 525 total lines for 480, 628 for 600, 806 for 768, 1066 for 1024 --
     with the two VGA-derived heights kept at the VGA's numbers: 400 lines is the
     449-line 70 Hz frame, and the 200/240-line modes are DOUBLE-SCANNED to 400/480
     as mode 13h is (so 320x200 is 70 Hz and 320x240 is 60 Hz). Blanking starts at
     the end of the picture: the DMT modes carry no border. Anything else scales the
     480-line frame. Only the ratios and the rate matter to the model.
   Returns 0 outside a VESA graphics mode: the caller keeps the VGA path unchanged. */
static INT VideoVesaGeometry(PCVIDEO_STATE state, UINT32 *verticalTotal, UINT32 *verticalDisplay,
                      UINT32 *verticalBlank, UINT32 *hz)
{
    UINT32 lines;
    if (!state->IsVesa || !state->VesaHeight) return 0;
    lines = state->VesaHeight <= 240u ? state->VesaHeight * 2u : state->VesaHeight;   /* double scan */
    *hz = 60u;
    switch (lines) {
    case 400:  *verticalTotal = 449u;  *hz = 70u; break;
    case 480:  *verticalTotal = 525u;  break;
    case 600:  *verticalTotal = 628u;  break;
    case 768:  *verticalTotal = 806u;  break;
    case 1024: *verticalTotal = 1066u; break;
    default:   *verticalTotal = lines * 525u / 480u; if (*verticalTotal <= lines) *verticalTotal = lines + 1u; break;
    }
    *verticalDisplay = *verticalBlank = lines;
    return 1;
}
/* Record a VESA mode query and its answer -- see vesa_q[] in vdd_video.h. */
static VOID VideoVesaNote(PVIDEO_STATE state, BYTE function, WORD mode, INT isOk)
{
    UINT slot;
    for (slot = 0; slot < state->VesaQueryCount; ++slot)                  /* collapse repeats */
        if (state->VesaQueries[slot] == mode && state->VesaQueryFunction[slot] == function) return;
    if (state->VesaQueryCount >= sizeof(state->VesaQueries)/sizeof(state->VesaQueries[0])) return;
    slot = state->VesaQueryCount++;
    state->VesaQueries[slot] = mode; state->VesaQueryOk[slot] = (BYTE)(isOk ? 1 : 0); state->VesaQueryFunction[slot] = function;
}

static INT VideoVesaFind(WORD modeNumber, WORD *width, WORD *height, BYTE *bitsPerPixel)
{
    UINT index;
    for (index = 0; index < sizeof(g_VideoVesaModes)/sizeof(g_VideoVesaModes[0]); ++index)
        if (g_VideoVesaModes[index].Number == (modeNumber & 0x3FFF)) {
            UINT32 need = (UINT32)g_VideoVesaModes[index].Width * g_VideoVesaModes[index].Height
                          * VideoVesaBytesPerPixel(g_VideoVesaModes[index].Bpp);
            /* ⚠ A MODE WE CANNOT STORE IS NOT A MODE WE SUPPORT. Answering 4F01 for
                 geometry that does not fit VIDEO_VESA_VRAM invites a 4F02 we would have
                 to fail, or worse, blits off the end of the buffer. Checked here so
                 the list and the answer can never disagree. */
            if (need > VIDEO_VESA_VRAM) return 0;
            *width = g_VideoVesaModes[index].Width; *height = g_VideoVesaModes[index].Height;
            if (bitsPerPixel) *bitsPerPixel = g_VideoVesaModes[index].Bpp;
            return 1;
        }
    return 0;
}
static VOID VideoWrite16(BYTE *bytes, WORD value) { bytes[0] = (BYTE)value; bytes[1] = (BYTE)(value >> 8); }
static VOID VideoWrite32(BYTE *p, UINT32 v) { p[0]=(BYTE)v; p[1]=(BYTE)(v>>8); p[2]=(BYTE)(v>>16); p[3]=(BYTE)(v>>24); }

/* sync the live A0000 window into vesa_vram[current bank]. */
static VOID VideoVesaSync(PVIDEO_STATE st)
{
    UINT32 off = (UINT32)st->VesaBank * VIDEO_VESA_WINDOW; UINT i;
    /* ⚠ AN LFB GUEST NEVER WRITES A0000, so copying that window into vram would
         paint a stale (usually blank) 64KB hole over the frame it just drew. */
    if (st->IsVesaLfb) return;
    if (off + VIDEO_VESA_WINDOW > VIDEO_VESA_VRAM) return;
    for (i = 0; i < VIDEO_VESA_WINDOW; ++i) st->VesaVram[off + i] = st->VideoMemory[i];
}

/* Window A to bank `n` (64 KB units): flush the live window into its bank, load the new
   one. 0 = refused (no banked VESA mode, or past the end of VRAM) and nothing changes.
   ONE implementation for INT 10h 4F05h and the 4F0Ah protected-mode code's port write
   (#53), so the two cannot disagree about what a bank switch is. */
static INT VideoVesaSetBank(PVIDEO_STATE st, UINT32 n)
{
    UINT32 off = n * VIDEO_VESA_WINDOW; UINT k;
    if (!st->IsVesa || st->IsVesaLfb) return 0;
    if (off + VIDEO_VESA_WINDOW > VIDEO_VESA_VRAM) return 0;
    VideoVesaSync(st);                            /* flush the current bank first */
    st->VesaBank = (WORD)n;
    for (k = 0; k < VIDEO_VESA_WINDOW; ++k) st->VideoMemory[k] = st->VesaVram[off + k];
    st->IsDirty = 1;
    return 1;
}

/* ══ #53: THE 4F0Ah PROTECTED-MODE INTERFACE. ══════════════════════════════════════
     4F0Ah answered AX=0100h ("no such function") on purpose: there was no code to hand
     out, and 004Fh with a null pointer would have had a client call into nothing. Now
     there is: src/vdd/vbe_pm.asm, assembled into vbe_pm.h -- relocatable 32-bit code
     that drives the card through two ports, as a real card's block drives its own
     registers. A protected-mode client copies it and calls SetWindow / SetDisplayStart /
     SetPalette with a near call: no INT 10h, no mode switch, so a banked frame stops
     costing a DPMI 0300h round trip per bank.
   ► THE PORTS (ours; index at 01CEh, data at 01CFh -- the pair Bochs's VBE uses, and 05h
     is its bank index too; nothing else here is Bochs's, so index 00h, its ID register,
     reads 0 and no Bochs driver will take us for one):
       05h  bank: what 4F05h BH=00h BL=00h does (VideoVesaSetBank); reads back the bank
       10h  display start, bits 0-15, in DWORDS (byte address / 4)
       11h  display start, bits 16-31 -- the write that COMMITS, as 4F07h BL=00h: at once.
            The "during retrace" form waited on 3DAh in the guest's own code before this.
       03h  (read) bits per pixel, 06h (read) logical line in pixels -- what a client that
            wants to compute a start for itself needs; writes ignored.
     A write when no banked VESA mode is set (or one that would not fit) is refused and
     counted (vbe_pm_rej), and changes nothing -- the INT 10h forms answer 03h/02h there. */
static VOID VideoVbePmInstall(PVIDEO_STATE st)
{
    BYTE *p;
    UINT i;
    if (!st || !st->Bus) return;
    p = (BYTE *)VddMapFlat(st->Bus, VDD_VBEPM_SEG, 0);
    if (!p) return;
    for (i = 0; i < VBE_PM_LEN; ++i) p[i] = vbe_pm_block[i];
    for (i = 0; i < VBE_RM_LEN; ++i) p[VDD_VBERM_OFF + i] = vbe_rm_winfunc[i];   /* #273 */
}
/* both fit their 256 bytes without overlapping (a compile error otherwise) */
typedef char VIDEO_VBE_RM_FITS[(VBE_PM_LEN <= VDD_VBERM_OFF && VDD_VBERM_OFF + VBE_RM_LEN <= 256) ? 1 : -1];

static VOID VideoVbePortOut(PVOID self, WORD port, BYTE w, UINT32 v)
{
    PVIDEO_STATE st = (PVIDEO_STATE)self;
    WORD d = (WORD)(w >= 2 ? v : (v & 0xFF));
    if (port == 0x1CE) { st->VbeIndex = d; return; }
    switch (st->VbeIndex) {
    case 0x05:
        if (VideoVesaSetBank(st, d)) st->VbePmBankCount++; else st->VbePmRejected++;
        break;
    case 0x10:
        st->VbeStartLow = d;
        break;
    case 0x11: {
        UINT32 org = (((UINT32)d << 16) | st->VbeStartLow) * 4u;
        UINT32 bypp = VideoVesaBytesPerPixel(st->VesaBpp);
        if (!st->IsVesa || !st->VesaStride || !VideoVesaOriginFits(st, org)) { st->VbePmRejected++; break; }
        VideoLatch(st, 0);                     /* boundaries already passed keep the old start */
        st->VesaStartY = (WORD)(org / st->VesaStride);
        st->VesaStartX = (WORD)((org % st->VesaStride) / bypp);
        st->VesaOrigin = st->VesaOriginVs = st->VesaOriginLive = org;
        st->VbePmStartCount++;
        st->IsDirty = 1;
        break; }
    default:
        break;                                /* not a register here */
    }
}

static VOID VideoVbePortIn(PVOID self, WORD port, BYTE w, UINT32 *v)
{
    PVIDEO_STATE st = (PVIDEO_STATE)self;
    UINT32 r = 0;
    (VOID)w;
    if (port == 0x1CE) { *v = st->VbeIndex; return; }
    switch (st->VbeIndex) {
    case 0x03: r = st->IsVesa ? st->VesaBpp : 0; break;
    case 0x05: r = st->VesaBank; break;
    case 0x06: r = (st->IsVesa && st->VesaBpp) ? st->VesaStride / VideoVesaBytesPerPixel(st->VesaBpp) : 0; break;
    case 0x10: r = (st->VesaOrigin / 4u) & 0xFFFFu; break;
    case 0x11: r = (st->VesaOrigin / 4u) >> 16; break;
    default:   r = 0; break;                  /* 00h (ID) and the rest: 0 */
    }
    *v = r;
}

/* ── DIRECT COLOUR -> ARGB, ONCE PER FRAME. (s74) ─────────────────────────────────
     The frame contract has a bpp field, but every consumer of it indexed a palette --
     so a 15/16/24bpp mode has to be converted somewhere, and this is the only place
     that knows the guest's pixel format. Channels are expanded by REPLICATING the
     high bits into the low ones (5 bits -> 8 as (v<<3)|(v>>2)), not by shifting and
     leaving zeros: a white pixel must come out 0xFF, and 0x1F<<3 is 0xF8, which is a
     visibly grey white and the classic giveaway of a lazy 5-to-8 expansion. */
/* Bounding offsets of everything non-zero in the framebuffer. Answers, without any
   assumption about stride or origin, WHERE the guest put its pixels. */
static VOID VideoVesaScanWritten(PVIDEO_STATE st)
{
    UINT32 i, lo = 0xFFFFFFFFu, hi = 0, nz = 0;
    UINT32 end = st->VesaOriginLive + st->VesaStride * st->VesaHeight;   /* the displayed page */
    if (!end || end > VIDEO_VESA_VRAM) end = VIDEO_VESA_VRAM;
    for (i = 0; i < end; ++i)
        if (st->VesaVram[i]) { if (lo == 0xFFFFFFFFu) lo = i; hi = i; ++nz; }
    st->VramLow = (lo == 0xFFFFFFFFu) ? 0 : lo;
    st->VramHigh = hi; st->VramNonZero = nz;
}

static VOID VideoVesaToArgb(PVIDEO_STATE st)
{
    UINT32 y, x, w = st->VesaWidth, h = st->VesaHeight, pitch = st->VesaStride;
    UINT32 bypp = VideoVesaBytesPerPixel(st->VesaBpp), org = st->VesaOriginLive;  /* as displayed (VideoLatch) */
    if (!w || !h || !pitch) return;
    if (w > VIDEO_VESA_MAX_WIDTH || h > VIDEO_VESA_MAX_HEIGHT) return;   /* cannot happen: VideoVesaFind caps it */
    for (y = 0; y < h; ++y) {
        const BYTE *src = st->VesaVram + org + y * pitch;  /* from the 4F07 start */
        UINT32 *dst = st->VesaArgb + y * w;
        if (org + y * pitch + w * bypp > VIDEO_VESA_VRAM) break;
        for (x = 0; x < w; ++x) {
            UINT32 r, g, b;
            if (st->VesaBpp == 15) {
                UINT32 v = (UINT32)src[x*2] | ((UINT32)src[x*2+1] << 8);
                r = (v >> 10) & 0x1F; g = (v >> 5) & 0x1F; b = v & 0x1F;
                r = (r << 3) | (r >> 2); g = (g << 3) | (g >> 2); b = (b << 3) | (b >> 2);
            } else if (st->VesaBpp == 16) {
                UINT32 v = (UINT32)src[x*2] | ((UINT32)src[x*2+1] << 8);
                r = (v >> 11) & 0x1F; g = (v >> 5) & 0x3F; b = v & 0x1F;
                r = (r << 3) | (r >> 2); g = (g << 2) | (g >> 4); b = (b << 3) | (b >> 2);
            } else {                                   /* 24bpp, B G R in memory */
                b = src[x*3]; g = src[x*3+1]; r = src[x*3+2];
            }
            dst[x] = 0xFF000000u | (r << 16) | (g << 8) | b;
        }
    }
}

/* ── SAVE / RESTORE STATE: one block for INT 10h AH=1Ch and VBE 4F04 (§4.7). (s74b)
     AH=1Ch used to report 3 blocks (192 bytes) and then write 768 bytes of DAC into the
     caller's buffer -- the Heretic MCB overrun again, in a different function -- and
     4F04 did not exist. A guest treats the buffer as opaque, so the layout is ours:
        +0   'NTVS'  +4 mask  +6 version
        +8   mode, in_vesa, vesa_mode(2), vesa_bpp, vesa_lfb, dacwidth, bank(2),
             stride(4) @+20, start_x(2) @+24, start_y(2) @+26
        +32  DAC, 256 x (R,G,B) at 8 bits each -- the 8-bit width would lose precision
             at 6 bits, and AH=1Ch's own 6-bit format is produced from this on the way out
        +800 the attribute controller (vpal[17], AR10, AR14, overscan), text cursor
             (row, col, shape), page, CRTC start/offset
     896 bytes = 14 blocks, reported for every mask; the spec permits over-reporting
     and a guest allocates from the answer, which is the one thing that must hold.
     Restore re-enters the saved mode with the DON'T-CLEAR bit -- frame buffer memory
     is explicitly not part of the state -- then puts the registers back over it. */
#define VIDEO_STATE_BYTES  896u
#define VIDEO_STATE_BLOCKS (VIDEO_STATE_BYTES / 64u)
static VOID VideoInt10(PVOID self, PNTVDD_REGISTERS r);
static VOID VideoVesa(PVIDEO_STATE st, PNTVDD_REGISTERS r);
static VOID VideoStateSave(PVIDEO_STATE st, BYTE *b, WORD mask)
{
    UINT i;
    for (i = 0; i < VIDEO_STATE_BYTES; ++i) b[i] = 0;
    b[0] = 'N'; b[1] = 'T'; b[2] = 'V'; b[3] = 'S'; VideoWrite16(b + 4, mask); VideoWrite16(b + 6, 1);
    b[8] = st->Mode; b[9] = st->IsVesa; VideoWrite16(b + 10, st->VesaMode);
    b[12] = st->VesaBpp; b[13] = st->IsVesaLfb; b[14] = st->VesaDacWidth;
    VideoWrite16(b + 16, st->VesaBank); VideoWrite32(b + 20, st->VesaStride);
    VideoWrite16(b + 24, st->VesaStartX); VideoWrite16(b + 26, st->VesaStartY);
    for (i = 0; i < 256; ++i) {
        b[32 + i*3]     = (BYTE)(st->Dac[i] >> 16);
        b[32 + i*3 + 1] = (BYTE)(st->Dac[i] >> 8);
        b[32 + i*3 + 2] = (BYTE)(st->Dac[i]);
    }
    for (i = 0; i < 17; ++i) b[800 + i] = st->PaletteRegisters[i];
    b[817] = st->AttributeMode; b[818] = st->AttributeColorSelect; b[819] = st->Overscan;
    b[820] = st->CursorRow; b[821] = st->CursorColumn; VideoWrite16(b + 822, st->CursorShape);
    b[824] = st->Page; VideoWrite16(b + 826, st->CrtcStart); b[828] = st->CrtcOffset;
}
/* 1 = restored, 0 = not a buffer we wrote (the caller answers AH=02). */
static INT VideoStateLoad(PVIDEO_STATE st, const BYTE *b)
{
    UINT i; NTVDD_REGISTERS m;
    if (b[0] != 'N' || b[1] != 'T' || b[2] != 'V' || b[3] != 'S') return 0;
    for (i = 0; i < sizeof m; ++i) ((BYTE *)&m)[i] = 0;
    if (b[9]) {                                   /* back into the VESA mode, no clear */
        WORD bx = (WORD)(0x8000u | (b[13] ? 0x4000u : 0u) | (b[10] | (b[11] << 8)));
        VddSetAh(&m, 0x4F); VddSetAl(&m, 0x02); VddSetBx(&m, bx); VideoVesa(st, &m);
        st->VesaDacWidth = b[14]; st->VesaBank = (WORD)(b[16] | (b[17] << 8));
        st->VesaStride   = (UINT32)b[20] | ((UINT32)b[21] << 8) | ((UINT32)b[22] << 16) | ((UINT32)b[23] << 24);
        st->VesaStartX  = (WORD)(b[24] | (b[25] << 8));
        st->VesaStartY  = (WORD)(b[26] | (b[27] << 8));
        st->VesaOrigin = st->VesaOriginVs = st->VesaOriginLive =   /* shown at once (#226) */
            VideoVesaOriginFits(st, VideoVesaXyOrigin(st, st->VesaStartX, st->VesaStartY))
                ? VideoVesaXyOrigin(st, st->VesaStartX, st->VesaStartY) : 0u;
    } else {                                      /* a standard mode, bit 7 = no clear */
        VddSetAh(&m, 0x00); VddSetAl(&m, (BYTE)(b[8] | 0x80)); VideoInt10(st, &m);
    }
    for (i = 0; i < 256; ++i)
        st->Dac[i] = 0xFF000000u | ((UINT32)b[32 + i*3] << 16)
                   | ((UINT32)b[32 + i*3 + 1] << 8) | (UINT32)b[32 + i*3 + 2];
    for (i = 0; i < 17; ++i) st->PaletteRegisters[i] = b[800 + i];
    st->AttributeMode = b[817]; st->AttributeColorSelect = b[818]; st->Overscan = b[819];
    st->CursorRow = b[820]; st->CursorColumn = b[821]; st->CursorShape = (WORD)(b[822] | (b[823] << 8));
    st->Page = b[824]; st->CrtcStart = (WORD)(b[826] | (b[827] << 8)); st->CrtcOffset = b[828];
    VideoPaletteRefresh(st); st->IsDirty = 1;
    return 1;
}

/* INT 10h AX=4Fxx. Always returns AX=0x004F (supported+ok) for what we handle. */
static VOID VideoVesa(PVIDEO_STATE st, PNTVDD_REGISTERS r)
{
    BYTE al = VddGetAl(r); UINT i;
    if (al < 0x16) {                              /* inventory: see vesa_calls[]   */
        BYTE bl = (BYTE)(VddGetBx(r) & 0xFF);
        st->VesaCalls[al]++;
        st->VesaBl[al] |= (WORD)(bl == 0x80 ? 0x8000u : bl < 15 ? (1u << bl) : 0u);
    }
    switch (al) {
    case 0x00: {                                  /* return controller info       */
        /* ⚠ THE CALLER'S BLOCK IS 256 BYTES UNLESS IT PRESET "VBE2". (s74) VBE 2.0
             §4.3: a VbeInfoBlock is 256 bytes for a VBE 1.x caller and 512 only when
             the caller wrote "VBE2" into the signature first; the OEM string and mode
             list go in the reserved area at +34 (that is what it is for). We used to
             put the OEM string at +0x100 and the mode list at +0x120 REGARDLESS.
             Heretic allocates exactly 16 paragraphs for this call, so +0x100 was the
             MCB of the next block: its signature became 'N' (of "NTVDMEX VESA"), the
             chain walk stopped there, ~480 KB above went invisible, and its next DOS
             allocation died with "I_AllocLow: DOS alloc of 1024 failed, 256 free".
             Doom never probes VESA, which is why only Heretic paid. */
        BYTE *b = (BYTE *)VddMapFlat(st->Bus, r->Es, (WORD)(WORD)r->Edi);
        INT vbe2 = (b[0]=='V' && b[1]=='B' && b[2]=='E' && b[3]=='2');
        const UINT OEM = 0x22, MODES = 0x40;  /* both inside the reserved area */
        for (i = 0; i < (vbe2 ? 512u : 256u); ++i) b[i] = 0;
        b[0]='V'; b[1]='E'; b[2]='S'; b[3]='A';
        VideoWrite16(b + 4, 0x0200);                      /* VBE 2.0                      */
        VideoWrite32(b + 6, ((UINT32)r->Es << 16) | (((WORD)r->Edi + OEM) & 0xFFFF));    /* OEM string */
        /* Capabilities D0 = "DAC width is switchable to 8 bits per primary" (§4.3).
           We answer 4F08 BH=8 with 8 -- and advertised 0 here, so a guest that
           follows the spec's own advice ("query capabilities before 4F08") never
           asked. Found by p_vesa against QEMU's VBE (s74b), the first oracle row. */
        VideoWrite32(b + 10, 1);                          /* capabilities: D0 DAC switchable */
        VideoWrite32(b + 14, ((UINT32)r->Es << 16) | (((WORD)r->Edi + MODES) & 0xFFFF));  /* mode list  */
        VideoWrite16(b + 18, VIDEO_VESA_VRAM / 0x10000);    /* total memory in 64KB units   */
        { const char *o = "NTVDMEX VESA"; for (i = 0; o[i]; ++i) b[OEM + i] = (BYTE)o[i]; b[OEM+i]=0; }
        if (vbe2) {                               /* VBE 2.0 fields, only for a 2.0 caller */
            /* ── THE FOUR STRINGS GO IN OemData (+100h), EACH ITS OWN (#226). §4.3: "VBE
                 2.0 BIOS implementations must place this string [OemString] in the
                 OemData area within the VbeInfoBlock if 'VBE2' is preset", and "The
                 OemVendorName string, OemProductName string and OemProductRev string
                 are copied into this area by the VBE implementation" -- so a protected-
                 mode client can turn each far pointer into an offset in ITS copy of the
                 block. All four pointed at the OEM string at +22h: inside the block,
                 but in the Reserved area §4.3 keeps for the mode list, and three of
                 them named the wrong thing. A 1.x caller (no 'VBE2', 256 bytes) keeps
                 the +22h string: it has no OemData, and +100h is not its memory. */
            static const char *const strs[4] = { "NTVDMEX VESA", "NTVDMEX", "NTVDMEX VBE", "1.00" };
            static const UINT ptro[4] = { 6, 22, 26, 30 };   /* OemString, Vendor, Product, Rev */
            UINT at = 0x100, k;
            VideoWrite16(b + 20, 0x0100);                 /* OEM software rev 1.00        */
            for (k = 0; k < 4; ++k) {
                VideoWrite32(b + ptro[k], ((UINT32)r->Es << 16) | (((WORD)r->Edi + at) & 0xFFFF));
                for (i = 0; strs[k][i]; ++i) b[at++] = (BYTE)strs[k][i];
                b[at++] = 0;
            }
        }
        for (i = 0; i < sizeof(g_VideoVesaModes)/sizeof(g_VideoVesaModes[0]); ++i)
            VideoWrite16(b + MODES + i*2, g_VideoVesaModes[i].Number);
        { UINT t;
          for (t = 0; t < sizeof(g_VideoVesaTextModes)/sizeof(g_VideoVesaTextModes[0]); ++t, ++i)
              VideoWrite16(b + MODES + i*2, g_VideoVesaTextModes[t].Number); }
        VideoWrite16(b + MODES + i*2, 0xFFFF);            /* mode-list terminator         */
        VddSetAx(r, 0x004F);
        break; }
    case 0x01: {                                  /* return mode info             */
        WORD w, h; BYTE mbpp = 8;
        { BYTE tc, tr, th;
          if (VideoVesaFindText(VddGetCx(r), &tc, &tr, &th)) {   /* a TEXT mode: answer in characters */
              BYTE *b = (BYTE *)VddMapFlat(st->Bus, r->Es, (WORD)r->Edi);
              UINT page = (UINT)tc * tr * 2u;
              VideoVesaNote(st, 0x01, VddGetCx(r), 1);
              for (i = 0; i < 256; ++i) b[i] = 0;
              VideoWrite16(b + 0, 0x000F);              /* supported|opt info|BIOS output|colour; bit 4 clear = TEXT */
              b[2] = 0x07; b[3] = 0x00;         /* WinA r/w/exists; WinB none    */
              VideoWrite16(b + 4, 32); VideoWrite16(b + 6, 32); /* the 32 KB colour-text window  */
              VideoWrite16(b + 8, 0xB800); VideoWrite16(b + 10, 0);
              VideoWrite32(b + 12, 0);
              VideoWrite16(b + 16, (WORD)(tc * 2u)); /* bytes per character row       */
              VideoWrite16(b + 18, tc); VideoWrite16(b + 20, tr); /* X/Y resolution IN CHARACTERS  */
              b[22] = 8; b[23] = th;            /* char cell                     */
              b[24] = 1;                        /* planes                        */
              b[25] = 4;                        /* bits per pixel (attribute)    */
              b[26] = 1;                        /* NumberOfBanks                 */
              b[27] = 0;                        /* MemoryModel 0 = text          */
              b[28] = 0;                        /* BankSize                      */
              b[29] = (BYTE)(page ? (0x8000u / page) - 1u : 0u);   /* image pages */
              b[30] = 1;                        /* Reserved = 1                  */
              VddSetAx(r, 0x004F);
              break;
          } }
        VideoVesaNote(st, 0x01, VddGetCx(r), VideoVesaFind(VddGetCx(r), &w, &h, &mbpp));
        if (VideoVesaFind(VddGetCx(r), &w, &h, &mbpp)) {
            BYTE *b = (BYTE *)VddMapFlat(st->Bus, r->Es, (WORD)(WORD)r->Edi);
            UINT32 bypp = VideoVesaBytesPerPixel(mbpp), pitch = (UINT32)w * bypp;
            for (i = 0; i < 256; ++i) b[i] = 0;
            /* ⛔ D5 STAYS CLEAR (s84, a user-found regression). #226 set it (0xBB, "not
                 VGA compatible", as QEMU's SeaVGABIOS does) and ZARMMX stopped seeing VESA
                 at all: its VESA 1 renderer draws through the banked window at A0000, which
                 D5 set says may not exist, so it filtered out every mode and fell back to
                 320x200 (headless A/B, runs/s84/zarvesa/: 0xBB -> VGA, 0x9B -> 640x480).
                 Our window DOES work -- 4F05 banking drove ZAR's VESA 1 in s74c -- so for
                 the window D5 clear is the true answer, and it is what the period card
                 says too (Tseng ET4000 under PCem: 1Fh/1Bh, D5 clear). */
            VideoWrite16(b + 0, 0x009B);                  /* attrs: supported|color|graphics */
            b[2] = 0x07; b[3] = 0x00;             /* WinA r/w/exists; WinB none    */
            VideoWrite16(b + 4, 64); VideoWrite16(b + 6, 64);     /* granularity / size (KB)       */
            VideoWrite16(b + 8, 0xA000); VideoWrite16(b + 10, 0); /* WinA seg / WinB seg           */
            /* #273: WinFuncPtr -> the real-mode stub beside the 4F0Ah block (vbe_rm.asm).
                 It was NULL ("use 4F05h"), legal, but a VBE 1.x program that far-calls it
                 without checking ran 0000:0000. Re-planted on every 4F01h, like 4F0Ah's. */
            VideoVbePmInstall(st);
            VideoWrite32(b + 12, ((UINT32)VDD_VBEPM_SEG << 16) | VDD_VBERM_OFF);
            VideoWrite16(b + 16, (WORD)pitch);        /* bytes per scan line           */
            VideoWrite16(b + 18, w); VideoWrite16(b + 20, h);     /* X / Y resolution              */
            /* char cell: the BIOS font the mode's line count implies -- 8x8 at 200
               lines, 8x14 at 350, 8x16 otherwise. The ET4000/W32p ROM says YCharSize=8
               for 320x200 (p_vesa vs pcem-vesa, s74b); we said 16 for everything. */
            b[22] = 8; b[23] = (BYTE)(h <= 200 ? 8 : h <= 350 ? 14 : 16);
            b[24] = 1; b[25] = mbpp;              /* planes / bits per pixel       */
            /* ⚠ MEMORY MODEL IS NOT A CONSTANT. It was 4 ("packed pixel", i.e. a
                 palette index) for every mode, which is a lie for direct colour --
                 a guest reads this byte to decide whether the bytes it writes are
                 indices or channels. VBE 2.0 §4.4: 04h packed pixel, 06h direct colour. */
            b[27] = (BYTE)(mbpp > 8 ? 6 : 4);
            /* ⚠⚠ NumberOfBanks IS **NOT** "how many 64KB windows the mode needs".
                 I wrote that from memory and it is the wrong CONCEPT, not just a wrong
                 number -- it computed 22 for 640x480x24. VBE 2.0 §4.4: banks are the
                 groups the SCAN LINES are divided into (the CGA/Hercules interleave),
                 and "for modes that don't have scanline banks (such as VGA modes
                 0Dh-13h), this field should be set to 1". BankSize likewise 0.
                 Caught by reading the spec, not by any guest -- heaven7 never looks. */
            b[26] = 1;                            /* +26 NumberOfBanks: no scanline banks */
            /* ⚠⚠⚠ AND THE OFFSETS FROM HERE WERE OFF BY ONE, ALSO FROM MEMORY. The
                 VBE 2.0 ModeInfoBlock runs +26 NumberOfBanks, +27 MemoryModel,
                 +28 BankSize, +29 NumberOfImagePages, +30 Reserved(=1). I had written
                 NumberOfImagePages at +28 and "reserved must be 1" at +29 -- so the
                 image-page count was landing in BankSize, a 1 was landing in
                 NumberOfImagePages (which is a count MINUS ONE, i.e. claiming two
                 pages of a mode we have one page of VRAM for), and Reserved at +30 was
                 left 0 when the spec says it is always 1 in this version.
                 Three fields wrong, none of which any guest we have would have caught. */
            b[28] = 0;                            /* +28 BankSize: no scanline banks   */
            /* +29 NumberOfImagePages = pages VRAM holds MINUS ONE. The s74 audit put
               this field at the right offset and left a 0 in it, which told every
               page-flipping guest there was a single page. QEMU's VBE says 50 for
               640x480x8 in 16 MB; with 4 MB we say 12. Found by p_vesa (s74b). */
            { UINT32 pg = (UINT32)pitch * h;
              UINT32 np = pg ? VIDEO_VESA_VRAM / pg : 1u;
              b[29] = (BYTE)(np ? (np > 256u ? 255u : np - 1u) : 0u); }
            b[30] = 1;                            /* +30 Reserved: always 1 in VBE 2.0 */
            /* ── DIRECT-COLOUR FIELD LAYOUT (offsets 31..38). A guest cannot pack a
                 pixel without these, and it will not trust a mode that leaves them
                 zero. 15bpp is 5:5:5 with one byte unused, 16bpp is 5:6:5, 24bpp is
                 8:8:8 -- all little-endian, blue in the low bits, which is what every
                 PC VBE implementation does. */
            if (mbpp == 15)      { b[31]=5; b[32]=10; b[33]=5; b[34]=5; b[35]=5; b[36]=0;
                                   b[37]=1; b[38]=15; }
            else if (mbpp == 16) { b[31]=5; b[32]=11; b[33]=6; b[34]=5; b[35]=5; b[36]=0;
                                   b[37]=0; b[38]=0; }
            else if (mbpp == 24) { b[31]=8; b[32]=16; b[33]=8; b[34]=8;  b[35]=8; b[36]=0;
                                   b[37]=0; b[38]=0; }
            /* DirectColorModeInfo: D0 colour ramp programmable (no), D1 "bits in the
               Rsvd field are usable by the application" -- yes for 5:5:5, whose spare
               bit nothing reads (the ET4000/W32p ROM says 02 there; p_vesa, s74b). */
            b[39] = (BYTE)(mbpp == 15 ? 0x02 : 0x00);
            /* ── ★★★ LINEAR FRAMEBUFFER. (s74) Attribute bit 7 says a mode HAS one and
                 PhysBasePtr says where; with them 0 the whole mode list reads as
                 "banked only". heaven7 enumerated all twelve modes we published,
                 including 320x200x16 and 640x480x16, and refused every one without
                 ever calling 4F02 -- measured twice, before and after the list grew.
                 A 2000-era demo will not paginate a 64KB window to raytrace. */
            /* ⚠ 0x9B ALREADY CARRIES D7 (LFB available) -- D0|D1|D3|D4|D7. An earlier
                 note here claimed bit 7 was 0 and OR'd 0x80 in; that was a no-op and
                 the claim was wrong. What was actually missing was PhysBasePtr, which
                 D7 is worthless without. VBE 2.0 §4.4 D7/D6 table: D7=1,D6=0 means
                 "both windowed and linear", which is exactly what we now provide. */
            VideoWrite32(b + 40, VIDEO_VESA_LFB_PHYSICAL);            /* PhysBasePtr              */
            /* VBE 2.0 adds the linear-mode geometry at +50; a guest that drives the
               LFB reads these rather than the banked ones. Same numbers here because
               our pitch does not change between the two. */
            VideoWrite16(b + 50, (WORD)pitch);              /* LinBytesPerScanLine      */
            b[52] = b[29]; b[53] = b[29];               /* Lin/Bnk NumberOfImagePages = +29 */
            if (mbpp > 8) { b[54]=b[31]; b[55]=b[32]; b[56]=b[33]; b[57]=b[34];
                            b[58]=b[35]; b[59]=b[36]; b[60]=b[37]; b[61]=b[38]; }
            VddSetAx(r, 0x004F);
        } else VddSetAx(r, 0x014F);
        break; }
    case 0x02: {                                  /* set VBE mode                 */
        WORD w, h; BYTE mbpp = 8;
        { BYTE tc, tr, th;
          if (VideoVesaFindText(VddGetBx(r), &tc, &tr, &th)) {
              /* A VESA text mode = mode 3 with a different geometry. Go through the
                 standard mode set so everything a text mode resets is reset (font,
                 palette, CRTC, cursor, blink), honouring D15 as AL bit 7, then apply
                 the geometry. vesa_text_mode is what 4F03 reports until a standard
                 mode set clears it (that path zeroes it below). */
              NTVDD_REGISTERS m; UINT k;
              for (k = 0; k < sizeof m; ++k) ((BYTE *)&m)[k] = 0;
              /* §4.5: "If D14 is set, and a linear frame buffer model is not available
                 then the call will fail." A text mode has none (its ModeInfoBlock says
                 D7 = 0), so fail before anything changes (#226; we used to accept it). */
              if (VddGetBx(r) & 0x4000) {
                  VideoVesaNote(st, 0x02, VddGetBx(r), 0);
                  st->VesaSetBx = VddGetBx(r); st->IsVesaSetSeen = 1; st->IsVesaSetOk = 0;
                  VddSetAx(r, 0x014F);
                  break;
              }
              VideoVesaNote(st, 0x02, VddGetBx(r), 1);
              st->VesaSetBx = VddGetBx(r); st->IsVesaSetSeen = 1; st->IsVesaSetOk = 1;
              VddSetAh(&m, 0x00); VddSetAl(&m, (BYTE)(0x03 | ((VddGetBx(r) & 0x8000) ? 0x80 : 0x00)));
              VideoInt10(st, &m);
              st->Columns = tc; st->Rows = tr; st->CellHeight = th;
              st->GraphicsWidth = (WORD)(tc * VIDEO_CELL_WIDTH); st->GraphicsHeight = (WORD)(tr * th);
              if (!(VddGetBx(r) & 0x8000)) VideoClearText(st, 0x07);
              st->VesaTextMode = (WORD)(VddGetBx(r) & 0x3FFF);
              st->VesaModeFlags = (WORD)(VddGetBx(r) & 0x8000);   /* 4F03 D15 (#226) */
              st->IsDirty = 1; VddSetAx(r, 0x004F);
              break;
          } }
        VideoVesaNote(st, 0x02, VddGetBx(r), VideoVesaFind(VddGetBx(r), &w, &h, &mbpp));
        st->VesaSetBx = VddGetBx(r); st->IsVesaSetSeen = 1;
        st->IsVesaSetOk = (BYTE)(VideoVesaFind(VddGetBx(r), &w, &h, &mbpp) ? 1 : 0);
        if (VideoVesaFind(VddGetBx(r), &w, &h, &mbpp)) {
            UINT32 n;
            st->IsVesa = 1; st->VesaMode = VddGetBx(r) & 0x3FFF; st->VesaWidth = w; st->VesaHeight = h;
            /* Bit 14 of the mode = "use the linear framebuffer". It matters beyond
               bookkeeping: an LFB guest never touches A0000, so VideoVesaSync must stop
               copying that window over the picture (see VideoVesaSync). */
            st->IsVesaLfb = (BYTE)((VddGetBx(r) & 0x4000) ? 1 : 0);
            st->VesaBpp = mbpp;
            st->VesaStride = (UINT32)w * VideoVesaBytesPerPixel(mbpp);
            st->VesaStartX = st->VesaStartY = 0;   /* a mode set shows page 1 */
            st->VesaOrigin = st->VesaOriginVs = st->VesaOriginLive = 0;   /* ...on every stage */
            st->Vesa07Vbl = 0; st->Int10WaitUntil = 0;
            st->VesaDacWidth = 6;                     /* §4.11: any mode set -> 6 bits */
            st->VesaBank = 0;
            /* ── ★★★ D15 = "DON'T CLEAR DISPLAY MEMORY". (VBE 2.0 §4.5) ──────────────
                 We cleared unconditionally. This is the SAME defect as the standard
                 BIOS one already on the books -- "AL bit 7 on a mode set = DO NOT
                 CLEAR VRAM" -- just on the VESA function instead, and it was found the
                 same way it should have been the first time: by reading the spec.
                 A guest that sets a mode to change geometry while keeping its picture
                 gets a black screen from us otherwise. */
            if (!(VddGetBx(r) & 0x8000)) {
                for (n = 0; n < VIDEO_VESA_VRAM; ++n) st->VesaVram[n] = 0;
                for (n = 0; n < VIDEO_VESA_WINDOW; ++n) st->VideoMemory[n] = 0;
            }
            /* ── 4F03h REPORTS D14/D15 AS THIS CALL SET THEM, AND 40:87h BIT 7 RECORDS
                 D15 (#226). §4.6: BX D14 = linear, D15 = "memory not cleared at last mode
                 set", and the Version 2.x note: "Unlike version 1.x VBE implementations,
                 the memory clear flag will be returned". §4.5: "VBE BIOS 2.0
                 implementations should also update the BIOS Data Area 40:87 memory clear
                 bit so that VBE Function 03h can return this flag." We stored the mode
                 `& 3FFFh`, so a guest that saves 4F03 and re-sets it with 4F02 came back
                 BANKED -- and VideoVesaSync then painted the stale A0000 window over the LFB
                 it was still drawing into, every frame. */
            st->VesaModeFlags = (WORD)(VddGetBx(r) & 0xC000);
            st->IsModeSetNoClear = (BYTE)((VddGetBx(r) & 0x8000) ? 1 : 0);
            /* ── ★★ THE VGA LAYER UNDER A VESA MODE (#226). ─────────────────────────────
                 4F02h used to set the VESA fields and nothing else: `mkind`, the
                 sequencer/GC shadows, gw/gh, the text geometry and 40:49h all kept
                 the PREVIOUS standard mode's values. The hazard that left, read from the
                 code (not yet seen on a guest): after mode 12h, mkind stayed
                 VIDEO_KIND_PLANAR and chain4 0, so
                   * VddVideoIsPlanarActive() said 1 and the host (video_trap_sync)
                     kept running the guest in its INTERPRETER, whose A0000 stores go
                     through VddVideoPlanarWrite() into the four planes -- not into the
                     A0000 window VideoVesaSync copies into vesa_vram. A banked VESA guest
                     would have drawn into planes nothing displays, at interpreter speed;
                   * the host's A0000 mapping stayed on a plane section (ymap_select was
                     last told the planar map mask), so even native stores missed vmem;
                   * write mode / read mode / bit mask stayed mode 12h's.
                 And ModeAttributes told the guest the mode was VGA-compatible.
               ► WHAT A VBE BIOS DOES, so what we do: it programs the VGA for a CHAINED
                 256-colour pixel pipe and extends it -- measured in QEMU's SeaVGABIOS
                 binary (vgabios-stdvga.bin, qemu 10.2.1: SR4 |= 08h chain-4, GR5 = 40h
                 256-colour shift, AR index 20h video on) -- then fills the BDA from the
                 mode. So: mode 13h's register file (VideoLoadModeDefinition), kind LINEAR8 with
                 chain-4 on and the host window back on the linear section (the same arm
                 as the INT 10h mode-13h set), and the mode's geometry in gw/gh.
               ► THE BDA, FROM THE SAME BINARY'S vga_set_mode(): 40:49h = the mode if it
                 fits a byte, else FFh -- a VBE mode number does not, so FFh (Bochs's
                 older VGABIOS leaves 40:49h alone; SeaVGABIOS is the one we can read,
                 and "the last standard mode" is the lie that let a TSR believe mode 12h
                 was still set); 40:4Ah = XRes / 8; 40:84h = YRes / char height - 1;
                 40:85h = the char height (the YCharSize 4F01h reports); cursors and page
                 zeroed; 40:87h = 60h | (D15 ? 80h : 0). VddVideoBdaSync derives all
                 of those from the fields set here. */
            /* ── #325: AND THE 256-COLOUR DEFAULT PALETTE, as a mode 13h set loads it.
                 A 4F02 left the DAC as the previous mode had it, so after a 16-colour
                 mode (0Dh) colour 15 drew grey -- seen on the rig as a grey VESA
                 checkerboard. AH=12h BL=31h (palette loading off) is still honoured
                 inside VideoLoadDefaultPalette. */
            st->Mode   = 0x13;
            VideoLoadDefaultPalette(st);
            st->Mode   = 0xFF;
            VideoLoadModeDefinition(st, 0x13);               /* the chained 256-colour register file */
            st->ModeKind  = VIDEO_KIND_LINEAR8;
            st->MapMask = 0x0F; st->YMask = 0x0F;
            if (!st->IsChain4) { st->IsChain4 = 1; st->Chain4Selects++;
                               if (st->YMapSelect) st->YMapSelect(st->YMapContext, -1); }
            st->GraphicsWidth = w; st->GraphicsHeight = h;
            st->CellHeight = (BYTE)(h <= 200 ? 8 : h <= 350 ? 14 : 16);
            st->Columns   = (BYTE)(w / 8u);
            st->Rows   = (BYTE)(h / st->CellHeight);
            st->CursorRow = st->CursorColumn = 0; st->Page = 0;
            st->VesaTextMode = 0;
            VideoPaletteRefresh(st);                           /* identity DAC path, see VideoPaletteRefresh */
            st->IsDirty = 1; VddSetAx(r, 0x004F);
        } else VddSetAx(r, 0x014F);
        break; }
    case 0x04: {                                  /* save/restore state (§4.7)     */
        BYTE dl = (BYTE)(VddGetDx(r) & 0xFF);
        if (dl == 0x00) { VddSetBx(r, VIDEO_STATE_BLOCKS); VddSetAx(r, 0x004F); break; }
        if (dl == 0x01 || dl == 0x02) {
            BYTE *sb = (BYTE *)VddMapFlat(st->Bus, r->Es, (WORD)r->Ebx);
            if (dl == 0x01) { VideoStateSave(st, sb, VddGetCx(r)); VddSetAx(r, 0x004F); }
            else VddSetAx(r, VideoStateLoad(st, sb) ? 0x004F : 0x024F);
            break;
        }
        VddSetAx(r, 0x014F);
        break; }
    case 0x03:                                    /* get the current VBE mode      */
        /* D0-D13 the mode, D14 linear, D15 not cleared -- as the last mode set left
           them (#226, §4.6; see 4F02). A standard mode reports its number and D15 from
           AL bit 7, which is what SeaVGABIOS's 4F03h does (it returns the word its
           vga_set_mode() stored for EVERY mode set, flags included); §4.6 itself only
           promises an accurate answer after a 4F02h. */
        if (st->IsVesa)             VddSetBx(r, (WORD)(st->VesaMode | st->VesaModeFlags));
        else if (st->VesaTextMode) VddSetBx(r, (WORD)(st->VesaTextMode | (st->VesaModeFlags & 0x8000u)));
        else                         VddSetBx(r, (WORD)(st->Mode | (st->IsModeSetNoClear ? 0x8000u : 0u)));
        VddSetAx(r, 0x004F);
        break;
    /* ── 4F06 / 4F07: THE LOGICAL SCREEN, AND WHICH PART OF IT IS SHOWN. (s74b) ──
         VBE 2.0 §4.9/§4.10. Both of these used to be accepted and IGNORED: 4F06 kept a
         private `vesa_scanline` the presenter never read (and assumed bytes == pixels,
         true only at 8bpp), and 4F07 stored a start the presenter never applied. So a
         guest that draws page 2 and calls 4F07 to show it -- the standard VESA
         double-buffer -- got 004F back and saw page 1 forever. `VesaStride` is the
         single truth for bytes-per-line and `VideoVesaOrigin()` for the displayed start;
         the presenter reads both.
       ► Failure codes are the spec's: AH=02 for a length or start that does not fit,
         AH=03 outside a VESA mode ("invalid in current video mode"). "Fail and make no
         changes" -- §4.10 -- so nothing is written until the request has been checked. */
    case 0x06: {                                  /* get/set logical scan length   */
        BYTE  bl   = (BYTE)(VddGetBx(r) & 0xFF);
        UINT32 bypp = VideoVesaBytesPerPixel(st->VesaBpp);
        UINT32 minb = (UINT32)st->VesaWidth * bypp;                 /* the mode's own pitch */
        UINT32 maxb = st->VesaHeight ? VIDEO_VESA_VRAM / st->VesaHeight : 0; /* longest line that still holds h rows */
        maxb -= maxb % bypp;                                         /* whole pixels          */
        if (!st->IsVesa || !minb || !maxb) { VddSetAx(r, 0x034F); break; }
        if (bl == 0x00 || bl == 0x02) {
            UINT32 want = VddGetCx(r);
            if (bl == 0x00) want *= bypp;                            /* pixels -> bytes       */
            want = (want + bypp - 1) / bypp * bypp;                  /* "next larger value"   */
            if (want < minb || want > maxb) { VddSetAx(r, 0x024F); break; }
            st->VesaStride = want;
            /* a start that no longer leaves a full page at the new pitch is reset,
               which is what a BIOS that re-latches its CRTC offset does in effect.
               The (x, y) start is kept and re-derived at the new pitch, at once (the
               behaviour this call has always had; #226 keeps it). */
            if (!VideoVesaOriginFits(st, VideoVesaXyOrigin(st, st->VesaStartX, st->VesaStartY)))
                st->VesaStartX = st->VesaStartY = 0;
            st->VesaOrigin = st->VesaOriginVs = st->VesaOriginLive =
                VideoVesaXyOrigin(st, st->VesaStartX, st->VesaStartY);
            st->IsDirty = 1;
        } else if (bl == 0x03) {                  /* get maximum                   */
            VddSetBx(r, (WORD)maxb); VddSetCx(r, (WORD)(maxb / bypp));
            VddSetDx(r, (WORD)(VIDEO_VESA_VRAM / maxb)); VddSetAx(r, 0x004F);
            break;
        } else if (bl != 0x01) { VddSetAx(r, 0x014F); break; }
        VddSetBx(r, (WORD)st->VesaStride);
        VddSetCx(r, (WORD)(st->VesaStride / bypp));
        VddSetDx(r, (WORD)(VIDEO_VESA_VRAM / st->VesaStride));
        VddSetAx(r, 0x004F);
        break; }
    case 0x07: {                                  /* get/set display start         */
        BYTE bl = (BYTE)(VddGetBx(r) & 0xFF);
        if (!st->IsVesa) { VddSetAx(r, 0x034F); break; }
        /* ── #226: THE START ON THE RETRACE'S SCHEDULE, AND 80h WAITS FOR IT. ────────
             BL=00h  set now: register, retrace load and display all take it at once --
                     the behaviour this call has always had, kept deliberately (a guest
                     that pans with 00h and draws straight after sees what it expects);
             BL=80h  set "during vertical retrace": the register takes it, the retrace
                     loads it (VideoLatch) and the call does not complete before that
                     retrace -- int10_wait_until, which the HOST honours outside its
                     lock (VddVideoInt10WaitUs). heaven7's 15,900 (0,0) calls a run
                     are this idiom and were paced by nothing;
             BL=02h  (3.0) schedule a start given as a BYTE address; return at once;
             BL=82h  (3.0) the same, and wait as 80h does;
             BL=04h  (3.0) has the scheduled flip happened? CX = 0 not yet, 1 done --
                     "done" meaning the retrace has loaded the register (vs == reg);
             BL=03h/83h/05h/06h  stereo: no such hardware (ModeAttributes D11/D12 = 0,
                     Capabilities D3 = 0), so 014Fh -- which 3.0's implementation note
                     prescribes for a card without it. */
        if (bl == 0x01) {                         /* get                           */
            VddSetCx(r, st->VesaStartX); VddSetDx(r, st->VesaStartY); VddSetBx(r, 0);
            VddSetAx(r, 0x004F);
        } else if (bl == 0x04) {                  /* 3.0: scheduled flip status    */
            VideoLatch(st, 0);                     /* bring the schedule up to now  */
            VddSetCx(r, (WORD)(st->VesaOriginVs == st->VesaOrigin ? 1 : 0));
            VddSetAx(r, 0x004F);
        } else if (bl == 0x00 || bl == 0x80 || bl == 0x02 || bl == 0x82) {
            UINT32 bypp = VideoVesaBytesPerPixel(st->VesaBpp), org, x, y;
            if (bl == 0x02 || bl == 0x82) {       /* ECX = byte address (3.0)      */
                org = r->Ecx;
                y = st->VesaStride ? org / st->VesaStride : 0;
                x = st->VesaStride ? (org % st->VesaStride) / bypp : 0;
            } else {
                x = VddGetCx(r); y = VddGetDx(r);
                org = VideoVesaXyOrigin(st, x, y);
            }
            if (x > st->Vesa07MaxX) st->Vesa07MaxX = (WORD)(x > 0xFFFFu ? 0xFFFFu : x);   /* inventory */
            if (y > st->Vesa07MaxY) st->Vesa07MaxY = (WORD)(y > 0xFFFFu ? 0xFFFFu : y);
            /* the whole displayed page must exist: "if the requested Display Start
               coordinates do not allow for a full page of video memory ... fail" */
            if (!VideoVesaOriginFits(st, org)) { st->Vesa07Rejected++; VddSetAx(r, 0x024F); break; }
            VideoLatch(st, 0);                     /* boundaries already passed keep the old start */
            st->VesaStartX = (WORD)x; st->VesaStartY = (WORD)y;
            st->VesaOrigin = org;
            if (bl == 0x00) {
                st->VesaOriginVs = st->VesaOriginLive = org;          /* at once, as always */
            } else if (bl & 0x80) {
                INT in_vbl = 0;
                UINT64 until = VideoVesaVblRelease(st, &in_vbl);
                if (in_vbl) st->VesaOriginVs = org;  /* this retrace loads it: next picture */
                st->Int10WaitUntil = until;
                if (until && st->TimeUs) {
                    UINT64 now = st->TimeUs();
                    st->Vesa07Waits++;
                    if (until > now) st->Vesa07WaitUs += until - now;
                }
            }                                     /* 02h: the latch takes it at the retrace */
            st->IsDirty = 1;
            VddSetAx(r, 0x004F);
        } else VddSetAx(r, 0x014F);                   /* 03h/83h/05h/06h stereo, unknown BL */
        break; }
    case 0x08: {                                  /* get/set DAC palette width     */
        /* VBE 2.0 §4.11: BL=00 set (BH = wanted bits), BL=01 get; BH out = current.
           "If the hardware cannot select the requested width, the NEXT LOWER value it
           can is selected" -- we have 6 and 8, so 10 -> 8 and 7 -> 6 (this used to send
           anything but exactly 8 to 6). AH=03 in a direct-colour mode; the width is
           reset to 6 by any mode set (done in 4F02 and the standard AH=00).
         ► THE WIDTH NOW REACHES THE PORTS (#226) -- 3C9h and the INT 10h AH=10h DAC
           calls convert through VideoDacTo8/VideoDacFrom8 -- so it is a real switch, not a
           4F09-only flag.
         ► AND IT IS NOT CONFINED TO VESA MODES. §4.11 refuses the call in "a direct
           color or YUV mode" and nowhere else; the DAC is one device, and mode 13h (or
           a text mode, whose colours are DAC entries through the attribute controller)
           drives the same RAMDAC. We answered 034Fh outside a VESA mode, which a
           mode-13h game that wants 8-bit primaries reads as "not in this mode". */
        BYTE bl8 = (BYTE)(VddGetBx(r) & 0xFF);
        if (st->IsVesa && st->VesaBpp > 8) { VddSetAx(r, 0x034F); break; }
        if (bl8 == 0x00) {
            BYTE w8 = (BYTE)((VddGetBx(r) >> 8) & 0xFF);
            st->VesaDacWidth = (BYTE)(w8 >= 8 ? 8 : 6);
        } else if (bl8 != 0x01) { VddSetAx(r, 0x014F); break; }
        if (!st->VesaDacWidth) st->VesaDacWidth = 6;
        VddSetBx(r, (WORD)((VddGetBx(r) & 0x00FF) | ((WORD)st->VesaDacWidth << 8)));
        VddSetAx(r, 0x004F);
        break; }
    case 0x09: {                                  /* get/set palette data          */
        /* VBE 2.0 §4.12: BL=00 set, 01 get, 02/03 secondary palette (none here ->
           AH=02), 80h set during retrace; DX=first, CX=count, ES:DI = quads laid out
           B,G,R,align in memory. The DAC width set by 4F08 decides how far to shift.
           ⚠ (s74b) A set never called VideoPaletteRefresh(), so the presenter kept showing the
             OLD palette until some other DAC path happened to refresh it. */
        BYTE bl9 = (BYTE)(VddGetBx(r) & 0xFF);
        WORD first = VddGetDx(r), n = VddGetCx(r), i;
        BYTE *t = (BYTE *)VddMapFlat(st->Bus, r->Es, (WORD)r->Edi);
        if (bl9 == 0x02 || bl9 == 0x03) { VddSetAx(r, 0x024F); break; }
        if (bl9 != 0x00 && bl9 != 0x01 && bl9 != 0x80) { VddSetAx(r, 0x014F); break; }
        if ((UINT32)first + n > 256) { VddSetAx(r, 0x024F); break; }   /* fail, change nothing */
        /* The same conversion the ports use (VideoDacTo8/VideoDacFrom8). ⚠ (#226) The 6-bit set
           used to shift WITHOUT masking, so bits 6-7 of green and blue spilled into the
           low bits of red and green (`t << 2` of a byte, OR'd into its neighbour's
           field). §4.12: "When in 6 bit mode, the format of the 6 bits is LSB" -- the
           top two bits are not part of the value, as on the port. */
        for (i = 0; i < n && (first + i) < 256; ++i) {
            if (bl9 == 0x00 || bl9 == 0x80)
                st->Dac[first + i] = VideoDacPackWidth(st, t[i*4+2], t[i*4+1], t[i*4+0]);
            else {
                UINT32 v = st->Dac[first + i];
                t[i*4+0] = VideoDacFrom8(st, (BYTE)v);
                t[i*4+1] = VideoDacFrom8(st, (BYTE)(v >> 8));
                t[i*4+2] = VideoDacFrom8(st, (BYTE)(v >> 16));
                t[i*4+3] = 0;
            }
        }
        if (bl9 != 0x01) VideoPaletteRefresh(st);         /* the presenter reads st->Palette */
        st->IsDirty = 1;
        VddSetAx(r, 0x004F);
        break; }
    case 0x0A:                                    /* protected-mode interface      */
        /* ── #53: BL=00h -> ES:DI = the block, CX = its length (VBE 2.0 §4.13). See
             VideoVbePmInstall. It is rewritten on every call, so the copy a client takes
             is always the real one. Any other BL: 014Fh (the function exists; that
             subfunction does not).
           ⚠ (s74b) The answer was AX=0100h, which BOTH measured real BIOSes give (the
             Tseng ET4000/W32p ROM and Bochs's -- p_vesapm), and which was right while
             there was nothing to hand out. Neither is an oracle for the block now:
             the spec is (VBE 2.0 §4.13), and video_test.c runs the code it returns. */
        if ((VddGetBx(r) & 0xFF) != 0x00) { VddSetAx(r, 0x014F); break; }
        VideoVbePmInstall(st);
        r->Es = VDD_VBEPM_SEG;
        r->Edi = (r->Edi & 0xFFFF0000u);
        VddSetCx(r, (WORD)VBE_PM_LEN);
        VddSetAx(r, 0x004F);
        break;
    case 0x10: {                                  /* VBE/PM: display power (DPMS)  */
        /* VBE/PM 1.0. BL=00 report: BL=version 10h (BCD), BH=states supported
           (bit0 standby, bit1 suspend, bit2 off, bit3 reduced-on); BL=01 set state
           BH; BL=02 get state -> BH. ⚠ From the published interface (what Bochs and
           DOSBox answer), not from a VESA PDF in docs/ref -- there is no oracle for
           it. We claim all four states and remember the one set; nothing blanks,
           which is what a monitor with no power management would show too. */
        BYTE bl = (BYTE)(VddGetBx(r) & 0xFF), bh = (BYTE)((VddGetBx(r) >> 8) & 0xFF);
        if (bl == 0x00)      VddSetBx(r, (WORD)((0x0Fu << 8) | 0x10));
        else if (bl == 0x01) { if (bh & ~0x0Fu) { VddSetAx(r, 0x024F); break; } st->VesaPmState = bh; }
        else if (bl == 0x02) VddSetBx(r, (WORD)(((WORD)st->VesaPmState << 8) | bl));
        else { VddSetAx(r, 0x014F); break; }
        VddSetAx(r, 0x004F);
        break; }
    case 0x15: {                                  /* VBE/DDC: display identification */
        /* BL=00 report capabilities: BH = seconds per EDID block, BL = bit0 DDC1,
           bit1 DDC2, bit2 "screen blanked during transfer". BL=01 read EDID block
           DX into ES:DI (128 bytes). ⚠ From the published VBE/DDC interface (what
           Bochs/DOSBox answer), not a PDF in docs/ref; no oracle. The block is a
           SYNTHESISED EDID 1.3 for a generic analogue monitor that does every mode
           we publish -- "no monitor" was the previous answer, and a guest that asks
           deserves a plausible one rather than a refusal. One block, no extensions. */
        BYTE bl = (BYTE)(VddGetBx(r) & 0xFF);
        if (bl == 0x00) { VddSetBx(r, 0x0103); VddSetAx(r, 0x004F); break; }   /* 1 s, DDC1+DDC2 */
        if (bl == 0x01) {
            BYTE *e; UINT k, sum = 0;
            if (VddGetDx(r) != 0) { VddSetAx(r, 0x014F); break; }              /* block 0 only   */
            e = (BYTE *)VddMapFlat(st->Bus, r->Es, (WORD)r->Edi);
            for (k = 0; k < 128; ++k) e[k] = 0;
            e[0] = 0x00; for (k = 1; k < 7; ++k) e[k] = 0xFF; e[7] = 0x00;   /* header   */
            e[8] = 0x3A; e[9] = 0x96;                 /* manufacturer "NTV" (5-bit packed) */
            e[10] = 0x01; e[11] = 0x00;               /* product code 1                */
            e[16] = 1; e[17] = 10;                    /* week 1, 2000                  */
            e[18] = 1; e[19] = 3;                     /* EDID 1.3                      */
            e[20] = 0x0E;                             /* analogue, 0.7/0.3 V, sync on H/V + composite */
            e[21] = 34; e[22] = 27;                   /* 34 x 27 cm (17")               */
            e[23] = 120;                              /* gamma 2.2                     */
            e[24] = 0xEE;                             /* DPMS standby/suspend/off, RGB, preferred timing */
            /* chromaticity: sRGB primaries */
            e[25] = 0xEE; e[26] = 0x91; e[27] = 0xA3; e[28] = 0x54; e[29] = 0x4C;
            e[30] = 0x99; e[31] = 0x26; e[32] = 0x0F; e[33] = 0x50; e[34] = 0x54;
            e[35] = 0x2D; e[36] = 0xEF; e[37] = 0x00; /* established: 720x400@70 640x480@60/72/75 800x600@56/60/72/75 1024x768@60/70/75 */
            e[38] = 0x81; e[39] = 0x80;               /* standard timing 1280x1024@60  */
            e[40] = 0x81; e[41] = 0x40;               /* 1280x960@60                   */
            for (k = 42; k < 54; ++k) e[k] = 0x01;    /* unused standard timings       */
            /* detailed timing 1: 1280x1024@60 (108 MHz) */
            e[54] = 0x30; e[55] = 0x2A; e[56] = 0x00; e[57] = 0x98; e[58] = 0x51; e[59] = 0x00;
            e[60] = 0x2A; e[61] = 0x40; e[62] = 0x30; e[63] = 0x70; e[64] = 0x13; e[65] = 0x00;
            e[66] = 0x54; e[67] = 0x0E; e[68] = 0x11; e[69] = 0x00; e[70] = 0x00; e[71] = 0x1E;
            /* descriptor 2: range limits 50-75 Hz, 30-80 kHz, 110 MHz */
            e[72] = 0; e[73] = 0; e[74] = 0; e[75] = 0xFD; e[76] = 0;
            e[77] = 50; e[78] = 75; e[79] = 30; e[80] = 80; e[81] = 11; e[82] = 0x00; e[83] = 0x0A;
            for (k = 84; k < 90; ++k) e[k] = 0x20;
            /* descriptor 3: monitor name */
            e[90] = 0; e[91] = 0; e[92] = 0; e[93] = 0xFC; e[94] = 0;
            { const char *nm = "NTVDMEX VESA\n"; for (k = 0; k < 13; ++k) e[95 + k] = (BYTE)(nm[k] ? nm[k] : ' '); }
            /* descriptor 4: serial */
            e[108] = 0; e[109] = 0; e[110] = 0; e[111] = 0xFF; e[112] = 0;
            { const char *sn = "0000001\n"; for (k = 0; k < 13; ++k) e[113 + k] = (BYTE)(sn[k] ? sn[k] : ' '); }
            e[126] = 0;                               /* no extension blocks           */
            for (k = 0; k < 127; ++k) sum += e[k];
            e[127] = (BYTE)(0x100u - (sum & 0xFFu));
            VddSetAx(r, 0x004F); break;
        }
        VddSetAx(r, 0x014F);
        break; }
    case 0x05: {                                  /* window control (bank switch) */
        /* VBE 2.0 §4.8: BH = 00h set / 01h get, BL = WINDOW (00h A, 01h B), DX = window
           position in granularity units (64 KB here, as 4F01 says).
           ⚠ (s74b) This read BL as the set/get selector. Window A's number is 0, so a
             SET of window A (BH=00,BL=00) happened to work -- and a GET of window A
             (BH=01,BL=00) was treated as a set to whatever DX held. A guest that reads
             the bank back to restore it later flipped its own window to junk. Caught
             by the spec audit, not by a guest; there is no oracle for VESA yet.
           Window B is advertised as absent (WinBAttributes=0) so BL=01 fails; in an LFB
           mode the spec requires AH=03. */
        BYTE bh = (BYTE)((VddGetBx(r) >> 8) & 0xFF), bl = (BYTE)(VddGetBx(r) & 0xFF);
        if (!st->IsVesa || st->IsVesaLfb) { VddSetAx(r, 0x034F); break; }
        if (bl != 0x00) { VddSetAx(r, 0x014F); break; }
        if (bh == 0x00) {                         /* set window A                  */
            if (!VideoVesaSetBank(st, VddGetDx(r))) { VddSetAx(r, 0x024F); break; }   /* past VRAM */
        } else if (bh == 0x01) {                  /* get window A                  */
            VddSetDx(r, st->VesaBank);
        } else { VddSetAx(r, 0x014F); break; }
        VddSetAx(r, 0x004F);
        break; }
    default: VddSetAx(r, 0x0100); break;              /* no such sub-function: AL != 4Fh (measured on two BIOSes) */
    }
}

/* (glyph_12h -- the 8x16-at-640-stride planar glyph, with a background taken from
   BL bits 4-7 -- was replaced by VideoGraphicsGlyph for #252. QuickBASIC's SCREEN 12 text,
   the case it was written for, is the 8x16 row of VideoGraphicsGlyph's planar arm.) */

/* INT 10h text + mode + palette services. */
static VOID VideoInt10(PVOID self, PNTVDD_REGISTERS r)
{
    PVIDEO_STATE st = (PVIDEO_STATE)self;
    BYTE ah = VddGetAh(r), al = VddGetAl(r);
    st->IsDirty = 1;
    st->Int10WaitUntil = 0;          /* every call completes at once unless it says otherwise (#226) */
    switch (ah) {
    case 0x00:                                        /* set video mode          */
        /* ── ▶ AL BIT 7 = "DO NOT CLEAR VIDEO MEMORY", AND IT IS NOT DECORATION. ────
             Standard since the EGA: a mode set with bit 7 reprograms the card but
             LEAVES DISPLAY MEMORY ALONE. We masked the bit off to get the mode number
             and then threw it away, so every mode set wiped all four 64KB planes.
           ⚠ THAT IS THE MISSING LEMMINGS TOOLBAR. The game composes its skill-button
             panel into OFF-SCREEN VRAM at 0xF91F..0xFFFE, then sets its mode with
             `mov ax,0x008D` (0x0FB3) -- and the menu with `mov ax,0x0090` (0x4BEF) --
             exactly so that cache survives. We cleared it, and the panel blit at
             0x7626 then copied 1760 bytes of zeroes onto the screen, faithfully.
             MEASURED: the blit writes with wm=1 and lat=00000000 while the cache's
             own last write left 0x00/0x24/0x7F/0x00 there -- the copy was never wrong,
             its source had been destroyed underneath it.
           ▶ Everything else a mode set does still happens; only the erase is skipped,
             which is the whole difference the bit describes. */
        {   INT noclear = (al & 0x80) != 0;
        st->Mode = al & 0x7F; st->IsVesa = 0;        /* a standard mode leaves VESA */
        st->VesaTextMode = 0;                       /* ...including a VESA text mode */
        /* 40:87h bit 7 is "the last mode set did not clear memory" -- IBM's EGA/VGA
           BIOS copies AL bit 7 there, and VBE 2.0 §4.5/§4.6 builds 4F02h's D15 on the
           same bit (#226). It read 60h always. */
        st->IsModeSetNoClear = (BYTE)(noclear ? 1 : 0);
        st->VesaModeFlags = 0;
        st->VesaDacWidth = 6;                        /* §4.11: any mode set -> 6 bits */
        st->CursorRow = st->CursorColumn = 0; st->Page = 0;
        {   INT pg; for (pg = 0; pg < 8; ++pg) st->PageRow[pg] = st->PageColumn[pg] = 0; }   /* #252 */
        /* These three are the FALLBACK for a mode VGA_MODEDEFS does not cover:
           VideoLoadModeDefinition below overrides all of them from the measured table for
           every mode it knows, which is where 0x0D0E rather than this 8-line
           0x0607 comes from. A mode nobody measured still gets a sane cursor and
           blink enabled rather than zeros. */
        st->CursorShape = 0x0607;                       /* the BIOS resets the shape too */
        /* #188: 0040:0065/0066, the CGA mode-select and palette registers, are the
           standard CGA table for modes 00h-07h and are LEFT ALONE by the EGA/VGA
           modes -- measured (p_video2) on PCem's genuine IBM VGA ROM and DOSBox-X:
           0Dh-11h read back 05h's 2E after it, 12h/13h read 06h's 1E/3F. 0066 is 30h,
           3Fh in mode 6. We wrote 29h/30h once at start-up and never again. */
        if (st->Mode <= 0x07) {
            static const BYTE cga_msr[8] = { 0x2C, 0x28, 0x2D, 0x29, 0x2A, 0x2E, 0x1E, 0x29 };
            st->CgaSelect = (BYTE)(st->Mode == 0x06 ? 0x3F : 0x30);   /* AH=0Bh's shadow (#266) */
            if (st->BiosData) { st->BiosData[0x65] = cga_msr[st->Mode]; st->BiosData[0x66] = st->CgaSelect; }
        }
        st->IsBlink = 1;                                /* ...and re-enables blink (AR10 bit 3) */
        st->AttributeMode = (BYTE)(st->AttributeMode | 0x08u);   /* the register agrees    */
        st->IsUserFontOn = 0;                         /* the ROM font comes back with the mode */
        /* The cell height the mode's BIOS font gives: 8x16 in the VGA text modes and
           the 480-line graphics modes, 8x14 at 350 lines, 8x8 at 200. */
        st->CellHeight = VideoModeCellHeight(st->Mode);
        VideoLoadDefaultPalette(st);                    /* HW reloads the DAC on mode set */
        st->IsGeometryRegistersOk = 0;                         /* #325: until a measured set loads */
        VideoLoadModeDefinition(st, st->Mode);               /* ...and programs the register file */
        VideoLoadDefaultCrtc(st);                        /* ...and reprograms the CRTC     */
        {   UINT mi; PCVOID found = 0;
            for (mi = 0; mi < sizeof(g_VideoModes)/sizeof(g_VideoModes[0]); ++mi)
                if (g_VideoModes[mi].Mode == st->Mode) { found = &g_VideoModes[mi]; break; }
            if (!found) {                             /* a mode nobody defines   */
                VIDEO_UNIMPLEMENTED_SET(st->UnimplementedModes, st->Mode);
                st->ModeKind = VIDEO_KIND_TEXT;
                st->Columns = VIDEO_COLUMNS; st->Rows = VIDEO_ROWS;
                st->GraphicsWidth = VIDEO_FRAME_WIDTH; st->GraphicsHeight = VIDEO_FRAME_HEIGHT;
                if (!noclear) VideoClearText(st, 0x07);
            } else {
                st->ModeKind = g_VideoModes[mi].Kind;
                st->Columns  = g_VideoModes[mi].Columns;
                st->Rows  = g_VideoModes[mi].Rows;
                st->GraphicsWidth    = g_VideoModes[mi].Width;
                st->GraphicsHeight    = g_VideoModes[mi].Height;
                if (st->ModeKind == VIDEO_KIND_UNSUPPORTED) {
                    /* Say so; do not paint a text screen and let the program
                       draw into a layout that is not there. */
                    VIDEO_UNIMPLEMENTED_SET(st->UnimplementedModes, st->Mode);
                    st->ModeKind = VIDEO_KIND_TEXT;
                    st->Columns = VIDEO_COLUMNS; st->Rows = VIDEO_ROWS;
                    st->GraphicsWidth = VIDEO_FRAME_WIDTH;  st->GraphicsHeight = VIDEO_FRAME_HEIGHT;
                    if (!noclear) VideoClearText(st, 0x07);
                } else if (st->ModeKind == VIDEO_KIND_LINEAR8) {
                    INT i;
                    /* the BIOS sets SR4 = 0Eh for mode 13h: chain-4 ON (see the planar arm) */
                    st->MapMask = 0x0F; st->YMask = 0x0F;
                    if (!st->IsChain4) { st->IsChain4 = 1; st->Chain4Selects++;
                                       if (st->YMapSelect) st->YMapSelect(st->YMapContext, -1); }
                    if (!noclear) for (i = 0; i < VIDEO_MODE13_WIDTH * VIDEO_MODE13_HEIGHT; ++i) st->VideoMemory[i] = 0;
                } else if (st->ModeKind == VIDEO_KIND_PLANAR) {
                    INT pl; UINT32 i;
                    /* ── ★★★ THE BIOS PROGRAMS THE SEQUENCER TOO. (s74b) A mode set writes
                         SR4 = 06h for the planar modes (chain-4 OFF, odd/even off) and SR2 =
                         0Fh; we modelled neither, so chain4 kept whatever the guest last
                         wrote -- 1 from reset -- and the map-mask handler took its
                         `MaskSkipChain4` branch for EVERY mask write in mode 12h. The
                         host window never left the linear section, and a guest that fills
                         the four planes with mask 1/2/4/8 (Hexen's and Heretic's 640x480
                         loaders, from protected mode where no interpreter routes the store)
                         wrote all four on top of each other into memory nothing renders.
                         `planar hi_water=0`, black screen. Doom escaped because it programs
                         SR4 itself for mode Y. Real-mode guests escaped because their pixel
                         loops run in the interpreter, which reaches the engine regardless. */
                    st->MapMask = 0x0F; st->YMask = 0x0F;
                    if (st->IsChain4) { st->IsChain4 = 0; st->Chain4Selects++; }
                    if (st->YMapSelect) st->YMapSelect(st->YMapContext, (INT)st->MapMask);
                    if (!noclear)
                        for (pl = 0; pl < 4; ++pl)
                            for (i = 0; i < VIDEO_PLANE_SIZE; ++i) VideoPlaneBytes(st,pl)[i] = 0;
                } else if (st->ModeKind == VIDEO_KIND_CGA) {
                    INT i;
                    st->CgaBpp = (BYTE)(st->Mode == 0x06 ? 1 : 2);
                    st->CgaPalette = 0;
                    if (!noclear) for (i = 0; i < 16384; ++i) st->VideoMemory[VIDEO_TEXT_OFFSET + i] = 0;
                } else {
                    if (!noclear) VideoClearText(st, 0x07);
                }
            }
            /* ── ★ THE COLOURS ARE REBUILT AFTER THE MODE'S KIND IS KNOWN. (s81, user:
                 "white shows faint blue" after Doom; ENDOOM's bright words pale blue.)
                 VideoLoadDefaultPalette() above runs VideoPaletteRefresh() while mkind is still
                 the OLD mode's -- so leaving mode 13h built the text colours with the
                 8bpp rule (colour n = DAC n, no attribute palette): bright white 0Fh
                 drew as DAC 0Fh = AAAAFF, yellow as AAAA55, light cyan as 00AAFF. The
                 machine state was right (a report after the exit read vpal[15]=3F,
                 DAC 3F=FFFFFF); only the renderer's table was stale, and grey 07h looks
                 the same either way, which is why a shell prompt hid it. And the BIOS
                 resets the DAC pixel mask on a mode set as well. */
            /* ── AH=12h BL=30h CHOSE THE SCAN LINES FOR THIS (#252). The colour text modes
                 come up in 200, 350 or 400 lines with the font that fills 25 rows of
                 them -- 8x8, 8x14, 8x16. It answered "supported" and every text mode
                 came up at 400 regardless. */
            if (st->ModeKind == VIDEO_KIND_TEXT && st->Mode <= 0x03 && st->ScanSelect < 2) {
                st->CellHeight = (BYTE)(st->ScanSelect == 0 ? 8 : 14);
                st->GraphicsHeight = (WORD)(st->Rows * st->CellHeight);
            }
            /* ── #266: INT 43h FOLLOWS THE MODE. A VGA BIOS points the graphics-font
                 vector at the table of the new mode's cell (SeaVGABIOS's vga_set_mode:
                 8 / 14 / 16 from the mode's character height), so a program that reads
                 INT 43h after a mode set finds the font the BIOS will draw with. Nothing
                 wrote it before. ⚠ Whether IBM's ROM also does this in the TEXT modes is
                 unmeasured (p_vid266 `i43.mode03` asks). INT 1Fh is a power-on vector and
                 is left alone (VideoInt1FRom, at install). A caller font (AL=21h) ends here. */
            VideoInt43Rom(st, st->CellHeight);
            st->DacMask = 0xFF;
            VideoPaletteRefresh(st);
            /* ── AH=00h RETURNS A "VIDEO MODE FLAG" IN AL, NOT THE MODE. (s74b) Measured
                 on the AMI 486 ROM under PCem and on SeaBIOS alike (p_plan12: AX=0020
                 after mode 12h); RBIL documents it for Phoenix/AMI: 20h for modes > 7,
                 30h for modes 0-5 and 7, 3Fh for mode 6. We returned AL = the mode,
                 which is what a caller that saved AX would see as "mode 12h set" --
                 harmless for most, wrong for anything that keys on the flag. */
            VddSetAl(r, (BYTE)(st->Mode > 7 ? 0x20 : st->Mode == 6 ? 0x3F : 0x30));
            if (st->ModeQueryCount < 8) {
                st->ModeQueries[st->ModeQueryCount].Mode = st->Mode;
                st->ModeQueries[st->ModeQueryCount].Kind = st->ModeKind;
                st->ModeQueries[st->ModeQueryCount].Columns = st->Columns;
                st->ModeQueries[st->ModeQueryCount].Rows = st->Rows;
                st->ModeQueries[st->ModeQueryCount].Width    = st->GraphicsWidth;
                st->ModeQueries[st->ModeQueryCount].Height    = st->GraphicsHeight;
                st->ModeQueryCount++;
            }
        } }
        break;
    case 0x01: st->CursorShape = VddGetCx(r); break;
    /* ── 02h/03h: THE CURSOR OF PAGE BH (#252). Eight cursors, one per page -- the
         active page's is cur_row/cur_col, the rest pg_row/pg_col (0040:0050). BH was
         ignored, so positioning page 1's cursor moved the one on screen. */
    case 0x02: {
        BYTE pg = (BYTE)((VddGetBx(r) >> 8) & 7);
        if (pg == (st->Page & 7)) { st->CursorRow = (BYTE)(VddGetDx(r) >> 8); st->CursorColumn = (BYTE)(VddGetDx(r) & 0xFF); }
        else { st->PageRow[pg] = (BYTE)(VddGetDx(r) >> 8); st->PageColumn[pg] = (BYTE)(VddGetDx(r) & 0xFF); }
        break; }
    case 0x03: {
        /* THERE IS NO TEXT CURSOR IN A GRAPHICS MODE, and the BIOS says so: CX comes
           back 0000 in 06h/12h/13h, where we were still handing out the text
           underline shape 0607. Measured, p_video.asm VideoInt10.03.<mode>. The stored
           shape is left alone so returning to a text mode restores it. */
        BYTE pg = (BYTE)((VddGetBx(r) >> 8) & 7);
        INT act = (pg == (st->Page & 7));
        VddSetDx(r, (WORD)(((act ? st->CursorRow : st->PageRow[pg]) << 8) | (act ? st->CursorColumn : st->PageColumn[pg])));
        VddSetCx(r, (WORD)(st->ModeKind == VIDEO_KIND_TEXT ? st->CursorShape : 0));
        break; }
    /* ── 05h: SELECT THE ACTIVE PAGE, AND SHOW IT (#252). It stored the number and the
         BDA followed (oracle-verified, #188) -- but the CRTC start address never moved,
         so the screen stayed on page 0. The BIOS loads CR0C/CR0D with the page's offset:
         in WORDS in the text modes (page 1 of 80x25 = 0800h), in BYTES in the planar
         modes (0Dh page 1 = 2000h) -- p_vidtxt t03/t0D.05.crtc on PCem's IBM ROM. The
         renderers read the latched start (crtc_start_live), so it is loaded at once, as
         VideoLoadDefaultCrtc does. The cursor swaps with the page. Modes with one page
         (CGA, 11h-13h) keep the number only. */
    case 0x05: {
        BYTE np = (BYTE)(al & 7), op = (BYTE)(st->Page & 7);
        st->PageRow[op] = st->CursorRow; st->PageColumn[op] = st->CursorColumn;
        st->Page = al;
        st->CursorRow = st->PageRow[np]; st->CursorColumn = st->PageColumn[np];
        if (st->ModeKind == VIDEO_KIND_TEXT || (st->ModeKind == VIDEO_KIND_PLANAR && st->Mode <= 0x10)) {
            UINT32 off = (UINT32)np * VideoPageSize(st);
            if (st->ModeKind == VIDEO_KIND_TEXT) off >>= 1;
            st->CrtcStart = (WORD)off;
            st->CrtcStartLive = st->StartVs = (WORD)off;
            st->IsCrtcStartPending = 0;
            if (st->ModeKind == VIDEO_KIND_PLANAR) st->IsCrtcSeen = 1;   /* VideoRenderPlanar reads it */
        }
        break; }
    case 0x06:
        VideoScrollUp(st, al, (BYTE)(VddGetCx(r) >> 8), (BYTE)(VddGetCx(r) & 0xFF),
                  (BYTE)(VddGetDx(r) >> 8), (BYTE)(VddGetDx(r) & 0xFF), (BYTE)(VddGetBx(r) >> 8));
        break;
    /* ── 08h: READ THE CHARACTER AT PAGE BH's CURSOR -- from the cell in text, from
         the PIXELS in a graphics mode (VideoGraphicsReadChar), AH = 0 there. */
    case 0x08: {
        BYTE pg = (BYTE)((VddGetBx(r) >> 8) & 7);
        INT act = (pg == (st->Page & 7));
        INT rr = act ? st->CursorRow : st->PageRow[pg], cc = act ? st->CursorColumn : st->PageColumn[pg];
        if (st->ModeKind == VIDEO_KIND_TEXT) {
            BYTE *p = VideoPageCell(st, pg, rr, cc);
            VddSetAx(r, (WORD)((p[1] << 8) | p[0]));
        } else VddSetAx(r, VideoGraphicsReadChar(st, pg, cc, rr));
        break; }
    /* ── 09h/0Ah: CX copies at page BH's cursor, cursor not moved. Text: the (char,
         attr) pairs on THAT page. Graphics: the glyph, BL = colour (0Ah too -- RBIL:
         "BL = colour in graphics modes"), bit 7 XOR; see VideoGraphicsGlyph. */
    case 0x09:
    case 0x0A: {
        WORD n = VddGetCx(r); BYTE attr = (BYTE)(VddGetBx(r) & 0xFF);
        BYTE pg = (BYTE)((VddGetBx(r) >> 8) & 7);
        INT act = (pg == (st->Page & 7));
        INT c = act ? st->CursorColumn : st->PageColumn[pg], rr = act ? st->CursorRow : st->PageRow[pg];
        if (!n) n = 1;
        while (n-- && rr < st->Rows) {
            if (st->ModeKind == VIDEO_KIND_TEXT) {
                BYTE *p = VideoPageCell(st, pg, rr, c); p[0] = al; if (ah == 0x09) p[1] = attr;
            } else VideoGraphicsGlyph(st, pg, c, rr, al, attr);
            if (++c >= st->Columns) { c = 0; if (++rr >= st->Rows) break; }
        }
        break; }
    /* ── 0Ch/0Dh: ONE PIXEL, IN THE MODE'S OWN GEOMETRY (#252). The planar arm used a
         640-pixel stride in every planar mode and ignored the page; the CGA modes were
         not written at all (and read 0). AL bit 7 = XOR, except in 13h (one byte a
         pixel: the colour is all eight bits). BH = page in the planar modes. */
    case 0x0C: {
        UINT32 x = VddGetCx(r), y = VddGetDx(r);
        INT xr = (al & 0x80) != 0;
        if (st->IsVesa) break;
        if (st->ModeKind == VIDEO_KIND_LINEAR8) {
            if (x < st->GraphicsWidth && y < st->GraphicsHeight && y * st->GraphicsWidth + x < VIDEO_APERTURE_SIZE) st->VideoMemory[y * st->GraphicsWidth + x] = al;
        } else if (st->ModeKind == VIDEO_KIND_PLANAR) {
            if (x < st->GraphicsWidth && y < st->GraphicsHeight) {
                UINT32 byte = (UINT32)((VddGetBx(r) >> 8) & 7) * VideoPageSize(st) + y * (st->GraphicsWidth / 8u) + (x >> 3);
                BYTE  bit = (BYTE)(0x80 >> (x & 7)), p;
                if (byte < VIDEO_PLANE_SIZE)
                    for (p = 0; p < 4; ++p) {
                        if (xr) { if (al & (1 << p)) VideoPlaneBytes(st,p)[byte] ^= bit; }
                        else if (al & (1 << p)) VideoPlaneBytes(st,p)[byte] |= bit;
                        else                    VideoPlaneBytes(st,p)[byte] &= (BYTE)~bit;
                    }
            }
        } else if (st->ModeKind == VIDEO_KIND_CGA) {
            if (x < st->GraphicsWidth && y < st->GraphicsHeight) {
                BYTE *m = st->VideoMemory + VIDEO_TEXT_OFFSET + ((y & 1) ? 0x2000u : 0u) + (y >> 1) * 80u;
                if (st->CgaBpp == 1) {
                    BYTE bit = (BYTE)(0x80 >> (x & 7));
                    if (xr) { if (al & 1) m[x >> 3] ^= bit; }
                    else m[x >> 3] = (BYTE)((m[x >> 3] & ~bit) | ((al & 1) ? bit : 0));
                } else {
                    UINT sh = 6u - 2u * (x & 3u);
                    BYTE v = (BYTE)((al & 3u) << sh);
                    if (xr) m[x >> 2] ^= v;
                    else m[x >> 2] = (BYTE)((m[x >> 2] & ~(3u << sh)) | v);
                }
            }
        }
        break; }
    /* ── 0Eh: TELETYPE ON THE ACTIVE PAGE; BL = foreground in graphics (#252). BH is
         not consulted -- p_vidtxt t03.0E.which: with page 1 active a BH=0 teletype
         lands on page 1 (PCem's IBM ROM). */
    case 0x0E:
        VideoTeletypeChar(st, al, (BYTE)(VddGetBx(r) & 0xFF));
        break;
    case 0x0F:
        /* BH is the active page; BL IS NOT DEFINED BY THIS CALL and the real BIOS
           leaves it alone -- we were zeroing the whole of BX and taking the caller's
           BL with it. Measured: the oracle returns the probe's poison in BL. */
        VddSetAx(r, (WORD)((st->Columns << 8) | st->Mode));
        VddSetBx(r, (WORD)((st->Page << 8) | (VddGetBx(r) & 0xFF)));
        break;
    case 0x10:                                        /* palette / DAC            */
        /* ⚠ THE DAC CALLS BELOW GO THROUGH VideoDacPackWidth / VideoDacFrom8, i.e. AT THE DAC
             WIDTH (#226). A VGA BIOS implements them as plain OUTs to 3C8h/3C9h and INs
             from 3C7h/3C9h -- it does not know the RAMDAC was switched -- so after 4F08h
             BH=8 a real card takes these values as 8-bit too. Same device, same rule. */
        if (al == 0x10) {                             /* set one DAC register     */
            WORD idx = VddGetBx(r);
            st->DacBlock[((idx & 0xFF) >> 4) & 15]++;
            st->Dac[idx & 0xFF] = VideoDacPackWidth(st, (BYTE)(VddGetDx(r) >> 8),
                                             (BYTE)(VddGetCx(r) >> 8), (BYTE)VddGetCx(r));
            if (st->IsGreySum) VideoDacGrey(st, idx & 0xFF, 1);   /* 12h BL=33h (#252) */
            VideoPaletteRefresh(st);
        } else if (al == 0x12) {                      /* set block of DAC regs    */
            WORD first = VddGetBx(r), n = VddGetCx(r), i;
            BYTE *t = (BYTE *)VddMapFlat(st->Bus, r->Es, (WORD)VddGetDx(r));
            for (i = 0; i < n && (first + i) < 256; ++i)
                { st->Dac[first + i] = VideoDacPackWidth(st, t[i*3], t[i*3+1], t[i*3+2]);
                  st->DacBlock[((first + i) >> 4) & 15]++;
                  if (((first + i) & 0xF0) == 0x30) st->DacHighSinceReset++; }
            st->DacWrites += n;
            if (st->IsGreySum) VideoDacGrey(st, first, n);   /* 12h BL=33h (#252) */
            VideoPaletteRefresh(st);
        } else if (al == 0x00) {                      /* set one palette register */
            BYTE reg = (BYTE)((VddGetBx(r) >> 8) & 0xFF);
            if (reg < 17) { st->PaletteRegisters[reg] = (BYTE)(VddGetBx(r) & 0x3F); st->AcBiosWrites++; }
            VideoPaletteRefresh(st);   /* AH=10h stored vpal and rendered from ega16: inert until now */
        } else if (al == 0x01) {                      /* set the border           */
            st->PaletteRegisters[16] = st->Overscan = (BYTE)((VddGetBx(r) >> 8) & 0x3F);
        } else if (al == 0x02) {                      /* set all 16 + border      */
            BYTE *t = (BYTE *)VddMapFlat(st->Bus, r->Es, (WORD)VddGetDx(r));
            INT i; for (i = 0; i < 17; ++i) st->PaletteRegisters[i] = (BYTE)(t[i] & 0x3F);
            st->Overscan = st->PaletteRegisters[16];
            st->AcBiosWrites += 17;
            VideoPaletteRefresh(st);
        } else if (al == 0x03) {                      /* blink vs bright background */
            /* The BIOS's job here is to write AR10 bit 3; keep both in step so a
               guest that sets it through the BIOS and then READS the register back
               sees what it asked for. Oracle-measured (p_video.asm ar10.blink.*):
               BL=0 leaves AR10 bit 3 clear, BL=1 sets it. */
            st->IsBlink = (BYTE)(VddGetBx(r) & 1);
            st->AttributeMode = (BYTE)((st->AttributeMode & ~0x08u) | (st->IsBlink ? 0x08u : 0u));
        } else if (al == 0x07) {                      /* get one palette register */
            BYTE reg = (BYTE)((VddGetBx(r) >> 8) & 0xFF);
            VddSetBx(r, (WORD)((VddGetBx(r) & 0xFF00) | (reg < 17 ? st->PaletteRegisters[reg] : 0)));
        } else if (al == 0x08) {                      /* get the border           */
            VddSetBx(r, (WORD)((VddGetBx(r) & 0x00FF) | ((WORD)st->PaletteRegisters[16] << 8)));
        } else if (al == 0x09) {                      /* get all 16 + border      */
            BYTE *t = (BYTE *)VddMapFlat(st->Bus, r->Es, (WORD)VddGetDx(r));
            INT i; for (i = 0; i < 17; ++i) t[i] = st->PaletteRegisters[i];
        } else if (al == 0x13) {                      /* select DAC page / mode   */
            st->DacPage = (BYTE)(VddGetBx(r) >> 8);
        } else if (al == 0x15) {                      /* get one DAC register     */
            UINT32 v = st->Dac[VddGetBx(r) & 0xFF];
            VddSetDx(r, (WORD)((WORD)VideoDacFrom8(st, (BYTE)(v >> 16)) << 8));
            VddSetCx(r, (WORD)(((WORD)VideoDacFrom8(st, (BYTE)(v >> 8)) << 8)
                              | VideoDacFrom8(st, (BYTE)v)));
        } else if (al == 0x17) {                      /* get block of DAC regs    */
            WORD first = VddGetBx(r), n = VddGetCx(r), i;
            BYTE *t = (BYTE *)VddMapFlat(st->Bus, r->Es, (WORD)VddGetDx(r));
            for (i = 0; i < n && (first + i) < 256; ++i) {
                UINT32 v = st->Dac[first + i];
                t[i*3]   = VideoDacFrom8(st, (BYTE)(v >> 16));
                t[i*3+1] = VideoDacFrom8(st, (BYTE)(v >> 8));
                t[i*3+2] = VideoDacFrom8(st, (BYTE)v);
            }
        } else if (al == 0x1A) {                      /* get DAC page state       */
            VddSetBx(r, (WORD)((st->DacPage << 8) | 0));
        } else if (al == 0x1B) {                      /* convert to grey scale    */
            VideoDacGrey(st, VddGetBx(r), VddGetCx(r));
            VideoPaletteRefresh(st);
        } else {
            VIDEO_UNIMPLEMENTED_SET(st->UnimplementedFunctions, 0x10);      /* name it, do not ignore it */
        }
        break;
    case 0x07: {                                       /* scroll window DOWN      */
        /* 06h scrolled up and 07h fell through to the unimplemented default, so
           any program scrolling downwards silently did nothing. */
        BYTE n = al, top = (BYTE)(VddGetCx(r) >> 8), lft = (BYTE)(VddGetCx(r) & 0xFF);
        BYTE bot = (BYTE)(VddGetDx(r) >> 8), rgt = (BYTE)(VddGetDx(r) & 0xFF);
        BYTE attr = (BYTE)(VddGetBx(r) >> 8);
        INT rr, cc, k;
        if (st->ModeKind != VIDEO_KIND_TEXT) {              /* graphics: pixels, BH = fill colour (#252) */
            VideoGraphicsScroll(st, n, top, lft, bot, rgt, attr, 0);
            break;
        }
        if (!n || n > (bot - top + 1)) {               /* 0 or oversized = clear  */
            for (rr = top; rr <= bot; ++rr)
                for (cc = lft; cc <= rgt; ++cc)
                    { BYTE *p2 = VideoCell(st, rr, cc); p2[0] = ' '; p2[1] = attr; }
        } else {
            for (k = 0; k < n; ++k) {
                for (rr = bot; rr > top; --rr)
                    for (cc = lft; cc <= rgt; ++cc) {
                        BYTE *d2 = VideoCell(st, rr, cc), *s2 = VideoCell(st, rr - 1, cc);
                        d2[0] = s2[0]; d2[1] = s2[1];
                    }
                for (cc = lft; cc <= rgt; ++cc)
                    { BYTE *p2 = VideoCell(st, top, cc); p2[0] = ' '; p2[1] = attr; }
            }
        }
        break; }
    /* ── 0Bh: SET BACKGROUND / BORDER / CGA PALETTE, THROUGH THE ATTRIBUTE CONTROLLER (#266).
         It wrote BL into `overscan` (not AR11, which read back unchanged) whatever the
         mode, and BH=1 flipped a private `CgaPalette` that VideoRenderCga indexed a hard-coded
         table with -- so the AC registers a guest reads back, and AH=10h's view of them,
         never moved, and a background colour never appeared in any graphics mode.
         Now the CGA colour-select byte 0040:0066 is maintained (VddCgaColourSelect)
         and turned into AC values the way the VGA BIOS does (DOSBox's arithmetic; see
         VddCgaBackgroundAr / VddCgaPaletteAr):
           BH=0  AR11 (border) = BL as an AC colour, in every mode. In a graphics mode
                 AR00 (the background) too -- except 06h, where CGA's colour select is the
                 FOREGROUND and AR01 takes it. In 04h/05h AR01-03 follow 0066 bit 4
                 (intensity, BL bit 4).
           BH=1  0066 bit 5 = BL bit 0; in 04h/05h AR01-03 = palette 0 (2/4/6) or 1
                 (3/5/7), keeping the intensity. Other modes: the byte only.
         ⚠ UNMEASURED, and p_vid266 asks each one on PCem's IBM ROM: (a) AR00 in the
           EGA/VGA graphics modes (DOSBox sets it AND AR01-03 for every mode above 3;
           the LGPL/SeaVGABIOS line sets AR00 in text modes too and never AR11); (b) the
           06h foreground (taken from the CGA, the issue's reading); (c) 0066 in text. */
    case 0x0B: {
        BYTE bh = (BYTE)(VddGetBx(r) >> 8), bl = (BYTE)(VddGetBx(r) & 0xFF);
        BYTE sel = st->BiosData ? st->BiosData[0x66] : st->CgaSelect;
        INT gfx = (st->ModeKind != VIDEO_KIND_TEXT) && !st->IsVesa;
        INT cga4 = gfx && (st->Mode == 0x04 || st->Mode == 0x05);
        BYTE ar[3];
        if (bh > 1) break;                             /* not a defined BH: nothing    */
        sel = VddCgaColourSelect(sel, bh, bl);
        st->CgaSelect = sel;
        if (st->BiosData) st->BiosData[0x66] = sel;
        if (bh == 0) {
            BYTE v = VddCgaBackgroundAr(bl);
            VideoAttributeBiosSet(st, 0x11, v);
            if (gfx) VideoAttributeBiosSet(st, (BYTE)(st->Mode == 0x06 ? 0x01 : 0x00), v);
        } else {
            st->CgaPalette = (BYTE)(bl & 1);
        }
        if (cga4) {
            VddCgaPaletteAr(sel, ar);
            VideoAttributeBiosSet(st, 1, ar[0]); VideoAttributeBiosSet(st, 2, ar[1]); VideoAttributeBiosSet(st, 3, ar[2]);
        }
        VideoPaletteRefresh(st);
        break; }
    /* ── 04h: READ LIGHT PEN. A VGA has no light-pen input; its BIOS answers AH=00h,
         "not triggered" (#266). We left AH=04h -- which a caller reads as "triggered",
         with BX/CX/DX as a position. The other registers are untouched.
       ★ s92, MEASURED (dosdiff p_vid266 t03.04.pen): AX=0000 on MS-DOS 6.22, DOSBox-X and
         PCem's IBM VGA BIOS alike -- AL is cleared too, not only AH. */
    case 0x04:
        VddSetAx(r, 0x0000);
        break;
    case 0x0D: {                                       /* READ a pixel            */
        WORD x = VddGetCx(r), y = VddGetDx(r);
        BYTE v = 0;
        if (st->ModeKind == VIDEO_KIND_LINEAR8) {
            if (x < st->GraphicsWidth && y < st->GraphicsHeight) v = st->VideoMemory[y * st->GraphicsWidth + x];
        } else if (st->ModeKind == VIDEO_KIND_PLANAR) {     /* page BH (#252) */
            UINT32 byi = (UINT32)((VddGetBx(r) >> 8) & 7) * VideoPageSize(st) + y * (st->GraphicsWidth / 8) + (x >> 3);
            BYTE  msk = (BYTE)(0x80 >> (x & 7));
            if (byi < VIDEO_PLANE_SIZE)
                v = (BYTE)(((VideoPlaneBytes(st,0)[byi] & msk) ? 1 : 0)
                            | ((VideoPlaneBytes(st,1)[byi] & msk) ? 2 : 0)
                            | ((VideoPlaneBytes(st,2)[byi] & msk) ? 4 : 0)
                            | ((VideoPlaneBytes(st,3)[byi] & msk) ? 8 : 0));
        } else if (st->ModeKind == VIDEO_KIND_CGA && x < st->GraphicsWidth && y < st->GraphicsHeight) {   /* #252: read 0 before */
            const BYTE *m = st->VideoMemory + VIDEO_TEXT_OFFSET + ((y & 1) ? 0x2000u : 0u) + (UINT32)(y >> 1) * 80u;
            v = st->CgaBpp == 1 ? (BYTE)((m[x >> 3] >> (7 - (x & 7))) & 1)
                                 : (BYTE)((m[x >> 2] >> (6 - 2 * (x & 3))) & 3);
        }
        VddSetAx(r, (WORD)((VddGetAx(r) & 0xFF00) | v));
        break; }
    case 0x1C: {                                       /* save / restore state    */
        /* CX is a bitmask of what to save; ES:BX is the buffer. Same state block as
           VBE 4F04 -- see VideoStateSave(). ⚠ This used to report 3 blocks and then
           write 768 bytes: a guest that allocated what it was told got its next MCB
           overwritten. The size reported is now the size written. */
        BYTE al1c = al;
        if (al1c == 0x00)      { VddSetBx(r, VIDEO_STATE_BLOCKS); VddSetAx(r, (WORD)((VddGetAx(r) & 0xFF00) | 0x1C)); }
        else if (al1c == 0x01 || al1c == 0x02) {
            BYTE *buf = (BYTE *)VddMapFlat(st->Bus, r->Es, (WORD)r->Ebx);
            if (al1c == 0x01) VideoStateSave(st, buf, VddGetCx(r));
            else              (VOID)VideoStateLoad(st, buf);   /* a foreign buffer: no-op */
            VddSetAx(r, (WORD)((VddGetAx(r) & 0xFF00) | 0x1C));
        }
        break; }
    /* ── 13h WRITE STRING (#252). AL bit 0 = leave the cursor after the string (clear:
         put it back), bit 1 = the string is (char, attr) pairs (else BL for all); BH =
         page; DH/DL = where. BEL, BS, CR and LF are EXECUTED, as the teletype does --
         they were stored as glyphs, the cursor was always moved, the page ignored, and
         a graphics mode got (char, attr) pairs at B800:0. Scrolling past the bottom
         happens on the active page only. */
    case 0x13: {
        WORD n = VddGetCx(r), i; BYTE mode = al, attr = (BYTE)(VddGetBx(r) & 0xFF);
        BYTE pg = (BYTE)((VddGetBx(r) >> 8) & 7);
        INT act = (pg == (st->Page & 7));
        INT rr = (BYTE)(VddGetDx(r) >> 8), cc = (BYTE)(VddGetDx(r) & 0xFF);
        BYTE *s = (BYTE *)VddMapFlat(st->Bus, r->Es, (WORD)r->Ebp);
        if (!s) break;
        for (i = 0; i < n; ++i) {
            BYTE ch = *s++; if (mode & 0x02) attr = *s++;
            if (ch == 0x0D) { cc = 0; continue; }
            if (ch == 0x08) { if (cc) --cc; continue; }
            if (ch == 0x07) continue;
            if (ch != 0x0A) {
                if (rr < st->Rows && cc < st->Columns) {
                    if (st->ModeKind == VIDEO_KIND_TEXT) { BYTE *p = VideoPageCell(st, pg, rr, cc); p[0] = ch; p[1] = attr; }
                    else VideoGraphicsGlyph(st, pg, cc, rr, ch, attr);
                }
                if (++cc < st->Columns) continue;
                cc = 0;
            }
            if (++rr >= st->Rows) {
                if (act) VideoScrollUp(st, 1, 0, 0, st->Rows - 1, st->Columns - 1, VideoTeletypeFill(st));
                rr = st->Rows - 1;
            }
        }
        if (mode & 0x01) {
            if (act) { st->CursorRow = (BYTE)rr; st->CursorColumn = (BYTE)cc; }
            else     { st->PageRow[pg] = (BYTE)rr; st->PageColumn[pg] = (BYTE)cc; }
        }
        break; }
    case 0x11:                                         /* character generator     */
        st->Int10Ah11Calls++;                          /* did the guest ASK at all? */
        if (al == 0x30) {                              /* get font info -> ES:BP  */
            /* THE POINTER IS THE POINT. BH selects which table the caller wants, and the
               answer is returned in ES:BP with CX = bytes per character. Returning only
               CX/DL (as we used to) leaves the caller drawing from whatever ES:BP already
               held -- which is why Skyroads' "ROAD COMPLETED" came out as glyph-shaped
               noise. BH: 0/1 = the INT 1Fh / INT 43h vectors, 2 = 8x14, 3 = 8x8 lower,
               4 = 8x8 upper (chars 128-255), 5 = 9x14 alt, 6 = 8x16, 7 = 9x16 alt. We hold
               two real tables and answer every code from the nearer of the two. */
            BYTE bh = (BYTE)((VddGetBx(r) >> 8) & 0xFF);
            WORD seg = VDD_FONT8X16_SEG, off = 0, bpc = 16;
            switch (bh) {
            case 0x03: seg = VDD_FONT8X8_SEG;  off = 0;       bpc = 8;  break;
            case 0x00:                                        /* INT 1Fh: 8x8 upper half */
                /* #266: what INT 1Fh holds -- ours (8x8 upper half) unless AL=20h
                   installed the caller's. */
                if (st->IsInt1FUser) { seg = st->Int1FSegment; off = st->Int1FOffset; bpc = 8; break; }
                /* fall through */
            case 0x04: seg = VDD_FONT8X8_SEG;  off = 128 * 8; bpc = 8;  break;
            case 0x02:                                        /* ROM 8x14 / 9x14 alt     */
            case 0x05: seg = VDD_FONT8X14_SEG; off = 0;       bpc = 14; break;
            case 0x01:                                        /* INT 43h: the CURRENT font */
                /* Whatever the active mode actually draws with -- 8x8 in the 200-line
                   graphics modes, 8x16 in text. We used to answer this (and 8x14) with the
                   8x16 table while reporting CX=14, so a caller striding by 14 through
                   16-byte glyphs drifted 2 bytes per character and drew shredded text.
                   cell_h is that answer, and it follows a 1112h/1111h font change too. */
                /* #266: and a caller's graphics font (AL=21h) IS the current font --
                   INT 43h points at it, so this answers with it. */
                if (st->IsGraphicsFontUser) { seg = st->Int43Segment; off = st->Int43Offset; bpc = st->CellHeight; }
                else if (st->CellHeight == 8)  { seg = VDD_FONT8X8_SEG;  off = 0; bpc = 8;  }
                else if (st->CellHeight == 14) { seg = VDD_FONT8X14_SEG; off = 0; bpc = 14; }
                else                       { seg = VDD_FONT8X16_SEG; off = 0; bpc = 16; }
                break;
            default:   seg = VDD_FONT8X16_SEG; off = 0;       bpc = 16; break;
            }
            r->Es = seg; r->Ebp = off;
            /* ── ★ CX IS THE ON-SCREEN FONT'S HEIGHT, NOT THE REQUESTED TABLE'S. ──
                 The classic gotcha in this call, and we had it backwards: BH selects
                 which TABLE ES:BP points at, but CX reports the height of the font
                 the screen is CURRENTLY drawing with. Measured on 6.22: BH=0 asks for
                 the 8x8 upper half and CX still comes back 16 in mode 3. We answered
                 8, contradicting our OWN BDA byte at 0040:0085 two lines of probe
                 output earlier -- and a 43/50-line editor sizes the screen from CX. */
            VddSetCx(r, st->CellHeight ? st->CellHeight : bpc);    /* on-screen bytes/character */
            VddSetDx(r, (WORD)(st->Rows ? st->Rows - 1 : 24));  /* DL = rows-1     */
            if (st->FontQueryCount < 4) {                     /* record the request + answer */
                st->FontQueries[st->FontQueryCount].Al  = al;
                st->FontQueries[st->FontQueryCount].Bh  = bh;
                st->FontQueries[st->FontQueryCount].Segment = seg;
                st->FontQueries[st->FontQueryCount].Offset = off;
                st->FontQueries[st->FontQueryCount].Cx  = bpc;
                st->FontQueryCount++;
            }
        } else if (al == 0x03) {
            /* ── 03h: SET BLOCK SPECIFIER = the Sequencer's Character Map Select (#266).
                 BL goes to SR3 as-is (SeaVGABIOS: stdvga_set_text_block_specifier) --
                 which font block attribute bit 3 = 0 / = 1 cells are drawn from. It was
                 caught by the ROM-font arm below as "AL & 0Fh <= 4" and reloaded the
                 8x16 ROM font, DROPPING a font the program had just loaded to select it.
               ⚠ The renderer still draws one font (the loaded one, whatever block it
                 was loaded to), so SR3 reads back right and selects nothing yet: the
                 512-character case is a docs/inventory/vga.md gap. */
            st->SequencerRegisters[3] = (BYTE)(VddGetBx(r) & 0xFF);
        } else if ((al & 0x0F) <= 0x04 && al != 0x13 && (al <= 0x04 || (al >= 0x10 && al <= 0x14))) {
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
                WORD fseg = r->Es, foff = (WORD)(r->Ebp & 0xFFFF);
                WORD cnt = (WORD)VddGetCx(r), first = (WORD)VddGetDx(r);
                BYTE  bpc = (BYTE)((VddGetBx(r) >> 8) & 0xFF);
                const BYTE *src = (const BYTE *)VddMapFlat(st->Bus, fseg, foff);
                if (!src || bpc == 0 || bpc > VIDEO_CELL_HEIGHT) {
                    /* Cannot represent it -- a cell is VIDEO_CELL_HEIGHT tall. Say so
                       rather than store something the renderer would misread. */
                    VIDEO_UNIMPLEMENTED_SET(st->UnimplementedFunctions, 0x11);
                } else {
                    UINT i, y;
                    if (!st->IsUserFontOn) {       /* seed from ROM so characters
                                                      the caller does NOT supply
                                                      still draw as themselves */
                        UINT c2;
                        for (c2 = 0; c2 < 256; ++c2)
                            for (y = 0; y < VIDEO_CELL_HEIGHT; ++y)
                                st->UserFont[c2 * VIDEO_CELL_HEIGHT + y] = vga_font_8x16[c2][y];
                    }
                    for (i = 0; i < cnt && (first + i) < 256; ++i) {
                        UINT ch2 = first + i;
                        for (y = 0; y < VIDEO_CELL_HEIGHT; ++y)
                            st->UserFont[ch2 * VIDEO_CELL_HEIGHT + y] =
                                (y < bpc) ? src[i * bpc + y] : 0;
                    }
                    st->UserFontRows = bpc;
                    st->IsUserFontOn = 1;
                    st->IsDirty = 1;
                    /* AL=10h (not 00h) also reprograms the CRTC for the new height:
                       the cell becomes the font's height and the row count follows. */
                    if (al == 0x10 && st->ModeKind == VIDEO_KIND_TEXT) {
                        st->CellHeight = bpc;
                        st->Rows = VideoTextRowsFor(bpc);
                        VideoInt43Rom(st, bpc);  /* 1130h BH=1 and INT 43h agree (#266) */
                    }
                }
            } else {
                /* ── ★ AL=x1/x2/x4 SELECT A ROM FONT, AND WITH 1x THAT IS A ROW COUNT. ──
                     1112h is THE 50-line call: load the 8x8 ROM font and, because AL
                     has bit 4 set, recompute the CRTC -- 400 lines / 8 = 50 rows.
                     1111h is 8x14 (28 rows) and 1114h 8x16 (25). The 0x variants only
                     change the glyphs, which is what the BIOS documents and what a
                     caller that follows 1102h with its own CRTC programming expects. */
                BYTE h = (BYTE)(((al & 0x0F) == 0x02) ? 8 : ((al & 0x0F) == 0x01) ? 14 : 16);
                st->IsUserFontOn = 0;              /* back to the ROM tables */
                if ((al & 0x10) && st->ModeKind == VIDEO_KIND_TEXT) {
                    st->CellHeight = h;
                    st->Rows = VideoTextRowsFor(h);
                    if (st->CursorRow >= st->Rows) st->CursorRow = (BYTE)(st->Rows - 1);
                    /* #266: INT 43h follows the cell, so 1130h BH=1 (which answers with
                       the cell's table, a measured fix) and the vector stay one fact.
                       ⚠ SeaVGABIOS leaves INT 43h alone on a text-mode font load; IBM's
                       ROM is unmeasured. Kept consistent with our own 1130h instead. */
                    VideoInt43Rom(st, h);
                }
                st->IsDirty = 1;
            }
            VddSetDx(r, (WORD)(st->Rows ? st->Rows - 1 : 24));
        } else if (al >= 0x20 && al <= 0x24) {
            /* ── 20h-24h: THE GRAPHICS-MODE FONT CALLS (#266). They answered DL and
                 stored nothing -- no vector, no row count -- so a program that loaded
                 its own graphics font (or asked for 43 rows of 8x8 in mode 10h) kept
                 drawing with the ROM table at the mode's own height.
                   20h  INT 1Fh = ES:BP (the 8x8 characters 80h-FFh)
                   21h  INT 43h = ES:BP, CX bytes a character
                   22h  INT 43h = the ROM 8x14     23h  the ROM 8x8     24h  the ROM 8x16
                 21h-24h also set the screen's rows from BL (VddGraphicsFontRows: 0 = DL,
                 1 = 14, 2 = 25, 3 = 43) and the character height -- 0040:0084/0085 --
                 which here are `rows` and `CellHeight`, so the glyph services draw at that
                 height from that table (VideoGraphicsFont) and the BDA follows.
               ⚠ Registers are left as they came (SeaVGABIOS returns nothing; this used
                 to write DX). In a TEXT mode only the vector moves: rows/cell_h are the
                 text screen's geometry, and the calls are documented for graphics modes
                 (IBM's text-mode behaviour unmeasured). A height outside 1..16 sets the
                 vector and is reported, since our glyph paths draw at most 16 lines. */
            if (al == 0x20) {
                st->Int1FSegment = r->Es; st->Int1FOffset = (WORD)(r->Ebp & 0xFFFF); st->IsInt1FUser = 1;
                VideoSetVector(st, 0x1F, st->Int1FSegment, st->Int1FOffset);
            } else {
                BYTE h = al == 0x21 ? (BYTE)(VddGetCx(r) & 0xFF) : al == 0x22 ? 14 : al == 0x23 ? 8 : 16;
                BYTE rows = VddGraphicsFontRows((BYTE)(VddGetBx(r) & 0xFF), (BYTE)(VddGetDx(r) & 0xFF));
                if (al == 0x21) {
                    st->Int43Segment = r->Es; st->Int43Offset = (WORD)(r->Ebp & 0xFFFF); st->IsGraphicsFontUser = 1;
                    VideoSetVector(st, 0x43, st->Int43Segment, st->Int43Offset);
                } else VideoInt43Rom(st, h);
                if (st->ModeKind != VIDEO_KIND_TEXT && !st->IsVesa) {
                    if (h >= 1 && h <= 16 && rows) { st->CellHeight = h; st->Rows = rows; }
                    else VIDEO_UNIMPLEMENTED_SET(st->UnimplementedFunctions, 0x11);
                }
            }
        } else {
            VIDEO_UNIMPLEMENTED_SET(st->UnimplementedFunctions, 0x11);
        }
        break;
    case 0x12: {                                       /* alternate function sel  */
        BYTE bl = (BYTE)(VddGetBx(r) & 0xFF);
        if (bl == 0x10) { VddSetBx(r, 0x0003); VddSetCx(r, 0x0009); }  /* color / 256K, switches */
        /* ── ★★ BL=31h: DEFAULT PALETTE LOADING. NOT A NO-OP. ────────────────────
             A mode set normally reloads the DAC and the attribute palette, which
             destroys any colours the program has already installed. A game that
             wants to keep them says so with this call BEFORE setting the mode --
             and that is exactly what Lemmings does.
           ⚠ WE USED TO ACCEPT IT AND IGNORE IT. The `else` arm below answers
             AL=12h ("supported") to every unhandled BL, so this looked handled and
             did nothing: measured, Lemmings wrote 752 entries into DAC 0x30..0x3F,
             a mode set wiped them (palresets=5, hi_since_reset=0), and it never
             wrote them again -- because on real hardware it did not have to. Its
             terrain then rendered in the leftover default EGA brights, which is the
             cyan-and-green "garbling" this whole thread has been chasing.
             The same "unimplemented call still answers" shape as ever: a sentinel
             that reads as success is worse than a refusal. */
        else if (bl == 0x31) {
            st->IsDefaultPaletteOff = (BYTE)((VddGetAx(r) & 0xFF) ? 1 : 0);
            VddSetAx(r, (WORD)((VddGetAx(r) & 0xFF00) | 0x12));
        }
        /* ── #252: THE REST OF THE VGA's BL TABLE, EACH DOING ITS JOB -- and anything
             else REFUSED. The `else` used to answer AL=12h for every BL, so 30h/32h/33h/
             34h/36h each said "done" and did nothing. AL in = 0 enable / 1 disable (30h:
             0/1/2 = 200/350/400 lines); AL out = 12h. An AL out of range is refused too.
               30h  the next colour TEXT mode set comes up in that many lines (VideoInt10 AH=00h);
               32h  CPU access to video memory: MiscOut bit 1 is the switch on the card.
                    Recorded in the register (3CCh reads it back); our aperture is host
                    memory and is not cut off -- a docs/inventory/vga.md gap, not this one;
               33h  grey-scale summing of later DAC loads (mode set, AH=10h AL=10h/12h);
               34h  CGA cursor emulation -- off, AH=01h's CX is drawn literally;
               36h  video refresh: Clocking Mode (SR1) bit 5 "screen off", recorded the
                    same way as 32h (the renderer does not blank on SR1 -- vga.md).
             Unknown BL: PCem's IBM VGA ROM (and DOSBox-X) return AL=00h -- p_vidtxt
             t12.55.unknown -- so does this. (SeaVGABIOS leaves AL: provisional.) */
        else if (bl == 0x30 || bl == 0x32 || bl == 0x33 || bl == 0x34 || bl == 0x36) {
            BYTE a = (BYTE)(VddGetAx(r) & 0xFF);
            if (a > (bl == 0x30 ? 2 : 1)) { VddSetAx(r, (WORD)(VddGetAx(r) & 0xFF00)); break; }
            if (bl == 0x30)      st->ScanSelect = a;
            else if (bl == 0x32) { st->IsVideoOff = a; st->MiscOutput = (BYTE)(a ? (st->MiscOutput & ~0x02u) : (st->MiscOutput | 0x02u)); }
            else if (bl == 0x33) st->IsGreySum = (BYTE)(a == 0);
            else if (bl == 0x34) st->IsCursorEmulationOff = a;
            else                 st->SequencerRegisters[1] = (BYTE)(a ? (st->SequencerRegisters[1] | 0x20u) : (st->SequencerRegisters[1] & ~0x20u));
            VddSetAx(r, (WORD)((VddGetAx(r) & 0xFF00) | 0x12));
        }
        else            { VddSetAx(r, (WORD)(VddGetAx(r) & 0xFF00)); }   /* not supported: AL=00h */
        break; }
    case 0x1A:                                         /* get display combination */
        VddSetAx(r, (WORD)((VddGetAx(r) & 0xFF00) | 0x1A));/* AL=1A: function present */
        VddSetBx(r, 0x0008);                               /* BL=08 active=VGA colour */
        break;
    case 0x1B: {                                       /* functionality/state info */
        BYTE *b = (BYTE *)VddMapFlat(st->Bus, r->Es, (WORD)r->Edi);
        UINT i;
        for (i = 0; i < 64; ++i) b[i] = 0;
        /* static functionality table kept inside the 64-byte block (reserved tail
           at 0x2E) so we never write past the caller's buffer. */
        VideoWrite32(b + 0, ((UINT32)r->Es << 16) | (((WORD)r->Edi + 0x2E) & 0xFFFF));
        b[4] = (BYTE)st->Mode;                      /* current mode            */
        VideoWrite16(b + 5, st->Columns);                         /* columns on screen       */
        b[0x22] = (BYTE)(st->Rows ? st->Rows : 25); /* character rows          */
        VideoWrite16(b + 0x23, st->CellHeight ? st->CellHeight : 16);  /* bytes per character     */
        b[0x25] = 0x08;                                /* active DCC = VGA colour */
        VideoWrite16(b + 0x27, 256);                           /* number of colours       */
        b[0x29] = 8;                                   /* number of pages         */
        b[0x2A] = 0;                                   /* scan lines (0 = 200)    */
        b[0x2B] = 0; b[0x2C] = 0; b[0x2D] = 0x21;      /* char blocks / misc      */
        /* static functionality table (16 bytes) -- modes 0..0x1F all supported   */
        b[0x2E + 0] = 0xFF; b[0x2E + 1] = 0xFF; b[0x2E + 2] = 0xFF; b[0x2E + 3] = 0xFF;
        b[0x2E + 7] = 0x07;                            /* scan lines 200/350/400  */
        b[0x2E + 8] = 8; b[0x2E + 9] = 8;              /* char blocks             */
        b[0x2E + 0x0A] = 0xFF; b[0x2E + 0x0B] = 0x07;  /* capability bits         */
        VddSetAx(r, (WORD)((VddGetAx(r) & 0xFF00) | 0x1B));/* AL=1B: supported        */
        break; }
    case 0x4F: VideoVesa(st, r); break;                     /* VESA VBE 2.0            */
    default:                                           /* unimplemented function  */
        VIDEO_UNIMPLEMENTED_SET(st->UnimplementedFunctions, ah);
        st->IsDirty = 0;
        break;
    }
    VddVideoBdaSync(st);                            /* the BDA follows every call */
}

/* DAC palette ports 3C7 (read index) / 3C8 (write index) / 3C9 (data). */
static VOID VideoDacOut(PVOID self, WORD port, BYTE w, UINT32 v)
{
    PVIDEO_STATE st = (PVIDEO_STATE)self; BYTE val = (BYTE)v; (VOID)w;
    if (port == 0x3C8) { st->DacWriteIndex = val; st->DacComponent = 0; }
    else if (port == 0x3C7) { st->DacReadIndex = val; st->DacComponent = 0; }
    else if (port == 0x3C9) {
        /* The byte as written; the width decides at the third primary what it means
           (6 bits: the low six, bits 6-7 ignored as the hardware ignores them; 8 bits:
           all of it). See VideoDacTo8 -- this used to mask to 6 bits whatever 4F08 said. */
        st->DacLatch[st->DacComponent++] = val;
        if (st->DacComponent >= 3) {
            st->Dac[st->DacWriteIndex] = VideoDacPackWidth(st, st->DacLatch[0], st->DacLatch[1], st->DacLatch[2]);
            st->DacBlock[(st->DacWriteIndex >> 4) & 15]++;
            if ((st->DacWriteIndex & 0xF0) == 0x30) st->DacHighSinceReset++;
            st->DacWriteIndex++; st->DacComponent = 0; st->DacWrites++;
            {   UINT64 now; UINT32 frame_us, vtotal, vdisp, vblank, frame_no, line; WORD row = 0xFFFE;
                if (VideoBeam(st, &now, &frame_us, &vtotal, &vdisp, &vblank, &frame_no, &line) && st->GraphicsHeight && vdisp)
                    row = (line >= vblank) ? 0xFFFF : (WORD)(((UINT64)line * st->GraphicsHeight) / vdisp);
                st->DacLastRow = row;
                st->DacRowHistogram[row == 0xFFFF ? 3 : row == 0xFFFE ? 0 : row < 2 ? 0 : row < 160 ? 1 : 2]++;
            }
            /* pal[] is DERIVED from dac[] -- see VideoPaletteRefresh. Without this a guest
               could reprogram the DAC and see nothing change, which is precisely the
               half of the Lemmings bug that survived the first fix. */
            VideoPaletteRefresh(st);
        }
    }
}
static VOID VideoDacIn(PVOID self, WORD port, BYTE w, UINT32 *v)
{
    PVIDEO_STATE st = (PVIDEO_STATE)self; UINT32 p; (VOID)w;
    if (port == 0x3C8) { *v = st->DacWriteIndex; return; }
    if (port != 0x3C9) { *v = 0xFF; return; }
    p = st->Dac[st->DacReadIndex];
    switch (st->DacComponent) {
    case 0: *v = VideoDacFrom8(st, (BYTE)(p >> 16)); break;   /* R, at the DAC width */
    case 1: *v = VideoDacFrom8(st, (BYTE)(p >> 8));  break;   /* G                   */
    default:*v = VideoDacFrom8(st, (BYTE)p); st->DacReadIndex++; break;   /* B, then advance */
    }
    if (++st->DacComponent >= 3) st->DacComponent = 0;
}

/* --- VGA planar write engine (mode 12h: Sequencer 3C4/5 + GC 3CE/F) ------- */
static BYTE VideoRotateRight(BYTE v, BYTE n)
{ n &= 7; return n ? (BYTE)((v >> n) | (v << (8 - n))) : v; }
static BYTE VideoAlu(BYTE op, BYTE v, BYTE lat)
{ switch (op & 3) { case 1: return (BYTE)(v & lat); case 2: return (BYTE)(v | lat);
                    case 3: return (BYTE)(v ^ lat); default: return v; } }

static VOID VideoPlanarWrite1(PVIDEO_STATE st, UINT32 off, BYTE cpu)
{
    BYTE alu = (BYTE)((st->FunctionRotate >> 3) & 3), bm = st->BitMask; INT p;
    if (off >= VIDEO_PLANE_SIZE) return;
    st->IsDirty = 1;
    switch (st->WriteMode & 3) {
    case 1:                                       /* copy latches -> planes        */
        for (p = 0; p < 4; ++p) if (st->MapMask & (1<<p)) VideoPlaneBytes(st,p)[off] = st->Latch[p];
        return;
    case 2:                                       /* CPU bit p -> plane p           */
        for (p = 0; p < 4; ++p) {
            BYTE val = (BYTE)((cpu & (1<<p)) ? 0xFF : 0x00);
            BYTE r = VideoAlu(alu, val, st->Latch[p]);
            r = (BYTE)((r & bm) | (st->Latch[p] & (BYTE)~bm));
            if (st->MapMask & (1<<p)) VideoPlaneBytes(st,p)[off] = r;
        }
        return;
    case 3: {                                     /* set/reset masked by rot(cpu)&bm */
        BYTE data = VideoRotateRight(cpu, st->FunctionRotate), mask = (BYTE)(data & bm);
        for (p = 0; p < 4; ++p) {
            BYTE val = (BYTE)((st->SetReset & (1<<p)) ? 0xFF : 0x00);
            BYTE r = (BYTE)((val & mask) | (st->Latch[p] & (BYTE)~mask));
            if (st->MapMask & (1<<p)) VideoPlaneBytes(st,p)[off] = r;
        }
        return; }
    default: {                                    /* write mode 0                   */
        BYTE data = VideoRotateRight(cpu, st->FunctionRotate);
        st->WriteEnableSetResetHistogram[st->EnableSetReset & 0x0F]++;
        st->WriteAluHistogram[alu & 3]++;
        for (p = 0; p < 4; ++p) {
            BYTE val = (st->EnableSetReset & (1<<p)) ? (BYTE)((st->SetReset & (1<<p)) ? 0xFF : 0x00) : data;
            BYTE r = VideoAlu(alu, val, st->Latch[p]);
            r = (BYTE)((r & bm) | (st->Latch[p] & (BYTE)~bm));
            if (p == 3 && (st->MapMask & 8)) {
                if (st->EnableSetReset & 8) { st->WritePlane3SetReset++; if (r) st->WritePlane3NonZero++; }
                else                     st->WritePlane3Data++;
            }
            if (st->MapMask & (1<<p)) VideoPlaneBytes(st,p)[off] = r;
        }
        return; }
    }
}

/* ── THE CACHE WITNESS. A linear scan over ten slots, entered only for accesses
     above VIDEO_CACHE_LOW, so its cost falls on nothing that draws the screen. It is
     deliberately NOT a hash: the whole point is that "this pc never appeared" must
     mean the pc never ran, and a hashed table cannot say that. See vdd_video.h. */
static VOID VideoCacheSiteNote(PVIDEO_STATE st, UINT32 off, INT wr)
{
    UINT32 pc, i;
    if (off < VIDEO_CACHE_LOW || !st->GuestPc) return;
    pc = st->GuestPc();
    st->CacheSequence++;
    for (i = 0; i < VIDEO_CACHE_SITES; ++i) {
        PVIDEO_CACHE_SITE c = &st->CacheSites[i];
        if (c->Count) {
            if (c->Pc != pc || c->IsWrite != (BYTE)wr) continue;
            if (off < c->Low) c->Low = off;
            if (off > c->High) c->High = off;
        } else {
            c->Pc = pc; c->IsWrite = (BYTE)wr; c->Low = c->High = off;
            c->First = st->CacheSequence;
        }
        c->Count++; c->Last = st->CacheSequence;
        return;
    }
    st->CacheSitesLost++;
}

/* The watchpoint wrapper. The engine above is left exactly as it was so that the
   instrument cannot change what it measures; this only records around it. */
VOID VddVideoPlanarWrite(PVIDEO_STATE st, UINT32 off, BYTE cpu)
{
    VIDEO_WATCH_RECORD r; INT p;
    if (off > st->PlanarHighWater) st->PlanarHighWater = off;
    if (st->GuestPc) {
        UINT32 pc = st->GuestPc();
        PVIDEO_SITE w = &st->WriteSites[VIDEO_SITE_HASH(pc)];
        if (!w->Count)            { w->Pc = pc; w->Low = w->High = off; w->Count = 1; }
        else if (w->Pc == pc) { if (off < w->Low) w->Low = off;
                                if (off > w->High) w->High = off; w->Count++; }
        else                  st->WriteSitesLost++;
    }
    VideoCacheSiteNote(st, off, 1);
    if (off != st->WatchOffset) { VideoPlanarWrite1(st, off, cpu); return; }
    r.Pc = st->GuestPc ? st->GuestPc() : 0;
    r.WriteMode = (BYTE)(st->WriteMode & 3); r.MapMask = st->MapMask;
    r.EnableSetReset = st->EnableSetReset; r.SetReset = st->SetReset;
    r.FunctionRotate = st->FunctionRotate; r.BitMask = st->BitMask; r.Cpu = cpu;
    for (p = 0; p < 4; ++p) r.Latch[p] = st->Latch[p];
    VideoPlanarWrite1(st, off, cpu);
    for (p = 0; p < 4; ++p)
        r.After[p] = (off < VIDEO_PLANE_SIZE) ? VideoPlaneBytes(st,p)[off] : 0;
    if (st->WatchCount < VIDEO_WATCH_MAX) st->Watch[st->WatchCount] = r;
    st->WatchLast = r;
    st->WatchCount++;
}

BYTE VddVideoPlanarRead(PVIDEO_STATE st, UINT32 off)
{
    INT p;
    if (off > st->PlanarHighWater) st->PlanarHighWater = off;
    if (st->GuestPc) {
        UINT32 pc = st->GuestPc();
        PVIDEO_SITE w = &st->ReadSites[VIDEO_SITE_HASH(pc)];
        if (!w->Count)            { w->Pc = pc; w->Low = w->High = off; w->Count = 1; }
        else if (w->Pc == pc) { if (off < w->Low) w->Low = off;
                                if (off > w->High) w->High = off; w->Count++; }
        else                  st->ReadSitesLost++;
        if (st->ReadMode & 1) {
            PVIDEO_SITE c = &st->CompareSites[VIDEO_SITE_HASH(pc)];
            if (!c->Count)            { c->Pc = pc; c->Low = c->High = off; c->Count = 1; }
            else if (c->Pc == pc) { if (off < c->Low) c->Low = off;
                                    if (off > c->High) c->High = off; c->Count++; }
            else                  st->CompareSitesLost++;
        }
    }
    VideoCacheSiteNote(st, off, 0);
    if (off >= VIDEO_PLANE_SIZE) return 0xFF;
    for (p = 0; p < 4; ++p) st->Latch[p] = VideoPlaneBytes(st,p)[off];   /* load latches    */
    st->ReadModeHistogram[st->ReadMode & 1]++;
    if (!(st->ReadMode & 1))
        return VideoPlaneBytes(st,st->ReadMap & 3)[off];                /* read mode 0     */
    /* ── READ MODE 1: COLOUR COMPARE. One bit per pixel, set where that pixel's
         colour matches GR2 in every plane GR7 selects. GR7 is "Color DON'T Care" and
         reads backwards: a SET bit means the plane DOES take part. With GR7 = 0 no
         plane is compared, so every pixel matches and the read is 0xFF -- which is the
         hardware's answer, not a failure, and worth not "fixing". */
    {   BYTE r = 0; INT b;
        for (b = 0; b < 8; ++b) {
            INT match = 1;
            for (p = 0; p < 4; ++p) {
                if (!((st->ColorDontCare >> p) & 1)) continue;
                if (((st->Latch[p] >> b) & 1) != ((st->ColorCompare >> p) & 1)) { match = 0; break; }
            }
            if (match) r = (BYTE)(r | (1 << b));
        }
        /* Record what we ANSWERED, not just that we were asked -- see vdd_video.h. */
        if (st->GuestPc) {
            UINT32 pc2 = st->GuestPc();
            UINT h = VIDEO_SITE_HASH(pc2);
            if (st->CompareSites[h].Pc == pc2) {
                if (!r)            st->CompareSitesZero[h]++;
                else if (r == 0xFF) st->CompareSitesOnes[h]++;
            }
        }
        return r; }
}

INT VddVideoIsPlanarActive(PCVIDEO_STATE st) { return st->ModeKind == VIDEO_KIND_PLANAR; }

/* CRT timings, shared by the 0x3DA status read and the present scheduler. */
#define VIDEO_VBL_HZ_HIGH     60        /* 640x480 modes                                */
#define VIDEO_VBL_HZ_LOW     70        /* 320x200 / 720x400 modes                      */
#define VIDEO_VTOTAL_HIGH    525        /* scanlines per frame incl. blanking, 480-line */
#define VIDEO_VTOTAL_LOW    449        /*                                    400-line  */
#define VIDEO_VACTIVE_HIGH   480
#define VIDEO_VACTIVE_LOW   400
#define VIDEO_HACTIVE_PERCENT   80        /* % of a scanline that is active (rest = hblank) */

/* See the header. Phase within the frame, in permille, against the point where the
   active picture ends (480/525 = 914, 400/449 = 891). The window is the last ~12%
   of the active period: late enough that a guest released by the PREVIOUS retrace
   has finished its drawing, early enough to be a distinct instant every frame. */
#define VIDEO_PRESENT_WINDOW_PER_MILLE 120
/* A poll this long after the previous one means the guest went away to draw. Its own
   draw is 1-4 ms (BOUNCEBX 2.4); a poll loop iterates in well under 50 us. */
#define VIDEO_PRESENT_GAP_US    400
static INT VideoVerticalTiming(PCVIDEO_STATE st, UINT32 *total, UINT32 *active,
                       UINT32 *blank_start);
/* The mode's frame period in microseconds -- the same 60/70 Hz choice the retrace
   model makes -- for the host's Auto fallback floor. 1/60 s when there is no clock. */
UINT32 VddVideoFrameUs(PCVIDEO_STATE st)
{
    UINT32 t, a, b, hz; INT tall = (st->GraphicsHeight > VIDEO_VACTIVE_LOW);
    if (VideoVesaGeometry(st, &t, &a, &b, &hz)) return 1000000u / hz;   /* #226: the VESA mode's */
    if (VideoVerticalTiming(st, &t, &a, &b)) tall = (t >= 500u);
    return 1000000u / (UINT32)(tall ? VIDEO_VBL_HZ_HIGH : VIDEO_VBL_HZ_LOW);
}
INT VddVideoIsPresentReady(PVIDEO_STATE st)
{
    UINT32 frame_us, pm, t, a, b, hz;
    INT act, tall;
    if (!st->TimeUs) return 1;                 /* no clock: present every tick   */
    if (VideoVesaGeometry(st, &t, &a, &b, &hz)) {      /* #226: the VESA mode's own frame */
        frame_us = 1000000u / hz; act = (INT)(a * 1000u / t);
        pm  = (UINT32)((st->TimeUs() % frame_us) * 1000u / frame_us);
        return (INT)pm >= act - VIDEO_PRESENT_WINDOW_PER_MILLE && (INT)pm < act;
    }
    /* Same geometry the 0x3DA read uses, and for the same reason: 914/891 permille
       are the two BIOS cases, and 640x350 is neither -- its picture ends at 350 of
       449 lines, 780 permille. Presenting at 891 there meant building the frame 1.6ms
       into the blanking interval rather than at the end of the picture. */
    tall = (st->GraphicsHeight > VIDEO_VACTIVE_LOW);
    act  = tall ? 914 : 891;
    if (VideoVerticalTiming(st, &t, &a, &b)) { tall = (t >= 500u); act = (INT)(a * 1000u / t); }
    frame_us = 1000000u / (UINT32)(tall ? VIDEO_VBL_HZ_HIGH : VIDEO_VBL_HZ_LOW);
    if (!frame_us) return 1;
    pm  = (UINT32)((st->TimeUs() % frame_us) * 1000u / frame_us);
    return (INT)pm >= act - VIDEO_PRESENT_WINDOW_PER_MILLE && (INT)pm < act;
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
#define VIDEO_MODEY_GAP_DEFAULT 2u

static VOID VideoModeYCopy(PVIDEO_STATE st, const INT *sel, INT nsel, UINT32 lo, UINT32 hi)
{
    UINT32 i;
    for (i = lo; i < hi; ++i) {
        BYTE b = st->VideoMemory[i];
        INT k;
        st->YShadow[i] = b;
        for (k = 0; k < nsel; ++k) st->YPlanes[sel[k]][i] = b;
    }
    st->YNonZero[0] += hi - lo;                              /* bytes attributed, for STAGE2 */
}

static VOID VideoModeYFlush(PVIDEO_STATE st)
{
    UINT32 i, run_lo = 0, run_hi = 0, gap = 0;
    const UINT32 *src32, *shd32;
    INT p, sel[4], nsel = 0, in_run = 0;
    if (st->IsChain4 || st->ModeKind != VIDEO_KIND_LINEAR8 || !st->VideoMemory) return;
    for (p = 0; p < 4; ++p) if (st->YMask & (1u << p)) sel[nsel++] = p;
    if (!nsel) return;
    src32 = (const UINT32 *)st->VideoMemory;
    shd32 = (const UINT32 *)st->YShadow;

    /* Dword-at-a-time scan. Most of the aperture is untouched between two adjacent
       mask changes -- and in Doom there are ~3,800 of those a second -- so the reject
       path is the one that has to be cheap. */
    for (i = 0; i < VIDEO_Y_PLANE_SIZE / 4; ++i) {
        if (src32[i] != shd32[i]) {
            if (!in_run) { in_run = 1; run_lo = i; }
            run_hi = i + 1; gap = 0;
        } else if (in_run && ++gap > st->ModeYGap) {
            VideoModeYCopy(st, sel, nsel, run_lo * 4u, run_hi * 4u);
            in_run = 0;
        }
    }
    if (in_run) VideoModeYCopy(st, sel, nsel, run_lo * 4u, run_hi * 4u);
    st->IsDirty = 1;
}

/* CRTC: only the registers unchained page-flipping needs. 0x0C/0x0D are the
   display START address (how a mode-Y program flips pages) and 0x13 the logical
   line width. Everything else is accepted and ignored -- this VDD does not model
   CRTC timing and pretending to would be worse than not. */
static VOID VideoIndexData(BYTE *index, BYTE w, UINT32 v,
                         VOID (*setdata)(PVOID , UINT32), PVOID ctx)
{
    *index = (BYTE)v;
    if (w == 2) setdata(ctx, (v >> 8) & 0xFF);
}

/* ── ★★★ A MODE SET PROGRAMS THE REGISTER FILE, AS A REAL BIOS DOES. ────────────────
     (docs/inventory/vga.md step 3.) Measured on genuine MS-DOS 6.22: a BIOS mode set
     leaves about SIXTY meaningful values across the Sequencer, CRTC, Graphics
     Controller, Attribute Controller and Miscellaneous Output. Ours left about five,
     so a guest that reads a register back to learn the geometry -- or saves and
     restores the card around its own mode switch, which plenty of DOS programs do --
     was told zero.
   ⚠ THIS FILLS THE FILE; IT DOES NOT YET DRIVE THE PICTURE. Every derived field
     (mkind, gw/gh, chain4, crtc_offset, map_mask ...) is still computed exactly as
     before and remains the authority for rendering. Moving the authority here is the
     NEXT step and is a separate commit, because it touches every rendering path.
   ⚠ ONLY MEASURED MODES. A mode absent from VGA_MODEDEFS is left exactly as it was
     rather than filled with a guess -- extend p_vgareg.asm and regenerate.
   ⚠ THE WRITE COUNTS ARE NOT TOUCHED. `*_w[i]` means "the GUEST wrote this index",
     and it is the evidence the inventory is built from; a BIOS load must not forge it.
     After this, `VGAREG` values are the BIOS's and the written-by-guest list is still
     only the guest's. */
static VOID VideoLoadModeDefinition(PVIDEO_STATE st, BYTE mode)
{
    INT i, k;
    for (k = 0; k < VGA_MODEDEFS_N; ++k) {
        const vga_modedef *d = &VGA_MODEDEFS[k];
        if (d->mode != mode) continue;
        st->IsGeometryRegistersOk = 1;                      /* #325: the file IS this mode */
        st->MiscOutput = d->misc;
        for (i = 0; i < 5;  ++i) st->SequencerRegisters[i]  = d->seq[i];
        for (i = 0; i < 25; ++i) st->CrtcRegisters[i] = d->crtc[i];
        for (i = 0; i < 9;  ++i) st->GcRegisters[i]   = d->gc[i];
        for (i = 0; i < 21; ++i) st->AttributeRegisters[i] = d->attr[i];
        /* ── ★★★★ AND THE LIVE SHADOWS, OR THE FILE AND THE AUTHORITY DISAGREE. ─────
             Loading the register FILE above is only half a mode set. Six of these
             registers are not read back from `*_reg[]` at all -- the read paths
             answer from a live shadow, because that shadow is what the rendering
             engine actually uses -- so the table set the file and the guest still
             saw the old value. MEASURED, on the rig and reproduced off-VM: after
             INT 10h mode 3 we answered SR2=0F (want 03), CR0A/0B=06/07 (want
             0D/0E), GR5=00 (want 10), AR10=00 (want 0C). Five registers, 55 of the
             VGA parity gap's 55 bytes, ONE defect.
           ⚠ ORDER MATTERS AND IT IS DELIBERATE. This runs BEFORE the per-kind arms
             below, so a mode that really does need its own value -- LINEAR8 and
             PLANAR both force map_mask 0x0F and drive ymap_select -- still wins.
             What changes is only the modes those arms say nothing about.
           ⚠ GR7 IS THE ONE THE ORACLES SPLIT ON, and the table already carries the
             answer: both say 0x0F in the graphics modes, and in the text/CGA modes
             QEMU says 0x0F where PCem's real IBM VGA says 0x00. Taking the table
             rather than a constant is what keeps that distinction. */
        st->MapMask     = (BYTE)(d->seq[2] & 0x0F);
        st->YMask       = st->MapMask;
        /* CR0A/CR0B ARE the cursor shape; `CursorShape` is that pair, not a copy of
           it. The BIOS leaves 0x0D0E for an 8x16 cell, and the 0x0607 set above is
           the 8-line CGA shape -- which VddCursorLines then RESCALES to lines
           14-15. So this moves the drawn cursor by one scan line, and stops
           AH=03h reporting a shape the card does not hold. */
        st->CursorShape    = (WORD)(((WORD)d->crtc[0x0A] << 8) | d->crtc[0x0B]);
        st->WriteMode   = (BYTE)(d->gc[5] & 3);
        st->ReadMode    = (BYTE)((d->gc[5] >> 3) & 1);
        st->ColorDontCare = (BYTE)(d->gc[7] & 0x0F);
        st->AttributeMode    = d->attr[0x10];
        st->IsBlink        = (BYTE)((d->attr[0x10] >> 3) & 1);
        return;
    }
}

/* ── #266: THE VIDEO PARAMETER TABLE, FROM THE SAME MEASURED REGISTER SETS THE MODE SET
     LOADS. A VGA BIOS programs a mode FROM this table, and publishes it through
     0040:00A8 -> Save Pointer table -> first far pointer; text utilities and mode
     switchers read the register values out of it rather than out of the card. So
     each entry here is built from VGA_MODEDEFS -- the bytes VideoLoadModeDefinition puts in
     the register file -- and can never disagree with what the mode set leaves.
     The 29 slots are IBM's (RBIL "Video Parameter Table"): 00h-03h modes 0-3 at 200
     lines, 04h-07h, 08h-0Ch PCjr/reserved, 0Dh/0Eh, 0Fh/10h modes 0Fh/10h on a 64 KB
     card, 11h/12h the same on 256 KB, 13h-16h modes 0-3 at 350 lines, 17h modes 0+/1+,
     18h 2+/3+, 19h 7+ (the 400-line 9-dot text modes), 1Ah-1Ch modes 11h-13h.
   ⚠ WHICH SLOTS ARE FILLED is SeaVGABIOS's choice (stdvga_build_video_param: 04h-07h,
     0Dh, 0Eh, 11h, 12h, 17h-1Ch), less 11h (mode 0Fh: we hold no measured set for it).
     The 200- and 350-line text slots and the 64 KB slots are ZERO, as there: we have no
     measurement of those register sets, and inventing them is the thing not to do.
     IBM's ROM fills them -- p_vid266 dumps slots 03h and 18h so the lead can see what
     a naive `mode * 64` lookup finds there on the real ROM. */
static const BYTE VideoParameterMode[VDD_VPARAM_N] = {
    0, 0, 0, 0, 0x04, 0x05, 0x06, 0x07,  0, 0, 0, 0, 0, 0x0D, 0x0E, 0,
    0, 0, 0x10, 0, 0, 0, 0, 0x01,  0x03, 0x07, 0x11, 0x12, 0x13
};
INT VddVideoParameterEntry(BYTE idx, BYTE out[64])
{
    INT i, k;
    UINT mi;
    BYTE mode;
    for (i = 0; i < 64; ++i) out[i] = 0;
    if (idx >= VDD_VPARAM_N || !(mode = VideoParameterMode[idx])) return 0;
    for (k = 0; k < VGA_MODEDEFS_N; ++k) {
        const vga_modedef *d = &VGA_MODEDEFS[k];
        if (d->mode != mode) continue;
        for (mi = 0; mi < sizeof(g_VideoModes)/sizeof(g_VideoModes[0]); ++mi)
            if (g_VideoModes[mi].Mode == mode) break;
        if (mi == sizeof(g_VideoModes)/sizeof(g_VideoModes[0])) return 0;
        {   UINT ps = VideoModePageSize(mode, g_VideoModes[mi].Kind,
                                         g_VideoModes[mi].Columns, g_VideoModes[mi].Rows);
            out[0x00] = g_VideoModes[mi].Columns;
            out[0x01] = (BYTE)(g_VideoModes[mi].Rows - 1);
            out[0x02] = VideoModeCellHeight(mode);
            out[0x03] = (BYTE)ps; out[0x04] = (BYTE)(ps >> 8); }
        for (i = 0; i < 4;  ++i) out[0x05 + i] = d->seq[1 + i];     /* SR1-SR4 */
        out[0x09] = d->misc;
        for (i = 0; i < 25; ++i) out[0x0A + i] = d->crtc[i];
        for (i = 0; i < 20; ++i) out[0x23 + i] = d->attr[i];        /* AR00-AR13 */
        for (i = 0; i < 9;  ++i) out[0x37 + i] = d->gc[i];
        return 1;
    }
    return 0;
}

static VOID VideoCrtcSetData(PVOID self, UINT32 v);

/* The cursor's CRTC address (0x0E/0x0F) as the BIOS path implies it: cells from the
   start of video memory, so the display start is added back in. */
static WORD VideoCrtcCursorOf(PCVIDEO_STATE st)
{ return (WORD)(st->CrtcStart + (UINT)st->CursorRow * st->Columns + st->CursorColumn); }
/* ...and the reverse: a guest wrote 0x0E/0x0F, so derive row/col from it. A value
   off the visible page (some guests park the cursor at 0x7FFF to hide it) is left
   where it is -- hidden by being out of range, as it is on the card. */
static VOID VideoCrtcCursorApply(PVIDEO_STATE st)
{
    UINT pos = st->CrtcCursor;
    UINT base = st->CrtcStart;
    st->IsDirty = 1;
    if (!st->Columns || !st->Rows) return;
    if (pos < base) { st->CursorRow = st->Rows; return; }          /* off-page: hidden  */
    pos -= base;
    if (pos >= (UINT)st->Columns * st->Rows) { st->CursorRow = st->Rows; return; }
    st->CursorRow = (BYTE)(pos / st->Columns);
    st->CursorColumn = (BYTE)(pos % st->Columns);
    VddVideoBdaSync(st);
}

/* Reassemble the ten-bit Line Compare from its three registers. */
static VOID VideoCrtcLineCompareUpdate(PVIDEO_STATE st)
{
    st->CrtcLineCompare = (WORD)(st->CrtcLineCompareLow
                            | (((WORD)(st->CrtcOverflow >> 4) & 1u) << 8)
                            | (((WORD)(st->CrtcMaxScan  >> 6) & 1u) << 9));
}

/* A mode set writes 0x06, 0x12 and 0x15 among the rest; only once all three have
   arrived is the vertical timing a complete statement rather than one register of
   the old mode's geometry beside two of the new one's. */
/* ── ▶ DERIVED ONCE PER CRTC WRITE, NOT ONCE PER 0x3DA READ. ──────────────────────
     The vertical geometry is a function of five registers. Recomputing it inside
     VideoStatusIn meant reassembling three 10-bit values out of scattered bits, running
     three validity tests and a division ON EVERY POLL -- and a guest polls this port
     harder than it does anything else: Lemmings reads 0x3DA 73.8 MILLION times in a
     45-second run, 1.6M/s, and in its menu phase it does essentially nothing else.
     MEASURED: that put the per-poll cost up from 16.0ns to 20.0ns off-VM, and on the
     rig the guest got through 77.0M polls per run before and 73.6M after -- 4.4% fewer
     in the same wall time, paid by every guest that waits on retrace.
     The inputs change only when the guest writes the CRTC, so the answer is cached
     there and the hot path just reads it. Same numbers, none of the arithmetic. */
static VOID VideoCrtcVerticalTimingRecompute(PVIDEO_STATE st)
{
    UINT32 ov, ms, vt, vde, vbs;
    st->IsVerticalTimingValid = 0;
    if (!st->IsCrtcVerticalTimingSeen) return;
    ov = st->CrtcOverflow; ms = st->CrtcMaxScan;
    vt  = (UINT32)st->CrtcVerticalTotalLow | ((ov >> 0 & 1u) << 8) | ((ov >> 5 & 1u) << 9);
    vde = (UINT32)st->CrtcVerticalDisplayEndLow    | ((ov >> 1 & 1u) << 8) | ((ov >> 6 & 1u) << 9);
    vbs = (UINT32)st->CrtcVerticalBlankStartLow    | ((ov >> 3 & 1u) << 8) | ((ms >> 5 & 1u) << 9);
    vt += 2; vde += 1;
    if (vt < 100u || vt > 1200u) return;
    if (vde == 0u || vde > vt)   return;
    if (vbs < vde || vbs >= vt)  return;
    st->VerticalTotal = (WORD)vt;
    st->VerticalActive = (WORD)vde;
    st->VerticalBlank  = (WORD)vbs;
    st->IsVerticalTimingValid  = 1;
}

static VOID VideoCrtcVerticalTimingUpdate(PVIDEO_STATE st)
{
    if (st->CrtcVerticalTotalLow && st->CrtcVerticalDisplayEndLow && st->CrtcVerticalBlankStartLow) st->IsCrtcVerticalTimingSeen = 1;
    VideoCrtcVerticalTimingRecompute(st);
}

/* ── THE CRT'S VERTICAL GEOMETRY, FROM THE REGISTERS THE GUEST WROTE. ──────────────
     Each of the three quantities is a 10-bit value split across three registers: the
     low byte of its own, plus one bit in Overflow (0x07) and -- for Blank Start only
     -- one in Maximum Scan Line (0x09). That scattering is why it is worth composing
     in one place instead of at each use.

       Vertical Total        0x06 + ov bit0 + ov bit5   (+2 = scanlines per frame)
       Vertical Display End  0x12 + ov bit1 + ov bit6   (+1 = active scanlines)
       Vertical Blank Start  0x15 + ov bit3 + maxscan bit5

   ▶ WHY NOT KEEP THE TWO CONSTANTS. They were right for every mode the BIOS sets
     except 0Fh/10h, and wrong there by 45 lines -- checked against the per-mode CRTC
     tables in vga_defaults.h, which were read back off a real card. A guest that
     programs its own timing (Mode X and friends) was never covered at all.
   ▶ Returns 0 and touches nothing when the guest has not programmed the CRTC, or
     when what it programmed is not a plausible screen: the caller then keeps the old
     constants. An off-VM test that sets gh directly takes this path, which is why
     the existing battery is unaffected. */
/* Read back what VideoCrtcVerticalTimingRecompute() worked out. The validity rules -- a plausible
   screen: blanking after the picture and inside the frame -- live there, because a
   half-written mode set must be rejected ONCE, not re-rejected 73 million times. */
static INT VideoVerticalTiming(PCVIDEO_STATE st, UINT32 *total, UINT32 *active,
                       UINT32 *blank_start)
{
    if (!st->IsVerticalTimingValid) return 0;
    *total = st->VerticalTotal; *active = st->VerticalActive; *blank_start = st->VerticalBlank;
    return 1;
}

static VOID VideoCrtcOut(PVOID self, WORD port, BYTE w, UINT32 v)
{
    PVIDEO_STATE st = (PVIDEO_STATE)self;
    /* 3B4 is the SAME index port as 3D4 (Misc Output bit 0 picks which one decodes);
       both are claimed, so both must be recognised as the index half or a mono guest's
       index write would be taken as data. */
    if (port == 0x3D4 || port == 0x3B4) { VideoIndexData(&st->CrtcIndex, w, v, VideoCrtcSetData, st); return; }
    VideoCrtcSetData(st, v);
}
/* ── #187: THE VERTICAL-RETRACE INTERRUPT LATCH, Input Status 0 bit 7. ──────────────
     IBM VGA: CR11 bit 5 = 0 ENABLES the vertical interrupt (active low), bit 4 = 0
     CLEARS it and holds it clear; with the interrupt enabled and not held, the start of
     vertical retrace sets the latch, which reads back as 3C2 bit 7. DOSBox-X does this;
     PCem's IBM VGA never sets the bit (p_vgaext is0.vsync) -- the spec outranks the
     oracle that leaves it out, so it is built.
   ⚠ THE LATCH ONLY. The card would also raise IRQ 2 (cascaded to 9), and we do NOT:
     that line is a jumper left open on most VGA cards, and every BIOS mode set writes
     CR11 with bit 5 = 0 -- the interrupt nominally ENABLED -- so raising it would fire an
     unexpected IRQ 9 into every graphics program. The BIOS modes also write bit 4 = 0,
     so an ordinary program still reads 0x10, as both oracles do.
   Evaluated lazily at the read, from the same beam clock as the 3DA status bits. */
static VOID VideoVintCr11(PVIDEO_STATE st, BYTE v)
{
    INT en = !(v & 0x20), hold_clear = !(v & 0x10);
    if (hold_clear) { st->IsVintPending = 0; st->IsVintArmed = 0; return; }
    if (!en)        { st->IsVintArmed = 0; return; }
    if (!st->IsVintArmed) {
        st->IsVintArmed = 1;
        st->VintArmTime = st->TimeUs ? st->TimeUs() : 0;
    }
}
static BYTE VideoVintStatus(PVIDEO_STATE st)
{
    UINT64 now, next, vbo;
    UINT32 F, vt, vd, vb, fno, line;
    if (st->IsVintArmed && !st->IsVintPending) {
        if (!VideoBeam(st, &now, &F, &vt, &vd, &vb, &fno, &line) || !F || !vt) {
            st->IsVintPending = 1;                       /* no clock: a retrace has passed */
        } else {
            (VOID)vd; (VOID)fno; (VOID)line;
            vbo  = (UINT64)vb * F / vt;             /* retrace start within a frame */
            next = (st->VintArmTime / F) * F + vbo;    /* this frame's retrace start   */
            if (next <= st->VintArmTime) next += F;    /* ...already gone: the next one */
            if (now >= next) st->IsVintPending = 1;
        }
    }
    return st->IsVintPending ? 0x80 : 0x00;
}

static VOID VideoCrtcSetData(PVOID self, UINT32 v)
{
    PVIDEO_STATE st = (PVIDEO_STATE)self;
    /* ── ⛔ CR11 BIT 7 WRITE-PROTECTS CR00..CR07, AND WE USED TO IGNORE IT. ─────────
         Measured against 6.22 by p_vgareg (`vga.cr11wp.protected.cr00`): with the bit
         set, hardware REFUSES a write to CR00 and the register still reads back 0x5F;
         we accepted it and read back 0x55. A guest that protects the timing registers
         and then writes them -- which is what the protect is FOR -- got a card that
         silently reprogrammed itself.
       ⚠ Refused means refused: not stored, not counted as a guest write, and not
         applied below. `CrtcWriteProtectRefused` counts them so the report can say it
         happened rather than leaving an absence to be interpreted. */
    if ((st->CrtcIndex & 31) <= 0x07 && (st->CrtcRegisters[0x11] & 0x80)) {
        st->CrtcWriteProtectRefused++;
        return;
    }
    /* CAPTURE FIRST, always, whatever the switch below does with it -- an index that
       falls into `default:` is exactly the one the inventory needs to hear about. */
    st->CrtcRegisters[st->CrtcIndex & 31] = (BYTE)v;
    st->CrtcWrites  [st->CrtcIndex & 31]++;
    switch (st->CrtcIndex & 31) {                 /* #325: a geometry register */
    case 0x01: case 0x07: case 0x09: case 0x12: case 0x17: st->IsGeometryRegistersOk = 1; break;
    default: break;
    }
    if ((st->CrtcIndex & 31) == 0x11) VideoVintCr11(st, (BYTE)v);   /* #187 */
    switch (st->CrtcIndex) {
    /* ── THE START ADDRESS IS SIXTEEN BITS WRITTEN AS TWO REGISTERS, so between the
         two writes it holds a value the guest never asked for -- half of the old
         address and half of the new. Real hardware survives that because the address
         counter LOADS FROM THESE REGISTERS AT THE VERTICAL RETRACE, not continuously,
         and a guest that flips pages during retrace is therefore never seen torn.
         We rendered from them directly, so a frame built between the two writes
         showed a garbage address -- a whole-screen flicker on any guest that scrolls
         or page-flips, which is every scrolling game. crtc_start_half counts how
         often a frame was built mid-pair; crtc_start_live is what the renderer uses. */
    case 0x0C: VideoLatch(st, 0);          /* boundaries passed BEFORE this write see the old value */
               st->CrtcStart = (WORD)((st->CrtcStart & 0x00FF) | ((WORD)(v & 0xFF) << 8));
               st->IsCrtcSeen = 1; st->IsCrtcStartPending ^= 1; st->IsDirty = 1; break;
    case 0x0D: VideoLatch(st, 0);
               st->CrtcStart = (WORD)((st->CrtcStart & 0xFF00) | (v & 0xFF));
               st->IsCrtcSeen = 1; st->IsCrtcStartPending ^= 1;
               if (!st->IsCrtcStartPending) {
                   st->CrtcStartWrites++;
                   if (st->TimeUs) {           /* pacing: frames since the last pair */
                       UINT64 n0; UINT32 F0, a0, b0, c0, fno, ln;
                       if (VideoBeam(st, &n0, &F0, &a0, &b0, &c0, &fno, &ln)) {
                           UINT32 g = st->StartPreviousFrame ? fno - st->StartPreviousFrame : 1u;
                           st->StartGapHistogram[g < 4u ? g : 4u]++;
                           st->StartPreviousFrame = fno;
                       }
                   }
               }
               st->IsDirty = 1; break;
    case 0x13: st->CrtcOffset = (BYTE)v; st->IsCrtcOffsetSeen = 1;                                   st->IsDirty = 1; break;
    /* Line Compare, and the two registers that carry its top two bits. */
    case 0x07: st->CrtcOverflow = (BYTE)v; VideoCrtcLineCompareUpdate(st); VideoCrtcVerticalTimingUpdate(st); st->IsDirty = 1; break;
    case 0x09: st->CrtcMaxScan  = (BYTE)v; VideoCrtcLineCompareUpdate(st); VideoCrtcVerticalTimingUpdate(st); st->IsDirty = 1; break;
    case 0x18: st->CrtcLineCompareLow   = (BYTE)v; VideoCrtcLineCompareUpdate(st); st->IsDirty = 1; break;
    /* Vertical timing -- see VideoVerticalTiming(). Low bytes only; 0x07/0x09 carry the
       high bits and are latched above for Line Compare already. */
    case 0x06: st->CrtcVerticalTotalLow = (BYTE)v; VideoCrtcVerticalTimingUpdate(st); st->IsDirty = 1; break;
    case 0x12: st->CrtcVerticalDisplayEndLow    = (BYTE)v; VideoCrtcVerticalTimingUpdate(st); st->IsDirty = 1; break;
    case 0x15: st->CrtcVerticalBlankStartLow    = (BYTE)v; VideoCrtcVerticalTimingUpdate(st); st->IsDirty = 1; break;
    /* ── THE TEXT CURSOR, PROGRAMMED DIRECTLY. ────────────────────────────────────
         0x0A/0x0B are Cursor Start/End (the same CH/CL INT 10h AH=01h takes, bit 5
         of Start = off) and 0x0E/0x0F the cursor's address in character cells from
         the start of video memory. Full-screen editors and every CRT unit (Turbo
         Pascal's, QB's runtime) position the cursor this way instead of through the
         BIOS, and these registers fell into `default:` -- so the cursor sat wherever
         the last INT 10h left it, which in an editor is nowhere near the text. */
    case 0x0A: st->CursorShape = (WORD)((st->CursorShape & 0x00FF) | ((WORD)(v & 0x3F) << 8));
               st->IsDirty = 1; VddVideoBdaSync(st); break;
    case 0x0B: st->CursorShape = (WORD)((st->CursorShape & 0xFF00) | (v & 0x1F));
               st->IsDirty = 1; VddVideoBdaSync(st); break;
    case 0x0E: st->CrtcCursor = (WORD)((st->CrtcCursor & 0x00FF) | ((WORD)(v & 0xFF) << 8));
               VideoCrtcCursorApply(st); break;
    case 0x0F: st->CrtcCursor = (WORD)((st->CrtcCursor & 0xFF00) | (v & 0xFF));
               VideoCrtcCursorApply(st); break;
    default: break;
    }
}
static VOID VideoCrtcIn(PVOID self, WORD port, BYTE w, UINT32 *v)
{
    PVIDEO_STATE st = (PVIDEO_STATE)self; (VOID)w;
    if (port == 0x3D4 || port == 0x3B4) { *v = st->CrtcIndex; return; }
    switch (st->CrtcIndex) {
    case 0x0C: *v = (BYTE)(st->CrtcStart >> 8); break;
    case 0x0D: *v = (BYTE)(st->CrtcStart & 0xFF); break;
    case 0x13: *v = st->CrtcOffset; break;
    case 0x0A: *v = (BYTE)((st->CursorShape >> 8) & 0x3F); break;
    case 0x0B: *v = (BYTE)(st->CursorShape & 0x1F); break;
    /* Read back what the BIOS path set, in the hardware's own units -- in TEXT modes.
       ► In a graphics mode the BIOS never writes CR0E/CR0F (there is no hardware
         cursor to place), so they hold what the mode set loaded, or what the guest
         wrote: plain storage. Deriving them there leaked the text cursor into Mode X
         (`modeX.320x240` CR0F: hardware 0x00, ours 0xA0 -- the last 1/690 VGA parity
         byte; s81, #186). The text-mode cursor paths are untouched. */
    case 0x0E: *v = st->ModeKind == VIDEO_KIND_TEXT ? (BYTE)(VideoCrtcCursorOf(st) >> 8)
                                               : st->CrtcRegisters[0x0E]; break;
    case 0x0F: *v = st->ModeKind == VIDEO_KIND_TEXT ? (BYTE)(VideoCrtcCursorOf(st) & 0xFF)
                                               : st->CrtcRegisters[0x0F]; break;
    default:   *v = st->CrtcRegisters[st->CrtcIndex & 31]; break;  /* CR00-05/11/17 read back */
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
static VOID VideoSequencerSetData(PVOID self, UINT32 v);

static VOID VideoSequencerOut(PVOID self, WORD port, BYTE w, UINT32 v)
{
    PVIDEO_STATE st = (PVIDEO_STATE)self;
    if (port == 0x3C4) { VideoIndexData(&st->SequencerIndex, w, v, VideoSequencerSetData, st); return; }
    VideoSequencerSetData(st, v);
}
static VOID VideoSequencerSetData(PVOID self, UINT32 v)
{
    PVIDEO_STATE st = (PVIDEO_STATE)self;
    if ((st->SequencerIndex & 7) == 1 && ((st->SequencerRegisters[1] ^ (BYTE)v) & 0x20u))
        st->IsDirty = 1;                             /* SR1.5 screen off/on: re-present (#266) */
    st->SequencerRegisters[st->SequencerIndex & 7] = (BYTE)v;
    st->SequencerWrites  [st->SequencerIndex & 7]++;
    if (st->SequencerIndex == 2) {
        /* Which map-mask values does this program actually use, and how often? The
           de-interleave is built entirely on the assumption that an unchained program
           selects ONE plane at a time and changes the mask between planes; nothing has
           ever checked that against a real one. A 16-entry histogram costs nothing and
           turns "the frame comes out doubled" into "plane 1 was never selected". */
        st->MaskHistogram[v & 0x0F]++;
        /* ► THE PAIR, NOT THE TWO HISTOGRAMS SEPARATELY. "write mode 1 happens 120
             times" and "mask 0x0F happens 44 times" cannot be combined by the reader:
             a latch copy through a SINGLE-plane mask is served correctly by per-plane
             backing, one through an ALL-plane mask is not, and only the pairing says
             which Doom actually does. */
        st->ModeMaskHistogram[(st->WriteMode & 3) * 16 + (v & 0x0F)]++;
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
        if (st->IsChain4)                                   st->MaskSkipChain4++;
        else if ((BYTE)(v & 0x0F) == st->YMask)       st->MaskSkipSame++;
        if (!st->IsChain4) {
            /* ⚠ WITH HOST BACKING, CALL ON EVERY WRITE -- NOT ONLY ON A CHANGE. Once the
                 host follows GR4 (the read plane) the window can have MOVED since the
                 last map-mask write, so "the mask value is unchanged" no longer implies
                 "the window is where the writes need it". The host early-returns when it
                 already is, so the extra calls cost a compare; the alternative is a
                 store landing in the plane the last READ selected.
                 The fallback de-interleave path has no such window and keeps the skip. */
            if (st->YMapSelect)                        st->YMapSelect(st->YMapContext, (INT)(v & 0x0F));
            else if ((BYTE)(v & 0x0F) != st->YMask) VideoModeYFlush(st);
            if ((BYTE)(v & 0x0F) != st->YMask)      st->IsDirty = 1;
        }
        st->MapMask = (BYTE)(v & 0x0F);
        st->YMask   = st->MapMask;
    }
    else if (st->SequencerIndex == 4) {                 /* Memory Mode: bit 3 = Chain-4 */
        BYTE c4 = (BYTE)((v >> 3) & 1);
        if (c4 != st->IsChain4) {
            /* ── #184: THE SAME BYTES, TWO ADDRESSINGS. A chain-4 store at CPU address A
                 lands in plane A&3 at plane offset A&~3 (docs/ref/vga.md §8) -- so a program
                 that draws chained and then unchains (Doom, and p_vgamem's chain4.abcd)
                 must find those bytes spread across the planes. Here the chained view is the
                 linear aperture and the unchained one is the four plane sections, so the
                 switch has to MOVE them: scatter on the way out of chain-4, gather on the way
                 back in. Snapshot first -- the aperture changes meaning at the remap, and a
                 host may back the linear view with plane 0 itself. */
            static BYTE xfer[VIDEO_Y_PLANE_SIZE];
            UINT32 a;
            INT linear = (st->ModeKind == VIDEO_KIND_LINEAR8 && st->VideoMemory);
            if (linear && !c4) for (a = 0; a < VIDEO_Y_PLANE_SIZE; ++a) xfer[a] = st->VideoMemory[a];
            if (linear && c4)
                for (a = 0; a < VIDEO_Y_PLANE_SIZE; ++a) xfer[a] = VideoPlaneBytes(st, (INT)(a & 3))[a & ~3u];
            st->IsChain4 = c4; st->YMask = st->MapMask;
            st->Chain4Selects++;
            if (st->YMapSelect) st->YMapSelect(st->YMapContext, c4 ? -1 : (INT)st->MapMask);
            if (linear && !c4) {
                for (a = 0; a < VIDEO_Y_PLANE_SIZE; ++a) VideoPlaneBytes(st, (INT)(a & 3))[a & ~3u] = xfer[a];
                if (!st->YMapPlane)                      /* the no-host fallback's copy */
                    for (a = 0; a < VIDEO_Y_PLANE_SIZE; ++a) st->YPlanes[a & 3][a & ~3u] = xfer[a];
                ++st->Chain4Transfers;
            }
            if (linear && c4) {
                for (a = 0; a < VIDEO_Y_PLANE_SIZE; ++a) st->VideoMemory[a] = xfer[a];
                ++st->Chain4Transfers;
            }
            st->IsDirty = 1;
        }
    }
}
static VOID VideoSequencerIn(PVOID self, WORD port, BYTE w, UINT32 *v)
{
    PVIDEO_STATE st = (PVIDEO_STATE)self; (VOID)w;
    /* ⚠ USED TO RETURN 0 FOR EVERY INDEX BUT 2. A VGA reads every Sequencer register
       back, and a guest that probes the card by writing and re-reading one got a zero
       that says "no card here". The register file answers properly now; index 2 still
       comes from map_mask, which is the live authority for it. */
    if (port == 0x3C4) { *v = st->SequencerIndex; return; }
    *v = (st->SequencerIndex == 2) ? st->MapMask : st->SequencerRegisters[st->SequencerIndex & 7];
}
/* Graphics Controller ports 3CE (index) / 3CF (data). */
static VOID VideoGcSetData(PVOID self, UINT32 v);

static VOID VideoGcOut(PVOID self, WORD port, BYTE w, UINT32 v)
{
    PVIDEO_STATE st = (PVIDEO_STATE)self;
    if (port == 0x3CE) { VideoIndexData(&st->GcIndex, w, v, VideoGcSetData, st); return; }
    VideoGcSetData(st, v);
}
static VOID VideoGcSetData(PVOID self, UINT32 v)
{
    PVIDEO_STATE st = (PVIDEO_STATE)self;
    st->GcRegisters[st->GcIndex & 15] = (BYTE)v;
    st->GcWrites  [st->GcIndex & 15]++;
    switch (st->GcIndex) {
    case 0: st->SetReset   = (BYTE)(v & 0x0F); break;
    case 1: st->EnableSetReset   = (BYTE)(v & 0x0F); break;
    /* GR2 and GR7 are the two halves of read mode 1 and used to fall into default:,
       i.e. be dropped. See read_mode in the header. */
    case 2: st->ColorCompare  = (BYTE)(v & 0x0F); break;
    case 7: st->ColorDontCare = (BYTE)(v & 0x0F); break;
    case 3: st->FunctionRotate = (BYTE)(v & 0x1F); break;
    /* ── GR4 IS THE READ PLANE, AND THE REMAP PATH CANNOT SEE READS AT ALL. ──────────
         In the `st->Planes[]` interpreter path a guest read is served by us and honours
         this register (see the read-mode-0 return). With host-supplied per-plane backing
         A0000 is a REAL mapped section, so a guest read never reaches this file and
         returns whatever plane the WRITE MASK last selected. Read plane and write plane
         are independent on the hardware, so any guest that sets them apart -- Doom's
         `I_ReadScreen` cycles GR4 with the write mask irrelevant -- gets the wrong bytes,
         SILENTLY. Every exclusion so far in the status-bar hunt has been about writes.
       ► Count the pairing, not the register. `Gr4Histogram` alone cannot say whether GR4
         ever DISAGREED with the mapped plane, and disagreement is the entire defect;
         the host compares against `g_ycur` in the hook. */
    case 4:
        st->ReadMap = (BYTE)(v & 3);
        st->Gr4Histogram[v & 3]++;
        if (st->YMapReadMap) st->YMapReadMap(st->YMapContext, (INT)(v & 3));
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
        st->WriteModeHistogram[v & 3]++;
        st->WriteMode  = (BYTE)(v & 3);
        st->ReadMode   = (BYTE)((v >> 3) & 1);   /* ⚠ bit 3 used to be masked off */
        if (st->YMapWriteMode) st->YMapWriteMode(st->YMapContext, (INT)(v & 3));
        break;
    case 8: st->BitMask    = (BYTE)v;          break;
    default: break;
    }
}
static VOID VideoGcIn(PVOID self, WORD port, BYTE w, UINT32 *v)
{
    PVIDEO_STATE st = (PVIDEO_STATE)self; (VOID)w;
    if (port == 0x3CE) { *v = st->GcIndex; return; }
    switch (st->GcIndex) {
    case 0: *v = st->SetReset; break;  case 1: *v = st->EnableSetReset; break;
    case 2: *v = st->ColorCompare; break; case 7: *v = st->ColorDontCare; break;
    case 3: *v = st->FunctionRotate; break; case 4: *v = st->ReadMap; break;
    /* ⚠ GR5 IS NOT ONLY THE TWO MODE FIELDS. Bits 0:1 are the write mode and bit 3
         the read mode, and those are shadowed because the engine uses them -- but
         bit 2 (test), bit 4 (odd/even), bit 5 (shift register) and bit 6 (256-colour
         shift) are not modelled, and returning only the shadows reported 0x00 where
         a real BIOS leaves 0x10 in mode 3 and 0x40 in 13h. Merge: the shadows for
         what we model, the stored byte for what we do not. */
    case 5: *v = (BYTE)((st->GcRegisters[5] & 0x74)
                           | st->WriteMode | (st->ReadMode << 3)); break;
    case 8: *v = st->BitMask; break;
    default: *v = st->GcRegisters[st->GcIndex & 15]; break;   /* GR6 and the rest read back */
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
     `st->TimeUs` is NULL off-VM by default, which keeps the old toggle for tests
     that do not care about timing.
   ▶ The retrace bit is asserted for the whole VERTICAL BLANKING interval rather
     than just the 2-line sync pulse. A 2-line window is 0.4% of a frame, and a
     guest that does any work between polls would miss it and wait an extra frame;
     the blanking interval (~9%) is the forgiving reading and is what emulators
     conventionally report. */
/* ── WHERE IS THE BEAM? One answer for everyone who asks. ──────────────────────
     VideoStatusIn derived the frame timing inline; the raster-split bookkeeping needs
     the same numbers at every DAC write, so it lives here. Returns 0 with no clock
     (the off-VM default). `now` is the model's microseconds; `frame_us` the frame
     period; `vtotal`/`vdisp`/`vblank` the line counts (total, display end, blank
     start); `frame_no` = now / frame_us; `line` = the scanline the beam is on. */
static INT VideoBeam(PCVIDEO_STATE st, UINT64 *now, UINT32 *frame_us,
                    UINT32 *vtotal, UINT32 *vdisp, UINT32 *vblank,
                    UINT32 *frame_no, UINT32 *line)
{
    INT tall; UINT32 t, a, b, hz; UINT64 in_frame;
    if (!st->TimeUs) return 0;
    /* A VESA graphics mode runs on its own timing, not the last VGA mode's (#226). */
    if (VideoVesaGeometry(st, vtotal, vdisp, vblank, &hz)) {
        *frame_us = 1000000u / hz;
        *now      = st->TimeUs();
        *frame_no = (UINT32)(*now / *frame_us);
        in_frame  = *now % (UINT64)*frame_us;
        *line     = (UINT32)((in_frame * (UINT64)*vtotal) / *frame_us);
        return 1;
    }
    /* 480-line modes run at 60 Hz, the 200/400-line ones at 70 Hz. Mode 13h is
       320x200 displayed as 400 scanlines, so it belongs with the 70 Hz group -- key
       the choice off the DISPLAYED height, not the mode number. */
    tall    = (st->GraphicsHeight > VIDEO_VACTIVE_LOW);
    *vtotal = tall ? VIDEO_VTOTAL_HIGH  : VIDEO_VTOTAL_LOW;
    *vblank = tall ? VIDEO_VACTIVE_HIGH : VIDEO_VACTIVE_LOW;
    *vdisp  = *vblank;
    /* Prefer the geometry the guest programmed. `vblank` is BLANK START, not display
       end -- they differ by 6 lines in the BIOS modes and by 45 in 640x350, and it is
       blanking, not the end of the picture, that raises the status bit. */
    if (VideoVerticalTiming(st, &t, &a, &b)) {
        *vtotal = t; *vdisp = a; *vblank = b;
        /* 449-line modes are the 70 Hz family, 525-line the 60 Hz one. Keyed off the
           measured total rather than the displayed height, so a 350-line mode is no
           longer forced to pick a side of a 400-line fence. */
        tall = (t >= 500u);
    }
    *frame_us = 1000000u / (UINT32)(tall ? VIDEO_VBL_HZ_HIGH : VIDEO_VBL_HZ_LOW);
    *now      = st->TimeUs();
    *frame_no = (UINT32)(*now / *frame_us);
    in_frame  = *now % (UINT64)*frame_us;
    /* SCALE FIRST, DIVIDE ONCE -- see VideoStatusIn for why line_us must not be an integer. */
    *line     = (UINT32)((in_frame * (UINT64)*vtotal) / *frame_us);
    return 1;
}

/* ── ★ THE DISPLAYED FRAME'S START ADDRESS AND PEL PANNING, ON THE HARDWARE'S SCHEDULE. ──
     (s83, Mario vs Windows 98 and stock NTVDM: "butter smooth" there, "jagged, jarring"
     here.) Mario's scroll routine, from its binary:
         cli / wait 3DA bit 3 CLEAR           ; in the picture
         out 3D4: 0Ch,0Dh = y*90 + x/4        ; coarse position, whole bytes
         wait 3DA bit 3 SET                   ; retrace
         out 3C0: 33h, (x*2) & 7              ; AR13 pel panning: the 0-3 pixel remainder
     On a VGA the address counter LOADS the start address at the start of vertical
     retrace, and the panning written inside that retrace applies as the next picture
     begins -- so the pair lands together on the next frame. We took the start address
     whenever the host happened to draw and ignored AR13 altogether: the view moved in
     4-pixel jumps and, drawn at the wrong moment, a frame out of step with its pan.
   ► THE RULE, computed from the same beam clock the 3DA status read uses (so a guest
     that saw bit 3 set is past the latch point, and one that saw it clear is before it):
       * at each retrace start (the blanking line VideoBeam reports) start_vs := register;
       * at each frame start (line 0) the display takes start_vs and the current AR13.
     Called BEFORE every write to 0Ch/0Dh/AR13 and when a frame is built, so the register
     values between two calls are constant and "the value at that boundary" is simply
     the value now. No clock (off-VM) or a long gap: take everything as it stands. */
static VOID VideoLatch(PVIDEO_STATE st, INT at_frame)
{
    UINT64 now, t0, ds, vbo;
    UINT32 F, vt, vd, vb, fno, line;
    if (!VideoBeam(st, &now, &F, &vt, &vd, &vb, &fno, &line) || !F || !vt) {
        /* No beam clock: the frame build IS the retrace, as before s83. */
        if (!at_frame) return;
        if (st->IsCrtcStartPending) st->CrtcStartHalf++;
        st->StartVs = st->CrtcStartLive = (WORD)st->CrtcStart;
        st->DisplayPan = st->AttributeRegisters[0x13];
        st->VesaOriginVs = st->VesaOriginLive = st->VesaOrigin;
        return;
    }
    (VOID)vd; (VOID)fno; (VOID)line;
    t0 = st->LatchTime; st->LatchTime = now;
    if (!t0 || now < t0 || now - t0 > 4u * (UINT64)F) {
        st->StartVs = st->CrtcStartLive = (WORD)st->CrtcStart;
        st->DisplayPan = st->AttributeRegisters[0x13];
        st->VesaOriginVs = st->VesaOriginLive = st->VesaOrigin;
        return;
    }
    /* ► THE VESA DISPLAY START RIDES THE SAME SCHEDULE (#226): a VBE BIOS implements
         4F07h by writing the CRTC start (plus its extension bits), so it loads at the
         retrace start and shows from the next picture exactly as 0Ch/0Dh do. */
    vbo = (UINT64)vb * F / vt;                    /* retrace start, within the frame */
    ds  = (now / F) * F;                            /* the last frame start <= now     */
    if (ds > t0) {                                  /* a new picture began since t0    */
        if (ds >= F && ds - F + vbo > t0) {         /* ...and its retrace was after t0 */
            if (st->IsCrtcStartPending) st->CrtcStartHalf++;   /* loaded mid-pair: torn */
            st->StartVs = (WORD)st->CrtcStart;
            st->VesaOriginVs = st->VesaOrigin;
        }
        st->CrtcStartLive = st->StartVs;
        st->DisplayPan        = st->AttributeRegisters[0x13];
        st->VesaOriginLive   = st->VesaOriginVs;
    }
    if (ds + vbo <= now && ds + vbo > t0) {         /* this frame's retrace began      */
        if (st->IsCrtcStartPending) st->CrtcStartHalf++;
        st->StartVs = (WORD)st->CrtcStart;
        st->VesaOriginVs = st->VesaOrigin;
    }
}

/* ── #226: WHEN MAY A 4F07h BL=80h/82h CALL RETURN? ────────────────────────────────────
     "Set Display Start during Vertical Retrace" (VBE 2.0 §4.10; 3.0 adds 82h, which
     "schedule[s] the display start address change to occur, and then wait[s] until the
     address has changed before returning"). The rule, on the beam clock 3DAh uses:
       * the beam is IN a retrace that has not yet released such a call: return now, and
         the start is the one this retrace loads (it shows from the next picture);
       * otherwise wait for the NEXT retrace start -- which also means a guest that calls
         twice inside one retrace is paced to one flip per frame, as the Bochs/SeaVGABIOS
         style `wait while in retrace; wait until in retrace` loop would pace it.
     Returns the model time the call completes at (0 = now) and says whether this call's
     start should be taken by the CURRENT retrace (1) or left to the latch (0). */
static UINT64 VideoVesaVblRelease(PVIDEO_STATE st, INT *now_in_vbl)
{
    UINT64 now, vbo, ds, vstart;
    UINT32 F, vt, vd, vb, fno, line;
    *now_in_vbl = 0;
    if (!VideoBeam(st, &now, &F, &vt, &vd, &vb, &fno, &line) || !F || !vt) return 0;
    (VOID)vd; (VOID)line;
    vbo    = (UINT64)vb * F / vt;
    ds     = (now / F) * F;
    vstart = ds + vbo;                              /* this frame's retrace start   */
    if (now >= vstart) {                            /* in retrace now               */
        if (st->Vesa07Vbl != fno + 1u) { st->Vesa07Vbl = fno + 1u; *now_in_vbl = 1; return 0; }
        st->Vesa07Vbl = fno + 2u;                 /* taken: the next one          */
        return vstart + F;
    }
    st->Vesa07Vbl = fno + 1u;
    return vstart;
}

UINT32 VddVideoInt10WaitUs(PVIDEO_STATE st)
{
    UINT64 now, until = st->Int10WaitUntil;
    if (!until) return 0;
    if (!st->TimeUs) { st->Int10WaitUntil = 0; return 0; }
    now = st->TimeUs();
    /* Done, or a stamp more than a second out (a clock that went backwards, a stale
       value): never park the guest on it. */
    if (now >= until || until - now > 1000000u) { st->Int10WaitUntil = 0; return 0; }
    return (UINT32)(until - now);
}

/* ── ★ THE EXTERNAL REGISTERS -- CLAIMED AT LAST. (docs/inventory/vga.md, step 1) ──
     3C2, 3C3, 3C6, 3CA and 3CC were claimed by NOBODY: a guest's write vanished and a
     read came back 0xFF from the bus's absent-device default. The inventory calls
     Miscellaneous Output the worst of them, because bits 6-7 are the sync polarities
     -- which is how a VGA encodes 400- vs 350- vs 480-line vertical size -- and bits
     2-3 the dot clock. A guest setting Mode X writes 0xE3 here, and we never saw it.
   ⚠ CAPTURE ONLY: these are stored and reported, and NOTHING derives geometry from
     them yet. In particular Misc Output bit 0 (CRTC at 3Bx vs 3Dx) is recorded and not
     acted on -- acting on it is step 3, with the whole shelf as the regression set.
   ⚠ READS CHANGE, AND THAT IS THE POINT. Every one of these ports used to answer 0xFF
     (not a value any VGA returns); now they answer what was written, or the documented
     power-on value. The one exception is deliberate: the DAC Pixel Mask resets to 0xFF,
     which is BOTH the hardware default and what the port used to return by accident. */
static VOID VideoExternalOut(PVOID self, WORD port, BYTE w, UINT32 v)
{
    PVIDEO_STATE st = (PVIDEO_STATE)self; (VOID)w;
    if (port == 0x3C2)      { st->MiscOutput   = (BYTE)v; st->MiscWrites++;  }
    else if (port == 0x3C3) { st->VgaEnable = (BYTE)(v & 1); st->VgaEnableWrites++; }
    else if (port == 0x3C6) { st->DacMask   = (BYTE)v; st->DacMaskWrites++; }
    /* 3CA and 3CC are READ-ONLY aliases (Feature Control / Misc Output); a write
       there is a guest bug on real hardware too, so it is dropped, not stored. */
}

static VOID VideoExternalIn(PVOID self, WORD port, BYTE w, UINT32 *v)
{
    PVIDEO_STATE st = (PVIDEO_STATE)self; (VOID)w;
    switch (port) {
    /* ── ★★★ INPUT STATUS 0 = 0x10. MEASURED, after three sessions of 0x00. ────
         Bit 4 is Switch Sense, the DAC comparator a real card drives; bit 7 is
         "vertical retrace interrupt pending", which we never raise. This used to
         answer 0x00 and say so -- flagged UNVERIFIED here and in the STAGE2 dump,
         deliberately not guessed at, pending an oracle with a real video BIOS.
       ★ PCem WITH A GENUINE IBM VGA ROM SAYS 0x10, IN ALL TWELVE MODES p_vgareg
         sets -- text, planar, 13h, Mode X and mono alike. dosbox-x also drives bit
         4 (0x70 in mode 3, 0x60 in 13h, so it varies there); only QEMU answers
         0x00, and QEMU is the host that has been wrong on every external-register
         row this project has checked. Two of three set bit 4, and the one that
         matters is the one running period-correct firmware.
       ⚠ A CONSTANT IS THE RIGHT SHAPE FOR US even though the real bit is a
         comparator: the sense line reports the monitor, and ours is fixed. What
         was wrong was the value, not the constancy.
       ✅ #187 (s84): BIT 7 IS NOW BUILT -- see VideoVintCr11. What follows is why it was
         once left out, kept because the reasoning about the oracles still holds.
       ⛔ BIT 7 STAYED 0, AND THAT WAS A RECORDED GAP, NOT AN OVERSIGHT. p_vgaext's
         is0.vsync case enables the vertical-retrace interrupt in CR11 and looks:
         dosbox-x sets bit 7 and clears it through CR11 bit 4; PCem's IBM VGA never
         sets it at all. So the oracles split 1-2 AGAINST the feature, the IBM VGA
         spec is for it, and no DOS guest this project has met uses it -- the VGA
         vertical interrupt is famously unreliable and IBM's own documentation
         steers software away from it. Implementing it would be a guest-visible
         change with no guest to check it against. See docs/inventory/vga.md. */
    case 0x3C2: *v = 0x10u | VideoVintStatus(st); break;   /* bit 7: #187, see VideoVintCr11 */
    case 0x3C3: *v = st->VgaEnable; break;
    case 0x3C6: *v = st->DacMask;   break;
    case 0x3CA: *v = st->FeatureControl;  break;   /* Feature Control read  */
    case 0x3CC: *v = st->MiscOutput;   break;   /* Misc Output read      */
    default:    *v = 0xFF; break;             /* 3CB: nothing decodes there */
    }
}

/* Feature Control WRITE lives at 3BA/3DA -- the same port whose READ is Input Status 1.
   It was thrown away here; now it is captured, for the same reason as the rest. */
static VOID VideoStatusOut(PVOID self, WORD port, BYTE w, UINT32 v)
{
    PVIDEO_STATE st = (PVIDEO_STATE)self; (VOID)port; (VOID)w;
    st->FeatureControl = (BYTE)v; st->FeatureWrites++;
}
#define VIDEO_HBL_DEBT_MAX 16u   /* lines: further apart than this, a poll is not counting lines */
/* #183: how long until 3DAh bit 3 (vertical retrace: blank start to frame end) READS
   `want_set`? 0 = it already does. UINT32_MAX = no beam clock to say. The host uses this
   to sleep through a retrace-wait loop instead of trapping on every iteration. */
UINT32 VddVideoUsToRetrace(PVIDEO_STATE st, INT want_set)
{
    UINT64 now, in_frame, vbo;
    UINT32 F, vt, vd, vb, fno, line;
    if (!VideoBeam(st, &now, &F, &vt, &vd, &vb, &fno, &line) || !F || !vt) return 0xFFFFFFFFu;
    (VOID)vd; (VOID)fno; (VOID)line;
    in_frame = now % (UINT64)F;
    vbo = (UINT64)vb * F / vt;                      /* retrace (bit 3) starts here */
    if (want_set) return in_frame >= vbo ? 0u : (UINT32)(vbo - in_frame);
    return in_frame < vbo ? 0u : (UINT32)((UINT64)F - in_frame);
}

static VOID VideoStatusIn(PVOID self, WORD port, BYTE w, UINT32 *v)
{
    PVIDEO_STATE st = (PVIDEO_STATE)self; (VOID)port; (VOID)w;
    UINT64 now, in_frame;
    /* ⚠ LOAD-BEARING, AND NOT A VBLANK CONCERN. Reading this port resets the
         Attribute Controller's index/data flip-flop, and every guest relies on it
         before touching 0x3C0. See VideoAttributeOut. */
    st->AttributeFlipFlop = 0;
    UINT32 frame_us, line, u_vtotal, u_vdisp, u_vblank, frame_no;
    INT vtotal, vactive, in_vbl, in_hbl;

    if (!VideoBeam(st, &now, &frame_us, &u_vtotal, &u_vdisp, &u_vblank, &frame_no, &line)) {
        st->Retrace ^= 0x09;                        /* no clock injected: old behaviour */
        *v = st->Retrace;
        return;
    }
    vtotal = (INT)u_vtotal; vactive = (INT)u_vblank; (VOID)u_vdisp;
    /* Bracket the polling in the model's own microseconds -- see the header. */
    if (!st->Time3DaFirst) st->Time3DaFirst = now;
    if (st->Time3DaLast) {
        UINT64 d = now - st->Time3DaLast;
        st->PresentGapUs = (d > 0xFFFFFFFFull) ? 0xFFFFFFFFu : (UINT32)d;
        if (!d) st->Dt3DaZero++;
        else {
            UINT b = 0;
            UINT64 t = d;
            while (b < 7 && t >= 4) { t >>= 2; b++; }
            st->Dt3DaHistogram[b]++;
            if (d > (UINT64)st->Dt3DaMax)
                st->Dt3DaMax = (d > 0xFFFFFFFFull) ? 0xFFFFFFFFu : (UINT32)d;
        }
    }
    st->Time3DaLast = now;
    /* ── SCALE FIRST, DIVIDE ONCE. A scanline is not a whole number of microseconds:
         at 70 Hz it is 14285/449 = 31.8us, and taking `line_us = frame_us / vtotal`
         truncated that to 31. Lines then ran 2.6% fast -- a frame's worth of them
         reached 460 on a 449-line screen, which is what the clamp below was quietly
         absorbing, and it put every line boundary up to 12 lines out by the bottom of
         the picture. Multiplying into the frame before dividing keeps the fraction and
         needs no clamp: `line` cannot exceed vtotal-1 because in_frame < frame_us.
         The remainder is the position WITHIN the line, on the same scale. */
    in_frame = now % (UINT64)frame_us;
    {   UINT64 pos = in_frame * (UINT64)vtotal;
        line   = (UINT32)(pos / frame_us);
        in_hbl = ((UINT64)(pos % frame_us) * 100u >= (UINT64)frame_us * VIDEO_HACTIVE_PERCENT);
    }
    in_vbl = (line >= (UINT32)vactive);
    /* ── #225: A RETRACE THAT STARTED AND ENDED BETWEEN TWO POLLS HAPPENED TOO -- but
         only while the CPU throttle holds the guest (vbl_owe_on). The previous poll
         saw the active picture of an EARLIER frame, so that frame's retrace passed
         unseen during a hold; report it ONCE, as the bit-0 rule below does for a line,
         and let the next poll read the true phase. One per poll, however many frames
         were skipped: a slow machine loses real time, it does not get extra frames. */
    if (st->IsVblOweOn && !in_vbl && st->IsPort3DaHaveLast && !st->IsPort3DaLastVbl
        && frame_no != st->Port3DaLastFrame) {
        in_vbl = 1; st->Port3DaVblOwed++;
    }
    st->Port3DaLastFrame = frame_no;
    /* bit 3 = vertical retrace; bit 0 = display disabled (h- OR v-blank). Bit 0 is a
       DIFFERENT signal on a real card -- it changes per scanline, not per frame -- so
       toggling the two together, as we used to, was doubly wrong.
       The owed-blank rule below is what makes a scanline counter exact. */
    /* ── ★★★★ A BLANK THAT PASSED BETWEEN TWO POLLS HAPPENED, WHETHER OR NOT A SAMPLE
         LANDED IN IT. Bit 0 is what a scanline COUNTER reads: Lemmings' "High
         Performance PC" calibration does `wait while set; wait while clear` 320 times
         against the 8254 and uses the elapsed clocks as its game tick (guest
         CS:1602..1643), and the tick is then what places its raster palette split on
         the screen -- row 160 when the count is exactly 320 lines. On real silicon an
         `in` is a microsecond and every 6.4us hblank is sampled. Here any host stall
         during the ~10 ms window (a lock, a present, a capture) skips whole lines:
         measured reloads 0x2F00..0x38CD across one evening -- 320 to 383 lines -- and
         at 383 the toolbar's top 31 rows were drawn in the level's palette.
       ► So if the guest's previous poll was in an EARLIER line and saw the display
         active, the blanking of that earlier line went unobserved; report it ONCE and
         let the next poll read the true phase. The rule cannot fire within a line (two
         reads at one instant still agree, the property the timed model was built for)
         and does not fire when the guest saw the blank itself, so a fast poller sees
         exactly what it saw before. A poller slower than a whole line cannot count
         lines on real hardware either and is not helped. (s69 shipped this and s69
         reverted it with the PIT change it was bundled with; on its own it was right.) */
    {   UINT64 abs_line = (now / (UINT64)frame_us) * (UINT64)vtotal + line;
        INT bit0 = (in_vbl || in_hbl);
        /* ► ONE blank owed per straddle, and only for a poll that is plausibly counting
             lines (s69's rule, bounded, s70). Two generalisations were tried and
             measured wrong in video_test: repaying every line crossed as a debt of
             synthetic pulses either swallowed real blanks in the guest's `wait while
             set` phase (a 330us stall came back wholly unrepaid) or, drained the other
             way round, broke the plain case. A two-phase poll loop cannot be fed more
             than one blank per line boundary it crosses. So the residual stands: a
             host stall of N lines inside a count is repaid ONE line; the rest is
             real time the guest lost. With the interpreter port path now syncing the
             PIT (main.c iio_out) the rig measured 320.7..322.7 lines on four runs.
           ► A poll more than VIDEO_HBL_DEBT_MAX lines after the previous one is not a
             line counter (the attribute flip-flop reset before a palette write,
             once a frame) and owes nothing. */
        if (!bit0 && st->IsPort3DaHaveLast && !st->Port3DaLastBit0 && !in_vbl && !st->IsPort3DaLastVbl
            && abs_line > st->Port3DaLastLine
            && abs_line - st->Port3DaLastLine <= VIDEO_HBL_DEBT_MAX) {
            bit0 = 1; st->Port3DaHblOwed++;
        }
        st->Port3DaLastLine = abs_line; st->Port3DaLastBit0 = (BYTE)bit0;
        st->IsPort3DaLastVbl = (BYTE)in_vbl;
        st->IsPort3DaHaveLast = 1;
        *v = (UINT32)((in_vbl ? 0x08u : 0u) | (bit0 ? 0x01u : 0u));
        if (st->IsPort3DaRingOn) {          /* debug only -- see the note in the header */
            st->Port3DaRingUs[st->Port3DaRingCount & (VIDEO_PORT_3DA_RING - 1)] = (UINT32)now;
            st->Port3DaRingValue [st->Port3DaRingCount & (VIDEO_PORT_3DA_RING - 1)] = (BYTE)*v;
            st->Port3DaRingCount++;
        }
    }
    st->Retrace = (BYTE)*v;                      /* keep it observable in dumps    */
    /* Count the edge the GUEST sees, not the one the model produces: a clear->set
       transition between two of its own reads is exactly one completed
       `WAIT &H3DA,8`, so edges/second IS the guest's frame rate. */
    st->Port3DaReads++;
    if (in_vbl && !st->VblPrevious) st->VblEdges++;
    st->VblPrevious = (BYTE)in_vbl;
    /* ── RAISE THE PRESENT FROM THE GUEST'S FRAME (see present_hook in the header).
         Same window as VddVideoIsPresentReady -- the last VIDEO_PRESENT_WINDOW_PER_MILLE of
         the frame before blanking, when a retrace-paced guest has finished drawing
         and is parked here polling -- expressed in lines so no second clock read is
         paid on a path that runs millions of times a second. A poller that skips
         the window (one read per frame, at the edge) gets the edge instead: the
         frame it drew is complete by then too. Once per frame_no either way. */
    if (st->PresentHook && st->PresentFrame != frame_no) {
        UINT32 win_lines = (UINT32)vtotal * VIDEO_PRESENT_WINDOW_PER_MILLE / 1000u;
        /* ► THE FIRST POLL AFTER A GAP IS THE BEST MOMENT OF ALL. A retrace-paced
             guest leaves this port to DRAW and comes back to wait: the first read
             after it was away (>= VIDEO_PRESENT_GAP_US) means the frame is complete
             and the guest is parked -- with most of the frame still ahead for the
             render, which blocks its polls while it runs. Firing in the window
             instead (first cut) put that render right before the retrace the guest
             was waiting for, and it missed one frame in twenty (BOUNCEBX 1798 ->
             1697 edges, dtmax 2.4 -> 13.5 ms). The window and the edge remain for
             a guest that never leaves the port. */
        INT gap = st->PresentGapUs >= VIDEO_PRESENT_GAP_US;
        if (gap || in_vbl || (line + win_lines >= (UINT32)vactive)) {
            st->PresentFrame = frame_no;
            st->PresentHookFires++;
            if (gap) st->PresentHookGap++;
            st->PresentHook(st->PresentContext);
        }
    }
    st->PresentGapUs = 0;
}

/* Copy the three character generators into guest-visible memory so the pointer handed
   out by INT 10h AH=11h AL=30h resolves to real glyph data. The tables are filled at
   start-up from the system's fonts (#322, src/host/sysfont.h) before this runs. */
/* #321: copy the three tables into guest memory again after the font has changed, and
   redraw. Only the glyphs: the save-pointer table and the vectors install_fonts writes
   are a POST job a program may since have re-pointed. A font a program loaded itself
   (AH=11h, user_font_on) is its own and stays. */
INT VddVideoRefreshFonts(PVIDEO_STATE st)
{
    BYTE *f16, *f8, *f14;
    UINT c, y;
    if (!st || !st->Bus) return 0;
    f16 = (BYTE *)VddMapFlat(st->Bus, VDD_FONT8X16_SEG, 0);
    f8  = (BYTE *)VddMapFlat(st->Bus, VDD_FONT8X8_SEG, 0);
    f14 = (BYTE *)VddMapFlat(st->Bus, VDD_FONT8X14_SEG, 0);
    if (!f16 || !f8 || !f14) return 0;
    for (c = 0; c < 256; ++c) {
        for (y = 0; y < 16; ++y) f16[c * 16 + y] = vga_font_8x16[c][y];
        for (y = 0; y < 8;  ++y) f8 [c * 8  + y] = vga_font_8x8 [c][y];
        for (y = 0; y < 14; ++y) f14[c * 14 + y] = vga_font_8x14[c][y];
    }
    st->IsDirty = 1;
    return 1;
}

VOID VddVideoInstallFonts(PVIDEO_STATE st)
{
    if (!st || !st->Bus) return;           /* called before the VDD joined the bus */
    if (!VddVideoRefreshFonts(st)) return;
    /* All three are REAL designs at their own size (#322: from the system's fonts, see
       src/host/sysfont.h -- the 8x8 is Terminal's own 8x8). The 8x8 used to be
       manufactured here by OR-ing adjacent row pairs of the 8x16 -- which squashes a
       16-row glyph into 6 and fills in every counter, so 'A' came out solid and 'E' came
       out as noise. Skyroads asks for this exact table (BH=3 and BH=4, measured) and
       draws its own text from the pointer we return, so that hack WAS the game's
       garbled text. Never derive a font. */
    VideoVbePmInstall(st);                    /* #53: the 4F0Ah block, beside them */
    /* ── #266: THE SAVE POINTER TABLE, AND 0040:00A8 POINTING AT IT. ──────────────────
         0040:00A8 was never written: it held whatever the VDM started with -- on the
         rig the real machine's BIOS tables, which describe a different card than the
         one our INT 10h programs. Layout per IBM / RBIL "Video Save Pointer Table":
           +00 video parameter table         +04 dynamic save area (0: none)
           +08 alpha font override (0)       +0C graphics font override (0)
           +10 secondary save pointer table  +14/+18 reserved (0)
         and the secondary one: +00 its length (1Ah), +02 the display combination code
         table, +06 secondary alpha font (0), +0A user palette profile (0), 12 reserved.
         The DCC table is the IBM/SeaVGABIOS one (16 entries, version 1, max code 8).
         Written once here (a POST job): a program may legitimately re-point 0040:00A8
         at its own table, and later syncs must not undo that. */
    {
        BYTE *t = (BYTE *)VddMapFlat(st->Bus, VDD_VIDTAB_SEG, 0);
        static const WORD dcc[16] = {
            0x0000, 0x0100, 0x0200, 0x0102, 0x0400, 0x0104, 0x0500, 0x0502,
            0x0600, 0x0601, 0x0605, 0x0800, 0x0801, 0x0700, 0x0702, 0x0706 };
        UINT i;
        if (t) {
            for (i = 0; i < VDD_VPARAM_OFF + VDD_VPARAM_N * 64u; ++i) t[i] = 0;
            VideoWrite16(t + VDD_SAVEPTR_OFF + 0x00, VDD_VPARAM_OFF);   VideoWrite16(t + VDD_SAVEPTR_OFF + 0x02, VDD_VIDTAB_SEG);
            VideoWrite16(t + VDD_SAVEPTR_OFF + 0x10, VDD_SAVEPTR2_OFF); VideoWrite16(t + VDD_SAVEPTR_OFF + 0x12, VDD_VIDTAB_SEG);
            VideoWrite16(t + VDD_SAVEPTR2_OFF + 0x00, 0x001A);
            VideoWrite16(t + VDD_SAVEPTR2_OFF + 0x02, VDD_DCC_OFF);     VideoWrite16(t + VDD_SAVEPTR2_OFF + 0x04, VDD_VIDTAB_SEG);
            t[VDD_DCC_OFF + 0] = 16; t[VDD_DCC_OFF + 1] = 1; t[VDD_DCC_OFF + 2] = 8; t[VDD_DCC_OFF + 3] = 0;
            for (i = 0; i < 16; ++i) VideoWrite16(t + VDD_DCC_OFF + 4 + i * 2, dcc[i]);
            for (i = 0; i < VDD_VPARAM_N; ++i)
                (VOID)VddVideoParameterEntry((BYTE)i, t + VDD_VPARAM_OFF + i * 64u);
            if (st->BiosData) { VideoWrite16(st->BiosData + 0xA8, VDD_SAVEPTR_OFF); VideoWrite16(st->BiosData + 0xAA, VDD_VIDTAB_SEG); }
        }
    }
    /* ...and the font vectors, which VddVideoReset set before the host's IVT was
       final: written again now that it is (the host plants its own vectors first). */
    VideoSetVector(st, 0x43, st->Int43Segment, st->Int43Offset);
    VideoSetVector(st, 0x1F, st->Int1FSegment, st->Int1FOffset);
}

/* B8000 window hook (for the off-VM test; the live host maps the aperture RAM
   so direct writes never trap -- the renderer just reads vmem each frame). */
static BYTE VideoRead(PVOID self, UINT32 off)
{ PVIDEO_STATE st = (PVIDEO_STATE)self; return st->VideoMemory[VIDEO_TEXT_OFFSET + off]; }
static VOID VideoWrite(PVOID self, UINT32 off, BYTE v)
{ PVIDEO_STATE st = (PVIDEO_STATE)self; st->VideoMemory[VIDEO_TEXT_OFFSET + off] = v; st->IsDirty = 1; }

/* One character's glyph rows: the loaded user font wins (GH #52), otherwise the ROM
   table for the cell height in force -- 8x8 after a 1112h, 8x14 after 1111h, 8x16
   otherwise. One predictable branch in the ordinary case. */
static const BYTE *VideoGlyphRows(PCVIDEO_STATE st, BYTE ch)
{
    if (st->IsUserFontOn)  return &st->UserFont[ch * VIDEO_CELL_HEIGHT];
    if (st->CellHeight == 8)   return vga_font_8x8[ch];
    if (st->CellHeight == 14)  return vga_font_8x14[ch];
    return vga_font_8x16[ch];
}

/* ── ONE CELL. The attribute byte's top bit is BLINK OR BRIGHT BACKGROUND, and the
     Attribute Controller (AR10 bit 3, INT 10h AX=1003h) decides which. Blink is the
     power-on default. We always masked the bit off -- `(attr >> 4) & 7` -- so a
     program that turned blink OFF to get sixteen background colours (every text-mode
     UI with a light-grey dialog on a bright panel) got the dark eight, and one that
     left blink ON and used it never blinked. `st->IsBlinkOffPhase` is the phase for this
     render, set once per frame by VddVideoRender from the injected clock. */
/* ── #324: A VGA TEXT CELL IS NINE DOTS WIDE. Sequencer Clocking Mode bit 0 picks 8 or 9
     (every standard VGA text mode sets 9: 80x25 is 720x400, 40x25 is 360x400). The ninth
     column is background, except that with Line Graphics Enable (AR10 bit 2) the box-
     drawing range C0h-DFh repeats the eighth column into it, which is what makes
     horizontal lines join from cell to cell. A VESA 132-column mode is 8 dots: the
     wider character clock would not fit the line. */
static INT VideoTextCellWidthOf(PCVIDEO_STATE st)
{
    if (st->VesaTextMode) return 8;
    return (st->SequencerRegisters[1] & 0x01) ? 8 : 9;
}
INT VddVideoTextCellWidth(PCVIDEO_STATE st) { return VideoTextCellWidthOf(st); }

static VOID VideoRenderCell(PVIDEO_STATE st, INT r, INT c, BYTE ch, BYTE attr)
{
    INT gy, gx;
    INT cell_h = st->CellHeight ? st->CellHeight : VIDEO_CELL_HEIGHT;
    INT cw = VideoTextCellWidthOf(st);
    INT stride = st->Columns * cw;                        /* 360 in a 40-column mode  */
    INT lge = cw == 9 && (st->AttributeMode & 0x04) && ch >= 0xC0 && ch <= 0xDF;
    BYTE fg = attr & 0x0F, bg;
    const BYTE *gl = VideoGlyphRows(st, ch);
    if (st->IsBlink) {
        bg = (BYTE)((attr >> 4) & 0x07);
        if ((attr & 0x80) && st->IsBlinkOffPhase) fg = bg;  /* off phase: the glyph hides */
    } else {
        bg = (BYTE)((attr >> 4) & 0x0F);           /* sixteen backgrounds      */
    }
    for (gy = 0; gy < cell_h; ++gy) {
        BYTE bits = gl[gy];
        BYTE *row = &st->FrameBuffer[(r*cell_h + gy) * stride + c*cw];
        for (gx = 0; gx < 8; ++gx) row[gx] = (bits & (0x80 >> gx)) ? fg : bg;
        if (cw == 9) row[8] = (lge && (bits & 0x01)) ? fg : bg;
    }
}

static VOID VideoDrawHardwareCursor(PVIDEO_STATE st);

/* ── THE TEXT SCREEN AS THE GUEST WROTE IT, not as we drew it. ───────────────────
     An instrument for exactly one question, and it is a question screenshots cannot
     answer: when something is missing from the display, did the guest never PUT it
     there, or did we never DRAW it? Chasing QBasic's empty file list from captured
     images cost three wrong guesses -- blink, the search API, the DTA names -- two
     of which were about the picture rather than the data.
     Rows of characters, then the attribute of each cell in hex, straight out of the
     text VRAM the renderer itself reads. */
INT VddVideoTextSnapshot(PVIDEO_STATE st, char *out, INT cap)
{
    static const char hexd[] = "0123456789abcdef";
    INT r, c, n = 0;
    if (!out || cap < 16) return 0;
    for (r = 0; r < st->Rows; ++r) {
        for (c = 0; c < st->Columns && n < cap - 2; ++c) {
            BYTE ch = VideoDisplayCell(st, r, c)[0];
            out[n++] = (ch >= 32 && ch < 127) ? (char)ch : '.';
        }
        if (n < cap - 2) out[n++] = '\n';
    }
    if (n < cap - 8) { const char *h = "--attr--\n"; while (*h && n < cap - 2) out[n++] = *h++; }
    for (r = 0; r < st->Rows; ++r) {
        for (c = 0; c < st->Columns && n < cap - 3; ++c) {
            BYTE a = VideoDisplayCell(st, r, c)[1];
            out[n++] = hexd[(a >> 4) & 0xF]; out[n++] = hexd[a & 0xF];
        }
        if (n < cap - 2) out[n++] = '\n';
    }
    out[n] = 0;
    return n;
}

VOID VddVideoRender(PVIDEO_STATE st)                 /* text glyph render        */
{
    INT r, c;
    /* Text blinks at half the cursor rate: 32 frames on, 32 off at 60 Hz. */
    st->IsBlinkOffPhase = (BYTE)((st->IsBlink && st->TimeUs)
                              ? ((st->TimeUs() % 1066000u) >= 533000u) : 0);
    for (r = 0; r < st->Rows; ++r)
        for (c = 0; c < st->Columns; ++c) {
            BYTE *p = VideoDisplayCell(st, r, c);
            VideoRenderCell(st, r, c, p[0], p[1]);
        }
    VideoDrawHardwareCursor(st);
}

VOID VddVideoTextCursor(PVIDEO_STATE st, INT col, INT row,
                           WORD and_mask, WORD xor_mask)
{
    BYTE *p, ch, attr;
    if (st->ModeKind != VIDEO_KIND_TEXT) return;
    if (col < 0 || row < 0 || col >= st->Columns || row >= st->Rows) return;
    p    = VideoDisplayCell(st, row, col);
    ch   = (BYTE)((p[0] & (and_mask & 0xFF)) ^ (xor_mask & 0xFF));
    attr = (BYTE)((p[1] & (and_mask >> 8)) ^ (xor_mask >> 8));
    VideoRenderCell(st, row, col, ch, attr);
    /* The hardware cursor is drawn by the CRTC over whatever the cell holds, so it
       stays on top of the pointer when the two share a cell. */
    if (row == st->CursorRow && col == st->CursorColumn) VideoDrawHardwareCursor(st);
}

static VOID VideoDrawHardwareCursor(PVIDEO_STATE st)
{
    INT gy, gx;
    INT cell_h = st->CellHeight ? st->CellHeight : VIDEO_CELL_HEIGHT;
    INT cw = VideoTextCellWidthOf(st);
    INT stride = st->Columns * cw;
    /* ── THE TEXT CURSOR: SHAPE FROM THE GUEST, SCALED, BLINK FROM THE CLOCK. ────
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
         The phase comes from st->TimeUs, the injected clock the CRT timebase already
         uses, so this stays pure C and off-VM testable: with no clock injected the
         cursor is simply steady, which is what the existing battery expects. */
    if (st->CursorRow < st->Rows && st->CursorColumn < st->Columns) {
        UINT start, end;
        INT hidden, lit = 1;
        VddCursorLines(st->CursorShape, (UINT)cell_h, &start, &end, &hidden);
        if (st->IsCursorEmulationOff) {                  /* AH=12h BL=34h: CX as written (#252) */
            start = (st->CursorShape >> 8) & 0x1Fu; end = st->CursorShape & 0x1Fu;
            hidden = ((st->CursorShape >> 8) & 0x20u) != 0 || start > end || start >= (UINT)cell_h;
            if (end >= (UINT)cell_h) end = (UINT)cell_h - 1u;
        }
        if (st->IsCursorBlink && st->TimeUs) {
            /* 16 frames on / 16 off at 60 Hz = a 533 ms period, lit for the first
               half. Integer maths only; no floating point in a VDD. */
            UINT64 ph = st->TimeUs() % 533000u;
            lit = (ph < 266500u);
        }
        if (!hidden && lit) {
            BYTE fg = VideoDisplayCell(st, st->CursorRow, st->CursorColumn)[1] & 0x0F;
            for (gy = (INT)start; gy <= (INT)end; ++gy)
                for (gx = 0; gx < cw; ++gx)              /* all nine: the CRTC does */
                    st->FrameBuffer[(st->CursorRow*cell_h + gy) * stride
                           + st->CursorColumn*cw + gx] = fg;
        }
    }
}

/* ── ★ CURSOR EMULATION. See the note in vdd_video.h for WHY. ────────────────────
     The rule is the VGA BIOS's own (IBM's, as carried by Bochs/SeaBIOS), reproduced
     rather than approximated -- an approximation here shows up as a cursor a pixel
     or two out of place on every DOS prompt in existence:

         CH &= 0x3f;  CL &= 0x1f;
         if (cell > 8 && CL < 8 && CH < 0x20) {
             CH = (CL == CH + 1) ? ((CL + 1) * cell / 8) - 2
                                 : ((CH + 1) * cell / 8) - 1;
             CL = ((CL + 1) * cell / 8) - 1;
         }

     ⚠ THE `CL == CH + 1` BRANCH IS THE ONE THAT MATTERS and it looks like a special
       case for nothing. It is not: a two-line cursor (6-7) is DOS's UNDERLINE, and
       scaling both ends the ordinary way would give 13-15, a three-line smear. The
       branch keeps it two lines tall at the bottom of the cell.
     ⚠ `CL < 8` is what stops a shape that ALREADY knows about 16-line cells from
       being scaled twice; `CH < 0x20` leaves the hide bit alone. */
VOID VddCursorLines(WORD shape, UINT cell_h,
                      UINT *start, UINT *end, INT *hidden)
{
    UINT ch = (shape >> 8) & 0x3Fu;
    UINT cl =  shape       & 0x1Fu;
    *hidden = (ch & 0x20u) != 0;                 /* CH bit 5: cursor off        */
    ch &= 0x1Fu;
    if (cell_h > 8u && cl < 8u && !*hidden) {
        ch = (cl == ch + 1u) ? ((cl + 1u) * cell_h / 8u) - 2u
                             : ((ch + 1u) * cell_h / 8u) - 1u;
        cl = ((cl + 1u) * cell_h / 8u) - 1u;
    }
    if (ch >= cell_h) ch = cell_h - 1u;
    if (cl >= cell_h) cl = cell_h - 1u;
    if (ch > cl) *hidden = 1;                    /* the other "off" idiom       */
    *start = ch; *end = cl;
}

/* combine the 4 bit-planes into fb (16-colour indices) -- mode 12h. */
/* CGA modes 4/5/6 at B800.  The layout is the reason these were left out before:
   rows INTERLEAVE between two 8 KB banks -- even rows from offset 0, odd rows
   from 0x2000 -- and pixels are 2 bits (modes 4/5) or 1 bit (mode 6), packed
   high-bit-first.  Nothing about that is shared with the planar path, which is
   why approximating it with a text screen was never going to work. */
/* Mode 5's palette is the grey/brown variant; 4's default is cyan/magenta. Exported
   (vdd_video.h) because the INT 33h graphics cursor has to undo it: its masks act on the
   2-bit value in video memory, and the frame holds the colour this table made of it. */
/* s92: since #266 VideoRenderCga stores the 2-bit value ITSELF (an attribute-controller
   index; pal[] maps it through AR0n), so the frame holds what video memory holds and
   there is nothing to undo -- the identity. Kept as the cursor's one question to ask. */
const BYTE *VddVideoCga4Map(PCVIDEO_STATE st)
{
    static const BYTE ident[4] = { 0, 1, 2, 3 };
    (VOID)st;
    return ident;
}

static VOID VideoRenderCga(PVIDEO_STATE st)
{
    const BYTE *src = st->VideoMemory + VIDEO_TEXT_OFFSET;
    INT gw = st->GraphicsWidth, gh = st->GraphicsHeight, y, x;
    INT per = st->CgaBpp == 1 ? 8 : 4;             /* pixels per byte          */
    /* ── #266: THE PIXEL VALUE IS AN ATTRIBUTE-CONTROLLER INDEX, as on the card: 0-3 in
         04h/05h (AR12 = 03h), 0-1 in 06h (AR12 = 01h), and pal[] carries it through
         AR0n -> DAC. This drew from a private table -- pixel 1/2/3 as index 11/13/15
         (or 10/12/14 after AH=0Bh BH=1) -- which came out right only while AR0B/0D/0F
         happened to equal what AR01-03 hold after a mode set (13h/15h/17h; they do,
         by the measured table), so a guest's own AR01-03, or the BIOS's background,
         never showed. Unchanged picture for an unmodified mode 04h/05h/06h. */
    for (y = 0; y < gh; ++y) {
        const BYTE *row = src + ((y & 1) ? 0x2000 : 0) + (y >> 1) * (gw / per);
        BYTE *out = &st->FrameBuffer[y * gw];
        for (x = 0; x < gw; ++x) {
            BYTE b = row[x / per];
            if (st->CgaBpp == 1)
                out[x] = (BYTE)((b >> (7 - (x & 7))) & 1);
            else
                out[x] = (BYTE)((b >> (6 - 2 * (x & 3))) & 3);
        }
    }
}

/* ── ★★★ THE PLANAR RENDERER HAS TO READ THE CRTC. (s64) ─────────────────────────
     This drew every frame from address 0 with a stride of gw/8, which is correct for
     exactly one thing: a stationary full-screen picture. That is what the test card
     draws, what the mode-12h demos draw, and what QBasic's BUBBLES draws -- so the
     whole planar suite passed while Lemmings' GAMEPLAY was garbled, because Lemmings
     SCROLLS. Measured on the rig, mid-level: `crtc_seen=01 crtc_start=0x000002c2`.
     The game had panned 706 bytes into the buffer and we were still rendering from
     the top of it.
   ⚠ ANIMATION IS NOT SCROLLING, and that distinction is why every other guest missed
     this. BUBBLES animates by REDRAWING PIXELS -- its start address never leaves 0.
     A scrolling game leaves the pixels alone and moves the WINDOW over them. Nothing
     but Lemmings, in everything tested, does the second.
   ★ VideoRenderModeY() has honoured both registers for ages -- Doom needed the start
     address to page-flip. The capability existed in one renderer and not its sibling,
     purely because nothing had yet asked the sibling for it.
   ⚠ THE OFFSET REGISTER COUNTS IN 2-BYTE UNITS, and its reset value of 40 is right
     for 12h and wrong for 0Dh -- hence crtc_off_seen: use it only when the guest has
     actually written it, and fall back to the mode's natural stride otherwise.
   ⚠ Both values come from a guest register, so every plane index is wrapped into the
     plane rather than trusted: a mid-scroll write must not read off the end. */
/* ── #325: THE PICTURE'S SIZE AS THE CRTC IS PROGRAMMED, NOT AS THE MODE NUMBER SAYS.
     A game sets mode 13h and reprograms the CRTC -- Mode X is 320x240, others 360x480 --
     and the monitor shows what the registers say. Width: Horizontal Display End (CR01)
     + 1 character clocks of 8 dots, halved when the attribute controller pairs dots
     into 256-colour pixels (AR10 bit 6). Height: Vertical Display End (CR12 + overflow
     bits, + 1) scanlines, halved by double-scan (CR09 bit 7) and divided by the max
     scan line + 1 -- except where CGA addressing (CR17 bit 0 clear) uses those scan
     lines to interleave banks rather than to repeat lines (modes 4-6).
     Graphics modes only; trusted only when geom_regs_ok, otherwise the mode's table
     size. Checked against g_VideoModes[] for every measured mode in video_test. */
static VOID VideoGeometryOf(PCVIDEO_STATE st, INT *w, INT *h)
{
    UINT32 hde, vde, ov, ms;
    *w = st->GraphicsWidth; *h = st->GraphicsHeight;
    if (st->IsVesa || !st->IsGeometryRegistersOk) return;
    if (st->ModeKind != VIDEO_KIND_PLANAR && st->ModeKind != VIDEO_KIND_LINEAR8) return;
    hde = ((UINT32)st->CrtcRegisters[0x01] + 1u) * 8u;
    if (st->AttributeMode & 0x40) hde /= 2u;
    ov  = st->CrtcRegisters[0x07]; ms = st->CrtcRegisters[0x09];
    vde = ((UINT32)st->CrtcRegisters[0x12] | ((ov >> 1 & 1u) << 8) | ((ov >> 6 & 1u) << 9)) + 1u;
    if (ms & 0x80) vde /= 2u;
    if (st->CrtcRegisters[0x17] & 0x01) vde /= (ms & 0x1Fu) + 1u;
    if (hde < 64u || hde > NTVDD_FRAME_MAX_WIDTH || vde < 50u || vde > NTVDD_FRAME_MAX_HEIGHT) return;
    *w = (INT)hde; *h = (INT)vde;
}
VOID VddVideoGeometry(PCVIDEO_STATE st, INT *w, INT *h) { VideoGeometryOf(st, w, h); }

static VOID VideoRenderPlanar(PVIDEO_STATE st)
{
    INT y, xb, b, gw, gh;
    VideoGeometryOf(st, &gw, &gh);                            /* #325: the CRTC's size */
    if (!gw) gw = VIDEO_MODE12_WIDTH;
    if (!gh) gh = VIDEO_MODE12_HEIGHT;
    UINT32 bytes = (UINT32)(gw / 8);
    UINT32 pitch = (st->IsCrtcOffsetSeen && st->CrtcOffset)
                     ? (UINT32)st->CrtcOffset * 2u : bytes;
    /* The LATCHED start address, not the register pair -- see VideoCrtcOut case 0x0C. */
    UINT32 base  = st->IsCrtcSeen ? (UINT32)st->CrtcStartLive : 0u;
    /* ── SPLIT SCREEN. Below Line Compare the address generator restarts at 0, which
         is how a scrolling game pins a status panel to the bottom of the screen while
         the level pans behind it. Lemmings does exactly this, and without it the panel
         is drawn from the scrolled address -- the striped band under an otherwise
         correct level.
       ⚠ LINE COMPARE COUNTS SCANLINES, NOT OUR ROWS. A 200-line mode is displayed as
         400 scanlines (double-scanned), so a value that looks past the bottom of the
         picture is really in the doubled space -- halve it rather than ignoring it.
         A value that is still past the end after that means "no split", which is the
         power-on state (all ones) and must stay inert. */
    UINT32 split = (UINT32)gh;                 /* gh = no split */
    if (st->CrtcLineCompare) {
        UINT32 lc = st->CrtcLineCompare;
        if (lc >= (UINT32)gh && (lc / 2u) < (UINT32)gh) lc /= 2u;
        if (lc < (UINT32)gh) split = lc;
    }
    /* ► PEL PANNING (AR13): shift the picture left 0-7 pixels, the fine half of a smooth
         scroll (s83). Below the split line the pan is dropped when AR10 bit 5 (PPM) is
         set -- which is how a panel stays still under a panning playfield. */
    UINT32 pan  = (st->DisplayPan & 0x0Fu) < 8u ? (UINT32)(st->DisplayPan & 7u) : 0u;
    INT      ppm  = (st->AttributeMode >> 5) & 1;
    if (!pitch) pitch = bytes;
    for (y = 0; y < gh; ++y) {
        UINT32 row = (y < (INT)split) ? base + (UINT32)y * pitch
                                        : (UINT32)(y - (INT)split) * pitch;
        UINT32 sh  = (y >= (INT)split && ppm) ? 0u : pan;
        BYTE *out = &st->FrameBuffer[y * gw];
        if (!sh) {
            for (xb = 0; xb < (INT)bytes; ++xb) {
                UINT32 o = (row + (UINT32)xb) % (UINT32)VIDEO_PLANE_SIZE;
                BYTE p0 = VideoPlaneBytes(st,0)[o], p1 = VideoPlaneBytes(st,1)[o];
                BYTE p2 = VideoPlaneBytes(st,2)[o], p3 = VideoPlaneBytes(st,3)[o];
                for (b = 0; b < 8; ++b) {
                    BYTE m = (BYTE)(0x80 >> b);
                    out[xb*8 + b] = (BYTE)(((p0&m)?1:0) | ((p1&m)?2:0) | ((p2&m)?4:0) | ((p3&m)?8:0));
                }
            }
        } else {
            INT x;
            for (x = 0; x < gw; ++x) {
                UINT32 sx = (UINT32)x + sh;
                UINT32 o  = (row + (sx >> 3)) % (UINT32)VIDEO_PLANE_SIZE;
                BYTE  m  = (BYTE)(0x80 >> (sx & 7));
                out[x] = (BYTE)(((VideoPlaneBytes(st,0)[o]&m)?1:0) | ((VideoPlaneBytes(st,1)[o]&m)?2:0)
                                 | ((VideoPlaneBytes(st,2)[o]&m)?4:0) | ((VideoPlaneBytes(st,3)[o]&m)?8:0));
            }
        }
    }
}

/* Render the current mode into st->Frame each tick (always, so direct A0000
   writes show and the client stays refreshed). Does NOT blit -- the host presents
   st->Frame outside the bus lock so the slow blit never starves the V86 thread. */
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
static UINT32 VideoModeYPage(PCVIDEO_STATE st)
{
    /* ► PAGES ARE 0x4000 APART, NOT 16000. A 320x200 mode-Y page OCCUPIES 16000
         bytes per plane, but programs align the pages to 0x4000 so the page
         address is a shift rather than a multiply -- Doom's pagestart[] is
         0, 0x4000, 0x8000. Detecting on a 16000 stride put the start 384 bytes
         (16384-16000) below the real page base, which is 4.8 rows: the frame came
         out VERTICALLY ROTATED by ~5 rows, with the bottom of the picture stitched
         onto the top. Measured -- the largest row-to-row discontinuity in the
         captured frame sits at y=5. */
    UINT32 page = 0, bestn = 0, pg;
    for (pg = 0; pg < 4; ++pg) {
        UINT32 base = pg * 0x4000u, i, n = 0;
        if (base + 16000u > VIDEO_Y_PLANE_SIZE) break;
        for (i = 0; i < 16000u; i += 8) if (st->YPlanes[0][base + i]) ++n;
        if (n > bestn) { bestn = n; page = pg; }
    }
    return page * 0x4000u;
}

static VOID VideoRenderModeY(PVIDEO_STATE st)
{
    UINT32 pitch = (UINT32)(st->CrtcOffset ? st->CrtcOffset : 40) * 2u;
    /* ► READ THE PAGE FLIP; DO NOT GUESS IT. VideoModeYPage() picks the busiest page out
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
    UINT32 start = st->IsCrtcSeen ? st->CrtcStartLive : VideoModeYPage(st);
    /* ► PEL PANNING (AR13), IN 256-COLOUR UNITS: the value counts half-pixels here, so
         0/2/4/6 shift the picture left by 0-3 pixels -- the part of a smooth scroll the
         whole-byte start address cannot express (4 pixels per byte in this mode). The
         shifted row simply reads on into the next bytes, as the address counter does. */
    UINT32 pan = (UINT32)((st->DisplayPan & 7u) >> 1);
    /* ► THE SELECTED PLANE IS READ LIVE. Its most recent bytes are in the aperture
         and nowhere else -- once a static screen stops changing the mask, no further
         flush ever comes, and that plane's columns would render as whatever was last
         snapshotted. Only a SINGLE-plane mask can be attributed this way; with more
         bits set the aperture belongs to no one plane, so fall back to the snapshots. */
    /* Flush first, then render from the planes ONLY. There is no live-aperture read
       any more: under box updates the aperture is a mixture of whichever planes were
       written most recently, so reading it for the selected plane pulls in another
       plane's pixels -- which is the same error as the whole-aperture copy, wearing a
       different hat. VideoModeYFlush() has already moved everything that was written. */
    if (!st->YMapPlane) VideoModeYFlush(st);
    { INT y, x2;
      const BYTE *pl[4];
      for (x2 = 0; x2 < 4; ++x2)
          pl[x2] = st->YMapPlane ? st->YMapPlane(st->YMapContext, x2) : st->YPlanes[x2];
      INT mw, mh;
      VideoGeometryOf(st, &mw, &mh);                          /* #325: Mode X is 320x240 */
      for (y = 0; y < mh; ++y) {
          UINT32 row = start + (UINT32)y * pitch;
          BYTE *dst = st->FrameBuffer + (UINT32)y * (UINT32)mw;
          for (x2 = 0; x2 < mw; ++x2)
          { UINT32 sx = (UINT32)x2 + pan;
            dst[x2] = pl[sx & 3][(row + (sx >> 2)) & (VIDEO_Y_PLANE_SIZE - 1u)]; }
      } }
}

static VOID VideoOnFrame(PVOID self)
{
    PVIDEO_STATE st = (PVIDEO_STATE)self;
    if (!st->VideoMemory) return;
    /* ── THE START ADDRESS IS LATCHED ONCE PER FRAME, as the hardware's address
         counter loads it at the vertical retrace. Rendering straight from the
         register pair means a frame built between the two byte writes of a page flip
         shows half the old address and half the new. crtc_start_half counts the
         frames latched mid-pair -- real hardware tears there too, so this is a
         diagnostic and not a guarantee; a guest avoids it by writing both halves
         inside the retrace.
       ⚠ MEASURED ON LEMMINGS: 941 completed pairs, crtc_start_half = 0. So this is
         NOT the cause of the flicker it was written to explain. Kept because it is
         what the hardware does and the hazard is real for other guests. */
    VideoLatch(st, 1);                /* start address + pel panning as displayed (s83) */
    if (st->IsVesa) {                                 /* VESA: sync window -> vram */
        VideoVesaSync(st);
        st->Frame.Width = st->VesaWidth; st->Frame.Height = st->VesaHeight;
        if (st->VesaBpp > 8) {                        /* direct colour -> ARGB */
            VideoVesaScanWritten(st);
            VideoVesaToArgb(st);
            st->Frame.BitsPerPixel = 32;
            st->Frame.Stride = (UINT32)st->VesaWidth * 4;
            st->Frame.Pixels = (const BYTE *)st->VesaArgb;
            st->Frame.Palette = 0;                     /* contract: NULL unless bpp==8 */
        } else {
            st->Frame.BitsPerPixel = 8;
            /* ⚠ THE STRIDE IS THE MODE'S PITCH, NOT ITS WIDTH. Equal for 8bpp, and
                 that equality is why this read `= st->VesaWidth` and nobody noticed.
                 And the pitch is the 4F06 LOGICAL one, the origin the 4F07 start:
                 a page flip is nothing more than this pointer moving. */
            st->Frame.Stride = st->VesaStride ? st->VesaStride : st->VesaWidth;
            st->Frame.Pixels = st->VesaVram + st->VesaOriginLive; st->Frame.Palette = st->Palette;
        }
    } else if (st->ModeKind == VIDEO_KIND_LINEAR8 && !st->IsChain4) {  /* mode Y */
        /* ⚠ NO SNAPSHOT HERE. It used to capture the "live" plane at present time,
             but present time is an ARBITRARY moment: it can land mid-write, and
             which planes get overwritten then depends purely on timing. That made
             the picture NON-DETERMINISTIC -- the same binary produced a perfect
             title screen on one run and a coarse, blocky one on the next (observed
             directly on the physical screen; my own analysis missed it because I
             only ever inspected the richest captured frame, which hid the bad runs).
             The mask-change snapshot is well defined -- the outgoing plane is
             complete by then -- so rely on that alone. */
        INT mw, mh;
        VideoRenderModeY(st);
        VideoGeometryOf(st, &mw, &mh);                        /* #325 */
        st->Frame.Width = (WORD)mw; st->Frame.Height = (WORD)mh; st->Frame.BitsPerPixel = 8;
        st->Frame.Stride = (UINT32)mw; st->Frame.Pixels = st->FrameBuffer; st->Frame.Palette = st->Palette;
    } else if (st->ModeKind == VIDEO_KIND_LINEAR8) {        /* graphics: vmem is the FB */
        st->Frame.Width = st->GraphicsWidth; st->Frame.Height = st->GraphicsHeight; st->Frame.BitsPerPixel = 8;
        st->Frame.Stride = st->GraphicsWidth; st->Frame.Pixels = st->VideoMemory; st->Frame.Palette = st->Palette;
    } else if (st->ModeKind == VIDEO_KIND_CGA) {            /* CGA: de-interleave -> fb */
        VideoRenderCga(st);
        st->Frame.Width = st->GraphicsWidth; st->Frame.Height = st->GraphicsHeight; st->Frame.BitsPerPixel = 8;
        st->Frame.Stride = st->GraphicsWidth; st->Frame.Pixels = st->FrameBuffer; st->Frame.Palette = st->Palette;
    } else if (st->ModeKind == VIDEO_KIND_PLANAR) {         /* planar: combine -> fb    */
        INT pw, ph;
        VideoRenderPlanar(st);
        VideoGeometryOf(st, &pw, &ph);                        /* #325 */
        if (!pw) pw = VIDEO_MODE12_WIDTH;
        if (!ph) ph = VIDEO_MODE12_HEIGHT;
        st->Frame.Width = (WORD)pw; st->Frame.Height = (WORD)ph; st->Frame.BitsPerPixel = 8;
        st->Frame.Stride = (UINT32)pw; st->Frame.Pixels = st->FrameBuffer; st->Frame.Palette = st->Palette;
    } else {                                           /* text: render glyphs      */
        /* Geometry now follows the MODE, not a fixed 80x25 -- a 40-column mode
           renders 320 pixels wide instead of pretending to be 640. */
        VddVideoRender(st);
        st->Frame.Width = (WORD)(st->Columns * VideoTextCellWidthOf(st));   /* #324: 9-dot cells */
        st->Frame.Height = (WORD)(st->Rows * (st->CellHeight ? st->CellHeight : VIDEO_CELL_HEIGHT));
        VddVideoBdaSync(st);                        /* cursor moved by teletype etc. */
        st->Frame.BitsPerPixel = 8;
        st->Frame.Stride = st->Frame.Width;
        st->Frame.Pixels = st->FrameBuffer; st->Frame.Palette = st->Palette;
    }
    /* ── #266: SR1 BIT 5, "SCREEN OFF", BLANKS THE PICTURE. The sequencer stops feeding
         the attribute controller, so the monitor sees black for as long as the bit is
         set -- and the picture comes back untouched when it clears, because nothing
         in video memory moved. Programs set it to hide a redraw or a mode change, and
         AH=12h BL=36h (video refresh off) is the BIOS's door to the same bit. We stored
         it (VideoSequencerSetData; 12h BL=36h since #252) and drew the frame anyway.
       ► The frame keeps its geometry (the presenter's aspect does not jump) and goes out
         as 8bpp with a zero stride -- every row is fb's first row -- through an all-black
         palette, so the result is black whatever fb holds and nothing is rendered into
         or cleared. The render above still ran: state the frame derives (the BDA sync,
         VESA's window sync) must not stall while the screen is dark. frame_touch drops
         the raster-split arrays for a blanked frame. A mode set clears the bit (the
         measured SR1 of every mode has bit 5 = 0). */
    st->IsBlanked = (BYTE)((st->SequencerRegisters[1] & 0x20u) != 0);
    if (st->IsBlanked) {
        static UINT32 black[256];
        if (!black[0]) { INT i; for (i = 0; i < 256; ++i) black[i] = 0xFF000000u; }
        st->Frame.BitsPerPixel = 8; st->Frame.Stride = 0;
        st->Frame.Pixels = st->FrameBuffer; st->Frame.Palette = black;
    }
    st->IsDirty = 0;
}

VOID VddVideoPutChar(PVIDEO_STATE st, BYTE ch) { VideoTeletype(st, ch); st->IsDirty = 1; }

/* ── THE REGISTER FILE AS TEXT. (docs/inventory/vga.md) ──────────────────────────────
     Two things, and the second is the one that matters: every index with its value,
     and then the indices the GUEST wrote. A value whose write count is zero is OUR
     reset default -- a statement about this host, not about the guest -- so printing
     the values alone would invite exactly the wrong reading. No CRT here (the VDD is
     built off-VM too), so the formatting is by hand. */
static char *VideoReadHex2(char *p, UINT v)
{
    static const char H[] = "0123456789ABCDEF";
    *p++ = H[(v >> 4) & 15]; *p++ = H[v & 15]; return p;
}
static char *VideoReadString(char *p, const char *s) { while (*s) *p++ = *s++; return p; }
static char *VideoReadDecimal(char *p, UINT v)
{
    char t[12]; INT n = 0;
    if (!v) { *p++ = '0'; return p; }
    while (v) { t[n++] = (char)('0' + v % 10); v /= 10; }
    while (n) *p++ = t[--n];
    return p;
}
/* "SEQ 00:03 01:01 ..." for one group, then "  written:" and the indices with w>0. */
static char *VideoReadGroup(char *p, const char *name, const BYTE *reg,
                      const UINT32 *w, INT n)
{
    INT i, any = 0;
    p = VideoReadString(p, "STAGE2: VGAREG "); p = VideoReadString(p, name);
    for (i = 0; i < n; ++i) {
        *p++ = ' '; p = VideoReadHex2(p, (UINT)i); *p++ = ':';
        p = VideoReadHex2(p, reg[i]);
    }
    p = VideoReadString(p, "\r\n");
    p = VideoReadString(p, "STAGE2: VGAREG "); p = VideoReadString(p, name);
    p = VideoReadString(p, " written-by-guest:");
    for (i = 0; i < n; ++i) {
        if (!w[i]) continue;
        any = 1; *p++ = ' '; p = VideoReadHex2(p, (UINT)i);
        *p++ = 'x'; p = VideoReadDecimal(p, w[i]);
    }
    if (!any) p = VideoReadString(p, " none");
    p = VideoReadString(p, "\r\n");
    return p;
}

INT VddVideoRegistersDump(PCVIDEO_STATE st, char *out, INT cap)
{
    char *p = out;
    /* Worst case is the four groups at ~7 bytes an entry plus the externals; 1600 is
       comfortably over it. Refuse rather than overrun -- this runs inside the exit
       report, where a smashed buffer would take the whole report with it. */
    if (!st || !out || cap < 1600) return 0;
    p = VideoReadString(p, "STAGE2: VGAREG ext misc=0x");   p = VideoReadHex2(p, st->MiscOutput);
    p = VideoReadString(p, "(w=");                          p = VideoReadDecimal(p, st->MiscWrites);
    p = VideoReadString(p, ") feat=0x");                    p = VideoReadHex2(p, st->FeatureControl);
    p = VideoReadString(p, "(w=");                          p = VideoReadDecimal(p, st->FeatureWrites);
    p = VideoReadString(p, ") dacmask=0x");                 p = VideoReadHex2(p, st->DacMask);
    p = VideoReadString(p, "(w=");                          p = VideoReadDecimal(p, st->DacMaskWrites);
    p = VideoReadString(p, ") vgaen=0x");                   p = VideoReadHex2(p, st->VgaEnable);
    p = VideoReadString(p, "(w=");                          p = VideoReadDecimal(p, st->VgaEnableWrites);
    /* Say so in the artefact, not only in the source: an unwritten external register
       is OUR spec-derived default and has never been checked against a real card. */
    p = VideoReadString(p, ") cr11wp_refused=");            p = VideoReadDecimal(p, st->CrtcWriteProtectRefused);
    p = VideoReadString(p, "  [unwritten externals are spec defaults;"
                  " InputStatus0 reads 0x10 -- MEASURED on PCem's IBM VGA ROM,"
                  " all 12 modes]\r\n");
    p = VideoReadGroup(p, "SEQ ", st->SequencerRegisters,  st->SequencerWrites,   8);
    p = VideoReadGroup(p, "CRTC", st->CrtcRegisters, st->CrtcWrites, 32);
    p = VideoReadGroup(p, "GC  ", st->GcRegisters,   st->GcWrites,   16);
    p = VideoReadGroup(p, "AC  ", st->AttributeRegisters, st->AttributeWrites, 32);
    return (INT)(p - out);
}

VOID VddVideoReset(PVOID self)
{
    PVIDEO_STATE st = (PVIDEO_STATE)self;
    /* ── THE EXTERNAL REGISTERS' POWER-ON VALUES. (inventory step 1) ────────────────
         Only reached once, from VddVideoInitialize -- a mode set does NOT come through
         here -- so the write counters below accumulate for the whole run and a
         `*_w == 0` really does mean "this guest never wrote that index".
       ⚠ misc_out = 0x67 is what a VGA BIOS leaves after a mode 3 set (colour at 3Dx,
         RAM enabled, 28 MHz clock, 400 lines: -hsync +vsync). It is a SPEC value, not
         a measured one -- vga_defaults.h is generated from a probe against genuine
         6.22 but covers only the AC palette and the DAC, so there is no measured
         table for the externals yet. Step 2's probe against the PCem ET4000 is what
         turns this from spec-correct into verified. Until then the dump marks it. */
    st->FeatureControl = 0x00; st->VgaEnable = 0x01; st->DacMask = 0xFF;
    /* ⚠ AND THE POWER-ON STATE IS MODE 3'S, NOT ZEROES. A real machine reaches DOS
         with its BIOS having set mode 3, so the register file holds mode 3's values
         before a guest runs at all -- and most guests never call AH=00h for text,
         they just start drawing. Loading it here rather than only from the INT 10h
         path is what makes `VGAREG` describe a plausible card from the first line.
         VideoLoadModeDefinition sets misc_out too, so it is not set separately. */
    VideoLoadModeDefinition(st, 0x03);
    st->Mode = 3; st->Columns = VIDEO_COLUMNS; st->Rows = VIDEO_ROWS;
    st->ModeKind = VIDEO_KIND_TEXT; st->GraphicsWidth = VIDEO_FRAME_WIDTH; st->GraphicsHeight = VIDEO_FRAME_HEIGHT;
    st->CellHeight = VIDEO_CELL_HEIGHT; st->IsBlink = 1; st->IsUserFontOn = 0;
    st->AttributeMode = (BYTE)(st->AttributeMode | 0x08u);
    st->CrtcCursor = 0;
    st->ModeQueryCount = 0;
    st->CursorRow = st->CursorColumn = 0; st->CursorShape = 0x0607; st->Page = 0;
    {   INT pg; for (pg = 0; pg < 8; ++pg) st->PageRow[pg] = st->PageColumn[pg] = 0; }
    st->ScanSelect = 2; st->IsGreySum = 0; st->IsCursorEmulationOff = 0; st->IsVideoOff = 0;   /* #252 */
    st->DacWriteIndex = st->DacReadIndex = st->DacComponent = 0;
    st->SequencerIndex = st->GcIndex = 0;
    st->MapMask = 0x0F; st->BitMask = 0xFF; st->WriteMode = 0;
    st->IsChain4 = 1; st->YMask = 0x0F;
    VideoLoadDefaultCrtc(st);                      /* mode 3's CRTC, measured not assumed */
    st->SetReset = st->EnableSetReset = st->FunctionRotate = st->ReadMap = 0;
    st->ReadMode = st->ColorCompare = st->ColorDontCare = 0;
    st->Latch[0] = st->Latch[1] = st->Latch[2] = st->Latch[3] = 0;
    st->IsVesa = 0; st->VesaMode = 0; st->VesaBank = 0;
    st->VesaModeFlags = 0; st->IsModeSetNoClear = 0;   /* 40:87h = 60h at power-on */
    st->VesaDacWidth = 6;                      /* the power-on RAMDAC is a VGA's: 6 bits */
    VideoLoadDefaultPalette(st);
    if (st->VideoMemory) VideoClearText(st, 0x07);
    /* #266: the power-on font vectors and CGA colour select (mode 3's 30h). */
    st->CgaSelect = 0x30; st->IsBlanked = 0;
    VideoInt43Rom(st, st->CellHeight);
    VideoInt1FRom(st);
    VddVideoBdaSync(st);
    st->IsDirty = 1;
}

INT VddVideoInitialize(PVDD_BUS b, PVOID self)
{
    PVIDEO_STATE st = (PVIDEO_STATE)self;
    st->Bus = b;
    /* Disarmed before the reset, so no offset a guest can produce matches. */
    st->WatchOffset = 0xFFFFFFFFu;
    VddVideoReset(st);
    st->ModeYGap = VIDEO_MODEY_GAP_DEFAULT;
    if (VddClaimMemory(b, VIDEO_TEXT_BASE, 0x8000, VideoRead, VideoWrite, st)) return -1;
    if (VddClaimInterrupt(b, 0x10, VideoInt10, st)) return -1;
    if (VddClaimPorts(b, 0x3C4, 0x3C5, VideoSequencerIn, VideoSequencerOut, st)) return -1;  /* Sequencer */
    /* ⚠ THE OLD WARNING HERE ("DO NOT CLAIM CRTC 0x3D4/0x3D5", three regressions,
         mechanism UNKNOWN) IS RESOLVED, not ignored. The mechanism was that Doom
         page-flips with ONE 16-BIT WRITE and these handlers dropped the data byte, so
         claiming the port broke the flip outright -- worse than not claiming it. See
         VideoSequencerOut()/VideoIndexData(). */
    if (VddClaimPorts(b, 0x3C0, 0x3C1, VideoAttributeIn, VideoAttributeOut, st)) return -1; /* Attribute */
    if (VddClaimPorts(b, 0x3C7, 0x3C9, VideoDacIn, VideoDacOut, st)) return -1;  /* DAC       */
    if (VddClaimPorts(b, 0x3CE, 0x3CF, VideoGcIn, VideoGcOut, st)) return -1;    /* Graphics  */
    /* CRTC. Claimed at last -- see VideoRenderModeY() for why three earlier attempts
       regressed Doom and why that cause is gone. */
    if (VddClaimPorts(b, 0x3D4, 0x3D5, VideoCrtcIn, VideoCrtcOut, st)) return -1; /* CRTC     */
    if (VddClaimPorts(b, 0x3DA, 0x3DA, VideoStatusIn, VideoStatusOut, st)) return -1; /* InpStatus1 */
    /* ── THE EXTERNAL REGISTERS AND THE MONOCHROME ALIASES. (inventory step 1) ──────
         3C2 Misc Output / Input Status 0, 3C3 VGA Enable, 3C6 DAC Pixel Mask,
         3CA Feature Control read, 3CC Misc Output read -- none of which any device
         claimed, so every write was lost and every read answered 0xFF.
       ⚠ 3CB decodes nothing on a VGA and is inside the 3CA-3CC range; VideoExternalIn answers
         it 0xFF, which is what an unclaimed port answered before, so nothing changes
         for a guest that touches it.
       ⚠ THE MONO ALIASES ARE REAL. vdd_video.c already COMPUTES 0x3B4 for mode 7
         (see the BDA sync) while no handler was registered for it, so a mode-7 guest
         programming its CRTC wrote into nothing. 3B4/3B5 are the same CRTC and 3BA the
         same Input Status 1 / Feature Control as their 3Dx twins -- the same handlers,
         because they are the same registers. */
    if (VddClaimPorts(b, 0x3C2, 0x3C3, VideoExternalIn, VideoExternalOut, st)) return -1;  /* MiscOut/Enable */
    if (VddClaimPorts(b, 0x3C6, 0x3C6, VideoExternalIn, VideoExternalOut, st)) return -1;  /* DAC pixel mask */
    if (VddClaimPorts(b, 0x3CA, 0x3CC, VideoExternalIn, VideoExternalOut, st)) return -1;  /* FeatCtl/MiscOut */
    if (VddClaimPorts(b, 0x3B4, 0x3B5, VideoCrtcIn, VideoCrtcOut, st)) return -1; /* mono CRTC */
    if (VddClaimPorts(b, 0x3BA, 0x3BA, VideoStatusIn, VideoStatusOut, st)) return -1; /* mono status */
    if (VddClaimPorts(b, 0x1CE, 0x1CF, VideoVbePortIn, VideoVbePortOut, st)) return -1; /* #53: 4F0Ah's ports */
    if (VddOnFrame(b, VideoOnFrame, st)) return -1;
    return 0;
}

/* NTVDMEX -- An NTVDM replacement for Microsoft Windows
 *
 * Off-VM unit battery for the video VDD (vdd_video.c): text mode
 * 3 + graphics mode 13h + the DAC palette, over the shared video aperture. The
 * renderer pixels are checked against the real font glyph; mode 13h presents the
 * aperture directly. No VM.
 *
 *
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Matthew Layton
 */

#include <stdio.h>
#include <string.h>
#include "vdd_video.h"
#include "vga_font.h"
#define CHECK(condition,message) do{ g_Total++; if(condition){printf("  PASS  %s\n",(message));} \
    else{printf("  FAIL  %s\n",(message)); g_Failures++;} }while(0)

/* #324: the text frame's stride -- cols x the live cell width (9 dots in VGA text). */
#define TXW     (g_Video.Columns * VddVideoTextCellWidth(&g_Video))

/* Fake microsecond clock for the 0x3DA retrace timing tests (T17). The VDD takes
 * its timebase as a hook so the host can hand it QueryPerformanceCounter and the
 * battery can hand it a value it controls -- which makes CRT timing, normally the
 * least testable thing in an emulator, an ordinary deterministic assertion.
 */
UINT64 g_FakeMicroseconds = 0;

static INT g_Total = 0;
static INT g_Failures = 0;

static BYTE g_GuestMemory[0x100000];          /* guest memory for INT 10h ES:BP/ES:DX */

/* The same trick for the guest's CS:IP. The site instruments all key on it, so
 * without a hook the battery exercises the VGA engine and NONE of the tables that
 * are used to reason about a guest -- which is how they shipped unverified.
 */
static UINT32 g_FakePc = 0;

static BYTE g_VideoMemory[VIDEO_APERTURE_SIZE]; /* the video aperture (A0000) stand-in */
static VIDEO_STATE g_Video;

/* #322: the host fills the character tables from the system's fonts at start-up; off-VM
 * there are no fonts, so the tables get a deterministic pattern with every glyph lit.
 * These checks compare what was DRAWN with what the TABLE says, so any content works.
 */
/* The pattern keeps what every real font has and these checks rely on: the blank
 * characters (00h, 20h, FFh) are blank, and every other glyph has column 2 lit and
 * columns 0, 1 and 7 unlit on every row -- so it has both foreground and background
 * pixels, at known places -- and is unique (see TEST_ROW).
 */
static VOID VideoTestFillFonts(VOID)
{
    INT character;
    INT row;

    for (character = 0; character < 256; ++character)
    {
        INT blank = (character == 0x00 || character == 0x20 || character == 0xFF);
        /* rows 0 and 1 carry the code's two nibbles, so every glyph is UNIQUE -- AH=08h
         * reads a character back by matching pixels against the table, as a real
         * font allows
         */
#define TEST_ROW(row, multiplier) (BYTE)(0x20 | (((row) == 0 ? (character & 0x0F) : (row) == 1 ? (character >> 4) \
                                         : ((character * (multiplier) + (row) * 11) >> 1)) & 0x0F) << 1)
        for (row = 0; row < 16; ++row)
            g_VgaFont8x16[character][row] = blank ? 0 : TEST_ROW(row, 37);
        for (row = 0; row < 14; ++row)
            g_VgaFont8x14[character][row] = blank ? 0 : TEST_ROW(row, 29);
        for (row = 0; row < 8;  ++row)
            g_VgaFont8x8 [character][row] = blank ? 0 : TEST_ROW(row, 23);
#undef TEST_ROW
    }
}

/* True if some VDD claimed `port` -- used instead of asserting a range count. */
static INT VideoTestClaimsPort(const VDD_BUS *bus, WORD port)
{
    INT index;

    for (index = 0; index < bus->PortCount; ++index)
        if (port >= bus->Ports[index].First && port <= bus->Ports[index].Last)
            return 1;
    return 0;
}

static UINT64 VideoTestFakeClock(VOID)
{
    return g_FakeMicroseconds;
}

static UINT32 VideoTestFakePc(VOID)
{
    return g_FakePc;
}

/* Index/data register writes the way a guest does them. */
static VOID VideoTestWriteGraphics(VDD_BUS *bus, BYTE registerIndex, BYTE byteValue)
{
    UINT32 value = registerIndex;

    VddBusIo(bus,0x3CE,1,0,&value);
    value = byteValue;
    VddBusIo(bus,0x3CF,1,0,&value);
}

static VOID VideoTestWriteSequencer(VDD_BUS *bus, BYTE registerIndex, BYTE byteValue)
{
    UINT32 value = registerIndex;

    VddBusIo(bus,0x3C4,1,0,&value);
    value = byteValue;
    VddBusIo(bus,0x3C5,1,0,&value);
}

/* Write one CRTC register the way a guest does: index to 0x3D4, data to 0x3D5. */
static VOID VideoTestWriteCrtc(VDD_BUS *bus, BYTE registerIndex, BYTE byteValue)
{
    UINT32 value = registerIndex;

    VddBusIo(bus, 0x3D4, 1, 0, &value);
    value = byteValue;
    VddBusIo(bus, 0x3D5, 1, 0, &value);
}

static PBYTE VideoTestTextCell(INT row, INT column)
{
    return g_VideoMemory + VIDEO_TEXT_OFFSET + (row*g_Video.Columns+column)*2;
}

static BYTE VideoTestCellCharacter(INT row, INT column)
{
    return VideoTestTextCell(row,column)[0];
}

static BYTE VideoTestCellAttribute(INT row, INT column)
{
    return VideoTestTextCell(row,column)[1];
}

static UINT32 VideoTestDacPackReference(BYTE red, BYTE green, BYTE blue)
{
    return 0xFF000000u | ((UINT32)(red<<2)<<16) | ((UINT32)(green<<2)<<8) | (UINT32)(blue<<2);
}

INT main(VOID)
{
    VideoTestFillFonts();
    VDD_BUS bus;
    NTVDD_DEVICE device;
    NTVDD_REGISTERS registers;
    memset(&g_Video, 0, sizeof g_Video);
    g_Video.VideoMemory = g_VideoMemory;                       /* caller wires the aperture */
    device = VddVideoDevice(&g_Video);

    printf("== M3 video battery (text mode 3 + mode 13h) ==\n");

    VddBusInitialize(&bus, g_GuestMemory);
    VddBusSetSinks(&bus, 0, 0, 0, 0);

    /* T0: registers + clean mode-3 screen -------------------------------- */
    CHECK(VddBusAdd(&bus, &device) == 0, "add: video init ok");
    /* Assert WHAT was claimed, not how many ranges: a bare count silently went
     * stale when the OPL detect stub was bolted onto this VDD, and the battery
     * reported a failure that had nothing to do with video.
     */
    CHECK(bus.MemoryCount == 1 && bus.Interrupts[0x10].Service && bus.FrameCount == 1 &&
          VideoTestClaimsPort(&bus, 0x3C4) && VideoTestClaimsPort(&bus, 0x3C9) &&
          VideoTestClaimsPort(&bus, 0x3CE) && VideoTestClaimsPort(&bus, 0x3DA),
          "add: B8000 + INT10h + Seq/DAC/GC/Status ports + frame claimed");
    CHECK(g_Video.Mode == 3 && g_Video.Columns == 80 && g_Video.Rows == 25, "reset: mode 3, 80x25");
    CHECK(VideoTestCellCharacter(0,0) == ' ' && VideoTestCellAttribute(0,0) == 0x07, "reset: text cleared to spaces/0x07");

    /* T0b: Input Status 1 (3DA) toggles the retrace bit so vsync polls advance */
    { UINT32 firstStatus, secondStatus;
    VddBusIo(&bus, 0x3DA, 1, 1, &firstStatus);
    VddBusIo(&bus, 0x3DA, 1, 1, &secondStatus);
      CHECK(((firstStatus ^ secondStatus) & 0x08) == 0x08, "3DA: vertical-retrace bit toggles between reads"); }

    /* T1: teletype + cursor --------------------------------------------- */
    memset(&registers,0,sizeof registers);
    VddSetAh(&registers,0x02);
    VddSetDx(&registers,0);
    VddBusDeliverInterrupt(&bus,0x10,&registers);
    {
        PCSTR text="Hi";
        INT index;
        for(index=0;text[index];++index)
        {
            memset(&registers,0,sizeof registers);
            VddSetAh(&registers,0x0E);
            VddSetAl(&registers,(BYTE)text[index]);
            VddBusDeliverInterrupt(&bus,0x10,&registers);
        }
    }
    CHECK(VideoTestCellCharacter(0,0)=='H' && VideoTestCellCharacter(0,1)=='i' && g_Video.CursorColumn==2, "int10/0E: 'Hi' + cursor advance");

    /* T2: write char+attr + scroll + string ----------------------------- */
    memset(&registers,0,sizeof registers);
    VddSetAh(&registers,0x02);
    VddSetDx(&registers,(WORD)((5<<8)|0));
    VddBusDeliverInterrupt(&bus,0x10,&registers);
    memset(&registers,0,sizeof registers);
    VddSetAh(&registers,0x09);
    VddSetAl(&registers,'X');
    VddSetBx(&registers,0x1F);
    VddSetCx(&registers,3);
    VddBusDeliverInterrupt(&bus,0x10,&registers);
    CHECK(VideoTestCellCharacter(5,0)=='X'&&VideoTestCellAttribute(5,2)==0x1F, "int10/09: 'XXX' attr 0x1F");
    { WORD segment=0x2000,offset=0x10;
    memcpy(&g_GuestMemory[(segment<<4)+offset],"OK",2);
      memset(&registers,0,sizeof registers);
      VddSetAh(&registers,0x13);
      VddSetAl(&registers,0);
      VddSetBx(&registers,0x4E);
      VddSetCx(&registers,2);
      VddSetDx(&registers,(WORD)((12<<8)|3));
      registers.Es=segment;
      registers.Ebp=offset;
      VddBusDeliverInterrupt(&bus,0x10,&registers);
      CHECK(VideoTestCellCharacter(12,3)=='O'&&VideoTestCellAttribute(12,3)==0x4E, "int10/13: string 'OK' attr 0x4E"); }

    /* T3: B8000 hook routes to the aperture text region ------------------ */
    CHECK(VddBusMemoryWrite(&bus, 0xB8000 + (2*80+1)*2, 'Z')==1 && VideoTestCellCharacter(2,1)=='Z', "mem: B8000 write -> cell");

    /* T4: text render matches the font glyph ---------------------------- */
    memset(&registers,0,sizeof registers);
    VddSetAh(&registers,0x02);
    VddSetDx(&registers,0);
    VddBusDeliverInterrupt(&bus,0x10,&registers);
    memset(&registers,0,sizeof registers);
    VddSetAh(&registers,0x09);
    VddSetAl(&registers,'A');
    VddSetBx(&registers,0x0F);
    VddSetCx(&registers,1);
    VddBusDeliverInterrupt(&bus,0x10,&registers);
    memset(&registers,0,sizeof registers);
    VddSetAh(&registers,0x02);
    VddSetDx(&registers,(WORD)((24<<8)|79));
    VddBusDeliverInterrupt(&bus,0x10,&registers);
    VddVideoRender(&g_Video);
    { INT glyphRow,glyphColumn,mismatches=0;
    PCBYTE glyph=g_VgaFont8x16['A'];
      for(glyphRow=0;glyphRow<VIDEO_CELL_HEIGHT;++glyphRow)for(glyphColumn=0;glyphColumn<VIDEO_CELL_WIDTH;++glyphColumn)
      {
          BYTE expected=(glyph[glyphRow]&(0x80>>glyphColumn))?15:0;
          if(g_Video.FrameBuffer[glyphRow*TXW+glyphColumn]!=expected)
              mismatches++; }
      CHECK(mismatches==0, "render: text cell matches font glyph 'A'");
      /* #324: a VGA text cell is NINE dots; the ninth is background for 'A'... */
      CHECK(VddVideoTextCellWidth(&g_Video) == 9, "render: mode 3 text cells are 9 dots wide (SR01 bit 0 clear)");
      for (mismatches = 0, glyphRow = 0; glyphRow < VIDEO_CELL_HEIGHT; ++glyphRow)
          if (g_Video.FrameBuffer[glyphRow*TXW + 8] != 0)
              mismatches++;
      CHECK(mismatches==0, "render: column 9 of 'A' is background"); }

    /* T4b: A USER-LOADED FONT MUST CHANGE WHAT IS DRAWN.  GH #52 -----------
     * INT 10h AH=11h AL=00h loads the caller's own character generator. It used
     * to be accepted, marked unimplemented, and IGNORED -- the ROM glyphs were
     * drawn anyway, so a program that installed a custom character set got the
     * stock font and no error. Silent wrong output, which is the whole point of
     * GH #27. The check is deliberately a RENDER, not a "did the call return
     * ok": the old code returned ok too.
     */
    { WORD fontSegment = 0x4000, fontOffset = 0x0000;
      PBYTE fontBitmap = &g_GuestMemory[(fontSegment << 4) + fontOffset];
      INT glyphRow;
      INT glyphColumn;
      INT solid = 1;
      memset(fontBitmap, 0xFF, 16);                       /* one glyph: every pixel set */
      memset(&registers,0,sizeof registers);
      VddSetAh(&registers,0x11);
      VddSetAl(&registers,0x00);
      VddSetBx(&registers,(WORD)(16 << 8));                /* BH = 16 bytes per char */
      VddSetCx(&registers,1);                                  /* one character */
      VddSetDx(&registers,'A');                                /* starting at 'A' */
      registers.Es = fontSegment;
      registers.Ebp = fontOffset;
      VddBusDeliverInterrupt(&bus,0x10,&registers);
      CHECK(g_Video.IsUserFontOn == 1, "int10/11/00: a user font load is recorded");
      VddVideoRender(&g_Video);
      for(glyphRow=0;glyphRow<VIDEO_CELL_HEIGHT;++glyphRow)for(glyphColumn=0;glyphColumn<VIDEO_CELL_WIDTH;++glyphColumn)
          if(g_Video.FrameBuffer[glyphRow*TXW+glyphColumn] != 15)
              solid = 0;
      CHECK(solid, "int10/11/00: the USER glyph is drawn, not the ROM one");

      /* A character the caller did NOT supply must still draw as itself --
       * the table is seeded from ROM, so loading one glyph cannot blank the
       * other 255.
       */
      memset(&registers,0,sizeof registers);
      VddSetAh(&registers,0x02);
      VddSetDx(&registers,(WORD)((0<<8)|1));
      VddBusDeliverInterrupt(&bus,0x10,&registers);
      memset(&registers,0,sizeof registers);
      VddSetAh(&registers,0x09);
      VddSetAl(&registers,'B');
      VddSetBx(&registers,0x0F);
      VddSetCx(&registers,1);
      VddBusDeliverInterrupt(&bus,0x10,&registers);
      /* Park the cursor off in the corner FIRST. VddVideoRender draws the text
       * cursor over the cell it sits on, so leaving it here compares a glyph
       * against a glyph-plus-cursor -- which is what made this check fail on its
       * first run, in the TEST and not in the code. T4 above moves it to
       * (24,79) for exactly the same reason.
       */
      memset(&registers,0,sizeof registers);
      VddSetAh(&registers,0x02);
      VddSetDx(&registers,(WORD)((24<<8)|79));
      VddBusDeliverInterrupt(&bus,0x10,&registers);
      VddVideoRender(&g_Video);
      CHECK(VideoTestCellCharacter(0,1)=='B', "int10/09: 'B' landed at row 0 col 1");
      { INT mismatches2=0;
      PCBYTE glyph2=g_VgaFont8x16['B'];
        for(glyphRow=0;glyphRow<VIDEO_CELL_HEIGHT;++glyphRow)for(glyphColumn=0;glyphColumn<VIDEO_CELL_WIDTH;++glyphColumn)
        {
            BYTE expected=(glyph2[glyphRow]&(0x80>>glyphColumn))?15:0;
            if(g_Video.FrameBuffer[glyphRow*TXW + 9 + glyphColumn]!=expected)
                mismatches2++; }
        CHECK(mismatches2==0, "int10/11/00: unsupplied chars keep their ROM glyphs"); }

      /* AL=02h selects a ROM font, which is a request to go BACK -- it must
       * clear the override rather than leave a stale user font installed.
       */
      memset(&registers,0,sizeof registers);
      VddSetAh(&registers,0x11);
      VddSetAl(&registers,0x02);
      VddSetBx(&registers,0);
      VddBusDeliverInterrupt(&bus,0x10,&registers);
      CHECK(g_Video.IsUserFontOn == 0, "int10/11/02: a ROM-font select clears the override");
      VddVideoRender(&g_Video);
      { INT mismatches3=0;
      PCBYTE glyph3=g_VgaFont8x16['A'];
        for(glyphRow=0;glyphRow<VIDEO_CELL_HEIGHT;++glyphRow)for(glyphColumn=0;glyphColumn<VIDEO_CELL_WIDTH;++glyphColumn)
        {
            BYTE expected=(glyph3[glyphRow]&(0x80>>glyphColumn))?15:0;
            if(g_Video.FrameBuffer[glyphRow*TXW+glyphColumn]!=expected)
                mismatches3++; }
        CHECK(mismatches3==0, "int10/11/02: ...and 'A' is the ROM glyph again"); }

      /* A cell is VIDEO_CELL_HEIGHT tall, so a font taller than that cannot be drawn.
       * REFUSE it and mark the function unimplemented rather than store rows
       * the renderer would silently truncate.
       */
      memset(&registers,0,sizeof registers);
      VddSetAh(&registers,0x11);
      VddSetAl(&registers,0x00);
      VddSetBx(&registers,(WORD)(32 << 8));
      VddSetCx(&registers,1);
      VddSetDx(&registers,'A');
      registers.Es = fontSegment;
      registers.Ebp = fontOffset;
      VddBusDeliverInterrupt(&bus,0x10,&registers);
      CHECK(g_Video.IsUserFontOn == 0, "int10/11/00: a font taller than the cell is REFUSED");
    }

    /* T5: text frame is 640x400x8 --------------------------------------- */
    g_Video.IsDirty=1;
    VddBusFrame(&bus);
    CHECK(g_Video.Frame.Width==720 && g_Video.Frame.Height==400 && g_Video.Frame.BitsPerPixel==8,
          "frame(text): 720x400x8 palettised (9-dot cells, #324)");
    /* #324: LINE GRAPHICS. With AR10 bit 2 set (mode 3's default), C0h-DFh repeat the
     * eighth column into the ninth, so a row of horizontal lines is unbroken; outside
     * that range (B3h, a vertical line) the ninth column stays background.
     */
    { INT glyphRow, isNinthColumnOk = 1, isNinthColumnBackground = 1;
      memset(&registers,0,sizeof registers);
      VddSetAh(&registers,0x02);
      VddSetDx(&registers,(WORD)((24<<8)|79));
      VddBusDeliverInterrupt(&bus,0x10,&registers);
      VideoTestTextCell(5,0)[0] = 0xC4;
      VideoTestTextCell(5,0)[1] = 0x0F;          /* - */
      VideoTestTextCell(5,1)[0] = 0xB3;
      VideoTestTextCell(5,1)[1] = 0x0F;          /* | */
      VddVideoRender(&g_Video);
      for (glyphRow = 0; glyphRow < 16; ++glyphRow)
      {
          if (g_Video.FrameBuffer[(5*16+glyphRow)*TXW + 8] != g_Video.FrameBuffer[(5*16+glyphRow)*TXW + 7])
              isNinthColumnOk = 0;
          if (g_Video.FrameBuffer[(5*16+glyphRow)*TXW + 9 + 8] != 0)
              isNinthColumnBackground = 0;
      }
      CHECK(isNinthColumnOk, "render: C4h repeats column 8 into column 9 (line graphics, AR10 bit 2)");
      CHECK(isNinthColumnBackground, "render: B3h (outside C0h-DFh) leaves column 9 background");
      VideoTestTextCell(5,0)[0] = ' ';
      VideoTestTextCell(5,1)[0] = ' '; }

    /* T6: DAC ports set a palette entry --------------------------------- */
    { UINT32 value;
    value=0x10;
    VddBusIo(&bus,0x3C8,1,0,&value);     /* write index 0x10 */
      value=0x3F;
      VddBusIo(&bus,0x3C9,1,0,&value);                 /* R=63 */
      value=0x00;
      VddBusIo(&bus,0x3C9,1,0,&value);                 /* G=0 */
      value=0x15;
      VddBusIo(&bus,0x3C9,1,0,&value);                 /* B=21 */
      CHECK(g_Video.Palette[0x10]==(0xFF000000u|(0x3F<<2)<<16|(0x15<<2)), "DAC: 3C8/3C9 set pal[0x10]"); }

    /* T7: INT 10h AH=10/AL=10 sets one DAC reg -------------------------- */
    memset(&registers,0,sizeof registers);
    VddSetAh(&registers,0x10);
    VddSetAl(&registers,0x10);
    VddSetBx(&registers,0x20);
    VddSetDx(&registers,(WORD)(0x20<<8));
    VddSetCx(&registers,(WORD)((0x10<<8)|0x08));  /* R=0x20 G=0x10 B=0x08 */
    VddBusDeliverInterrupt(&bus,0x10,&registers);
    CHECK(g_Video.Palette[0x20]==VideoTestDacPackReference(0x20,0x10,0x08), "int10/10/10: set DAC reg 0x20");

    /* T8: mode 13h -- set mode, write a pixel, present the aperture ------ */
    memset(&registers,0,sizeof registers);
    VddSetAh(&registers,0x00);
    VddSetAl(&registers,0x13);
    VddBusDeliverInterrupt(&bus,0x10,&registers);
    CHECK(g_Video.Mode==0x13, "int10/00: mode set to 13h");
    CHECK(g_VideoMemory[0]==0 && g_VideoMemory[63999]==0, "mode13: A0000 cleared");
    g_VideoMemory[100*VIDEO_MODE13_WIDTH + 50] = 0x10;        /* direct framebuffer write */
    /* INT 10h AH=0C write pixel */
    memset(&registers,0,sizeof registers);
    VddSetAh(&registers,0x0C);
    VddSetAl(&registers,0x20);
    VddSetCx(&registers,10);
    VddSetDx(&registers,20);
    VddBusDeliverInterrupt(&bus,0x10,&registers);
    CHECK(g_VideoMemory[20*VIDEO_MODE13_WIDTH + 10]==0x20, "int10/0C: write pixel (10,20)=0x20");
    g_Video.IsDirty=1;
    VddBusFrame(&bus);
    CHECK(g_Video.Frame.Width==320 && g_Video.Frame.Height==200 && g_Video.Frame.BitsPerPixel==8
          && g_Video.Frame.Pixels==g_VideoMemory, "frame(mode13): 320x200x8 from the aperture");

    /* T9: VESA 4F00 controller info ------------------------------------- */
    /* [CAUTION]: THE INFO BLOCK IS 256 BYTES UNLESS THE CALLER PRESET "VBE2". (s74) Heretic
     * allocates exactly 256 bytes of DOS memory for it, and we used to write the OEM
     * string at +0x100 -- over the MCB of the next block, which broke the chain and
     * killed it at I_AllocLow. So: poison 256..511, and check nothing lands there.
     */
    { WORD segment=0x3000, offset=0x0000;
    PBYTE buffer=&g_GuestMemory[(segment<<4)+offset];
    UINT32 modeListOffset;
    UINT32 oemStringOffset;
    INT index;
    INT clean=1;
      memset(buffer, 0xAA, 512);
      memset(&registers,0,sizeof registers);
      VddSetAh(&registers,0x4F);
      VddSetAl(&registers,0x00);
      registers.Es=segment;
      registers.Edi=offset;
      VddBusDeliverInterrupt(&bus,0x10,&registers);
      CHECK(VddGetAx(&registers)==0x004F && buffer[0]=='V'&&buffer[1]=='E'&&buffer[2]=='S'&&buffer[3]=='A', "vesa/4F00: 'VESA' signature");
      modeListOffset = buffer[14]|(buffer[15]<<8);                 /* mode-list offset (low word of far ptr) */
      oemStringOffset = buffer[6]|(buffer[7]<<8);                   /* OEM-string offset */
      CHECK((buffer[(modeListOffset&0xFFFF)]|(buffer[(modeListOffset&0xFFFF)+1]<<8))==0x100, "vesa/4F00: mode list starts 0x100");
      CHECK((buffer[16]|(buffer[17]<<8))==segment && (buffer[8]|(buffer[9]<<8))==segment, "vesa/4F00: pointers are in the caller's segment");
      CHECK(modeListOffset>=34 && modeListOffset<256 && oemStringOffset>=34 && oemStringOffset<256, "vesa/4F00: mode list and OEM string inside the 256-byte block");
      for (index=256;index<512;++index)
          if (buffer[index]!=0xAA)
              clean=0;
      CHECK(clean, "vesa/4F00: nothing written past 256 bytes without 'VBE2'");
      /* With "VBE2" preset the block is 512 bytes and may be used in full. */
      memset(buffer, 0xAA, 512);
      buffer[0]='V';
      buffer[1]='B';
      buffer[2]='E';
      buffer[3]='2';
      memset(&registers,0,sizeof registers);
      VddSetAh(&registers,0x4F);
      VddSetAl(&registers,0x00);
      registers.Es=segment;
      registers.Edi=offset;
      VddBusDeliverInterrupt(&bus,0x10,&registers);
      CHECK(VddGetAx(&registers)==0x004F && buffer[0]=='V'&&buffer[1]=='E'&&buffer[2]=='S'&&buffer[3]=='A' && (buffer[4]|(buffer[5]<<8))==0x0200,
            "vesa/4F00 (VBE2): signature rewritten, version 2.0");
      CHECK(buffer[511]==0, "vesa/4F00 (VBE2): 512-byte block initialised");
      /* section 4.3: with 'VBE2' the OEM string -- and the vendor, product and revision strings --
       * are copied into OemData (+100h). They all pointed at one string at +22h. #226
       */
      { UINT pointerOffsets[4] = { 6, 22, 26, 30 }, index, inside = 1, distinct = 1;
        for (index = 0; index < 4; ++index)
        {
          UINT pointerOffset = buffer[pointerOffsets[index]] | (buffer[pointerOffsets[index]+1] << 8);
          UINT pointerSegment = buffer[pointerOffsets[index]+2] | (buffer[pointerOffsets[index]+3] << 8);
          if (pointerSegment != segment || pointerOffset < 0x100 || pointerOffset >= 0x200)
              inside = 0;
          if (index && pointerOffset == (UINT)(buffer[pointerOffsets[index-1]] | (buffer[pointerOffsets[index-1]+1] << 8)))
              distinct = 0;
        }
        CHECK(inside && distinct && memcmp(&buffer[buffer[6]|(buffer[7]<<8)], "NTVDMEX VESA", 13)==0
              && memcmp(&buffer[buffer[22]|(buffer[23]<<8)], "NTVDMEX", 8)==0,
              "vesa/4F00 (VBE2): OEM/vendor/product/rev strings are four strings in OemData (+100h)"); } }

    /* T10: VESA 4F01 mode info for 0x101 (640x480x8) -------------------- */
    { WORD segment=0x3100;
    PBYTE buffer=&g_GuestMemory[(segment<<4)];
      memset(&registers,0,sizeof registers);
      VddSetAh(&registers,0x4F);
      VddSetAl(&registers,0x01);
      VddSetCx(&registers,0x101);
      registers.Es=segment;
      registers.Edi=0;
      VddBusDeliverInterrupt(&bus,0x10,&registers);
      CHECK(VddGetAx(&registers)==0x004F && (buffer[18]|(buffer[19]<<8))==640 && (buffer[20]|(buffer[21]<<8))==480 && buffer[25]==8,
            "vesa/4F01: 0x101 = 640x480x8");
      CHECK(buffer[29]==(VIDEO_VESA_VRAM/(640u*480u))-1 && buffer[30]==1, "vesa/4F01: NumberOfImagePages = pages-1 (was 0, oracle row), Reserved=1");
      memset(&registers,0,sizeof registers);
      VddSetAh(&registers,0x4F);
      VddSetAl(&registers,0x00);
      registers.Es=segment;
      registers.Edi=0;
      memset(buffer,0,512);
      VddBusDeliverInterrupt(&bus,0x10,&registers);
      CHECK((buffer[10]|(buffer[11]<<8)|(buffer[12]<<16)|(buffer[13]<<24))==1, "vesa/4F00: Capabilities D0 = DAC switchable (we honour 4F08 BH=8)"); }

    /* T11: VESA 4F02 set mode + 4F05 banking round-trips through vram ---- */
    memset(&registers,0,sizeof registers);
    VddSetAh(&registers,0x4F);
    VddSetAl(&registers,0x02);
    VddSetBx(&registers,0x101);
    VddBusDeliverInterrupt(&bus,0x10,&registers);
    CHECK(VddGetAx(&registers)==0x004F && g_Video.IsVesa && g_Video.VesaWidth==640 && g_Video.VesaHeight==480, "vesa/4F02: set 0x101");
    g_VideoMemory[10] = 0xAB;                          /* write into bank 0 window */
    memset(&registers,0,sizeof registers);
    VddSetAh(&registers,0x4F);
    VddSetAl(&registers,0x05);
    VddSetBx(&registers,0);
    VddSetDx(&registers,1); /* -> bank 1 */
    VddBusDeliverInterrupt(&bus,0x10,&registers);
    CHECK(g_Video.VesaBank==1, "vesa/4F05: switched to bank 1");
    g_VideoMemory[10] = 0xCD;                          /* write into bank 1 window */
    memset(&registers,0,sizeof registers);
    VddSetAh(&registers,0x4F);
    VddSetAl(&registers,0x05);
    VddSetBx(&registers,0);
    VddSetDx(&registers,0); /* back to 0 */
    VddBusDeliverInterrupt(&bus,0x10,&registers);
    CHECK(g_VideoMemory[10]==0xAB, "vesa/4F05: bank 0 window restored from vram");
    CHECK(g_Video.VesaVram[1*VIDEO_VESA_WINDOW + 10]==0xCD, "vesa/4F05: bank 1 byte kept in vram");
    /* section 4.8: BH selects set(00)/get(01), BL is the WINDOW (A=0, B=1). The code read BL as
     * the selector, so a "get window A" (BH=01,BL=00,DX=junk) was a SET to junk.
     */
    memset(&registers,0,sizeof registers);
    VddSetAh(&registers,0x4F);
    VddSetAl(&registers,0x05);
    VddSetBx(&registers,0x0100);
    VddSetDx(&registers,0x1234);
    VddBusDeliverInterrupt(&bus,0x10,&registers);
    CHECK(VddGetAx(&registers)==0x004F && VddGetDx(&registers)==0 && g_Video.VesaBank==0, "vesa/4F05 get (BH=01): DX=bank 0, bank NOT changed by DX in");
    memset(&registers,0,sizeof registers);
    VddSetAh(&registers,0x4F);
    VddSetAl(&registers,0x05);
    VddSetBx(&registers,0x0001);
    VddSetDx(&registers,1);
    VddBusDeliverInterrupt(&bus,0x10,&registers);
    CHECK(VddGetAx(&registers)!=0x004F && g_Video.VesaBank==0, "vesa/4F05 window B (BL=01): fails, we advertise none");
    memset(&registers,0,sizeof registers);
    VddSetAh(&registers,0x4F);
    VddSetAl(&registers,0x05);
    VddSetBx(&registers,0x0000);
    VddSetDx(&registers,(WORD)(VIDEO_VESA_VRAM/VIDEO_VESA_WINDOW));
    VddBusDeliverInterrupt(&bus,0x10,&registers);
    CHECK(VddGetAx(&registers)==0x024F && g_Video.VesaBank==0, "vesa/4F05 set past memory: AH=02, bank kept");
    memset(&registers,0,sizeof registers);
    VddSetAh(&registers,0x4F);
    VddSetAl(&registers,0x02);
    VddSetBx(&registers,0x4101);
    VddBusDeliverInterrupt(&bus,0x10,&registers);   /* LFB */
    memset(&registers,0,sizeof registers);
    VddSetAh(&registers,0x4F);
    VddSetAl(&registers,0x05);
    VddSetBx(&registers,0x0000);
    VddSetDx(&registers,1);
    VddBusDeliverInterrupt(&bus,0x10,&registers);
    CHECK(VddGetAx(&registers)==0x034F, "vesa/4F05 in an LFB mode: AH=03 (invalid in current mode)");
    memset(&registers,0,sizeof registers);
    VddSetAh(&registers,0x4F);
    VddSetAl(&registers,0x02);
    VddSetBx(&registers,0x0101);
    VddBusDeliverInterrupt(&bus,0x10,&registers);   /* banked again */

    /* T11b: 4F08 DAC width + 4F09 palette, VBE 2.0 section 4.11/section 4.12 ------------------- */
    { WORD segment=0x3200;
    PBYTE table=&g_GuestMemory[(segment<<4)];
      memset(&registers,0,sizeof registers);
      VddSetAh(&registers,0x4F);
      VddSetAl(&registers,0x08);
      VddSetBx(&registers,0x0001);
      VddBusDeliverInterrupt(&bus,0x10,&registers);
      CHECK(VddGetAx(&registers)==0x004F && (VddGetBx(&registers)>>8)==6, "vesa/4F08 get after a mode set: 6 bits");
      memset(&registers,0,sizeof registers);
      VddSetAh(&registers,0x4F);
      VddSetAl(&registers,0x08);
      VddSetBx(&registers,0x0A00);
      VddBusDeliverInterrupt(&bus,0x10,&registers);
      CHECK(VddGetAx(&registers)==0x004F && (VddGetBx(&registers)>>8)==8, "vesa/4F08 set 10 bits: next lower we have = 8");
      /* 8-bit palette entry 1 = pure blue, through 4F09; the presenter palette must follow */
      table[0]=0xFF;
      table[1]=0;
      table[2]=0;
      table[3]=0;
      memset(&registers,0,sizeof registers);
      VddSetAh(&registers,0x4F);
      VddSetAl(&registers,0x09);
      VddSetBx(&registers,0x0000);
      VddSetCx(&registers,1);
      VddSetDx(&registers,1);
      registers.Es=segment;
      registers.Edi=0;
      VddBusDeliverInterrupt(&bus,0x10,&registers);
      CHECK(VddGetAx(&registers)==0x004F && g_Video.Dac[1]==0xFF0000FFu && g_Video.Palette[1]==0xFF0000FFu, "vesa/4F09 set (8-bit): dac[1] blue AND pal[1] refreshed");
      memset(&registers,0,sizeof registers);
      VddSetAh(&registers,0x4F);
      VddSetAl(&registers,0x08);
      VddSetBx(&registers,0x0700);
      VddBusDeliverInterrupt(&bus,0x10,&registers);
      CHECK((VddGetBx(&registers)>>8)==6, "vesa/4F08 set 7 bits: next lower = 6");
      /* 6-bit: index 6 -- one the EGA attribute mapping would send to DAC 0x14 -- must be identity in a VESA 8bpp mode */
      table[0]=0x3F;
      table[1]=0;
      table[2]=0;
      table[3]=0;
      memset(&registers,0,sizeof registers);
      VddSetAh(&registers,0x4F);
      VddSetAl(&registers,0x09);
      VddSetBx(&registers,0x0000);
      VddSetCx(&registers,1);
      VddSetDx(&registers,6);
      registers.Es=segment;
      registers.Edi=0;
      VddBusDeliverInterrupt(&bus,0x10,&registers);
      CHECK(g_Video.Dac[6]==0xFF0000FCu && g_Video.Palette[6]==0xFF0000FCu, "vesa/4F09 set (6-bit) index 6: pal[6] is the DAC entry, not the EGA remap");
      table[0]=table[1]=table[2]=table[3]=0xEE;
      memset(&registers,0,sizeof registers);
      VddSetAh(&registers,0x4F);
      VddSetAl(&registers,0x09);
      VddSetBx(&registers,0x0001);
      VddSetCx(&registers,1);
      VddSetDx(&registers,6);
      registers.Es=segment;
      registers.Edi=0;
      VddBusDeliverInterrupt(&bus,0x10,&registers);
      CHECK(VddGetAx(&registers)==0x004F && table[0]==0x3F && table[1]==0 && table[2]==0 && table[3]==0, "vesa/4F09 get (6-bit): B,G,R,0 = 3F,0,0,0");
      memset(&registers,0,sizeof registers);
      VddSetAh(&registers,0x4F);
      VddSetAl(&registers,0x09);
      VddSetBx(&registers,0x0002);
      VddSetCx(&registers,1);
      VddSetDx(&registers,0);
      registers.Es=segment;
      registers.Edi=0;
      VddBusDeliverInterrupt(&bus,0x10,&registers);
      CHECK(VddGetAx(&registers)==0x024F, "vesa/4F09 secondary palette (BL=02): AH=02, none here");
      memset(&registers,0,sizeof registers);
      VddSetAh(&registers,0x4F);
      VddSetAl(&registers,0x09);
      VddSetBx(&registers,0x0000);
      VddSetCx(&registers,10);
      VddSetDx(&registers,250);
      registers.Es=segment;
      registers.Edi=0;
      VddBusDeliverInterrupt(&bus,0x10,&registers);
      CHECK(VddGetAx(&registers)==0x024F, "vesa/4F09 DX+CX past 256: AH=02");
      memset(&registers,0,sizeof registers);
      VddSetAh(&registers,0x4F);
      VddSetAl(&registers,0x08);
      VddSetBx(&registers,0x0800);
      VddBusDeliverInterrupt(&bus,0x10,&registers);
      memset(&registers,0,sizeof registers);
      VddSetAh(&registers,0x4F);
      VddSetAl(&registers,0x02);
      VddSetBx(&registers,0x0101);
      VddBusDeliverInterrupt(&bus,0x10,&registers);
      memset(&registers,0,sizeof registers);
      VddSetAh(&registers,0x4F);
      VddSetAl(&registers,0x08);
      VddSetBx(&registers,0x0001);
      VddBusDeliverInterrupt(&bus,0x10,&registers);
      CHECK((VddGetBx(&registers)>>8)==6, "vesa/4F08: a mode set resets the DAC to 6 bits");
      memset(&registers,0,sizeof registers);
      VddSetAh(&registers,0x4F);
      VddSetAl(&registers,0x02);
      VddSetBx(&registers,0x0111);
      VddBusDeliverInterrupt(&bus,0x10,&registers);
      memset(&registers,0,sizeof registers);
      VddSetAh(&registers,0x4F);
      VddSetAl(&registers,0x08);
      VddSetBx(&registers,0x0001);
      VddBusDeliverInterrupt(&bus,0x10,&registers);
      CHECK(VddGetAx(&registers)==0x034F, "vesa/4F08 in a direct-colour mode: AH=03");
      memset(&registers,0,sizeof registers);
      VddSetAh(&registers,0x4F);
      VddSetAl(&registers,0x02);
      VddSetBx(&registers,0x0101);
      VddBusDeliverInterrupt(&bus,0x10,&registers); }

    /* T11c: 4F04 save/restore state (section 4.7) and the AH=1Ch it is a superset of.
     * AH=1Ch reported 3 blocks (192 bytes) and then wrote 768 bytes of DAC into the
     * caller's buffer -- the Heretic MCB overrun, in another function. The size we
     * report must be at least what we write, for both entry points.
     */
    { WORD segment=0x3300;
    PBYTE stateBuffer=&g_GuestMemory[(segment<<4)];
    WORD blocks;
    WORD blocks1c;
    UINT index;
    UINT spill=0;
      memset(&registers,0,sizeof registers);
      VddSetAh(&registers,0x4F);
      VddSetAl(&registers,0x04);
      VddSetDx(&registers,0x0000);
      VddSetCx(&registers,0x000F);
      VddBusDeliverInterrupt(&bus,0x10,&registers);
      blocks = VddGetBx(&registers);
      CHECK(VddGetAx(&registers)==0x004F && blocks>=12, "vesa/4F04 DL=00: reports a size that can hold a 768-byte DAC");
      memset(&registers,0,sizeof registers);
      VddSetAh(&registers,0x1C);
      VddSetAl(&registers,0x00);
      VddSetCx(&registers,0x0007);
      VddBusDeliverInterrupt(&bus,0x10,&registers);
      blocks1c = VddGetBx(&registers);
      CHECK(VddGetAl(&registers)==0x1C && blocks1c>=12, "int10/1C AL=00: size >= 12 blocks (was 3, then wrote 768 bytes)");
      /* arrange a state: 8-bit DAC, entry 7 = (R=0x12,G=0x34,B=0x56), page 2 at stride 1024 */
      memset(&registers,0,sizeof registers);
      VddSetAh(&registers,0x4F);
      VddSetAl(&registers,0x08);
      VddSetBx(&registers,0x0800);
      VddBusDeliverInterrupt(&bus,0x10,&registers);
      { PBYTE table=&g_GuestMemory[(0x3200<<4)];
      table[0]=0x56;
      table[1]=0x34;
      table[2]=0x12;
      table[3]=0;
        memset(&registers,0,sizeof registers);
        VddSetAh(&registers,0x4F);
        VddSetAl(&registers,0x09);
        VddSetBx(&registers,0x0000);
        VddSetCx(&registers,1);
        VddSetDx(&registers,7);
        registers.Es=0x3200;
        registers.Edi=0;
        VddBusDeliverInterrupt(&bus,0x10,&registers); }
      memset(&registers,0,sizeof registers);
      VddSetAh(&registers,0x4F);
      VddSetAl(&registers,0x06);
      VddSetBx(&registers,0x00);
      VddSetCx(&registers,1024);
      VddBusDeliverInterrupt(&bus,0x10,&registers);
      memset(&registers,0,sizeof registers);
      VddSetAh(&registers,0x4F);
      VddSetAl(&registers,0x07);
      VddSetBx(&registers,0x00);
      VddSetCx(&registers,0);
      VddSetDx(&registers,480);
      VddBusDeliverInterrupt(&bus,0x10,&registers);
      /* save; the bytes past the reported size must be untouched */
      memset(stateBuffer, 0xA5, 4096);
      memset(&registers,0,sizeof registers);
      VddSetAh(&registers,0x4F);
      VddSetAl(&registers,0x04);
      VddSetDx(&registers,0x0001);
      VddSetCx(&registers,0x000F);
      registers.Es=segment;
      registers.Ebx=0;
      VddBusDeliverInterrupt(&bus,0x10,&registers);
      for (index = blocks*64u; index < 4096; ++index)
          if (stateBuffer[index] != 0xA5)
              ++spill;
      CHECK(VddGetAx(&registers)==0x004F && spill==0, "vesa/4F04 DL=01 save: nothing written past the reported size");
      /* disturb everything, then restore */
      memset(&registers,0,sizeof registers);
      VddSetAh(&registers,0x00);
      VddSetAl(&registers,0x03);
      VddBusDeliverInterrupt(&bus,0x10,&registers);   /* text mode: leaves VESA */
      g_Video.Dac[7] = 0xFF000000u;
      memset(&registers,0,sizeof registers);
      VddSetAh(&registers,0x4F);
      VddSetAl(&registers,0x04);
      VddSetDx(&registers,0x0002);
      VddSetCx(&registers,0x000F);
      registers.Es=segment;
      registers.Ebx=0;
      VddBusDeliverInterrupt(&bus,0x10,&registers);
      CHECK(VddGetAx(&registers)==0x004F && g_Video.IsVesa && g_Video.VesaMode==0x101 && g_Video.VesaStride==1024 && g_Video.VesaStartY==480
            && g_Video.VesaDacWidth==8 && g_Video.Dac[7]==0xFF123456u && g_Video.Palette[7]==0xFF123456u,
            "vesa/4F04 DL=02 restore: VESA mode, pitch, start, DAC width and DAC entry all back");
      memset(&registers,0,sizeof registers);
      VddSetAh(&registers,0x4F);
      VddSetAl(&registers,0x04);
      VddSetDx(&registers,0x0002);
      VddSetCx(&registers,0x000F);
      registers.Es=0x3400;
      registers.Ebx=0;
      VddBusDeliverInterrupt(&bus,0x10,&registers);
      CHECK(VddGetAx(&registers)==0x024F, "vesa/4F04 restore from a buffer we did not write: AH=02, refused");
      memset(&registers,0,sizeof registers);
      VddSetAh(&registers,0x4F);
      VddSetAl(&registers,0x02);
      VddSetBx(&registers,0x0101);
      VddBusDeliverInterrupt(&bus,0x10,&registers); }

    /* T12: VESA frame is vesa_w x vesa_h x8 ----------------------------- */
    g_Video.IsDirty=1;
    VddBusFrame(&bus);
    CHECK(g_Video.Frame.Width==640 && g_Video.Frame.Height==480 && g_Video.Frame.Pixels==g_Video.VesaVram,
          "frame(vesa): 640x480x8 from vesa_vram");
    /* a banked guest writes into the A0000 window and never calls 4F05 again: the
     * present must sync the window into the frame (vesacube, s74b)
     */
    g_VideoMemory[640*10 + 7] = 0x0C;
    g_Video.IsDirty=1;
    VddBusFrame(&bus);
    CHECK(g_Video.Frame.Pixels[640*10 + 7]==0x0C, "frame(vesa banked): a window write reaches the presented frame");

    /* T12b: VESA 4F06 logical scan line + 4F07 display start, VBE 2.0 section 4.9/4.10.
     * Both used to be ACCEPTED AND IGNORED: the stride the presenter used never moved
     * and the start it displayed was always (0,0), so a guest that page-flips through
     * 4F07 -- the standard VESA double-buffer -- showed the wrong page while every call
     * returned 004F. Expectations below are the spec's, not the code's.
     */
    /* BL=01 get: BX bytes/line, CX pixels/line, DX max lines at that length */
    memset(&registers,0,sizeof registers);
    VddSetAh(&registers,0x4F);
    VddSetAl(&registers,0x06);
    VddSetBx(&registers,0x01);
    VddBusDeliverInterrupt(&bus,0x10,&registers);
    CHECK(VddGetAx(&registers)==0x004F && VddGetBx(&registers)==640 && VddGetCx(&registers)==640 && VddGetDx(&registers)==VIDEO_VESA_VRAM/640,
          "vesa/4F06 get: 640 bytes, 640 px, VRAM/640 lines");
    /* BL=00 set 1024 pixels -> stride 1024 */
    memset(&registers,0,sizeof registers);
    VddSetAh(&registers,0x4F);
    VddSetAl(&registers,0x06);
    VddSetBx(&registers,0x00);
    VddSetCx(&registers,1024);
    VddBusDeliverInterrupt(&bus,0x10,&registers);
    CHECK(VddGetAx(&registers)==0x004F && VddGetBx(&registers)==1024 && VddGetCx(&registers)==1024 && VddGetDx(&registers)==VIDEO_VESA_VRAM/1024
          && g_Video.VesaStride==1024, "vesa/4F06 set 1024 px: stride 1024, DX=VRAM/1024");
    /* BL=03 get maximum: longest line that still holds the mode's 480 rows */
    memset(&registers,0,sizeof registers);
    VddSetAh(&registers,0x4F);
    VddSetAl(&registers,0x06);
    VddSetBx(&registers,0x03);
    VddBusDeliverInterrupt(&bus,0x10,&registers);
    CHECK(VddGetAx(&registers)==0x004F && VddGetBx(&registers)==VIDEO_VESA_VRAM/480 && VddGetCx(&registers)==VIDEO_VESA_VRAM/480
          && VddGetDx(&registers)>=480 && g_Video.VesaStride==1024, "vesa/4F06 get max: VRAM/480, stride untouched");
    /* too long (65535*480 > VRAM) -> 02h, unchanged; narrower than the mode -> 02h */
    memset(&registers,0,sizeof registers);
    VddSetAh(&registers,0x4F);
    VddSetAl(&registers,0x06);
    VddSetBx(&registers,0x02);
    VddSetCx(&registers,0xFFFF);
    VddBusDeliverInterrupt(&bus,0x10,&registers);
    CHECK(VddGetAx(&registers)==0x024F && g_Video.VesaStride==1024, "vesa/4F06 set too long: AH=02, stride kept");
    memset(&registers,0,sizeof registers);
    VddSetAh(&registers,0x4F);
    VddSetAl(&registers,0x06);
    VddSetBx(&registers,0x00);
    VddSetCx(&registers,320);
    VddBusDeliverInterrupt(&bus,0x10,&registers);
    CHECK(VddGetAx(&registers)==0x024F && g_Video.VesaStride==1024, "vesa/4F06 set narrower than mode: AH=02, stride kept");
    /* 4F07 set (0,480): page 2 at stride 1024 -> the frame starts 480 rows in */
    memset(&registers,0,sizeof registers);
    VddSetAh(&registers,0x4F);
    VddSetAl(&registers,0x07);
    VddSetBx(&registers,0x00);
    VddSetCx(&registers,0);
    VddSetDx(&registers,480);
    VddBusDeliverInterrupt(&bus,0x10,&registers);
    g_Video.VesaVram[480u*1024u + 5] = 0x77;
    g_Video.IsDirty=1;
    VddBusFrame(&bus);
    CHECK(VddGetAx(&registers)==0x004F && g_Video.Frame.Stride==1024 && g_Video.Frame.Pixels==g_Video.VesaVram+480u*1024u
          && g_Video.Frame.Pixels[5]==0x77, "vesa/4F07 set (0,480): frame is page 2 at stride 1024");
    memset(&registers,0,sizeof registers);
    VddSetAh(&registers,0x4F);
    VddSetAl(&registers,0x07);
    VddSetBx(&registers,0x01);
    VddBusDeliverInterrupt(&bus,0x10,&registers);
    CHECK(VddGetAx(&registers)==0x004F && (VddGetBx(&registers)>>8)==0 && VddGetCx(&registers)==0 && VddGetDx(&registers)==480, "vesa/4F07 get: (0,480), BH=0");
    /* a start that leaves less than a full page -> fail, no change */
    memset(&registers,0,sizeof registers);
    VddSetAh(&registers,0x4F);
    VddSetAl(&registers,0x07);
    VddSetBx(&registers,0x00);
    VddSetCx(&registers,0);
    VddSetDx(&registers,(WORD)(VIDEO_VESA_VRAM/1024 - 100));
    VddBusDeliverInterrupt(&bus,0x10,&registers);
    CHECK(VddGetAx(&registers)==0x024F && g_Video.VesaStartY==480, "vesa/4F07 set past memory: AH=02, start kept");
    /* BL=80h (during retrace) is a set too; x offset moves the origin by bytes-per-pixel */
    memset(&registers,0,sizeof registers);
    VddSetAh(&registers,0x4F);
    VddSetAl(&registers,0x07);
    VddSetBx(&registers,0x80);
    VddSetCx(&registers,8);
    VddSetDx(&registers,0);
    VddBusDeliverInterrupt(&bus,0x10,&registers);
    g_Video.IsDirty=1;
    VddBusFrame(&bus);
    CHECK(VddGetAx(&registers)==0x004F && g_Video.Frame.Pixels==g_Video.VesaVram+8, "vesa/4F07 BL=80 set (8,0): origin +8 bytes");
    /* a mode set resets both */
    memset(&registers,0,sizeof registers);
    VddSetAh(&registers,0x4F);
    VddSetAl(&registers,0x02);
    VddSetBx(&registers,0x101);
    VddBusDeliverInterrupt(&bus,0x10,&registers);
    CHECK(g_Video.VesaStride==640 && g_Video.VesaStartX==0 && g_Video.VesaStartY==0, "vesa/4F02: resets stride and display start");
    /* direct colour: 0x111 (640x480x16), flip to row 480 and read a white pixel back as ARGB */
    memset(&registers,0,sizeof registers);
    VddSetAh(&registers,0x4F);
    VddSetAl(&registers,0x02);
    VddSetBx(&registers,0x111);
    VddBusDeliverInterrupt(&bus,0x10,&registers);
    memset(&registers,0,sizeof registers);
    VddSetAh(&registers,0x4F);
    VddSetAl(&registers,0x07);
    VddSetBx(&registers,0x00);
    VddSetCx(&registers,0);
    VddSetDx(&registers,480);
    VddBusDeliverInterrupt(&bus,0x10,&registers);
    g_Video.VesaVram[480u*1280u + 0] = 0xFF;
    g_Video.VesaVram[480u*1280u + 1] = 0xFF;
    g_Video.IsDirty=1;
    VddBusFrame(&bus);
    CHECK(VddGetAx(&registers)==0x004F && g_Video.Frame.BitsPerPixel==32
          && ((const UINT32 *)(const VOID *)g_Video.Frame.Pixels)[0]==0xFFFFFFFFu,
          "vesa/4F07 (16bpp): page 2 pixel 0 = white after the flip");
    memset(&registers,0,sizeof registers);
    VddSetAh(&registers,0x4F);
    VddSetAl(&registers,0x02);
    VddSetBx(&registers,0x101);
    VddBusDeliverInterrupt(&bus,0x10,&registers);

    /* T12c: 1024x768 -- the list, the presenter cap and VRAM are sized from one number */
    { WORD segment=0x3100;
    PBYTE buffer=&g_GuestMemory[(segment<<4)];
      memset(&registers,0,sizeof registers);
      VddSetAh(&registers,0x4F);
      VddSetAl(&registers,0x01);
      VddSetCx(&registers,0x105);
      registers.Es=segment;
      registers.Edi=0;
      VddBusDeliverInterrupt(&bus,0x10,&registers);
      CHECK(VddGetAx(&registers)==0x004F && (buffer[18]|(buffer[19]<<8))==1024 && (buffer[20]|(buffer[21]<<8))==768 && buffer[25]==8 && (buffer[16]|(buffer[17]<<8))==1024,
            "vesa/4F01: 0x105 = 1024x768x8, pitch 1024");
      memset(&registers,0,sizeof registers);
      VddSetAh(&registers,0x4F);
      VddSetAl(&registers,0x02);
      VddSetBx(&registers,0x105);
      VddBusDeliverInterrupt(&bus,0x10,&registers);
      g_Video.IsDirty=1;
      VddBusFrame(&bus);
      CHECK(VddGetAx(&registers)==0x004F && g_Video.Frame.Width==1024 && g_Video.Frame.Height==768 && g_Video.Frame.BitsPerPixel==8, "vesa/4F02 0x105: frame 1024x768x8");
      memset(&registers,0,sizeof registers);
      VddSetAh(&registers,0x4F);
      VddSetAl(&registers,0x02);
      VddSetBx(&registers,0x4118);
      VddBusDeliverInterrupt(&bus,0x10,&registers);
      g_Video.VesaVram[(767u*1024u+1023u)*3+0]=0xFF;
      g_Video.VesaVram[(767u*1024u+1023u)*3+1]=0xFF;
      g_Video.VesaVram[(767u*1024u+1023u)*3+2]=0xFF;
      g_Video.IsDirty=1;
      VddBusFrame(&bus);
      CHECK(VddGetAx(&registers)==0x004F && g_Video.Frame.Width==1024 && g_Video.Frame.Height==768 && g_Video.Frame.BitsPerPixel==32
            && ((const UINT32 *)(const VOID *)g_Video.Frame.Pixels)[767u*1024u+1023u]==0xFFFFFFFFu,
            "vesa/4F02 0x4118: 1024x768x24 LFB, last pixel reaches the ARGB frame");
      CHECK(NTVDD_FRAME_MAX_WIDTH>=1024 && NTVDD_FRAME_MAX_HEIGHT>=768 && VIDEO_VESA_VRAM>=1024u*768u*3u, "sizes: presenter cap and VRAM hold 1024x768x24");
      memset(&registers,0,sizeof registers);
      VddSetAh(&registers,0x4F);
      VddSetAl(&registers,0x02);
      VddSetBx(&registers,0x0101);
      VddBusDeliverInterrupt(&bus,0x10,&registers); }

    /* T12e: VESA text modes 0x108..0x10C (132 columns) and 1280x1024 ------------ */
    { WORD segment=0x3100;
    PBYTE buffer=&g_GuestMemory[(segment<<4)];
    static BYTE biosData[0x100];
    g_Video.BiosData = biosData;
      memset(&registers,0,sizeof registers);
      VddSetAh(&registers,0x4F);
      VddSetAl(&registers,0x01);
      VddSetCx(&registers,0x109);
      registers.Es=segment;
      registers.Edi=0;
      VddBusDeliverInterrupt(&bus,0x10,&registers);
      CHECK(VddGetAx(&registers)==0x004F && !((buffer[0]|(buffer[1]<<8)) & 0x10) && (buffer[18]|(buffer[19]<<8))==132 && (buffer[20]|(buffer[21]<<8))==25
            && buffer[22]==8 && buffer[23]==16 && (buffer[16]|(buffer[17]<<8))==264 && buffer[27]==0 && (buffer[8]|(buffer[9]<<8))==0xB800,
            "vesa/4F01 0x109: text attrs, 132x25 chars, 8x16 cell, 264 bytes/line, model 0, window B800");
      memset(&registers,0,sizeof registers);
      VddSetAh(&registers,0x4F);
      VddSetAl(&registers,0x02);
      VddSetBx(&registers,0x109);
      VddBusDeliverInterrupt(&bus,0x10,&registers);
      CHECK(VddGetAx(&registers)==0x004F && g_Video.ModeKind==VIDEO_KIND_TEXT && g_Video.Columns==132 && g_Video.Rows==25 && g_Video.CellHeight==16 && !g_Video.IsVesa,
            "vesa/4F02 0x109: text kind, 132x25, not a graphics VESA state");
      memset(&registers,0,sizeof registers);
      VddSetAh(&registers,0x4F);
      VddSetAl(&registers,0x03);
      VddBusDeliverInterrupt(&bus,0x10,&registers);
      CHECK(VddGetAx(&registers)==0x004F && VddGetBx(&registers)==0x109, "vesa/4F03 in a VESA text mode: 0x109");
      memset(&registers,0,sizeof registers);
      VddSetAh(&registers,0x0F);
      VddBusDeliverInterrupt(&bus,0x10,&registers);
      CHECK((VddGetAx(&registers)>>8)==132 && biosData[0x4A]==132 && biosData[0x84]==24, "int10/0F + BDA: 132 columns, 25 rows");
      memset(&registers,0,sizeof registers);
      VddSetAh(&registers,0x02);
      VddSetDx(&registers,(WORD)((3<<8)|100));
      VddBusDeliverInterrupt(&bus,0x10,&registers);
      memset(&registers,0,sizeof registers);
      VddSetAh(&registers,0x0E);
      VddSetAl(&registers,'Z');
      VddBusDeliverInterrupt(&bus,0x10,&registers);
      CHECK(g_Video.VideoMemory[VIDEO_TEXT_OFFSET + (3*132+100)*2]=='Z', "text at (3,100): cell addressing uses 132 columns");
      g_Video.IsDirty=1;
      VddBusFrame(&bus);
      CHECK(g_Video.Frame.Width==1056 && g_Video.Frame.Height==400 && g_Video.Frame.BitsPerPixel==8, "frame(0x109): 1056x400");
      memset(&registers,0,sizeof registers);
      VddSetAh(&registers,0x4F);
      VddSetAl(&registers,0x02);
      VddSetBx(&registers,0x10C);
      VddBusDeliverInterrupt(&bus,0x10,&registers);
      g_Video.IsDirty=1;
      VddBusFrame(&bus);
      CHECK(VddGetAx(&registers)==0x004F && g_Video.Columns==132 && g_Video.Rows==60 && g_Video.CellHeight==8 && g_Video.Frame.Width==1056 && g_Video.Frame.Height==480,
            "vesa/4F02 0x10C: 132x60 at 8x8 -> 1056x480");
      memset(&registers,0,sizeof registers);
      VddSetAh(&registers,0x4F);
      VddSetAl(&registers,0x02);
      VddSetBx(&registers,0x108);
      VddBusDeliverInterrupt(&bus,0x10,&registers);
      CHECK(VddGetAx(&registers)==0x004F && g_Video.Columns==80 && g_Video.Rows==60, "vesa/4F02 0x108: 80x60");
      memset(&registers,0,sizeof registers);
      VddSetAh(&registers,0x00);
      VddSetAl(&registers,0x03);
      VddBusDeliverInterrupt(&bus,0x10,&registers);
      memset(&registers,0,sizeof registers);
      VddSetAh(&registers,0x4F);
      VddSetAl(&registers,0x03);
      VddBusDeliverInterrupt(&bus,0x10,&registers);
      CHECK(g_Video.Columns==80 && g_Video.Rows==25 && VddGetBx(&registers)==0x03, "int10/00 mode 3 leaves the VESA text mode; 4F03 = 3");
      memset(&registers,0,sizeof registers);
      VddSetAh(&registers,0x4F);
      VddSetAl(&registers,0x01);
      VddSetCx(&registers,0x107);
      registers.Es=segment;
      registers.Edi=0;
      VddBusDeliverInterrupt(&bus,0x10,&registers);
      CHECK(VddGetAx(&registers)==0x004F && (buffer[18]|(buffer[19]<<8))==1280 && (buffer[20]|(buffer[21]<<8))==1024 && buffer[25]==8, "vesa/4F01: 0x107 = 1280x1024x8");
      memset(&registers,0,sizeof registers);
      VddSetAh(&registers,0x4F);
      VddSetAl(&registers,0x02);
      VddSetBx(&registers,0x411B);
      VddBusDeliverInterrupt(&bus,0x10,&registers);
      g_Video.IsDirty=1;
      VddBusFrame(&bus);
      CHECK(VddGetAx(&registers)==0x004F && g_Video.Frame.Width==1280 && g_Video.Frame.Height==1024 && g_Video.Frame.BitsPerPixel==32, "vesa/4F02 0x411B: 1280x1024x24 LFB frame");
      g_Video.BiosData = 0;
      memset(&registers,0,sizeof registers);
      VddSetAh(&registers,0x4F);
      VddSetAl(&registers,0x02);
      VddSetBx(&registers,0x0101);
      VddBusDeliverInterrupt(&bus,0x10,&registers); }

    /* T12f: 4F15 VBE/DDC -- a synthesised EDID 1.3 block ------------------------ */
    { WORD segment=0x3500;
    PBYTE edid=&g_GuestMemory[(segment<<4)];
    UINT index;
    UINT sum=0;
      memset(&registers,0,sizeof registers);
      VddSetAh(&registers,0x4F);
      VddSetAl(&registers,0x15);
      VddSetBx(&registers,0x0000);
      registers.Es=0;
      registers.Edi=0;
      VddBusDeliverInterrupt(&bus,0x10,&registers);
      CHECK(VddGetAx(&registers)==0x004F && (VddGetBx(&registers)&0x03)!=0, "vesa/4F15 BL=00: DDC supported (DDC1 and/or DDC2 bits)");
      memset(edid, 0xEE, 256);
      memset(&registers,0,sizeof registers);
      VddSetAh(&registers,0x4F);
      VddSetAl(&registers,0x15);
      VddSetBx(&registers,0x0001);
      VddSetDx(&registers,0);
      registers.Es=segment;
      registers.Edi=0;
      VddBusDeliverInterrupt(&bus,0x10,&registers);
      for (index = 0; index < 128; ++index)
          sum += edid[index];
      CHECK(VddGetAx(&registers)==0x004F && edid[0]==0x00 && edid[1]==0xFF && edid[6]==0xFF && edid[7]==0x00 && (sum & 0xFF)==0
            && edid[18]==1 && edid[19]>=3 && edid[126]==0 && edid[128]==0xEE,
            "vesa/4F15 BL=01: EDID header, version 1.3+, checksum 0, no extensions, exactly 128 bytes written");
      memset(&registers,0,sizeof registers);
      VddSetAh(&registers,0x4F);
      VddSetAl(&registers,0x15);
      VddSetBx(&registers,0x0001);
      VddSetDx(&registers,1);
      registers.Es=segment;
      registers.Edi=0;
      VddBusDeliverInterrupt(&bus,0x10,&registers);
      CHECK(VddGetAx(&registers)!=0x004F, "vesa/4F15 BL=01 block 1: none, fails"); }

    /* T12d: 4F10 VBE/PM (DPMS) ----------------------------------------------- */
    memset(&registers,0,sizeof registers);
    VddSetAh(&registers,0x4F);
    VddSetAl(&registers,0x10);
    VddSetBx(&registers,0x0000);
    VddBusDeliverInterrupt(&bus,0x10,&registers);
    CHECK(VddGetAx(&registers)==0x004F && (VddGetBx(&registers)&0xFF)==0x10 && (VddGetBx(&registers)>>8)==0x0F, "vesa/4F10 report: VBE/PM 1.0, all four states");
    memset(&registers,0,sizeof registers);
    VddSetAh(&registers,0x4F);
    VddSetAl(&registers,0x10);
    VddSetBx(&registers,0x0401);
    VddBusDeliverInterrupt(&bus,0x10,&registers);
    CHECK(VddGetAx(&registers)==0x004F, "vesa/4F10 set: off");
    memset(&registers,0,sizeof registers);
    VddSetAh(&registers,0x4F);
    VddSetAl(&registers,0x10);
    VddSetBx(&registers,0x0002);
    VddBusDeliverInterrupt(&bus,0x10,&registers);
    CHECK(VddGetAx(&registers)==0x004F && (VddGetBx(&registers)>>8)==0x04, "vesa/4F10 get: off");
    memset(&registers,0,sizeof registers);
    VddSetAh(&registers,0x4F);
    VddSetAl(&registers,0x10);
    VddSetBx(&registers,0x0001);
    VddBusDeliverInterrupt(&bus,0x10,&registers);
    memset(&registers,0,sizeof registers);
    VddSetAh(&registers,0x4F);
    VddSetAl(&registers,0x10);
    VddSetBx(&registers,0x0002);
    VddBusDeliverInterrupt(&bus,0x10,&registers);
    CHECK((VddGetBx(&registers)>>8)==0x00, "vesa/4F10 set on, get: on");

    /* T12g: THE 4F08 DAC WIDTH REACHES THE PORTS (#226, VBE 2.0 section 4.11). -----------------
     * Capabilities D0 says the DAC switches to 8 bits; 4F08 BH=8 said it had. Port 3C9h
     * still stored `v & 3Fh` and read back `>> 2`, so a guest that switched and then
     * loaded its palette the usual way lost the top two bits of every primary. Every
     * expectation is the RAMDAC's: 8 bits in, 8 bits out; 6 bits = the low six, stored
     * as the top six of the register (so a width switch re-interprets, not rescales).
     */
    { UINT32 value;
    WORD segment=0x3200;
    PBYTE table=&g_GuestMemory[(segment<<4)];
      memset(&registers,0,sizeof registers);
      VddSetAh(&registers,0x4F);
      VddSetAl(&registers,0x02);
      VddSetBx(&registers,0x0101);
      VddBusDeliverInterrupt(&bus,0x10,&registers);
      memset(&registers,0,sizeof registers);
      VddSetAh(&registers,0x4F);
      VddSetAl(&registers,0x08);
      VddSetBx(&registers,0x0800);
      VddBusDeliverInterrupt(&bus,0x10,&registers);
      CHECK(VddGetAx(&registers)==0x004F && (VddGetBx(&registers)>>8)==8, "dac8: 4F08 BH=8 in 0x101 -> 8 bits");
      value=0x40;
      VddBusIo(&bus,0x3C8,1,0,&value);
      value=0x80;
      VddBusIo(&bus,0x3C9,1,0,&value);
      value=0xC0;
      VddBusIo(&bus,0x3C9,1,0,&value);
      value=0xFF;
      VddBusIo(&bus,0x3C9,1,0,&value);
      CHECK(g_Video.Dac[0x40]==0xFF80C0FFu && g_Video.Palette[0x40]==0xFF80C0FFu,
            "dac8: 3C9h carries all 8 bits (80,C0,FF) -- was masked to 00,00,3F<<2");
      { UINT32 red=0,green=0,blue=0;
      value=0x40;
      VddBusIo(&bus,0x3C7,1,0,&value);
        VddBusIo(&bus,0x3C9,1,1,&red);
        VddBusIo(&bus,0x3C9,1,1,&green);
        VddBusIo(&bus,0x3C9,1,1,&blue);
        CHECK(red==0x80 && green==0xC0 && blue==0xFF, "dac8: 3C9h reads back 8 bits, no >>2"); }
      memset(&registers,0,sizeof registers);
      VddSetAh(&registers,0x10);
      VddSetAl(&registers,0x10);
      VddSetBx(&registers,0x41);
      VddSetDx(&registers,0xFF00);
      VddSetCx(&registers,0x8001);
      VddBusDeliverInterrupt(&bus,0x10,&registers);
      CHECK(g_Video.Dac[0x41]==0xFFFF8001u, "dac8: INT 10h 1010h stores 8-bit primaries (a VGA BIOS just OUTs to 3C9h)");
      memset(&registers,0,sizeof registers);
      VddSetAh(&registers,0x10);
      VddSetAl(&registers,0x15);
      VddSetBx(&registers,0x41);
      VddBusDeliverInterrupt(&bus,0x10,&registers);
      CHECK((VddGetDx(&registers)>>8)==0xFF && VddGetCx(&registers)==0x8001, "dac8: INT 10h 1015h reads them back at 8 bits");
      memset(table,0xEE,8);
      memset(&registers,0,sizeof registers);
      VddSetAh(&registers,0x10);
      VddSetAl(&registers,0x17);
      VddSetBx(&registers,0x40);
      VddSetCx(&registers,2);
      registers.Es=segment;
      VddSetDx(&registers,0);
      VddBusDeliverInterrupt(&bus,0x10,&registers);
      CHECK(table[0]==0x80 && table[1]==0xC0 && table[2]==0xFF && table[3]==0xFF && table[4]==0x80 && table[5]==0x01 && table[6]==0xEE,
            "dac8: INT 10h 1017h block read at 8 bits, exactly 2x3 bytes");
      memset(table,0xEE,8);
      memset(&registers,0,sizeof registers);
      VddSetAh(&registers,0x4F);
      VddSetAl(&registers,0x09);
      VddSetBx(&registers,0x0001);
      VddSetCx(&registers,1);
      VddSetDx(&registers,0x40);
      registers.Es=segment;
      registers.Edi=0;
      VddBusDeliverInterrupt(&bus,0x10,&registers);
      CHECK(table[0]==0xFF && table[1]==0xC0 && table[2]==0x80 && table[3]==0, "dac8: 4F09 get agrees with the port (B,G,R,0)");
      /* A mode set returns the width to 6 (section 4.11) and the SAME register reads as its top six bits. */
      memset(&registers,0,sizeof registers);
      VddSetAh(&registers,0x4F);
      VddSetAl(&registers,0x02);
      VddSetBx(&registers,0x8101);
      VddBusDeliverInterrupt(&bus,0x10,&registers);
      memset(&registers,0,sizeof registers);
      VddSetAh(&registers,0x4F);
      VddSetAl(&registers,0x08);
      VddSetBx(&registers,0x0001);
      VddBusDeliverInterrupt(&bus,0x10,&registers);
      CHECK((VddGetBx(&registers)>>8)==6, "dac6: 4F02 put the width back to 6");
      g_Video.Dac[0x40] = 0xFF80C0FFu;
      { UINT32 red=0,green=0,blue=0;
      value=0x40;
      VddBusIo(&bus,0x3C7,1,0,&value);
        VddBusIo(&bus,0x3C9,1,1,&red);
        VddBusIo(&bus,0x3C9,1,1,&green);
        VddBusIo(&bus,0x3C9,1,1,&blue);
        CHECK(red==0x20 && green==0x30 && blue==0x3F, "dac6: the port reads the top six bits again (80,C0,FF -> 20,30,3F)"); }
      value=0x42;
      VddBusIo(&bus,0x3C8,1,0,&value);
      value=0xFF;
      VddBusIo(&bus,0x3C9,1,0,&value);
      value=0x40;
      VddBusIo(&bus,0x3C9,1,0,&value);
      value=0x3F;
      VddBusIo(&bus,0x3C9,1,0,&value);
      CHECK(g_Video.Dac[0x42]==0xFFFC00FCu, "dac6: 3C9h ignores bits 6-7 at 6 bits, as before (FF->3F, 40->00)");
      /* 4F09's 6-bit set used to shift without masking, spilling bits 6-7 into the next field */
      table[0]=0xFF;
      table[1]=0xC0;
      table[2]=0x00;
      table[3]=0;
      memset(&registers,0,sizeof registers);
      VddSetAh(&registers,0x4F);
      VddSetAl(&registers,0x09);
      VddSetBx(&registers,0x0000);
      VddSetCx(&registers,1);
      VddSetDx(&registers,0x43);
      registers.Es=segment;
      registers.Edi=0;
      VddBusDeliverInterrupt(&bus,0x10,&registers);
      CHECK(VddGetAx(&registers)==0x004F && g_Video.Dac[0x43]==0xFF0000FCu, "dac6: 4F09 set masks each primary to 6 bits (no spill into G/R)");
      table[0]=0x01;
      table[1]=0x02;
      table[2]=0x03;
      table[3]=0;
      memset(&registers,0,sizeof registers);
      VddSetAh(&registers,0x4F);
      VddSetAl(&registers,0x09);
      VddSetBx(&registers,0x0080);
      VddSetCx(&registers,1);
      VddSetDx(&registers,0x44);
      registers.Es=segment;
      registers.Edi=0;
      VddBusDeliverInterrupt(&bus,0x10,&registers);
      CHECK(VddGetAx(&registers)==0x004F && g_Video.Dac[0x44]==0xFF0C0804u && g_Video.Palette[0x44]==0xFF0C0804u,
            "4F09 BL=80h (set during retrace, blank bit): a set like 00h -- Capabilities D2 = 0");
      /* 4F08 in a standard mode: section 4.11 refuses only direct colour/YUV. Mode 13h drives the same DAC. */
      memset(&registers,0,sizeof registers);
      VddSetAh(&registers,0x00);
      VddSetAl(&registers,0x13);
      VddBusDeliverInterrupt(&bus,0x10,&registers);
      memset(&registers,0,sizeof registers);
      VddSetAh(&registers,0x4F);
      VddSetAl(&registers,0x08);
      VddSetBx(&registers,0x0800);
      VddBusDeliverInterrupt(&bus,0x10,&registers);
      CHECK(VddGetAx(&registers)==0x004F && (VddGetBx(&registers)>>8)==8, "dac8: 4F08 works in mode 13h (was 034Fh outside VESA)");
      value=0x05;
      VddBusIo(&bus,0x3C8,1,0,&value);
      value=0x81;
      VddBusIo(&bus,0x3C9,1,0,&value);
      value=0x82;
      VddBusIo(&bus,0x3C9,1,0,&value);
      value=0x83;
      VddBusIo(&bus,0x3C9,1,0,&value);
      CHECK(g_Video.Palette[0x05]==0xFF818283u, "dac8: mode 13h pixel 5 renders the 8-bit entry");
      memset(&registers,0,sizeof registers);
      VddSetAh(&registers,0x00);
      VddSetAl(&registers,0x13);
      VddBusDeliverInterrupt(&bus,0x10,&registers);
      memset(&registers,0,sizeof registers);
      VddSetAh(&registers,0x4F);
      VddSetAl(&registers,0x08);
      VddSetBx(&registers,0x0001);
      VddBusDeliverInterrupt(&bus,0x10,&registers);
      CHECK(VddGetAx(&registers)==0x004F && (VddGetBx(&registers)>>8)==6, "dac6: INT 10h AH=00h returns the width to 6");
      memset(&registers,0,sizeof registers);
      VddSetAh(&registers,0x4F);
      VddSetAl(&registers,0x02);
      VddSetBx(&registers,0x0101);
      VddBusDeliverInterrupt(&bus,0x10,&registers); }

    /* T12h: 4F07h BL=80h WAITS FOR THE RETRACE, and the start rides the latch (#226). --
     * VBE 2.0 section 4.10 "Set Display Start during Vertical Retrace". It returned at once and
     * paced nothing. A VESA mode also runs on its OWN timing now, not the last VGA
     * mode's CRTC: 0x101 is 640x480 at 60 Hz, 525 lines, retrace from line 480 --
     * F = 16666 us, retrace start at 480*F/525 = 15237 us into each frame.
     */
    { const UINT64 framePeriod = 16666u, duration = 1000u * 16666u, vblankStart = (480u * 16666u) / 525u;
      UINT32 value;
      g_Video.TimeUs = VideoTestFakeClock;
      g_Video.LatchTime = 0;
      g_FakeMicroseconds = duration + 1000;
      g_Video.IsDirty=1;
      VddBusFrame(&bus);              /* sync the latch */
      CHECK(VddVideoFrameUs(&g_Video)==16666u, "vesa beam: 640x480 is a 60 Hz frame, whatever the last VGA mode was");
      VddBusIo(&bus,0x3DA,1,1,&value);
      CHECK((value & 8)==0, "vesa beam: 3DAh in the picture at +1000us");
      g_FakeMicroseconds = duration + vblankStart + 20;
      VddBusIo(&bus,0x3DA,1,1,&value);
      CHECK((value & 8)!=0, "vesa beam: 3DAh in retrace from line 480 of 525 (+15237us)");
      g_FakeMicroseconds = duration + 2000;
      memset(&registers,0,sizeof registers);
      VddSetAh(&registers,0x4F);
      VddSetAl(&registers,0x07);
      VddSetBx(&registers,0x80);
      VddSetCx(&registers,0);
      VddSetDx(&registers,480);
      VddBusDeliverInterrupt(&bus,0x10,&registers);
      CHECK(VddGetAx(&registers)==0x004F && g_Video.Int10WaitUntil==duration+vblankStart && VddVideoInt10WaitUs(&g_Video)==(UINT32)(vblankStart-2000u),
            "4F07 BL=80h in the picture: completes at the retrace start (host waits the rest)");
      g_Video.IsDirty=1;
      VddBusFrame(&bus);
      CHECK(g_Video.Frame.Pixels==g_Video.VesaVram, "4F07 BL=80h: the old page is still displayed before the retrace");
      memset(&registers,0,sizeof registers);
      VddSetAh(&registers,0x4F);
      VddSetAl(&registers,0x07);
      VddSetBx(&registers,0x01);
      VddBusDeliverInterrupt(&bus,0x10,&registers);
      CHECK(VddGetCx(&registers)==0 && VddGetDx(&registers)==480, "4F07 BL=01h reports the start just set (the register), (0,480)");
      g_FakeMicroseconds = duration + vblankStart + 50;
      CHECK(VddVideoInt10WaitUs(&g_Video)==0 && g_Video.Int10WaitUntil==0, "the wait ends once the beam is in retrace, and clears");
      g_Video.IsDirty=1;
      VddBusFrame(&bus);
      CHECK(g_Video.Frame.Pixels==g_Video.VesaVram, "during the retrace the old picture is still the one up");
      g_FakeMicroseconds = duration + framePeriod + 100;
      g_Video.IsDirty=1;
      VddBusFrame(&bus);
      CHECK(g_Video.Frame.Pixels==g_Video.VesaVram + 480u*640u, "the next picture shows page 2 -- the retrace loaded it");
      /* called INSIDE a retrace nobody has used: returns at once, and THAT retrace takes it */
      g_FakeMicroseconds = duration + framePeriod + vblankStart + 30;
      memset(&registers,0,sizeof registers);
      VddSetAh(&registers,0x4F);
      VddSetAl(&registers,0x07);
      VddSetBx(&registers,0x80);
      VddSetCx(&registers,0);
      VddSetDx(&registers,0);
      VddBusDeliverInterrupt(&bus,0x10,&registers);
      CHECK(VddGetAx(&registers)==0x004F && g_Video.Int10WaitUntil==0 && VddVideoInt10WaitUs(&g_Video)==0,
            "4F07 BL=80h inside a fresh retrace: completes at once");
      g_FakeMicroseconds = duration + 2*framePeriod + 100;
      g_Video.IsDirty=1;
      VddBusFrame(&bus);
      CHECK(g_Video.Frame.Pixels==g_Video.VesaVram, "...and page 1 is on the very next picture");
      /* a SECOND call in the same retrace is paced to the next one: one flip a frame */
      g_FakeMicroseconds = duration + 2*framePeriod + vblankStart + 10;
      memset(&registers,0,sizeof registers);
      VddSetAh(&registers,0x4F);
      VddSetAl(&registers,0x07);
      VddSetBx(&registers,0x80);
      VddSetCx(&registers,0);
      VddSetDx(&registers,480);
      VddBusDeliverInterrupt(&bus,0x10,&registers);
      CHECK(g_Video.Int10WaitUntil==0, "first call in this retrace: at once");
      g_FakeMicroseconds = duration + 2*framePeriod + vblankStart + 40;
      memset(&registers,0,sizeof registers);
      VddSetAh(&registers,0x4F);
      VddSetAl(&registers,0x07);
      VddSetBx(&registers,0x80);
      VddSetCx(&registers,0);
      VddSetDx(&registers,0);
      VddBusDeliverInterrupt(&bus,0x10,&registers);
      CHECK(g_Video.Int10WaitUntil==duration+3*framePeriod+vblankStart, "second call in the same retrace: waits for the next one");
      /* BL=00h stays immediate: no wait, the display takes it now */
      g_FakeMicroseconds = duration + 3*framePeriod + 1000;
      memset(&registers,0,sizeof registers);
      VddSetAh(&registers,0x4F);
      VddSetAl(&registers,0x07);
      VddSetBx(&registers,0x00);
      VddSetCx(&registers,0);
      VddSetDx(&registers,480);
      VddBusDeliverInterrupt(&bus,0x10,&registers);
      g_Video.IsDirty=1;
      VddBusFrame(&bus);
      CHECK(g_Video.Int10WaitUntil==0 && g_Video.Frame.Pixels==g_Video.VesaVram + 480u*640u, "4F07 BL=00h: no wait, shown at once (unchanged)");
      /* 3.0 BL=02h: schedule a BYTE address, return at once; BL=04h reports the flip */
      memset(&registers,0,sizeof registers);
      VddSetAh(&registers,0x4F);
      VddSetAl(&registers,0x07);
      VddSetBx(&registers,0x02);
      registers.Ecx=0;
      VddBusDeliverInterrupt(&bus,0x10,&registers);
      CHECK(VddGetAx(&registers)==0x004F && g_Video.Int10WaitUntil==0, "4F07 BL=02h (3.0): scheduled, returns at once");
      memset(&registers,0,sizeof registers);
      VddSetAh(&registers,0x4F);
      VddSetAl(&registers,0x07);
      VddSetBx(&registers,0x04);
      VddBusDeliverInterrupt(&bus,0x10,&registers);
      CHECK(VddGetAx(&registers)==0x004F && VddGetCx(&registers)==0, "4F07 BL=04h: the flip has not happened in the picture");
      g_FakeMicroseconds = duration + 3*framePeriod + vblankStart + 10;
      memset(&registers,0,sizeof registers);
      VddSetAh(&registers,0x4F);
      VddSetAl(&registers,0x07);
      VddSetBx(&registers,0x04);
      VddBusDeliverInterrupt(&bus,0x10,&registers);
      CHECK(VddGetAx(&registers)==0x004F && VddGetCx(&registers)!=0, "4F07 BL=04h: ...and has once the retrace loaded it");
      memset(&registers,0,sizeof registers);
      VddSetAh(&registers,0x4F);
      VddSetAl(&registers,0x07);
      VddSetBx(&registers,0x02);
      registers.Ecx=640u*100u+8u;
      VddBusDeliverInterrupt(&bus,0x10,&registers);
      memset(&registers,0,sizeof registers);
      VddSetAh(&registers,0x4F);
      VddSetAl(&registers,0x07);
      VddSetBx(&registers,0x01);
      VddBusDeliverInterrupt(&bus,0x10,&registers);
      CHECK(VddGetCx(&registers)==8 && VddGetDx(&registers)==100, "4F07 BL=02h byte address 640*100+8 reads back as (8,100)");
      memset(&registers,0,sizeof registers);
      VddSetAh(&registers,0x4F);
      VddSetAl(&registers,0x07);
      VddSetBx(&registers,0x02);
      registers.Ecx=VIDEO_VESA_VRAM;
      VddBusDeliverInterrupt(&bus,0x10,&registers);
      CHECK(VddGetAx(&registers)==0x024F, "4F07 BL=02h past memory: AH=02");
      /* 3.0 BL=82h waits like 80h */
      g_FakeMicroseconds = duration + 4*framePeriod + 500;
      memset(&registers,0,sizeof registers);
      VddSetAh(&registers,0x4F);
      VddSetAl(&registers,0x07);
      VddSetBx(&registers,0x82);
      registers.Ecx=0;
      VddBusDeliverInterrupt(&bus,0x10,&registers);
      CHECK(VddGetAx(&registers)==0x004F && g_Video.Int10WaitUntil==duration+4*framePeriod+vblankStart, "4F07 BL=82h (3.0): waits for the retrace too");
      memset(&registers,0,sizeof registers);
      VddSetAh(&registers,0x4F);
      VddSetAl(&registers,0x07);
      VddSetBx(&registers,0x03);
      VddBusDeliverInterrupt(&bus,0x10,&registers);
      CHECK(VddGetAx(&registers)==0x014F, "4F07 BL=03h stereo: 014Fh, no such hardware");
      /* a stale stamp can never park the guest */
      g_Video.Int10WaitUntil = g_FakeMicroseconds + 5000000u;
      CHECK(VddVideoInt10WaitUs(&g_Video)==0 && g_Video.Int10WaitUntil==0, "a wait stamp > 1 s out is dropped, not honoured");
      g_Video.TimeUs = 0;
      memset(&registers,0,sizeof registers);
      VddSetAh(&registers,0x4F);
      VddSetAl(&registers,0x07);
      VddSetBx(&registers,0x80);
      VddSetCx(&registers,0);
      VddSetDx(&registers,0);
      VddBusDeliverInterrupt(&bus,0x10,&registers);
      CHECK(VddGetAx(&registers)==0x004F && g_Video.Int10WaitUntil==0 && VddVideoInt10WaitUs(&g_Video)==0, "no clock: BL=80h never waits");
      memset(&registers,0,sizeof registers);
      VddSetAh(&registers,0x4F);
      VddSetAl(&registers,0x02);
      VddSetBx(&registers,0x0101);
      VddBusDeliverInterrupt(&bus,0x10,&registers); }

    /* T12i: 4F03h RETURNS D14/D15, 40:87h BIT 7 RECORDS D15, AND A VESA MODE IS NOT THE
     * PREVIOUS VGA MODE WEARING A NEW NUMBER (#226). ----------------------------------
     * section 4.6: BX D14 linear, D15 memory not cleared; section 4.5: 2.0 BIOSes update 40:87h bit 7.
     * And after mode 12h, 4F02h left mkind PLANAR and chain-4 off -- the host kept
     * interpreting the guest and routing A0000 stores into the planes. The BDA values
     * are SeaVGABIOS's vga_set_mode(), read from QEMU's vgabios-stdvga.bin.
     */
    { static BYTE biosData[0x100];
    g_Video.BiosData = biosData;
      memset(&registers,0,sizeof registers);
      VddSetAh(&registers,0x4F);
      VddSetAl(&registers,0x02);
      VddSetBx(&registers,0xC101);
      VddBusDeliverInterrupt(&bus,0x10,&registers);
      memset(&registers,0,sizeof registers);
      VddSetAh(&registers,0x4F);
      VddSetAl(&registers,0x03);
      VddBusDeliverInterrupt(&bus,0x10,&registers);
      CHECK(VddGetAx(&registers)==0x004F && VddGetBx(&registers)==0xC101, "4F03 after 4F02 C101h: C101h -- D14 and D15 kept (was 0101h)");
      CHECK(biosData[0x87]==0xE0, "40:87h bit 7 set by 4F02 D15 (60h -> E0h)");
      { WORD saved = VddGetBx(&registers);                 /* the save/restore idiom: 4F03 -> 4F02 */
        memset(&registers,0,sizeof registers);
        VddSetAh(&registers,0x4F);
        VddSetAl(&registers,0x02);
        VddSetBx(&registers,0x0101);
        VddBusDeliverInterrupt(&bus,0x10,&registers);
        CHECK(g_Video.IsVesaLfb==0 && biosData[0x87]==0x60, "4F02 0101h: banked, 40:87h bit 7 clear");
        memset(&registers,0,sizeof registers);
        VddSetAh(&registers,0x4F);
        VddSetAl(&registers,0x02);
        VddSetBx(&registers,saved);
        VddBusDeliverInterrupt(&bus,0x10,&registers);
        CHECK(VddGetAx(&registers)==0x004F && g_Video.IsVesaLfb==1, "re-setting what 4F03 returned comes back LINEAR, not banked"); }
      memset(&registers,0,sizeof registers);
      VddSetAh(&registers,0x00);
      VddSetAl(&registers,0x83);
      VddBusDeliverInterrupt(&bus,0x10,&registers);
      memset(&registers,0,sizeof registers);
      VddSetAh(&registers,0x4F);
      VddSetAl(&registers,0x03);
      VddBusDeliverInterrupt(&bus,0x10,&registers);
      CHECK(biosData[0x87]==0xE0 && VddGetBx(&registers)==0x8003, "INT 10h AH=00h AL=83h: 40:87h bit 7 set, 4F03 = 8003h");
      memset(&registers,0,sizeof registers);
      VddSetAh(&registers,0x00);
      VddSetAl(&registers,0x03);
      VddBusDeliverInterrupt(&bus,0x10,&registers);
      CHECK(biosData[0x87]==0x60, "INT 10h AH=00h AL=03h: 40:87h back to 60h");
      /* section 4.5: D14 on a mode with no linear frame buffer (a text mode) fails, nothing changes */
      memset(&registers,0,sizeof registers);
      VddSetAh(&registers,0x4F);
      VddSetAl(&registers,0x02);
      VddSetBx(&registers,0x4109);
      VddBusDeliverInterrupt(&bus,0x10,&registers);
      CHECK(VddGetAx(&registers)==0x014F && g_Video.Columns==80 && g_Video.VesaTextMode==0, "4F02 4109h (text + LFB): 014Fh, still mode 3");
      memset(&registers,0,sizeof registers);
      VddSetAh(&registers,0x4F);
      VddSetAl(&registers,0x02);
      VddSetBx(&registers,0x8109);
      VddBusDeliverInterrupt(&bus,0x10,&registers);
      memset(&registers,0,sizeof registers);
      VddSetAh(&registers,0x4F);
      VddSetAl(&registers,0x03);
      VddBusDeliverInterrupt(&bus,0x10,&registers);
      CHECK(VddGetBx(&registers)==0x8109, "4F03 in a VESA text mode set with D15: 8109h");
      /* mode 12h, then a VESA mode: the planar machinery must not survive into it */
      memset(&registers,0,sizeof registers);
      VddSetAh(&registers,0x00);
      VddSetAl(&registers,0x12);
      VddBusDeliverInterrupt(&bus,0x10,&registers);
      VideoTestWriteGraphics(&bus, 0x05, 0x02);                      /* write mode 2, as a planar guest leaves it */
      CHECK(VddVideoIsPlanarActive(&g_Video) && g_Video.IsChain4==0, "mode 12h: planar, chain-4 off (the precondition)");
      memset(&registers,0,sizeof registers);
      VddSetAh(&registers,0x4F);
      VddSetAl(&registers,0x02);
      VddSetBx(&registers,0x0101);
      VddBusDeliverInterrupt(&bus,0x10,&registers);
      CHECK(!VddVideoIsPlanarActive(&g_Video) && g_Video.ModeKind==VIDEO_KIND_LINEAR8 && g_Video.IsChain4==1
            && g_Video.WriteMode==0 && g_Video.MapMask==0x0F,
            "4F02 after 12h: not planar (host stops interpreting), chained, write mode 0 -- was PLANAR");
      { UINT32 value = 0x04;
      VddBusIo(&bus,0x3C4,1,0,&value);
      value = 0;
      VddBusIo(&bus,0x3C5,1,1,&value);
        CHECK((value & 0x08)!=0, "4F02: SR4 reads back chain-4 on (mode 13h's register file)"); }
      CHECK(g_Video.GraphicsWidth==640 && g_Video.GraphicsHeight==480, "4F02: gw/gh are the VESA mode's extent (were mode 12h's by luck)");
      CHECK(biosData[0x49]==0xFF && biosData[0x4A]==80 && biosData[0x84]==29 && biosData[0x85]==16 && biosData[0x62]==0,
            "4F02 0101h BDA: 40:49=FFh, 4A=80 cols, 84=29, 85=16 (SeaVGABIOS vga_set_mode)");
      memset(&registers,0,sizeof registers);
      VddSetAh(&registers,0x0F);
      VddBusDeliverInterrupt(&bus,0x10,&registers);
      CHECK(VddGetAl(&registers)==0xFF && (VddGetAx(&registers)>>8)==80, "INT 10h AH=0Fh in a VESA mode: AL=FFh (40:49h), AH=80");
      g_VideoMemory[5] = 0x33;
      g_Video.IsDirty=1;
      VddBusFrame(&bus);
      CHECK(g_Video.VesaVram[5]==0x33 && g_Video.Frame.Pixels[5]==0x33, "4F02 after 12h: an A0000 store reaches the VESA picture");
      memset(&registers,0,sizeof registers);
      VddSetAh(&registers,0x4F);
      VddSetAl(&registers,0x02);
      VddSetBx(&registers,0x010E);
      VddBusDeliverInterrupt(&bus,0x10,&registers);
      CHECK(biosData[0x4A]==40 && biosData[0x84]==24 && biosData[0x85]==8, "4F02 010Eh (320x200): 40 cols, 25 rows of 8x8");
      memset(&registers,0,sizeof registers);
      VddSetAh(&registers,0x4F);
      VddSetAl(&registers,0x02);
      VddSetBx(&registers,0x0107);
      VddBusDeliverInterrupt(&bus,0x10,&registers);
      CHECK(biosData[0x4A]==160 && biosData[0x84]==63, "4F02 0107h (1280x1024): 160 cols, 64 rows");
      memset(&registers,0,sizeof registers);
      VddSetAh(&registers,0x00);
      VddSetAl(&registers,0x03);
      VddBusDeliverInterrupt(&bus,0x10,&registers);
      CHECK(biosData[0x49]==0x03 && g_Video.ModeKind==VIDEO_KIND_TEXT, "INT 10h AH=00h 03h after VESA: 40:49=03h, text");
      g_Video.BiosData = 0;
      memset(&registers,0,sizeof registers);
      VddSetAh(&registers,0x4F);
      VddSetAl(&registers,0x02);
      VddSetBx(&registers,0x0101);
      VddBusDeliverInterrupt(&bus,0x10,&registers); }

    /* T13: mode 12h planar -- set mode, plot a pixel, check planes + render --- */
    memset(&registers,0,sizeof registers);
    VddSetAh(&registers,0x00);
    VddSetAl(&registers,0x12);
    VddBusDeliverInterrupt(&bus,0x10,&registers);
    CHECK(g_Video.Mode==0x12, "int10/00: mode set to 12h");
    CHECK(g_Video.IsChain4==0 && g_Video.MapMask==0x0F, "int10/00 mode 12h: the BIOS's SR4 (chain-4 OFF) and SR2 (0Fh) are modelled -- Hexen's loader, s74b");
    CHECK(VddGetAl(&registers)==0x20, "int10/00 mode 12h: AL=20h video-mode flag (AMI ROM and SeaBIOS agree; was the mode number)");
    memset(&registers,0,sizeof registers);
    VddSetAh(&registers,0x00);
    VddSetAl(&registers,0x06);
    VddBusDeliverInterrupt(&bus,0x10,&registers);
    CHECK(VddGetAl(&registers)==0x3F, "int10/00 mode 6: AL=3Fh");
    memset(&registers,0,sizeof registers);
    VddSetAh(&registers,0x00);
    VddSetAl(&registers,0x03);
    VddBusDeliverInterrupt(&bus,0x10,&registers);
    CHECK(VddGetAl(&registers)==0x30, "int10/00 mode 3: AL=30h");
    memset(&registers,0,sizeof registers);
    VddSetAh(&registers,0x4F);
    VddSetAl(&registers,0x0A);
    VddSetBx(&registers,0);
    VddBusDeliverInterrupt(&bus,0x10,&registers);
    /* #53: 4F0Ah now hands out a real PM interface (vbepm_test.c runs its code). It was
     * AX=0100h -- what the two real BIOSes answer -- while there was nothing to hand out.
     */
    CHECK(VddGetAx(&registers)==0x004F && registers.Es==VDD_VBEPM_SEG, "vesa/4F0A: AX=004F, ES:DI = the PM interface block (#53; was 0100)");
    memset(&registers,0,sizeof registers);
    VddSetAh(&registers,0x00);
    VddSetAl(&registers,0x12);
    VddBusDeliverInterrupt(&bus,0x10,&registers);
    /* plot (x=9,y=1) colour 0x0A (1010b -> planes 1 and 3) */
    memset(&registers,0,sizeof registers);
    VddSetAh(&registers,0x0C);
    VddSetAl(&registers,0x0A);
    VddSetCx(&registers,9);
    VddSetDx(&registers,1);
    VddBusDeliverInterrupt(&bus,0x10,&registers);
    { UINT32 byte = 1*(VIDEO_MODE12_WIDTH/8) + (9>>3);
    BYTE bit = 0x80>>(9&7);
      CHECK((g_Video.Planes[1][byte]&bit) && (g_Video.Planes[3][byte]&bit)
            && !(g_Video.Planes[0][byte]&bit) && !(g_Video.Planes[2][byte]&bit),
            "mode12: AH=0C set planes 1+3 for colour 0x0A"); }
    g_Video.IsDirty=1;
    VddBusFrame(&bus);
    CHECK(g_Video.Frame.Width==640 && g_Video.Frame.Height==480, "frame(mode12): 640x480x8");
    CHECK(g_Video.FrameBuffer[1*VIDEO_MODE12_WIDTH + 9]==0x0A, "mode12: plane-combine render -> pixel = 0x0A");

    /* T14: planar write-mode 0 + Map Mask (the common plane-fill path) -------- */
    memset(&registers,0,sizeof registers);
    VddSetAh(&registers,0x00);
    VddSetAl(&registers,0x12);
    VddBusDeliverInterrupt(&bus,0x10,&registers); /* clears planes */
    { UINT32 value;
      value=2;
      VddBusIo(&bus,0x3C4,1,0,&value);
      value=0x0F;
      VddBusIo(&bus,0x3C5,1,0,&value);   /* map mask=0F */
      value=5;
      VddBusIo(&bus,0x3CE,1,0,&value);
      value=0;
      VddBusIo(&bus,0x3CF,1,0,&value);   /* write mode 0 */
      CHECK(g_Video.MapMask==0x0F && g_Video.WriteMode==0, "planar: ports set map_mask/write_mode");
      VddVideoPlanarWrite(&g_Video, 0, 0xAA);
      CHECK(g_Video.Planes[0][0]==0xAA && g_Video.Planes[1][0]==0xAA && g_Video.Planes[2][0]==0xAA && g_Video.Planes[3][0]==0xAA,
            "planar wm0: byte -> all enabled planes");
      value=2;
      VddBusIo(&bus,0x3C4,1,0,&value);
      value=0x05;
      VddBusIo(&bus,0x3C5,1,0,&value);   /* map mask=05 */
      VddVideoPlanarWrite(&g_Video, 1, 0xFF);
      CHECK(g_Video.Planes[0][1]==0xFF && g_Video.Planes[2][1]==0xFF && g_Video.Planes[1][1]==0 && g_Video.Planes[3][1]==0,
            "planar wm0: map mask gates planes (0+2 only)"); }

    /* T15: planar write-mode 2 (CPU bit p -> plane p) ------------------------ */
    { UINT32 value;
    value=5;
    VddBusIo(&bus,0x3CE,1,0,&value);
    value=2;
    VddBusIo(&bus,0x3CF,1,0,&value); /* wm2 */
      value=2;
      VddBusIo(&bus,0x3C4,1,0,&value);
      value=0x0F;
      VddBusIo(&bus,0x3C5,1,0,&value);          /* mask 0F */
      VddVideoPlanarWrite(&g_Video, 2, 0x0A);   /* colour 1010b -> planes 1 and 3 */
      CHECK(g_Video.Planes[1][2]==0xFF && g_Video.Planes[3][2]==0xFF && g_Video.Planes[0][2]==0 && g_Video.Planes[2][2]==0,
            "planar wm2: colour 0x0A -> planes 1+3 (all 8 px)"); }

    /* T16: latch read loads all planes ------------------------------------- */
    g_Video.Planes[0][3]=0x11;
    g_Video.Planes[1][3]=0x22;
    g_Video.Planes[2][3]=0x33;
    g_Video.Planes[3][3]=0x44;
    { /* read_map=2 */
        UINT32 value;
        value=4;
        VddBusIo(&bus,0x3CE,1,0,&value);
        value=2;
        VddBusIo(&bus,0x3CF,1,0,&value);
    }
    { BYTE actual = VddVideoPlanarRead(&g_Video, 3);
      CHECK(actual==0x33 && g_Video.Latch[0]==0x11 && g_Video.Latch[3]==0x44, "planar read: latches + read_map=2"); }

    /* T17: 0x3DA vertical retrace is TIMED, not toggled (GH #55 follow-up) ----- *
     * The old implementation flipped bits 3 and 0 on every read, so `WAIT &H3DA,8`
     * -- the frame clock of most DOS graphics code -- returned instantly and those
     * programs ran unbounded. A fake clock makes the real thing deterministic to
     * test: set the microsecond time, read the port, assert the bits.
     */
    { UINT32 value;
    INT index;
    INT highCount;
    INT lowCount;
      g_Video.TimeUs = VideoTestFakeClock;

      /* --- 640x480 (mode 12h): 60 Hz, 525 lines, 480 active --------------- */
      g_Video.GraphicsHeight = 480;
      g_FakeMicroseconds = 0;                       /* line 0 = active picture */
      VddBusIo(&bus, 0x3DA, 1, 1, &value);
      CHECK(!(value & 0x08), "3DA: no retrace during the active picture");

      g_FakeMicroseconds = 16000;                   /* 16.0ms of a 16.67ms frame = vblank */
      VddBusIo(&bus, 0x3DA, 1, 1, &value);
      CHECK((value & 0x08) != 0, "3DA: retrace asserted during vertical blanking");
      CHECK((value & 0x01) != 0, "3DA: display-disabled set during vblank too");

      /* --- IT DOES NOT ALTERNATE. Two reads at the SAME instant must agree; the
       * old toggle failed exactly here, and that was the whole bug. -------
       */
      { UINT32 firstRead, secondRead;
        g_FakeMicroseconds = 1000;
        VddBusIo(&bus, 0x3DA, 1, 1, &firstRead);
        VddBusIo(&bus, 0x3DA, 1, 1, &secondRead);
        CHECK(firstRead == secondRead, "3DA: two reads at the same instant agree (no toggle)"); }

      /* --- Duty cycle: retrace must be a MINORITY of the frame, or a program
       * that waits for it to clear stalls. Sample a whole frame. --------
       */
      highCount = lowCount = 0;
      for (index = 0; index < 1000; index++)
      {
          g_FakeMicroseconds = (UINT64)(index * 16667 / 1000);       /* one 60 Hz frame */
          VddBusIo(&bus, 0x3DA, 1, 1, &value);
          if (value & 0x08)
              highCount++;
          else
              lowCount++;
      }
      CHECK(highCount > 0 && lowCount > 0, "3DA: retrace both asserted and clear across a frame");
      CHECK(highCount < lowCount / 4, "3DA: retrace is a small minority of the frame (~9%)");

      /* --- 320x200 / text run at 70 Hz, so the SAME wall-clock instant lands
       * differently. [CAUTION] This used to key off `vid.gh` alone; the CRTC is now
       * authoritative and a mode set earlier in this battery left 12h's
       * 525-line timing behind, so the 70 Hz family has to be asked for in
       * the registers. These are mode 03h/0Dh/13h's, from vga_defaults.h:
       * Vertical Total 0xBF|bit8 = 449 lines, Blank Start 0x96|bit8 = 406.
       */
      g_Video.GraphicsHeight = 400;
      VideoTestWriteCrtc(&bus, 0x06, 0xBF);
      VideoTestWriteCrtc(&bus, 0x07, 0x1F);
      VideoTestWriteCrtc(&bus, 0x09, 0x41);
      VideoTestWriteCrtc(&bus, 0x12, 0x8F);
      VideoTestWriteCrtc(&bus, 0x15, 0x96);
      highCount = lowCount = 0;
      for (index = 0; index < 1000; index++)
      {
          g_FakeMicroseconds = (UINT64)(index * 14286 / 1000);       /* one 70 Hz frame */
          VddBusIo(&bus, 0x3DA, 1, 1, &value);
          if (value & 0x08)
              highCount++;
          else
              lowCount++;
      }
      CHECK(highCount > 0 && lowCount > 0, "3DA: 70 Hz modes also retrace once per frame");

      /* --- #225: a THROTTLED guest held across a whole retrace is owed it, once.
       * Polls at 1 ms into consecutive 70 Hz frames never land in the ~1.2 ms
       * blank. Off (the default): bit 3 is never seen. On: the first poll in the
       * next frame reads it, the one after reads the true phase, and skipping
       * several frames still owes only ONE. --------------------------------
       */
      { UINT32 firstRead, secondRead, thirdRead, fourthRead, owedBefore;
      UINT64 time;
      UINT64 firstRetrace = 0;
      UINT64 secondRetrace = 0;
      UINT64 framePeriod;
      UINT64 pollTime;
        /* The frame is MEASURED off the model rather than assumed: find two
         * successive retrace starts, then poll 2 ms before one (active picture).
         */
        g_Video.IsVblOweOn = 0;
        for (time = 0; time < 100000 && !secondRetrace; ++time)
        {
            g_FakeMicroseconds = time;
            VddBusIo(&bus, 0x3DA, 1, 1, &firstRead);
            if ((firstRead & 0x08) && time && !(thirdRead & 0x08))
            {
                if (!firstRetrace)
                    firstRetrace = time;
                else
                    secondRetrace = time;
            }
            thirdRead = firstRead;
        }
        framePeriod = secondRetrace - firstRetrace;
        pollTime = secondRetrace + 10*framePeriod - 2000;       /* active, well clear of the blank */
        g_Video.IsPort3DaHaveLast = 0;
        g_FakeMicroseconds = pollTime;
        VddBusIo(&bus, 0x3DA, 1, 1, &firstRead);
        g_FakeMicroseconds = pollTime + framePeriod;
        VddBusIo(&bus, 0x3DA, 1, 1, &secondRead);
        CHECK(framePeriod > 10000 && framePeriod < 20000 && !(firstRead & 0x08) && !(secondRead & 0x08),
              "3DA #225: off, a retrace slept through stays unseen");
        g_Video.IsVblOweOn = 1;
        owedBefore = g_Video.Port3DaVblOwed;
        g_FakeMicroseconds = pollTime + 2*framePeriod;
        VddBusIo(&bus, 0x3DA, 1, 1, &firstRead);
        g_FakeMicroseconds = pollTime + 2*framePeriod + 10;
        VddBusIo(&bus, 0x3DA, 1, 1, &secondRead);
        CHECK((firstRead & 0x09) == 0x09, "3DA #225: on, the missed retrace is reported on the next poll");
        CHECK(!(secondRead & 0x08), "3DA #225: ...once; the following poll reads the true phase");
        g_FakeMicroseconds = pollTime + 6*framePeriod;
        VddBusIo(&bus, 0x3DA, 1, 1, &thirdRead);
        g_FakeMicroseconds = pollTime + 6*framePeriod + 10;
        VddBusIo(&bus, 0x3DA, 1, 1, &fourthRead);
        CHECK((thirdRead & 0x08) && !(fourthRead & 0x08) && g_Video.Port3DaVblOwed == owedBefore + 2,
              "3DA #225: four frames skipped still owe ONE retrace");
        g_FakeMicroseconds = pollTime + 6*framePeriod + 20;
        VddBusIo(&bus, 0x3DA, 1, 1, &thirdRead);
        CHECK(!(thirdRead & 0x08), "3DA #225: nothing owed within one frame");
        g_Video.IsVblOweOn = 0; }

      /* --- bit 0 is a DIFFERENT signal: it must change WITHIN one scanline,
       * which the old code (toggling it with bit 3) could never do. -----
       */
      { INT changed = 0;
      UINT32 prev = 0xFF;
        g_Video.GraphicsHeight = 480;
        for (index = 0; index < 40; index++)                          /* ~1.3 scanlines */
        {
            g_FakeMicroseconds = (UINT64)index;                      /* 1 us steps */
            VddBusIo(&bus, 0x3DA, 1, 1, &value);
            if (prev != 0xFF && (value & 1) != (prev & 1))
                changed = 1;
            prev = value;
        }
        CHECK(changed, "3DA: display-disabled (bit 0) toggles within a scanline"); }
      /* --- No clock injected -> the legacy toggle still applies, so off-VM
       * callers that never set a clock are unaffected. ------------------
       */
      { UINT32 firstRead, secondRead;
        g_Video.TimeUs = 0;
        VddBusIo(&bus, 0x3DA, 1, 1, &firstRead);
        VddBusIo(&bus, 0x3DA, 1, 1, &secondRead);
        CHECK(firstRead != secondRead, "3DA: with no clock injected the legacy toggle remains"); }
    }

    /* THE BLANKING INTERVAL COMES FROM THE CRTC, NOT FROM A TWO-CASE GUESS (Importance = 2):
     * The old model asserted retrace from line 400 (or 480 for tall modes). That is
     * within 8 lines of the truth for every mode the BIOS sets EXCEPT 0Fh/10h --
     * 640x350 -- where real blanking starts at line 355 of 449 and the guess said
     * 400. It reported a 10.9% blanking interval where the card gives 20.9%: less
     * than half. [CAUTION] 640x350 is Lemmings' MENU screen -- its gameplay is mode 0Dh,
     * 320x200, measured from the BDA of a dump of the real game.
     *
     * [CAUTION]: EVERY NUMBER BELOW IS DECODED FROM g_VgaCrtcDefaults in vga_defaults.h, which
     * was read back off a real card by tests/probes/dos/vgadefs.asm. None of it is
     * written from memory, and none of it is this implementation's own opinion --
     * that is the whole point, and it is what caught the bug.
     *
     * mode 10h: CR06=0xBF CR07=0x1F CR09=0x40 CR12=0x5D CR15=0x63
     *   Vertical Total       = 0xBF | ov bit0<<8              = 447, +2 = 449 lines
     *   Vertical Display End = 0x5D | ov bit1<<8              = 349, +1 = 350 active
     *   Vertical Blank Start = 0x63 | ov bit3<<8 | ms bit5<<9 = 355
     */
    {   UINT32 value;
    INT index;
    INT highCount;
    INT lowCount;
    INT lowEdge = -1;
        g_Video.TimeUs = VideoTestFakeClock;
        g_Video.GraphicsHeight = 350;                      /* what the old model could not express */
        VideoTestWriteCrtc(&bus, 0x06, 0xBF);
        VideoTestWriteCrtc(&bus, 0x07, 0x1F);
        VideoTestWriteCrtc(&bus, 0x09, 0x40);
        VideoTestWriteCrtc(&bus, 0x12, 0x5D);
        VideoTestWriteCrtc(&bus, 0x15, 0x63);

        /* Line 354 is still picture, line 356 is blanked. Straddling the boundary is
         * the whole claim -- a duty-cycle count alone would pass on a window in the
         * wrong PLACE, so pin the edge itself. 449 lines in 1000000/70 us.
         */
        g_FakeMicroseconds = (UINT64)(354.0 * (1000000.0 / 70.0) / 449.0);
        VddBusIo(&bus, 0x3DA, 1, 1, &value);
        CHECK(!(value & 0x08), "3DA/CRTC: 640x350 line 354 is still the active picture");
        g_FakeMicroseconds = (UINT64)(357.0 * (1000000.0 / 70.0) / 449.0);
        VddBusIo(&bus, 0x3DA, 1, 1, &value);
        CHECK((value & 0x08) != 0, "3DA/CRTC: 640x350 blanking has begun by line 357");

        /* ...and the duty cycle that follows from it: 94 of 449 lines = 20.9%. The
         * old model gave 49 of 449 = 10.9%, so a >15% floor separates them.
         */
        highCount = lowCount = 0;
        for (index = 0; index < 1000; index++)
        {
            g_FakeMicroseconds = (UINT64)((double)index * (1000000.0 / 70.0) / 1000.0);
            VddBusIo(&bus, 0x3DA, 1, 1, &value);
            if (value & 0x08)
            {
                highCount++;
                if (lowEdge < 0)
                    lowEdge = index;
            }
            else
                lowCount++;
        }
        CHECK(highCount > 150 && highCount < 260, "3DA/CRTC: 640x350 blanks for ~20.9% of the frame");
        CHECK(lowCount > 0, "3DA/CRTC: 640x350 still shows a picture for most of the frame");

        /* A HALF-WRITTEN MODE SET MUST NOT BE BELIEVED. Blank Start before Display
         * End is not a screen; the guest is mid-reprogram. Falling back to the old
         * constants is survivable, a garbage frame period is not -- it would freeze
         * every guest that waits on retrace.
         */
        VideoTestWriteCrtc(&bus, 0x15, 0x10);          /* blank start 16, well above the picture */
        highCount = lowCount = 0;
        for (index = 0; index < 200; index++)
        {
            g_FakeMicroseconds = (UINT64)((double)index * (1000000.0 / 70.0) / 200.0);
            VddBusIo(&bus, 0x3DA, 1, 1, &value);
            if (value & 0x08)
                highCount++;
            else
                lowCount++;
        }
        CHECK(highCount > 0 && lowCount > highCount, "3DA/CRTC: an impossible blank start falls back, still sane");
        g_Video.TimeUs = 0;
    }

    /* LEMMINGS' OWN BLITTERS, REPLAYED REGISTER FOR REGISTER (Importance = 3):
     * Not invented, and not read off a datasheet: the register sequences the game
     * was OBSERVED to program -- the rig's IO-SITE and read/write-site instruments,
     * cross-checked against the game running under genuine MS-DOS 6.22
     * (scripts/lemref.py).
     *
     * Why these two and not some other pair: they are the ONLY two sites in the game
     * that read video memory -- the read-site histogram attributes every one of
     * ~970,000 reads in a level to them, with zero lost to hash collisions. Pin these
     * and the whole of Lemmings' drawing is pinned.
     *
     * [CAUTION]: THE VALUE IS THAT A SHIPPING 1991 GAME IS THE EXPECTATION. These sequences ran
     * on real hardware; if our VGA disagrees with them it is our VGA that is wrong.
     * That is a different and stronger claim than "it matches our reading of the spec".
     */
    {   BYTE actual;

        /* -- (1) THE MASKED SPRITE BLITTER, the colour-compare path. Set-up:
         *   GR5 = 0x08  read mode 1
         *   GR7 = 0x08  don't care: plane 3 only
         *   GR2 = 0x08  compare: plane 3 SET
         * and then, per byte: a colour-compare read, its complement into the Bit
         * Mask (GR8), and one byte of sprite written --
         * i.e. "which pixels here are colour 8..15? -- write my sprite into the
         * OTHERS." Transparency done by the card, one byte at a time. This is what
         * read mode 1 is actually for in this game; it is not collision detection.
         */
        memset(&registers,0,sizeof registers);
        VddSetAh(&registers,0x00);
        VddSetAl(&registers,0x12);
        VddBusDeliverInterrupt(&bus,0x10,&registers);
        g_Video.Planes[3][0x40] = 0xF0;     /* left four pixels are colour 8..15 = "terrain" */
        g_Video.Planes[0][0x40] = 0x00;
        VideoTestWriteGraphics(&bus, 0x00, 0x00);        /* GR0 set/reset        = 0 (as Lemmings does) */
        VideoTestWriteGraphics(&bus, 0x01, 0x00);        /* GR1 enable set/reset = 0 */
        VideoTestWriteGraphics(&bus, 0x05, 0x08);        /* GR5 READ MODE 1 */
        VideoTestWriteGraphics(&bus, 0x07, 0x08);        /* GR7 colour don't care */
        VideoTestWriteGraphics(&bus, 0x02, 0x08);        /* GR2 colour compare */
        actual = VddVideoPlanarRead(&g_Video, 0x40);
        CHECK(actual == 0xF0, "lemmings blit: colour-compare read names the colour-8..15 pixels");
        CHECK(g_Video.Latch[3] == 0xF0, "lemmings blit: ...and a mode-1 read still loads the latches");

        /* The complement -> 0x0F, straight into the Bit Mask, then one byte of sprite. */
        VideoTestWriteGraphics(&bus, 0x08, (BYTE)~actual);   /* GR8 bit mask = 0x0F */
        VideoTestWriteGraphics(&bus, 0x03, 0x00);            /* GR3 replace (the OR case is below) */
        VideoTestWriteGraphics(&bus, 0x05, 0x00);            /* back to write mode 0 to do the movsb */
        VideoTestWriteSequencer(&bus, 0x02, 0x01);            /* map mask = plane 0, as its per-plane loop */
        VddVideoPlanarWrite(&g_Video, 0x40, 0xFF);
        CHECK(g_Video.Planes[0][0x40] == 0x0F,
              "lemmings blit: the sprite lands ONLY where the terrain was not");
        CHECK(g_Video.Planes[3][0x40] == 0xF0,
              "lemmings blit: ...and the terrain plane is untouched by it");

        /* [CAUTION]: IT REALLY DOES USE THE ALU. The game also writes GR3 = 0x10, which is
         * function select = OR, not replace. A card that ignored GR3 would pass
         * every check above and still draw this game wrong, so pin the OR itself:
         * masked-in bits become (cpu OR latch), masked-out bits stay latch.
         */
        g_Video.Planes[0][0x41] = 0x55;                 /* latch source */
        VddVideoPlanarRead(&g_Video, 0x41);               /* load the latches */
        VideoTestWriteGraphics(&bus, 0x03, 0x10);                    /* GR3 = OR, exactly as Lemmings */
        VideoTestWriteGraphics(&bus, 0x08, 0x0F);                    /* bit mask: low nibble only */
        VddVideoPlanarWrite(&g_Video, 0x41, 0x0A);
        CHECK(g_Video.Planes[0][0x41] == 0x5F,
              "lemmings blit: GR3 function select ORs cpu with latch (0x5|0xA -> 0xF)");
        VideoTestWriteGraphics(&bus, 0x03, 0x00);
        VideoTestWriteGraphics(&bus, 0x08, 0xFF);

        /* -- (2) THE PLAIN BLITTER, which is the busier of the two (858,644 reads of
         * the 970,000). Per byte it reads VRAM and then copies one byte in.
         * The read's VALUE IS DISCARDED -- it is there to load the latches, so that
         * the planes the Map Mask disables keep what they had. If a card lets a
         * disabled plane change, every sprite in the game smears across the others.
         * That is the guarantee this pins.
         */
        g_Video.Planes[0][0x50]=0x11;
        g_Video.Planes[1][0x50]=0x22;
        g_Video.Planes[2][0x50]=0x33;
        g_Video.Planes[3][0x50]=0x44;
        VideoTestWriteGraphics(&bus, 0x05, 0x00);            /* read mode 0, write mode 0 */
        VideoTestWriteGraphics(&bus, 0x04, 0x00);            /* read map 0 -- the value it throws away */
        VideoTestWriteSequencer(&bus, 0x02, 0x04);            /* map mask = plane 2 only */
        (VOID)VddVideoPlanarRead(&g_Video, 0x50); /* the discarded read: latches, not data */
        CHECK(g_Video.Latch[1] == 0x22 && g_Video.Latch[3] == 0x44,
              "lemmings blit: the discarded read is what loads all four latches");
        VddVideoPlanarWrite(&g_Video, 0x50, 0x99);
        CHECK(g_Video.Planes[2][0x50] == 0x99, "lemmings blit: movsb writes the selected plane");
        CHECK(g_Video.Planes[0][0x50] == 0x11 && g_Video.Planes[1][0x50] == 0x22 &&
              g_Video.Planes[3][0x50] == 0x44,
              "lemmings blit: the other three planes are preserved exactly");

        /* -- (3) THE VRAM->VRAM COPY. This is how the toolbar gets on screen, and it
         * is the mechanism behind the open "panel has no icons" bug:
         *     Map Mask = 0x0F (all four planes), GR5 = WRITE MODE 1,
         *     then byte copies with source AND destination in A000.
         * Write mode 1 ignores the CPU byte entirely and writes THE FOUR LATCHES to
         * the four planes, so one `movsb` moves a four-plane pixel group. It is the
         * only way to move 16-colour artwork without four passes, and it is how the
         * panel is pulled from its off-screen cache (VRAM 0xF91F..0xFFFA -- which is
         * above the visible page and only addressable because a plane is 64KB).
         *
         * [CAUTION]: THE CPU BYTE MUST NOT MATTER. A card that quietly used it would copy
         * whatever `movsb` happened to load instead of the latched pixels, and the
         * panel would come out a flat colour -- icons missing, which is the symptom.
         * So the check feeds it a deliberately wrong byte.
         */
        g_Video.Planes[0][0x60]=0xDE;
        g_Video.Planes[1][0x60]=0xAD;
        g_Video.Planes[2][0x60]=0xBE;
        g_Video.Planes[3][0x60]=0xEF;
        g_Video.Planes[0][0x61]=0x00;
        g_Video.Planes[1][0x61]=0x00;
        g_Video.Planes[2][0x61]=0x00;
        g_Video.Planes[3][0x61]=0x00;
        VideoTestWriteSequencer(&bus, 0x02, 0x0F);            /* Map Mask = 0x0F, all four planes */
        VideoTestWriteGraphics(&bus, 0x05, 0x01);            /* GR5 = write mode 1 */
        (VOID)VddVideoPlanarRead(&g_Video, 0x60); /* the `movsb` source read: latches */
        VddVideoPlanarWrite(&g_Video, 0x61, 0x00);/* ...and its store. CPU byte is a LIE. */
        CHECK(g_Video.Planes[0][0x61]==0xDE && g_Video.Planes[1][0x61]==0xAD &&
              g_Video.Planes[2][0x61]==0xBE && g_Video.Planes[3][0x61]==0xEF,
              "lemmings panel: write mode 1 copies all four planes from the latches");
        CHECK(g_Video.Planes[0][0x60]==0xDE && g_Video.Planes[3][0x60]==0xEF,
              "lemmings panel: ...and the source group is left alone");

        /* The panel cache lives at 0xF91F..0xFFFA -- ABOVE the 320x200 visible page
         * (0x1F40) and running to the last byte of the plane. `VIDEO_PLANE_SIZE` was
         * once 38400, so every one of those bytes read back 0xFF and every write was
         * dropped: the panel was being cached into a hole. Pin both ends.
         */
        CHECK(VIDEO_PLANE_SIZE == 0x10000, "a VGA plane is 64KB, so off-screen VRAM exists");
        g_Video.Planes[2][0xF91F] = 0x5A;
        g_Video.Planes[2][0xFFFA] = 0xA5;
        VideoTestWriteGraphics(&bus, 0x05, 0x00);
        VideoTestWriteGraphics(&bus, 0x04, 0x02);   /* read mode 0, read plane 2 */
        CHECK(VddVideoPlanarRead(&g_Video, 0xF91F)==0x5A && VddVideoPlanarRead(&g_Video, 0xFFFA)==0xA5,
              "lemmings panel: the off-screen cache 0xF91F..0xFFFA reads back");

        /* -- (4) THE WHOLE-PANEL BLIT -- THE ROUTINE THAT ACTUALLY PUTS THE TOOLBAR ON
         * SCREEN, and the one the "missing icons" bug is about. Same set-up as (3),
         * then ONE `rep movsb` of 0x6E0 bytes (1760 = 40 rows x 44) from the panel
         * cache at 0xF91F, to BOTH pages (destination +0x1E42 on each). It runs on
         * every level start, unconditionally.
         *
         * [CAUTION]: THIS CORRECTS THE PINNED STORY. The per-button routine whose write site the
         * rig named is NOT the panel painter: it repaints ONE button when the
         * SELECTED skill changes. Running ~1.5 times in a run where the player
         * changed selection once is correct, not a defect -- so "the blitter runs
         * 1.5 times instead of 12" was a question about the wrong routine.
         *
         * [CAUTION]: WHY A REP AND NOT A LOOP OF ONE. In `rep movsb` every single byte must do
         * its OWN read-then-write: the read loads the latches, the write emits them.
         * A model that hoisted the read out of the rep, or let the latches go stale
         * across it, would smear ONE pixel group over all 1760 bytes -- a flat-colour
         * panel with no icons, which is exactly the reported symptom. So the source
         * here is deliberately NON-uniform and every byte of the result is checked.
         */
        {   UINT32 step;
        INT same = 1;
        UINT plane;
            const UINT32 sourceOffset = 0xF91F;
            const UINT32 destinationOffset = 0x1E42;
            const UINT32 length = 0x6E0;
            /* A pattern that differs per byte AND per plane, so a stale latch or a
             * wrong plane cannot coincidentally reproduce it.
             */
            for (step = 0; step < length; ++step)
                for (plane = 0; plane < 4; ++plane)
                {
                    g_Video.Planes[plane][sourceOffset + step] = (BYTE)(step * 7u + plane * 61u + 1u);
                    g_Video.Planes[plane][destinationOffset + step] = 0x00;      /* a black panel to start from */
                }
            VideoTestWriteSequencer(&bus, 0x02, 0x0F);            /* Map Mask = 0x0F */
            VideoTestWriteGraphics(&bus, 0x05, 0x01);            /* GR5 = write mode 1 */
            for (step = 0; step < length; ++step)         /* rep movsb, byte for byte */
            {
                (VOID)VddVideoPlanarRead(&g_Video, sourceOffset + step);
                VddVideoPlanarWrite(&g_Video, destinationOffset + step, 0x00);  /* CPU byte is ignored */
            }
            for (step = 0; step < length && same; ++step)
                for (plane = 0; plane < 4; ++plane)
                    if (g_Video.Planes[plane][destinationOffset + step] != g_Video.Planes[plane][sourceOffset + step])
                    {
                        same = 0;
                        break;
                    }
            CHECK(same, "lemmings panel: the 1760-byte rep movsb reproduces all four planes");
            /* A stale-latch model passes a one-byte check and fails this one: it would
             * leave every destination byte equal to the FIRST source group.
             */
            CHECK(g_Video.Planes[0][destinationOffset + 1] != g_Video.Planes[0][destinationOffset],
                  "lemmings panel: ...byte by byte, not one group smeared over the copy");
            /* THE COPY ENDS ONE BYTE FROM THE TOP OF THE PLANE: 0xF91F + 0x6E0 - 1 =
             * 0xFFFE. An off-by-one in the plane bound truncates the last row of the
             * toolbar rather than failing outright, so pin the final byte explicitly.
             */
            CHECK(sourceOffset + length - 1 == 0xFFFE, "lemmings panel: the blit ends at 0xFFFE, inside the plane");
            CHECK(g_Video.Planes[3][destinationOffset + length - 1] == g_Video.Planes[3][sourceOffset + length - 1],
                  "lemmings panel: ...and that last byte copies like any other");
        }

        /* (5) AL BIT 7 ON A MODE SET MEANS "DO NOT CLEAR VIDEO MEMORY" (Importance = 3):
         * THE ACTUAL TOOLBAR BUG. We took `al & 0x7F` for the mode number and threw
         * the bit away, so every mode set wiped all four 64KB planes. Lemmings
         * composes its skill-button panel into OFF-SCREEN VRAM and only then sets
         * its mode -- INT 10h AX=008Dh for gameplay, AX=0090h for the menu (the
         * INT 10h trace) -- with bit 7 set precisely so that cache survives.
         * We erased it, and the panel blit copied 1760 bytes of zeroes.
         *
         * [CAUTION]: The off-screen half is the half that matters and the half a screen-shaped
         * test would miss: the visible page gets redrawn immediately either way, so
         * a check that only looked at the picture would pass while the bug remained.
         */
        {   NTVDD_REGISTERS registers;
            UINT32 offscreenOffset = 0xF91F;
            UINT32 visibleOffset = 0x0100;
            INT plane;

            for (plane = 0; plane < 4; ++plane)
            {
                g_Video.Planes[plane][offscreenOffset] = (BYTE)(0xA0 + plane);
                g_Video.Planes[plane][visibleOffset] = (BYTE)(0x50 + plane);
            }
            memset(&registers, 0, sizeof registers);
            VddSetAh(&registers, 0x00);
            VddSetAl(&registers, 0x8D);   /* mode 0Dh, PRESERVE */
            VddBusDeliverInterrupt(&bus, 0x10, &registers);
            CHECK(g_Video.Mode == 0x0D, "mode set: AL=0x8D still selects mode 0Dh (bit 7 is not the mode)");
            {   INT kept = 1;
                for (plane = 0; plane < 4; ++plane)
                    if (g_Video.Planes[plane][offscreenOffset] != (BYTE)(0xA0 + plane))
                        kept = 0;
                CHECK(kept, "mode set: AL bit 7 PRESERVES the off-screen sprite cache");
            }
            {   INT kept = 1;
                for (plane = 0; plane < 4; ++plane)
                    if (g_Video.Planes[plane][visibleOffset] != (BYTE)(0x50 + plane))
                        kept = 0;
                CHECK(kept, "mode set: ...and the visible page too -- it is ALL of display memory");
            }
            /* AND THE DEFAULT MUST STILL CLEAR, or every guest that relies on a mode
             * set to blank the screen inherits the last program's picture.
             */
            memset(&registers, 0, sizeof registers);
            VddSetAh(&registers, 0x00);
            VddSetAl(&registers, 0x0D);   /* mode 0Dh, CLEAR */
            VddBusDeliverInterrupt(&bus, 0x10, &registers);
            {   INT cleared = 1;
                for (plane = 0; plane < 4; ++plane)
                    if (g_Video.Planes[plane][offscreenOffset] || g_Video.Planes[plane][visibleOffset])
                        cleared = 0;
                CHECK(cleared, "mode set: WITHOUT bit 7 the planes are cleared, as before");
            }
        }

        /* (6) THE INSTRUMENT THAT HAS TO ANSWER "DID THE BLIT RUN AT ALL":
         * The rig's answer so far is an ABSENCE: no read site at the panel blit's
         * pc. But that report is drawn from 256-slot single-slot hashes which lost
         * 249,630 reads on the same run, so an absence there can equally mean the
         * pc collided with a busier one -- the two readings are indistinguishable,
         * and acting on the wrong one costs a session. The linear cache table
         * exists to make the absence mean something, so it is worth exactly as
         * much as this test: replay the two routines and check it names both, in
         * the order they happened.
         */
        {   UINT32 step;
        UINT index;
        UINT readCount = 0;
        UINT writeCount = 0;
            UINT32 firstWrite = 0;
            UINT32 firstRead = 0;
            const UINT32 sourceOffset = 0xF91F;
            const UINT32 destinationOffset = 0x1E42;
            const UINT32 composePc = 0x01105815;
            const UINT32 blitPc = 0x01107626;

            memset(g_Video.CacheSites, 0, sizeof g_Video.CacheSites);
            g_Video.CacheSitesLost = 0;
            g_Video.CacheSequence = 0;
            g_Video.GuestPc = VideoTestFakePc;

            g_FakePc = composePc;                     /* the compositor fills it */
            VideoTestWriteGraphics(&bus, 0x05, 0x00);                     /* write mode 0, plain bytes */
            VideoTestWriteSequencer(&bus, 0x02, 0x0F);
            for (step = 0; step < 64; ++step)
                VddVideoPlanarWrite(&g_Video, sourceOffset + step, (BYTE)step);
            g_FakePc = blitPc;                        /* ...then the blit reads it */
            VideoTestWriteGraphics(&bus, 0x05, 0x01);
            for (step = 0; step < 64; ++step)
            {
                (VOID)VddVideoPlanarRead(&g_Video, sourceOffset + step);
                VddVideoPlanarWrite(&g_Video, destinationOffset + step, 0x00);
            }
            for (index = 0; index < VIDEO_CACHE_SITES; ++index)
            {
                if (!g_Video.CacheSites[index].Count)
                    continue;
                if (g_Video.CacheSites[index].IsWrite)
                {
                    writeCount++;
                    if (g_Video.CacheSites[index].Pc == composePc)
                        firstWrite = g_Video.CacheSites[index].First;
                }
                else
                {
                    readCount++;
                    if (g_Video.CacheSites[index].Pc == blitPc)
                        firstRead = g_Video.CacheSites[index].First;
                }
            }
            CHECK(writeCount == 1 && readCount == 1 && !g_Video.CacheSitesLost,
                  "cache sites: the compositor and the blit are BOTH named, none lost");
            CHECK(firstWrite && firstRead && firstWrite < firstRead,
                  "cache sites: ...and the order says the cache was filled BEFORE it was read");
            /* THE WRITE TO THE SCREEN MUST NOT BE COUNTED AS A CACHE TOUCH. The blit's
             * destination is an ordinary visible page; if the floor let it in, the table
             * would report the blitter as its own compositor and invert the ordering.
             */
            {   INT below = 0;
                for (index = 0; index < VIDEO_CACHE_SITES; ++index)
                    if (g_Video.CacheSites[index].Count && g_Video.CacheSites[index].Low < VIDEO_CACHE_LOW)
                        below = 1;
                CHECK(!below && destinationOffset < VIDEO_CACHE_LOW,
                      "cache sites: the visible page is below the floor, so it is ignored");
            }
            /* A FULL TABLE MUST SAY SO RATHER THAN SILENTLY DROP. */
            for (step = 0; step < VIDEO_CACHE_SITES + 4u; ++step)
            {
                g_FakePc = 0x02000000u + step * 0x100u;
                (VOID)VddVideoPlanarRead(&g_Video, sourceOffset);
            }
            CHECK(g_Video.CacheSitesLost > 0, "cache sites: overflow is REPORTED, never dropped in silence");

            memset(g_Video.CacheSites, 0, sizeof g_Video.CacheSites);
            g_Video.CacheSitesLost = 0;
            g_Video.CacheSequence = 0;
            g_Video.GuestPc = 0;                  /* leave the rest of the battery as it was */
        }
    }

    /* THE CURSOR'S SHAPE, IN THE UNITS DOS ACTUALLY ASKS IN (Importance = 1):
     * DOS sets its cursor in SCAN LINES of an 8-line character cell, because that
     * is the machine it was written for. Our cell is 16 lines, so honouring those
     * numbers literally puts the underline halfway up -- rendered as "ABC123-"
     * where a real DOS box shows "ABC123_". These are the exact shapes DOS uses,
     * so a regression here is visible on every prompt.
     */
    {   UINT startLine = 99, endLine = 99;
    INT isHidden = 9;

        VddCursorLines(0x0607, 16, &startLine, &endLine, &isHidden);
        CHECK(startLine == 14 && endLine == 15 && !isHidden,
              "cursor 6-7 (DOS overwrite underline) scales to 14-15: the BOTTOM two lines");

        VddCursorLines(0x0007, 16, &startLine, &endLine, &isHidden);
        CHECK(startLine == 1 && endLine == 15 && !isHidden,
              "cursor 0-7 (DOS INSERT mode) scales to 1-15: a full block");

        /* [CAUTION]: The two-line branch. Scaling both ends the ordinary way would give
         * 13-15 -- three lines -- and the underline would be visibly fat.
         */
        CHECK((0x0607 >> 8) + 1 == (0x0607 & 0x1f),
              "...and 6-7 IS the adjacent-line case that branch exists for");

        VddCursorLines(0x0507, 16, &startLine, &endLine, &isHidden);
        CHECK(startLine == 11 && endLine == 15 && !isHidden, "cursor 5-7 (half block) scales to 11-15");

        /* A shape that already knows about 16-line cells must NOT be scaled again. */
        VddCursorLines(0x0D0F, 16, &startLine, &endLine, &isHidden);
        CHECK(startLine == 13 && endLine == 15 && !isHidden,
              "cursor 13-15 is already in 16-line units and is left alone");

        /* Hiding, both idioms. */
        VddCursorLines(0x2607, 16, &startLine, &endLine, &isHidden);
        CHECK(isHidden, "CH bit 5 hides the cursor, and is not mistaken for a scan line");
        VddCursorLines(0x0F0E, 16, &startLine, &endLine, &isHidden);
        CHECK(isHidden, "start past end is the other 'no cursor' idiom");

        /* An 8-line cell must pass through untouched -- that is what the guard is for. */
        VddCursorLines(0x0607, 8, &startLine, &endLine, &isHidden);
        CHECK(startLine == 6 && endLine == 7 && !isHidden, "on a real 8-line cell nothing is scaled");

        /* Never off the end of the cell, whatever is asked for. */
        VddCursorLines(0x1F1F, 16, &startLine, &endLine, &isHidden);
        CHECK(startLine < 16 && endLine < 16, "an out-of-range shape is clamped inside the cell"); }

    /* T20: WHAT A MODE SET LEAVES IN THE AC AND THE DAC, PER MODE -------------
     * These values are measured on genuine MS-DOS 6.22 by tests/probes/dos/vgadefs.asm
     * (checked in as vgadefs.ref.txt) and generated into src/vdd/vga_defaults.h.
     * They are asserted here because the previous single table was inferred from a
     * screenshot and was wrong in two independent ways at once, and nothing in the
     * battery noticed -- a test card renders the whole chain, so it passes whenever
     * two wrong links cancel. Reading the registers back cannot do that.
     */
    {   static const BYTE cgaAttributes[16] = { 0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,
                                            0x10,0x11,0x12,0x13,0x14,0x15,0x16,0x17 };
        static const BYTE egaAttributes[16] = { 0x00,0x01,0x02,0x03,0x04,0x05,0x14,0x07,
                                            0x38,0x39,0x3A,0x3B,0x3C,0x3D,0x3E,0x3F };
        INT index;
        INT isOk;

        memset(&registers,0,sizeof registers);
        VddSetAh(&registers,0x00);
        VddSetAl(&registers,0x0D);
        VddBusDeliverInterrupt(&bus,0x10,&registers);
        for (index = 0, isOk = 1; index < 16; ++index)
            if (g_Video.PaletteRegisters[index] != cgaAttributes[index])
                isOk = 0;
        CHECK(isOk, "mode 0Dh leaves the CGA attribute table (6 at index 6, 10h..17h high)");
        /* Mode 0Dh's DAC repeats the same sixteen colours four times over 0..0x3F.
         * That repetition is why card 11 could not tell 0x10..0x17 from 0x38..0x3F.
         */
        for (index = 0, isOk = 1; index < 64; ++index)
            /* [CAUTION]: <<4, not <<3. The bright eight sit at 0x10..0x17, and 0x08..0x0F is
             * the DARK eight over again -- which is exactly why mode 0Dh's AC table
             * has to reach up to 0x10 to find them.
             */
            if (g_Video.Dac[index] != g_Video.Dac[(index & 7) | (((index >> 4) & 1) << 4)])
                isOk = 0;
        CHECK(isOk, "mode 0Dh's default DAC is the 16 CGA colours, repeated four times");

        memset(&registers,0,sizeof registers);
        VddSetAh(&registers,0x00);
        VddSetAl(&registers,0x10);
        VddBusDeliverInterrupt(&bus,0x10,&registers);
        for (index = 0, isOk = 1; index < 16; ++index)
            if (g_Video.PaletteRegisters[index] != egaAttributes[index])
                isOk = 0;
        CHECK(isOk, "mode 10h leaves the EGA attribute table (0x14 at index 6)");
        CHECK(g_Video.Dac[0x14] == 0xFFAA5500u, "mode 10h: DAC 0x14 is EGA brown");
        CHECK(g_Video.Dac[0x06] == 0xFFAAAA00u, "mode 10h: DAC 0x06 is dark yellow, not brown");

        /* [INFO]: THE WHOLE LEMMINGS FAULT IN ONE ASSERTION. The game writes its colour 6
         * to the DAC entry its own copy of this table names -- 0x14 -- and a pixel of
         * value 6 must read it back. With 6 in that slot the write went to 0x14 and
         * the read came from 0x06, so exactly one colour of sixteen was stale.
         */
        {   UINT32 value;
        value=0x14;
        VddBusIo(&bus,0x3C8,1,0,&value);
            value=0x3F;
            VddBusIo(&bus,0x3C9,1,0,&value);
            value=0x00;
            VddBusIo(&bus,0x3C9,1,0,&value);
            value=0x00;
            VddBusIo(&bus,0x3C9,1,0,&value);
            CHECK(g_Video.Palette[6] == g_Video.Dac[0x14],
                  "mode 10h: a DAC 0x14 write is what pixel value 6 renders with"); }

        memset(&registers,0,sizeof registers);
        VddSetAh(&registers,0x00);
        VddSetAl(&registers,0x13);
        VddBusDeliverInterrupt(&bus,0x10,&registers);
        for (index = 0, isOk = 1; index < 16; ++index)
            if (g_Video.PaletteRegisters[index] != index)
                isOk = 0;
        CHECK(isOk, "mode 13h leaves the identity attribute table");
        /* 13h's default is the real 256-colour palette, not a grey ramp: greys sit
         * at 0x10..0x1F and the colour wheel starts at 0x20.
         */
        CHECK(g_Video.Dac[0x10]==0xFF000000u && g_Video.Dac[0x1F]==0xFFFFFFFFu,
              "mode 13h: 0x10..0x1F is the grey ramp, black to white");
        CHECK(g_Video.Dac[0x20]==0xFF0000FFu, "mode 13h: the colour wheel starts at 0x20");

        /* [INFO]: A MODE SET REPROGRAMS THE CRTC. Without this a screen inherits the
         * geometry of the one before it: Lemmings' gameplay sets Offset=22 for its
         * 352-pixel scrolling window, and the mode 10h screen that follows was drawn
         * 44 bytes to the line instead of 80 -- diagonal noise.
         */
        memset(&registers,0,sizeof registers);
        VddSetAh(&registers,0x00);
        VddSetAl(&registers,0x0D);
        VddBusDeliverInterrupt(&bus,0x10,&registers);
        CHECK(g_Video.CrtcOffset==0x14, "mode 0Dh's default CRTC Offset is 20 (320px)");
        {   UINT32 value = 0x13;
        VddBusIo(&bus,0x3D4,1,0,&value);   /* Offset register */
            value = 22;
            VddBusIo(&bus,0x3D5,1,0,&value); } /* ...as Lemmings sets it */
        CHECK(g_Video.CrtcOffset==22 && g_Video.IsCrtcOffsetSeen,
              "a guest CAN set its own Offset, and it is recorded as seen");
        memset(&registers,0,sizeof registers);
        VddSetAh(&registers,0x00);
        VddSetAl(&registers,0x10);
        VddBusDeliverInterrupt(&bus,0x10,&registers);
        CHECK(g_Video.CrtcOffset==0x28, "...and the next mode set takes it back to 40 (640px)");
        CHECK(g_Video.CrtcStart==0 && !g_Video.IsCrtcOffsetSeen,
              "a mode set also clears the start address and the seen flag");

        /* Bit 7 of AL means "do not clear the buffer" and nothing else -- measured. */
        memset(&registers,0,sizeof registers);
        VddSetAh(&registers,0x00);
        VddSetAl(&registers,0x90);
        VddBusDeliverInterrupt(&bus,0x10,&registers);
        for (index = 0, isOk = 1; index < 16; ++index)
            if (g_Video.PaletteRegisters[index] != egaAttributes[index])
                isOk = 0;
        CHECK(isOk, "AL=90h is mode 10h: bit 7 does not change the palette load"); }

    /* T21: READ MODE 1 -- COLOUR COMPARE. ------------------------------------
     * A read in mode 1 returns one BIT PER PIXEL, set where that pixel's 4-bit
     * colour matches GR2 in every plane GR7 selects. It is how a game asks the
     * hardware "which of these eight pixels are solid", i.e. pixel-perfect terrain
     * collision in one instruction. GR5 bit 3 used to be masked off and GR2/GR7
     * dropped entirely, so every such read came back as a raw plane byte.
     */
    {   UINT32 value;
        /* [CAUTION]: A DELTA, NOT A TOTAL. This used to assert `rmode_hist[1]==4` against the
         * whole run's count, so adding a read ANYWHERE earlier in the battery broke a
         * test that has nothing to do with the addition -- which is exactly what the
         * Lemmings blit cases then did. Snapshot here and assert the change; the claim
         * is just as tight and it no longer depends on what else the file does.
         */
        UINT32 readMode0Before = g_Video.ReadModeHistogram[0];
        UINT32 readMode1Before = g_Video.ReadModeHistogram[1];
        memset(&registers,0,sizeof registers);
        VddSetAh(&registers,0x00);
        VddSetAl(&registers,0x12);
        VddBusDeliverInterrupt(&bus,0x10,&registers);
        /* Hand-place eight pixels in one byte: colours 0,1,2,3,4,5,6,7 left to right.
         * Plane p bit (7-k) is bit p of pixel k's colour.
         */
        {   INT index, plane;
            for (plane = 0; plane < 4; ++plane)
            {
                BYTE byteValue = 0;
                for (index = 0; index < 8; ++index)
                    if ((index >> plane) & 1)
                        byteValue = (BYTE)(byteValue | (0x80 >> index));
                g_Video.Planes[plane][0] = byteValue;
            } }
        value = 5;
        VddBusIo(&bus,0x3CE,1,0,&value);            /* GR5 Mode */
        value = 0x08;
        VddBusIo(&bus,0x3CF,1,0,&value);         /* read mode 1 */
        CHECK(g_Video.ReadMode==1 && g_Video.WriteMode==0,
              "GR5 bit 3 selects read mode 1 and leaves the write mode alone");
        value = 7;
        VddBusIo(&bus,0x3CE,1,0,&value);            /* GR7 Don't Care */
        value = 0x0F;
        VddBusIo(&bus,0x3CF,1,0,&value);         /* compare all planes */
        value = 2;
        VddBusIo(&bus,0x3CE,1,0,&value);            /* GR2 Color Compare */
        value = 5;
        VddBusIo(&bus,0x3CF,1,0,&value);            /* looking for colour 5 */
        CHECK(VddVideoPlanarRead(&g_Video,0)==(0x80>>5),
              "read mode 1: exactly the pixel whose colour is 5 comes back set");
        value = 2;
        VddBusIo(&bus,0x3CE,1,0,&value);
        value = 0;
        VddBusIo(&bus,0x3CF,1,0,&value);
        CHECK(VddVideoPlanarRead(&g_Video,0)==(0x80>>0),
              "...and colour 0 finds only pixel 0");
        /* GR7 = 0 means NO plane takes part, so every pixel matches. That is the
         * hardware's answer and not a bug to be tidied away.
         */
        value = 7;
        VddBusIo(&bus,0x3CE,1,0,&value);
        value = 0;
        VddBusIo(&bus,0x3CF,1,0,&value);
        CHECK(VddVideoPlanarRead(&g_Video,0)==0xFF,
              "Color Don't Care = 0 compares nothing, so every pixel matches");
        /* Only plane 0 in the comparison: colours 1,3,5,7 have bit 0 set. */
        value = 7;
        VddBusIo(&bus,0x3CE,1,0,&value);
        value = 1;
        VddBusIo(&bus,0x3CF,1,0,&value);
        value = 2;
        VddBusIo(&bus,0x3CE,1,0,&value);
        value = 1;
        VddBusIo(&bus,0x3CF,1,0,&value);
        CHECK(VddVideoPlanarRead(&g_Video,0)==0x55,
              "comparing plane 0 alone finds every odd-numbered colour");
        /* A read in mode 1 must STILL load the latches -- a masked write right after
         * one depends on them, and that is the pairing a collision-and-draw loop uses.
         */
        CHECK(g_Video.Latch[0]==0x55, "read mode 1 still loads the latches");
        /* Back to mode 0 and the plane select works as before. */
        value = 5;
        VddBusIo(&bus,0x3CE,1,0,&value);
        value = 0;
        VddBusIo(&bus,0x3CF,1,0,&value);
        value = 4;
        VddBusIo(&bus,0x3CE,1,0,&value);
        value = 2;
        VddBusIo(&bus,0x3CF,1,0,&value);
        CHECK(VddVideoPlanarRead(&g_Video,0)==g_Video.Planes[2][0],
              "read mode 0 still returns the plane GR4 selects");
        CHECK(g_Video.ReadModeHistogram[1]-readMode1Before==4 && g_Video.ReadModeHistogram[0]-readMode0Before>=1,
              "the read-mode histogram counts what was actually served"); }

    /* T22: THE START ADDRESS IS LATCHED, so a page flip is never seen half-written.
     * The pair is two byte registers; between them the value is half old and half new,
     * and a frame built there is a whole-screen glitch. Real hardware loads the
     * address counter at the vertical retrace, so it cannot happen.
     */
    {   UINT32 value;
        memset(&registers,0,sizeof registers);
        VddSetAh(&registers,0x00);
        VddSetAl(&registers,0x0D);
        VddBusDeliverInterrupt(&bus,0x10,&registers);
        value = 0x0C;
        VddBusIo(&bus,0x3D4,1,0,&value);
        value = 0x20;
        VddBusIo(&bus,0x3D5,1,0,&value);         /* high byte of 0x2040 */
        CHECK(g_Video.CrtcStartLive==0 && g_Video.IsCrtcStartPending,
              "the high byte alone does not move the display -- nothing is latched yet");
        value = 0x0D;
        VddBusIo(&bus,0x3D4,1,0,&value);
        value = 0x40;
        VddBusIo(&bus,0x3D5,1,0,&value);         /* low byte completes it */
        CHECK(g_Video.CrtcStartLive==0 && !g_Video.IsCrtcStartPending
              && g_Video.CrtcStartWrites>=1,
              "...and neither does the low byte: the LATCH is what moves it");
        g_Video.IsDirty=1;
        VddBusFrame(&bus);
        CHECK(g_Video.CrtcStartLive==0x2040,
              "the next frame latches the completed pair, as the retrace does");
        {   UINT32 before = g_Video.CrtcStartHalf;
            value = 0x0C;
            VddBusIo(&bus,0x3D4,1,0,&value);
            value = 0x30;
            VddBusIo(&bus,0x3D5,1,0,&value);     /* half a new address */
            g_Video.IsDirty=1;
            VddBusFrame(&bus);
            CHECK(g_Video.CrtcStartHalf==before+1,
                  "a frame latched mid-pair is COUNTED -- hardware tears there too"); } }

    /* T22b: THE HARDWARE'S SCHEDULE, ON A CLOCK (s83, Mario). The start address loads at
     * the start of vertical retrace; pel panning (AR13) is taken as the next picture
     * begins. Mario writes the start during display, waits for retrace, writes the pan:
     * both must appear TOGETHER on the next frame -- not the start early, not the pan
     * never. 0Dh: 70 Hz, F = 14285 us, 449 lines, retrace from line 400 (~12726 us).
     */
    {   UINT32 value;
    const UINT64 framePeriod = 14285u;
    const UINT64 duration = 1000u * 14285u;
        value = 0x0C;
        VddBusIo(&bus,0x3D4,1,0,&value);
        value = 0x00;
        VddBusIo(&bus,0x3D5,1,0,&value);
        value = 0x0D;
        VddBusIo(&bus,0x3D4,1,0,&value);
        value = 0x00;
        VddBusIo(&bus,0x3D5,1,0,&value);
        g_Video.TimeUs = VideoTestFakeClock;
        g_Video.GraphicsHeight = 200;
        g_Video.LatchTime = 0;
        g_FakeMicroseconds = duration + 1000;
        g_Video.IsDirty=1;
        VddBusFrame(&bus);       /* sync: start 0 */
        g_FakeMicroseconds = duration + 2000;                                          /* in the picture */
        value = 0x0C;
        VddBusIo(&bus,0x3D4,1,0,&value);
        value = 0x01;
        VddBusIo(&bus,0x3D5,1,0,&value);
        value = 0x0D;
        VddBusIo(&bus,0x3D4,1,0,&value);
        value = 0x08;
        VddBusIo(&bus,0x3D5,1,0,&value);
        g_Video.IsDirty=1;
        VddBusFrame(&bus);
        CHECK(g_Video.CrtcStartLive==0, "clocked: a start written in the picture is not shown yet");
        g_FakeMicroseconds = duration + 13000;                                         /* in retrace */
        VddBusIo(&bus,0x3DA,1,1,&value);                                 /* reset the AC flip-flop */
        value = 0x33;
        VddBusIo(&bus,0x3C0,1,0,&value);
        value = 0x03;
        VddBusIo(&bus,0x3C0,1,0,&value);
        g_Video.IsDirty=1;
        VddBusFrame(&bus);
        CHECK(g_Video.CrtcStartLive==0 && g_Video.DisplayPan==0,
              "clocked: during the retrace the old picture is still up (start and pan)");
        g_FakeMicroseconds = duration + framePeriod + 500;                                       /* next picture */
        g_Video.IsDirty=1;
        VddBusFrame(&bus);
        CHECK(g_Video.CrtcStartLive==0x0108 && g_Video.DisplayPan==3,
              "clocked: the next frame shows the new start AND the new pan together");
        /* A start written INSIDE the retrace missed that load: a frame later. */
        g_FakeMicroseconds = duration + framePeriod + 13000;
        value = 0x0C;
        VddBusIo(&bus,0x3D4,1,0,&value);
        value = 0x02;
        VddBusIo(&bus,0x3D5,1,0,&value);
        value = 0x0D;
        VddBusIo(&bus,0x3D4,1,0,&value);
        value = 0x00;
        VddBusIo(&bus,0x3D5,1,0,&value);
        g_FakeMicroseconds = duration + 2*framePeriod + 500;
        g_Video.IsDirty=1;
        VddBusFrame(&bus);
        CHECK(g_Video.CrtcStartLive==0x0108, "clocked: a start written inside the retrace waits a frame");
        g_FakeMicroseconds = duration + 3*framePeriod + 500;
        g_Video.IsDirty=1;
        VddBusFrame(&bus);
        CHECK(g_Video.CrtcStartLive==0x0200, "clocked: ...and is shown on the one after");
        /* Pel panning moves planar pixels: pan 1 shows the byte's SECOND pixel first. */
        memset(g_Video.Planes[0], 0, 0x400);
        memset(g_Video.Planes[1], 0, 0x400);
        memset(g_Video.Planes[2], 0, 0x400);
        memset(g_Video.Planes[3], 0, 0x400);
        g_Video.Planes[0][0x200] = 0x40;                                    /* pixel 1 = colour 1 */
        g_Video.DisplayPan = 1;
        g_Video.TimeUs = 0;
        g_Video.AttributeRegisters[0x13] = 1;
        g_Video.IsDirty=1;
        VddBusFrame(&bus);
        CHECK(g_Video.FrameBuffer[0]==1 && g_Video.FrameBuffer[1]==0, "planar: AR13=1 shifts the picture left one pixel");
        g_Video.AttributeRegisters[0x13] = 0;
        g_Video.IsDirty=1;
        VddBusFrame(&bus);
        CHECK(g_Video.FrameBuffer[0]==0 && g_Video.FrameBuffer[1]==1, "planar: AR13=0 shows it where it is");
        value = 0x0C;
        VddBusIo(&bus,0x3D4,1,0,&value);
        value = 0x00;
        VddBusIo(&bus,0x3D5,1,0,&value);
        value = 0x0D;
        VddBusIo(&bus,0x3D4,1,0,&value);
        value = 0x00;
        VddBusIo(&bus,0x3D5,1,0,&value);
        g_Video.IsDirty=1;
        VddBusFrame(&bus); }

    /* T-OWED: A SCANLINE COUNTER MUST NOT SKIP LINES BECAUSE OUR PORT IS SLOW -------
     * Lemmings' HP calibration counts 320 hblanks on bit 0 (`wait while set; wait
     * while clear` per line) against the 8254, and the tick that count programs is
     * what places its palette split at row 160. A poll slower than the 6.4us hblank
     * must still be told about every line it crossed -- once -- or the count runs
     * long (measured on the rig: 320..383 lines) and the split lands too low.
     */
    { UINT32 value;
    UINT64 time;
    INT iteration;
      g_Video.TimeUs = VideoTestFakeClock;
      g_Video.GraphicsHeight = 400;               /* 70 Hz, 31.8us lines */
      VideoTestWriteCrtc(&bus, 0x06, 0xBF);
      VideoTestWriteCrtc(&bus, 0x07, 0x1F);
      VideoTestWriteCrtc(&bus, 0x09, 0x41);
      VideoTestWriteCrtc(&bus, 0x12, 0x8F);
      VideoTestWriteCrtc(&bus, 0x15, 0x96);
      time = 0;
      g_FakeMicroseconds = 0;
      VddBusIo(&bus, 0x3DA, 1, 1, &value); /* prime: line 0, active */
      for (iteration = 0; iteration < 320; ++iteration)                          /* the guest's loop, 12us/in */
      {
          do
          {
              time += 12;
              g_FakeMicroseconds = time;
              VddBusIo(&bus, 0x3DA, 1, 1, &value);
          } while (value & 1);
          do
          {
              time += 12;
              g_FakeMicroseconds = time;
              VddBusIo(&bus, 0x3DA, 1, 1, &value);
          } while (!(value & 1));
      }
      /* 320 lines at 14285us/449 = 10181us; a missed line is +32us. */
      CHECK(time >= 10150 && time <= 10230, "3DA: a 12us poll loop counts EVERY scanline (320 in ~10.18ms)");
      time = 0;
      g_FakeMicroseconds = 0;
      VddBusIo(&bus, 0x3DA, 1, 1, &value);
      for (iteration = 0; iteration < 320; ++iteration)                          /* a 4us poller sees each blank */
      {
          do
          {
              time += 4;
              g_FakeMicroseconds = time;
              VddBusIo(&bus, 0x3DA, 1, 1, &value);
          } while (value & 1);
          do
          {
              time += 4;
              g_FakeMicroseconds = time;
              VddBusIo(&bus, 0x3DA, 1, 1, &value);
          } while (!(value & 1));
      }
      CHECK(time >= 10150 && time <= 10230, "3DA: ...and a 4us poll loop counts the same 320");
      /* A HOST STALL MID-COUNT: the guest is not polled for 330us (~10 lines) at
       * iteration 100. Every line crossed is a blank owed and the count must still
       * end at 320 lines' worth of real time -- one-blank repayment left +6 lines
       * on the rig (0x310B).
       */
      time = 0;
      g_FakeMicroseconds = 0;
      VddBusIo(&bus, 0x3DA, 1, 1, &value);
      for (iteration = 0; iteration < 320; ++iteration)
      {
          if (iteration == 100)
              time += 330;                                               /* the stall */
          do
          {
              time += 12;
              g_FakeMicroseconds = time;
              VddBusIo(&bus, 0x3DA, 1, 1, &value);
          } while (value & 1);
          do
          {
              time += 12;
              g_FakeMicroseconds = time;
              VddBusIo(&bus, 0x3DA, 1, 1, &value);
          } while (!(value & 1));
      }
      /* Honest expectation (two exact repayment schemes measured wrong, see VideoStatusIn):
       * the stall is repaid ONE line and never over-repaid -- the count ends between
       * 320 lines' time and 320 lines + the stall.
       */
      CHECK(time >= 10150 && time <= 10181 + 330, "3DA: a 330us stall mid-count is repaid one line, never over-repaid");
      { UINT32 firstRead, secondRead;
      g_FakeMicroseconds = 100000;
      VddBusIo(&bus, 0x3DA, 1, 1, &firstRead);
        g_FakeMicroseconds = 100001;
        VddBusIo(&bus, 0x3DA, 1, 1, &secondRead);
        CHECK(firstRead == secondRead, "3DA: two reads in the same line still agree (the rule needs a line boundary)"); }
      /* A gap of a frame or more owes nothing: the next poll reads the true phase. */
      /* A once-a-frame reader (the attribute flip-flop reset) owes nothing: 100 lines
       * apart, both polls active -- the owed count must not move.
       */
      { UINT32 firstRead, before;
      g_FakeMicroseconds = 200000 + 5;
      VddBusIo(&bus, 0x3DA, 1, 1, &firstRead);
        before = g_Video.Port3DaHblOwed;
        g_FakeMicroseconds = 200000 + 5 + 100 * 14285 / 449;
        VddBusIo(&bus, 0x3DA, 1, 1, &firstRead);
        CHECK(g_Video.Port3DaHblOwed == before, "3DA: a poll 100 lines after the last owes nothing (not a line counter)"); }
      g_Video.TimeUs = 0;
    }

    /* T-SPLIT: A PALETTE WRITE MID-FRAME IS A RASTER SPLIT (s70, Lemmings #1/#2) ---
     * Lemmings rewrites DAC 16..23 twice per frame: once from its timer tick, which
     * is calibrated to land at row 160 (the toolbar's top), and once after the
     * retrace. The rows above 160 must show the frame-start value, the rows from
     * 160 down the mid-frame one -- and that must hold WHATEVER phase the snapshot
     * is taken at, and stop holding once the guest stops doing it. No oracle can
     * pin this (QEMU's default 0x3DA makes the two writes land back to back), so
     * the fake clock and the game's own idiom do.
     */
    { UINT32 value, baseColour, splitColour;
    PNTVDD_FRAME frame = &g_Video.Frame;
      const UINT64 framePeriod = 1000000u / 70u;                 /* 14285 us: 449 lines */
#define AT(frame, line)     ((UINT64)(frame) * framePeriod + (UINT64)(line) * framePeriod / 449u)
#define DAC(index, red, green, blue) do { value=(index); VddBusIo(&bus,0x3C8,1,0,&value); value=(red); VddBusIo(&bus,0x3C9,1,0,&value); \
                                value=(green); VddBusIo(&bus,0x3C9,1,0,&value); value=(blue); VddBusIo(&bus,0x3C9,1,0,&value); } while (0)
      g_Video.TimeUs = VideoTestFakeClock;
      g_Video.GraphicsHeight = 200;                                       /* 320x200 shown as 400 lines */
      VideoTestWriteCrtc(&bus, 0x06, 0xBF);
      VideoTestWriteCrtc(&bus, 0x07, 0x1F);
      VideoTestWriteCrtc(&bus, 0x09, 0x41);
      VideoTestWriteCrtc(&bus, 0x12, 0x8F);
      VideoTestWriteCrtc(&bus, 0x15, 0x96);
      g_FakeMicroseconds = AT(10, 3);
      DAC(16, 0x3F, 0x00, 0x00);
      baseColour = g_Video.Palette[16];   /* row 1: base */
      /* (321, not 320: AT() and the model both truncate, and 320 lands on 319.99.) */
      g_FakeMicroseconds = AT(10, 321);
      DAC(16, 0x00, 0x3F, 0x00);
      splitColour = g_Video.Palette[16];   /* row 160: split */
      CHECK(baseColour != splitColour && g_Video.PaletteSplitRow[16] == 160 && g_Video.PaletteBase[16] == baseColour && g_Video.PaletteSplit[16] == splitColour,
            "split: a DAC write at scanline 320 is a split at row 160 over the frame-start value");
      g_FakeMicroseconds = AT(10, 400);
      VddVideoFrameTouch(&g_Video);
      CHECK(VddFrameHasSplit(frame), "split: the frame reports a live split");
      CHECK(VddFramePaletteAt(frame, 100, 16) == baseColour && VddFramePaletteAt(frame, 159, 16) == baseColour,
            "split: rows above 160 resolve to the frame-start colour");
      CHECK(VddFramePaletteAt(frame, 160, 16) == splitColour && VddFramePaletteAt(frame, 199, 16) == splitColour,
            "split: rows from 160 down resolve to the mid-frame colour");
      CHECK(VddFramePaletteAt(frame, 100, 17) == g_Video.Palette[17], "split: an entry never split is the live palette");
      /* Next frame, the post-retrace push on row 0, snapshot taken BEFORE this
       * frame's tick: the split from the previous frame must still hold.
       */
      g_FakeMicroseconds = AT(11, 1);
      DAC(16, 0x3F, 0x00, 0x00);
      g_FakeMicroseconds = AT(11, 200);
      VddVideoFrameTouch(&g_Video);
      CHECK(VddFramePaletteAt(frame, 100, 16) == baseColour && VddFramePaletteAt(frame, 180, 16) == splitColour,
            "split: phase-independent -- a snapshot before this frame's tick still shows both");
      /* A jittering tick: the next frames' writes land on rows 165 and 158. The
       * boundary must STAY at 160 (IRQ jitter is ours, not the guest's).
       */
      g_FakeMicroseconds = AT(12, 1);
      DAC(16, 0x3F, 0x00, 0x00);
      g_FakeMicroseconds = AT(12, 331);
      DAC(16, 0x00, 0x3F, 0x00);      /* row 165 */
      CHECK(g_Video.PaletteSplitRow[16] == 160, "split: a write 5 rows off keeps the boundary at 160 (sticky)");
      g_FakeMicroseconds = AT(13, 1);
      DAC(16, 0x3F, 0x00, 0x00);
      g_FakeMicroseconds = AT(13, 317);
      DAC(16, 0x00, 0x3F, 0x00);      /* row 158 */
      CHECK(g_Video.PaletteSplitRow[16] == 160, "split: ...and 2 rows the other way too");
      g_FakeMicroseconds = AT(13, 400);
      VddVideoFrameTouch(&g_Video);
      CHECK(VddFramePaletteAt(frame, 159, 16) == baseColour && VddFramePaletteAt(frame, 160, 16) == splitColour,
            "split: rows 159/160 still resolve either side of the sticky boundary");
      /* The guest stops splitting: two frames later it is a single palette again. */
      g_FakeMicroseconds = AT(17, 100);
      VddVideoFrameTouch(&g_Video);
      CHECK(!VddFrameHasSplit(frame) && VddFramePaletteAt(frame, 180, 16) == g_Video.Palette[16],
            "split: expires two frames after the guest stops -- the DAC simply holds");
      /* A genuinely different row (a new effect at row 60) does move it. */
      g_FakeMicroseconds = AT(18, 1);
      DAC(16, 0x3F, 0x00, 0x00);
      g_FakeMicroseconds = AT(18, 121);
      DAC(16, 0x00, 0x3F, 0x00);      /* row 60 */
      CHECK(g_Video.PaletteSplitRow[16] == 60, "split: a write far from the old boundary moves it");
      /* A write during blanking is the frame's base, not a split. */
      g_FakeMicroseconds = AT(20, 420);
      DAC(16, 0x00, 0x00, 0x3F);
      CHECK(g_Video.PaletteBase[16] == g_Video.Palette[16] && !VddFrameHasSplit(frame),
            "split: a write in vertical blanking is the next frame's base");
      g_Video.TimeUs = 0;
#undef AT
#undef DAC
    }

    /* T21: TEXT-MODE FIDELITY FOR FULL-SCREEN APPLICATIONS (edit.com, QBasic) (Importance = 1):
     * Five things a text-mode UI relies on and none of which existed: the BDA display
     * fields, bright-vs-blink backgrounds, the 43/50-line font calls, the CRTC cursor
     * registers, and the mouse driver's inverted-cell text cursor. Each is checked as a
     * RENDER or a byte in guest memory, not as a return code.
     */
    {   static BYTE textBiosData[0x100];
        INT glyphRow;
        INT glyphColumn;
        memset(textBiosData, 0, sizeof textBiosData);
        g_Video.BiosData = textBiosData;
        memset(&registers,0,sizeof registers);
        VddSetAh(&registers,0x00);
        VddSetAl(&registers,0x03);
        VddBusDeliverInterrupt(&bus,0x10,&registers);

        /* a. the BDA describes the display, and a text app reads it rather than asking */
        CHECK(textBiosData[0x49]==3 && textBiosData[0x4A]==80 && textBiosData[0x4B]==0 && textBiosData[0x84]==24 &&
              textBiosData[0x85]==16 && textBiosData[0x63]==0xD4 && textBiosData[0x64]==0x03,
              "bda: mode 3 -> 0449=03 044A=80 0484=24 (rows-1) 0485=16 0463=3D4h");
        CHECK((textBiosData[0x89] & 0x01) && (textBiosData[0x87] & 0x60) == 0x60,
              "bda: 0489 says VGA active at 400 lines, 0487 says 256K EGA/VGA");
        memset(&registers,0,sizeof registers);
        VddSetAh(&registers,0x02);
        VddSetDx(&registers,(WORD)((7<<8)|12));
        VddBusDeliverInterrupt(&bus,0x10,&registers);
        CHECK(textBiosData[0x50]==12 && textBiosData[0x51]==7, "bda: INT 10h AH=02 lands in 0450 as (col,row)");
        memset(&registers,0,sizeof registers);
        VddSetAh(&registers,0x01);
        VddSetCx(&registers,0x0007);
        VddBusDeliverInterrupt(&bus,0x10,&registers);
        CHECK(textBiosData[0x60]==0x07 && textBiosData[0x61]==0x00, "bda: INT 10h AH=01 lands in 0460 as (end,start)");

        /* b. attribute bit 7: BLINK by default, BRIGHT BACKGROUND after 1003h BL=0 */
        memset(&registers,0,sizeof registers);
        VddSetAh(&registers,0x02);
        VddSetDx(&registers,0);
        VddBusDeliverInterrupt(&bus,0x10,&registers);
        memset(&registers,0,sizeof registers);
        VddSetAh(&registers,0x09);
        VddSetAl(&registers,' ');
        VddSetBx(&registers,0xF0);
        VddSetCx(&registers,1);
        VddBusDeliverInterrupt(&bus,0x10,&registers);
        memset(&registers,0,sizeof registers);
        VddSetAh(&registers,0x02);
        VddSetDx(&registers,(WORD)((24<<8)|79));
        VddBusDeliverInterrupt(&bus,0x10,&registers);
        CHECK(g_Video.IsBlink == 1, "blink: a mode set enables blink (the power-on default)");
        VddVideoRender(&g_Video);
        CHECK(g_Video.FrameBuffer[0] == 7, "blink on: attribute F0h draws background 7 (bit 7 is blink, not bright)");
        memset(&registers,0,sizeof registers);
        VddSetAx(&registers,0x1003);
        VddSetBx(&registers,0x0000);
        VddBusDeliverInterrupt(&bus,0x10,&registers);
        CHECK(g_Video.IsBlink == 0, "int10/1003 BL=0: blink off");
        VddVideoRender(&g_Video);
        CHECK(g_Video.FrameBuffer[0] == 15, "blink off: attribute F0h draws background 15 (sixteen backgrounds)");
        memset(&registers,0,sizeof registers);
        VddSetAx(&registers,0x1003);
        VddSetBx(&registers,0x0001);
        VddBusDeliverInterrupt(&bus,0x10,&registers);
        VideoTestTextCell(0,0)[0]='A';
        VideoTestTextCell(0,0)[1]=0x87;                /* blinking grey on black */
        g_Video.TimeUs = VideoTestFakeClock;
        g_FakeMicroseconds = 100000;
        VddVideoRender(&g_Video);
        { PCBYTE glyph = g_VgaFont8x16['A'];
        INT litCount = 0;
          for (glyphRow=0;glyphRow<16;++glyphRow) for (glyphColumn=0;glyphColumn<8;++glyphColumn)
              if ((glyph[glyphRow]&(0x80>>glyphColumn)) && g_Video.FrameBuffer[glyphRow*TXW+glyphColumn]==7)
                  litCount++;
          CHECK(litCount > 0, "blink: in the on phase the glyph is drawn"); }
        g_FakeMicroseconds = 700000;
        VddVideoRender(&g_Video);
        { INT anyCount = 0;
        for (glyphRow=0;glyphRow<16;++glyphRow)
            for (glyphColumn=0;glyphColumn<8;++glyphColumn)
                if (g_Video.FrameBuffer[glyphRow*TXW+glyphColumn]!=0)
                    anyCount++;
          CHECK(anyCount == 0, "blink: in the off phase the glyph hides (fg == bg)"); }
        g_Video.TimeUs = 0;
        VideoTestTextCell(0,0)[1]=0x07;

        /* c. 1112h IS the 50-line call: 8x8 ROM font, 400/8 rows, an 8x8 render */
        memset(&registers,0,sizeof registers);
        VddSetAx(&registers,0x1112);
        VddSetBx(&registers,0);
        VddBusDeliverInterrupt(&bus,0x10,&registers);
        CHECK(g_Video.Rows == 50 && g_Video.CellHeight == 8, "int10/1112: the 8x8 ROM font gives 50 rows");
        CHECK((VddGetDx(&registers) & 0xFF) == 49, "int10/1112: DL = rows-1 = 49");
        CHECK(textBiosData[0x84]==49 && textBiosData[0x85]==8, "bda: 0484=49 0485=8 after 1112h");
        g_Video.IsDirty=1;
        VddBusFrame(&bus);
        CHECK(g_Video.Frame.Width==720 && g_Video.Frame.Height==400, "frame(50-line): still 720x400");
        VideoTestTextCell(49,0)[0]='A';
        VideoTestTextCell(49,0)[1]=0x0F;
        VddVideoRender(&g_Video);
        { PCBYTE glyph = g_VgaFont8x8['A'];
        INT mismatches = 0;
          for (glyphRow=0;glyphRow<8;++glyphRow) for (glyphColumn=0;glyphColumn<8;++glyphColumn)
          {
              BYTE expected=(glyph[glyphRow]&(0x80>>glyphColumn))?15:0;
              if (g_Video.FrameBuffer[(392+glyphRow)*TXW+glyphColumn]!=expected)
                  mismatches++; }
          CHECK(mismatches==0, "render(50-line): row 49 is an 8x8 ROM glyph at scan line 392"); }
        memset(&registers,0,sizeof registers);
        VddSetAh(&registers,0x11);
        VddSetAl(&registers,0x30);
        VddSetBx(&registers,0x0100);
        VddBusDeliverInterrupt(&bus,0x10,&registers);
        CHECK(VddGetCx(&registers) == 8 && (VddGetDx(&registers)&0xFF) == 49, "int10/1130 BH=1: the CURRENT font is 8x8, 50 rows");
        memset(&registers,0,sizeof registers);
        VddSetAx(&registers,0x1114);
        VddSetBx(&registers,0);
        VddBusDeliverInterrupt(&bus,0x10,&registers);
        CHECK(g_Video.Rows == 25 && g_Video.CellHeight == 16, "int10/1114: the 8x16 ROM font gives 25 rows again");
        memset(&registers,0,sizeof registers);
        VddSetAx(&registers,0x1102);
        VddSetBx(&registers,0);
        VddBusDeliverInterrupt(&bus,0x10,&registers);
        CHECK(g_Video.Rows == 25 && g_Video.CellHeight == 16, "int10/1102: the 0x variant changes glyphs only, not rows");
        memset(&registers,0,sizeof registers);
        VddSetAx(&registers,0x1111);
        VddSetBx(&registers,0);
        VddBusDeliverInterrupt(&bus,0x10,&registers);
        CHECK(g_Video.Rows == 28 && g_Video.CellHeight == 14, "int10/1111: the 8x14 ROM font gives 28 rows");

        /* d. the cursor programmed straight into the CRTC (every CRT unit does this) */
        memset(&registers,0,sizeof registers);
        VddSetAh(&registers,0x00);
        VddSetAl(&registers,0x03);
        VddBusDeliverInterrupt(&bus,0x10,&registers);
        VideoTestWriteCrtc(&bus, 0x0E, (BYTE)((3*80+5) >> 8));
        VideoTestWriteCrtc(&bus, 0x0F, (BYTE)((3*80+5) & 0xFF));
        CHECK(g_Video.CursorRow == 3 && g_Video.CursorColumn == 5, "crtc 0E/0F: cursor address 3*80+5 -> row 3 col 5");
        CHECK(textBiosData[0x50]==5 && textBiosData[0x51]==3, "crtc 0E/0F: ...and the BDA follows");
        { UINT32 value = 0, registerIndex = 0x0E;
        VddBusIo(&bus,0x3D4,1,0,&registerIndex);
        VddBusIo(&bus,0x3D5,1,1,&value);
          CHECK(value == ((3*80+5)>>8), "crtc 0E: reads back the cursor address high byte"); }
        VideoTestWriteCrtc(&bus, 0x0A, 0x20);
        { UINT startLine,endLine;
        INT isHidden;
        VddCursorLines(g_Video.CursorShape, 16, &startLine,&endLine,&isHidden);
          CHECK(isHidden, "crtc 0A: bit 5 hides the cursor"); }
        VideoTestWriteCrtc(&bus, 0x0A, 0x06);
        VideoTestWriteCrtc(&bus, 0x0B, 0x07);
        CHECK(g_Video.CursorShape == 0x0607, "crtc 0A/0B: start/end become the INT 10h shape word");
        VideoTestWriteCrtc(&bus, 0x0E, 0x7F);
        VideoTestWriteCrtc(&bus, 0x0F, 0xFF);
        CHECK(g_Video.CursorRow >= g_Video.Rows, "crtc 0E/0F: an off-page address (7FFFh) hides the cursor");

        /* e. the INT 33h text cursor: the cell under the pointer, attribute masked */
        memset(&registers,0,sizeof registers);
        VddSetAh(&registers,0x00);
        VddSetAl(&registers,0x03);
        VddBusDeliverInterrupt(&bus,0x10,&registers);
        memset(&registers,0,sizeof registers);
        VddSetAh(&registers,0x02);
        VddSetDx(&registers,(WORD)((24<<8)|79));
        VddBusDeliverInterrupt(&bus,0x10,&registers);
        VideoTestTextCell(2,3)[0]='X';
        VideoTestTextCell(2,3)[1]=0x1F;                /* white on blue */
        VddVideoRender(&g_Video);
        VddVideoTextCursor(&g_Video, 3, 2, 0x77FF, 0x7700); /* the driver's defaults */
        { PCBYTE glyph = g_VgaFont8x16['X'];
        INT mismatches = 0;
          for (glyphRow=0;glyphRow<16;++glyphRow) for (glyphColumn=0;glyphColumn<8;++glyphColumn)
          {
              BYTE expected=(glyph[glyphRow]&(0x80>>glyphColumn))?0:6;
              if (g_Video.FrameBuffer[(2*16+glyphRow)*TXW+3*9+glyphColumn]!=expected)
                  mismatches++; }
          CHECK(mismatches==0, "int33 text cursor: cell (3,2) redrawn with (1F & 77) ^ 77 = 60h: black on brown"); }
        CHECK(VideoTestTextCell(2,3)[1] == 0x1F, "int33 text cursor: VRAM itself is untouched (no trail)");
        VddVideoTextCursor(&g_Video, 80, 2, 0x77FF, 0x7700);
        VddVideoTextCursor(&g_Video, -1, 2, 0x77FF, 0x7700);
        CHECK(1, "int33 text cursor: out-of-range cells are ignored, not written");
        memset(&registers,0,sizeof registers);
        VddSetAh(&registers,0x00);
        VddSetAl(&registers,0x13);
        VddBusDeliverInterrupt(&bus,0x10,&registers);
        VddVideoTextCursor(&g_Video, 3, 2, 0x77FF, 0x7700);
        CHECK(g_Video.ModeKind != VIDEO_KIND_TEXT, "int33 text cursor: a no-op outside text modes");

        /* f. a 40-column mode renders at ITS stride, which is what the frame declares */
        memset(&registers,0,sizeof registers);
        VddSetAh(&registers,0x00);
        VddSetAl(&registers,0x01);
        VddBusDeliverInterrupt(&bus,0x10,&registers);
        memset(&registers,0,sizeof registers);
        VddSetAh(&registers,0x02);
        VddSetDx(&registers,(WORD)((24<<8)|39));
        VddBusDeliverInterrupt(&bus,0x10,&registers);
        VideoTestTextCell(1,0)[0]='A';
        VideoTestTextCell(1,0)[1]=0x0F;
        VddVideoRender(&g_Video);
        { PCBYTE glyph = g_VgaFont8x16['A'];
        INT mismatches = 0;
          for (glyphRow=0;glyphRow<16;++glyphRow) for (glyphColumn=0;glyphColumn<8;++glyphColumn)
          {
              BYTE expected=(glyph[glyphRow]&(0x80>>glyphColumn))?15:0;
              if (g_Video.FrameBuffer[(16+glyphRow)*360+glyphColumn]!=expected)
                  mismatches++; }
          CHECK(mismatches==0, "render(40-col): row 1 is at stride 360 -- 40 nine-dot cells (#324)"); }
        CHECK(textBiosData[0x4A]==40 && textBiosData[0x49]==1, "bda: mode 1 -> 40 columns");
        memset(&registers,0,sizeof registers);
        VddSetAh(&registers,0x00);
        VddSetAl(&registers,0x03);
        VddBusDeliverInterrupt(&bus,0x10,&registers);
        g_Video.BiosData = 0;
    }

    /* THE TWO EXTERNAL READ-ONLY REGISTERS. docs/ref/vga.md 3.
     *
     * [WARNING]: INPUT STATUS 0 WAS 0x00 FOR THREE SESSIONS AND WAS TWICE RECORDED AS
     * "confirmed" on the strength of a single oracle. It is 0x10 -- bit 4,
     * Switch Sense -- measured on PCem's genuine IBM VGA ROM in all twelve
     * modes p_vgareg sets, and dosbox-x drives bit 4 too. Pinned here so the
     * value has a check of its own rather than living only in a switch arm.
     *
     * [INFO]: BIT 7, the CRT interrupt, reads 0 HERE because the mode's CR11 holds it
     * clear (bit 4 = 0), as every BIOS mode does; with it enabled it latches at
     * retrace -- #187, pinned just below.
     */
    {
        UINT32 value;
        VddBusIo(&bus, 0x3C2, 1, 1, &value);
        CHECK(value == 0x10, "ext: Input Status 0 reads 0x10 -- bit 4 Switch Sense");
        CHECK((value & 0x80) == 0, "ext: ...and bit 7, the CRT interrupt, stays low");

        /* #187: BIT 7 IS THE VERTICAL-RETRACE INTERRUPT LATCH (IBM VGA). CR11 bit 5 = 0
         * enables, bit 4 = 0 clears and holds clear; the first retrace start after it
         * is armed sets it. 70 Hz: F = 14285 us, retrace from line 400 (~12726 us).
         */
        {   const UINT64 framePeriod = 14285u, duration = 2000u * 14285u;
            UINT32 crtc11;
            UINT32 portValue;
            UINT64 (*oldClock)(VOID) = g_Video.TimeUs;
            UINT32 oldGraphicsHeight = g_Video.GraphicsHeight;
            g_Video.TimeUs = VideoTestFakeClock;
            g_Video.GraphicsHeight = 200;
            portValue = 0x11;
            VddBusIo(&bus,0x3D4,1,0,&portValue);
            VddBusIo(&bus,0x3D5,1,1,&crtc11);
            g_FakeMicroseconds = duration + 1000;                                   /* in the picture */
            portValue = (crtc11 & 0x4F) | 0x10;
            VddBusIo(&bus,0x3D5,1,0,&portValue);   /* enable, not held */
            g_FakeMicroseconds = duration + 2000;
            VddBusIo(&bus, 0x3C2, 1, 1, &value);
            CHECK((value & 0x80) == 0, "vint: armed in the picture -- no retrace yet, bit 7 low");
            g_FakeMicroseconds = duration + 13000;
            VddBusIo(&bus, 0x3C2, 1, 1, &value);
            CHECK(value == 0x90, "vint: the retrace start sets bit 7 (0x90)");
            g_FakeMicroseconds = duration + framePeriod + 1000;
            VddBusIo(&bus, 0x3C2, 1, 1, &value);
            CHECK(value == 0x90, "vint: ...and it stays latched into the next picture");
            portValue = crtc11 & 0x4F;
            VddBusIo(&bus,0x3D5,1,0,&portValue);            /* bit 4 = 0: clear */
            g_FakeMicroseconds = duration + 2*framePeriod + 13000;
            VddBusIo(&bus, 0x3C2, 1, 1, &value);
            CHECK(value == 0x10, "vint: CR11 bit 4 = 0 clears it and holds it clear");
            g_FakeMicroseconds = duration + 2*framePeriod + 13500;                             /* inside a retrace */
            portValue = (crtc11 & 0x4F) | 0x10;
            VddBusIo(&bus,0x3D5,1,0,&portValue);
            g_FakeMicroseconds = duration + 2*framePeriod + 14000;
            VddBusIo(&bus, 0x3C2, 1, 1, &value);
            CHECK((value & 0x80) == 0, "vint: re-armed mid-retrace -- that retrace began before it");
            g_FakeMicroseconds = duration + 3*framePeriod + 13000;
            VddBusIo(&bus, 0x3C2, 1, 1, &value);
            CHECK(value == 0x90, "vint: ...the next retrace sets it");
            portValue = (crtc11 & 0x4F) | 0x30;
            VddBusIo(&bus,0x3D5,1,0,&portValue);   /* bit 5 = 1: disabled */
            portValue = crtc11 & 0x4F;
            VddBusIo(&bus,0x3D5,1,0,&portValue);   /* clear it */
            portValue = (crtc11 & 0x4F) | 0x30;
            VddBusIo(&bus,0x3D5,1,0,&portValue);
            g_FakeMicroseconds = duration + 5*framePeriod + 13000;
            VddBusIo(&bus, 0x3C2, 1, 1, &value);
            CHECK(value == 0x10, "vint: disabled (bit 5 = 1), a retrace sets nothing");
            VddBusIo(&bus,0x3D5,1,0,&crtc11);                           /* put CR11 back */
            g_Video.TimeUs = oldClock;
            g_Video.GraphicsHeight = oldGraphicsHeight;
            VddBusIo(&bus, 0x3C2, 1, 1, &value);
        }

        /* Feature Control is storage: write at 3DA, read back at 3CA. NO ORACLE
         * CAN ADJUDICATE THIS -- 6.22 and dosbox-x both read 0x00 whatever is
         * written, and PCem reads 0xFF, which is an undecoded port rather than a
         * measurement. The IBM VGA spec says it reads back, so it reads back.
         */
        value = 0x0F;
        VddBusIo(&bus, 0x3DA, 1, 0, &value);
        VddBusIo(&bus, 0x3CA, 1, 1, &value);
        CHECK(value == 0x0F, "ext: Feature Control written at 3DA reads back at 3CA");
        value = 0x00;
        VddBusIo(&bus, 0x3DA, 1, 0, &value);
    }

    /* A MODE SET MUST LEAVE THE SHADOWS AND THE REGISTER FILE SAYING THE SAME THING.
     * docs/inventory/vga.md step 5. VideoLoadModeDefinition filled `*_reg[]` from the
     * measured table, but six registers are not read back from there at all -- the
     * port answers from a live shadow, because the shadow is what the engine uses.
     * So the file was right and the guest still saw the old value. Measured on the
     * rig and reproduced here: mode 3 answered SR2=0F, CR0A/0B=06/07, GR5=00,
     * AR10=00. Five registers; 55 of the 55 bytes of the VGA parity gap.
     *
     * [CAUTION]: THESE EXPECTATIONS COME FROM VGA_MODEDEFS, i.e. from two oracles, not from
     * a datasheet or from this file's own idea of a VGA.
     */
    {
        UINT32 value;
        NTVDD_REGISTERS registers2;
        struct
        {
            PCSTR Name;
            WORD IndexPort;
            WORD DataPort;
            BYTE Index;
            BYTE Expected;
        }
        registerCases[] = {
            { "SR02", 0x3C4, 0x3C5, 0x02, 0x03 },
            { "CR0A", 0x3D4, 0x3D5, 0x0A, 0x0D },
            { "CR0B", 0x3D4, 0x3D5, 0x0B, 0x0E },
            { "GR05", 0x3CE, 0x3CF, 0x05, 0x10 },
        };
        UINT index;
        memset(&registers2, 0, sizeof registers2);
        VddSetAh(&registers2, 0x00);
        VddSetAl(&registers2, 0x03);
        VddBusDeliverInterrupt(&bus, 0x10, &registers2);
        for (index = 0; index < sizeof registerCases / sizeof registerCases[0]; ++index)
        {
            CHAR description[80];
            value = registerCases[index].Index;
            VddBusIo(&bus, registerCases[index].IndexPort, 1, 0, &value);
            value = 0;
            VddBusIo(&bus, registerCases[index].DataPort, 1, 1, &value);
            sprintf(description, "modedef: mode 3 leaves %s = 0x%02X, as a real BIOS does",
                    registerCases[index].Name, registerCases[index].Expected);
            CHECK(value == registerCases[index].Expected, description);
        }
        VddBusIo(&bus, 0x3DA, 1, 1, &value);                   /* reset the AC flip-flop */
        value = 0x10;
        VddBusIo(&bus, 0x3C0, 1, 0, &value);
        value = 0;
        VddBusIo(&bus, 0x3C1, 1, 1, &value);
        CHECK(value == 0x0C, "modedef: mode 3 leaves AR10 = 0x0C (line graphics + blink)");
        CHECK(g_Video.CursorShape == 0x0D0E,
              "modedef: the cursor shape is CR0A/CR0B, 0x0D0E for an 8x16 cell");

        /* [CAUTION]: GR7 IS THE BYTE THE TWO ORACLES SPLIT ON, and the split is not noise:
         * both say 0x0F in the GRAPHICS modes, and in the text and CGA modes QEMU
         * says 0x0F where PCem's real IBM VGA ROM says 0x00. Taking the table
         * rather than a constant is the whole point -- generating this from QEMU
         * alone would have written 0x0F into the text modes AGAINST the real card
         * and it would have LOOKED like a fix, because parity would have moved.
         */
        value = 0x07;
        VddBusIo(&bus, 0x3CE, 1, 0, &value);
        value = 0;
        VddBusIo(&bus, 0x3CF, 1, 1, &value);
        CHECK(value == 0x00, "modedef: mode 3 GR7 = 0x00 -- PCem's answer, not QEMU's 0x0F");

        memset(&registers2, 0, sizeof registers2);
        VddSetAh(&registers2, 0x00);
        VddSetAl(&registers2, 0x12);
        VddBusDeliverInterrupt(&bus, 0x10, &registers2);
        value = 0x07;
        VddBusIo(&bus, 0x3CE, 1, 0, &value);
        value = 0;
        VddBusIo(&bus, 0x3CF, 1, 1, &value);
        CHECK(value == 0x0F, "modedef: mode 12h GR7 = 0x0F -- both oracles agree there");
        /* ...and the per-kind arm still wins where it has to: planar forces the
         * map mask to all four planes after the table has been loaded.
         */
        value = 0x02;
        VddBusIo(&bus, 0x3C4, 1, 0, &value);
        value = 0;
        VddBusIo(&bus, 0x3C5, 1, 1, &value);
        CHECK(value == 0x0F, "modedef: mode 12h keeps map_mask 0x0F -- the planar arm wins");

        memset(&registers2, 0, sizeof registers2);
        VddSetAh(&registers2, 0x00);
        VddSetAl(&registers2, 0x03);
        VddBusDeliverInterrupt(&bus, 0x10, &registers2);
    }

    /* T#252: THE CHARACTER SERVICES IN THE GRAPHICS MODES, PAGES, AND AH=12h. Every
     * expectation is a byte PCem's genuine IBM VGA ROM wrote in p_vidtxt (DOSBox-X
     * agrees on each), re-derived here from the ROM font so a font edit cannot hide a
     * layout bug. Before #252 all of these failed: the glyphs went to B800:0 as
     * (char, attr) pairs, 05h never moved the CRTC, and 12h said "supported" to all.
     */
    {
        NTVDD_REGISTERS registers2;
        static BYTE biosData[0x100];
        INT row;
        INT isOk;
        UINT32 value;
        g_Video.BiosData = biosData;
#define I10(ax_, bx_, cx_, dx_) do { memset(&registers2, 0, sizeof registers2); registers2.Eax = (ax_); registers2.Ebx = (bx_); \
            registers2.Ecx = (cx_); registers2.Edx = (dx_); VddBusDeliverInterrupt(&bus, 0x10, &registers2); } while (0)
        /* mode 13h: 'A' in colour 0Eh over a 55h fill -> glyph bits 0Eh, the rest 00h */
        I10(0x0013, 0, 0, 0);
        memset(g_VideoMemory, 0x55, 8 * 320);
        I10(0x0941, 0x000E, 1, 0);
        for (isOk = 1, row = 0; row < 8; ++row) { INT column;
            for (column = 0; column < 8; ++column)
                if (g_VideoMemory[row * 320 + column] != ((g_VgaFont8x8['A'][row] & (0x80 >> column)) ? 0x0E : 0x00))
                    isOk = 0; }
        CHECK(isOk, "#252 13h: AH=09h draws the 8x8 glyph at A000:0, background 00h (was (char,attr) at B800:0)");
        CHECK(g_VideoMemory[VIDEO_TEXT_OFFSET] != 'A' || g_VideoMemory[VIDEO_TEXT_OFFSET + 1] != 0x0E,
              "#252 13h: ...and nothing is written to B800:0 as a text cell");
        I10(0x0941, 0x008E, 1, 0);           /* bit 7 is part of the colour in 13h, not XOR */
        CHECK(g_VideoMemory[2] == 0x8E && g_VideoMemory[0] == 0x00, "#252 13h: BL=8Eh is the colour 8Eh (no XOR in 256 colours -- PCem)");
        I10(0x0200, 0, 0, 0x0001);
        I10(0x0E42, 0x000C, 0, 0);
        CHECK(g_VideoMemory[8 + 0] == ((g_VgaFont8x8['B'][0] & 0x80) ? 0x0C : 0) && g_Video.CursorColumn == 2,
              "#252 13h: AH=0Eh draws 'B' at column 1 in BL and advances the cursor to 2");
        I10(0x0200, 0, 0, 0x0001);
        I10(0x0800, 0, 0, 0);
        CHECK((registers2.Eax & 0xFF) == 'B', "#252 13h: AH=08h reads 'B' back from the pixels");
        /* 06h in 13h: row 1 moves up to row 0, row 1 filled with BH */
        I10(0x0013, 0, 0, 0);
        I10(0x0200, 0, 0, 0x0100);
        I10(0x0958, 0x000F, 1, 0);
        I10(0x0601, 0x0700, 0, 0x0127);
        CHECK(g_VideoMemory[1] == ((g_VgaFont8x8['X'][0] & 0x40) ? 0x0F : 0) && g_VideoMemory[8 * 320] == 0x07
              && g_VideoMemory[8 * 320 + 319] == 0x07,
              "#252 13h: AH=06h scrolls pixels by a character row and fills with BH");
        /* mode 04h: two bits a pixel across the interleaved banks */
        I10(0x0004, 0, 0, 0);
        I10(0x0941, 0x0003, 1, 0);
        for (isOk = 1, row = 0; row < 8; ++row)
        {
            UINT32 offset = VIDEO_TEXT_OFFSET + ((row & 1) ? 0x2000u : 0u) + (UINT32)(row >> 1) * 80u;
            WORD word = 0;
            INT index;
            for (index = 0; index < 8; ++index)
                if (g_VgaFont8x8['A'][row] & (0x80 >> index))
                    word |= (WORD)(3u << (14 - 2 * index));
            if (g_VideoMemory[offset] != (BYTE)(word >> 8) || g_VideoMemory[offset + 1] != (BYTE)word)
                isOk = 0;
        }
        CHECK(isOk, "#252 04h: AH=09h draws 'A' two bits a pixel, even lines at B800:0, odd at B800:2000");
        I10(0x0941, 0x0083, 1, 0);
        CHECK(g_VideoMemory[VIDEO_TEXT_OFFSET] == 0 && g_VideoMemory[VIDEO_TEXT_OFFSET + 0x2000] == 0, "#252 04h: BL bit 7 XORs it off again");
        I10(0x0C02, 0, 5, 5);
        I10(0x0D00, 0, 5, 5);
        CHECK(g_VideoMemory[VIDEO_TEXT_OFFSET + 0x2000 + 2 * 80 + 1] == 0x20 && (registers2.Eax & 0xFF) == 2,
              "#252 04h: AH=0Ch/0Dh write and read pixel (5,5) = 2 -- byte 20h at B800:20A1 (was not written, read 0)");
        /* mode 0Dh: planes, the 8x8 cell, 40 bytes a line, BL bits 4-6 not a background */
        I10(0x000D, 0, 0, 0);
        I10(0x0200, 0, 0, 0x0001);
        I10(0x0941, 0x001E, 1, 0);
        for (isOk = 1, row = 0; row < 8; ++row)
        {
            if (g_Video.Planes[0][row * 40 + 1] != 0)
                isOk = 0;
            if (g_Video.Planes[1][row * 40 + 1] != g_VgaFont8x8['A'][row])
                isOk = 0;
            if (g_Video.Planes[3][row * 40 + 1] != g_VgaFont8x8['A'][row])
                isOk = 0;
        }
        CHECK(isOk, "#252 0Dh: 'A' in colour Eh at 40 bytes a line, 8 lines; plane 0 stays 0 (BL bit 4 is not a background)");
        /* 05h: page 1 of 0Dh = CRTC start 2000h, and a write to page 1 lands there */
        I10(0x0501, 0, 0, 0);
        value = 0x0C;
        VddBusIo(&bus, 0x3D4, 1, 0, &value);
        value = 0;
        VddBusIo(&bus, 0x3D5, 1, 1, &value);
        CHECK(value == 0x20 && g_Video.CrtcStartLive == 0x2000, "#252 0Dh: AH=05h AL=1 loads the CRTC start with 2000h (bytes)");
        I10(0x0500, 0, 0, 0);
        /* mode 10h: the 8x14 cell at 80 bytes a line */
        I10(0x0010, 0, 0, 0);
        I10(0x0200, 0, 0, 0x0101);
        I10(0x0941, 0x000E, 1, 0);
        for (isOk = 1, row = 0; row < 14; ++row)
            if (g_Video.Planes[1][(14 + row) * 80 + 1] != g_VgaFont8x14['A'][row])
                isOk = 0;
        CHECK(isOk, "#252 10h: 'A' is the 8x14 glyph at row 1 (line 14), 80 bytes a line (was 8x16 at 640/8 of 480)");
        /* mode 3 pages */
        I10(0x0003, 0, 0, 0);
        I10(0x0501, 0, 0, 0);
        value = 0x0C;
        VddBusIo(&bus, 0x3D4, 1, 0, &value);
        value = 0;
        VddBusIo(&bus, 0x3D5, 1, 1, &value);
        CHECK(value == 0x08, "#252 03h: AH=05h AL=1 loads the CRTC start with 0800h (words)");
        I10(0x0200, 0x0100, 0, 0x0203);
        I10(0x0300, 0x0000, 0, 0);
        CHECK((registers2.Edx & 0xFFFF) == 0x0000, "#252 03h: page 0's cursor is its own (BH=0 reads 0000 while page 1's is 0203)");
        I10(0x095A, 0x011F, 1, 0);
        CHECK(g_VideoMemory[VIDEO_TEXT_OFFSET + 0x1000 + (2 * 80 + 3) * 2] == 'Z' && g_VideoMemory[VIDEO_TEXT_OFFSET + (2 * 80 + 3) * 2] != 'Z',
              "#252 03h: AH=09h BH=1 writes page 1 (B900) at page 1's cursor, not page 0");
        I10(0x0E51, 0x0007, 0, 0);
        CHECK(g_VideoMemory[VIDEO_TEXT_OFFSET + 0x1000 + (2 * 80 + 3) * 2] == 'Q',
              "#252 03h: AH=0Eh writes the ACTIVE page whatever BH says (PCem's IBM ROM)");
        g_VideoMemory[VIDEO_TEXT_OFFSET + (2 * 80 + 3) * 2] = 'W';      /* page 0, same cell: must NOT show */
        g_VideoMemory[VIDEO_TEXT_OFFSET + (2 * 80 + 3) * 2 + 1] = 0x1F;
        VddVideoRender(&g_Video);
        for (isOk = 1, row = 0; row < 16; ++row) { INT column;
            for (column = 0; column < 8; ++column)
                if (g_Video.FrameBuffer[(2 * 16 + row) * TXW + 3 * 9 + column] != ((g_VgaFont8x16['Q'][row] & (0x80 >> column)) ? 0x0F : 0x01))
                    isOk = 0; }
        CHECK(isOk, "#252 03h: the renderer SHOWS page 1 (from the CRTC start) -- 'Q' in 1Fh at row 2, column 3");
        I10(0x0500, 0, 0, 0);
        /* AH=12h */
        I10(0x1201, 0x0030, 0, 0);
        CHECK((registers2.Eax & 0xFF) == 0x12, "#252 12h BL=30h: AL=12h");
        I10(0x0003, 0, 0, 0);
        CHECK(g_Video.CellHeight == 14 && biosData[0x85] == 14 && biosData[0x84] == 24 && biosData[0x89] == 0x01,
              "#252 12h BL=30h AL=1, then mode 3: 8x14 cell, 25 rows, 0040:0089 = 01h (PCem)");
        I10(0x1202, 0x0030, 0, 0);
        I10(0x0003, 0, 0, 0);
        CHECK(g_Video.CellHeight == 16, "#252 12h BL=30h AL=2: back to 400 lines");
        I10(0x1277, 0x0055, 0, 0);
        CHECK((registers2.Eax & 0xFF) == 0x00, "#252 12h unknown BL: AL=00h (PCem's IBM ROM) -- not 12h");
        I10(0x1201, 0x0033, 0, 0);
        CHECK((registers2.Eax & 0xFF) == 0x12 && g_Video.IsGreySum == 0, "#252 12h BL=33h AL=1: summing off, AL=12h");
        I10(0x1200, 0x0033, 0, 0);
        I10(0x0013, 0, 0, 0);
        { UINT32 colour = g_Video.Dac[1];
        CHECK(((colour >> 16) & 0xFF) == ((colour >> 8) & 0xFF) && ((colour >> 8) & 0xFF) == (colour & 0xFF),
              "#252 12h BL=33h AL=0: the next mode set's DAC comes up grey"); }
        I10(0x1201, 0x0033, 0, 0);
        I10(0x0003, 0, 0, 0);
        I10(0x1201, 0x0034, 0, 0);
        CHECK((registers2.Eax & 0xFF) == 0x12 && g_Video.IsCursorEmulationOff == 1 && (biosData[0x87] & 1), "#252 12h BL=34h AL=1: emulation off, 0040:0087 bit 0");
        I10(0x1200, 0x0034, 0, 0);
        I10(0x1203, 0x0036, 0, 0);
        CHECK((registers2.Eax & 0xFF) == 0x00, "#252 12h BL=36h AL=3: out of range, refused");

        /* T#266: INT 10h AFTER #252's REMAINDERS:
         * The pure arithmetic first: the CGA colour-select byte and what it becomes in
         * the attribute controller. The anchor is the MEASURED mode 04h table: from
         * the mode set's own 0066 = 30h the arithmetic must give AR01-03 = 13h/15h/17h
         * (vga_modedefs.h, PCem's IBM ROM) -- or the formula is wrong, not the card.
         */
        { BYTE attributes[3];
          VddCgaPaletteAr(0x30, attributes);
          CHECK(attributes[0] == 0x13 && attributes[1] == 0x15 && attributes[2] == 0x17,
                "#266 0Bh: 0066=30h (palette 1, intensity) -> AR01-03 13/15/17 = the measured mode 04h table");
          VddCgaPaletteAr(0x10, attributes);
          CHECK(attributes[0] == 0x12 && attributes[1] == 0x14 && attributes[2] == 0x16, "#266 0Bh: palette 0 + intensity -> 12/14/16 (light green/red/yellow)");
          VddCgaPaletteAr(0x20, attributes);
          CHECK(attributes[0] == 0x03 && attributes[1] == 0x05 && attributes[2] == 0x07, "#266 0Bh: palette 1, no intensity -> 03/05/07 (cyan/magenta/white)");
          VddCgaPaletteAr(0x00, attributes);
          CHECK(attributes[0] == 0x02 && attributes[1] == 0x04 && attributes[2] == 0x06, "#266 0Bh: palette 0, no intensity -> 02/04/06 (green/red/brown)"); }
        CHECK(VddCgaBackgroundAr(0x04) == 0x04 && VddCgaBackgroundAr(0x0C) == 0x14 && VddCgaBackgroundAr(0x1F) == 0x17,
              "#266 0Bh: BL IRGB -> AC value, intensity (bit 3) to bit 4: 04->04, 0C->14, 1F->17");
        CHECK(VddCgaColourSelect(0x30, 0, 0x1C) == 0x3C && VddCgaColourSelect(0x3C, 1, 0x00) == 0x1C
              && VddCgaColourSelect(0x1C, 1, 0xFF) == 0x3C,
              "#266 0Bh: 0066 -- BH=0 replaces bits 0-4, BH=1 only bit 5 (from BL bit 0)");
        CHECK(VddGraphicsFontRows(0, 30) == 30 && VddGraphicsFontRows(1, 0) == 14 && VddGraphicsFontRows(2, 0) == 25
              && VddGraphicsFontRows(3, 0) == 43 && VddGraphicsFontRows(7, 9) == 25,
              "#266 11h/2xh: BL rows -- 0 = DL, 1 = 14, 2 = 25, 3 = 43, other = 25");

        /* ...and through INT 10h, in mode 04h, read back the way a guest does (3C0/3C1). */
        I10(0x0004, 0, 0, 0);
        CHECK(biosData[0x66] == 0x30 && g_Video.PaletteRegisters[1] == 0x13 && g_Video.PaletteRegisters[3] == 0x17, "#266 04h: mode set leaves 0066=30h, AR01/03 13h/17h");
        I10(0x0B00, 0x0100, 0, 0);
        CHECK(g_Video.PaletteRegisters[1] == 0x12 && g_Video.PaletteRegisters[2] == 0x14 && g_Video.PaletteRegisters[3] == 0x16 && biosData[0x66] == 0x10,
              "#266 04h: 0Bh BH=1 BL=0 -> palette 0 (12/14/16), intensity kept, 0066=10h");
        I10(0x0B00, 0x0004, 0, 0);
        { UINT32 readValue;
        value = 0x00;
        VddBusIo(&bus, 0x3DA, 1, 1, &readValue);
        VddBusIo(&bus, 0x3C0, 1, 0, &value);
          readValue = 0;
          VddBusIo(&bus, 0x3C1, 1, 1, &readValue);
          CHECK(readValue == 0x04 && g_Video.PaletteRegisters[16] == 0x04 && biosData[0x66] == 0x04
                && g_Video.PaletteRegisters[1] == 0x02 && g_Video.PaletteRegisters[2] == 0x04 && g_Video.PaletteRegisters[3] == 0x06,
                "#266 04h: 0Bh BH=0 BL=04h -> AR00 = AR11 = 04h (read back at 3C1), BL bit 4 clear drops the intensity");
          value = 0x11;
          VddBusIo(&bus, 0x3DA, 1, 1, &readValue);
          VddBusIo(&bus, 0x3C0, 1, 0, &value);
          readValue = 0;
          VddBusIo(&bus, 0x3C1, 1, 1, &readValue);
          CHECK(readValue == 0x04, "#266 04h: ...AR11 (border) reads 04h too -- 0Bh used to write a shadow nothing read"); }
        /* The renderer: a pixel's value is the AC index -- pixel 0 shows the background. */
        memset(g_VideoMemory + VIDEO_TEXT_OFFSET, 0, 0x4000);
        g_VideoMemory[VIDEO_TEXT_OFFSET] = 0x1B;                       /* pixels 0,1,2,3 */
        g_Video.IsDirty = 1;
        VddBusFrame(&bus);
        CHECK(g_Video.FrameBuffer[0] == 0 && g_Video.FrameBuffer[1] == 1 && g_Video.FrameBuffer[2] == 2 && g_Video.FrameBuffer[3] == 3
              && g_Video.Frame.Palette[0] == g_Video.Dac[0x04] && g_Video.Frame.Palette[1] == g_Video.Dac[0x02],
              "#266 04h: render_cga emits the 2-bit value; colour 0 = DAC[AR00] = the background just set");
        I10(0x0B00, 0x0101, 0, 0);
        CHECK(g_Video.PaletteRegisters[1] == 0x03 && g_Video.PaletteRegisters[3] == 0x07 && biosData[0x66] == 0x24 && g_Video.Frame.Palette[0] == g_Video.Dac[0x04],
              "#266 04h: 0Bh BH=1 BL=1 -> palette 1 without intensity (03/05/07), background kept");
        I10(0x0B00, 0x0200, 0, 0);
        CHECK(g_Video.PaletteRegisters[1] == 0x03 && biosData[0x66] == 0x24, "#266 0Bh: BH=2 is not a function -- nothing moves");
        /* mode 06h: BH=0 is the foreground (AR01), the background stays black */
        I10(0x0006, 0, 0, 0);
        I10(0x0B00, 0x0004, 0, 0);
        CHECK(g_Video.PaletteRegisters[1] == 0x04 && g_Video.PaletteRegisters[0] == 0x00 && g_Video.PaletteRegisters[16] == 0x04 && biosData[0x66] == 0x24,
              "#266 06h: 0Bh BH=0 BL=04h -> AR01 (foreground) = 04h, AR00 stays 00h, border 04h");
        /* text: the border only */
        I10(0x0003, 0, 0, 0);
        I10(0x0B00, 0x0001, 0, 0);
        CHECK(g_Video.PaletteRegisters[16] == 0x01 && g_Video.PaletteRegisters[0] == 0x00 && biosData[0x66] == 0x21,
              "#266 03h: 0Bh BH=0 -> the border (AR11) only; AR00 is text colour 0 and is left alone");

        /* AH=04h: no light pen on a VGA -- AH=00h, the rest untouched */
        I10(0x0400, 0xB1B1, 0xC1C1, 0xD1D1);
        CHECK((registers2.Eax & 0xFF00) == 0 && (registers2.Ebx & 0xFFFF) == 0xB1B1 && (registers2.Edx & 0xFFFF) == 0xD1D1,
              "#266 04h: light pen -> AH=00h (not triggered); BX/DX untouched");

        /* AH=11h AL=03h: SR3 gets BL, and a loaded user font is NOT dropped */
        { static BYTE userFont[16];
        INT index;
        for (index = 0; index < 16; ++index)
            userFont[index] = 0xAA;
          memcpy(g_GuestMemory + 0x40000, userFont, 16);
          memset(&registers2, 0, sizeof registers2);
          registers2.Eax = 0x1100;
          registers2.Ebx = 0x1000;
          registers2.Ecx = 1;
          registers2.Edx = 'Z';
          registers2.Es = 0x4000;
          registers2.Ebp = 0;
          VddBusDeliverInterrupt(&bus, 0x10, &registers2); }
        CHECK(g_Video.IsUserFontOn == 1, "#266 11h/00h: a user font is loaded (setup)");
        I10(0x1103, 0x0005, 0, 0);
        { UINT32 readValue;
        value = 0x03;
        VddBusIo(&bus, 0x3C4, 1, 0, &value);
        readValue = 0;
        VddBusIo(&bus, 0x3C5, 1, 1, &readValue);
          CHECK(readValue == 0x05 && g_Video.IsUserFontOn == 1,
                "#266 11h/03h: SR3 = BL (05h) and the user font survives (it used to reload the ROM 8x16)"); }
        I10(0x1103, 0x0000, 0, 0);

        /* AH=11h AL=22h-24h in mode 12h: INT 43h, rows, height -- and the BDA follows */
        { PCBYTE interruptTable = g_GuestMemory;
#define VEC(vector)     ((UINT32)(interruptTable[(vector)*4] | (interruptTable[(vector)*4+1] << 8)) | ((UINT32)(interruptTable[(vector)*4+2] | (interruptTable[(vector)*4+3] << 8)) << 16))
          I10(0x0012, 0, 0, 0);
          CHECK(VEC(0x43) == ((UINT32)VDD_FONT8X16_SEG << 16), "#266 12h: the mode set points INT 43h at the 8x16 table");
          I10(0x1122, 0x0001, 0, 0);
          CHECK(VEC(0x43) == ((UINT32)VDD_FONT8X14_SEG << 16) && biosData[0x84] == 13 && biosData[0x85] == 14,
                "#266 11h/22h BL=1: INT 43h = 8x14, 0040:0084 = 13 (14 rows), 0085 = 14");
          I10(0x1123, 0x0002, 0, 0xD1D1);
          CHECK(VEC(0x43) == ((UINT32)VDD_FONT8X8_SEG << 16) && biosData[0x84] == 24 && biosData[0x85] == 8
                && (registers2.Edx & 0xFFFF) == 0xD1D1,
                "#266 11h/23h BL=2: 8x8, 25 rows; DX left as it came (no longer overwritten)");
          I10(0x1124, 0x0003, 0, 0);
          CHECK(biosData[0x84] == 42 && biosData[0x85] == 16, "#266 11h/24h BL=3: 8x16, 43 rows");
          I10(0x1123, 0x0000, 0, 0x001E);
          CHECK(biosData[0x84] == 29 && biosData[0x85] == 8, "#266 11h/23h BL=0 DL=30: 30 rows from DL");
          /* 21h: the caller's font, drawn from where INT 43h points */
          {
              INT index;
              for (index = 0; index < 256 * 10; ++index)
                  g_GuestMemory[0x50000 + index] = 0;
              for (index = 0; index < 10; ++index)
                  g_GuestMemory[0x50000 + 'Q' * 10 + index] = 0x81;
          }
          memset(&registers2, 0, sizeof registers2);
          registers2.Eax = 0x1121;
          registers2.Ebx = 0x0000;
          registers2.Ecx = 10;
          registers2.Edx = 48;
          registers2.Es = 0x5000;
          registers2.Ebp = 0;
          VddBusDeliverInterrupt(&bus, 0x10, &registers2);
          CHECK(VEC(0x43) == 0x50000000u && biosData[0x84] == 47 && biosData[0x85] == 10 && g_Video.IsGraphicsFontUser,
                "#266 11h/21h: INT 43h = ES:BP, 48 rows (DL), height CX = 10");
          memset(&registers2, 0, sizeof registers2);
          registers2.Eax = 0x1130;
          registers2.Ebx = 0x0100;
          VddBusDeliverInterrupt(&bus, 0x10, &registers2);
          CHECK(registers2.Es == 0x5000 && (registers2.Ebp & 0xFFFF) == 0 && (registers2.Ecx & 0xFFFF) == 10,
                "#266 11h/30h BH=1 answers with the caller's font (= INT 43h), CX = 10");
          I10(0x0200, 0, 0, 0x0001);
          I10(0x0951, 0x000F, 1, 0);
          { INT isOk2 = 1;
          for (row = 0; row < 10; ++row)
              if (g_Video.Planes[0][row * 80 + 1] != 0x81)
                  isOk2 = 0;
            CHECK(isOk2 && g_Video.Planes[0][10 * 80 + 1] == 0, "#266 12h: AH=09h draws 'Q' from the caller's 10-line table at INT 43h"); }
          /* 20h: INT 1Fh */
          memset(&registers2, 0, sizeof registers2);
          registers2.Eax = 0x1120;
          registers2.Es = 0x5100;
          registers2.Ebp = 0x0010;
          VddBusDeliverInterrupt(&bus, 0x10, &registers2);
          CHECK(VEC(0x1F) == 0x51000010u, "#266 11h/20h: INT 1Fh = ES:BP");
          memset(&registers2, 0, sizeof registers2);
          registers2.Eax = 0x1130;
          registers2.Ebx = 0x0000;
          VddBusDeliverInterrupt(&bus, 0x10, &registers2);
          CHECK(registers2.Es == 0x5100 && (registers2.Ebp & 0xFFFF) == 0x0010, "#266 11h/30h BH=0 answers with INT 1Fh");
          I10(0x0003, 0, 0, 0);
          CHECK(!g_Video.IsGraphicsFontUser && VEC(0x43) == ((UINT32)VDD_FONT8X16_SEG << 16) && VEC(0x1F) == 0x51000010u,
                "#266: a mode set takes INT 43h back to the ROM; INT 1Fh (a POST vector) is left as set");
          VddVideoReset(&g_Video);   /* INT 1Fh back to ours for the tests after */
#undef VEC
        }

        /* 0040:00A8 -> the save pointer table -> the parameter table */
        VddVideoInstallFonts(&g_Video);
        { WORD saveOffset = (WORD)(biosData[0xA8] | (biosData[0xA9] << 8)), saveSegment = (WORD)(biosData[0xAA] | (biosData[0xAB] << 8));
          PCBYTE savePointer = g_GuestMemory + ((UINT32)saveSegment << 4) + saveOffset;
          PCBYTE parameterTable;
          PCBYTE mode3Entry;
          PCBYTE mode13Entry;
          WORD parameterOffset = (WORD)(savePointer[0] | (savePointer[1] << 8));
          WORD parameterSegment = (WORD)(savePointer[2] | (savePointer[3] << 8));
          BYTE reference[64];
          INT isOk3;
          parameterTable = g_GuestMemory + ((UINT32)parameterSegment << 4) + parameterOffset;
          mode3Entry = parameterTable + 0x18 * 64;
          mode13Entry = parameterTable + 0x1C * 64;
          CHECK(saveSegment == VDD_VIDTAB_SEG && saveOffset == 0 && parameterSegment == VDD_VIDTAB_SEG && parameterOffset == VDD_VPARAM_OFF,
                "#266 0040:00A8 -> B270:0000, whose first pointer -> the parameter table");
          CHECK(mode3Entry[0] == 80 && mode3Entry[1] == 24 && mode3Entry[2] == 16 && mode3Entry[3] == 0x00 && mode3Entry[4] == 0x10
                && mode3Entry[9] == 0x67 && mode3Entry[0x0A] == 0x5F && mode3Entry[0x0A + 0x13] == 0x28 && mode3Entry[0x0A + 0x0A] == 0x0D
                && mode3Entry[0x23 + 6] == 0x14 && mode3Entry[0x23 + 0x10] == 0x0C && mode3Entry[0x37 + 5] == 0x10 && mode3Entry[0x37 + 6] == 0x0E,
                "#266 param slot 18h (mode 3+): 80 cols, 25 rows, 16 high, 1000h; misc 67h, CR00 5Fh, CR13 28h, AR06 14h, GR6 0Eh");
          CHECK(mode13Entry[0] == 40 && mode13Entry[2] == 8 && mode13Entry[9] == 0x63 && mode13Entry[0x05 + 3] == 0x0E && mode13Entry[0x37 + 5] == 0x40,
                "#266 param slot 1Ch (13h): 40 cols, 8 high, misc 63h, SR4 0Eh (chain-4), GR5 40h");
          for (isOk3 = 1, row = 0; row < 29; ++row)
          {
              (VOID)VddVideoParameterEntry((BYTE)row, reference);
              if (memcmp(reference, parameterTable + row * 64, 64))
                  isOk3 = 0;
          }
          CHECK(isOk3 && parameterTable[3 * 64] == 0 && parameterTable[0x11 * 64] == 0,
                "#266 the table in memory is vdd_video_param_entry's; slot 03h (200-line) and 11h (0Fh) are zero -- unmeasured");
          CHECK(savePointer[0x10] == VDD_SAVEPTR2_OFF && savePointer[0x04] == 0 && g_GuestMemory[((UINT32)VDD_VIDTAB_SEG << 4) + VDD_DCC_OFF] == 16,
                "#266 save table +10h -> secondary table; dynamic save area 0; DCC table has 16 entries"); }

        /* SR1 bit 5: the picture goes black, and comes back */
        I10(0x0013, 0, 0, 0);
        memset(g_VideoMemory, 0x0F, 320 * 200);
        I10(0x1201, 0x0036, 0, 0);                         /* refresh OFF -> SR1.5 */
        g_Video.IsDirty = 1;
        VddBusFrame(&bus);
        CHECK(g_Video.IsBlanked && g_Video.Frame.Stride == 0 && g_Video.Frame.Palette[g_Video.Frame.Pixels[0]] == 0xFF000000u
              && g_Video.Frame.Width == 320 && g_Video.Frame.Height == 200,
              "#266 SR1.5 (12h BL=36h AL=1): the frame goes out black, geometry kept");
        I10(0x1200, 0x0036, 0, 0);
        g_Video.IsDirty = 1;
        VddBusFrame(&bus);
        CHECK(!g_Video.IsBlanked && g_Video.Frame.Pixels == g_VideoMemory && g_Video.Frame.Palette[0x0F] == g_Video.Dac[0x0F],
              "#266 SR1.5 cleared: the same picture is back, nothing in VRAM was touched");
        VideoTestWriteSequencer(&bus, 0x01, 0x21);                             /* a guest's own write */
        g_Video.IsDirty = 1;
        VddBusFrame(&bus);
        CHECK(g_Video.IsBlanked, "#266 SR1.5 written at 3C5h by the guest blanks too");
        I10(0x0003, 0, 0, 0);
        g_Video.IsDirty = 1;
        VddBusFrame(&bus);
        CHECK(!g_Video.IsBlanked, "#266 a mode set clears SR1.5 (every measured mode's SR1 has bit 5 = 0)");
#undef I10
        g_Video.BiosData = 0;
    }

    /* -- #325: THE DISPLAYED SIZE COMES FROM THE CRTC. For every graphics mode whose
     * measured register set we load, the CRTC-derived size must equal the table --
     * that is the derivation's calibration -- and a guest that reprograms the CRTC
     * (Mode X) gets the size it programmed.
     */
    {
        static const struct
        {
            BYTE Mode;
            WORD Width;
            WORD Height;
        }
        graphicsModes[] = {
            { 0x0D, 320, 200 }, { 0x0E, 640, 200 }, { 0x10, 640, 350 },
            { 0x11, 640, 480 }, { 0x12, 640, 480 }, { 0x13, 320, 200 },
        };
        UINT index;
        INT badCount = 0;
        NTVDD_REGISTERS modeRegisters;
        for (index = 0; index < sizeof graphicsModes / sizeof graphicsModes[0]; ++index)
        {
            INT graphicsWidth;
            INT graphicsHeight;
            memset(&modeRegisters, 0, sizeof modeRegisters);
            VddSetAh(&modeRegisters, 0x00);
            VddSetAl(&modeRegisters, graphicsModes[index].Mode);
            VddBusDeliverInterrupt(&bus, 0x10, &modeRegisters);
            g_Video.IsDirty = 1;
            VddBusFrame(&bus);
            VddVideoGeometry(&g_Video, &graphicsWidth, &graphicsHeight);
            if (!g_Video.IsGeometryRegistersOk || graphicsWidth != graphicsModes[index].Width || graphicsHeight != graphicsModes[index].Height
                || g_Video.Frame.Width != graphicsModes[index].Width || g_Video.Frame.Height != graphicsModes[index].Height)
            {
                printf("        mode %02Xh: regs_ok=%d geom %dx%d frame %ux%u, want %ux%u\n", graphicsModes[index].Mode,
                       g_Video.IsGeometryRegistersOk, graphicsWidth, graphicsHeight, g_Video.Frame.Width, g_Video.Frame.Height, graphicsModes[index].Width, graphicsModes[index].Height);
                badCount++;
            }
        }
        CHECK(badCount == 0, "#325 geometry: CRTC-derived size == the mode table for 0Dh 0Eh 10h 11h 12h 13h");
        /* Mode X: 13h, unchained, then the classic 240-line CRTC program. */
        memset(&modeRegisters, 0, sizeof modeRegisters);
        VddSetAh(&modeRegisters, 0x00);
        VddSetAl(&modeRegisters, 0x13);
        VddBusDeliverInterrupt(&bus, 0x10, &modeRegisters);
        VideoTestWriteSequencer(&bus, 0x04, 0x06);                              /* chain-4 off */
        VideoTestWriteCrtc(&bus, 0x11, 0x0E);                            /* unprotect CR0-7 first */
        VideoTestWriteCrtc(&bus, 0x06, 0x0D);
        VideoTestWriteCrtc(&bus, 0x07, 0x3E);
        VideoTestWriteCrtc(&bus, 0x09, 0x41);
        VideoTestWriteCrtc(&bus, 0x10, 0xEA);
        VideoTestWriteCrtc(&bus, 0x11, 0xAC);
        VideoTestWriteCrtc(&bus, 0x12, 0xDF);
        VideoTestWriteCrtc(&bus, 0x14, 0x00);
        VideoTestWriteCrtc(&bus, 0x15, 0xE7);
        VideoTestWriteCrtc(&bus, 0x16, 0x06);
        VideoTestWriteCrtc(&bus, 0x17, 0xE3);
        g_Video.IsDirty = 1;
        VddBusFrame(&bus);
        CHECK(g_Video.Frame.Width == 320 && g_Video.Frame.Height == 240,
              "#325 geometry: Mode X (VDE 480, CR09 41h) presents 320x240, not mode 13h's 320x200");
        VideoTestWriteCrtc(&bus, 0x11, 0x0E);
        VideoTestWriteCrtc(&bus, 0x01, 0x59);  /* 90 char clocks -> 360 */
        VideoTestWriteCrtc(&bus, 0x09, 0x40);                            /* no line repeat -> 480 */
        g_Video.IsDirty = 1;
        VddBusFrame(&bus);
        CHECK(g_Video.Frame.Width == 360 && g_Video.Frame.Height == 480, "#325 geometry: CR01 59h + CR09 40h presents 360x480");
        memset(&modeRegisters, 0, sizeof modeRegisters);
        VddSetAh(&modeRegisters, 0x00);
        VddSetAl(&modeRegisters, 0x03);
        VddBusDeliverInterrupt(&bus, 0x10, &modeRegisters);
        g_Video.IsDirty = 1;
        VddBusFrame(&bus);
        CHECK(g_Video.Frame.Width == 720 && g_Video.Frame.Height == 400, "#325 geometry: a mode set back to 3 is 720x400 again");
    }

    /* #325: a VESA 8bpp mode set loads the 256-colour default DAC, as mode 13h does --
     * after a 16-colour mode (0Dh) colour 15 drew grey, seen on the rig.
     */
    {   NTVDD_REGISTERS registers3;
    UINT32 mode13Colour;
    UINT32 vesaColour;
        memset(&registers3, 0, sizeof registers3);
        VddSetAh(&registers3, 0x00);
        VddSetAl(&registers3, 0x13);
        VddBusDeliverInterrupt(&bus, 0x10, &registers3);
        g_Video.IsDirty = 1;
        VddBusFrame(&bus);
        mode13Colour = g_Video.Palette[15];
        memset(&registers3, 0, sizeof registers3);
        VddSetAh(&registers3, 0x00);
        VddSetAl(&registers3, 0x0D);
        VddBusDeliverInterrupt(&bus, 0x10, &registers3);
        memset(&registers3, 0, sizeof registers3);
        VddSetAx(&registers3, 0x4F02);
        VddSetBx(&registers3, 0x0101);
        VddBusDeliverInterrupt(&bus, 0x10, &registers3);
        g_Video.IsDirty = 1;
        VddBusFrame(&bus);
        vesaColour = g_Video.Palette[15];
        CHECK(vesaColour == mode13Colour && (mode13Colour & 0xFFFFFF) == 0xFFFFFF,
              "#325 vesa/4F02: after mode 0Dh, VESA 101h colour 15 is white (the 256-colour DAC), not grey");
        memset(&registers3, 0, sizeof registers3);
        VddSetAh(&registers3, 0x00);
        VddSetAl(&registers3, 0x03);
        VddBusDeliverInterrupt(&bus, 0x10, &registers3);
    }

    printf("\n%d checks, %d failed\n", g_Total, g_Failures);
    return g_Failures ? 1 : 0;
}

/* video_test.c -- off-VM unit battery for the video VDD (vdd_video.c): text mode
 * 3 + graphics mode 13h + the DAC palette, over the shared video aperture. The
 * renderer pixels are checked against the real font glyph; mode 13h presents the
 * aperture directly. No VM.
 */
#include <stdio.h>
#include <string.h>
#include "vdd_video.h"
#include "vga_font.h"

/* #322: the host fills the character tables from the system's fonts at start-up; off-VM
   there are no fonts, so the tables get a deterministic pattern with every glyph lit.
   These checks compare what was DRAWN with what the TABLE says, so any content works. */
/* The pattern keeps what every real font has and these checks rely on: the blank
   characters (00h, 20h, FFh) are blank, and every other glyph has column 2 lit and
   columns 0, 1 and 7 unlit on every row -- so it has both foreground and background
   pixels, at known places -- and is unique (see TEST_ROW). */
static void fill_test_fonts(void)
{
    int c, y;
    for (c = 0; c < 256; ++c) {
        int blank = (c == 0x00 || c == 0x20 || c == 0xFF);
        /* rows 0 and 1 carry the code's two nibbles, so every glyph is UNIQUE -- AH=08h
           reads a character back by matching pixels against the table, as a real
           font allows */
#define TEST_ROW(y, k) (uint8_t)(0x20 | (((y) == 0 ? (c & 0x0F) : (y) == 1 ? (c >> 4) \
                                         : ((c * (k) + (y) * 11) >> 1)) & 0x0F) << 1)
        for (y = 0; y < 16; ++y) vga_font_8x16[c][y] = blank ? 0 : TEST_ROW(y, 37);
        for (y = 0; y < 14; ++y) vga_font_8x14[c][y] = blank ? 0 : TEST_ROW(y, 29);
        for (y = 0; y < 8;  ++y) vga_font_8x8 [c][y] = blank ? 0 : TEST_ROW(y, 23);
#undef TEST_ROW
    }
}

/* True if some VDD claimed `port` -- used instead of asserting a range count. */
static int claims_port(const vdd_bus *b, uint16_t port)
{
    int i;
    for (i = 0; i < b->n_ports; ++i)
        if (port >= b->ports[i].lo && port <= b->ports[i].hi) return 1;
    return 0;
}

static int total = 0, fails = 0;
#define CHECK(c,m) do{ total++; if(c){printf("  PASS  %s\n",(m));} \
    else{printf("  FAIL  %s\n",(m)); fails++;} }while(0)


static uint8_t g_flat[0x100000];          /* guest memory for INT 10h ES:BP/ES:DX */

/* Fake microsecond clock for the 0x3DA retrace timing tests (T17). The VDD takes
   its timebase as a hook so the host can hand it QueryPerformanceCounter and the
   battery can hand it a value it controls -- which makes CRT timing, normally the
   least testable thing in an emulator, an ordinary deterministic assertion. */
uint64_t g_fake_us = 0;
static uint64_t fake_clock(void) { return g_fake_us; }
/* The same trick for the guest's CS:IP. The site instruments all key on it, so
   without a hook the battery exercises the VGA engine and NONE of the tables that
   are used to reason about a guest -- which is how they shipped unverified. */
static uint32_t g_fake_pc = 0;
static uint32_t fake_pc(void) { return g_fake_pc; }
/* Index/data register writes the way a guest does them. */
static void gc_w(vdd_bus *b, uint8_t idx, uint8_t val)
{ uint32_t v = idx; vdd_bus_io(b,0x3CE,1,0,&v); v = val; vdd_bus_io(b,0x3CF,1,0,&v); }
static void sc_w(vdd_bus *b, uint8_t idx, uint8_t val)
{ uint32_t v = idx; vdd_bus_io(b,0x3C4,1,0,&v); v = val; vdd_bus_io(b,0x3C5,1,0,&v); }
/* Write one CRTC register the way a guest does: index to 0x3D4, data to 0x3D5. */
static void crtc_w(vdd_bus *b, uint8_t idx, uint8_t val)
{
    uint32_t v = idx; vdd_bus_io(b, 0x3D4, 1, 0, &v);
    v = val;          vdd_bus_io(b, 0x3D5, 1, 0, &v);
}
static uint8_t g_vmem[VID_APERTURE_SIZE]; /* the video aperture (A0000) stand-in   */
static video_state vid;

static uint8_t *txt(int r,int c){ return g_vmem + VID_TEXT_OFF + (r*vid.cols+c)*2; }
static uint8_t cchar(int r,int c){ return txt(r,c)[0]; }
static uint8_t cattr(int r,int c){ return txt(r,c)[1]; }
static uint32_t dac_pack_ref(uint8_t r,uint8_t g,uint8_t b)
{ return 0xFF000000u | ((uint32_t)(r<<2)<<16) | ((uint32_t)(g<<2)<<8) | (uint32_t)(b<<2); }

int main(void)
{
    fill_test_fonts();
    vdd_bus bus;
    ntvdd dev;
    ntvdd_regs r;
    memset(&vid, 0, sizeof vid);
    vid.vmem = g_vmem;                       /* caller wires the aperture          */
    dev = vdd_video_device(&vid);

    printf("== M3 video battery (text mode 3 + mode 13h) ==\n");

    vdd_bus_init(&bus, g_flat);
    vdd_bus_set_sinks(&bus, 0, 0, 0, 0);

    /* T0: registers + clean mode-3 screen -------------------------------- */
    CHECK(vdd_bus_add(&bus, &dev) == 0, "add: video init ok");
    /* Assert WHAT was claimed, not how many ranges: a bare count silently went
       stale when the OPL detect stub was bolted onto this VDD, and the battery
       reported a failure that had nothing to do with video. */
    CHECK(bus.n_mem == 1 && bus.ints[0x10].svc && bus.n_frame == 1 &&
          claims_port(&bus, 0x3C4) && claims_port(&bus, 0x3C9) &&
          claims_port(&bus, 0x3CE) && claims_port(&bus, 0x3DA),
          "add: B8000 + INT10h + Seq/DAC/GC/Status ports + frame claimed");
    CHECK(vid.mode == 3 && vid.cols == 80 && vid.rows == 25, "reset: mode 3, 80x25");
    CHECK(cchar(0,0) == ' ' && cattr(0,0) == 0x07, "reset: text cleared to spaces/0x07");

    /* T0b: Input Status 1 (3DA) toggles the retrace bit so vsync polls advance */
    { uint32_t s1, s2; vdd_bus_io(&bus, 0x3DA, 1, 1, &s1); vdd_bus_io(&bus, 0x3DA, 1, 1, &s2);
      CHECK(((s1 ^ s2) & 0x08) == 0x08, "3DA: vertical-retrace bit toggles between reads"); }

    /* T1: teletype + cursor --------------------------------------------- */
    memset(&r,0,sizeof r); s_ah(&r,0x02); s_dx(&r,0); vdd_bus_deliver_int(&bus,0x10,&r);
    { const char *s="Hi"; int i; for(i=0;s[i];++i){memset(&r,0,sizeof r);s_ah(&r,0x0E);s_al(&r,(uint8_t)s[i]);vdd_bus_deliver_int(&bus,0x10,&r);} }
    CHECK(cchar(0,0)=='H' && cchar(0,1)=='i' && vid.cur_col==2, "int10/0E: 'Hi' + cursor advance");

    /* T2: write char+attr + scroll + string ----------------------------- */
    memset(&r,0,sizeof r); s_ah(&r,0x02); s_dx(&r,(uint16_t)((5<<8)|0)); vdd_bus_deliver_int(&bus,0x10,&r);
    memset(&r,0,sizeof r); s_ah(&r,0x09); s_al(&r,'X'); s_bx(&r,0x1F); s_cx(&r,3); vdd_bus_deliver_int(&bus,0x10,&r);
    CHECK(cchar(5,0)=='X'&&cattr(5,2)==0x1F, "int10/09: 'XXX' attr 0x1F");
    { uint16_t seg=0x2000,off=0x10; memcpy(&g_flat[(seg<<4)+off],"OK",2);
      memset(&r,0,sizeof r); s_ah(&r,0x13); s_al(&r,0); s_bx(&r,0x4E); s_cx(&r,2);
      s_dx(&r,(uint16_t)((12<<8)|3)); r.es=seg; r.ebp=off; vdd_bus_deliver_int(&bus,0x10,&r);
      CHECK(cchar(12,3)=='O'&&cattr(12,3)==0x4E, "int10/13: string 'OK' attr 0x4E"); }

    /* T3: B8000 hook routes to the aperture text region ------------------ */
    CHECK(vdd_bus_mem_write(&bus, 0xB8000 + (2*80+1)*2, 'Z')==1 && cchar(2,1)=='Z', "mem: B8000 write -> cell");

    /* T4: text render matches the font glyph ---------------------------- */
    memset(&r,0,sizeof r); s_ah(&r,0x02); s_dx(&r,0); vdd_bus_deliver_int(&bus,0x10,&r);
    memset(&r,0,sizeof r); s_ah(&r,0x09); s_al(&r,'A'); s_bx(&r,0x0F); s_cx(&r,1); vdd_bus_deliver_int(&bus,0x10,&r);
    memset(&r,0,sizeof r); s_ah(&r,0x02); s_dx(&r,(uint16_t)((24<<8)|79)); vdd_bus_deliver_int(&bus,0x10,&r);
    vdd_video_render(&vid);
    { int gy,gx,mism=0; const uint8_t *gl=vga_font_8x16['A'];
      for(gy=0;gy<VID_CELL_H;++gy)for(gx=0;gx<VID_CELL_W;++gx){
          uint8_t e=(gl[gy]&(0x80>>gx))?15:0; if(vid.fb[gy*VID_FB_W+gx]!=e)mism++; }
      CHECK(mism==0, "render: text cell matches font glyph 'A'"); }

    /* T4b: A USER-LOADED FONT MUST CHANGE WHAT IS DRAWN.  GH #52 -----------
       INT 10h AH=11h AL=00h loads the caller's own character generator. It used
       to be accepted, marked unimplemented, and IGNORED -- the ROM glyphs were
       drawn anyway, so a program that installed a custom character set got the
       stock font and no error. Silent wrong output, which is the whole point of
       GH #27. The check is deliberately a RENDER, not a "did the call return
       ok": the old code returned ok too. */
    { uint16_t fseg = 0x4000, foff = 0x0000;
      uint8_t *fb2 = &g_flat[(fseg << 4) + foff];
      int gy, gx, solid = 1;
      memset(fb2, 0xFF, 16);                       /* one glyph: every pixel set */
      memset(&r,0,sizeof r);
      s_ah(&r,0x11); s_al(&r,0x00);
      s_bx(&r,(uint16_t)(16 << 8));                /* BH = 16 bytes per char     */
      s_cx(&r,1);                                  /* one character              */
      s_dx(&r,'A');                                /* starting at 'A'            */
      r.es = fseg; r.ebp = foff;
      vdd_bus_deliver_int(&bus,0x10,&r);
      CHECK(vid.user_font_on == 1, "int10/11/00: a user font load is recorded");
      vdd_video_render(&vid);
      for(gy=0;gy<VID_CELL_H;++gy)for(gx=0;gx<VID_CELL_W;++gx)
          if(vid.fb[gy*VID_FB_W+gx] != 15) solid = 0;
      CHECK(solid, "int10/11/00: the USER glyph is drawn, not the ROM one");

      /* A character the caller did NOT supply must still draw as itself --
         the table is seeded from ROM, so loading one glyph cannot blank the
         other 255. */
      memset(&r,0,sizeof r); s_ah(&r,0x02); s_dx(&r,(uint16_t)((0<<8)|1));
      vdd_bus_deliver_int(&bus,0x10,&r);
      memset(&r,0,sizeof r); s_ah(&r,0x09); s_al(&r,'B'); s_bx(&r,0x0F); s_cx(&r,1);
      vdd_bus_deliver_int(&bus,0x10,&r);
      /* Park the cursor off in the corner FIRST. vdd_video_render draws the text
         cursor over the cell it sits on, so leaving it here compares a glyph
         against a glyph-plus-cursor -- which is what made this check fail on its
         first run, in the TEST and not in the code. T4 above moves it to
         (24,79) for exactly the same reason. */
      memset(&r,0,sizeof r); s_ah(&r,0x02); s_dx(&r,(uint16_t)((24<<8)|79));
      vdd_bus_deliver_int(&bus,0x10,&r);
      vdd_video_render(&vid);
      CHECK(cchar(0,1)=='B', "int10/09: 'B' landed at row 0 col 1");
      { int mism2=0; const uint8_t *gl2=vga_font_8x16['B'];
        for(gy=0;gy<VID_CELL_H;++gy)for(gx=0;gx<VID_CELL_W;++gx){
            uint8_t e=(gl2[gy]&(0x80>>gx))?15:0;
            if(vid.fb[gy*VID_FB_W + VID_CELL_W + gx]!=e) mism2++; }
        CHECK(mism2==0, "int10/11/00: unsupplied chars keep their ROM glyphs"); }

      /* AL=02h selects a ROM font, which is a request to go BACK -- it must
         clear the override rather than leave a stale user font installed. */
      memset(&r,0,sizeof r); s_ah(&r,0x11); s_al(&r,0x02); s_bx(&r,0);
      vdd_bus_deliver_int(&bus,0x10,&r);
      CHECK(vid.user_font_on == 0, "int10/11/02: a ROM-font select clears the override");
      vdd_video_render(&vid);
      { int mism3=0; const uint8_t *gl3=vga_font_8x16['A'];
        for(gy=0;gy<VID_CELL_H;++gy)for(gx=0;gx<VID_CELL_W;++gx){
            uint8_t e=(gl3[gy]&(0x80>>gx))?15:0;
            if(vid.fb[gy*VID_FB_W+gx]!=e) mism3++; }
        CHECK(mism3==0, "int10/11/02: ...and 'A' is the ROM glyph again"); }

      /* A cell is VID_CELL_H tall, so a font taller than that cannot be drawn.
         REFUSE it and mark the function unimplemented rather than store rows
         the renderer would silently truncate. */
      memset(&r,0,sizeof r);
      s_ah(&r,0x11); s_al(&r,0x00); s_bx(&r,(uint16_t)(32 << 8));
      s_cx(&r,1); s_dx(&r,'A'); r.es = fseg; r.ebp = foff;
      vdd_bus_deliver_int(&bus,0x10,&r);
      CHECK(vid.user_font_on == 0, "int10/11/00: a font taller than the cell is REFUSED");
    }

    /* T5: text frame is 640x400x8 --------------------------------------- */
    vid.dirty=1; vdd_bus_frame(&bus);
    CHECK(vid.frame.w==640 && vid.frame.h==400 && vid.frame.bpp==8,
          "frame(text): 640x400x8 palettised");

    /* T6: DAC ports set a palette entry --------------------------------- */
    { uint32_t v; v=0x10; vdd_bus_io(&bus,0x3C8,1,0,&v);     /* write index 0x10  */
      v=0x3F; vdd_bus_io(&bus,0x3C9,1,0,&v);                 /* R=63              */
      v=0x00; vdd_bus_io(&bus,0x3C9,1,0,&v);                 /* G=0               */
      v=0x15; vdd_bus_io(&bus,0x3C9,1,0,&v);                 /* B=21              */
      CHECK(vid.pal[0x10]==(0xFF000000u|(0x3F<<2)<<16|(0x15<<2)), "DAC: 3C8/3C9 set pal[0x10]"); }

    /* T7: INT 10h AH=10/AL=10 sets one DAC reg -------------------------- */
    memset(&r,0,sizeof r); s_ah(&r,0x10); s_al(&r,0x10); s_bx(&r,0x20);
    s_dx(&r,(uint16_t)(0x20<<8)); s_cx(&r,(uint16_t)((0x10<<8)|0x08));  /* R=0x20 G=0x10 B=0x08 */
    vdd_bus_deliver_int(&bus,0x10,&r);
    CHECK(vid.pal[0x20]==dac_pack_ref(0x20,0x10,0x08), "int10/10/10: set DAC reg 0x20");

    /* T8: mode 13h -- set mode, write a pixel, present the aperture ------ */
    memset(&r,0,sizeof r); s_ah(&r,0x00); s_al(&r,0x13); vdd_bus_deliver_int(&bus,0x10,&r);
    CHECK(vid.mode==0x13, "int10/00: mode set to 13h");
    CHECK(g_vmem[0]==0 && g_vmem[63999]==0, "mode13: A0000 cleared");
    g_vmem[100*VID_G13_W + 50] = 0x10;        /* direct framebuffer write           */
    /* INT 10h AH=0C write pixel */
    memset(&r,0,sizeof r); s_ah(&r,0x0C); s_al(&r,0x20); s_cx(&r,10); s_dx(&r,20);
    vdd_bus_deliver_int(&bus,0x10,&r);
    CHECK(g_vmem[20*VID_G13_W + 10]==0x20, "int10/0C: write pixel (10,20)=0x20");
    vid.dirty=1; vdd_bus_frame(&bus);
    CHECK(vid.frame.w==320 && vid.frame.h==200 && vid.frame.bpp==8
          && vid.frame.pixels==g_vmem, "frame(mode13): 320x200x8 from the aperture");

    /* T9: VESA 4F00 controller info ------------------------------------- */
    /* ⚠ THE INFO BLOCK IS 256 BYTES UNLESS THE CALLER PRESET "VBE2". (s74) Heretic
       allocates exactly 256 bytes of DOS memory for it, and we used to write the OEM
       string at +0x100 -- over the MCB of the next block, which broke the chain and
       killed it at I_AllocLow. So: poison 256..511, and check nothing lands there. */
    { uint16_t seg=0x3000, off=0x0000; uint8_t *b=&g_flat[(seg<<4)+off]; uint32_t mlp, oem; int i, clean=1;
      memset(b, 0xAA, 512);
      memset(&r,0,sizeof r); s_ah(&r,0x4F); s_al(&r,0x00); r.es=seg; r.edi=off;
      vdd_bus_deliver_int(&bus,0x10,&r);
      CHECK(r_ax(&r)==0x004F && b[0]=='V'&&b[1]=='E'&&b[2]=='S'&&b[3]=='A', "vesa/4F00: 'VESA' signature");
      mlp = b[14]|(b[15]<<8);                 /* mode-list offset (low word of far ptr) */
      oem = b[6]|(b[7]<<8);                   /* OEM-string offset                      */
      CHECK((b[(mlp&0xFFFF)]|(b[(mlp&0xFFFF)+1]<<8))==0x100, "vesa/4F00: mode list starts 0x100");
      CHECK((b[16]|(b[17]<<8))==seg && (b[8]|(b[9]<<8))==seg, "vesa/4F00: pointers are in the caller's segment");
      CHECK(mlp>=34 && mlp<256 && oem>=34 && oem<256, "vesa/4F00: mode list and OEM string inside the 256-byte block");
      for (i=256;i<512;++i) if (b[i]!=0xAA) clean=0;
      CHECK(clean, "vesa/4F00: nothing written past 256 bytes without 'VBE2'");
      /* With "VBE2" preset the block is 512 bytes and may be used in full. */
      memset(b, 0xAA, 512); b[0]='V'; b[1]='B'; b[2]='E'; b[3]='2';
      memset(&r,0,sizeof r); s_ah(&r,0x4F); s_al(&r,0x00); r.es=seg; r.edi=off;
      vdd_bus_deliver_int(&bus,0x10,&r);
      CHECK(r_ax(&r)==0x004F && b[0]=='V'&&b[1]=='E'&&b[2]=='S'&&b[3]=='A' && (b[4]|(b[5]<<8))==0x0200,
            "vesa/4F00 (VBE2): signature rewritten, version 2.0");
      CHECK(b[511]==0, "vesa/4F00 (VBE2): 512-byte block initialised");
      /* §4.3: with 'VBE2' the OEM string -- and the vendor, product and revision strings --
         are copied into OemData (+100h). They all pointed at one string at +22h. #226 */
      { unsigned po[4] = { 6, 22, 26, 30 }, k, inside = 1, distinct = 1;
        for (k = 0; k < 4; ++k) {
          unsigned o = b[po[k]] | (b[po[k]+1] << 8), sg = b[po[k]+2] | (b[po[k]+3] << 8);
          if (sg != seg || o < 0x100 || o >= 0x200) inside = 0;
          if (k && o == (unsigned)(b[po[k-1]] | (b[po[k-1]+1] << 8))) distinct = 0;
        }
        CHECK(inside && distinct && memcmp(&b[b[6]|(b[7]<<8)], "NTVDMEX VESA", 13)==0
              && memcmp(&b[b[22]|(b[23]<<8)], "NTVDMEX", 8)==0,
              "vesa/4F00 (VBE2): OEM/vendor/product/rev strings are four strings in OemData (+100h)"); } }

    /* T10: VESA 4F01 mode info for 0x101 (640x480x8) -------------------- */
    { uint16_t seg=0x3100; uint8_t *b=&g_flat[(seg<<4)];
      memset(&r,0,sizeof r); s_ah(&r,0x4F); s_al(&r,0x01); s_cx(&r,0x101); r.es=seg; r.edi=0;
      vdd_bus_deliver_int(&bus,0x10,&r);
      CHECK(r_ax(&r)==0x004F && (b[18]|(b[19]<<8))==640 && (b[20]|(b[21]<<8))==480 && b[25]==8,
            "vesa/4F01: 0x101 = 640x480x8");
      CHECK(b[29]==(VID_VESA_VRAM/(640u*480u))-1 && b[30]==1, "vesa/4F01: NumberOfImagePages = pages-1 (was 0, oracle row), Reserved=1");
      memset(&r,0,sizeof r); s_ah(&r,0x4F); s_al(&r,0x00); r.es=seg; r.edi=0; memset(b,0,512); vdd_bus_deliver_int(&bus,0x10,&r);
      CHECK((b[10]|(b[11]<<8)|(b[12]<<16)|(b[13]<<24))==1, "vesa/4F00: Capabilities D0 = DAC switchable (we honour 4F08 BH=8)"); }

    /* T11: VESA 4F02 set mode + 4F05 banking round-trips through vram ---- */
    memset(&r,0,sizeof r); s_ah(&r,0x4F); s_al(&r,0x02); s_bx(&r,0x101); vdd_bus_deliver_int(&bus,0x10,&r);
    CHECK(r_ax(&r)==0x004F && vid.in_vesa && vid.vesa_w==640 && vid.vesa_h==480, "vesa/4F02: set 0x101");
    g_vmem[10] = 0xAB;                          /* write into bank 0 window           */
    memset(&r,0,sizeof r); s_ah(&r,0x4F); s_al(&r,0x05); s_bx(&r,0); s_dx(&r,1); /* -> bank 1 */
    vdd_bus_deliver_int(&bus,0x10,&r);
    CHECK(vid.vesa_bank==1, "vesa/4F05: switched to bank 1");
    g_vmem[10] = 0xCD;                          /* write into bank 1 window           */
    memset(&r,0,sizeof r); s_ah(&r,0x4F); s_al(&r,0x05); s_bx(&r,0); s_dx(&r,0); /* back to 0 */
    vdd_bus_deliver_int(&bus,0x10,&r);
    CHECK(g_vmem[10]==0xAB, "vesa/4F05: bank 0 window restored from vram");
    CHECK(vid.vesa_vram[1*VID_VESA_WIN + 10]==0xCD, "vesa/4F05: bank 1 byte kept in vram");
    /* §4.8: BH selects set(00)/get(01), BL is the WINDOW (A=0, B=1). The code read BL as
       the selector, so a "get window A" (BH=01,BL=00,DX=junk) was a SET to junk. */
    memset(&r,0,sizeof r); s_ah(&r,0x4F); s_al(&r,0x05); s_bx(&r,0x0100); s_dx(&r,0x1234); vdd_bus_deliver_int(&bus,0x10,&r);
    CHECK(r_ax(&r)==0x004F && r_dx(&r)==0 && vid.vesa_bank==0, "vesa/4F05 get (BH=01): DX=bank 0, bank NOT changed by DX in");
    memset(&r,0,sizeof r); s_ah(&r,0x4F); s_al(&r,0x05); s_bx(&r,0x0001); s_dx(&r,1); vdd_bus_deliver_int(&bus,0x10,&r);
    CHECK(r_ax(&r)!=0x004F && vid.vesa_bank==0, "vesa/4F05 window B (BL=01): fails, we advertise none");
    memset(&r,0,sizeof r); s_ah(&r,0x4F); s_al(&r,0x05); s_bx(&r,0x0000); s_dx(&r,(uint16_t)(VID_VESA_VRAM/VID_VESA_WIN)); vdd_bus_deliver_int(&bus,0x10,&r);
    CHECK(r_ax(&r)==0x024F && vid.vesa_bank==0, "vesa/4F05 set past memory: AH=02, bank kept");
    memset(&r,0,sizeof r); s_ah(&r,0x4F); s_al(&r,0x02); s_bx(&r,0x4101); vdd_bus_deliver_int(&bus,0x10,&r);   /* LFB */
    memset(&r,0,sizeof r); s_ah(&r,0x4F); s_al(&r,0x05); s_bx(&r,0x0000); s_dx(&r,1); vdd_bus_deliver_int(&bus,0x10,&r);
    CHECK(r_ax(&r)==0x034F, "vesa/4F05 in an LFB mode: AH=03 (invalid in current mode)");
    memset(&r,0,sizeof r); s_ah(&r,0x4F); s_al(&r,0x02); s_bx(&r,0x0101); vdd_bus_deliver_int(&bus,0x10,&r);   /* banked again */

    /* T11b: 4F08 DAC width + 4F09 palette, VBE 2.0 §4.11/§4.12 ------------------- */
    { uint16_t seg=0x3200; uint8_t *t=&g_flat[(seg<<4)];
      memset(&r,0,sizeof r); s_ah(&r,0x4F); s_al(&r,0x08); s_bx(&r,0x0001); vdd_bus_deliver_int(&bus,0x10,&r);
      CHECK(r_ax(&r)==0x004F && (r_bx(&r)>>8)==6, "vesa/4F08 get after a mode set: 6 bits");
      memset(&r,0,sizeof r); s_ah(&r,0x4F); s_al(&r,0x08); s_bx(&r,0x0A00); vdd_bus_deliver_int(&bus,0x10,&r);
      CHECK(r_ax(&r)==0x004F && (r_bx(&r)>>8)==8, "vesa/4F08 set 10 bits: next lower we have = 8");
      /* 8-bit palette entry 1 = pure blue, through 4F09; the presenter palette must follow */
      t[0]=0xFF; t[1]=0; t[2]=0; t[3]=0;
      memset(&r,0,sizeof r); s_ah(&r,0x4F); s_al(&r,0x09); s_bx(&r,0x0000); s_cx(&r,1); s_dx(&r,1); r.es=seg; r.edi=0; vdd_bus_deliver_int(&bus,0x10,&r);
      CHECK(r_ax(&r)==0x004F && vid.dac[1]==0xFF0000FFu && vid.pal[1]==0xFF0000FFu, "vesa/4F09 set (8-bit): dac[1] blue AND pal[1] refreshed");
      memset(&r,0,sizeof r); s_ah(&r,0x4F); s_al(&r,0x08); s_bx(&r,0x0700); vdd_bus_deliver_int(&bus,0x10,&r);
      CHECK((r_bx(&r)>>8)==6, "vesa/4F08 set 7 bits: next lower = 6");
      /* 6-bit: index 6 -- one the EGA attribute mapping would send to DAC 0x14 -- must be identity in a VESA 8bpp mode */
      t[0]=0x3F; t[1]=0; t[2]=0; t[3]=0;
      memset(&r,0,sizeof r); s_ah(&r,0x4F); s_al(&r,0x09); s_bx(&r,0x0000); s_cx(&r,1); s_dx(&r,6); r.es=seg; r.edi=0; vdd_bus_deliver_int(&bus,0x10,&r);
      CHECK(vid.dac[6]==0xFF0000FCu && vid.pal[6]==0xFF0000FCu, "vesa/4F09 set (6-bit) index 6: pal[6] is the DAC entry, not the EGA remap");
      t[0]=t[1]=t[2]=t[3]=0xEE;
      memset(&r,0,sizeof r); s_ah(&r,0x4F); s_al(&r,0x09); s_bx(&r,0x0001); s_cx(&r,1); s_dx(&r,6); r.es=seg; r.edi=0; vdd_bus_deliver_int(&bus,0x10,&r);
      CHECK(r_ax(&r)==0x004F && t[0]==0x3F && t[1]==0 && t[2]==0 && t[3]==0, "vesa/4F09 get (6-bit): B,G,R,0 = 3F,0,0,0");
      memset(&r,0,sizeof r); s_ah(&r,0x4F); s_al(&r,0x09); s_bx(&r,0x0002); s_cx(&r,1); s_dx(&r,0); r.es=seg; r.edi=0; vdd_bus_deliver_int(&bus,0x10,&r);
      CHECK(r_ax(&r)==0x024F, "vesa/4F09 secondary palette (BL=02): AH=02, none here");
      memset(&r,0,sizeof r); s_ah(&r,0x4F); s_al(&r,0x09); s_bx(&r,0x0000); s_cx(&r,10); s_dx(&r,250); r.es=seg; r.edi=0; vdd_bus_deliver_int(&bus,0x10,&r);
      CHECK(r_ax(&r)==0x024F, "vesa/4F09 DX+CX past 256: AH=02");
      memset(&r,0,sizeof r); s_ah(&r,0x4F); s_al(&r,0x08); s_bx(&r,0x0800); vdd_bus_deliver_int(&bus,0x10,&r);
      memset(&r,0,sizeof r); s_ah(&r,0x4F); s_al(&r,0x02); s_bx(&r,0x0101); vdd_bus_deliver_int(&bus,0x10,&r);
      memset(&r,0,sizeof r); s_ah(&r,0x4F); s_al(&r,0x08); s_bx(&r,0x0001); vdd_bus_deliver_int(&bus,0x10,&r);
      CHECK((r_bx(&r)>>8)==6, "vesa/4F08: a mode set resets the DAC to 6 bits");
      memset(&r,0,sizeof r); s_ah(&r,0x4F); s_al(&r,0x02); s_bx(&r,0x0111); vdd_bus_deliver_int(&bus,0x10,&r);
      memset(&r,0,sizeof r); s_ah(&r,0x4F); s_al(&r,0x08); s_bx(&r,0x0001); vdd_bus_deliver_int(&bus,0x10,&r);
      CHECK(r_ax(&r)==0x034F, "vesa/4F08 in a direct-colour mode: AH=03");
      memset(&r,0,sizeof r); s_ah(&r,0x4F); s_al(&r,0x02); s_bx(&r,0x0101); vdd_bus_deliver_int(&bus,0x10,&r); }

    /* T11c: 4F04 save/restore state (§4.7) and the AH=1Ch it is a superset of.
       AH=1Ch reported 3 blocks (192 bytes) and then wrote 768 bytes of DAC into the
       caller's buffer -- the Heretic MCB overrun, in another function. The size we
       report must be at least what we write, for both entry points. */
    { uint16_t seg=0x3300; uint8_t *sb=&g_flat[(seg<<4)]; uint16_t blocks, blocks1c; unsigned k, spill=0;
      memset(&r,0,sizeof r); s_ah(&r,0x4F); s_al(&r,0x04); s_dx(&r,0x0000); s_cx(&r,0x000F); vdd_bus_deliver_int(&bus,0x10,&r);
      blocks = r_bx(&r);
      CHECK(r_ax(&r)==0x004F && blocks>=12, "vesa/4F04 DL=00: reports a size that can hold a 768-byte DAC");
      memset(&r,0,sizeof r); s_ah(&r,0x1C); s_al(&r,0x00); s_cx(&r,0x0007); vdd_bus_deliver_int(&bus,0x10,&r);
      blocks1c = r_bx(&r);
      CHECK(r_al(&r)==0x1C && blocks1c>=12, "int10/1C AL=00: size >= 12 blocks (was 3, then wrote 768 bytes)");
      /* arrange a state: 8-bit DAC, entry 7 = (R=0x12,G=0x34,B=0x56), page 2 at stride 1024 */
      memset(&r,0,sizeof r); s_ah(&r,0x4F); s_al(&r,0x08); s_bx(&r,0x0800); vdd_bus_deliver_int(&bus,0x10,&r);
      { uint8_t *t=&g_flat[(0x3200<<4)]; t[0]=0x56; t[1]=0x34; t[2]=0x12; t[3]=0;
        memset(&r,0,sizeof r); s_ah(&r,0x4F); s_al(&r,0x09); s_bx(&r,0x0000); s_cx(&r,1); s_dx(&r,7); r.es=0x3200; r.edi=0; vdd_bus_deliver_int(&bus,0x10,&r); }
      memset(&r,0,sizeof r); s_ah(&r,0x4F); s_al(&r,0x06); s_bx(&r,0x00); s_cx(&r,1024); vdd_bus_deliver_int(&bus,0x10,&r);
      memset(&r,0,sizeof r); s_ah(&r,0x4F); s_al(&r,0x07); s_bx(&r,0x00); s_cx(&r,0); s_dx(&r,480); vdd_bus_deliver_int(&bus,0x10,&r);
      /* save; the bytes past the reported size must be untouched */
      memset(sb, 0xA5, 4096);
      memset(&r,0,sizeof r); s_ah(&r,0x4F); s_al(&r,0x04); s_dx(&r,0x0001); s_cx(&r,0x000F); r.es=seg; r.ebx=0; vdd_bus_deliver_int(&bus,0x10,&r);
      for (k = blocks*64u; k < 4096; ++k) if (sb[k] != 0xA5) ++spill;
      CHECK(r_ax(&r)==0x004F && spill==0, "vesa/4F04 DL=01 save: nothing written past the reported size");
      /* disturb everything, then restore */
      memset(&r,0,sizeof r); s_ah(&r,0x00); s_al(&r,0x03); vdd_bus_deliver_int(&bus,0x10,&r);   /* text mode: leaves VESA */
      vid.dac[7] = 0xFF000000u;
      memset(&r,0,sizeof r); s_ah(&r,0x4F); s_al(&r,0x04); s_dx(&r,0x0002); s_cx(&r,0x000F); r.es=seg; r.ebx=0; vdd_bus_deliver_int(&bus,0x10,&r);
      CHECK(r_ax(&r)==0x004F && vid.in_vesa && vid.vesa_mode==0x101 && vid.vesa_stride==1024 && vid.vesa_start_y==480
            && vid.vesa_dacwidth==8 && vid.dac[7]==0xFF123456u && vid.pal[7]==0xFF123456u,
            "vesa/4F04 DL=02 restore: VESA mode, pitch, start, DAC width and DAC entry all back");
      memset(&r,0,sizeof r); s_ah(&r,0x4F); s_al(&r,0x04); s_dx(&r,0x0002); s_cx(&r,0x000F); r.es=0x3400; r.ebx=0; vdd_bus_deliver_int(&bus,0x10,&r);
      CHECK(r_ax(&r)==0x024F, "vesa/4F04 restore from a buffer we did not write: AH=02, refused");
      memset(&r,0,sizeof r); s_ah(&r,0x4F); s_al(&r,0x02); s_bx(&r,0x0101); vdd_bus_deliver_int(&bus,0x10,&r); }

    /* T12: VESA frame is vesa_w x vesa_h x8 ----------------------------- */
    vid.dirty=1; vdd_bus_frame(&bus);
    CHECK(vid.frame.w==640 && vid.frame.h==480 && vid.frame.pixels==vid.vesa_vram,
          "frame(vesa): 640x480x8 from vesa_vram");
    /* a banked guest writes into the A0000 window and never calls 4F05 again: the
       present must sync the window into the frame (vesacube, s74b) */
    g_vmem[640*10 + 7] = 0x0C;
    vid.dirty=1; vdd_bus_frame(&bus);
    CHECK(vid.frame.pixels[640*10 + 7]==0x0C, "frame(vesa banked): a window write reaches the presented frame");

    /* T12b: VESA 4F06 logical scan line + 4F07 display start, VBE 2.0 §4.9/4.10.
       Both used to be ACCEPTED AND IGNORED: the stride the presenter used never moved
       and the start it displayed was always (0,0), so a guest that page-flips through
       4F07 -- the standard VESA double-buffer -- showed the wrong page while every call
       returned 004F. Expectations below are the spec's, not the code's. */
    /* BL=01 get: BX bytes/line, CX pixels/line, DX max lines at that length */
    memset(&r,0,sizeof r); s_ah(&r,0x4F); s_al(&r,0x06); s_bx(&r,0x01); vdd_bus_deliver_int(&bus,0x10,&r);
    CHECK(r_ax(&r)==0x004F && r_bx(&r)==640 && r_cx(&r)==640 && r_dx(&r)==VID_VESA_VRAM/640,
          "vesa/4F06 get: 640 bytes, 640 px, VRAM/640 lines");
    /* BL=00 set 1024 pixels -> stride 1024 */
    memset(&r,0,sizeof r); s_ah(&r,0x4F); s_al(&r,0x06); s_bx(&r,0x00); s_cx(&r,1024); vdd_bus_deliver_int(&bus,0x10,&r);
    CHECK(r_ax(&r)==0x004F && r_bx(&r)==1024 && r_cx(&r)==1024 && r_dx(&r)==VID_VESA_VRAM/1024
          && vid.vesa_stride==1024, "vesa/4F06 set 1024 px: stride 1024, DX=VRAM/1024");
    /* BL=03 get maximum: longest line that still holds the mode's 480 rows */
    memset(&r,0,sizeof r); s_ah(&r,0x4F); s_al(&r,0x06); s_bx(&r,0x03); vdd_bus_deliver_int(&bus,0x10,&r);
    CHECK(r_ax(&r)==0x004F && r_bx(&r)==VID_VESA_VRAM/480 && r_cx(&r)==VID_VESA_VRAM/480
          && r_dx(&r)>=480 && vid.vesa_stride==1024, "vesa/4F06 get max: VRAM/480, stride untouched");
    /* too long (65535*480 > VRAM) -> 02h, unchanged; narrower than the mode -> 02h */
    memset(&r,0,sizeof r); s_ah(&r,0x4F); s_al(&r,0x06); s_bx(&r,0x02); s_cx(&r,0xFFFF); vdd_bus_deliver_int(&bus,0x10,&r);
    CHECK(r_ax(&r)==0x024F && vid.vesa_stride==1024, "vesa/4F06 set too long: AH=02, stride kept");
    memset(&r,0,sizeof r); s_ah(&r,0x4F); s_al(&r,0x06); s_bx(&r,0x00); s_cx(&r,320); vdd_bus_deliver_int(&bus,0x10,&r);
    CHECK(r_ax(&r)==0x024F && vid.vesa_stride==1024, "vesa/4F06 set narrower than mode: AH=02, stride kept");
    /* 4F07 set (0,480): page 2 at stride 1024 -> the frame starts 480 rows in */
    memset(&r,0,sizeof r); s_ah(&r,0x4F); s_al(&r,0x07); s_bx(&r,0x00); s_cx(&r,0); s_dx(&r,480); vdd_bus_deliver_int(&bus,0x10,&r);
    vid.vesa_vram[480u*1024u + 5] = 0x77;
    vid.dirty=1; vdd_bus_frame(&bus);
    CHECK(r_ax(&r)==0x004F && vid.frame.stride==1024 && vid.frame.pixels==vid.vesa_vram+480u*1024u
          && vid.frame.pixels[5]==0x77, "vesa/4F07 set (0,480): frame is page 2 at stride 1024");
    memset(&r,0,sizeof r); s_ah(&r,0x4F); s_al(&r,0x07); s_bx(&r,0x01); vdd_bus_deliver_int(&bus,0x10,&r);
    CHECK(r_ax(&r)==0x004F && (r_bx(&r)>>8)==0 && r_cx(&r)==0 && r_dx(&r)==480, "vesa/4F07 get: (0,480), BH=0");
    /* a start that leaves less than a full page -> fail, no change */
    memset(&r,0,sizeof r); s_ah(&r,0x4F); s_al(&r,0x07); s_bx(&r,0x00); s_cx(&r,0); s_dx(&r,(uint16_t)(VID_VESA_VRAM/1024 - 100)); vdd_bus_deliver_int(&bus,0x10,&r);
    CHECK(r_ax(&r)==0x024F && vid.vesa_start_y==480, "vesa/4F07 set past memory: AH=02, start kept");
    /* BL=80h (during retrace) is a set too; x offset moves the origin by bytes-per-pixel */
    memset(&r,0,sizeof r); s_ah(&r,0x4F); s_al(&r,0x07); s_bx(&r,0x80); s_cx(&r,8); s_dx(&r,0); vdd_bus_deliver_int(&bus,0x10,&r);
    vid.dirty=1; vdd_bus_frame(&bus);
    CHECK(r_ax(&r)==0x004F && vid.frame.pixels==vid.vesa_vram+8, "vesa/4F07 BL=80 set (8,0): origin +8 bytes");
    /* a mode set resets both */
    memset(&r,0,sizeof r); s_ah(&r,0x4F); s_al(&r,0x02); s_bx(&r,0x101); vdd_bus_deliver_int(&bus,0x10,&r);
    CHECK(vid.vesa_stride==640 && vid.vesa_start_x==0 && vid.vesa_start_y==0, "vesa/4F02: resets stride and display start");
    /* direct colour: 0x111 (640x480x16), flip to row 480 and read a white pixel back as ARGB */
    memset(&r,0,sizeof r); s_ah(&r,0x4F); s_al(&r,0x02); s_bx(&r,0x111); vdd_bus_deliver_int(&bus,0x10,&r);
    memset(&r,0,sizeof r); s_ah(&r,0x4F); s_al(&r,0x07); s_bx(&r,0x00); s_cx(&r,0); s_dx(&r,480); vdd_bus_deliver_int(&bus,0x10,&r);
    vid.vesa_vram[480u*1280u + 0] = 0xFF; vid.vesa_vram[480u*1280u + 1] = 0xFF;
    vid.dirty=1; vdd_bus_frame(&bus);
    CHECK(r_ax(&r)==0x004F && vid.frame.bpp==32
          && ((const uint32_t *)(const void *)vid.frame.pixels)[0]==0xFFFFFFFFu,
          "vesa/4F07 (16bpp): page 2 pixel 0 = white after the flip");
    memset(&r,0,sizeof r); s_ah(&r,0x4F); s_al(&r,0x02); s_bx(&r,0x101); vdd_bus_deliver_int(&bus,0x10,&r);

    /* T12c: 1024x768 -- the list, the presenter cap and VRAM are sized from one number */
    { uint16_t seg=0x3100; uint8_t *b=&g_flat[(seg<<4)];
      memset(&r,0,sizeof r); s_ah(&r,0x4F); s_al(&r,0x01); s_cx(&r,0x105); r.es=seg; r.edi=0; vdd_bus_deliver_int(&bus,0x10,&r);
      CHECK(r_ax(&r)==0x004F && (b[18]|(b[19]<<8))==1024 && (b[20]|(b[21]<<8))==768 && b[25]==8 && (b[16]|(b[17]<<8))==1024,
            "vesa/4F01: 0x105 = 1024x768x8, pitch 1024");
      memset(&r,0,sizeof r); s_ah(&r,0x4F); s_al(&r,0x02); s_bx(&r,0x105); vdd_bus_deliver_int(&bus,0x10,&r);
      vid.dirty=1; vdd_bus_frame(&bus);
      CHECK(r_ax(&r)==0x004F && vid.frame.w==1024 && vid.frame.h==768 && vid.frame.bpp==8, "vesa/4F02 0x105: frame 1024x768x8");
      memset(&r,0,sizeof r); s_ah(&r,0x4F); s_al(&r,0x02); s_bx(&r,0x4118); vdd_bus_deliver_int(&bus,0x10,&r);
      vid.vesa_vram[(767u*1024u+1023u)*3+0]=0xFF; vid.vesa_vram[(767u*1024u+1023u)*3+1]=0xFF; vid.vesa_vram[(767u*1024u+1023u)*3+2]=0xFF;
      vid.dirty=1; vdd_bus_frame(&bus);
      CHECK(r_ax(&r)==0x004F && vid.frame.w==1024 && vid.frame.h==768 && vid.frame.bpp==32
            && ((const uint32_t *)(const void *)vid.frame.pixels)[767u*1024u+1023u]==0xFFFFFFFFu,
            "vesa/4F02 0x4118: 1024x768x24 LFB, last pixel reaches the ARGB frame");
      CHECK(NTVDD_FRAME_MAXW>=1024 && NTVDD_FRAME_MAXH>=768 && VID_VESA_VRAM>=1024u*768u*3u, "sizes: presenter cap and VRAM hold 1024x768x24");
      memset(&r,0,sizeof r); s_ah(&r,0x4F); s_al(&r,0x02); s_bx(&r,0x0101); vdd_bus_deliver_int(&bus,0x10,&r); }

    /* T12e: VESA text modes 0x108..0x10C (132 columns) and 1280x1024 ------------ */
    { uint16_t seg=0x3100; uint8_t *b=&g_flat[(seg<<4)]; static uint8_t tb[0x100]; vid.bda = tb;
      memset(&r,0,sizeof r); s_ah(&r,0x4F); s_al(&r,0x01); s_cx(&r,0x109); r.es=seg; r.edi=0; vdd_bus_deliver_int(&bus,0x10,&r);
      CHECK(r_ax(&r)==0x004F && !((b[0]|(b[1]<<8)) & 0x10) && (b[18]|(b[19]<<8))==132 && (b[20]|(b[21]<<8))==25
            && b[22]==8 && b[23]==16 && (b[16]|(b[17]<<8))==264 && b[27]==0 && (b[8]|(b[9]<<8))==0xB800,
            "vesa/4F01 0x109: text attrs, 132x25 chars, 8x16 cell, 264 bytes/line, model 0, window B800");
      memset(&r,0,sizeof r); s_ah(&r,0x4F); s_al(&r,0x02); s_bx(&r,0x109); vdd_bus_deliver_int(&bus,0x10,&r);
      CHECK(r_ax(&r)==0x004F && vid.mkind==VID_KIND_TEXT && vid.cols==132 && vid.rows==25 && vid.cell_h==16 && !vid.in_vesa,
            "vesa/4F02 0x109: text kind, 132x25, not a graphics VESA state");
      memset(&r,0,sizeof r); s_ah(&r,0x4F); s_al(&r,0x03); vdd_bus_deliver_int(&bus,0x10,&r);
      CHECK(r_ax(&r)==0x004F && r_bx(&r)==0x109, "vesa/4F03 in a VESA text mode: 0x109");
      memset(&r,0,sizeof r); s_ah(&r,0x0F); vdd_bus_deliver_int(&bus,0x10,&r);
      CHECK((r_ax(&r)>>8)==132 && tb[0x4A]==132 && tb[0x84]==24, "int10/0F + BDA: 132 columns, 25 rows");
      memset(&r,0,sizeof r); s_ah(&r,0x02); s_dx(&r,(uint16_t)((3<<8)|100)); vdd_bus_deliver_int(&bus,0x10,&r);
      memset(&r,0,sizeof r); s_ah(&r,0x0E); s_al(&r,'Z'); vdd_bus_deliver_int(&bus,0x10,&r);
      CHECK(vid.vmem[VID_TEXT_OFF + (3*132+100)*2]=='Z', "text at (3,100): cell addressing uses 132 columns");
      vid.dirty=1; vdd_bus_frame(&bus);
      CHECK(vid.frame.w==1056 && vid.frame.h==400 && vid.frame.bpp==8, "frame(0x109): 1056x400");
      memset(&r,0,sizeof r); s_ah(&r,0x4F); s_al(&r,0x02); s_bx(&r,0x10C); vdd_bus_deliver_int(&bus,0x10,&r);
      vid.dirty=1; vdd_bus_frame(&bus);
      CHECK(r_ax(&r)==0x004F && vid.cols==132 && vid.rows==60 && vid.cell_h==8 && vid.frame.w==1056 && vid.frame.h==480,
            "vesa/4F02 0x10C: 132x60 at 8x8 -> 1056x480");
      memset(&r,0,sizeof r); s_ah(&r,0x4F); s_al(&r,0x02); s_bx(&r,0x108); vdd_bus_deliver_int(&bus,0x10,&r);
      CHECK(r_ax(&r)==0x004F && vid.cols==80 && vid.rows==60, "vesa/4F02 0x108: 80x60");
      memset(&r,0,sizeof r); s_ah(&r,0x00); s_al(&r,0x03); vdd_bus_deliver_int(&bus,0x10,&r);
      memset(&r,0,sizeof r); s_ah(&r,0x4F); s_al(&r,0x03); vdd_bus_deliver_int(&bus,0x10,&r);
      CHECK(vid.cols==80 && vid.rows==25 && r_bx(&r)==0x03, "int10/00 mode 3 leaves the VESA text mode; 4F03 = 3");
      memset(&r,0,sizeof r); s_ah(&r,0x4F); s_al(&r,0x01); s_cx(&r,0x107); r.es=seg; r.edi=0; vdd_bus_deliver_int(&bus,0x10,&r);
      CHECK(r_ax(&r)==0x004F && (b[18]|(b[19]<<8))==1280 && (b[20]|(b[21]<<8))==1024 && b[25]==8, "vesa/4F01: 0x107 = 1280x1024x8");
      memset(&r,0,sizeof r); s_ah(&r,0x4F); s_al(&r,0x02); s_bx(&r,0x411B); vdd_bus_deliver_int(&bus,0x10,&r);
      vid.dirty=1; vdd_bus_frame(&bus);
      CHECK(r_ax(&r)==0x004F && vid.frame.w==1280 && vid.frame.h==1024 && vid.frame.bpp==32, "vesa/4F02 0x411B: 1280x1024x24 LFB frame");
      vid.bda = 0;
      memset(&r,0,sizeof r); s_ah(&r,0x4F); s_al(&r,0x02); s_bx(&r,0x0101); vdd_bus_deliver_int(&bus,0x10,&r); }

    /* T12f: 4F15 VBE/DDC -- a synthesised EDID 1.3 block ------------------------ */
    { uint16_t seg=0x3500; uint8_t *e=&g_flat[(seg<<4)]; unsigned k, sum=0;
      memset(&r,0,sizeof r); s_ah(&r,0x4F); s_al(&r,0x15); s_bx(&r,0x0000); r.es=0; r.edi=0; vdd_bus_deliver_int(&bus,0x10,&r);
      CHECK(r_ax(&r)==0x004F && (r_bx(&r)&0x03)!=0, "vesa/4F15 BL=00: DDC supported (DDC1 and/or DDC2 bits)");
      memset(e, 0xEE, 256);
      memset(&r,0,sizeof r); s_ah(&r,0x4F); s_al(&r,0x15); s_bx(&r,0x0001); s_dx(&r,0); r.es=seg; r.edi=0; vdd_bus_deliver_int(&bus,0x10,&r);
      for (k = 0; k < 128; ++k) sum += e[k];
      CHECK(r_ax(&r)==0x004F && e[0]==0x00 && e[1]==0xFF && e[6]==0xFF && e[7]==0x00 && (sum & 0xFF)==0
            && e[18]==1 && e[19]>=3 && e[126]==0 && e[128]==0xEE,
            "vesa/4F15 BL=01: EDID header, version 1.3+, checksum 0, no extensions, exactly 128 bytes written");
      memset(&r,0,sizeof r); s_ah(&r,0x4F); s_al(&r,0x15); s_bx(&r,0x0001); s_dx(&r,1); r.es=seg; r.edi=0; vdd_bus_deliver_int(&bus,0x10,&r);
      CHECK(r_ax(&r)!=0x004F, "vesa/4F15 BL=01 block 1: none, fails"); }

    /* T12d: 4F10 VBE/PM (DPMS) ----------------------------------------------- */
    memset(&r,0,sizeof r); s_ah(&r,0x4F); s_al(&r,0x10); s_bx(&r,0x0000); vdd_bus_deliver_int(&bus,0x10,&r);
    CHECK(r_ax(&r)==0x004F && (r_bx(&r)&0xFF)==0x10 && (r_bx(&r)>>8)==0x0F, "vesa/4F10 report: VBE/PM 1.0, all four states");
    memset(&r,0,sizeof r); s_ah(&r,0x4F); s_al(&r,0x10); s_bx(&r,0x0401); vdd_bus_deliver_int(&bus,0x10,&r);
    CHECK(r_ax(&r)==0x004F, "vesa/4F10 set: off");
    memset(&r,0,sizeof r); s_ah(&r,0x4F); s_al(&r,0x10); s_bx(&r,0x0002); vdd_bus_deliver_int(&bus,0x10,&r);
    CHECK(r_ax(&r)==0x004F && (r_bx(&r)>>8)==0x04, "vesa/4F10 get: off");
    memset(&r,0,sizeof r); s_ah(&r,0x4F); s_al(&r,0x10); s_bx(&r,0x0001); vdd_bus_deliver_int(&bus,0x10,&r);
    memset(&r,0,sizeof r); s_ah(&r,0x4F); s_al(&r,0x10); s_bx(&r,0x0002); vdd_bus_deliver_int(&bus,0x10,&r);
    CHECK((r_bx(&r)>>8)==0x00, "vesa/4F10 set on, get: on");

    /* T12g: THE 4F08 DAC WIDTH REACHES THE PORTS (#226, VBE 2.0 §4.11). -----------------
       Capabilities D0 says the DAC switches to 8 bits; 4F08 BH=8 said it had. Port 3C9h
       still stored `v & 3Fh` and read back `>> 2`, so a guest that switched and then
       loaded its palette the usual way lost the top two bits of every primary. Every
       expectation is the RAMDAC's: 8 bits in, 8 bits out; 6 bits = the low six, stored
       as the top six of the register (so a width switch re-interprets, not rescales). */
    { uint32_t v; uint16_t seg=0x3200; uint8_t *t=&g_flat[(seg<<4)];
      memset(&r,0,sizeof r); s_ah(&r,0x4F); s_al(&r,0x02); s_bx(&r,0x0101); vdd_bus_deliver_int(&bus,0x10,&r);
      memset(&r,0,sizeof r); s_ah(&r,0x4F); s_al(&r,0x08); s_bx(&r,0x0800); vdd_bus_deliver_int(&bus,0x10,&r);
      CHECK(r_ax(&r)==0x004F && (r_bx(&r)>>8)==8, "dac8: 4F08 BH=8 in 0x101 -> 8 bits");
      v=0x40; vdd_bus_io(&bus,0x3C8,1,0,&v);
      v=0x80; vdd_bus_io(&bus,0x3C9,1,0,&v); v=0xC0; vdd_bus_io(&bus,0x3C9,1,0,&v); v=0xFF; vdd_bus_io(&bus,0x3C9,1,0,&v);
      CHECK(vid.dac[0x40]==0xFF80C0FFu && vid.pal[0x40]==0xFF80C0FFu,
            "dac8: 3C9h carries all 8 bits (80,C0,FF) -- was masked to 00,00,3F<<2");
      { uint32_t a=0,b=0,c=0; v=0x40; vdd_bus_io(&bus,0x3C7,1,0,&v);
        vdd_bus_io(&bus,0x3C9,1,1,&a); vdd_bus_io(&bus,0x3C9,1,1,&b); vdd_bus_io(&bus,0x3C9,1,1,&c);
        CHECK(a==0x80 && b==0xC0 && c==0xFF, "dac8: 3C9h reads back 8 bits, no >>2"); }
      memset(&r,0,sizeof r); s_ah(&r,0x10); s_al(&r,0x10); s_bx(&r,0x41);
      s_dx(&r,0xFF00); s_cx(&r,0x8001); vdd_bus_deliver_int(&bus,0x10,&r);
      CHECK(vid.dac[0x41]==0xFFFF8001u, "dac8: INT 10h 1010h stores 8-bit primaries (a VGA BIOS just OUTs to 3C9h)");
      memset(&r,0,sizeof r); s_ah(&r,0x10); s_al(&r,0x15); s_bx(&r,0x41); vdd_bus_deliver_int(&bus,0x10,&r);
      CHECK((r_dx(&r)>>8)==0xFF && r_cx(&r)==0x8001, "dac8: INT 10h 1015h reads them back at 8 bits");
      memset(t,0xEE,8);
      memset(&r,0,sizeof r); s_ah(&r,0x10); s_al(&r,0x17); s_bx(&r,0x40); s_cx(&r,2); r.es=seg; s_dx(&r,0); vdd_bus_deliver_int(&bus,0x10,&r);
      CHECK(t[0]==0x80 && t[1]==0xC0 && t[2]==0xFF && t[3]==0xFF && t[4]==0x80 && t[5]==0x01 && t[6]==0xEE,
            "dac8: INT 10h 1017h block read at 8 bits, exactly 2x3 bytes");
      memset(t,0xEE,8);
      memset(&r,0,sizeof r); s_ah(&r,0x4F); s_al(&r,0x09); s_bx(&r,0x0001); s_cx(&r,1); s_dx(&r,0x40); r.es=seg; r.edi=0; vdd_bus_deliver_int(&bus,0x10,&r);
      CHECK(t[0]==0xFF && t[1]==0xC0 && t[2]==0x80 && t[3]==0, "dac8: 4F09 get agrees with the port (B,G,R,0)");
      /* A mode set returns the width to 6 (§4.11) and the SAME register reads as its top six bits. */
      memset(&r,0,sizeof r); s_ah(&r,0x4F); s_al(&r,0x02); s_bx(&r,0x8101); vdd_bus_deliver_int(&bus,0x10,&r);
      memset(&r,0,sizeof r); s_ah(&r,0x4F); s_al(&r,0x08); s_bx(&r,0x0001); vdd_bus_deliver_int(&bus,0x10,&r);
      CHECK((r_bx(&r)>>8)==6, "dac6: 4F02 put the width back to 6");
      vid.dac[0x40] = 0xFF80C0FFu;
      { uint32_t a=0,b=0,c=0; v=0x40; vdd_bus_io(&bus,0x3C7,1,0,&v);
        vdd_bus_io(&bus,0x3C9,1,1,&a); vdd_bus_io(&bus,0x3C9,1,1,&b); vdd_bus_io(&bus,0x3C9,1,1,&c);
        CHECK(a==0x20 && b==0x30 && c==0x3F, "dac6: the port reads the top six bits again (80,C0,FF -> 20,30,3F)"); }
      v=0x42; vdd_bus_io(&bus,0x3C8,1,0,&v);
      v=0xFF; vdd_bus_io(&bus,0x3C9,1,0,&v); v=0x40; vdd_bus_io(&bus,0x3C9,1,0,&v); v=0x3F; vdd_bus_io(&bus,0x3C9,1,0,&v);
      CHECK(vid.dac[0x42]==0xFFFC00FCu, "dac6: 3C9h ignores bits 6-7 at 6 bits, as before (FF->3F, 40->00)");
      /* 4F09's 6-bit set used to shift without masking, spilling bits 6-7 into the next field */
      t[0]=0xFF; t[1]=0xC0; t[2]=0x00; t[3]=0;
      memset(&r,0,sizeof r); s_ah(&r,0x4F); s_al(&r,0x09); s_bx(&r,0x0000); s_cx(&r,1); s_dx(&r,0x43); r.es=seg; r.edi=0; vdd_bus_deliver_int(&bus,0x10,&r);
      CHECK(r_ax(&r)==0x004F && vid.dac[0x43]==0xFF0000FCu, "dac6: 4F09 set masks each primary to 6 bits (no spill into G/R)");
      t[0]=0x01; t[1]=0x02; t[2]=0x03; t[3]=0;
      memset(&r,0,sizeof r); s_ah(&r,0x4F); s_al(&r,0x09); s_bx(&r,0x0080); s_cx(&r,1); s_dx(&r,0x44); r.es=seg; r.edi=0; vdd_bus_deliver_int(&bus,0x10,&r);
      CHECK(r_ax(&r)==0x004F && vid.dac[0x44]==0xFF0C0804u && vid.pal[0x44]==0xFF0C0804u,
            "4F09 BL=80h (set during retrace, blank bit): a set like 00h -- Capabilities D2 = 0");
      /* 4F08 in a standard mode: §4.11 refuses only direct colour/YUV. Mode 13h drives the same DAC. */
      memset(&r,0,sizeof r); s_ah(&r,0x00); s_al(&r,0x13); vdd_bus_deliver_int(&bus,0x10,&r);
      memset(&r,0,sizeof r); s_ah(&r,0x4F); s_al(&r,0x08); s_bx(&r,0x0800); vdd_bus_deliver_int(&bus,0x10,&r);
      CHECK(r_ax(&r)==0x004F && (r_bx(&r)>>8)==8, "dac8: 4F08 works in mode 13h (was 034Fh outside VESA)");
      v=0x05; vdd_bus_io(&bus,0x3C8,1,0,&v);
      v=0x81; vdd_bus_io(&bus,0x3C9,1,0,&v); v=0x82; vdd_bus_io(&bus,0x3C9,1,0,&v); v=0x83; vdd_bus_io(&bus,0x3C9,1,0,&v);
      CHECK(vid.pal[0x05]==0xFF818283u, "dac8: mode 13h pixel 5 renders the 8-bit entry");
      memset(&r,0,sizeof r); s_ah(&r,0x00); s_al(&r,0x13); vdd_bus_deliver_int(&bus,0x10,&r);
      memset(&r,0,sizeof r); s_ah(&r,0x4F); s_al(&r,0x08); s_bx(&r,0x0001); vdd_bus_deliver_int(&bus,0x10,&r);
      CHECK(r_ax(&r)==0x004F && (r_bx(&r)>>8)==6, "dac6: INT 10h AH=00h returns the width to 6");
      memset(&r,0,sizeof r); s_ah(&r,0x4F); s_al(&r,0x02); s_bx(&r,0x0101); vdd_bus_deliver_int(&bus,0x10,&r); }

    /* T12h: 4F07h BL=80h WAITS FOR THE RETRACE, and the start rides the latch (#226). --
       VBE 2.0 §4.10 "Set Display Start during Vertical Retrace". It returned at once and
       paced nothing. A VESA mode also runs on its OWN timing now, not the last VGA
       mode's CRTC: 0x101 is 640x480 at 60 Hz, 525 lines, retrace from line 480 --
       F = 16666 us, retrace start at 480*F/525 = 15237 us into each frame. */
    { const uint64_t F = 16666u, T = 1000u * 16666u, VB = (480u * 16666u) / 525u;
      uint32_t v;
      vid.time_us = fake_clock; vid.latch_t = 0;
      g_fake_us = T + 1000; vid.dirty=1; vdd_bus_frame(&bus);              /* sync the latch */
      CHECK(vdd_video_frame_us(&vid)==16666u, "vesa beam: 640x480 is a 60 Hz frame, whatever the last VGA mode was");
      vdd_bus_io(&bus,0x3DA,1,1,&v);
      CHECK((v & 8)==0, "vesa beam: 3DAh in the picture at +1000us");
      g_fake_us = T + VB + 20; vdd_bus_io(&bus,0x3DA,1,1,&v);
      CHECK((v & 8)!=0, "vesa beam: 3DAh in retrace from line 480 of 525 (+15237us)");
      g_fake_us = T + 2000;
      memset(&r,0,sizeof r); s_ah(&r,0x4F); s_al(&r,0x07); s_bx(&r,0x80); s_cx(&r,0); s_dx(&r,480); vdd_bus_deliver_int(&bus,0x10,&r);
      CHECK(r_ax(&r)==0x004F && vid.int10_wait_until==T+VB && vdd_video_int10_wait_us(&vid)==(uint32_t)(VB-2000u),
            "4F07 BL=80h in the picture: completes at the retrace start (host waits the rest)");
      vid.dirty=1; vdd_bus_frame(&bus);
      CHECK(vid.frame.pixels==vid.vesa_vram, "4F07 BL=80h: the old page is still displayed before the retrace");
      memset(&r,0,sizeof r); s_ah(&r,0x4F); s_al(&r,0x07); s_bx(&r,0x01); vdd_bus_deliver_int(&bus,0x10,&r);
      CHECK(r_cx(&r)==0 && r_dx(&r)==480, "4F07 BL=01h reports the start just set (the register), (0,480)");
      g_fake_us = T + VB + 50;
      CHECK(vdd_video_int10_wait_us(&vid)==0 && vid.int10_wait_until==0, "the wait ends once the beam is in retrace, and clears");
      vid.dirty=1; vdd_bus_frame(&bus);
      CHECK(vid.frame.pixels==vid.vesa_vram, "during the retrace the old picture is still the one up");
      g_fake_us = T + F + 100; vid.dirty=1; vdd_bus_frame(&bus);
      CHECK(vid.frame.pixels==vid.vesa_vram + 480u*640u, "the next picture shows page 2 -- the retrace loaded it");
      /* called INSIDE a retrace nobody has used: returns at once, and THAT retrace takes it */
      g_fake_us = T + F + VB + 30;
      memset(&r,0,sizeof r); s_ah(&r,0x4F); s_al(&r,0x07); s_bx(&r,0x80); s_cx(&r,0); s_dx(&r,0); vdd_bus_deliver_int(&bus,0x10,&r);
      CHECK(r_ax(&r)==0x004F && vid.int10_wait_until==0 && vdd_video_int10_wait_us(&vid)==0,
            "4F07 BL=80h inside a fresh retrace: completes at once");
      g_fake_us = T + 2*F + 100; vid.dirty=1; vdd_bus_frame(&bus);
      CHECK(vid.frame.pixels==vid.vesa_vram, "...and page 1 is on the very next picture");
      /* a SECOND call in the same retrace is paced to the next one: one flip a frame */
      g_fake_us = T + 2*F + VB + 10;
      memset(&r,0,sizeof r); s_ah(&r,0x4F); s_al(&r,0x07); s_bx(&r,0x80); s_cx(&r,0); s_dx(&r,480); vdd_bus_deliver_int(&bus,0x10,&r);
      CHECK(vid.int10_wait_until==0, "first call in this retrace: at once");
      g_fake_us = T + 2*F + VB + 40;
      memset(&r,0,sizeof r); s_ah(&r,0x4F); s_al(&r,0x07); s_bx(&r,0x80); s_cx(&r,0); s_dx(&r,0); vdd_bus_deliver_int(&bus,0x10,&r);
      CHECK(vid.int10_wait_until==T+3*F+VB, "second call in the same retrace: waits for the next one");
      /* BL=00h stays immediate: no wait, the display takes it now */
      g_fake_us = T + 3*F + 1000;
      memset(&r,0,sizeof r); s_ah(&r,0x4F); s_al(&r,0x07); s_bx(&r,0x00); s_cx(&r,0); s_dx(&r,480); vdd_bus_deliver_int(&bus,0x10,&r);
      vid.dirty=1; vdd_bus_frame(&bus);
      CHECK(vid.int10_wait_until==0 && vid.frame.pixels==vid.vesa_vram + 480u*640u, "4F07 BL=00h: no wait, shown at once (unchanged)");
      /* 3.0 BL=02h: schedule a BYTE address, return at once; BL=04h reports the flip */
      memset(&r,0,sizeof r); s_ah(&r,0x4F); s_al(&r,0x07); s_bx(&r,0x02); r.ecx=0; vdd_bus_deliver_int(&bus,0x10,&r);
      CHECK(r_ax(&r)==0x004F && vid.int10_wait_until==0, "4F07 BL=02h (3.0): scheduled, returns at once");
      memset(&r,0,sizeof r); s_ah(&r,0x4F); s_al(&r,0x07); s_bx(&r,0x04); vdd_bus_deliver_int(&bus,0x10,&r);
      CHECK(r_ax(&r)==0x004F && r_cx(&r)==0, "4F07 BL=04h: the flip has not happened in the picture");
      g_fake_us = T + 3*F + VB + 10;
      memset(&r,0,sizeof r); s_ah(&r,0x4F); s_al(&r,0x07); s_bx(&r,0x04); vdd_bus_deliver_int(&bus,0x10,&r);
      CHECK(r_ax(&r)==0x004F && r_cx(&r)!=0, "4F07 BL=04h: ...and has once the retrace loaded it");
      memset(&r,0,sizeof r); s_ah(&r,0x4F); s_al(&r,0x07); s_bx(&r,0x02); r.ecx=640u*100u+8u; vdd_bus_deliver_int(&bus,0x10,&r);
      memset(&r,0,sizeof r); s_ah(&r,0x4F); s_al(&r,0x07); s_bx(&r,0x01); vdd_bus_deliver_int(&bus,0x10,&r);
      CHECK(r_cx(&r)==8 && r_dx(&r)==100, "4F07 BL=02h byte address 640*100+8 reads back as (8,100)");
      memset(&r,0,sizeof r); s_ah(&r,0x4F); s_al(&r,0x07); s_bx(&r,0x02); r.ecx=VID_VESA_VRAM; vdd_bus_deliver_int(&bus,0x10,&r);
      CHECK(r_ax(&r)==0x024F, "4F07 BL=02h past memory: AH=02");
      /* 3.0 BL=82h waits like 80h */
      g_fake_us = T + 4*F + 500;
      memset(&r,0,sizeof r); s_ah(&r,0x4F); s_al(&r,0x07); s_bx(&r,0x82); r.ecx=0; vdd_bus_deliver_int(&bus,0x10,&r);
      CHECK(r_ax(&r)==0x004F && vid.int10_wait_until==T+4*F+VB, "4F07 BL=82h (3.0): waits for the retrace too");
      memset(&r,0,sizeof r); s_ah(&r,0x4F); s_al(&r,0x07); s_bx(&r,0x03); vdd_bus_deliver_int(&bus,0x10,&r);
      CHECK(r_ax(&r)==0x014F, "4F07 BL=03h stereo: 014Fh, no such hardware");
      /* a stale stamp can never park the guest */
      vid.int10_wait_until = g_fake_us + 5000000u;
      CHECK(vdd_video_int10_wait_us(&vid)==0 && vid.int10_wait_until==0, "a wait stamp > 1 s out is dropped, not honoured");
      vid.time_us = 0;
      memset(&r,0,sizeof r); s_ah(&r,0x4F); s_al(&r,0x07); s_bx(&r,0x80); s_cx(&r,0); s_dx(&r,0); vdd_bus_deliver_int(&bus,0x10,&r);
      CHECK(r_ax(&r)==0x004F && vid.int10_wait_until==0 && vdd_video_int10_wait_us(&vid)==0, "no clock: BL=80h never waits");
      memset(&r,0,sizeof r); s_ah(&r,0x4F); s_al(&r,0x02); s_bx(&r,0x0101); vdd_bus_deliver_int(&bus,0x10,&r); }

    /* T12i: 4F03h RETURNS D14/D15, 40:87h BIT 7 RECORDS D15, AND A VESA MODE IS NOT THE
       PREVIOUS VGA MODE WEARING A NEW NUMBER (#226). ----------------------------------
       §4.6: BX D14 linear, D15 memory not cleared; §4.5: 2.0 BIOSes update 40:87h bit 7.
       And after mode 12h, 4F02h left mkind PLANAR and chain-4 off -- the host kept
       interpreting the guest and routing A0000 stores into the planes. The BDA values
       are SeaVGABIOS's vga_set_mode(), read from QEMU's vgabios-stdvga.bin. */
    { static uint8_t tb[0x100]; vid.bda = tb;
      memset(&r,0,sizeof r); s_ah(&r,0x4F); s_al(&r,0x02); s_bx(&r,0xC101); vdd_bus_deliver_int(&bus,0x10,&r);
      memset(&r,0,sizeof r); s_ah(&r,0x4F); s_al(&r,0x03); vdd_bus_deliver_int(&bus,0x10,&r);
      CHECK(r_ax(&r)==0x004F && r_bx(&r)==0xC101, "4F03 after 4F02 C101h: C101h -- D14 and D15 kept (was 0101h)");
      CHECK(tb[0x87]==0xE0, "40:87h bit 7 set by 4F02 D15 (60h -> E0h)");
      { uint16_t saved = r_bx(&r);                 /* the save/restore idiom: 4F03 -> 4F02 */
        memset(&r,0,sizeof r); s_ah(&r,0x4F); s_al(&r,0x02); s_bx(&r,0x0101); vdd_bus_deliver_int(&bus,0x10,&r);
        CHECK(vid.vesa_lfb==0 && tb[0x87]==0x60, "4F02 0101h: banked, 40:87h bit 7 clear");
        memset(&r,0,sizeof r); s_ah(&r,0x4F); s_al(&r,0x02); s_bx(&r,saved); vdd_bus_deliver_int(&bus,0x10,&r);
        CHECK(r_ax(&r)==0x004F && vid.vesa_lfb==1, "re-setting what 4F03 returned comes back LINEAR, not banked"); }
      memset(&r,0,sizeof r); s_ah(&r,0x00); s_al(&r,0x83); vdd_bus_deliver_int(&bus,0x10,&r);
      memset(&r,0,sizeof r); s_ah(&r,0x4F); s_al(&r,0x03); vdd_bus_deliver_int(&bus,0x10,&r);
      CHECK(tb[0x87]==0xE0 && r_bx(&r)==0x8003, "INT 10h AH=00h AL=83h: 40:87h bit 7 set, 4F03 = 8003h");
      memset(&r,0,sizeof r); s_ah(&r,0x00); s_al(&r,0x03); vdd_bus_deliver_int(&bus,0x10,&r);
      CHECK(tb[0x87]==0x60, "INT 10h AH=00h AL=03h: 40:87h back to 60h");
      /* §4.5: D14 on a mode with no linear frame buffer (a text mode) fails, nothing changes */
      memset(&r,0,sizeof r); s_ah(&r,0x4F); s_al(&r,0x02); s_bx(&r,0x4109); vdd_bus_deliver_int(&bus,0x10,&r);
      CHECK(r_ax(&r)==0x014F && vid.cols==80 && vid.vesa_text_mode==0, "4F02 4109h (text + LFB): 014Fh, still mode 3");
      memset(&r,0,sizeof r); s_ah(&r,0x4F); s_al(&r,0x02); s_bx(&r,0x8109); vdd_bus_deliver_int(&bus,0x10,&r);
      memset(&r,0,sizeof r); s_ah(&r,0x4F); s_al(&r,0x03); vdd_bus_deliver_int(&bus,0x10,&r);
      CHECK(r_bx(&r)==0x8109, "4F03 in a VESA text mode set with D15: 8109h");
      /* mode 12h, then a VESA mode: the planar machinery must not survive into it */
      memset(&r,0,sizeof r); s_ah(&r,0x00); s_al(&r,0x12); vdd_bus_deliver_int(&bus,0x10,&r);
      gc_w(&bus, 0x05, 0x02);                      /* write mode 2, as a planar guest leaves it */
      CHECK(vdd_video_planar_active(&vid) && vid.chain4==0, "mode 12h: planar, chain-4 off (the precondition)");
      memset(&r,0,sizeof r); s_ah(&r,0x4F); s_al(&r,0x02); s_bx(&r,0x0101); vdd_bus_deliver_int(&bus,0x10,&r);
      CHECK(!vdd_video_planar_active(&vid) && vid.mkind==VID_KIND_LINEAR8 && vid.chain4==1
            && vid.write_mode==0 && vid.map_mask==0x0F,
            "4F02 after 12h: not planar (host stops interpreting), chained, write mode 0 -- was PLANAR");
      { uint32_t v = 0x04; vdd_bus_io(&bus,0x3C4,1,0,&v); v = 0; vdd_bus_io(&bus,0x3C5,1,1,&v);
        CHECK((v & 0x08)!=0, "4F02: SR4 reads back chain-4 on (mode 13h's register file)"); }
      CHECK(vid.gw==640 && vid.gh==480, "4F02: gw/gh are the VESA mode's extent (were mode 12h's by luck)");
      CHECK(tb[0x49]==0xFF && tb[0x4A]==80 && tb[0x84]==29 && tb[0x85]==16 && tb[0x62]==0,
            "4F02 0101h BDA: 40:49=FFh, 4A=80 cols, 84=29, 85=16 (SeaVGABIOS vga_set_mode)");
      memset(&r,0,sizeof r); s_ah(&r,0x0F); vdd_bus_deliver_int(&bus,0x10,&r);
      CHECK(r_al(&r)==0xFF && (r_ax(&r)>>8)==80, "INT 10h AH=0Fh in a VESA mode: AL=FFh (40:49h), AH=80");
      g_vmem[5] = 0x33; vid.dirty=1; vdd_bus_frame(&bus);
      CHECK(vid.vesa_vram[5]==0x33 && vid.frame.pixels[5]==0x33, "4F02 after 12h: an A0000 store reaches the VESA picture");
      memset(&r,0,sizeof r); s_ah(&r,0x4F); s_al(&r,0x02); s_bx(&r,0x010E); vdd_bus_deliver_int(&bus,0x10,&r);
      CHECK(tb[0x4A]==40 && tb[0x84]==24 && tb[0x85]==8, "4F02 010Eh (320x200): 40 cols, 25 rows of 8x8");
      memset(&r,0,sizeof r); s_ah(&r,0x4F); s_al(&r,0x02); s_bx(&r,0x0107); vdd_bus_deliver_int(&bus,0x10,&r);
      CHECK(tb[0x4A]==160 && tb[0x84]==63, "4F02 0107h (1280x1024): 160 cols, 64 rows");
      memset(&r,0,sizeof r); s_ah(&r,0x00); s_al(&r,0x03); vdd_bus_deliver_int(&bus,0x10,&r);
      CHECK(tb[0x49]==0x03 && vid.mkind==VID_KIND_TEXT, "INT 10h AH=00h 03h after VESA: 40:49=03h, text");
      vid.bda = 0;
      memset(&r,0,sizeof r); s_ah(&r,0x4F); s_al(&r,0x02); s_bx(&r,0x0101); vdd_bus_deliver_int(&bus,0x10,&r); }

    /* T13: mode 12h planar -- set mode, plot a pixel, check planes + render --- */
    memset(&r,0,sizeof r); s_ah(&r,0x00); s_al(&r,0x12); vdd_bus_deliver_int(&bus,0x10,&r);
    CHECK(vid.mode==0x12, "int10/00: mode set to 12h");
    CHECK(vid.chain4==0 && vid.map_mask==0x0F, "int10/00 mode 12h: the BIOS's SR4 (chain-4 OFF) and SR2 (0Fh) are modelled -- Hexen's loader, s74b");
    CHECK(r_al(&r)==0x20, "int10/00 mode 12h: AL=20h video-mode flag (AMI ROM and SeaBIOS agree; was the mode number)");
    memset(&r,0,sizeof r); s_ah(&r,0x00); s_al(&r,0x06); vdd_bus_deliver_int(&bus,0x10,&r);
    CHECK(r_al(&r)==0x3F, "int10/00 mode 6: AL=3Fh");
    memset(&r,0,sizeof r); s_ah(&r,0x00); s_al(&r,0x03); vdd_bus_deliver_int(&bus,0x10,&r);
    CHECK(r_al(&r)==0x30, "int10/00 mode 3: AL=30h");
    memset(&r,0,sizeof r); s_ah(&r,0x4F); s_al(&r,0x0A); s_bx(&r,0); vdd_bus_deliver_int(&bus,0x10,&r);
    /* #53: 4F0Ah now hands out a real PM interface (vbepm_test.c runs its code). It was
       AX=0100h -- what the two real BIOSes answer -- while there was nothing to hand out. */
    CHECK(r_ax(&r)==0x004F && r.es==VDD_VBEPM_SEG, "vesa/4F0A: AX=004F, ES:DI = the PM interface block (#53; was 0100)");
    memset(&r,0,sizeof r); s_ah(&r,0x00); s_al(&r,0x12); vdd_bus_deliver_int(&bus,0x10,&r);
    /* plot (x=9,y=1) colour 0x0A (1010b -> planes 1 and 3) */
    memset(&r,0,sizeof r); s_ah(&r,0x0C); s_al(&r,0x0A); s_cx(&r,9); s_dx(&r,1);
    vdd_bus_deliver_int(&bus,0x10,&r);
    { uint32_t byte = 1*(VID_G12_W/8) + (9>>3); uint8_t bit = 0x80>>(9&7);
      CHECK((vid.plane[1][byte]&bit) && (vid.plane[3][byte]&bit)
            && !(vid.plane[0][byte]&bit) && !(vid.plane[2][byte]&bit),
            "mode12: AH=0C set planes 1+3 for colour 0x0A"); }
    vid.dirty=1; vdd_bus_frame(&bus);
    CHECK(vid.frame.w==640 && vid.frame.h==480, "frame(mode12): 640x480x8");
    CHECK(vid.fb[1*VID_G12_W + 9]==0x0A, "mode12: plane-combine render -> pixel = 0x0A");

    /* T14: planar write-mode 0 + Map Mask (the common plane-fill path) -------- */
    memset(&r,0,sizeof r); s_ah(&r,0x00); s_al(&r,0x12); vdd_bus_deliver_int(&bus,0x10,&r); /* clears planes */
    { uint32_t v;
      v=2; vdd_bus_io(&bus,0x3C4,1,0,&v); v=0x0F; vdd_bus_io(&bus,0x3C5,1,0,&v);   /* map mask=0F */
      v=5; vdd_bus_io(&bus,0x3CE,1,0,&v); v=0;    vdd_bus_io(&bus,0x3CF,1,0,&v);   /* write mode 0 */
      CHECK(vid.map_mask==0x0F && vid.write_mode==0, "planar: ports set map_mask/write_mode");
      vga_planar_write(&vid, 0, 0xAA);
      CHECK(vid.plane[0][0]==0xAA && vid.plane[1][0]==0xAA && vid.plane[2][0]==0xAA && vid.plane[3][0]==0xAA,
            "planar wm0: byte -> all enabled planes");
      v=2; vdd_bus_io(&bus,0x3C4,1,0,&v); v=0x05; vdd_bus_io(&bus,0x3C5,1,0,&v);   /* map mask=05 */
      vga_planar_write(&vid, 1, 0xFF);
      CHECK(vid.plane[0][1]==0xFF && vid.plane[2][1]==0xFF && vid.plane[1][1]==0 && vid.plane[3][1]==0,
            "planar wm0: map mask gates planes (0+2 only)"); }

    /* T15: planar write-mode 2 (CPU bit p -> plane p) ------------------------ */
    { uint32_t v; v=5; vdd_bus_io(&bus,0x3CE,1,0,&v); v=2; vdd_bus_io(&bus,0x3CF,1,0,&v); /* wm2 */
      v=2; vdd_bus_io(&bus,0x3C4,1,0,&v); v=0x0F; vdd_bus_io(&bus,0x3C5,1,0,&v);          /* mask 0F */
      vga_planar_write(&vid, 2, 0x0A);   /* colour 1010b -> planes 1 and 3 */
      CHECK(vid.plane[1][2]==0xFF && vid.plane[3][2]==0xFF && vid.plane[0][2]==0 && vid.plane[2][2]==0,
            "planar wm2: colour 0x0A -> planes 1+3 (all 8 px)"); }

    /* T16: latch read loads all planes ------------------------------------- */
    vid.plane[0][3]=0x11; vid.plane[1][3]=0x22; vid.plane[2][3]=0x33; vid.plane[3][3]=0x44;
    { uint32_t v; v=4; vdd_bus_io(&bus,0x3CE,1,0,&v); v=2; vdd_bus_io(&bus,0x3CF,1,0,&v); } /* read_map=2 */
    { uint8_t got = vga_planar_read(&vid, 3);
      CHECK(got==0x33 && vid.latch[0]==0x11 && vid.latch[3]==0x44, "planar read: latches + read_map=2"); }

    /* T17: 0x3DA vertical retrace is TIMED, not toggled (GH #55 follow-up) ----- *
     * The old implementation flipped bits 3 and 0 on every read, so `WAIT &H3DA,8`
     * -- the frame clock of most DOS graphics code -- returned instantly and those
     * programs ran unbounded. A fake clock makes the real thing deterministic to
     * test: set the microsecond time, read the port, assert the bits.            */
    { uint32_t v; int i, hi, lo;
      vid.time_us = fake_clock;

      /* --- 640x480 (mode 12h): 60 Hz, 525 lines, 480 active --------------- */
      vid.gh = 480;
      g_fake_us = 0;                       /* line 0 = active picture           */
      vdd_bus_io(&bus, 0x3DA, 1, 1, &v);
      CHECK(!(v & 0x08), "3DA: no retrace during the active picture");

      g_fake_us = 16000;                   /* 16.0ms of a 16.67ms frame = vblank */
      vdd_bus_io(&bus, 0x3DA, 1, 1, &v);
      CHECK((v & 0x08) != 0, "3DA: retrace asserted during vertical blanking");
      CHECK((v & 0x01) != 0, "3DA: display-disabled set during vblank too");

      /* --- IT DOES NOT ALTERNATE. Two reads at the SAME instant must agree; the
       *     old toggle failed exactly here, and that was the whole bug. ------- */
      { uint32_t a, b;
        g_fake_us = 1000;
        vdd_bus_io(&bus, 0x3DA, 1, 1, &a);
        vdd_bus_io(&bus, 0x3DA, 1, 1, &b);
        CHECK(a == b, "3DA: two reads at the same instant agree (no toggle)"); }

      /* --- Duty cycle: retrace must be a MINORITY of the frame, or a program
       *     that waits for it to clear stalls. Sample a whole frame. -------- */
      hi = lo = 0;
      for (i = 0; i < 1000; i++) {
          g_fake_us = (uint64_t)(i * 16667 / 1000);       /* one 60 Hz frame */
          vdd_bus_io(&bus, 0x3DA, 1, 1, &v);
          if (v & 0x08) hi++; else lo++;
      }
      CHECK(hi > 0 && lo > 0, "3DA: retrace both asserted and clear across a frame");
      CHECK(hi < lo / 4, "3DA: retrace is a small minority of the frame (~9%)");

      /* --- 320x200 / text run at 70 Hz, so the SAME wall-clock instant lands
       *     differently. ⚠ This used to key off `vid.gh` alone; the CRTC is now
       *     authoritative and a mode set earlier in this battery left 12h's
       *     525-line timing behind, so the 70 Hz family has to be asked for in
       *     the registers. These are mode 03h/0Dh/13h's, from vga_defaults.h:
       *     Vertical Total 0xBF|bit8 = 449 lines, Blank Start 0x96|bit8 = 406. */
      vid.gh = 400;
      crtc_w(&bus, 0x06, 0xBF); crtc_w(&bus, 0x07, 0x1F); crtc_w(&bus, 0x09, 0x41);
      crtc_w(&bus, 0x12, 0x8F); crtc_w(&bus, 0x15, 0x96);
      hi = lo = 0;
      for (i = 0; i < 1000; i++) {
          g_fake_us = (uint64_t)(i * 14286 / 1000);       /* one 70 Hz frame */
          vdd_bus_io(&bus, 0x3DA, 1, 1, &v);
          if (v & 0x08) hi++; else lo++;
      }
      CHECK(hi > 0 && lo > 0, "3DA: 70 Hz modes also retrace once per frame");

      /* --- #225: a THROTTLED guest held across a whole retrace is owed it, once.
       *     Polls at 1 ms into consecutive 70 Hz frames never land in the ~1.2 ms
       *     blank. Off (the default): bit 3 is never seen. On: the first poll in the
       *     next frame reads it, the one after reads the true phase, and skipping
       *     several frames still owes only ONE. -------------------------------- */
      { uint32_t a, b, c, d, o0; uint64_t t, s1 = 0, s2 = 0, F, P;
        /* The frame is MEASURED off the model rather than assumed: find two
           successive retrace starts, then poll 2 ms before one (active picture). */
        vid.vbl_owe_on = 0;
        for (t = 0; t < 100000 && !s2; ++t) {
            g_fake_us = t; vdd_bus_io(&bus, 0x3DA, 1, 1, &a);
            if ((a & 0x08) && t && !(c & 0x08)) { if (!s1) s1 = t; else s2 = t; }
            c = a;
        }
        F = s2 - s1; P = s2 + 10*F - 2000;       /* active, well clear of the blank */
        vid.p3da_have_last = 0;
        g_fake_us = P;     vdd_bus_io(&bus, 0x3DA, 1, 1, &a);
        g_fake_us = P + F; vdd_bus_io(&bus, 0x3DA, 1, 1, &b);
        CHECK(F > 10000 && F < 20000 && !(a & 0x08) && !(b & 0x08),
              "3DA #225: off, a retrace slept through stays unseen");
        vid.vbl_owe_on = 1; o0 = vid.p3da_vbl_owed;
        g_fake_us = P + 2*F;      vdd_bus_io(&bus, 0x3DA, 1, 1, &a);
        g_fake_us = P + 2*F + 10; vdd_bus_io(&bus, 0x3DA, 1, 1, &b);
        CHECK((a & 0x09) == 0x09, "3DA #225: on, the missed retrace is reported on the next poll");
        CHECK(!(b & 0x08), "3DA #225: ...once; the following poll reads the true phase");
        g_fake_us = P + 6*F;      vdd_bus_io(&bus, 0x3DA, 1, 1, &c);
        g_fake_us = P + 6*F + 10; vdd_bus_io(&bus, 0x3DA, 1, 1, &d);
        CHECK((c & 0x08) && !(d & 0x08) && vid.p3da_vbl_owed == o0 + 2,
              "3DA #225: four frames skipped still owe ONE retrace");
        g_fake_us = P + 6*F + 20; vdd_bus_io(&bus, 0x3DA, 1, 1, &c);
        CHECK(!(c & 0x08), "3DA #225: nothing owed within one frame");
        vid.vbl_owe_on = 0; }

      /* --- bit 0 is a DIFFERENT signal: it must change WITHIN one scanline,
       *     which the old code (toggling it with bit 3) could never do. ----- */
      { int changed = 0; uint32_t prev = 0xFF;
        vid.gh = 480;
        for (i = 0; i < 40; i++) {                        /* ~1.3 scanlines */
            g_fake_us = (uint64_t)i;                      /* 1 us steps      */
            vdd_bus_io(&bus, 0x3DA, 1, 1, &v);
            if (prev != 0xFF && (v & 1) != (prev & 1)) changed = 1;
            prev = v;
        }
        CHECK(changed, "3DA: display-disabled (bit 0) toggles within a scanline"); }
      /* --- No clock injected -> the legacy toggle still applies, so off-VM
       *     callers that never set a clock are unaffected. ------------------ */
      { uint32_t a, b;
        vid.time_us = 0;
        vdd_bus_io(&bus, 0x3DA, 1, 1, &a);
        vdd_bus_io(&bus, 0x3DA, 1, 1, &b);
        CHECK(a != b, "3DA: with no clock injected the legacy toggle remains"); }
    }

    /* ── ★★ THE BLANKING INTERVAL COMES FROM THE CRTC, NOT FROM A TWO-CASE GUESS. ──
     *   The old model asserted retrace from line 400 (or 480 for tall modes). That is
     *   within 8 lines of the truth for every mode the BIOS sets EXCEPT 0Fh/10h --
     *   640x350 -- where real blanking starts at line 355 of 449 and the guess said
     *   400. It reported a 10.9% blanking interval where the card gives 20.9%: less
     *   than half. ⚠ 640x350 is Lemmings' MENU screen -- its gameplay is mode 0Dh,
     *   320x200, measured from the BDA of a dump of the real game.
     *
     * ⚠ EVERY NUMBER BELOW IS DECODED FROM VGA_CRTC_DEFAULT in vga_defaults.h, which
     *   was read back off a real card by tests/probes/dos/vgadefs.asm. None of it is
     *   written from memory, and none of it is this implementation's own opinion --
     *   that is the whole point, and it is what caught the bug.
     *
     *   mode 10h: CR06=0xBF CR07=0x1F CR09=0x40 CR12=0x5D CR15=0x63
     *     Vertical Total       = 0xBF | ov bit0<<8              = 447, +2 = 449 lines
     *     Vertical Display End = 0x5D | ov bit1<<8              = 349, +1 = 350 active
     *     Vertical Blank Start = 0x63 | ov bit3<<8 | ms bit5<<9 = 355                */
    {   uint32_t v; int i, hi, lo; int lo_edge = -1;
        vid.time_us = fake_clock;
        vid.gh = 350;                      /* what the old model could not express   */
        crtc_w(&bus, 0x06, 0xBF); crtc_w(&bus, 0x07, 0x1F); crtc_w(&bus, 0x09, 0x40);
        crtc_w(&bus, 0x12, 0x5D); crtc_w(&bus, 0x15, 0x63);

        /* Line 354 is still picture, line 356 is blanked. Straddling the boundary is
         * the whole claim -- a duty-cycle count alone would pass on a window in the
         * wrong PLACE, so pin the edge itself. 449 lines in 1000000/70 us.          */
        g_fake_us = (uint64_t)(354.0 * (1000000.0 / 70.0) / 449.0);
        vdd_bus_io(&bus, 0x3DA, 1, 1, &v);
        CHECK(!(v & 0x08), "3DA/CRTC: 640x350 line 354 is still the active picture");
        g_fake_us = (uint64_t)(357.0 * (1000000.0 / 70.0) / 449.0);
        vdd_bus_io(&bus, 0x3DA, 1, 1, &v);
        CHECK((v & 0x08) != 0, "3DA/CRTC: 640x350 blanking has begun by line 357");

        /* ...and the duty cycle that follows from it: 94 of 449 lines = 20.9%. The
         * old model gave 49 of 449 = 10.9%, so a >15% floor separates them. */
        hi = lo = 0;
        for (i = 0; i < 1000; i++) {
            g_fake_us = (uint64_t)((double)i * (1000000.0 / 70.0) / 1000.0);
            vdd_bus_io(&bus, 0x3DA, 1, 1, &v);
            if (v & 0x08) { hi++; if (lo_edge < 0) lo_edge = i; } else lo++;
        }
        CHECK(hi > 150 && hi < 260, "3DA/CRTC: 640x350 blanks for ~20.9% of the frame");
        CHECK(lo > 0, "3DA/CRTC: 640x350 still shows a picture for most of the frame");

        /* A HALF-WRITTEN MODE SET MUST NOT BE BELIEVED. Blank Start before Display
         * End is not a screen; the guest is mid-reprogram. Falling back to the old
         * constants is survivable, a garbage frame period is not -- it would freeze
         * every guest that waits on retrace. */
        crtc_w(&bus, 0x15, 0x10);          /* blank start 16, well above the picture */
        hi = lo = 0;
        for (i = 0; i < 200; i++) {
            g_fake_us = (uint64_t)((double)i * (1000000.0 / 70.0) / 200.0);
            vdd_bus_io(&bus, 0x3DA, 1, 1, &v);
            if (v & 0x08) hi++; else lo++;
        }
        CHECK(hi > 0 && lo > hi, "3DA/CRTC: an impossible blank start falls back, still sane");
        vid.time_us = 0;
    }

    /* ── ★★★ LEMMINGS' OWN BLITTERS, REPLAYED REGISTER FOR REGISTER. ──────────────────
     *   Not invented, and not read off a datasheet: the register sequences the game
     *   was OBSERVED to program -- the rig's IO-SITE and read/write-site instruments,
     *   cross-checked against the game running under genuine MS-DOS 6.22
     *   (scripts/lemref.py).
     *
     *   Why these two and not some other pair: they are the ONLY two sites in the game
     *   that read video memory -- the read-site histogram attributes every one of
     *   ~970,000 reads in a level to them, with zero lost to hash collisions. Pin these
     *   and the whole of Lemmings' drawing is pinned.
     *
     * ⚠ THE VALUE IS THAT A SHIPPING 1991 GAME IS THE EXPECTATION. These sequences ran
     *   on real hardware; if our VGA disagrees with them it is our VGA that is wrong.
     *   That is a different and stronger claim than "it matches our reading of the spec".
     */
    {   uint8_t got;

        /* ── (1) THE MASKED SPRITE BLITTER, the colour-compare path. Set-up:
         *       GR5 = 0x08  read mode 1
         *       GR7 = 0x08  don't care: plane 3 only
         *       GR2 = 0x08  compare: plane 3 SET
         *   and then, per byte: a colour-compare read, its complement into the Bit
         *   Mask (GR8), and one byte of sprite written --
         *   i.e. "which pixels here are colour 8..15? -- write my sprite into the
         *   OTHERS." Transparency done by the card, one byte at a time. This is what
         *   read mode 1 is actually for in this game; it is not collision detection.  */
        memset(&r,0,sizeof r); s_ah(&r,0x00); s_al(&r,0x12); vdd_bus_deliver_int(&bus,0x10,&r);
        vid.plane[3][0x40] = 0xF0;     /* left four pixels are colour 8..15 = "terrain" */
        vid.plane[0][0x40] = 0x00;
        gc_w(&bus, 0x00, 0x00);        /* GR0 set/reset        = 0 (as Lemmings does) */
        gc_w(&bus, 0x01, 0x00);        /* GR1 enable set/reset = 0                    */
        gc_w(&bus, 0x05, 0x08);        /* GR5 READ MODE 1                             */
        gc_w(&bus, 0x07, 0x08);        /* GR7 colour don't care                       */
        gc_w(&bus, 0x02, 0x08);        /* GR2 colour compare                          */
        got = vga_planar_read(&vid, 0x40);
        CHECK(got == 0xF0, "lemmings blit: colour-compare read names the colour-8..15 pixels");
        CHECK(vid.latch[3] == 0xF0, "lemmings blit: ...and a mode-1 read still loads the latches");

        /* The complement -> 0x0F, straight into the Bit Mask, then one byte of sprite. */
        gc_w(&bus, 0x08, (uint8_t)~got);   /* GR8 bit mask = 0x0F                     */
        gc_w(&bus, 0x03, 0x00);            /* GR3 replace (the OR case is below)      */
        gc_w(&bus, 0x05, 0x00);            /* back to write mode 0 to do the movsb    */
        sc_w(&bus, 0x02, 0x01);            /* map mask = plane 0, as its per-plane loop */
        vga_planar_write(&vid, 0x40, 0xFF);
        CHECK(vid.plane[0][0x40] == 0x0F,
              "lemmings blit: the sprite lands ONLY where the terrain was not");
        CHECK(vid.plane[3][0x40] == 0xF0,
              "lemmings blit: ...and the terrain plane is untouched by it");

        /* ⚠ IT REALLY DOES USE THE ALU. The game also writes GR3 = 0x10, which is
         *   function select = OR, not replace. A card that ignored GR3 would pass
         *   every check above and still draw this game wrong, so pin the OR itself:
         *   masked-in bits become (cpu OR latch), masked-out bits stay latch.        */
        vid.plane[0][0x41] = 0x55;                 /* latch source                     */
        vga_planar_read(&vid, 0x41);               /* load the latches                 */
        gc_w(&bus, 0x03, 0x10);                    /* GR3 = OR, exactly as Lemmings    */
        gc_w(&bus, 0x08, 0x0F);                    /* bit mask: low nibble only        */
        vga_planar_write(&vid, 0x41, 0x0A);
        CHECK(vid.plane[0][0x41] == 0x5F,
              "lemmings blit: GR3 function select ORs cpu with latch (0x5|0xA -> 0xF)");
        gc_w(&bus, 0x03, 0x00);
        gc_w(&bus, 0x08, 0xFF);

        /* ── (2) THE PLAIN BLITTER, which is the busier of the two (858,644 reads of
         *   the 970,000). Per byte it reads VRAM and then copies one byte in.
         *   The read's VALUE IS DISCARDED -- it is there to load the latches, so that
         *   the planes the Map Mask disables keep what they had. If a card lets a
         *   disabled plane change, every sprite in the game smears across the others.
         *   That is the guarantee this pins.                                         */
        vid.plane[0][0x50]=0x11; vid.plane[1][0x50]=0x22;
        vid.plane[2][0x50]=0x33; vid.plane[3][0x50]=0x44;
        gc_w(&bus, 0x05, 0x00);            /* read mode 0, write mode 0               */
        gc_w(&bus, 0x04, 0x00);            /* read map 0 -- the value it throws away  */
        sc_w(&bus, 0x02, 0x04);            /* map mask = plane 2 only                 */
        (void)vga_planar_read(&vid, 0x50); /* the discarded read: latches, not data    */
        CHECK(vid.latch[1] == 0x22 && vid.latch[3] == 0x44,
              "lemmings blit: the discarded read is what loads all four latches");
        vga_planar_write(&vid, 0x50, 0x99);
        CHECK(vid.plane[2][0x50] == 0x99, "lemmings blit: movsb writes the selected plane");
        CHECK(vid.plane[0][0x50] == 0x11 && vid.plane[1][0x50] == 0x22 &&
              vid.plane[3][0x50] == 0x44,
              "lemmings blit: the other three planes are preserved exactly");

        /* ── (3) THE VRAM->VRAM COPY. This is how the toolbar gets on screen, and it
         *   is the mechanism behind the open "panel has no icons" bug:
         *       Map Mask = 0x0F (all four planes), GR5 = WRITE MODE 1,
         *       then byte copies with source AND destination in A000.
         *   Write mode 1 ignores the CPU byte entirely and writes THE FOUR LATCHES to
         *   the four planes, so one `movsb` moves a four-plane pixel group. It is the
         *   only way to move 16-colour artwork without four passes, and it is how the
         *   panel is pulled from its off-screen cache (VRAM 0xF91F..0xFFFA -- which is
         *   above the visible page and only addressable because a plane is 64KB).
         * ⚠ THE CPU BYTE MUST NOT MATTER. A card that quietly used it would copy
         *   whatever `movsb` happened to load instead of the latched pixels, and the
         *   panel would come out a flat colour -- icons missing, which is the symptom.
         *   So the check feeds it a deliberately wrong byte.                          */
        vid.plane[0][0x60]=0xDE; vid.plane[1][0x60]=0xAD;
        vid.plane[2][0x60]=0xBE; vid.plane[3][0x60]=0xEF;
        vid.plane[0][0x61]=0x00; vid.plane[1][0x61]=0x00;
        vid.plane[2][0x61]=0x00; vid.plane[3][0x61]=0x00;
        sc_w(&bus, 0x02, 0x0F);            /* Map Mask = 0x0F, all four planes        */
        gc_w(&bus, 0x05, 0x01);            /* GR5 = write mode 1                      */
        (void)vga_planar_read(&vid, 0x60); /* the `movsb` source read: latches         */
        vga_planar_write(&vid, 0x61, 0x00);/* ...and its store. CPU byte is a LIE.     */
        CHECK(vid.plane[0][0x61]==0xDE && vid.plane[1][0x61]==0xAD &&
              vid.plane[2][0x61]==0xBE && vid.plane[3][0x61]==0xEF,
              "lemmings panel: write mode 1 copies all four planes from the latches");
        CHECK(vid.plane[0][0x60]==0xDE && vid.plane[3][0x60]==0xEF,
              "lemmings panel: ...and the source group is left alone");

        /* The panel cache lives at 0xF91F..0xFFFA -- ABOVE the 320x200 visible page
         * (0x1F40) and running to the last byte of the plane. `VID_PLANE_SIZE` was
         * once 38400, so every one of those bytes read back 0xFF and every write was
         * dropped: the panel was being cached into a hole. Pin both ends. */
        CHECK(VID_PLANE_SIZE == 0x10000, "a VGA plane is 64KB, so off-screen VRAM exists");
        vid.plane[2][0xF91F] = 0x5A; vid.plane[2][0xFFFA] = 0xA5;
        gc_w(&bus, 0x05, 0x00); gc_w(&bus, 0x04, 0x02);   /* read mode 0, read plane 2 */
        CHECK(vga_planar_read(&vid, 0xF91F)==0x5A && vga_planar_read(&vid, 0xFFFA)==0xA5,
              "lemmings panel: the off-screen cache 0xF91F..0xFFFA reads back");

        /* ── (4) THE WHOLE-PANEL BLIT -- THE ROUTINE THAT ACTUALLY PUTS THE TOOLBAR ON
         *   SCREEN, and the one the "missing icons" bug is about. Same set-up as (3),
         *   then ONE `rep movsb` of 0x6E0 bytes (1760 = 40 rows x 44) from the panel
         *   cache at 0xF91F, to BOTH pages (destination +0x1E42 on each). It runs on
         *   every level start, unconditionally.
         *
         * ⚠ THIS CORRECTS THE PINNED STORY. The per-button routine whose write site the
         *   rig named is NOT the panel painter: it repaints ONE button when the
         *   SELECTED skill changes. Running ~1.5 times in a run where the player
         *   changed selection once is correct, not a defect -- so "the blitter runs
         *   1.5 times instead of 12" was a question about the wrong routine.
         *
         * ⚠⚠ WHY A REP AND NOT A LOOP OF ONE. In `rep movsb` every single byte must do
         *   its OWN read-then-write: the read loads the latches, the write emits them.
         *   A model that hoisted the read out of the rep, or let the latches go stale
         *   across it, would smear ONE pixel group over all 1760 bytes -- a flat-colour
         *   panel with no icons, which is exactly the reported symptom. So the source
         *   here is deliberately NON-uniform and every byte of the result is checked. */
        {   uint32_t i2; int same = 1; unsigned pl;
            const uint32_t SRC = 0xF91F, DST = 0x1E42, N = 0x6E0;
            /* A pattern that differs per byte AND per plane, so a stale latch or a
               wrong plane cannot coincidentally reproduce it. */
            for (i2 = 0; i2 < N; ++i2)
                for (pl = 0; pl < 4; ++pl) {
                    vid.plane[pl][SRC + i2] = (uint8_t)(i2 * 7u + pl * 61u + 1u);
                    vid.plane[pl][DST + i2] = 0x00;      /* a black panel to start from */
                }
            sc_w(&bus, 0x02, 0x0F);            /* Map Mask = 0x0F                      */
            gc_w(&bus, 0x05, 0x01);            /* GR5 = write mode 1                   */
            for (i2 = 0; i2 < N; ++i2) {       /* rep movsb, byte for byte             */
                (void)vga_planar_read(&vid, SRC + i2);
                vga_planar_write(&vid, DST + i2, 0x00);  /* CPU byte is ignored        */
            }
            for (i2 = 0; i2 < N && same; ++i2)
                for (pl = 0; pl < 4; ++pl)
                    if (vid.plane[pl][DST + i2] != vid.plane[pl][SRC + i2]) { same = 0; break; }
            CHECK(same, "lemmings panel: the 1760-byte rep movsb reproduces all four planes");
            /* A stale-latch model passes a one-byte check and fails this one: it would
               leave every destination byte equal to the FIRST source group. */
            CHECK(vid.plane[0][DST + 1] != vid.plane[0][DST],
                  "lemmings panel: ...byte by byte, not one group smeared over the copy");
            /* THE COPY ENDS ONE BYTE FROM THE TOP OF THE PLANE: 0xF91F + 0x6E0 - 1 =
               0xFFFE. An off-by-one in the plane bound truncates the last row of the
               toolbar rather than failing outright, so pin the final byte explicitly. */
            CHECK(SRC + N - 1 == 0xFFFE, "lemmings panel: the blit ends at 0xFFFE, inside the plane");
            CHECK(vid.plane[3][DST + N - 1] == vid.plane[3][SRC + N - 1],
                  "lemmings panel: ...and that last byte copies like any other");
        }

        /* ── (5) ★★★ AL BIT 7 ON A MODE SET MEANS "DO NOT CLEAR VIDEO MEMORY". ───
         *   THE ACTUAL TOOLBAR BUG. We took `al & 0x7F` for the mode number and threw
         *   the bit away, so every mode set wiped all four 64KB planes. Lemmings
         *   composes its skill-button panel into OFF-SCREEN VRAM and only then sets
         *   its mode -- INT 10h AX=008Dh for gameplay, AX=0090h for the menu (the
         *   INT 10h trace) -- with bit 7 set precisely so that cache survives.
         *   We erased it, and the panel blit copied 1760 bytes of zeroes.
         * ⚠ The off-screen half is the half that matters and the half a screen-shaped
         *   test would miss: the visible page gets redrawn immediately either way, so
         *   a check that only looked at the picture would pass while the bug remained. */
        {   ntvdd_regs r;
            uint32_t OFF = 0xF91F, VIS = 0x0100;
            int pl;

            for (pl = 0; pl < 4; ++pl) {
                vid.plane[pl][OFF] = (uint8_t)(0xA0 + pl);
                vid.plane[pl][VIS] = (uint8_t)(0x50 + pl);
            }
            memset(&r, 0, sizeof r); s_ah(&r, 0x00); s_al(&r, 0x8D);   /* mode 0Dh, PRESERVE */
            vdd_bus_deliver_int(&bus, 0x10, &r);
            CHECK(vid.mode == 0x0D, "mode set: AL=0x8D still selects mode 0Dh (bit 7 is not the mode)");
            {   int kept = 1;
                for (pl = 0; pl < 4; ++pl)
                    if (vid.plane[pl][OFF] != (uint8_t)(0xA0 + pl)) kept = 0;
                CHECK(kept, "mode set: AL bit 7 PRESERVES the off-screen sprite cache");
            }
            {   int kept = 1;
                for (pl = 0; pl < 4; ++pl)
                    if (vid.plane[pl][VIS] != (uint8_t)(0x50 + pl)) kept = 0;
                CHECK(kept, "mode set: ...and the visible page too -- it is ALL of display memory");
            }
            /* AND THE DEFAULT MUST STILL CLEAR, or every guest that relies on a mode
               set to blank the screen inherits the last program's picture. */
            memset(&r, 0, sizeof r); s_ah(&r, 0x00); s_al(&r, 0x0D);   /* mode 0Dh, CLEAR */
            vdd_bus_deliver_int(&bus, 0x10, &r);
            {   int cleared = 1;
                for (pl = 0; pl < 4; ++pl)
                    if (vid.plane[pl][OFF] || vid.plane[pl][VIS]) cleared = 0;
                CHECK(cleared, "mode set: WITHOUT bit 7 the planes are cleared, as before");
            }
        }

        /* ── (6) THE INSTRUMENT THAT HAS TO ANSWER "DID THE BLIT RUN AT ALL". ─────
         *   The rig's answer so far is an ABSENCE: no read site at the panel blit's
         *   pc. But that report is drawn from 256-slot single-slot hashes which lost
         *   249,630 reads on the same run, so an absence there can equally mean the
         *   pc collided with a busier one -- the two readings are indistinguishable,
         *   and acting on the wrong one costs a session. The linear cache table
         *   exists to make the absence mean something, so it is worth exactly as
         *   much as this test: replay the two routines and check it names both, in
         *   the order they happened.                                                */
        {   uint32_t i2; unsigned k, nrd = 0, nwr = 0;
            uint32_t first_wr = 0, first_rd = 0;
            const uint32_t SRC = 0xF91F, DST = 0x1E42;
            const uint32_t PC_COMPOSE = 0x01105815, PC_BLIT = 0x01107626;

            memset(vid.csite, 0, sizeof vid.csite);
            vid.csite_lost = 0; vid.csite_seq = 0;
            vid.guest_pc = fake_pc;

            g_fake_pc = PC_COMPOSE;                     /* the compositor fills it   */
            gc_w(&bus, 0x05, 0x00);                     /* write mode 0, plain bytes */
            sc_w(&bus, 0x02, 0x0F);
            for (i2 = 0; i2 < 64; ++i2) vga_planar_write(&vid, SRC + i2, (uint8_t)i2);
            g_fake_pc = PC_BLIT;                        /* ...then the blit reads it */
            gc_w(&bus, 0x05, 0x01);
            for (i2 = 0; i2 < 64; ++i2) {
                (void)vga_planar_read(&vid, SRC + i2);
                vga_planar_write(&vid, DST + i2, 0x00);
            }
            for (k = 0; k < VID_CSITES; ++k) {
                if (!vid.csite[k].n) continue;
                if (vid.csite[k].wr) { nwr++; if (vid.csite[k].pc == PC_COMPOSE) first_wr = vid.csite[k].first; }
                else                 { nrd++; if (vid.csite[k].pc == PC_BLIT)    first_rd = vid.csite[k].first; }
            }
            CHECK(nwr == 1 && nrd == 1 && !vid.csite_lost,
                  "cache sites: the compositor and the blit are BOTH named, none lost");
            CHECK(first_wr && first_rd && first_wr < first_rd,
                  "cache sites: ...and the order says the cache was filled BEFORE it was read");
            /* THE WRITE TO THE SCREEN MUST NOT BE COUNTED AS A CACHE TOUCH. The blit's
               destination is an ordinary visible page; if the floor let it in, the table
               would report the blitter as its own compositor and invert the ordering. */
            {   int below = 0;
                for (k = 0; k < VID_CSITES; ++k)
                    if (vid.csite[k].n && vid.csite[k].lo < VID_CACHE_LO) below = 1;
                CHECK(!below && DST < VID_CACHE_LO,
                      "cache sites: the visible page is below the floor, so it is ignored");
            }
            /* A FULL TABLE MUST SAY SO RATHER THAN SILENTLY DROP. */
            for (i2 = 0; i2 < VID_CSITES + 4u; ++i2) {
                g_fake_pc = 0x02000000u + i2 * 0x100u;
                (void)vga_planar_read(&vid, SRC);
            }
            CHECK(vid.csite_lost > 0, "cache sites: overflow is REPORTED, never dropped in silence");

            memset(vid.csite, 0, sizeof vid.csite);
            vid.csite_lost = 0; vid.csite_seq = 0;
            vid.guest_pc = 0;                  /* leave the rest of the battery as it was */
        }
    }


    /* ── ★ THE CURSOR'S SHAPE, IN THE UNITS DOS ACTUALLY ASKS IN. ────────────────
         DOS sets its cursor in SCAN LINES of an 8-line character cell, because that
         is the machine it was written for. Our cell is 16 lines, so honouring those
         numbers literally puts the underline halfway up -- rendered as "ABC123-"
         where a real DOS box shows "ABC123_". These are the exact shapes DOS uses,
         so a regression here is visible on every prompt. */
    {   unsigned st0 = 99, en = 99; int hid = 9;

        vdd_cursor_lines(0x0607, 16, &st0, &en, &hid);
        CHECK(st0 == 14 && en == 15 && !hid,
              "cursor 6-7 (DOS overwrite underline) scales to 14-15: the BOTTOM two lines");

        vdd_cursor_lines(0x0007, 16, &st0, &en, &hid);
        CHECK(st0 == 1 && en == 15 && !hid,
              "cursor 0-7 (DOS INSERT mode) scales to 1-15: a full block");

        /* ⚠ The two-line branch. Scaling both ends the ordinary way would give
             13-15 -- three lines -- and the underline would be visibly fat. */
        CHECK((0x0607 >> 8) + 1 == (0x0607 & 0x1f),
              "...and 6-7 IS the adjacent-line case that branch exists for");

        vdd_cursor_lines(0x0507, 16, &st0, &en, &hid);
        CHECK(st0 == 11 && en == 15 && !hid, "cursor 5-7 (half block) scales to 11-15");

        /* A shape that already knows about 16-line cells must NOT be scaled again. */
        vdd_cursor_lines(0x0D0F, 16, &st0, &en, &hid);
        CHECK(st0 == 13 && en == 15 && !hid,
              "cursor 13-15 is already in 16-line units and is left alone");

        /* Hiding, both idioms. */
        vdd_cursor_lines(0x2607, 16, &st0, &en, &hid);
        CHECK(hid, "CH bit 5 hides the cursor, and is not mistaken for a scan line");
        vdd_cursor_lines(0x0F0E, 16, &st0, &en, &hid);
        CHECK(hid, "start past end is the other 'no cursor' idiom");

        /* An 8-line cell must pass through untouched -- that is what the guard is for. */
        vdd_cursor_lines(0x0607, 8, &st0, &en, &hid);
        CHECK(st0 == 6 && en == 7 && !hid, "on a real 8-line cell nothing is scaled");

        /* Never off the end of the cell, whatever is asked for. */
        vdd_cursor_lines(0x1F1F, 16, &st0, &en, &hid);
        CHECK(st0 < 16 && en < 16, "an out-of-range shape is clamped inside the cell"); }

    /* T20: WHAT A MODE SET LEAVES IN THE AC AND THE DAC, PER MODE -------------
       These values are measured on genuine MS-DOS 6.22 by tests/probes/dos/vgadefs.asm
       (checked in as vgadefs.ref.txt) and generated into src/vdd/vga_defaults.h.
       They are asserted here because the previous single table was inferred from a
       screenshot and was wrong in two independent ways at once, and nothing in the
       battery noticed -- a test card renders the whole chain, so it passes whenever
       two wrong links cancel. Reading the registers back cannot do that. */
    {   static const uint8_t AC_CGA[16] = { 0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,
                                            0x10,0x11,0x12,0x13,0x14,0x15,0x16,0x17 };
        static const uint8_t AC_EGA[16] = { 0x00,0x01,0x02,0x03,0x04,0x05,0x14,0x07,
                                            0x38,0x39,0x3A,0x3B,0x3C,0x3D,0x3E,0x3F };
        int i, ok;

        memset(&r,0,sizeof r); s_ah(&r,0x00); s_al(&r,0x0D);
        vdd_bus_deliver_int(&bus,0x10,&r);
        for (i = 0, ok = 1; i < 16; ++i) if (vid.vpal[i] != AC_CGA[i]) ok = 0;
        CHECK(ok, "mode 0Dh leaves the CGA attribute table (6 at index 6, 10h..17h high)");
        /* Mode 0Dh's DAC repeats the same sixteen colours four times over 0..0x3F.
           That repetition is why card 11 could not tell 0x10..0x17 from 0x38..0x3F. */
        for (i = 0, ok = 1; i < 64; ++i)
            /* ⚠ <<4, not <<3. The bright eight sit at 0x10..0x17, and 0x08..0x0F is
               the DARK eight over again -- which is exactly why mode 0Dh's AC table
               has to reach up to 0x10 to find them. */
            if (vid.dac[i] != vid.dac[(i & 7) | (((i >> 4) & 1) << 4)]) ok = 0;
        CHECK(ok, "mode 0Dh's default DAC is the 16 CGA colours, repeated four times");

        memset(&r,0,sizeof r); s_ah(&r,0x00); s_al(&r,0x10);
        vdd_bus_deliver_int(&bus,0x10,&r);
        for (i = 0, ok = 1; i < 16; ++i) if (vid.vpal[i] != AC_EGA[i]) ok = 0;
        CHECK(ok, "mode 10h leaves the EGA attribute table (0x14 at index 6)");
        CHECK(vid.dac[0x14] == 0xFFAA5500u, "mode 10h: DAC 0x14 is EGA brown");
        CHECK(vid.dac[0x06] == 0xFFAAAA00u, "mode 10h: DAC 0x06 is dark yellow, not brown");

        /* ★ THE WHOLE LEMMINGS FAULT IN ONE ASSERTION. The game writes its colour 6
           to the DAC entry its own copy of this table names -- 0x14 -- and a pixel of
           value 6 must read it back. With 6 in that slot the write went to 0x14 and
           the read came from 0x06, so exactly one colour of sixteen was stale. */
        {   uint32_t v; v=0x14; vdd_bus_io(&bus,0x3C8,1,0,&v);
            v=0x3F; vdd_bus_io(&bus,0x3C9,1,0,&v);
            v=0x00; vdd_bus_io(&bus,0x3C9,1,0,&v);
            v=0x00; vdd_bus_io(&bus,0x3C9,1,0,&v);
            CHECK(vid.pal[6] == vid.dac[0x14],
                  "mode 10h: a DAC 0x14 write is what pixel value 6 renders with"); }

        memset(&r,0,sizeof r); s_ah(&r,0x00); s_al(&r,0x13);
        vdd_bus_deliver_int(&bus,0x10,&r);
        for (i = 0, ok = 1; i < 16; ++i) if (vid.vpal[i] != i) ok = 0;
        CHECK(ok, "mode 13h leaves the identity attribute table");
        /* 13h's default is the real 256-colour palette, not a grey ramp: greys sit
           at 0x10..0x1F and the colour wheel starts at 0x20. */
        CHECK(vid.dac[0x10]==0xFF000000u && vid.dac[0x1F]==0xFFFFFFFFu,
              "mode 13h: 0x10..0x1F is the grey ramp, black to white");
        CHECK(vid.dac[0x20]==0xFF0000FFu, "mode 13h: the colour wheel starts at 0x20");

        /* ★ A MODE SET REPROGRAMS THE CRTC. Without this a screen inherits the
           geometry of the one before it: Lemmings' gameplay sets Offset=22 for its
           352-pixel scrolling window, and the mode 10h screen that follows was drawn
           44 bytes to the line instead of 80 -- diagonal noise. */
        memset(&r,0,sizeof r); s_ah(&r,0x00); s_al(&r,0x0D);
        vdd_bus_deliver_int(&bus,0x10,&r);
        CHECK(vid.crtc_offset==0x14, "mode 0Dh's default CRTC Offset is 20 (320px)");
        {   uint32_t v = 0x13; vdd_bus_io(&bus,0x3D4,1,0,&v);   /* Offset register */
            v = 22;            vdd_bus_io(&bus,0x3D5,1,0,&v); } /* ...as Lemmings sets it */
        CHECK(vid.crtc_offset==22 && vid.crtc_off_seen,
              "a guest CAN set its own Offset, and it is recorded as seen");
        memset(&r,0,sizeof r); s_ah(&r,0x00); s_al(&r,0x10);
        vdd_bus_deliver_int(&bus,0x10,&r);
        CHECK(vid.crtc_offset==0x28, "...and the next mode set takes it back to 40 (640px)");
        CHECK(vid.crtc_start==0 && !vid.crtc_off_seen,
              "a mode set also clears the start address and the seen flag");

        /* Bit 7 of AL means "do not clear the buffer" and nothing else -- measured. */
        memset(&r,0,sizeof r); s_ah(&r,0x00); s_al(&r,0x90);
        vdd_bus_deliver_int(&bus,0x10,&r);
        for (i = 0, ok = 1; i < 16; ++i) if (vid.vpal[i] != AC_EGA[i]) ok = 0;
        CHECK(ok, "AL=90h is mode 10h: bit 7 does not change the palette load"); }

    /* T21: READ MODE 1 -- COLOUR COMPARE. ------------------------------------
       A read in mode 1 returns one BIT PER PIXEL, set where that pixel's 4-bit
       colour matches GR2 in every plane GR7 selects. It is how a game asks the
       hardware "which of these eight pixels are solid", i.e. pixel-perfect terrain
       collision in one instruction. GR5 bit 3 used to be masked off and GR2/GR7
       dropped entirely, so every such read came back as a raw plane byte. */
    {   uint32_t v;
        /* ⚠ A DELTA, NOT A TOTAL. This used to assert `rmode_hist[1]==4` against the
           whole run's count, so adding a read ANYWHERE earlier in the battery broke a
           test that has nothing to do with the addition -- which is exactly what the
           Lemmings blit cases then did. Snapshot here and assert the change; the claim
           is just as tight and it no longer depends on what else the file does. */
        uint32_t rm0 = vid.rmode_hist[0], rm1 = vid.rmode_hist[1];
        memset(&r,0,sizeof r); s_ah(&r,0x00); s_al(&r,0x12);
        vdd_bus_deliver_int(&bus,0x10,&r);
        /* Hand-place eight pixels in one byte: colours 0,1,2,3,4,5,6,7 left to right.
           Plane p bit (7-k) is bit p of pixel k's colour. */
        {   int k, pl;
            for (pl = 0; pl < 4; ++pl) {
                uint8_t b = 0;
                for (k = 0; k < 8; ++k) if ((k >> pl) & 1) b = (uint8_t)(b | (0x80 >> k));
                vid.plane[pl][0] = b;
            } }
        v = 5; vdd_bus_io(&bus,0x3CE,1,0,&v);            /* GR5 Mode          */
        v = 0x08; vdd_bus_io(&bus,0x3CF,1,0,&v);         /* read mode 1       */
        CHECK(vid.read_mode==1 && vid.write_mode==0,
              "GR5 bit 3 selects read mode 1 and leaves the write mode alone");
        v = 7; vdd_bus_io(&bus,0x3CE,1,0,&v);            /* GR7 Don't Care    */
        v = 0x0F; vdd_bus_io(&bus,0x3CF,1,0,&v);         /* compare all planes */
        v = 2; vdd_bus_io(&bus,0x3CE,1,0,&v);            /* GR2 Color Compare */
        v = 5; vdd_bus_io(&bus,0x3CF,1,0,&v);            /* looking for colour 5 */
        CHECK(vga_planar_read(&vid,0)==(0x80>>5),
              "read mode 1: exactly the pixel whose colour is 5 comes back set");
        v = 2; vdd_bus_io(&bus,0x3CE,1,0,&v);
        v = 0; vdd_bus_io(&bus,0x3CF,1,0,&v);
        CHECK(vga_planar_read(&vid,0)==(0x80>>0),
              "...and colour 0 finds only pixel 0");
        /* GR7 = 0 means NO plane takes part, so every pixel matches. That is the
           hardware's answer and not a bug to be tidied away. */
        v = 7; vdd_bus_io(&bus,0x3CE,1,0,&v);
        v = 0; vdd_bus_io(&bus,0x3CF,1,0,&v);
        CHECK(vga_planar_read(&vid,0)==0xFF,
              "Color Don't Care = 0 compares nothing, so every pixel matches");
        /* Only plane 0 in the comparison: colours 1,3,5,7 have bit 0 set. */
        v = 7; vdd_bus_io(&bus,0x3CE,1,0,&v);
        v = 1; vdd_bus_io(&bus,0x3CF,1,0,&v);
        v = 2; vdd_bus_io(&bus,0x3CE,1,0,&v);
        v = 1; vdd_bus_io(&bus,0x3CF,1,0,&v);
        CHECK(vga_planar_read(&vid,0)==0x55,
              "comparing plane 0 alone finds every odd-numbered colour");
        /* A read in mode 1 must STILL load the latches -- a masked write right after
           one depends on them, and that is the pairing a collision-and-draw loop uses. */
        CHECK(vid.latch[0]==0x55, "read mode 1 still loads the latches");
        /* Back to mode 0 and the plane select works as before. */
        v = 5; vdd_bus_io(&bus,0x3CE,1,0,&v);
        v = 0; vdd_bus_io(&bus,0x3CF,1,0,&v);
        v = 4; vdd_bus_io(&bus,0x3CE,1,0,&v);
        v = 2; vdd_bus_io(&bus,0x3CF,1,0,&v);
        CHECK(vga_planar_read(&vid,0)==vid.plane[2][0],
              "read mode 0 still returns the plane GR4 selects");
        CHECK(vid.rmode_hist[1]-rm1==4 && vid.rmode_hist[0]-rm0>=1,
              "the read-mode histogram counts what was actually served"); }

    /* T22: THE START ADDRESS IS LATCHED, so a page flip is never seen half-written.
       The pair is two byte registers; between them the value is half old and half new,
       and a frame built there is a whole-screen glitch. Real hardware loads the
       address counter at the vertical retrace, so it cannot happen. */
    {   uint32_t v;
        memset(&r,0,sizeof r); s_ah(&r,0x00); s_al(&r,0x0D);
        vdd_bus_deliver_int(&bus,0x10,&r);
        v = 0x0C; vdd_bus_io(&bus,0x3D4,1,0,&v);
        v = 0x20; vdd_bus_io(&bus,0x3D5,1,0,&v);         /* high byte of 0x2040 */
        CHECK(vid.crtc_start_live==0 && vid.crtc_start_pend,
              "the high byte alone does not move the display -- nothing is latched yet");
        v = 0x0D; vdd_bus_io(&bus,0x3D4,1,0,&v);
        v = 0x40; vdd_bus_io(&bus,0x3D5,1,0,&v);         /* low byte completes it */
        CHECK(vid.crtc_start_live==0 && !vid.crtc_start_pend
              && vid.crtc_start_writes>=1,
              "...and neither does the low byte: the LATCH is what moves it");
        vid.dirty=1; vdd_bus_frame(&bus);
        CHECK(vid.crtc_start_live==0x2040,
              "the next frame latches the completed pair, as the retrace does");
        {   uint32_t before = vid.crtc_start_half;
            v = 0x0C; vdd_bus_io(&bus,0x3D4,1,0,&v);
            v = 0x30; vdd_bus_io(&bus,0x3D5,1,0,&v);     /* half a new address */
            vid.dirty=1; vdd_bus_frame(&bus);
            CHECK(vid.crtc_start_half==before+1,
                  "a frame latched mid-pair is COUNTED -- hardware tears there too"); } }

    /* T22b: THE HARDWARE'S SCHEDULE, ON A CLOCK (s83, Mario). The start address loads at
       the start of vertical retrace; pel panning (AR13) is taken as the next picture
       begins. Mario writes the start during display, waits for retrace, writes the pan:
       both must appear TOGETHER on the next frame -- not the start early, not the pan
       never. 0Dh: 70 Hz, F = 14285 us, 449 lines, retrace from line 400 (~12726 us). */
    {   uint32_t v; const uint64_t F = 14285u, T = 1000u * 14285u;
        v = 0x0C; vdd_bus_io(&bus,0x3D4,1,0,&v); v = 0x00; vdd_bus_io(&bus,0x3D5,1,0,&v);
        v = 0x0D; vdd_bus_io(&bus,0x3D4,1,0,&v); v = 0x00; vdd_bus_io(&bus,0x3D5,1,0,&v);
        vid.time_us = fake_clock; vid.gh = 200; vid.latch_t = 0;
        g_fake_us = T + 1000; vid.dirty=1; vdd_bus_frame(&bus);       /* sync: start 0  */
        g_fake_us = T + 2000;                                          /* in the picture */
        v = 0x0C; vdd_bus_io(&bus,0x3D4,1,0,&v); v = 0x01; vdd_bus_io(&bus,0x3D5,1,0,&v);
        v = 0x0D; vdd_bus_io(&bus,0x3D4,1,0,&v); v = 0x08; vdd_bus_io(&bus,0x3D5,1,0,&v);
        vid.dirty=1; vdd_bus_frame(&bus);
        CHECK(vid.crtc_start_live==0, "clocked: a start written in the picture is not shown yet");
        g_fake_us = T + 13000;                                         /* in retrace     */
        vdd_bus_io(&bus,0x3DA,1,1,&v);                                 /* reset the AC flip-flop */
        v = 0x33; vdd_bus_io(&bus,0x3C0,1,0,&v); v = 0x03; vdd_bus_io(&bus,0x3C0,1,0,&v);
        vid.dirty=1; vdd_bus_frame(&bus);
        CHECK(vid.crtc_start_live==0 && vid.disp_pan==0,
              "clocked: during the retrace the old picture is still up (start and pan)");
        g_fake_us = T + F + 500;                                       /* next picture   */
        vid.dirty=1; vdd_bus_frame(&bus);
        CHECK(vid.crtc_start_live==0x0108 && vid.disp_pan==3,
              "clocked: the next frame shows the new start AND the new pan together");
        /* A start written INSIDE the retrace missed that load: a frame later. */
        g_fake_us = T + F + 13000;
        v = 0x0C; vdd_bus_io(&bus,0x3D4,1,0,&v); v = 0x02; vdd_bus_io(&bus,0x3D5,1,0,&v);
        v = 0x0D; vdd_bus_io(&bus,0x3D4,1,0,&v); v = 0x00; vdd_bus_io(&bus,0x3D5,1,0,&v);
        g_fake_us = T + 2*F + 500; vid.dirty=1; vdd_bus_frame(&bus);
        CHECK(vid.crtc_start_live==0x0108, "clocked: a start written inside the retrace waits a frame");
        g_fake_us = T + 3*F + 500; vid.dirty=1; vdd_bus_frame(&bus);
        CHECK(vid.crtc_start_live==0x0200, "clocked: ...and is shown on the one after");
        /* Pel panning moves planar pixels: pan 1 shows the byte's SECOND pixel first. */
        memset(vid.plane[0], 0, 0x400); memset(vid.plane[1], 0, 0x400);
        memset(vid.plane[2], 0, 0x400); memset(vid.plane[3], 0, 0x400);
        vid.plane[0][0x200] = 0x40;                                    /* pixel 1 = colour 1 */
        vid.disp_pan = 1; vid.time_us = 0; vid.attr_reg[0x13] = 1;
        vid.dirty=1; vdd_bus_frame(&bus);
        CHECK(vid.fb[0]==1 && vid.fb[1]==0, "planar: AR13=1 shifts the picture left one pixel");
        vid.attr_reg[0x13] = 0; vid.dirty=1; vdd_bus_frame(&bus);
        CHECK(vid.fb[0]==0 && vid.fb[1]==1, "planar: AR13=0 shows it where it is");
        v = 0x0C; vdd_bus_io(&bus,0x3D4,1,0,&v); v = 0x00; vdd_bus_io(&bus,0x3D5,1,0,&v);
        v = 0x0D; vdd_bus_io(&bus,0x3D4,1,0,&v); v = 0x00; vdd_bus_io(&bus,0x3D5,1,0,&v);
        vid.dirty=1; vdd_bus_frame(&bus); }

    /* T-OWED: A SCANLINE COUNTER MUST NOT SKIP LINES BECAUSE OUR PORT IS SLOW -------
     * Lemmings' HP calibration counts 320 hblanks on bit 0 (`wait while set; wait
     * while clear` per line) against the 8254, and the tick that count programs is
     * what places its palette split at row 160. A poll slower than the 6.4us hblank
     * must still be told about every line it crossed -- once -- or the count runs
     * long (measured on the rig: 320..383 lines) and the split lands too low.    */
    { uint32_t v; uint64_t t; int it;
      vid.time_us = fake_clock; vid.gh = 400;               /* 70 Hz, 31.8us lines */
      crtc_w(&bus, 0x06, 0xBF); crtc_w(&bus, 0x07, 0x1F); crtc_w(&bus, 0x09, 0x41);
      crtc_w(&bus, 0x12, 0x8F); crtc_w(&bus, 0x15, 0x96);
      t = 0; g_fake_us = 0; vdd_bus_io(&bus, 0x3DA, 1, 1, &v); /* prime: line 0, active */
      for (it = 0; it < 320; ++it) {                        /* the guest's loop, 12us/in */
          do { t += 12; g_fake_us = t; vdd_bus_io(&bus, 0x3DA, 1, 1, &v); } while (v & 1);
          do { t += 12; g_fake_us = t; vdd_bus_io(&bus, 0x3DA, 1, 1, &v); } while (!(v & 1));
      }
      /* 320 lines at 14285us/449 = 10181us; a missed line is +32us. */
      CHECK(t >= 10150 && t <= 10230, "3DA: a 12us poll loop counts EVERY scanline (320 in ~10.18ms)");
      t = 0; g_fake_us = 0; vdd_bus_io(&bus, 0x3DA, 1, 1, &v);
      for (it = 0; it < 320; ++it) {                        /* a 4us poller sees each blank */
          do { t += 4; g_fake_us = t; vdd_bus_io(&bus, 0x3DA, 1, 1, &v); } while (v & 1);
          do { t += 4; g_fake_us = t; vdd_bus_io(&bus, 0x3DA, 1, 1, &v); } while (!(v & 1));
      }
      CHECK(t >= 10150 && t <= 10230, "3DA: ...and a 4us poll loop counts the same 320");
      /* A HOST STALL MID-COUNT: the guest is not polled for 330us (~10 lines) at
         iteration 100. Every line crossed is a blank owed and the count must still
         end at 320 lines' worth of real time -- one-blank repayment left +6 lines
         on the rig (0x310B). */
      t = 0; g_fake_us = 0; vdd_bus_io(&bus, 0x3DA, 1, 1, &v);
      for (it = 0; it < 320; ++it) {
          if (it == 100) t += 330;                             /* the stall          */
          do { t += 12; g_fake_us = t; vdd_bus_io(&bus, 0x3DA, 1, 1, &v); } while (v & 1);
          do { t += 12; g_fake_us = t; vdd_bus_io(&bus, 0x3DA, 1, 1, &v); } while (!(v & 1));
      }
      /* Honest expectation (two exact repayment schemes measured wrong, see status_in):
         the stall is repaid ONE line and never over-repaid -- the count ends between
         320 lines' time and 320 lines + the stall. */
      CHECK(t >= 10150 && t <= 10181 + 330, "3DA: a 330us stall mid-count is repaid one line, never over-repaid");
      { uint32_t a, b; g_fake_us = 100000; vdd_bus_io(&bus, 0x3DA, 1, 1, &a);
        g_fake_us = 100001; vdd_bus_io(&bus, 0x3DA, 1, 1, &b);
        CHECK(a == b, "3DA: two reads in the same line still agree (the rule needs a line boundary)"); }
      /* A gap of a frame or more owes nothing: the next poll reads the true phase. */
      /* A once-a-frame reader (the attribute flip-flop reset) owes nothing: 100 lines
         apart, both polls active -- the owed count must not move. */
      { uint32_t a, before; g_fake_us = 200000 + 5; vdd_bus_io(&bus, 0x3DA, 1, 1, &a);
        before = vid.p3da_hbl_owed;
        g_fake_us = 200000 + 5 + 100 * 14285 / 449; vdd_bus_io(&bus, 0x3DA, 1, 1, &a);
        CHECK(vid.p3da_hbl_owed == before, "3DA: a poll 100 lines after the last owes nothing (not a line counter)"); }
      vid.time_us = 0;
    }

    /* T-SPLIT: A PALETTE WRITE MID-FRAME IS A RASTER SPLIT (s70, Lemmings #1/#2) ---
     * Lemmings rewrites DAC 16..23 twice per frame: once from its timer tick, which
     * is calibrated to land at row 160 (the toolbar's top), and once after the
     * retrace. The rows above 160 must show the frame-start value, the rows from
     * 160 down the mid-frame one -- and that must hold WHATEVER phase the snapshot
     * is taken at, and stop holding once the guest stops doing it. No oracle can
     * pin this (QEMU's default 0x3DA makes the two writes land back to back), so
     * the fake clock and the game's own idiom do.                              */
    { uint32_t v, A, B; ntvdd_frame *f = &vid.frame;
      const uint64_t FR = 1000000u / 70u;                 /* 14285 us: 449 lines */
#define AT(frame, line) ((uint64_t)(frame) * FR + (uint64_t)(line) * FR / 449u)
#define DAC(idx, r, g, bl) do { v=(idx); vdd_bus_io(&bus,0x3C8,1,0,&v); v=(r); vdd_bus_io(&bus,0x3C9,1,0,&v); \
                                v=(g); vdd_bus_io(&bus,0x3C9,1,0,&v); v=(bl); vdd_bus_io(&bus,0x3C9,1,0,&v); } while (0)
      vid.time_us = fake_clock;
      vid.gh = 200;                                       /* 320x200 shown as 400 lines */
      crtc_w(&bus, 0x06, 0xBF); crtc_w(&bus, 0x07, 0x1F); crtc_w(&bus, 0x09, 0x41);
      crtc_w(&bus, 0x12, 0x8F); crtc_w(&bus, 0x15, 0x96);
      g_fake_us = AT(10, 3);   DAC(16, 0x3F, 0x00, 0x00); A = vid.pal[16];   /* row 1: base  */
      /* (321, not 320: AT() and the model both truncate, and 320 lands on 319.99.) */
      g_fake_us = AT(10, 321); DAC(16, 0x00, 0x3F, 0x00); B = vid.pal[16];   /* row 160: split */
      CHECK(A != B && vid.pal_split_row[16] == 160 && vid.pal_base[16] == A && vid.pal_split[16] == B,
            "split: a DAC write at scanline 320 is a split at row 160 over the frame-start value");
      g_fake_us = AT(10, 400); vdd_video_frame_touch(&vid);
      CHECK(ntvdd_frame_has_split(f), "split: the frame reports a live split");
      CHECK(ntvdd_frame_pal_at(f, 100, 16) == A && ntvdd_frame_pal_at(f, 159, 16) == A,
            "split: rows above 160 resolve to the frame-start colour");
      CHECK(ntvdd_frame_pal_at(f, 160, 16) == B && ntvdd_frame_pal_at(f, 199, 16) == B,
            "split: rows from 160 down resolve to the mid-frame colour");
      CHECK(ntvdd_frame_pal_at(f, 100, 17) == vid.pal[17], "split: an entry never split is the live palette");
      /* Next frame, the post-retrace push on row 0, snapshot taken BEFORE this
         frame's tick: the split from the previous frame must still hold. */
      g_fake_us = AT(11, 1);   DAC(16, 0x3F, 0x00, 0x00);
      g_fake_us = AT(11, 200); vdd_video_frame_touch(&vid);
      CHECK(ntvdd_frame_pal_at(f, 100, 16) == A && ntvdd_frame_pal_at(f, 180, 16) == B,
            "split: phase-independent -- a snapshot before this frame's tick still shows both");
      /* A jittering tick: the next frames' writes land on rows 165 and 158. The
         boundary must STAY at 160 (IRQ jitter is ours, not the guest's). */
      g_fake_us = AT(12, 1);   DAC(16, 0x3F, 0x00, 0x00);
      g_fake_us = AT(12, 331); DAC(16, 0x00, 0x3F, 0x00);      /* row 165 */
      CHECK(vid.pal_split_row[16] == 160, "split: a write 5 rows off keeps the boundary at 160 (sticky)");
      g_fake_us = AT(13, 1);   DAC(16, 0x3F, 0x00, 0x00);
      g_fake_us = AT(13, 317); DAC(16, 0x00, 0x3F, 0x00);      /* row 158 */
      CHECK(vid.pal_split_row[16] == 160, "split: ...and 2 rows the other way too");
      g_fake_us = AT(13, 400); vdd_video_frame_touch(&vid);
      CHECK(ntvdd_frame_pal_at(f, 159, 16) == A && ntvdd_frame_pal_at(f, 160, 16) == B,
            "split: rows 159/160 still resolve either side of the sticky boundary");
      /* The guest stops splitting: two frames later it is a single palette again. */
      g_fake_us = AT(17, 100); vdd_video_frame_touch(&vid);
      CHECK(!ntvdd_frame_has_split(f) && ntvdd_frame_pal_at(f, 180, 16) == vid.pal[16],
            "split: expires two frames after the guest stops -- the DAC simply holds");
      /* A genuinely different row (a new effect at row 60) does move it. */
      g_fake_us = AT(18, 1);   DAC(16, 0x3F, 0x00, 0x00);
      g_fake_us = AT(18, 121); DAC(16, 0x00, 0x3F, 0x00);      /* row 60 */
      CHECK(vid.pal_split_row[16] == 60, "split: a write far from the old boundary moves it");
      /* A write during blanking is the frame's base, not a split. */
      g_fake_us = AT(20, 420); DAC(16, 0x00, 0x00, 0x3F);
      CHECK(vid.pal_base[16] == vid.pal[16] && !ntvdd_frame_has_split(f),
            "split: a write in vertical blanking is the next frame's base");
      vid.time_us = 0;
#undef AT
#undef DAC
    }

    /* ── T21: ★ TEXT-MODE FIDELITY FOR FULL-SCREEN APPLICATIONS (edit.com, QBasic). ──
       Five things a text-mode UI relies on and none of which existed: the BDA display
       fields, bright-vs-blink backgrounds, the 43/50-line font calls, the CRTC cursor
       registers, and the mouse driver's inverted-cell text cursor. Each is checked as a
       RENDER or a byte in guest memory, not as a return code. */
    {   static uint8_t tbda[0x100];
        int gy, gx;
        memset(tbda, 0, sizeof tbda);
        vid.bda = tbda;
        memset(&r,0,sizeof r); s_ah(&r,0x00); s_al(&r,0x03); vdd_bus_deliver_int(&bus,0x10,&r);

        /* a. the BDA describes the display, and a text app reads it rather than asking */
        CHECK(tbda[0x49]==3 && tbda[0x4A]==80 && tbda[0x4B]==0 && tbda[0x84]==24 &&
              tbda[0x85]==16 && tbda[0x63]==0xD4 && tbda[0x64]==0x03,
              "bda: mode 3 -> 0449=03 044A=80 0484=24 (rows-1) 0485=16 0463=3D4h");
        CHECK((tbda[0x89] & 0x01) && (tbda[0x87] & 0x60) == 0x60,
              "bda: 0489 says VGA active at 400 lines, 0487 says 256K EGA/VGA");
        memset(&r,0,sizeof r); s_ah(&r,0x02); s_dx(&r,(uint16_t)((7<<8)|12)); vdd_bus_deliver_int(&bus,0x10,&r);
        CHECK(tbda[0x50]==12 && tbda[0x51]==7, "bda: INT 10h AH=02 lands in 0450 as (col,row)");
        memset(&r,0,sizeof r); s_ah(&r,0x01); s_cx(&r,0x0007); vdd_bus_deliver_int(&bus,0x10,&r);
        CHECK(tbda[0x60]==0x07 && tbda[0x61]==0x00, "bda: INT 10h AH=01 lands in 0460 as (end,start)");

        /* b. attribute bit 7: BLINK by default, BRIGHT BACKGROUND after 1003h BL=0 */
        memset(&r,0,sizeof r); s_ah(&r,0x02); s_dx(&r,0); vdd_bus_deliver_int(&bus,0x10,&r);
        memset(&r,0,sizeof r); s_ah(&r,0x09); s_al(&r,' '); s_bx(&r,0xF0); s_cx(&r,1); vdd_bus_deliver_int(&bus,0x10,&r);
        memset(&r,0,sizeof r); s_ah(&r,0x02); s_dx(&r,(uint16_t)((24<<8)|79)); vdd_bus_deliver_int(&bus,0x10,&r);
        CHECK(vid.blink == 1, "blink: a mode set enables blink (the power-on default)");
        vdd_video_render(&vid);
        CHECK(vid.fb[0] == 7, "blink on: attribute F0h draws background 7 (bit 7 is blink, not bright)");
        memset(&r,0,sizeof r); s_ax(&r,0x1003); s_bx(&r,0x0000); vdd_bus_deliver_int(&bus,0x10,&r);
        CHECK(vid.blink == 0, "int10/1003 BL=0: blink off");
        vdd_video_render(&vid);
        CHECK(vid.fb[0] == 15, "blink off: attribute F0h draws background 15 (sixteen backgrounds)");
        memset(&r,0,sizeof r); s_ax(&r,0x1003); s_bx(&r,0x0001); vdd_bus_deliver_int(&bus,0x10,&r);
        txt(0,0)[0]='A'; txt(0,0)[1]=0x87;                /* blinking grey on black */
        vid.time_us = fake_clock;
        g_fake_us = 100000; vdd_video_render(&vid);
        { const uint8_t *gl = vga_font_8x16['A']; int lit = 0;
          for (gy=0;gy<16;++gy) for (gx=0;gx<8;++gx)
              if ((gl[gy]&(0x80>>gx)) && vid.fb[gy*640+gx]==7) lit++;
          CHECK(lit > 0, "blink: in the on phase the glyph is drawn"); }
        g_fake_us = 700000; vdd_video_render(&vid);
        { int any = 0; for (gy=0;gy<16;++gy) for (gx=0;gx<8;++gx) if (vid.fb[gy*640+gx]!=0) any++;
          CHECK(any == 0, "blink: in the off phase the glyph hides (fg == bg)"); }
        vid.time_us = 0; txt(0,0)[1]=0x07;

        /* c. 1112h IS the 50-line call: 8x8 ROM font, 400/8 rows, an 8x8 render */
        memset(&r,0,sizeof r); s_ax(&r,0x1112); s_bx(&r,0); vdd_bus_deliver_int(&bus,0x10,&r);
        CHECK(vid.rows == 50 && vid.cell_h == 8, "int10/1112: the 8x8 ROM font gives 50 rows");
        CHECK((r_dx(&r) & 0xFF) == 49, "int10/1112: DL = rows-1 = 49");
        CHECK(tbda[0x84]==49 && tbda[0x85]==8, "bda: 0484=49 0485=8 after 1112h");
        vid.dirty=1; vdd_bus_frame(&bus);
        CHECK(vid.frame.w==640 && vid.frame.h==400, "frame(50-line): still 640x400");
        txt(49,0)[0]='A'; txt(49,0)[1]=0x0F;
        vdd_video_render(&vid);
        { const uint8_t *gl = vga_font_8x8['A']; int mism = 0;
          for (gy=0;gy<8;++gy) for (gx=0;gx<8;++gx) {
              uint8_t e=(gl[gy]&(0x80>>gx))?15:0; if (vid.fb[(392+gy)*640+gx]!=e) mism++; }
          CHECK(mism==0, "render(50-line): row 49 is an 8x8 ROM glyph at scan line 392"); }
        memset(&r,0,sizeof r); s_ah(&r,0x11); s_al(&r,0x30); s_bx(&r,0x0100); vdd_bus_deliver_int(&bus,0x10,&r);
        CHECK(r_cx(&r) == 8 && (r_dx(&r)&0xFF) == 49, "int10/1130 BH=1: the CURRENT font is 8x8, 50 rows");
        memset(&r,0,sizeof r); s_ax(&r,0x1114); s_bx(&r,0); vdd_bus_deliver_int(&bus,0x10,&r);
        CHECK(vid.rows == 25 && vid.cell_h == 16, "int10/1114: the 8x16 ROM font gives 25 rows again");
        memset(&r,0,sizeof r); s_ax(&r,0x1102); s_bx(&r,0); vdd_bus_deliver_int(&bus,0x10,&r);
        CHECK(vid.rows == 25 && vid.cell_h == 16, "int10/1102: the 0x variant changes glyphs only, not rows");
        memset(&r,0,sizeof r); s_ax(&r,0x1111); s_bx(&r,0); vdd_bus_deliver_int(&bus,0x10,&r);
        CHECK(vid.rows == 28 && vid.cell_h == 14, "int10/1111: the 8x14 ROM font gives 28 rows");

        /* d. the cursor programmed straight into the CRTC (every CRT unit does this) */
        memset(&r,0,sizeof r); s_ah(&r,0x00); s_al(&r,0x03); vdd_bus_deliver_int(&bus,0x10,&r);
        crtc_w(&bus, 0x0E, (uint8_t)((3*80+5) >> 8)); crtc_w(&bus, 0x0F, (uint8_t)((3*80+5) & 0xFF));
        CHECK(vid.cur_row == 3 && vid.cur_col == 5, "crtc 0E/0F: cursor address 3*80+5 -> row 3 col 5");
        CHECK(tbda[0x50]==5 && tbda[0x51]==3, "crtc 0E/0F: ...and the BDA follows");
        { uint32_t v = 0, idx = 0x0E; vdd_bus_io(&bus,0x3D4,1,0,&idx); vdd_bus_io(&bus,0x3D5,1,1,&v);
          CHECK(v == ((3*80+5)>>8), "crtc 0E: reads back the cursor address high byte"); }
        crtc_w(&bus, 0x0A, 0x20);
        { unsigned s0,e0; int hid; vdd_cursor_lines(vid.cur_shape, 16, &s0,&e0,&hid);
          CHECK(hid, "crtc 0A: bit 5 hides the cursor"); }
        crtc_w(&bus, 0x0A, 0x06); crtc_w(&bus, 0x0B, 0x07);
        CHECK(vid.cur_shape == 0x0607, "crtc 0A/0B: start/end become the INT 10h shape word");
        crtc_w(&bus, 0x0E, 0x7F); crtc_w(&bus, 0x0F, 0xFF);
        CHECK(vid.cur_row >= vid.rows, "crtc 0E/0F: an off-page address (7FFFh) hides the cursor");

        /* e. the INT 33h text cursor: the cell under the pointer, attribute masked */
        memset(&r,0,sizeof r); s_ah(&r,0x00); s_al(&r,0x03); vdd_bus_deliver_int(&bus,0x10,&r);
        memset(&r,0,sizeof r); s_ah(&r,0x02); s_dx(&r,(uint16_t)((24<<8)|79)); vdd_bus_deliver_int(&bus,0x10,&r);
        txt(2,3)[0]='X'; txt(2,3)[1]=0x1F;                /* white on blue          */
        vdd_video_render(&vid);
        vdd_video_text_cursor(&vid, 3, 2, 0x77FF, 0x7700); /* the driver's defaults  */
        { const uint8_t *gl = vga_font_8x16['X']; int mism = 0;
          for (gy=0;gy<16;++gy) for (gx=0;gx<8;++gx) {
              uint8_t e=(gl[gy]&(0x80>>gx))?0:6; if (vid.fb[(2*16+gy)*640+3*8+gx]!=e) mism++; }
          CHECK(mism==0, "int33 text cursor: cell (3,2) redrawn with (1F & 77) ^ 77 = 60h: black on brown"); }
        CHECK(txt(2,3)[1] == 0x1F, "int33 text cursor: VRAM itself is untouched (no trail)");
        vdd_video_text_cursor(&vid, 80, 2, 0x77FF, 0x7700);
        vdd_video_text_cursor(&vid, -1, 2, 0x77FF, 0x7700);
        CHECK(1, "int33 text cursor: out-of-range cells are ignored, not written");
        memset(&r,0,sizeof r); s_ah(&r,0x00); s_al(&r,0x13); vdd_bus_deliver_int(&bus,0x10,&r);
        vdd_video_text_cursor(&vid, 3, 2, 0x77FF, 0x7700);
        CHECK(vid.mkind != VID_KIND_TEXT, "int33 text cursor: a no-op outside text modes");

        /* f. a 40-column mode renders at ITS stride, which is what the frame declares */
        memset(&r,0,sizeof r); s_ah(&r,0x00); s_al(&r,0x01); vdd_bus_deliver_int(&bus,0x10,&r);
        memset(&r,0,sizeof r); s_ah(&r,0x02); s_dx(&r,(uint16_t)((24<<8)|39)); vdd_bus_deliver_int(&bus,0x10,&r);
        txt(1,0)[0]='A'; txt(1,0)[1]=0x0F;
        vdd_video_render(&vid);
        { const uint8_t *gl = vga_font_8x16['A']; int mism = 0;
          for (gy=0;gy<16;++gy) for (gx=0;gx<8;++gx) {
              uint8_t e=(gl[gy]&(0x80>>gx))?15:0; if (vid.fb[(16+gy)*320+gx]!=e) mism++; }
          CHECK(mism==0, "render(40-col): row 1 is at stride 320 (was drawn at 640: every other line)"); }
        CHECK(tbda[0x4A]==40 && tbda[0x49]==1, "bda: mode 1 -> 40 columns");
        memset(&r,0,sizeof r); s_ah(&r,0x00); s_al(&r,0x03); vdd_bus_deliver_int(&bus,0x10,&r);
        vid.bda = 0;
    }

    /* THE TWO EXTERNAL READ-ONLY REGISTERS. docs/ref/vga.md 3.
       ⛔ INPUT STATUS 0 WAS 0x00 FOR THREE SESSIONS AND WAS TWICE RECORDED AS
         "confirmed" on the strength of a single oracle. It is 0x10 -- bit 4,
         Switch Sense -- measured on PCem's genuine IBM VGA ROM in all twelve
         modes p_vgareg sets, and dosbox-x drives bit 4 too. Pinned here so the
         value has a check of its own rather than living only in a switch arm.
       ★ BIT 7, the CRT interrupt, reads 0 HERE because the mode's CR11 holds it
         clear (bit 4 = 0), as every BIOS mode does; with it enabled it latches at
         retrace -- #187, pinned just below. */
    {
        uint32_t v;
        vdd_bus_io(&bus, 0x3C2, 1, 1, &v);
        CHECK(v == 0x10, "ext: Input Status 0 reads 0x10 -- bit 4 Switch Sense");
        CHECK((v & 0x80) == 0, "ext: ...and bit 7, the CRT interrupt, stays low");

        /* #187: BIT 7 IS THE VERTICAL-RETRACE INTERRUPT LATCH (IBM VGA). CR11 bit 5 = 0
           enables, bit 4 = 0 clears and holds clear; the first retrace start after it
           is armed sets it. 70 Hz: F = 14285 us, retrace from line 400 (~12726 us). */
        {   const uint64_t F = 14285u, T = 2000u * 14285u;
            uint32_t cr11, w;
            uint64_t (*old_clock)(void) = vid.time_us;
            uint32_t old_gh = vid.gh;
            vid.time_us = fake_clock; vid.gh = 200;
            w = 0x11; vdd_bus_io(&bus,0x3D4,1,0,&w); vdd_bus_io(&bus,0x3D5,1,1,&cr11);
            g_fake_us = T + 1000;                                   /* in the picture */
            w = (cr11 & 0x4F) | 0x10; vdd_bus_io(&bus,0x3D5,1,0,&w);   /* enable, not held */
            g_fake_us = T + 2000;  vdd_bus_io(&bus, 0x3C2, 1, 1, &v);
            CHECK((v & 0x80) == 0, "vint: armed in the picture -- no retrace yet, bit 7 low");
            g_fake_us = T + 13000; vdd_bus_io(&bus, 0x3C2, 1, 1, &v);
            CHECK(v == 0x90, "vint: the retrace start sets bit 7 (0x90)");
            g_fake_us = T + F + 1000; vdd_bus_io(&bus, 0x3C2, 1, 1, &v);
            CHECK(v == 0x90, "vint: ...and it stays latched into the next picture");
            w = cr11 & 0x4F; vdd_bus_io(&bus,0x3D5,1,0,&w);            /* bit 4 = 0: clear */
            g_fake_us = T + 2*F + 13000; vdd_bus_io(&bus, 0x3C2, 1, 1, &v);
            CHECK(v == 0x10, "vint: CR11 bit 4 = 0 clears it and holds it clear");
            g_fake_us = T + 2*F + 13500;                             /* inside a retrace */
            w = (cr11 & 0x4F) | 0x10; vdd_bus_io(&bus,0x3D5,1,0,&w);
            g_fake_us = T + 2*F + 14000; vdd_bus_io(&bus, 0x3C2, 1, 1, &v);
            CHECK((v & 0x80) == 0, "vint: re-armed mid-retrace -- that retrace began before it");
            g_fake_us = T + 3*F + 13000; vdd_bus_io(&bus, 0x3C2, 1, 1, &v);
            CHECK(v == 0x90, "vint: ...the next retrace sets it");
            w = (cr11 & 0x4F) | 0x30; vdd_bus_io(&bus,0x3D5,1,0,&w);   /* bit 5 = 1: disabled */
            w = cr11 & 0x4F;          vdd_bus_io(&bus,0x3D5,1,0,&w);   /* clear it */
            w = (cr11 & 0x4F) | 0x30; vdd_bus_io(&bus,0x3D5,1,0,&w);
            g_fake_us = T + 5*F + 13000; vdd_bus_io(&bus, 0x3C2, 1, 1, &v);
            CHECK(v == 0x10, "vint: disabled (bit 5 = 1), a retrace sets nothing");
            vdd_bus_io(&bus,0x3D5,1,0,&cr11);                           /* put CR11 back */
            vid.time_us = old_clock; vid.gh = old_gh;
            vdd_bus_io(&bus, 0x3C2, 1, 1, &v);
        }

        /* Feature Control is storage: write at 3DA, read back at 3CA. NO ORACLE
           CAN ADJUDICATE THIS -- 6.22 and dosbox-x both read 0x00 whatever is
           written, and PCem reads 0xFF, which is an undecoded port rather than a
           measurement. The IBM VGA spec says it reads back, so it reads back. */
        v = 0x0F; vdd_bus_io(&bus, 0x3DA, 1, 0, &v);
        vdd_bus_io(&bus, 0x3CA, 1, 1, &v);
        CHECK(v == 0x0F, "ext: Feature Control written at 3DA reads back at 3CA");
        v = 0x00; vdd_bus_io(&bus, 0x3DA, 1, 0, &v);
    }

    /* A MODE SET MUST LEAVE THE SHADOWS AND THE REGISTER FILE SAYING THE SAME THING.
       docs/inventory/vga.md step 5. vga_load_modedef filled `*_reg[]` from the
       measured table, but six registers are not read back from there at all -- the
       port answers from a live shadow, because the shadow is what the engine uses.
       So the file was right and the guest still saw the old value. Measured on the
       rig and reproduced here: mode 3 answered SR2=0F, CR0A/0B=06/07, GR5=00,
       AR10=00. Five registers; 55 of the 55 bytes of the VGA parity gap.
       ⚠ THESE EXPECTATIONS COME FROM VGA_MODEDEFS, i.e. from two oracles, not from
         a datasheet or from this file's own idea of a VGA. */
    {
        uint32_t v; ntvdd_regs rr;
        struct { const char *n; uint16_t ip, dp; uint8_t idx, want; } t3[] = {
            { "SR02", 0x3C4, 0x3C5, 0x02, 0x03 },
            { "CR0A", 0x3D4, 0x3D5, 0x0A, 0x0D },
            { "CR0B", 0x3D4, 0x3D5, 0x0B, 0x0E },
            { "GR05", 0x3CE, 0x3CF, 0x05, 0x10 },
        };
        unsigned k;
        memset(&rr, 0, sizeof rr); s_ah(&rr, 0x00); s_al(&rr, 0x03);
        vdd_bus_deliver_int(&bus, 0x10, &rr);
        for (k = 0; k < sizeof t3 / sizeof t3[0]; ++k) {
            char msg[80];
            v = t3[k].idx; vdd_bus_io(&bus, t3[k].ip, 1, 0, &v);
            v = 0;         vdd_bus_io(&bus, t3[k].dp, 1, 1, &v);
            sprintf(msg, "modedef: mode 3 leaves %s = 0x%02X, as a real BIOS does",
                    t3[k].n, t3[k].want);
            CHECK(v == t3[k].want, msg);
        }
        vdd_bus_io(&bus, 0x3DA, 1, 1, &v);                   /* reset the AC flip-flop */
        v = 0x10; vdd_bus_io(&bus, 0x3C0, 1, 0, &v);
        v = 0;    vdd_bus_io(&bus, 0x3C1, 1, 1, &v);
        CHECK(v == 0x0C, "modedef: mode 3 leaves AR10 = 0x0C (line graphics + blink)");
        CHECK(vid.cur_shape == 0x0D0E,
              "modedef: the cursor shape is CR0A/CR0B, 0x0D0E for an 8x16 cell");

        /* ⚠ GR7 IS THE BYTE THE TWO ORACLES SPLIT ON, and the split is not noise:
             both say 0x0F in the GRAPHICS modes, and in the text and CGA modes QEMU
             says 0x0F where PCem's real IBM VGA ROM says 0x00. Taking the table
             rather than a constant is the whole point -- generating this from QEMU
             alone would have written 0x0F into the text modes AGAINST the real card
             and it would have LOOKED like a fix, because parity would have moved. */
        v = 0x07; vdd_bus_io(&bus, 0x3CE, 1, 0, &v);
        v = 0;    vdd_bus_io(&bus, 0x3CF, 1, 1, &v);
        CHECK(v == 0x00, "modedef: mode 3 GR7 = 0x00 -- PCem's answer, not QEMU's 0x0F");

        memset(&rr, 0, sizeof rr); s_ah(&rr, 0x00); s_al(&rr, 0x12);
        vdd_bus_deliver_int(&bus, 0x10, &rr);
        v = 0x07; vdd_bus_io(&bus, 0x3CE, 1, 0, &v);
        v = 0;    vdd_bus_io(&bus, 0x3CF, 1, 1, &v);
        CHECK(v == 0x0F, "modedef: mode 12h GR7 = 0x0F -- both oracles agree there");
        /* ...and the per-kind arm still wins where it has to: planar forces the
           map mask to all four planes after the table has been loaded. */
        v = 0x02; vdd_bus_io(&bus, 0x3C4, 1, 0, &v);
        v = 0;    vdd_bus_io(&bus, 0x3C5, 1, 1, &v);
        CHECK(v == 0x0F, "modedef: mode 12h keeps map_mask 0x0F -- the planar arm wins");

        memset(&rr, 0, sizeof rr); s_ah(&rr, 0x00); s_al(&rr, 0x03);
        vdd_bus_deliver_int(&bus, 0x10, &rr);
    }

    /* T#252: THE CHARACTER SERVICES IN THE GRAPHICS MODES, PAGES, AND AH=12h. Every
       expectation is a byte PCem's genuine IBM VGA ROM wrote in p_vidtxt (DOSBox-X
       agrees on each), re-derived here from the ROM font so a font edit cannot hide a
       layout bug. Before #252 all of these failed: the glyphs went to B800:0 as
       (char, attr) pairs, 05h never moved the CRTC, and 12h said "supported" to all. */
    {
        ntvdd_regs rr; static uint8_t tb[0x100]; int y, ok; uint32_t v;
        vid.bda = tb;
#define I10(ax_, bx_, cx_, dx_) do { memset(&rr, 0, sizeof rr); rr.eax = (ax_); rr.ebx = (bx_); \
            rr.ecx = (cx_); rr.edx = (dx_); vdd_bus_deliver_int(&bus, 0x10, &rr); } while (0)
        /* mode 13h: 'A' in colour 0Eh over a 55h fill -> glyph bits 0Eh, the rest 00h */
        I10(0x0013, 0, 0, 0);
        memset(g_vmem, 0x55, 8 * 320);
        I10(0x0941, 0x000E, 1, 0);
        for (ok = 1, y = 0; y < 8; ++y) { int x;
            for (x = 0; x < 8; ++x)
                if (g_vmem[y * 320 + x] != ((vga_font_8x8['A'][y] & (0x80 >> x)) ? 0x0E : 0x00)) ok = 0; }
        CHECK(ok, "#252 13h: AH=09h draws the 8x8 glyph at A000:0, background 00h (was (char,attr) at B800:0)");
        CHECK(g_vmem[VID_TEXT_OFF] != 'A' || g_vmem[VID_TEXT_OFF + 1] != 0x0E,
              "#252 13h: ...and nothing is written to B800:0 as a text cell");
        I10(0x0941, 0x008E, 1, 0);           /* bit 7 is part of the colour in 13h, not XOR */
        CHECK(g_vmem[2] == 0x8E && g_vmem[0] == 0x00, "#252 13h: BL=8Eh is the colour 8Eh (no XOR in 256 colours -- PCem)");
        I10(0x0200, 0, 0, 0x0001);
        I10(0x0E42, 0x000C, 0, 0);
        CHECK(g_vmem[8 + 0] == ((vga_font_8x8['B'][0] & 0x80) ? 0x0C : 0) && vid.cur_col == 2,
              "#252 13h: AH=0Eh draws 'B' at column 1 in BL and advances the cursor to 2");
        I10(0x0200, 0, 0, 0x0001);
        I10(0x0800, 0, 0, 0);
        CHECK((rr.eax & 0xFF) == 'B', "#252 13h: AH=08h reads 'B' back from the pixels");
        /* 06h in 13h: row 1 moves up to row 0, row 1 filled with BH */
        I10(0x0013, 0, 0, 0);
        I10(0x0200, 0, 0, 0x0100);
        I10(0x0958, 0x000F, 1, 0);
        I10(0x0601, 0x0700, 0, 0x0127);
        CHECK(g_vmem[1] == ((vga_font_8x8['X'][0] & 0x40) ? 0x0F : 0) && g_vmem[8 * 320] == 0x07
              && g_vmem[8 * 320 + 319] == 0x07,
              "#252 13h: AH=06h scrolls pixels by a character row and fills with BH");
        /* mode 04h: two bits a pixel across the interleaved banks */
        I10(0x0004, 0, 0, 0);
        I10(0x0941, 0x0003, 1, 0);
        for (ok = 1, y = 0; y < 8; ++y) {
            uint32_t off = VID_TEXT_OFF + ((y & 1) ? 0x2000u : 0u) + (uint32_t)(y >> 1) * 80u;
            uint16_t w = 0; int k;
            for (k = 0; k < 8; ++k) if (vga_font_8x8['A'][y] & (0x80 >> k)) w |= (uint16_t)(3u << (14 - 2 * k));
            if (g_vmem[off] != (uint8_t)(w >> 8) || g_vmem[off + 1] != (uint8_t)w) ok = 0;
        }
        CHECK(ok, "#252 04h: AH=09h draws 'A' two bits a pixel, even lines at B800:0, odd at B800:2000");
        I10(0x0941, 0x0083, 1, 0);
        CHECK(g_vmem[VID_TEXT_OFF] == 0 && g_vmem[VID_TEXT_OFF + 0x2000] == 0, "#252 04h: BL bit 7 XORs it off again");
        I10(0x0C02, 0, 5, 5);
        I10(0x0D00, 0, 5, 5);
        CHECK(g_vmem[VID_TEXT_OFF + 0x2000 + 2 * 80 + 1] == 0x20 && (rr.eax & 0xFF) == 2,
              "#252 04h: AH=0Ch/0Dh write and read pixel (5,5) = 2 -- byte 20h at B800:20A1 (was not written, read 0)");
        /* mode 0Dh: planes, the 8x8 cell, 40 bytes a line, BL bits 4-6 not a background */
        I10(0x000D, 0, 0, 0);
        I10(0x0200, 0, 0, 0x0001);
        I10(0x0941, 0x001E, 1, 0);
        for (ok = 1, y = 0; y < 8; ++y) {
            if (vid.plane[0][y * 40 + 1] != 0) ok = 0;
            if (vid.plane[1][y * 40 + 1] != vga_font_8x8['A'][y]) ok = 0;
            if (vid.plane[3][y * 40 + 1] != vga_font_8x8['A'][y]) ok = 0;
        }
        CHECK(ok, "#252 0Dh: 'A' in colour Eh at 40 bytes a line, 8 lines; plane 0 stays 0 (BL bit 4 is not a background)");
        /* 05h: page 1 of 0Dh = CRTC start 2000h, and a write to page 1 lands there */
        I10(0x0501, 0, 0, 0);
        v = 0x0C; vdd_bus_io(&bus, 0x3D4, 1, 0, &v); v = 0; vdd_bus_io(&bus, 0x3D5, 1, 1, &v);
        CHECK(v == 0x20 && vid.crtc_start_live == 0x2000, "#252 0Dh: AH=05h AL=1 loads the CRTC start with 2000h (bytes)");
        I10(0x0500, 0, 0, 0);
        /* mode 10h: the 8x14 cell at 80 bytes a line */
        I10(0x0010, 0, 0, 0);
        I10(0x0200, 0, 0, 0x0101);
        I10(0x0941, 0x000E, 1, 0);
        for (ok = 1, y = 0; y < 14; ++y)
            if (vid.plane[1][(14 + y) * 80 + 1] != vga_font_8x14['A'][y]) ok = 0;
        CHECK(ok, "#252 10h: 'A' is the 8x14 glyph at row 1 (line 14), 80 bytes a line (was 8x16 at 640/8 of 480)");
        /* mode 3 pages */
        I10(0x0003, 0, 0, 0);
        I10(0x0501, 0, 0, 0);
        v = 0x0C; vdd_bus_io(&bus, 0x3D4, 1, 0, &v); v = 0; vdd_bus_io(&bus, 0x3D5, 1, 1, &v);
        CHECK(v == 0x08, "#252 03h: AH=05h AL=1 loads the CRTC start with 0800h (words)");
        I10(0x0200, 0x0100, 0, 0x0203);
        I10(0x0300, 0x0000, 0, 0);
        CHECK((rr.edx & 0xFFFF) == 0x0000, "#252 03h: page 0's cursor is its own (BH=0 reads 0000 while page 1's is 0203)");
        I10(0x095A, 0x011F, 1, 0);
        CHECK(g_vmem[VID_TEXT_OFF + 0x1000 + (2 * 80 + 3) * 2] == 'Z' && g_vmem[VID_TEXT_OFF + (2 * 80 + 3) * 2] != 'Z',
              "#252 03h: AH=09h BH=1 writes page 1 (B900) at page 1's cursor, not page 0");
        I10(0x0E51, 0x0007, 0, 0);
        CHECK(g_vmem[VID_TEXT_OFF + 0x1000 + (2 * 80 + 3) * 2] == 'Q',
              "#252 03h: AH=0Eh writes the ACTIVE page whatever BH says (PCem's IBM ROM)");
        g_vmem[VID_TEXT_OFF + (2 * 80 + 3) * 2] = 'W';      /* page 0, same cell: must NOT show */
        g_vmem[VID_TEXT_OFF + (2 * 80 + 3) * 2 + 1] = 0x1F;
        vdd_video_render(&vid);
        for (ok = 1, y = 0; y < 16; ++y) { int x;
            for (x = 0; x < 8; ++x)
                if (vid.fb[(2 * 16 + y) * 640 + 3 * 8 + x] != ((vga_font_8x16['Q'][y] & (0x80 >> x)) ? 0x0F : 0x01)) ok = 0; }
        CHECK(ok, "#252 03h: the renderer SHOWS page 1 (from the CRTC start) -- 'Q' in 1Fh at row 2, column 3");
        I10(0x0500, 0, 0, 0);
        /* AH=12h */
        I10(0x1201, 0x0030, 0, 0);
        CHECK((rr.eax & 0xFF) == 0x12, "#252 12h BL=30h: AL=12h");
        I10(0x0003, 0, 0, 0);
        CHECK(vid.cell_h == 14 && tb[0x85] == 14 && tb[0x84] == 24 && tb[0x89] == 0x01,
              "#252 12h BL=30h AL=1, then mode 3: 8x14 cell, 25 rows, 0040:0089 = 01h (PCem)");
        I10(0x1202, 0x0030, 0, 0);
        I10(0x0003, 0, 0, 0);
        CHECK(vid.cell_h == 16, "#252 12h BL=30h AL=2: back to 400 lines");
        I10(0x1277, 0x0055, 0, 0);
        CHECK((rr.eax & 0xFF) == 0x00, "#252 12h unknown BL: AL=00h (PCem's IBM ROM) -- not 12h");
        I10(0x1201, 0x0033, 0, 0);
        CHECK((rr.eax & 0xFF) == 0x12 && vid.grey_sum == 0, "#252 12h BL=33h AL=1: summing off, AL=12h");
        I10(0x1200, 0x0033, 0, 0);
        I10(0x0013, 0, 0, 0);
        { uint32_t c = vid.dac[1]; CHECK(((c >> 16) & 0xFF) == ((c >> 8) & 0xFF) && ((c >> 8) & 0xFF) == (c & 0xFF),
              "#252 12h BL=33h AL=0: the next mode set's DAC comes up grey"); }
        I10(0x1201, 0x0033, 0, 0);
        I10(0x0003, 0, 0, 0);
        I10(0x1201, 0x0034, 0, 0);
        CHECK((rr.eax & 0xFF) == 0x12 && vid.cur_emul_off == 1 && (tb[0x87] & 1), "#252 12h BL=34h AL=1: emulation off, 0040:0087 bit 0");
        I10(0x1200, 0x0034, 0, 0);
        I10(0x1203, 0x0036, 0, 0);
        CHECK((rr.eax & 0xFF) == 0x00, "#252 12h BL=36h AL=3: out of range, refused");

        /* ── T#266: INT 10h AFTER #252's REMAINDERS. ─────────────────────────────────
             The pure arithmetic first: the CGA colour-select byte and what it becomes in
             the attribute controller. The anchor is the MEASURED mode 04h table: from
             the mode set's own 0066 = 30h the arithmetic must give AR01-03 = 13h/15h/17h
             (vga_modedefs.h, PCem's IBM ROM) -- or the formula is wrong, not the card. */
        { uint8_t a[3];
          vdd_cga_pal_ar(0x30, a);
          CHECK(a[0] == 0x13 && a[1] == 0x15 && a[2] == 0x17,
                "#266 0Bh: 0066=30h (palette 1, intensity) -> AR01-03 13/15/17 = the measured mode 04h table");
          vdd_cga_pal_ar(0x10, a);
          CHECK(a[0] == 0x12 && a[1] == 0x14 && a[2] == 0x16, "#266 0Bh: palette 0 + intensity -> 12/14/16 (light green/red/yellow)");
          vdd_cga_pal_ar(0x20, a);
          CHECK(a[0] == 0x03 && a[1] == 0x05 && a[2] == 0x07, "#266 0Bh: palette 1, no intensity -> 03/05/07 (cyan/magenta/white)");
          vdd_cga_pal_ar(0x00, a);
          CHECK(a[0] == 0x02 && a[1] == 0x04 && a[2] == 0x06, "#266 0Bh: palette 0, no intensity -> 02/04/06 (green/red/brown)"); }
        CHECK(vdd_cga_bg_ar(0x04) == 0x04 && vdd_cga_bg_ar(0x0C) == 0x14 && vdd_cga_bg_ar(0x1F) == 0x17,
              "#266 0Bh: BL IRGB -> AC value, intensity (bit 3) to bit 4: 04->04, 0C->14, 1F->17");
        CHECK(vdd_cga_colour_select(0x30, 0, 0x1C) == 0x3C && vdd_cga_colour_select(0x3C, 1, 0x00) == 0x1C
              && vdd_cga_colour_select(0x1C, 1, 0xFF) == 0x3C,
              "#266 0Bh: 0066 -- BH=0 replaces bits 0-4, BH=1 only bit 5 (from BL bit 0)");
        CHECK(vdd_gfx_font_rows(0, 30) == 30 && vdd_gfx_font_rows(1, 0) == 14 && vdd_gfx_font_rows(2, 0) == 25
              && vdd_gfx_font_rows(3, 0) == 43 && vdd_gfx_font_rows(7, 9) == 25,
              "#266 11h/2xh: BL rows -- 0 = DL, 1 = 14, 2 = 25, 3 = 43, other = 25");

        /* ...and through INT 10h, in mode 04h, read back the way a guest does (3C0/3C1). */
        I10(0x0004, 0, 0, 0);
        CHECK(tb[0x66] == 0x30 && vid.vpal[1] == 0x13 && vid.vpal[3] == 0x17, "#266 04h: mode set leaves 0066=30h, AR01/03 13h/17h");
        I10(0x0B00, 0x0100, 0, 0);
        CHECK(vid.vpal[1] == 0x12 && vid.vpal[2] == 0x14 && vid.vpal[3] == 0x16 && tb[0x66] == 0x10,
              "#266 04h: 0Bh BH=1 BL=0 -> palette 0 (12/14/16), intensity kept, 0066=10h");
        I10(0x0B00, 0x0004, 0, 0);
        { uint32_t rv; v = 0x00; vdd_bus_io(&bus, 0x3DA, 1, 1, &rv); vdd_bus_io(&bus, 0x3C0, 1, 0, &v);
          rv = 0; vdd_bus_io(&bus, 0x3C1, 1, 1, &rv);
          CHECK(rv == 0x04 && vid.vpal[16] == 0x04 && tb[0x66] == 0x04
                && vid.vpal[1] == 0x02 && vid.vpal[2] == 0x04 && vid.vpal[3] == 0x06,
                "#266 04h: 0Bh BH=0 BL=04h -> AR00 = AR11 = 04h (read back at 3C1), BL bit 4 clear drops the intensity");
          v = 0x11; vdd_bus_io(&bus, 0x3DA, 1, 1, &rv); vdd_bus_io(&bus, 0x3C0, 1, 0, &v);
          rv = 0; vdd_bus_io(&bus, 0x3C1, 1, 1, &rv);
          CHECK(rv == 0x04, "#266 04h: ...AR11 (border) reads 04h too -- 0Bh used to write a shadow nothing read"); }
        /* The renderer: a pixel's value is the AC index -- pixel 0 shows the background. */
        memset(g_vmem + VID_TEXT_OFF, 0, 0x4000);
        g_vmem[VID_TEXT_OFF] = 0x1B;                       /* pixels 0,1,2,3 */
        vid.dirty = 1; vdd_bus_frame(&bus);
        CHECK(vid.fb[0] == 0 && vid.fb[1] == 1 && vid.fb[2] == 2 && vid.fb[3] == 3
              && vid.frame.palette[0] == vid.dac[0x04] && vid.frame.palette[1] == vid.dac[0x02],
              "#266 04h: render_cga emits the 2-bit value; colour 0 = DAC[AR00] = the background just set");
        I10(0x0B00, 0x0101, 0, 0);
        CHECK(vid.vpal[1] == 0x03 && vid.vpal[3] == 0x07 && tb[0x66] == 0x24 && vid.frame.palette[0] == vid.dac[0x04],
              "#266 04h: 0Bh BH=1 BL=1 -> palette 1 without intensity (03/05/07), background kept");
        I10(0x0B00, 0x0200, 0, 0);
        CHECK(vid.vpal[1] == 0x03 && tb[0x66] == 0x24, "#266 0Bh: BH=2 is not a function -- nothing moves");
        /* mode 06h: BH=0 is the foreground (AR01), the background stays black */
        I10(0x0006, 0, 0, 0);
        I10(0x0B00, 0x0004, 0, 0);
        CHECK(vid.vpal[1] == 0x04 && vid.vpal[0] == 0x00 && vid.vpal[16] == 0x04 && tb[0x66] == 0x24,
              "#266 06h: 0Bh BH=0 BL=04h -> AR01 (foreground) = 04h, AR00 stays 00h, border 04h");
        /* text: the border only */
        I10(0x0003, 0, 0, 0);
        I10(0x0B00, 0x0001, 0, 0);
        CHECK(vid.vpal[16] == 0x01 && vid.vpal[0] == 0x00 && tb[0x66] == 0x21,
              "#266 03h: 0Bh BH=0 -> the border (AR11) only; AR00 is text colour 0 and is left alone");

        /* AH=04h: no light pen on a VGA -- AH=00h, the rest untouched */
        I10(0x0400, 0xB1B1, 0xC1C1, 0xD1D1);
        CHECK((rr.eax & 0xFF00) == 0 && (rr.ebx & 0xFFFF) == 0xB1B1 && (rr.edx & 0xFFFF) == 0xD1D1,
              "#266 04h: light pen -> AH=00h (not triggered); BX/DX untouched");

        /* AH=11h AL=03h: SR3 gets BL, and a loaded user font is NOT dropped */
        { static uint8_t uf[16]; int i; for (i = 0; i < 16; ++i) uf[i] = 0xAA;
          memcpy(g_flat + 0x40000, uf, 16);
          memset(&rr, 0, sizeof rr); rr.eax = 0x1100; rr.ebx = 0x1000; rr.ecx = 1; rr.edx = 'Z';
          rr.es = 0x4000; rr.ebp = 0; vdd_bus_deliver_int(&bus, 0x10, &rr); }
        CHECK(vid.user_font_on == 1, "#266 11h/00h: a user font is loaded (setup)");
        I10(0x1103, 0x0005, 0, 0);
        { uint32_t rv; v = 0x03; vdd_bus_io(&bus, 0x3C4, 1, 0, &v); rv = 0; vdd_bus_io(&bus, 0x3C5, 1, 1, &rv);
          CHECK(rv == 0x05 && vid.user_font_on == 1,
                "#266 11h/03h: SR3 = BL (05h) and the user font survives (it used to reload the ROM 8x16)"); }
        I10(0x1103, 0x0000, 0, 0);

        /* AH=11h AL=22h-24h in mode 12h: INT 43h, rows, height -- and the BDA follows */
        { const uint8_t *ivt = g_flat;
#define VEC(n) ((uint32_t)(ivt[(n)*4] | (ivt[(n)*4+1] << 8)) | ((uint32_t)(ivt[(n)*4+2] | (ivt[(n)*4+3] << 8)) << 16))
          I10(0x0012, 0, 0, 0);
          CHECK(VEC(0x43) == ((uint32_t)VDD_FONT8X16_SEG << 16), "#266 12h: the mode set points INT 43h at the 8x16 table");
          I10(0x1122, 0x0001, 0, 0);
          CHECK(VEC(0x43) == ((uint32_t)VDD_FONT8X14_SEG << 16) && tb[0x84] == 13 && tb[0x85] == 14,
                "#266 11h/22h BL=1: INT 43h = 8x14, 0040:0084 = 13 (14 rows), 0085 = 14");
          I10(0x1123, 0x0002, 0, 0xD1D1);
          CHECK(VEC(0x43) == ((uint32_t)VDD_FONT8X8_SEG << 16) && tb[0x84] == 24 && tb[0x85] == 8
                && (rr.edx & 0xFFFF) == 0xD1D1,
                "#266 11h/23h BL=2: 8x8, 25 rows; DX left as it came (no longer overwritten)");
          I10(0x1124, 0x0003, 0, 0);
          CHECK(tb[0x84] == 42 && tb[0x85] == 16, "#266 11h/24h BL=3: 8x16, 43 rows");
          I10(0x1123, 0x0000, 0, 0x001E);
          CHECK(tb[0x84] == 29 && tb[0x85] == 8, "#266 11h/23h BL=0 DL=30: 30 rows from DL");
          /* 21h: the caller's font, drawn from where INT 43h points */
          { int i; for (i = 0; i < 256 * 10; ++i) g_flat[0x50000 + i] = 0; for (i = 0; i < 10; ++i) g_flat[0x50000 + 'Q' * 10 + i] = 0x81; }
          memset(&rr, 0, sizeof rr); rr.eax = 0x1121; rr.ebx = 0x0000; rr.ecx = 10; rr.edx = 48;
          rr.es = 0x5000; rr.ebp = 0; vdd_bus_deliver_int(&bus, 0x10, &rr);
          CHECK(VEC(0x43) == 0x50000000u && tb[0x84] == 47 && tb[0x85] == 10 && vid.gfont_user,
                "#266 11h/21h: INT 43h = ES:BP, 48 rows (DL), height CX = 10");
          memset(&rr, 0, sizeof rr); rr.eax = 0x1130; rr.ebx = 0x0100; vdd_bus_deliver_int(&bus, 0x10, &rr);
          CHECK(rr.es == 0x5000 && (rr.ebp & 0xFFFF) == 0 && (rr.ecx & 0xFFFF) == 10,
                "#266 11h/30h BH=1 answers with the caller's font (= INT 43h), CX = 10");
          I10(0x0200, 0, 0, 0x0001);
          I10(0x0951, 0x000F, 1, 0);
          { int ok2 = 1; for (y = 0; y < 10; ++y) if (vid.plane[0][y * 80 + 1] != 0x81) ok2 = 0;
            CHECK(ok2 && vid.plane[0][10 * 80 + 1] == 0, "#266 12h: AH=09h draws 'Q' from the caller's 10-line table at INT 43h"); }
          /* 20h: INT 1Fh */
          memset(&rr, 0, sizeof rr); rr.eax = 0x1120; rr.es = 0x5100; rr.ebp = 0x0010; vdd_bus_deliver_int(&bus, 0x10, &rr);
          CHECK(VEC(0x1F) == 0x51000010u, "#266 11h/20h: INT 1Fh = ES:BP");
          memset(&rr, 0, sizeof rr); rr.eax = 0x1130; rr.ebx = 0x0000; vdd_bus_deliver_int(&bus, 0x10, &rr);
          CHECK(rr.es == 0x5100 && (rr.ebp & 0xFFFF) == 0x0010, "#266 11h/30h BH=0 answers with INT 1Fh");
          I10(0x0003, 0, 0, 0);
          CHECK(!vid.gfont_user && VEC(0x43) == ((uint32_t)VDD_FONT8X16_SEG << 16) && VEC(0x1F) == 0x51000010u,
                "#266: a mode set takes INT 43h back to the ROM; INT 1Fh (a POST vector) is left as set");
          vdd_video_reset(&vid);   /* INT 1Fh back to ours for the tests after */
#undef VEC
        }

        /* 0040:00A8 -> the save pointer table -> the parameter table */
        vdd_video_install_fonts(&vid);
        { uint16_t so = (uint16_t)(tb[0xA8] | (tb[0xA9] << 8)), ss = (uint16_t)(tb[0xAA] | (tb[0xAB] << 8));
          const uint8_t *sp = g_flat + ((uint32_t)ss << 4) + so, *pt, *e3, *e13;
          uint16_t po = (uint16_t)(sp[0] | (sp[1] << 8)), ps = (uint16_t)(sp[2] | (sp[3] << 8));
          uint8_t ref[64]; int ok3;
          pt = g_flat + ((uint32_t)ps << 4) + po;
          e3 = pt + 0x18 * 64; e13 = pt + 0x1C * 64;
          CHECK(ss == VDD_VIDTAB_SEG && so == 0 && ps == VDD_VIDTAB_SEG && po == VDD_VPARAM_OFF,
                "#266 0040:00A8 -> B270:0000, whose first pointer -> the parameter table");
          CHECK(e3[0] == 80 && e3[1] == 24 && e3[2] == 16 && e3[3] == 0x00 && e3[4] == 0x10
                && e3[9] == 0x67 && e3[0x0A] == 0x5F && e3[0x0A + 0x13] == 0x28 && e3[0x0A + 0x0A] == 0x0D
                && e3[0x23 + 6] == 0x14 && e3[0x23 + 0x10] == 0x0C && e3[0x37 + 5] == 0x10 && e3[0x37 + 6] == 0x0E,
                "#266 param slot 18h (mode 3+): 80 cols, 25 rows, 16 high, 1000h; misc 67h, CR00 5Fh, CR13 28h, AR06 14h, GR6 0Eh");
          CHECK(e13[0] == 40 && e13[2] == 8 && e13[9] == 0x63 && e13[0x05 + 3] == 0x0E && e13[0x37 + 5] == 0x40,
                "#266 param slot 1Ch (13h): 40 cols, 8 high, misc 63h, SR4 0Eh (chain-4), GR5 40h");
          for (ok3 = 1, y = 0; y < 29; ++y) {
              (void)vdd_video_param_entry((uint8_t)y, ref);
              if (memcmp(ref, pt + y * 64, 64)) ok3 = 0;
          }
          CHECK(ok3 && pt[3 * 64] == 0 && pt[0x11 * 64] == 0,
                "#266 the table in memory is vdd_video_param_entry's; slot 03h (200-line) and 11h (0Fh) are zero -- unmeasured");
          CHECK(sp[0x10] == VDD_SAVEPTR2_OFF && sp[0x04] == 0 && g_flat[((uint32_t)VDD_VIDTAB_SEG << 4) + VDD_DCC_OFF] == 16,
                "#266 save table +10h -> secondary table; dynamic save area 0; DCC table has 16 entries"); }

        /* SR1 bit 5: the picture goes black, and comes back */
        I10(0x0013, 0, 0, 0);
        memset(g_vmem, 0x0F, 320 * 200);
        I10(0x1201, 0x0036, 0, 0);                         /* refresh OFF -> SR1.5 */
        vid.dirty = 1; vdd_bus_frame(&bus);
        CHECK(vid.blanked && vid.frame.stride == 0 && vid.frame.palette[vid.frame.pixels[0]] == 0xFF000000u
              && vid.frame.w == 320 && vid.frame.h == 200,
              "#266 SR1.5 (12h BL=36h AL=1): the frame goes out black, geometry kept");
        I10(0x1200, 0x0036, 0, 0);
        vid.dirty = 1; vdd_bus_frame(&bus);
        CHECK(!vid.blanked && vid.frame.pixels == g_vmem && vid.frame.palette[0x0F] == vid.dac[0x0F],
              "#266 SR1.5 cleared: the same picture is back, nothing in VRAM was touched");
        sc_w(&bus, 0x01, 0x21);                             /* a guest's own write */
        vid.dirty = 1; vdd_bus_frame(&bus);
        CHECK(vid.blanked, "#266 SR1.5 written at 3C5h by the guest blanks too");
        I10(0x0003, 0, 0, 0);
        vid.dirty = 1; vdd_bus_frame(&bus);
        CHECK(!vid.blanked, "#266 a mode set clears SR1.5 (every measured mode's SR1 has bit 5 = 0)");
#undef I10
        vid.bda = 0;
    }

    printf("\n%d checks, %d failed\n", total, fails);
    return fails ? 1 : 0;
}

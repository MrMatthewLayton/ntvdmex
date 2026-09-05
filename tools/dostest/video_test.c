/* video_test.c -- off-VM unit battery for the video VDD (vdd_video.c): text mode
 * 3 + graphics mode 13h + the DAC palette, over the shared video aperture. The
 * renderer pixels are checked against the real font glyph; mode 13h presents the
 * aperture directly. No VM.
 */
#include <stdio.h>
#include <string.h>
#include "vdd_video.h"
#include "vga_font_8x16.h"

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
static uint8_t g_vmem[VID_APERTURE_SIZE]; /* the video aperture (A0000) stand-in   */
static video_state vid;

static uint8_t *txt(int r,int c){ return g_vmem + VID_TEXT_OFF + (r*vid.cols+c)*2; }
static uint8_t cchar(int r,int c){ return txt(r,c)[0]; }
static uint8_t cattr(int r,int c){ return txt(r,c)[1]; }
static uint32_t dac_pack_ref(uint8_t r,uint8_t g,uint8_t b)
{ return 0xFF000000u | ((uint32_t)(r<<2)<<16) | ((uint32_t)(g<<2)<<8) | (uint32_t)(b<<2); }

int main(void)
{
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
    { uint16_t seg=0x3000, off=0x0000; uint8_t *b=&g_flat[(seg<<4)+off]; uint32_t mlp;
      memset(&r,0,sizeof r); s_ah(&r,0x4F); s_al(&r,0x00); r.es=seg; r.edi=off;
      vdd_bus_deliver_int(&bus,0x10,&r);
      CHECK(r_ax(&r)==0x004F && b[0]=='V'&&b[1]=='E'&&b[2]=='S'&&b[3]=='A', "vesa/4F00: 'VESA' signature");
      mlp = b[14]|(b[15]<<8);                 /* mode-list offset (low word of far ptr) */
      CHECK((b[(mlp&0xFFFF)]|(b[(mlp&0xFFFF)+1]<<8))==0x100, "vesa/4F00: mode list starts 0x100"); }

    /* T10: VESA 4F01 mode info for 0x101 (640x480x8) -------------------- */
    { uint16_t seg=0x3100; uint8_t *b=&g_flat[(seg<<4)];
      memset(&r,0,sizeof r); s_ah(&r,0x4F); s_al(&r,0x01); s_cx(&r,0x101); r.es=seg; r.edi=0;
      vdd_bus_deliver_int(&bus,0x10,&r);
      CHECK(r_ax(&r)==0x004F && (b[18]|(b[19]<<8))==640 && (b[20]|(b[21]<<8))==480 && b[25]==8,
            "vesa/4F01: 0x101 = 640x480x8"); }

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

    /* T12: VESA frame is vesa_w x vesa_h x8 ----------------------------- */
    vid.dirty=1; vdd_bus_frame(&bus);
    CHECK(vid.frame.w==640 && vid.frame.h==480 && vid.frame.pixels==vid.vesa_vram,
          "frame(vesa): 640x480x8 from vesa_vram");

    /* T13: mode 12h planar -- set mode, plot a pixel, check planes + render --- */
    memset(&r,0,sizeof r); s_ah(&r,0x00); s_al(&r,0x12); vdd_bus_deliver_int(&bus,0x10,&r);
    CHECK(vid.mode==0x12, "int10/00: mode set to 12h");
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
       *     differently. Keys off displayed height, not mode number. -------- */
      vid.gh = 400;
      hi = lo = 0;
      for (i = 0; i < 1000; i++) {
          g_fake_us = (uint64_t)(i * 14286 / 1000);       /* one 70 Hz frame */
          vdd_bus_io(&bus, 0x3DA, 1, 1, &v);
          if (v & 0x08) hi++; else lo++;
      }
      CHECK(hi > 0 && lo > 0, "3DA: 70 Hz modes also retrace once per frame");

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

    printf("\n%d checks, %d failed\n", total, fails);
    return fails ? 1 : 0;
}

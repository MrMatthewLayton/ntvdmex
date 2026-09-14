/* video_test.c -- off-VM unit battery for the video VDD (vdd_video.c): text mode
 * 3 + graphics mode 13h + the DAC palette, over the shared video aperture. The
 * renderer pixels are checked against the real font glyph; mode 13h presents the
 * aperture directly. No VM.
 */
#include <stdio.h>
#include <string.h>
#include "vdd_video.h"
#include "vga_font_8x16.h"
#include "vga_font_8x8.h"

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
     *   was read back off a real card by tools/dostest/vgadefs.asm. None of it is
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
     *   Not invented, and not read off a datasheet: DISASSEMBLED from the running game.
     *   VGALEMMI.EXE is PKLITE-packed, so the code was read back out of a 1MB memory
     *   dump of the game running under genuine MS-DOS 6.22 (scripts/lemref.py
     *   --memdump), anchored on the retrace-wait bytes the rig's IO-SITE instrument had
     *   already measured, which fixes the code segment without assuming a load address.
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

        /* ── (1) THE MASKED SPRITE BLITTER at CS:IP +0x95E0, the colour-compare path.
         *   Set-up, verbatim from the disassembly at +0x9540:
         *       mov ax,0x0805 out dx,ax   GR5 = 0x08  read mode 1
         *       mov ax,0x0807 out dx,ax   GR7 = 0x08  don't care: plane 3 only
         *       mov ax,0x0802 out dx,ax   GR2 = 0x08  compare: plane 3 SET
         *   and then, per byte:
         *       mov ah,[es:di] / not ah / mov al,8 / out dx,ax / movsb
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

        /* `not ah` -> 0x0F, straight into the Bit Mask, then one byte of sprite. */
        gc_w(&bus, 0x08, (uint8_t)~got);   /* GR8 bit mask = 0x0F                     */
        gc_w(&bus, 0x03, 0x00);            /* GR3 replace (the OR case is below)      */
        gc_w(&bus, 0x05, 0x00);            /* back to write mode 0 to do the movsb    */
        sc_w(&bus, 0x02, 0x01);            /* map mask = plane 0, as its per-plane loop */
        vga_planar_write(&vid, 0x40, 0xFF);
        CHECK(vid.plane[0][0x40] == 0x0F,
              "lemmings blit: the sprite lands ONLY where the terrain was not");
        CHECK(vid.plane[3][0x40] == 0xF0,
              "lemmings blit: ...and the terrain plane is untouched by it");

        /* ⚠ IT REALLY DOES USE THE ALU. +0x956B is `mov ax,0x1003` -- GR3 = 0x10, which
         *   is function select = OR, not replace. A card that ignored GR3 would pass
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

        /* ── (2) THE PLAIN BLITTER at CS:IP +0x98D1, which is the busier of the two
         *   (858,644 reads of the 970,000). Its inner loop is only:
         *       mov al,[es:di] / movsb / loop
         *   The read's VALUE IS DISCARDED -- it is there to load the latches, so that
         *   the planes the Map Mask disables keep what they had. If a card lets a
         *   disabled plane change, every sprite in the game smears across the others.
         *   That is the guarantee this pins.                                         */
        vid.plane[0][0x50]=0x11; vid.plane[1][0x50]=0x22;
        vid.plane[2][0x50]=0x33; vid.plane[3][0x50]=0x44;
        gc_w(&bus, 0x05, 0x00);            /* read mode 0, write mode 0               */
        gc_w(&bus, 0x04, 0x00);            /* read map 0 -- the value it throws away  */
        sc_w(&bus, 0x02, 0x04);            /* map mask = plane 2 only                 */
        (void)vga_planar_read(&vid, 0x50); /* `mov al,[es:di]`: latches, not data      */
        CHECK(vid.latch[1] == 0x22 && vid.latch[3] == 0x44,
              "lemmings blit: the discarded read is what loads all four latches");
        vga_planar_write(&vid, 0x50, 0x99);
        CHECK(vid.plane[2][0x50] == 0x99, "lemmings blit: movsb writes the selected plane");
        CHECK(vid.plane[0][0x50] == 0x11 && vid.plane[1][0x50] == 0x22 &&
              vid.plane[3][0x50] == 0x44,
              "lemmings blit: the other three planes are preserved exactly");

        /* ── (3) THE VRAM->VRAM COPY at +0x7161. This is how the toolbar gets on
         *   screen, and it is the mechanism behind the open "panel has no icons" bug:
         *       mov dx,0x3c4 / mov ax,0x0f02 / out    Map Mask = all four planes
         *       mov dx,0x3ce / mov ax,0x0105 / out    GR5 = WRITE MODE 1
         *       mov dx,0xa000 / mov es,dx / mov ds,dx   BOTH segments = VRAM
         *       movsb movsb ...                        A000 -> A000
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

        /* ── (4) THE WHOLE-PANEL BLIT at CS:IP 0x7626 -- THE ROUTINE THAT ACTUALLY
         *   PUTS THE TOOLBAR ON SCREEN, and the one the "missing icons" bug is about.
         *   Disassembled from the same dump (segment base linear 0x04CE0, seg 0x04CE,
         *   which is the PARAGRAPH-ALIGNED anchor -- see the warning below):
         *
         *       7602:  mov dx,0x3c4 / mov ax,0x0f02 / out   Map Mask = all four planes
         *              mov dx,0x3ce / mov ax,0x0105 / out   GR5 = WRITE MODE 1
         *              mov si,0xf91f                        source: the panel cache
         *              mov dx,0xa000 / mov es,dx / mov ds,dx
         *              mov cx,0x6e0                         1760 bytes = 40 rows x 44
         *       7626:  rep movsb                            <- THE WHOLE TOOLBAR, ONE OP
         *
         *   Its two callers (0x75EF) blit it to BOTH pages -- [0x1f76] and [0x1f78],
         *   each +0x1E42 -- and 0x75EF has exactly one caller, 0x39CE, whose FIRST
         *   instruction it is, which in turn is called straight-line from level init at
         *   0x048F. ▶ NOTHING GATES IT. It is unconditional on every level start.
         *
         * ⚠ THIS CORRECTS THE PINNED STORY. The per-button routine at 0x789E (whose
         *   first movsb at 0x78F4 is the site the rig named) is NOT the panel painter:
         *   its only caller, 0x3B3C, is behind `mov ah,[0x82] / cmp [0x83],ah / jz`,
         *   i.e. it repaints ONE button when the SELECTED skill changes. Running ~1.5
         *   times in a run where the player changed selection once is correct, not a
         *   defect -- so "the blitter runs 1.5 times instead of 12" was a question about
         *   the wrong routine.
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
         *   its mode -- `mov ax,0x008D` at guest 0x0FB3 for gameplay, `mov ax,0x0090`
         *   at 0x4BEF for the menu -- with bit 7 set precisely so that cache survives.
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
       These values are measured on genuine MS-DOS 6.22 by tools/dostest/vgadefs.asm
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

    printf("\n%d checks, %d failed\n", total, fails);
    return fails ? 1 : 0;
}

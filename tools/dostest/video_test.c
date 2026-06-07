/* video_test.c -- off-VM unit battery for the text-mode video VDD (vdd_video.c).
 *
 * M3 slice-4: exercise the INT 10h text subset, the B8000 trap, and the cell ->
 * pixel renderer natively, no VM. Asserts the framebuffer pixels match the font
 * glyph for a written character (the proof the whole text path renders right).
 */
#include <stdio.h>
#include <string.h>
#include "vdd_video.h"
#include "vga_font_8x16.h"

static int total = 0, fails = 0;
#define CHECK(c,m) do{ total++; if(c){printf("  PASS  %s\n",(m));} \
    else{printf("  FAIL  %s\n",(m)); fails++;} }while(0)

static int g_present = 0; static ntvdd_frame g_lastframe;
static void present_sink(void *ctx, const ntvdd_frame *f){ (void)ctx; g_present++; g_lastframe=*f; }

static uint8_t g_flat[0x100000];          /* guest memory for INT 10h AH=13      */
static video_state vid;                    /* ~290KB -- keep off the stack        */

static uint8_t cattr(int r,int c){ return vid.vram[(r*vid.cols+c)*2+1]; }
static uint8_t cchar(int r,int c){ return vid.vram[(r*vid.cols+c)*2]; }

int main(void)
{
    vdd_bus bus;
    ntvdd dev = vdd_video_device(&vid);
    ntvdd_regs r;
    memset(&vid, 0, sizeof vid);

    printf("== M3 slice-4 text-mode video battery ==\n");

    vdd_bus_init(&bus, g_flat);
    vdd_bus_set_sinks(&bus, 0, 0, present_sink, 0);

    /* T0: device registers + resets to a clean mode-3 screen ---------------- */
    CHECK(vdd_bus_add(&bus, &dev) == 0, "add: video init ok");
    CHECK(bus.n_mem == 1 && bus.ints[0x10].svc && bus.n_frame == 1, "add: B8000+INT10h+frame claimed");
    CHECK(vid.mode == 3 && vid.cols == 80 && vid.rows == 25, "reset: mode 3, 80x25");
    CHECK(cchar(0,0) == ' ' && cattr(0,0) == 0x07, "reset: screen cleared to spaces/0x07");

    /* T1: set/get cursor (AH=02/03) ---------------------------------------- */
    memset(&r,0,sizeof r); s_ah(&r,0x02); s_dx(&r,(uint16_t)((10<<8)|20));
    vdd_bus_deliver_int(&bus,0x10,&r);
    CHECK(vid.cur_row==10 && vid.cur_col==20, "int10/02: cursor set to 10,20");
    memset(&r,0,sizeof r); s_ah(&r,0x03);
    vdd_bus_deliver_int(&bus,0x10,&r);
    CHECK(r_dx(&r)==((10<<8)|20), "int10/03: cursor read back");

    /* T2: teletype (AH=0E) writes + advances; CR/LF ------------------------ */
    memset(&r,0,sizeof r); s_ah(&r,0x02); s_dx(&r,0);
    vdd_bus_deliver_int(&bus,0x10,&r);                /* cursor home            */
    { const char *s="Hi"; int i; for(i=0;s[i];++i){ memset(&r,0,sizeof r); s_ah(&r,0x0E); s_al(&r,(uint8_t)s[i]); vdd_bus_deliver_int(&bus,0x10,&r);} }
    CHECK(cchar(0,0)=='H' && cchar(0,1)=='i', "int10/0E: 'Hi' written at row 0");
    CHECK(vid.cur_col==2 && vid.cur_row==0, "int10/0E: cursor advanced to col 2");
    memset(&r,0,sizeof r); s_ah(&r,0x0E); s_al(&r,0x0D); vdd_bus_deliver_int(&bus,0x10,&r); /* CR */
    memset(&r,0,sizeof r); s_ah(&r,0x0E); s_al(&r,0x0A); vdd_bus_deliver_int(&bus,0x10,&r); /* LF */
    CHECK(vid.cur_col==0 && vid.cur_row==1, "int10/0E: CR+LF -> row 1 col 0");

    /* T3: write char+attr xN (AH=09) --------------------------------------- */
    memset(&r,0,sizeof r); s_ah(&r,0x02); s_dx(&r,(uint16_t)((5<<8)|0)); vdd_bus_deliver_int(&bus,0x10,&r);
    memset(&r,0,sizeof r); s_ah(&r,0x09); s_al(&r,'X'); s_bx(&r,0x1F); s_cx(&r,3); vdd_bus_deliver_int(&bus,0x10,&r);
    CHECK(cchar(5,0)=='X'&&cchar(5,1)=='X'&&cchar(5,2)=='X', "int10/09: 'XXX' written");
    CHECK(cattr(5,0)==0x1F && cattr(5,2)==0x1F, "int10/09: attr 0x1F applied");
    CHECK(vid.cur_col==0 && vid.cur_row==5, "int10/09: cursor NOT moved");

    /* T4: read char+attr (AH=08) ------------------------------------------- */
    memset(&r,0,sizeof r); s_ah(&r,0x08); vdd_bus_deliver_int(&bus,0x10,&r);
    CHECK(r_al(&r)=='X' && r_ah(&r)==0x1F, "int10/08: reads back 'X'/0x1F at cursor");

    /* T5: scroll up (AH=06) ------------------------------------------------- */
    /* put 'A' on row 1, scroll whole screen up 1 -> it lands on row 0 */
    { uint8_t *p=&vid.vram[(1*80+0)*2]; p[0]='A'; p[1]=0x07; }
    memset(&r,0,sizeof r); s_ah(&r,0x06); s_al(&r,1);
    s_cx(&r,0); s_dx(&r,(uint16_t)((24<<8)|79)); s_bx(&r,0x0700);
    vdd_bus_deliver_int(&bus,0x10,&r);
    CHECK(cchar(0,0)=='A', "int10/06: scroll up moved row1->row0");
    CHECK(cchar(24,0)==' ', "int10/06: bottom row blanked");

    /* T6: write string ES:BP (AH=13) --------------------------------------- */
    { uint16_t seg=0x2000, off=0x0010; uint8_t *gp=&g_flat[(seg<<4)+off];
      memcpy(gp,"OK",2);
      memset(&r,0,sizeof r); s_ah(&r,0x13); s_al(&r,0x00); s_bx(&r,0x4E); s_cx(&r,2);
      s_dx(&r,(uint16_t)((12<<8)|3)); r.es=seg; r.ebp=off;
      vdd_bus_deliver_int(&bus,0x10,&r);
      CHECK(cchar(12,3)=='O'&&cchar(12,4)=='K', "int10/13: string 'OK' written at 12,3");
      CHECK(cattr(12,3)==0x4E, "int10/13: string attr 0x4E applied"); }

    /* T7: direct B8000 write routes through the memory hook ----------------- */
    CHECK(vdd_bus_mem_write(&bus, 0xB8000 + (2*80+1)*2, 'Z')==1, "mem: B8000 write handled");
    CHECK(cchar(2,1)=='Z', "mem: B8000 write hit the right cell");

    /* T8: renderer: cell pixels match the font glyph ----------------------- */
    /* write 'A' at (0,0) attr fg=15 bg=0, move cursor away, render, compare */
    memset(&r,0,sizeof r); s_ah(&r,0x02); s_dx(&r,0); vdd_bus_deliver_int(&bus,0x10,&r);
    memset(&r,0,sizeof r); s_ah(&r,0x09); s_al(&r,'A'); s_bx(&r,0x0F); s_cx(&r,1); vdd_bus_deliver_int(&bus,0x10,&r);
    memset(&r,0,sizeof r); s_ah(&r,0x02); s_dx(&r,(uint16_t)((24<<8)|79)); vdd_bus_deliver_int(&bus,0x10,&r);
    vdd_video_render(&vid);
    { int gy,gx,mism=0; const uint8_t *gl=vga_font_8x16['A'];
      for(gy=0;gy<VID_CELL_H;++gy) for(gx=0;gx<VID_CELL_W;++gx){
          uint8_t exp=(gl[gy]&(0x80>>gx))?15:0;
          if(vid.fb[gy*VID_FB_W+gx]!=exp) mism++;
      }
      CHECK(mism==0, "render: cell (0,0) pixels match font glyph 'A'"); }

    /* T9: frame tick presents an 8bpp 640x400 frame ------------------------ */
    g_present=0; vid.dirty=1; vdd_bus_frame(&bus);
    CHECK(g_present==1, "frame: present called when dirty");
    CHECK(g_lastframe.w==640 && g_lastframe.h==400 && g_lastframe.bpp==8 && g_lastframe.palette!=0,
          "frame: 640x400x8 palettised frame");
    g_present=0; vdd_bus_frame(&bus);
    CHECK(g_present==0, "frame: no present when clean");

    printf("\n%d checks, %d failed\n", total, fails);
    return fails ? 1 : 0;
}

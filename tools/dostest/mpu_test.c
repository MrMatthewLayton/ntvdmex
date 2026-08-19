/* mpu_test.c -- off-VM unit battery for the MPU-401 MIDI VDD (vdd_mpu.c).
 *
 * The device forwards whole MIDI messages to a sink, so the battery captures them
 * and checks the assembly rules that real game output depends on:
 *
 *   - the reset / UART-mode handshake acknowledges with 0xFE (without it, drivers
 *     conclude there is no interface)
 *   - the status register's flags are ACTIVE LOW, which is the classic way to get
 *     an MPU driver stuck waiting for a ready bit that never clears
 *   - RUNNING STATUS: a sequencer sends many notes under one status byte
 *   - REALTIME bytes (clock, start, stop) may arrive between the data bytes of
 *     another message and must not corrupt it
 */
#include <stdio.h>
#include <string.h>
#include "vdd_mpu.h"

static int total = 0, fails = 0;
#define CHECK(c,m) do{ total++; if(c){printf("  PASS  %s\n",(m));} \
    else{printf("  FAIL  %s\n",(m)); fails++;} }while(0)

static uint8_t g_flat[0x10000];
static vdd_bus bus;
static mpu_state mpu;

#define CAP 32
static uint32_t g_msg[CAP];
static int      g_nmsg;
static void sink(void *ctx, uint32_t msg)
{ (void)ctx; if (g_nmsg < CAP) g_msg[g_nmsg++] = msg; }

#define BASE MPU_DEFAULT_BASE
static void wr(uint16_t p, uint8_t v){ uint32_t x=v; vdd_bus_io(&bus,p,1,0,&x); }
static uint8_t rd(uint16_t p){ uint32_t x=0; vdd_bus_io(&bus,p,1,1,&x); return (uint8_t)x; }

int main(void)
{
    printf("== sound epic: MPU-401 MIDI battery ==\n");

    memset(&mpu, 0, sizeof mpu);
    mpu.sink = sink;
    vdd_bus_init(&bus, g_flat);
    { ntvdd d = vdd_mpu_device(&mpu); CHECK(vdd_bus_add(&bus, &d) == 0, "add: mpu401 ok"); }

    /* T1: the handshake ----------------------------------------------------- */
    CHECK((rd(BASE + 1) & MPU_ST_DSR) != 0, "status: DSR set (active low) => no data waiting");
    wr(BASE + 1, 0xFF);                                   /* reset               */
    CHECK((rd(BASE + 1) & MPU_ST_DSR) == 0, "status: DSR CLEAR once the ACK is queued");
    CHECK(rd(BASE) == MPU_ACK, "reset: acknowledges with 0xFE");
    wr(BASE + 1, 0x3F);                                   /* enter UART mode     */
    CHECK(rd(BASE) == MPU_ACK, "uart: acknowledges with 0xFE");
    CHECK(mpu.uart_mode == 1, "uart: mode entered");
    CHECK((rd(BASE + 1) & MPU_ST_DRR) == 0, "status: DRR clear => ready to accept data");

    /* T2: a plain note-on --------------------------------------------------- */
    g_nmsg = 0;
    wr(BASE, 0x90); wr(BASE, 0x3C); wr(BASE, 0x64);       /* note on C4 vel 100  */
    CHECK(g_nmsg == 1, "note-on: one complete message emitted");
    CHECK(g_msg[0] == (0x90u | (0x3Cu << 8) | (0x64u << 16)),
          "note-on: packed as status | data1<<8 | data2<<16");

    /* nothing is emitted until the message is complete                         */
    g_nmsg = 0;
    wr(BASE, 0x80); wr(BASE, 0x3C);
    CHECK(g_nmsg == 0, "partial message: nothing emitted yet");
    wr(BASE, 0x40);
    CHECK(g_nmsg == 1, "partial message: emitted once complete");

    /* T3: running status ---------------------------------------------------- */
    g_nmsg = 0;
    wr(BASE, 0x90);                                       /* status once...      */
    wr(BASE, 0x40); wr(BASE, 0x7F);                       /* ...then note pairs  */
    wr(BASE, 0x43); wr(BASE, 0x7F);
    wr(BASE, 0x47); wr(BASE, 0x7F);
    CHECK(g_nmsg == 3, "running status: three notes under one status byte");
    CHECK((g_msg[1] & 0xFF) == 0x90 && ((g_msg[1] >> 8) & 0xFF) == 0x43,
          "running status: second note keeps the status byte");

    /* T4: one-data-byte messages -------------------------------------------- */
    g_nmsg = 0;
    wr(BASE, 0xC0); wr(BASE, 0x30);                       /* program change      */
    CHECK(g_nmsg == 1, "program change: completes after ONE data byte");
    CHECK(g_msg[0] == (0xC0u | (0x30u << 8)), "program change: second data byte is zero");

    /* T5: realtime bytes may interrupt a message ---------------------------- */
    g_nmsg = 0;
    wr(BASE, 0x90); wr(BASE, 0x3C);                       /* mid-message...      */
    wr(BASE, 0xF8);                                       /* ...timing clock     */
    CHECK(g_nmsg == 1 && g_msg[0] == 0xF8, "realtime: clock emitted immediately");
    wr(BASE, 0x64);                                       /* finish the note-on  */
    CHECK(g_nmsg == 2, "realtime: the interrupted note-on still completes");
    CHECK(g_msg[1] == (0x90u | (0x3Cu << 8) | (0x64u << 16)),
          "realtime: interrupted message is not corrupted");

    /* T6: sysex is swallowed, not mistaken for channel data ----------------- */
    g_nmsg = 0;
    wr(BASE, 0xF0); wr(BASE, 0x41); wr(BASE, 0x10); wr(BASE, 0xF7);
    CHECK(g_nmsg == 0, "sysex: swallowed without emitting garbage");
    g_nmsg = 0;
    wr(BASE, 0x90); wr(BASE, 0x3C); wr(BASE, 0x64);
    CHECK(g_nmsg == 1, "sysex: normal messages resume afterwards");

    /* T7: data before UART mode goes nowhere --------------------------------- */
    vdd_mpu_reset(&mpu);
    g_nmsg = 0;
    wr(BASE, 0x90); wr(BASE, 0x3C); wr(BASE, 0x64);
    CHECK(g_nmsg == 0, "not in UART mode: data bytes are ignored");

    printf("-- %d checks, %d failures --\n", total, fails);
    return fails ? 1 : 0;
}

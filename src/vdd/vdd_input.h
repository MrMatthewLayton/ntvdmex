/*
 * vdd_input.h -- the keyboard input VDD.  (M3 slice-6, ADR-0008)
 *
 * A pure key ring buffer + the INT 16h (BIOS keyboard) servicer. The host's UI
 * thread pushes keystrokes (scancode<<8 | ascii) as they arrive; the V86/DOS
 * side drains them via INT 16h and the INT 21h input calls. Blocking (INT 16h
 * AH=00 waiting for a key) is the host's job -- this VDD stays pure and reports
 * "no key" via the zero flag, exactly like the BIOS.
 */
#ifndef NTVDMEX_VDD_INPUT_H
#define NTVDMEX_VDD_INPUT_H

#include "vdd_bus.h"

#ifndef VDD_KBD_SIZE
#define VDD_KBD_SIZE 32        /* ring capacity (power of two not required)      */
#endif

/* THE BIOS KEYBOARD BUFFER LIVES IN THE GUEST, NOT IN US.
   `bda` points at guest segment 0x40 (linear 0x400). Every keystroke is stored in the real
   BIOS ring at 0040:001E with its head/tail at 0040:001A/001C, and INT 16h reads that same
   ring -- because a DOS program is entitled to read it directly, and a great many do. We
   used to keep the ring host-side instead, so INT 16h worked while the buffer in guest
   memory stayed empty forever (head==tail==0x1E in every capture). The Skyroads MENU polls
   exactly that buffer, which is why no arrow or Enter ever registered there while the same
   keys worked in-game (where the game installs its own INT 09h handler and reads port 60h).
   One buffer, in the place the hardware documentation says it is. */
#define BDA_KB_HEAD   0x1A     /* offsets from 0040:0000 */
#define BDA_KB_TAIL   0x1C
#define BDA_KB_START  0x1E     /* 16 entries, 2 bytes each */
#define BDA_KB_END    0x3E     /* one past the last entry  */
#define BDA_KB_FLAGS  0x17     /* shift/ctrl/alt + lock state (INT 16h AH=02) */
#define BDA_KB_FLAGS2 0x18     /* extended shift flags      (INT 16h AH=12) */

typedef struct input_state {
    vdd_bus *bus;
    uint8_t *bda;              /* guest 0040:0000; NULL only in unit tests before setup */
    uint8_t  ext_pending;      /* an E0 prefix has been seen; next code is an extended key */
    /* Raw AT keyboard controller (ports 0x60/0x64): the byte stream an INT 09h
       ISR / a port-polling game reads. UI pushes make (sc) + break (sc|0x80)
       codes; the guest drains one per IN 0x60. Separate from the INT 16h ring. */
    uint8_t  sc_buf[VDD_KBD_SIZE];
    int      sc_head, sc_tail; /* scancode FIFO: empty when sc_head==sc_tail      */
    /* HOW THE GUEST ASKS FOR KEYS. Arrow keys work in a Skyroads level but not in its
       menus, which means the two read the keyboard by different routes -- so count them:
       [0]=INT 16h AH=00/10 (blocking read), [1]=AH=01/11 (peek), [2]=AH=02 (shift flags),
       [3]=other, plus raw port 0x60 reads. Whichever the menu uses is where to look. */
    uint32_t int16_calls[4];
    uint32_t p60_reads;
    /* Scancode accounting. The FIFO discards silently when it fills, so a guest that
       will not drain it (Skyroads runs with IF clear for long stretches) loses bytes
       with nothing anywhere to say so -- and a lost E0 prefix turns an arrow into a
       different key entirely. Counted so "dropped" is a measurement, not a deduction
       from pushes-minus-reads. */
    uint32_t sc_pushed;
    uint32_t sc_dropped;
    uint32_t sc_hiwater;       /* deepest the FIFO ever got (of VDD_KBD_SIZE)     */
    uint8_t  sc_last;          /* last byte handed out on IN 0x60 (re-read)       */
} input_state;

/* BIOS ring ops, all operating on the guest's buffer at 0040:001E.
   (push = UI thread; pop/peek = V86 thread; caller serialises.) */
void vdd_input_push(input_state *st, uint16_t key);   /* full -> discard, as the BIOS does */
int  vdd_input_pop (input_state *st, uint16_t *key);  /* 1 if a key was returned */
int  vdd_input_peek(input_state *st, uint16_t *key);  /* 1 if a key is available */

/* raw scancode FIFO ops (ports 0x60/0x64) -- UI thread pushes, V86 drains. */
void vdd_input_push_scancode(input_state *st, uint8_t sc);
int  vdd_input_sc_pending(const input_state *st);     /* 1 if a scancode waits   */

/* THE BIOS INT 09h HANDLER. Takes the byte out of the controller, re-asserts the line if
   more are queued, tracks the E0 prefix and the shift/ctrl/alt/lock state into 0040:0017,
   translates the make code to a BIOS keycode (AH=scancode, AL=ascii; AL=0 for the extended
   keys, which is what makes an arrow an arrow) and stores it in the ring at 0040:001E.
   It used to consume the byte and DISCARD it -- the FIFO drained, so keystrokes kept
   interrupting, but a guest that had not hooked INT 09h could never see a key at all. */
void vdd_input_bios_consume(input_state *st);

int  vdd_input_init(vdd_bus *b, void *self);          /* claims INT 16h          */
void vdd_input_reset(void *self);
static inline ntvdd vdd_input_device(input_state *st)
{ ntvdd d; d.name = "input"; d.init = vdd_input_init; d.reset = vdd_input_reset;
  d.shutdown = 0; d.self = st; return d; }

#endif /* NTVDMEX_VDD_INPUT_H */

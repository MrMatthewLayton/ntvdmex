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

typedef struct input_state {
    vdd_bus *bus;
    uint16_t buf[VDD_KBD_SIZE];
    int      head, tail;       /* INT 16h ring: empty when head==tail            */
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
    uint8_t  sc_last;          /* last byte handed out on IN 0x60 (re-read)       */
} input_state;

/* ring ops (push = UI thread; pop/peek = V86 thread; caller serialises). */
void vdd_input_push(input_state *st, uint16_t key);   /* drop oldest if full     */
int  vdd_input_pop (input_state *st, uint16_t *key);  /* 1 if a key was returned */
int  vdd_input_peek(input_state *st, uint16_t *key);  /* 1 if a key is available */

/* raw scancode FIFO ops (ports 0x60/0x64) -- UI thread pushes, V86 drains. */
void vdd_input_push_scancode(input_state *st, uint8_t sc);
int  vdd_input_sc_pending(const input_state *st);     /* 1 if a scancode waits   */

/* Consume one scancode the way the BIOS INT 09h handler does: take the byte out of the
   controller and re-assert the line if more are queued. Our default INT 09h stub used to be
   a bare IRET, which meant a guest that has not hooked the vector never drained the FIFO --
   so the controller stayed permanently full and no further keystroke could ever interrupt. */
void vdd_input_bios_consume(input_state *st);

int  vdd_input_init(vdd_bus *b, void *self);          /* claims INT 16h          */
void vdd_input_reset(void *self);
static inline ntvdd vdd_input_device(input_state *st)
{ ntvdd d; d.name = "input"; d.init = vdd_input_init; d.reset = vdd_input_reset;
  d.shutdown = 0; d.self = st; return d; }

#endif /* NTVDMEX_VDD_INPUT_H */

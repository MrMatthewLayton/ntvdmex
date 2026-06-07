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
    int      head, tail;       /* empty when head==tail                          */
} input_state;

/* ring ops (push = UI thread; pop/peek = V86 thread; caller serialises). */
void vdd_input_push(input_state *st, uint16_t key);   /* drop oldest if full     */
int  vdd_input_pop (input_state *st, uint16_t *key);  /* 1 if a key was returned */
int  vdd_input_peek(input_state *st, uint16_t *key);  /* 1 if a key is available */

int  vdd_input_init(vdd_bus *b, void *self);          /* claims INT 16h          */
void vdd_input_reset(void *self);
static inline ntvdd vdd_input_device(input_state *st)
{ ntvdd d; d.name = "input"; d.init = vdd_input_init; d.reset = vdd_input_reset;
  d.shutdown = 0; d.self = st; return d; }

#endif /* NTVDMEX_VDD_INPUT_H */

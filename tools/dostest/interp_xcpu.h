/* interp_xcpu.h -- the register file interp_side.c and interp_superset.c exchange
 * (#194). Deliberately NOT v86interp.h's icpu: each side includes its own version of
 * that header, and this struct must not depend on either. */
#ifndef INTERP_XCPU_H
#define INTERP_XCPU_H
#include <stdint.h>
#define XMEM_SIZE 0x110000u
typedef struct { uint32_t r[8]; uint16_t seg[6]; uint16_t ip; uint32_t flags; } xcpu;
#endif

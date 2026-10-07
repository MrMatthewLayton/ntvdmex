/* interp_xcpu.h -- the register file interp_side.c and interp_superset.c exchange
 * (#194). Deliberately NOT v86interp.h's icpu: each side includes its own version of
 * that header, and this struct must not depend on either. */
#ifndef INTERP_XCPU_H
#define INTERP_XCPU_H
#include "../../src/ntvdmex_types.h"
#define XMEM_SIZE 0x110000u
typedef struct _INTERP_XCPU { UINT32 Registers[8]; UINT16 Segments[6]; UINT16 Ip; UINT32 Flags; } INTERP_XCPU, *PINTERP_XCPU;
typedef const INTERP_XCPU *PCINTERP_XCPU;
#endif

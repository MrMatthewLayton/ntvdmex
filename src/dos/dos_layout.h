/* dos_layout.h -- the host's conventional-memory layout for a DOS process.
 * Shared by the host (which places the PSP/IVT/handler/env) and the INT 21h
 * surface (which reports these segments back to the guest). Matches the spike.
 */
#ifndef DOS_LAYOUT_H
#define DOS_LAYOUT_H

#include "dos_mcb.h"          /* DOS_PSP_SEG (0x0100), DOS_MEM_TOP (0xA000) */

#define DOS_HDLR_SEG  0x0050  /* INT 21h BOP handler segment (linear 0x0500) */
#define DOS_ENV_SEG   0x0060  /* environment segment (linear 0x0600)         */
#define DOS_LOAD_OFF  0x0010  /* .EXE load module = DOS_PSP_SEG + this       */
#define DOS_DBCS_OFF  0x0018  /* empty DBCS table parked at DOS_HDLR_SEG:this */
#define DOS_EMM_NAME_OFF 0x000A /* "EMMXXXX0" device-header name (M4 EMS detect, *
                                 * INT 67h vector segment : offset 0Ah)        */

/* Bare RETF, planted by the host. INT 21h AH=38h hands the caller a FAR pointer
   to DOS's case-mapping routine; pointing it at nothing would send any program
   that actually calls it into the weeds, so it points here and returns
   immediately -- an identity case map, which is a correct no-op for ASCII. */
#define DOS_CASEMAP_OFF 0x0059

#endif /* DOS_LAYOUT_H */

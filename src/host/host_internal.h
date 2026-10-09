/* host_internal.h -- what the host_*.c files share: the types, macros and state
 * used by more than one of them, and a prototype for every function called across
 * files. Included once, by main.c, after its prelude. */
#ifndef NTVDMEX_HOST_INTERNAL_H
#define NTVDMEX_HOST_INTERNAL_H

#include "host_types.h"
#include "host_state.h"
#include "host_dpmi_int.h"
#include "host_dos.h"
#include "host_irq.h"
#include "host_video.h"
#include "host_diag.h"
#include "host_io.h"
#include "host_bios.h"
#include "host_dpmi.h"
#include "host_wow.h"
#include "host_input.h"
#include "host_mouse.h"
#include "host_window.h"
#include "host_settings.h"
#include "host_audio.h"
#include "main.h"
#include "host_timing.h"
#include "host_install.h"

/* State used from a file other than its owner's (tentative definitions). */
static UINT g_DmaPollOverflow;
static UINT g_PollStackOverflow;
/* Functions called from a file other than their own. */

#endif

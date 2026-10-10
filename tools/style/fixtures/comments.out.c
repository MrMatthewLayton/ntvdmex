/* NTVDMEX -- An NTVDM replacement for Microsoft Windows
 *
 * Comment markers, banners and gutters.
 *
 *
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Matthew Layton
 */

#include "comments.h"

/* THE RING BUFFER (Importance = 2): */

/* [CAUTION]: The buffer is shared with the audio thread.
 * Take the lock first.
 *
 * Never block in here.
 */
static INT g_Ring[16];

/* [INFO]: A starred note -- with a dash. */
static INT g_Head;

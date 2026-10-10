/* comments.c -- comment markers, banners and gutters. */

#include "comments.h"

/* ── ★★ THE RING BUFFER ─────────────────────────── */

/* ⚠ The buffer is shared with the audio thread.
   Take the lock first.
   ⇒ Never block in here. */
static INT g_Ring[16];

/* ★ A starred note — with a dash. */
static INT g_Head;

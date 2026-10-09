/* NTVDMEX -- An NTVDM replacement for Microsoft Windows
 *
 * The version both sides of the WOW shim contract check (#333).
 *
 * The host (src/host/main.c) fills an NTVDMEX_SHIM_API and hands it to each shim DLL's
 * NtvdmexShimInit; the shim refuses a table whose Version is not its own. The two are
 * built separately, so the number lives here, once (docs/STYLE.md section 7).
 * Defines only.
 *
 *
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Matthew Layton
 */

#ifndef NTVDMEX_SHIM_API_H
#define NTVDMEX_SHIM_API_H

#define SHIM_API_VERSION        3   /* Host + bin\wowshim\ must agree */

/* Global16's operation: krnl386's own global-heap call the host makes for the shim. */
#define SHIM_GLOBAL_ALLOC       0
#define SHIM_GLOBAL_FREE        1
#define SHIM_GLOBAL_LOCK        2
#define SHIM_GLOBAL_UNLOCK      3
#define SHIM_GLOBAL_SIZE        4
#define SHIM_GLOBAL_HANDLE      5
#define SHIM_GLOBAL_OPERATIONS  6

#endif /* NTVDMEX_SHIM_API_H */

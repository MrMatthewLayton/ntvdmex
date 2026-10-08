/* shim_api.h -- the version both sides of the WOW shim contract check (#333).
 *
 * The host (src/host/main.c) fills an NTVDMEX_SHIM_API and hands it to each shim DLL's
 * NtvdmexShimInit; the shim refuses a table whose Version is not its own. The two are
 * built separately, so the number lives here, once (docs/STYLE.md section 7).
 * Defines only.
 */
#ifndef NTVDMEX_SHIM_API_H
#define NTVDMEX_SHIM_API_H

#define SHIM_API_VERSION        3       /* host + bin\wowshim\ must agree */

#endif /* NTVDMEX_SHIM_API_H */

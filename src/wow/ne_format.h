/* ne_format.h -- the New Executable format's field layout, for src/wow/ne.h.
 *
 * Kept apart from ne.h because ne.h's line numbers are its error codes (`Error` is the
 * __LINE__ that rejected an image), so it cannot grow. Like ne.h, it uses only the
 * portable Windows types, so the loader still builds off-VM for tests/unit/ne_test.c.
 */
#ifndef NTVDMEX_NE_FORMAT_H
#define NTVDMEX_NE_FORMAT_H

#include "../ntvdmex_types.h"

#endif /* NTVDMEX_NE_FORMAT_H */

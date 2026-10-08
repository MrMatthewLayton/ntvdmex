/* ntvdmex_units.h -- time and frequency unit conversions (#333).
 *
 * One name per meaning, defined once (docs/STYLE.md, section 3). Each exists in the C type
 * the code used it in: plain = int, _U unsigned, _UL unsigned long, _LL long long, _ULL
 * unsigned long long (see ntvdmex_bits.h on why the type is part of the name).
 * Included by ntvdmex_types.h.
 */
#ifndef NTVDMEX_UNITS_H
#define NTVDMEX_UNITS_H

#define BYTES_PER_KILOBYTE          1024

#define MILLISECONDS_PER_SECOND         1000
#define MILLISECONDS_PER_SECOND_U       1000u
#define MICROSECONDS_PER_MILLISECOND    1000
#define MICROSECONDS_PER_MILLISECOND_U  1000u
#define MICROSECONDS_PER_MILLISECOND_UL 1000ul
#define MICROSECONDS_PER_MILLISECOND_ULL 1000ull
#define MICROSECONDS_PER_SECOND         1000000
#define MICROSECONDS_PER_SECOND_U       1000000u
#define MICROSECONDS_PER_SECOND_LL      1000000ll
#define MICROSECONDS_PER_SECOND_ULL     1000000ull
#define NANOSECONDS_PER_SECOND_U        1000000000u
#define FILETIME_TICKS_PER_MILLISECOND_U 10000u   /* a FILETIME counts 100 ns ticks */
#define HERTZ_PER_MEGAHERTZ_U           1000000u
#define MEGAHERTZ_PER_GIGAHERTZ_U       1000u
#define PER_MILLE_U                     1000u
#define PERCENT                         100
#define PERCENT_U                       100u

#endif /* NTVDMEX_UNITS_H */

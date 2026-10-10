/* braces.c -- Allman braces, statements and bodies. */

#include "braces.h"

static INT Clamp(INT value, INT low, INT high) {
    if (value < low) { return low; }
    if (value > high) return high; else return value;
}

INT Sum(INT *values, INT count) {
    INT total = 0, index;
    for (index = 0; index < count; index++) total += values[index];
    return total;
}

VOID Log2(INT a, INT b)
{
    INT x = a; INT y = b;
    LogPut(x); LogPut(y);
    switch (a) {
    case 1: x = 2; break;
    case 2: x = 3; break;
    }
    while (x > 0) { x--; }
    y = Clamp(x, 0, 9);
}

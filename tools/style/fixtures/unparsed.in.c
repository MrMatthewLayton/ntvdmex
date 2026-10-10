/* unparsed.c -- an #include after code: the order stage leaves the file's order alone. */

#include "unparsed.h"

static INT g_Late;

#include "late.h"

INT UnparsedValue(VOID)
{
    return g_Late;
}

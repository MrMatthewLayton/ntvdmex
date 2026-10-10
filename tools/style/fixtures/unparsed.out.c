/* NTVDMEX -- An NTVDM replacement for Microsoft Windows
 *
 * An #include after code: the order stage leaves the file's order alone.
 *
 *
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Matthew Layton
 */

#include "unparsed.h"

static INT g_Late;

#include "late.h"

INT UnparsedValue(VOID)
{
    return g_Late;
}

/* NTVDMEX -- An NTVDM replacement for Microsoft Windows
 *
 * A case label's statements are one level in.
 *
 *
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Matthew Layton
 */

#include "case.h"

INT CaseValue(INT selector)
{
    INT value = 0;

    switch (selector)
    {
    case 1:
        value = 2;
        break;

    case 2:
        value = 3;
        /* The last one. */
        return value;

    case 3:
    {
        value = 4;
    }
    break;
    }

    return value;
}

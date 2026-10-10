/* NTVDMEX -- An NTVDM replacement for Microsoft Windows
 *
 * The order of a source file's items.
 *
 *
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Matthew Layton
 */

#include "order.h"
#include <windows.h>
#include "project.h"

#define ORDER_LIMIT     9

typedef struct _ORDER_PAIR
{
    INT Left;
    INT Right;
} ORDER_PAIR;

static INT g_Count;

static INT Helper(INT value)
{
    return value > ORDER_LIMIT ? ORDER_LIMIT : value;
}

INT OrderPublic(INT value)
{
    return Helper(value) + g_Count;
}

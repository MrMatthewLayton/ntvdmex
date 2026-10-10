/* order.c -- the order of a source file's items. */

#include "project.h"
#include <windows.h>
#include "order.h"

static INT g_Count;

static INT Helper(INT value);

INT OrderPublic(INT value)
{
    return Helper(value) + g_Count;
}

#define ORDER_LIMIT 9

static INT Helper(INT value)
{
    return value > ORDER_LIMIT ? ORDER_LIMIT : value;
}

typedef struct _ORDER_PAIR
{
    INT Left;
    INT Right;
} ORDER_PAIR;

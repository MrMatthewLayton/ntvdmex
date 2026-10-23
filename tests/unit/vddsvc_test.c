/* NTVDMEX -- An NTVDM replacement for Microsoft Windows
 *
 * vdd_svc.c's tables (#11): memory hooks, IRQ reservations, user hooks, and the
 * registry's VDD list.
 *
 *
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Matthew Layton
 */

#include <stdio.h>
#include <string.h>
#include "vdd_svc.h"

#define CHECK(condition,message) do{ g_Total++; if(condition){printf("  PASS  %s\n",(message));} \
    else{printf("  FAIL  %s\n",(message)); g_Failures++;} }while(0)

static INT g_Total = 0;
static INT g_Failures = 0;

static VOID VddSvcTestHandler(VOID)
{
}

INT main(VOID)
{
    VDD_SVC_MEMORY_HOOKS memoryHooks;
    VDD_SVC_USER_HOOKS userHooks;
    PVOID vddA = (PVOID)0x1000;
    PVOID vddB = (PVOID)0x2000;
    PVOID handler = (PVOID)VddSvcTestHandler;
    WORD reserved;
    CHAR name[16];

    printf("== #11: the VDD service tables ==\n");
    memset(&memoryHooks, 0, sizeof memoryHooks);
    CHECK(VddSvcMemoryHookAdd(&memoryHooks, vddA, 0xD0000, 0x4000, handler), "mem: a hook on D0000-D3FFF");
    CHECK(VddSvcMemoryHookFind(&memoryHooks, 0xD0000) && VddSvcMemoryHookFind(&memoryHooks, 0xD3FFF)
          && !VddSvcMemoryHookFind(&memoryHooks, 0xD4000) && !VddSvcMemoryHookFind(&memoryHooks, 0xCFFFF),
          "mem: found at both ends, not one past either");
    CHECK(!VddSvcMemoryHookAdd(&memoryHooks, vddB, 0xD3000, 0x2000, handler), "mem: an overlapping range is refused (another VDD)");
    CHECK(!VddSvcMemoryHookAdd(&memoryHooks, vddA, 0xD2000, 0x10, handler), "mem: ...and from the same VDD");
    CHECK(VddSvcMemoryHookAdd(&memoryHooks, vddB, 0xD4000, 0x1000, handler), "mem: an adjacent range is fine");
    CHECK(!VddSvcMemoryHookAdd(&memoryHooks, vddA, 0xE0000, 0, handler) && !VddSvcMemoryHookAdd(&memoryHooks, vddA, 0xFFFFF000u, 0x2000, handler)
          && !VddSvcMemoryHookAdd(&memoryHooks, NULL, 0xE0000, 1, handler) && !VddSvcMemoryHookAdd(&memoryHooks, vddA, 0xE0000, 1, NULL),
          "mem: empty, wrapping, ownerless or handlerless ranges are refused");
    CHECK(!VddSvcMemoryHookRemove(&memoryHooks, vddB, 0xD0000, 0x4000), "mem: only the owner removes a hook");
    CHECK(!VddSvcMemoryHookRemove(&memoryHooks, vddA, 0xD0000, 0x1000), "mem: ...and only with its exact range");
    CHECK(VddSvcMemoryHookRemove(&memoryHooks, vddA, 0xD0000, 0x4000) && !VddSvcMemoryHookFind(&memoryHooks, 0xD1000),
          "mem: removed, the range is free");
    CHECK(VddSvcMemoryHookAdd(&memoryHooks, vddA, 0xE0000, 0x100, handler) && VddSvcMemoryHookAdd(&memoryHooks, vddA, 0xE1000, 0x100, handler)
          && VddSvcMemoryHookRemoveOwner(&memoryHooks, vddA) == 2 && VddSvcMemoryHookFind(&memoryHooks, 0xD4000),
          "mem: a VDD's module unloading takes its hooks, and only its");
    {
        INT added = 0;
        memset(&memoryHooks, 0, sizeof memoryHooks);

        while (VddSvcMemoryHookAdd(&memoryHooks, vddA, 0xC0000 + (DWORD)added * 0x1000, 0x1000, handler))
            ++added;

        CHECK(added == VDD_SVC_MEMORY_HOOKS_MAX, "mem: the table holds VDD_SVC_MEMORY_HOOKS_MAX ranges, then refuses");
    }

    reserved = (1u << 0) | (1u << 1) | (1u << 8) | (1u << 10);   /* the host's own lines */
    CHECK(VddSvcIrqReserve(&reserved, 5) == 5 && (reserved & (1u << 5)), "irq: a free line is reserved");
    CHECK(VddSvcIrqReserve(&reserved, 5) == VDD_SVC_NO_IRQ, "irq: ...once");
    CHECK(VddSvcIrqReserve(&reserved, 0) == VDD_SVC_NO_IRQ && VddSvcIrqReserve(&reserved, 2) == VDD_SVC_NO_IRQ
          && VddSvcIrqReserve(&reserved, 16) == VDD_SVC_NO_IRQ, "irq: a host line, the cascade and line 16 are refused");
    CHECK(VddSvcIrqReserve(&reserved, VDD_SVC_NO_IRQ) == 11, "irq: 'any' takes the first free line in AT order (10 was the host's)");
    CHECK(VddSvcIrqRelease(&reserved, 5) && !(reserved & (1u << 5)) && !VddSvcIrqRelease(&reserved, 5),
          "irq: released once, and not twice");
    reserved = 0xFFFF;
    CHECK(VddSvcIrqReserve(&reserved, VDD_SVC_NO_IRQ) == VDD_SVC_NO_IRQ, "irq: 'any' with every line taken fails");

    memset(&userHooks, 0, sizeof userHooks);
    CHECK(VddSvcUserHookAdd(&userHooks, vddA, handler, NULL, NULL, handler), "user: a set with gaps is accepted");
    CHECK(VddSvcUserHookAdd(&userHooks, vddA, NULL, handler, NULL, NULL) && userHooks.Entries[0].Create == NULL
          && userHooks.Entries[0].Terminate == handler && userHooks.Entries[1].Owner == NULL,
          "user: installing again replaces the VDD's set in place");
    CHECK(VddSvcUserHookAdd(&userHooks, vddB, handler, handler, handler, handler) && userHooks.Entries[1].Owner == vddB,
          "user: a second VDD gets its own slot");
    CHECK(VddSvcUserHookRemove(&userHooks, vddA) && !userHooks.Entries[0].Owner && !VddSvcUserHookRemove(&userHooks, vddA)
          && !VddSvcUserHookAdd(&userHooks, NULL, handler, NULL, NULL, NULL), "user: removed once; no owner, no hook");

    {
        static const CHAR list[] = "vdmsound.dll\0" "a-name-far-too-long-for-it.dll\0" "x.dll\0";
        PCSTR cursor = list;
        PCSTR end = list + sizeof list;
        CHECK(VddSvcMultiStringNext(&cursor, end, name, sizeof name) && !strcmp(name, "vdmsound.dll"), "multisz: the first name");
        CHECK(VddSvcMultiStringNext(&cursor, end, name, sizeof name) && !strcmp(name, "x.dll"), "multisz: a name too long is skipped whole");
        CHECK(!VddSvcMultiStringNext(&cursor, end, name, sizeof name) && name[0] == 0, "multisz: the empty name ends the list");
    }
    {
        static const CHAR unterminated[3] = { 'a', '.', 'd' };    /* no NULs at all */
        PCSTR cursor = unterminated;
        CHECK(VddSvcMultiStringNext(&cursor, unterminated + sizeof unterminated, name, sizeof name) && !strcmp(name, "a.d")
              && !VddSvcMultiStringNext(&cursor, unterminated + sizeof unterminated, name, sizeof name),
              "multisz: a list missing its NULs stops at the registry's length");
        cursor = unterminated;
        CHECK(!VddSvcMultiStringNext(&cursor, unterminated, name, sizeof name), "multisz: an empty value has no names");
    }

    printf("-- %d checks, %d failures --\n", g_Total, g_Failures);
    return g_Failures ? 1 : 0;
}

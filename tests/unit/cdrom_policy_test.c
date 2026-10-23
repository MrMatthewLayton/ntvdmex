/* NTVDMEX -- An NTVDM replacement for Microsoft Windows
 *
 * When a change on Settings > Drives > CD-ROM re-mounts the ISO drive (src/host/cdrom_policy.h),
 * pinned off-VM.  GH #241.
 *
 *   cc -std=c99 -I src -I src/dos -I src/host -o cdrom_policy_test tests/unit/cdrom_policy_test.c
 *
 *
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Matthew Layton
 */

#include <stdio.h>
#include "cdrom_policy.h"

#define CHECK(condition, message) do { ++g_Checks; if (condition) printf("  PASS  %s\n", (message)); \
    else { printf("  FAIL  %s\n", (message)); ++g_Failures; } } while (0)

static INT g_Checks;
static INT g_Failures;

static VOID CdromPolicyTestLetter(VOID)
{
    printf("drive letter\n");
    CHECK(CdromPolicyChooseLetter(0x0000000D, 0) == 4, "A: C: D: -> E:, after the last drive");
    CHECK(CdromPolicyChooseLetter(0x0200000D, 0x02000000) == 4, "A: C: D: + network Z: -> E: (the test machine)");
    CHECK(CdromPolicyChooseLetter(0x0000000D | 0x00000400, 0) == 11, "a local K: -> L:");
    CHECK(CdromPolicyChooseLetter(0x0200000D | 0x01000000, 0x02000000) == 4, "local Y:, network Z: -> the lowest free from D:, E:");
    CHECK(CdromPolicyChooseLetter(0x03FFFFFF, 0) == -1, "every letter taken -> none");
    CHECK(CdromPolicyChooseLetter(0x0000000C, 0) == 4, "C: D: -> E:");
}

static VOID CdromPolicyTestRemount(VOID)
{
    printf("re-mount decision\n");
    CHECK(!CdromPolicyNeedsRemount(0, "", 0, ""), "physical -> physical: nothing");
    CHECK(!CdromPolicyNeedsRemount(0, "", 0, "C:\\CD.ISO"), "physical -> physical with a path stored: nothing");
    CHECK(CdromPolicyNeedsRemount(0, "", 1, "C:\\CD.ISO"), "physical -> image: mount");
    CHECK(CdromPolicyNeedsRemount(0, "", 1, ""), "physical -> image with no path: mount an empty drive");
    CHECK(CdromPolicyNeedsRemount(1, "C:\\CD.ISO", 0, "C:\\CD.ISO"), "image -> physical: unmount");
    CHECK(!CdromPolicyNeedsRemount(1, "C:\\CD.ISO", 1, "C:\\CD.ISO"), "same image: nothing");
    CHECK(!CdromPolicyNeedsRemount(1, "C:\\Media\\Cd.Iso", 1, "c:\\media\\CD.ISO"), "same image, other case: nothing");
    CHECK(CdromPolicyNeedsRemount(1, "C:\\ONE.ISO", 1, "C:\\TWO.ISO"), "another image: swap the disc");
    CHECK(CdromPolicyNeedsRemount(1, "", 1, "C:\\CD.ISO"), "empty drive -> an image: mount it");
    CHECK(CdromPolicyNeedsRemount(1, "C:\\CD.ISO", 1, ""), "an image -> empty drive: re-mount empty");
    CHECK(!CdromPolicyNeedsRemount(1, NULL, 1, ""), "a NULL path reads as empty");
    CHECK(CdromPolicyNeedsRemount(1, "C:\\CD.IS", 1, "C:\\CD.ISO"), "a prefix is not the same path");
}

int main(VOID)
{
    CdromPolicyTestRemount();
    CdromPolicyTestLetter();
    printf("%d checks, %d failed\n", g_Checks, g_Failures);
    return g_Failures ? 1 : 0;
}

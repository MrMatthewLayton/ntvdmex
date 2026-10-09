/* NTVDMEX -- An NTVDM replacement for Microsoft Windows
 *
 * Off-VM battery for src/host/x86len.h, the instruction-length
 * decoder and boundary test that decide which `CD nn` byte pairs the DPMI host is
 * allowed to rewrite into a BOP.
 *
 * WHY THIS IS WORTH A BATTERY. Getting it wrong is invisible and fatal in both
 * directions:
 *   - reject a REAL site  -> the guest executes a raw `CD nn` in protected mode,
 *                            the one fault XP will not reflect, VDM torn down silently;
 *   - accept a FALSE one  -> we overwrite two bytes of someone else's instruction.
 *                            That is what killed Doom for five sessions: a `jle`
 *                            whose displacement was 0xcd, followed by `xor ecx,ecx`
 *                            (`7e cd 31 c9`), became `7e c4 c4 c9`, so a loop's back
 *                            edge jumped into the middle of another instruction.
 *
 * Every case below is a hand-built byte string, composed for this battery: the
 * encodings that actually decide boundaries (prefixes, sib, disp, group-3
 * immediates, the 0F map), and original sequences that reproduce each false
 * positive / false negative class the patcher has met in real guests.  The longer
 * fixtures are long on purpose -- the vote looks at the preceding 48 bytes, so a
 * fixture shorter than that window does not reproduce the stream the vote sees.
 *
 *
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Matthew Layton
 */

#include <stdio.h>
#include <string.h>

#include "../../src/host/x86len.h"

static INT g_Total = 0, g_Failures = 0;
#define CHECK(condition,message) do{ g_Total++; if(condition){printf("  PASS  %s\n",(message));} \
    else{printf("  FAIL  %s\n",(message)); g_Failures++;} }while(0)

static VOID X86LenTestLengthIs(
    PCSTR description,
    PCBYTE bytes,
    UINT count,
    INT isDefault32,
    UINT expected)
{
    UINT actual = X86InstructionLength(bytes, 0, count, isDefault32);

    if (actual == expected)
    {
        g_Total++;
        printf("  PASS  len %-34s = %u\n", description, actual);
    }
    else
    {
        g_Total++;
        g_Failures++;
        printf("  FAIL  len %-34s = %u (want %u)\n", description, actual, expected);
    }
}

INT main(VOID)
{
    printf("x86len battery\n");

    /* ---- lengths, 32-bit code ------------------------------------------------ */
    {
        static const BYTE bytes[] = { 0x31, 0xC9 };
        X86LenTestLengthIs("xor ecx,ecx", bytes, sizeof bytes, 1, 2);
    }
    {
        static const BYTE bytes[] = { 0x7E, 0xCD };
        X86LenTestLengthIs("jle rel8", bytes, sizeof bytes, 1, 2);
    }
    {
        static const BYTE bytes[] = { 0xCD, 0x21 };
        X86LenTestLengthIs("int 0x21", bytes, sizeof bytes, 1, 2);
    }
    {
        static const BYTE bytes[] = { 0xE8, 0xCD, 0x10, 0x00, 0x00 };
        X86LenTestLengthIs("call rel32", bytes, sizeof bytes, 1, 5);
    }
    { static const BYTE bytes[] = { 0x8B, 0x3D, 0x0C, 0x23, 0x03, 0x00 };
      X86LenTestLengthIs("mov edi,[disp32]", bytes, sizeof bytes, 1, 6); }
    { static const BYTE bytes[] = { 0x8B, 0xB8, 0xE4, 0x4F, 0x03, 0x00 };
      X86LenTestLengthIs("mov edi,[eax+disp32]", bytes, sizeof bytes, 1, 6); }
    { static const BYTE bytes[] = { 0x89, 0x99, 0x1C, 0x90, 0x03, 0x00 };
      X86LenTestLengthIs("mov [ecx+disp32],ebx", bytes, sizeof bytes, 1, 6); }
    { static const BYTE bytes[] = { 0x81, 0xEB, 0x00, 0x00, 0x00, 0x40 };
      X86LenTestLengthIs("sub ebx,imm32", bytes, sizeof bytes, 1, 6); }
    {
        static const BYTE bytes[] = { 0x83, 0xC0, 0x04 };
        X86LenTestLengthIs("add eax,imm8", bytes, sizeof bytes, 1, 3);
    }
    {
        static const BYTE bytes[] = { 0xC1, 0xE3, 0x13 };
        X86LenTestLengthIs("shl ebx,imm8", bytes, sizeof bytes, 1, 3);
    }
    {
        static const BYTE bytes[] = { 0xF7, 0xEB };
        X86LenTestLengthIs("imul ebx (grp3 /5, no imm)", bytes, sizeof bytes, 1, 2);
    }
    { static const BYTE bytes[] = { 0xF7, 0xC3, 0x01, 0x00, 0x00, 0x00 };
      X86LenTestLengthIs("test ebx,imm32 (grp3 /0)", bytes, sizeof bytes, 1, 6); }
    {
        static const BYTE bytes[] = { 0xF6, 0xC3, 0x01 };
        X86LenTestLengthIs("test bl,imm8 (grp3 /0)", bytes, sizeof bytes, 1, 3);
    }
    {
        static const BYTE bytes[] = { 0x0F, 0xAC, 0xD0, 0x10 };
        X86LenTestLengthIs("shrd eax,edx,imm8", bytes, sizeof bytes, 1, 4);
    }
    {
        static const BYTE bytes[] = { 0x0F, 0xA0 };
        X86LenTestLengthIs("push fs", bytes, sizeof bytes, 1, 2);
    }
    { static const BYTE bytes[] = { 0x0F, 0x84, 0x10, 0x00, 0x00, 0x00 };
      X86LenTestLengthIs("jz rel32", bytes, sizeof bytes, 1, 6); }
    {
        static const BYTE bytes[] = { 0x0F, 0xB6, 0x04, 0x18 };
        X86LenTestLengthIs("movzx eax,[eax+ebx] (sib)", bytes, sizeof bytes, 1, 4);
    }
    { static const BYTE bytes[] = { 0x8B, 0x04, 0x8D, 0x00, 0x10, 0x00, 0x00 };
      X86LenTestLengthIs("mov eax,[ecx*4+disp32] (sib/no base)", bytes, sizeof bytes, 1, 7); }
    {
        static const BYTE bytes[] = { 0x66, 0xB8, 0x34, 0x12 };
        X86LenTestLengthIs("mov ax,imm16 (66 in 32-bit)", bytes, sizeof bytes, 1, 4);
    }
    {
        static const BYTE bytes[] = { 0xA1, 0x00, 0x10, 0x00, 0x00 };
        X86LenTestLengthIs("mov eax,moffs32", bytes, sizeof bytes, 1, 5);
    }
    {
        static const BYTE bytes[] = { 0xC8, 0x10, 0x00, 0x00 };
        X86LenTestLengthIs("enter imm16,imm8", bytes, sizeof bytes, 1, 4);
    }
    {
        static const BYTE bytes[] = { 0xC2, 0x08, 0x00 };
        X86LenTestLengthIs("ret imm16", bytes, sizeof bytes, 1, 3);
    }
    { static const BYTE bytes[] = { 0x9A, 0x00, 0x00, 0x00, 0x00, 0x0F, 0x00 };
      X86LenTestLengthIs("lcall ptr16:32", bytes, sizeof bytes, 1, 7); }
    {
        static const BYTE bytes[] = { 0xF3, 0xA5 };
        X86LenTestLengthIs("rep movsd", bytes, sizeof bytes, 1, 2);
    }

    /* ---- lengths, 16-bit code (DOS-extender style) ---------------------------- */
    {
        static const BYTE bytes[] = { 0xB8, 0x34, 0x12 };
        X86LenTestLengthIs("mov ax,imm16 (16-bit)", bytes, sizeof bytes, 0, 3);
    }
    { static const BYTE bytes[] = { 0x66, 0xB8, 0x78, 0x56, 0x34, 0x12 };
      X86LenTestLengthIs("mov eax,imm32 (66 in 16-bit)", bytes, sizeof bytes, 0, 6); }
    {
        static const BYTE bytes[] = { 0x8B, 0x86, 0x04, 0x00 };
        X86LenTestLengthIs("mov ax,[bp+disp16]", bytes, sizeof bytes, 0, 4);
    }
    {
        static const BYTE bytes[] = { 0x8B, 0x1E, 0x00, 0x10 };
        X86LenTestLengthIs("mov bx,[disp16]", bytes, sizeof bytes, 0, 4);
    }
    {
        static const BYTE bytes[] = { 0xEA, 0x00, 0x00, 0x0F, 0x00 };
        X86LenTestLengthIs("ljmp ptr16:16", bytes, sizeof bytes, 0, 5);
    }
    {
        static const BYTE bytes[] = { 0xE8, 0x00, 0x10 };
        X86LenTestLengthIs("call rel16", bytes, sizeof bytes, 0, 3);
    }

    /* Truncation must be reported, never guessed at: a decoder that walks past the
     * end of the region is the "instrument that faults kills the run" failure.
     */
    { static const BYTE bytes[] = { 0x81, 0xEB, 0x00 };
      CHECK(X86InstructionLength(bytes, 0, sizeof bytes, 1) == 0, "truncated imm32 -> 0"); }
    { static const BYTE bytes[] = { 0x8B };
      CHECK(X86InstructionLength(bytes, 0, sizeof bytes, 1) == 0, "missing modrm -> 0"); }

    /* ---- the boundary test: the class that killed Doom ---------------------- */
    /* A loop whose back edge is `jle -51` (7e cd), followed by `xor ecx,ecx` (31 c9).
     * The `cd 31` at +0x1e is the jle displacement plus the xor opcode, NOT an
     * `int 0x31`.  This exact pattern, in a real game image, was patched into
     * `7e c4 c4 c9` and killed Doom for five sessions.  (The loop head is above
     * the fixture; the vote only needs the bytes leading up to the branch.)
     */
    { static const BYTE bytes[] = {
        0x8B,0x75,0xF0,                 /* 00 mov esi,[ebp-0x10] */
        0x8B,0x04,0x9E,                 /* 03 mov eax,[esi+ebx*4] */
        0x03,0x45,0xEC,                 /* 06 add eax,[ebp-0x14] */
        0x89,0x04,0x9F,                 /* 09 mov [edi+ebx*4],eax */
        0xC1,0xF8,0x10,                 /* 0c sar eax,0x10 */
        0x89,0x84,0x99,0x00,0x30,0x00,0x00, /* 0f mov [ecx+ebx*4+0x3000],eax */
        0x43,                           /* 16 inc ebx */
        0x83,0xC2,0x08,                 /* 17 add edx,8 */
        0x3B,0x5D,0xE8,                 /* 1a cmp ebx,[ebp-0x18] */
        0x7E,0xCD,                      /* 1d jle -51 (back to loop head) */
        0x31,0xC9,                      /* 1f xor ecx,ecx */
        0x8B,0x45,0xE4 };               /* 21 mov eax,[ebp-0x1c] */
      CHECK(bytes[0x1E] == 0xCD && bytes[0x1F] == 0x31, "fixture holds the CD 31 byte pair");
      CHECK(!X86IsInstructionStart(bytes, 0x1E, sizeof bytes, 1),
            "jle displacement followed by xor ecx,ecx is NOT an int 0x31");
      CHECK(!X86IsIntSiteReal(bytes, 0x1E, sizeof bytes, 1),
            "...so the patcher must REFUSE it (it is the jle's displacement)");
      CHECK(X86IsInstructionStart(bytes, 0x1D, sizeof bytes, 1),
            "...and the jle at +0x1d IS an instruction start"); }

    /* The `cd 10` inside a `call rel32` displacement (e8 cd 10 00 00).
     * - THE FIXTURE HAS 56 BYTES OF LEAD-IN ON PURPOSE.  A short fixture does not
     *   reproduce this case: the test votes over the PRECEDING bytes, and the
     *   instruction just before the call ends in `03 00`, so a stream that starts
     *   on the `00` decodes `00 e8` (add al,ch) and lands exactly on the `cd`.
     *   With only the seven bytes before the site, 3 of the 7 streams do that and
     *   the site reads as REAL.  Over the full 48-byte window the streams converge
     *   on the true boundaries first, and the site scores the same 3 -- now 3 of
     *   48, a minority.  A boundary test cannot be unit-tested on fragments
     *   shorter than its own window.
     */
    { static const BYTE bytes[] = {
        0x55,                           /* 00 push ebp */
        0x89,0xE5,                      /* 01 mov ebp,esp */
        0x53,                           /* 03 push ebx */
        0x56,                           /* 04 push esi */
        0x8B,0x5D,0x08,                 /* 05 mov ebx,[ebp+8] */
        0x8B,0x35,0x40,0x21,0x03,0x00,  /* 08 mov esi,[0x32140] */
        0x85,0xDB,                      /* 0e test ebx,ebx */
        0x74,0x0A,                      /* 10 je +10 (-> 0x1c) */
        0xC7,0x05,0x44,0x21,0x03,0x00,  /* 12 mov dword [0x32144],1 */
        0x01,0x00,0x00,0x00,
        0x31,0xD2,                      /* 1c xor edx,edx */
        0xB8,0x13,0x00,0x00,0x00,       /* 1e mov eax,0x13 */
        0xE8,0x6C,0xF2,0xFF,0xFF,       /* 23 call rel32 */
        0x89,0xC1,                      /* 28 mov ecx,eax */
        0xA3,0x48,0x21,0x03,0x00,       /* 2a mov [0x32148],eax */
        0x31,0xC0,                      /* 2f xor eax,eax */
        0x89,0x0D,0x4C,0x21,0x03,0x00,  /* 31 mov [0x3214c],ecx */
        0xE8,0xCD,0x10,0x00,0x00,       /* 37 call rel32 (+0x10cd) */
        0xBF,0x01,0x00,0x00,0x00 };     /* 3c mov edi,1 */
      CHECK(bytes[56] == 0xCD && bytes[57] == 0x10, "fixture holds the CD 10 byte pair");
      CHECK(!X86IsInstructionStart(bytes, 56, sizeof bytes, 1),
            "call rel32 displacement is NOT an int 0x10");
      CHECK(!X86IsIntSiteReal(bytes, 56, sizeof bytes, 1),
            "...so the patcher must REFUSE it (it is the call's displacement)");
      CHECK(X86IsInstructionStart(bytes, 55, sizeof bytes, 1),
            "...and the call at +0x37 IS an instruction start"); }

    /* A REAL int 0x21, in the shape Watcom emits it -- `mov ah,3ch / int 21h`.
     * This is the case a naive "is the previous byte a 1-byte-immediate opcode?"
     * filter gets wrong, which is why the boundary test is a vote and not a peek.
     */
    { static const BYTE bytes[] = {
        0x55,0x8B,0xEC,0x83,0xEC,0x08,  /* push ebp / mov ebp,esp / sub esp,8 */
        0x8B,0x55,0x08,                 /* mov edx,[ebp+8] */
        0xB4,0x3C,                      /* mov ah,0x3c */
        0xCD,0x21,                      /* int 0x21   <-- real */
        0xD1,0xD0 };                    /* rcl eax,1 */
      CHECK(bytes[11] == 0xCD && bytes[12] == 0x21, "fixture holds the real CD 21");
      CHECK(X86IsInstructionStart(bytes, 11, sizeof bytes, 1),
            "`mov ah,3ch / int 21h`: the int IS an instruction start");
      CHECK(X86IsIntSiteReal(bytes, 11, sizeof bytes, 1),
            "...and the patcher keeps it"); }

    /* -- THE FALSE NEGATIVE THAT COST A RUN.  A DOS-extender's DOS-version check sat
     * directly after its own `$`-terminated error message, so every backward
     * anchor decodes ASCII and the real site scores 1 vote in 48 -- by votes alone
     * indistinguishable from a branch displacement at 3 in 48.  Refusing it left a
     * raw `int 21h` in protected mode and ended the run inside the extender's own
     * startup.  The mechanism, in this fixture: the closing `$` (24) is `and al,imm8`
     * and swallows the `b4`, and the next pair `30 cd` is `xor ch,cl` -- so the
     * ASCII streams converge on the `30` (47 of 48) and step straight over the `cd`.
     * (A message ending "\r\n$" would NOT reproduce it: `0d 0a 24` is `or ax,imm16`
     * and re-synchronises on the `b4`.)
     */
    { static const BYTE bytes[] = {
        0x55,                           /* 00 push bp */
        0x8B,0xEC,                      /* 01 mov bp,sp */
        0x83,0xEC,0x08,                 /* 03 sub sp,8 */
        0x8B,0x46,0x04,                 /* 06 mov ax,[bp+4] */
        0x89,0x46,0xFA,                 /* 09 mov [bp-6],ax */
        0xE8,0x3A,0x01,                 /* 0c call rel16 */
        0x8B,0x46,0xFA,                 /* 0f mov ax,[bp-6] */
        0x0B,0xC0,                      /* 12 or ax,ax */
        0x74,0x05,                      /* 14 je +5 (-> 0x1b) */
        0xB8,0x01,0x00,                 /* 16 mov ax,1 */
        0xEB,0x02,                      /* 19 jmp +2 (-> 0x1d) */
        0x33,0xC0,                      /* 1b xor ax,ax */
        0x8B,0xE5,                      /* 1d mov sp,bp */
        0x5D,                           /* 1f pop bp */
        0xC3,                           /* 20 ret */
        'N','e','e','d','s',' ','D','O',/* 21 "Needs DOS 3.10 or later.$" */
        'S',' ','3','.','1','0',' ','o',
        'r',' ','l','a','t','e','r','.','$',
        0xB4,0x30,                      /* 3a mov ah,30h */
        0xCD,0x21,                      /* 3c int 21h   <-- real */
        0x3C,0x03,                      /* 3e cmp al,3 */
        0x73,0x05 };                    /* 40 jae +5 */
      CHECK(bytes[60] == 0xCD && bytes[61] == 0x21, "fixture holds the version-check int 21h");
      CHECK(!X86IsInstructionStart(bytes, 60, sizeof bytes, 0),
            "int 21h right after a `$`-terminated message: the vote alone CANNOT see it");
      /* [INFO]: SESSION 39: THIS ASSERTION IS INVERTED ON PURPOSE.  It used to read
       * `X86IsIntSiteReal(...)` -- KEEP -- because refusing a real site left a
       * raw `CD nn` in protected mode and that was fatal.  It is not fatal any
       * more: since session 34 a #GP with the IDT bit set IS the interrupt, and the
       * host services it and patches the site from the fault, where the CPU has
       * already proved the bytes are an instruction.  So a false reject now costs
       * one #GP and a false accept still costs silent code corruption -- see
       * x86len.h.  This site is REJECTED now, faults once, and is patched correctly.
       *
       * [CAUTION]: Flipping this back without also removing the #GP(IDT) arm would restore
       * the code corruption the next fixture pins.
       */
      CHECK(!X86IsIntSiteReal(bytes, 60, sizeof bytes, 0),
            "...and it is now REJECTED, to be serviced from the #GP instead"); }

    /* -- THE FALSE POSITIVE THAT KILLED THE WIN16 LAUNCH (session 39, GH #128).
     * The candidate at index 56 is the `cd` of a `cmp cl,ch` (3a cd) followed by
     * a `jne` (75 xx), so the `cd 75` spans two instructions.  The vote fails,
     * the owner IS named -- and it is a `cmp`, not a relative branch, so the old
     * rule kept it.  When the pattern turned up in a real Win16 kernel image,
     * `cd 75` became `c4 c4`, the `jne` became an `les`, and WOWEXEC died with a
     * General Protection Fault in KRNL386 the moment it tried to launch an
     * application.  The owner test is what has to catch this,
     * and "owner exists" is the only property that separates it -- the owning
     * instruction class does not.  The fixture is a full window (55 bytes of
     * 16-bit string-scanning code) so the vote at the `cd` -- 0 of 48 -- and at
     * the `cmp` -- 48 of 48 -- are measured over the stream the scan really sees.
     */
    { static const BYTE bytes[] = {
        0x56,                           /* 00 push si */
        0x57,                           /* 01 push di */
        0x8B,0x76,0x04,                 /* 02 mov si,[bp+4] */
        0x8B,0x7E,0x06,                 /* 05 mov di,[bp+6] */
        0x33,0xC9,                      /* 08 xor cx,cx */
        0x8A,0x2E,0x20,0x01,            /* 0a mov ch,[0x120] */
        0xAC,                           /* 0e lodsb */
        0x0A,0xC0,                      /* 0f or al,al */
        0x74,0x1E,                      /* 11 je +30 (-> 0x31) */
        0x3C,0x20,                      /* 13 cmp al,' ' */
        0x74,0xF7,                      /* 15 je -9 (-> 0x0e) */
        0x3C,0x61,                      /* 17 cmp al,'a' */
        0x72,0x06,                      /* 19 jb +6 (-> 0x21) */
        0x3C,0x7A,                      /* 1b cmp al,'z' */
        0x77,0x02,                      /* 1d ja +2 (-> 0x21) */
        0x2C,0x20,                      /* 1f sub al,20h */
        0xAA,                           /* 21 stosb */
        0xFE,0xC1,                      /* 22 inc cl */
        0x83,0x7E,0xFC,0x00,            /* 24 cmp word [bp-4],0 */
        0x75,0xE4,                      /* 28 jne -28 (-> 0x0e) */
        0xFF,0x46,0xFE,                 /* 2a inc word [bp-2] */
        0xC6,0x45,0xFF,0x00,            /* 2d mov byte [di-1],0 */
        0x8B,0x46,0xFE,                 /* 31 mov ax,[bp-2] */
        0x3B,0x46,0xF8,                 /* 34 cmp ax,[bp-8] */
        0x3A,0xCD,                      /* 37 cmp cl,ch */
        0x75,0x2E,                      /* 39 jne rel8 */
        0x0B,0xC9,                      /* 3b or cx,cx */
        0x75,0xCF };                    /* 3d jne -49 (-> 0x0e) */
      CHECK(bytes[56] == 0xCD && bytes[57] == 0x75, "fixture holds the CD 75 byte pair");
      CHECK(!X86IsInstructionStart(bytes, 56, sizeof bytes, 0),
            "cmp cl,ch followed by jne (16-bit): the vote correctly says it is no instruction start");
      CHECK(!X86IsIntSiteReal(bytes, 56, sizeof bytes, 0),
            "cmp cl,ch followed by jne (16-bit): `cmp` owns it -- REJECT (was the WOWEXEC launch GP)"); }

    /* A `jmp short` displacement in 16-bit code: `eb cd` followed by `xor ax,ax`
     * (33 c0) reads as a `cd 33` byte pair.  Same class as the jle above, different
     * branch, 16-bit code -- found in a DOS extender's own 16-bit modules.
     */
    { static const BYTE bytes[] = {
        0x8B,0x4E,0x06,                 /* 00 mov cx,[bp+6] */
        0xE3,0x0D,                      /* 03 jcxz +13 (-> 0x12) */
        0xAC,                           /* 05 lodsb */
        0x3C,0x0D,                      /* 06 cmp al,0dh */
        0x74,0x05,                      /* 08 je +5 (-> 0x0f) */
        0xAA,                           /* 0a stosb */
        0xE2,0xF8,                      /* 0b loop -8 (-> 0x05) */
        0xB0,0x24,                      /* 0d mov al,'$' */
        0xAA,                           /* 0f stosb */
        0xEB,0xCD,                      /* 10 jmp -51 */
        0x33,0xC0,                      /* 12 xor ax,ax */
        0xAA,                           /* 14 stosb */
        0x5F,                           /* 15 pop di */
        0x5E };                         /* 16 pop si */
      CHECK(bytes[17] == 0xCD && bytes[18] == 0x33, "fixture holds the CD 33 byte pair");
      CHECK(!X86IsIntSiteReal(bytes, 17, sizeof bytes, 0),
            "`jmp short` displacement followed by xor ax,ax is NOT an int 0x33"); }

    /* Offset 0 has nothing before it to vote, and the region start is where the
     * object begins -- trust it rather than reject every site in the first 48 bytes.
     */
    { static const BYTE bytes[] = { 0xCD, 0x21, 0x90, 0x90 };
      CHECK(X86IsInstructionStart(bytes, 0, sizeof bytes, 1), "offset 0 is trusted"); }

    printf("\n%d checks, %d failed\n", g_Total, g_Failures);
    return g_Failures ? 1 : 0;
}

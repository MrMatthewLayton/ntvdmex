/* x86len_test.c -- off-VM battery for src/host/x86len.h, the instruction-length
 * decoder and boundary test that decide which `CD nn` byte pairs the DPMI host is
 * allowed to rewrite into a BOP.
 *
 * WHY THIS IS WORTH A BATTERY. Getting it wrong is invisible and fatal in both
 * directions:
 *   - reject a REAL site  -> the guest executes a raw `CD nn` in protected mode,
 *                            the one fault XP will not reflect, VDM torn down silently;
 *   - accept a FALSE one  -> we overwrite two bytes of someone else's instruction.
 *                            That is what killed Doom for five sessions: `7e cd 31 c9`
 *                            (jle / xor ecx,ecx) at obj1+0x3593f became `7e c4 c4 c9`,
 *                            so R_InitTextureMapping's loop-2 back edge jumped into the
 *                            middle of a `jl` and read memory off an angle register.
 *
 * The cases below are hand-built byte strings -- the encodings that actually decide
 * boundaries (prefixes, sib, disp, group-3 immediates, the 0F map) plus the three
 * real false positives measured in Doom's own image, with the surrounding bytes
 * copied from the binary so the test reproduces the exact stream.
 */
#include <stdio.h>
#include <string.h>

#include "../../src/host/x86len.h"

static int total = 0, fails = 0;
#define CHECK(c,m) do{ total++; if(c){printf("  PASS  %s\n",(m));} \
    else{printf("  FAIL  %s\n",(m)); fails++;} }while(0)

static void len_is(const char *what, const unsigned char *b, unsigned n, int d32, unsigned want)
{
    unsigned got = x86_insn_len(b, 0, n, d32);
    if (got == want) { total++; printf("  PASS  len %-34s = %u\n", what, got); }
    else { total++; fails++; printf("  FAIL  len %-34s = %u (want %u)\n", what, got, want); }
}

int main(void)
{
    printf("x86len battery\n");

    /* ---- lengths, 32-bit code ------------------------------------------------ */
    { static const unsigned char b[] = { 0x31, 0xC9 };                  len_is("xor ecx,ecx", b, sizeof b, 1, 2); }
    { static const unsigned char b[] = { 0x7E, 0xCD };                  len_is("jle rel8", b, sizeof b, 1, 2); }
    { static const unsigned char b[] = { 0xCD, 0x21 };                  len_is("int 0x21", b, sizeof b, 1, 2); }
    { static const unsigned char b[] = { 0xE8, 0xCD, 0x10, 0x00, 0x00 };len_is("call rel32", b, sizeof b, 1, 5); }
    { static const unsigned char b[] = { 0x8B, 0x3D, 0x0C, 0x23, 0x03, 0x00 };
      len_is("mov edi,[disp32]", b, sizeof b, 1, 6); }
    { static const unsigned char b[] = { 0x8B, 0xB8, 0xE4, 0x4F, 0x03, 0x00 };
      len_is("mov edi,[eax+disp32]", b, sizeof b, 1, 6); }
    { static const unsigned char b[] = { 0x89, 0x99, 0x1C, 0x90, 0x03, 0x00 };
      len_is("mov [ecx+disp32],ebx", b, sizeof b, 1, 6); }
    { static const unsigned char b[] = { 0x81, 0xEB, 0x00, 0x00, 0x00, 0x40 };
      len_is("sub ebx,imm32", b, sizeof b, 1, 6); }
    { static const unsigned char b[] = { 0x83, 0xC0, 0x04 };            len_is("add eax,imm8", b, sizeof b, 1, 3); }
    { static const unsigned char b[] = { 0xC1, 0xE3, 0x13 };            len_is("shl ebx,imm8", b, sizeof b, 1, 3); }
    { static const unsigned char b[] = { 0xF7, 0xEB };                  len_is("imul ebx (grp3 /5, no imm)", b, sizeof b, 1, 2); }
    { static const unsigned char b[] = { 0xF7, 0xC3, 0x01, 0x00, 0x00, 0x00 };
      len_is("test ebx,imm32 (grp3 /0)", b, sizeof b, 1, 6); }
    { static const unsigned char b[] = { 0xF6, 0xC3, 0x01 };            len_is("test bl,imm8 (grp3 /0)", b, sizeof b, 1, 3); }
    { static const unsigned char b[] = { 0x0F, 0xAC, 0xD0, 0x10 };      len_is("shrd eax,edx,imm8", b, sizeof b, 1, 4); }
    { static const unsigned char b[] = { 0x0F, 0xA0 };                  len_is("push fs", b, sizeof b, 1, 2); }
    { static const unsigned char b[] = { 0x0F, 0x84, 0x10, 0x00, 0x00, 0x00 };
      len_is("jz rel32", b, sizeof b, 1, 6); }
    { static const unsigned char b[] = { 0x0F, 0xB6, 0x04, 0x18 };      len_is("movzx eax,[eax+ebx] (sib)", b, sizeof b, 1, 4); }
    { static const unsigned char b[] = { 0x8B, 0x04, 0x8D, 0x00, 0x10, 0x00, 0x00 };
      len_is("mov eax,[ecx*4+disp32] (sib/no base)", b, sizeof b, 1, 7); }
    { static const unsigned char b[] = { 0x66, 0xB8, 0x34, 0x12 };      len_is("mov ax,imm16 (66 in 32-bit)", b, sizeof b, 1, 4); }
    { static const unsigned char b[] = { 0xA1, 0x00, 0x10, 0x00, 0x00 };len_is("mov eax,moffs32", b, sizeof b, 1, 5); }
    { static const unsigned char b[] = { 0xC8, 0x10, 0x00, 0x00 };      len_is("enter imm16,imm8", b, sizeof b, 1, 4); }
    { static const unsigned char b[] = { 0xC2, 0x08, 0x00 };            len_is("ret imm16", b, sizeof b, 1, 3); }
    { static const unsigned char b[] = { 0x9A, 0x00, 0x00, 0x00, 0x00, 0x0F, 0x00 };
      len_is("lcall ptr16:32", b, sizeof b, 1, 7); }
    { static const unsigned char b[] = { 0xF3, 0xA5 };                  len_is("rep movsd", b, sizeof b, 1, 2); }

    /* ---- lengths, 16-bit code (DOS/4GW's own modules) ------------------------ */
    { static const unsigned char b[] = { 0xB8, 0x34, 0x12 };            len_is("mov ax,imm16 (16-bit)", b, sizeof b, 0, 3); }
    { static const unsigned char b[] = { 0x66, 0xB8, 0x78, 0x56, 0x34, 0x12 };
      len_is("mov eax,imm32 (66 in 16-bit)", b, sizeof b, 0, 6); }
    { static const unsigned char b[] = { 0x8B, 0x86, 0x04, 0x00 };      len_is("mov ax,[bp+disp16]", b, sizeof b, 0, 4); }
    { static const unsigned char b[] = { 0x8B, 0x1E, 0x00, 0x10 };      len_is("mov bx,[disp16]", b, sizeof b, 0, 4); }
    { static const unsigned char b[] = { 0xEA, 0x00, 0x00, 0x0F, 0x00 };len_is("ljmp ptr16:16", b, sizeof b, 0, 5); }
    { static const unsigned char b[] = { 0xE8, 0x00, 0x10 };            len_is("call rel16", b, sizeof b, 0, 3); }

    /* Truncation must be reported, never guessed at: a decoder that walks past the
       end of the region is the "instrument that faults kills the run" failure. */
    { static const unsigned char b[] = { 0x81, 0xEB, 0x00 };
      CHECK(x86_insn_len(b, 0, sizeof b, 1) == 0, "truncated imm32 -> 0"); }
    { static const unsigned char b[] = { 0x8B };
      CHECK(x86_insn_len(b, 0, sizeof b, 1) == 0, "missing modrm -> 0"); }

    /* ---- the boundary test: Doom's killer, byte-for-byte -------------------- */
    /* obj1+0x35924..0x35943, copied from DOOM.EXE's code object. The `cd 31` at
       +0x1c is the jle displacement plus the xor opcode, NOT an `int 0x31`. */
    { static const unsigned char b[] = {
        0x8B,0x3D,0x0C,0x23,0x03,0x00,  /* 35924 mov edi,[0x3230c]   */
        0xC1,0xE3,0x13,                 /* 3592a shl ebx,0x13        */
        0x83,0xC1,0x04,                 /* 3592d add ecx,4           */
        0x81,0xEB,0x00,0x00,0x00,0x40,  /* 35930 sub ebx,0x40000000  */
        0x42,                           /* 35936 inc edx             */
        0x89,0x99,0x1C,0x90,0x03,0x00,  /* 35937 mov [ecx+0x3901c],ebx */
        0x39,0xFA,                      /* 3593d cmp edx,edi         */
        0x7E,0xCD,                      /* 3593f jle -51             */
        0x31,0xC9,                      /* 35941 xor ecx,ecx         */
        0x8D,0x80,0x00,0x00,0x00,0x00 };/* 35943 lea eax,[eax+0]     */
      CHECK(b[0x1C] == 0xCD && b[0x1D] == 0x31, "fixture holds the CD 31 byte pair");
      CHECK(!x86_is_insn_start(b, 0x1C, sizeof b, 1),
            "Doom obj1+0x35940: jle displacement is NOT an int 0x31");
      CHECK(!x86_int_site_is_real(b, 0x1C, sizeof b, 1),
            "...so the patcher must REFUSE it (it is the jle's displacement)");
      CHECK(x86_is_insn_start(b, 0x1B, sizeof b, 1),
            "...and the jle at +0x1b IS an instruction start"); }

    /* obj1+0x0ae0f: the `cd 10` inside a `call rel32` displacement.
       ► THE FIXTURE IS 56 REAL BYTES OF LEAD-IN, COPIED FROM THE IMAGE, ON PURPOSE.
         A short fixture does not reproduce this case: the test votes over the
         PRECEDING bytes, so with only seven of them the handful of streams that
         exist all happen to land on the site and it reads as real. In the image
         there are 48 anchors and it scores 3 of them. A boundary test cannot be
         unit-tested on fragments shorter than its own window. */
    { static const unsigned char b[] = {
        0x8A, 0x25, 0x1A, 0x63, 0x02, 0x00, 0x80, 0xE4,
        0xDE, 0x31, 0xD2, 0x88, 0x25, 0x1A, 0x63, 0x02,
        0x00, 0x88, 0xE2, 0xB8, 0x41, 0x00, 0x00, 0x00,
        0xE8, 0x8C, 0xF4, 0xFF, 0xFF, 0x31, 0xD2, 0xB8,
        0x49, 0x00, 0x00, 0x00, 0x8A, 0x15, 0x1A, 0x63,
        0x02, 0x00, 0x31, 0xC9, 0xE8, 0x78, 0xF4, 0xFF,
        0xFF, 0x89, 0x0D, 0x84, 0x61, 0x02, 0x00, 0xE8,
        0xCD, 0x10, 0x00, 0x00, 0xBF, 0x01, 0x00, 0x00 };
      CHECK(b[56] == 0xCD && b[57] == 0x10, "fixture holds the CD 10 byte pair");
      CHECK(!x86_is_insn_start(b, 56, sizeof b, 1),
            "Doom obj1+0x0ae0f: call displacement is NOT an int 0x10");
      CHECK(!x86_int_site_is_real(b, 56, sizeof b, 1),
            "...so the patcher must REFUSE it (it is the call's displacement)");
      CHECK(x86_is_insn_start(b, 55, sizeof b, 1),
            "...and the call at +0x37 IS an instruction start"); }

    /* A REAL int 0x21, in the shape Watcom emits it -- `mov ah,3ch / int 21h`.
       This is the case a naive "is the previous byte a 1-byte-immediate opcode?"
       filter gets wrong, which is why the boundary test is a vote and not a peek. */
    { static const unsigned char b[] = {
        0x55,0x8B,0xEC,0x83,0xEC,0x08,  /* push ebp / mov ebp,esp / sub esp,8 */
        0x8B,0x55,0x08,                 /* mov edx,[ebp+8]                    */
        0xB4,0x3C,                      /* mov ah,0x3c                        */
        0xCD,0x21,                      /* int 0x21   <-- real                */
        0xD1,0xD0 };                    /* rcl eax,1                          */
      CHECK(b[11] == 0xCD && b[12] == 0x21, "fixture holds the real CD 21");
      CHECK(x86_is_insn_start(b, 11, sizeof b, 1),
            "`mov ah,3ch / int 21h`: the int IS an instruction start");
      CHECK(x86_int_site_is_real(b, 11, sizeof b, 1),
            "...and the patcher keeps it"); }

    /* ── THE FALSE NEGATIVE THAT COST A RUN. DOS/4GW's DOS-version check sits directly
         after the string "requires DOS/16M\n\r$", so every backward anchor decodes
         ASCII and the site scores 1 vote in 48 -- by votes alone indistinguishable from
         Doom's jle displacement at 3 in 48. Refusing it left a raw `int 21h` in
         protected mode and ended the run inside the extender's own startup.
         What separates them: nothing CONFIRMED covers this one. */
    { static const unsigned char b[] = {
        0xEC, 0x83, 0x7E, 0xE6, 0x00, 0x74, 0x03, 0xE9,
        0x75, 0xFF, 0x2B, 0xD2, 0xE9, 0x70, 0xFF, 0xC4,
        0x5E, 0x0E, 0x83, 0x46, 0x0E, 0x04, 0x26, 0x8B,
        0x07, 0x26, 0x8B, 0x57, 0x02, 0x89, 0x46, 0xEE,
        0x89, 0x56, 0xF0, 0x50, 0x52, 0x1E, 0x68, 0x40,
        0x38, 0xFF, 0x36, 0x26, 0x38, 0xFF, 0x36, 0x24,
        0x38, 0xE8, 0x21, 0xFD, 0x83, 0xC4, 0x0C, 0xE9,
        0x11, 0xFF, 0xC4, 0x1E, 0x24, 0x38, 0xFF, 0x06,
        0x24, 0x38, 0x26, 0xC6, 0x07, 0x00, 0xA1, 0x24,
        0x38, 0x2B, 0x46, 0x06, 0x48, 0x1F, 0xC9, 0xCB,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x72, 0x65, 0x71, 0x75, 0x69,   /* "requi"    */
        0x72, 0x65, 0x73, 0x20, 0x44, 0x4F, 0x53, 0x2F,   /* "res DOS/" */
        0x31, 0x36, 0x4D, 0x0A, 0x0D, 0x24,               /* "16M\n\r$"  */
        0xB4, 0x30, 0xCD, 0x21, 0x3C, 0x02, 0x73, 0x05 }; /* mov ah,30h / int 21h / cmp al,2 / jae */
      CHECK(b[128] == 0xCD && b[129] == 0x21, "fixture holds DOS/4GW's version-check int 21h");
      CHECK(!x86_is_insn_start(b, 128, sizeof b, 0),
            "DOS/4GW +0x2c65: the vote alone CANNOT see it (ASCII before it)");
      /* ★★ SESSION 39: THIS ASSERTION IS INVERTED ON PURPOSE.  It used to read
           `x86_int_site_is_real(...)` -- KEEP -- because refusing a real site left a
           raw `CD nn` in protected mode and that was fatal.  It is not fatal any
           more: since session 34 a #GP with the IDT bit set IS the interrupt, and the
           host services it and patches the site from the fault, where the CPU has
           already proved the bytes are an instruction.  So a false reject now costs
           one #GP and a false accept still costs silent code corruption -- see
           x86len.h.  This site is REJECTED now, faults once, and is patched correctly.
         ⚠ Flipping this back without also removing the #GP(IDT) arm would restore
           the krnl386 corruption the next fixture pins. */
      CHECK(!x86_int_site_is_real(b, 128, sizeof b, 0),
            "...and it is now REJECTED, to be serviced from the #GP instead"); }

    /* ── ★★★ THE FALSE POSITIVE THAT KILLED THE WIN16 LAUNCH (session 39, GH #128).
         Real bytes, krnl386.exe seg1 0x201a..0x2058, candidate at index 56 = 0x2052:
             3a cd   cmp cl,ch
             75 50   jne +0x50
         The `cd 75` spans them.  The vote fails, the owner IS named -- and it is a
         `cmp`, not a relative branch, so the old rule kept it.  `cd 75` became
         `c4 c4`, the `jne` at 0x2053 became `les dx,[bx+si+0x0b]`, and WOWEXEC died
         with "General Protection Fault in module KRNL386.EXE at 0001:2053" the moment
         it tried to launch an application.  The owner test is what has to catch this,
         and "owner exists" is the only property that separates it -- the owning
         instruction class does not. */
    { static const unsigned char b[] = {
        0xAC,0x3A,0xC3,0x74,0x07,0x3A,0xC7,0x74,
        0x03,0xE9,0x88,0x00,0x83,0x7E,0xFC,0x00,
        0x75,0x0D,0x83,0x7E,0xFA,0x00,0x74,0x07,
        0xAA,0xAC,0xFF,0x46,0xFC,0xEB,0x15,0x38,
        0x1C,0x74,0x04,0x38,0x3C,0x75,0x03,0x46,
        0xEB,0xF5,0xFF,0x4E,0xF8,0x79,0x05,0xC7,
        0x46,0xF8,0x00,0x00,0xFF,0x46,0xFE,0x3A,
        0xCD,0x75,0x50,0x0B,0xC9,0x75 };
      CHECK(b[56] == 0xCD && b[57] == 0x75, "fixture holds krnl386's CD 75 byte pair");
      CHECK(!x86_is_insn_start(b, 56, sizeof b, 0),
            "krnl386 seg1:0x2052: the vote correctly says it is no instruction start");
      CHECK(!x86_int_site_is_real(b, 56, sizeof b, 0),
            "krnl386 seg1:0x2052: `cmp cl,ch` owns it -- REJECT (was the 0001:2053 GP)"); }

    /* DOS/4GW's `jmp short` displacement, in both its 16-bit modules: `eb cd` reads as
       a `cd 33` byte pair. Same class as Doom's, different branch, 16-bit code. */
    { static const unsigned char b[] = {
        0x8B,0xC7,0x2B,0xC6,0x8B,0xF8,0xB9,0x00,0x02,0x2B,0xCF,0x73,0x96,
        0xB0,0x22,                      /* mov al,0x22            */
        0xAA,                           /* stosb                  */
        0xEB,0xCD,                      /* jmp -51                */
        0x33,0xC0,                      /* xor ax,ax              */
        0xAA,0x16,0x1F };               /* stosb / push ss / pop ds */
      CHECK(b[17] == 0xCD && b[18] == 0x33, "fixture holds the CD 33 byte pair");
      CHECK(!x86_int_site_is_real(b, 17, sizeof b, 0),
            "DOS/4GW: `jmp short` displacement is NOT an int 0x33"); }

    /* Offset 0 has nothing before it to vote, and the region start is where the
       object begins -- trust it rather than reject every site in the first 48 bytes. */
    { static const unsigned char b[] = { 0xCD, 0x21, 0x90, 0x90 };
      CHECK(x86_is_insn_start(b, 0, sizeof b, 1), "offset 0 is trusted"); }

    printf("\n%d checks, %d failed\n", total, fails);
    return fails ? 1 : 0;
}

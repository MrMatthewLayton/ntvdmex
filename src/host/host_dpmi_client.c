/* NTVDMEX -- An NTVDM replacement for Microsoft Windows
 *
 * A DPMI client session: starting it, running it in slices, reflecting its faults to its handlers, delivering its interrupts, ending it.
 *
 * Part of the host's single translation unit: #included by main.c.
 *
 *
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Matthew Layton
 */

/* A #GP through an IDT gate is an unserviced INT nn the client issued, not an exception it asked for: reflect it as the interrupt, and patch the site so the next pass takes the BOP path. */
static INT DpmiServiceIdtGateFault(
    PSTR *cursorIo,
    PSTR const base,
    volatile WORD * const frame,
    const DWORD faultEsp,
    const DWORD faultSs,
    const DWORD faultEip,
    volatile BYTE * const tib,
    DOS_MACHINE *machine,
    const UINT steps)
{
    PSTR cursor = *cursorIo;

    if ((frame[DPMI_FRAME_ERROR] & X86_ERROR_CODE_IDT) && HostReadable((const VOID *)frame, DPMI_FRAME16_SIZE))
    {
        DWORD gateVector = (DWORD)(DPMI_SELECTOR_INDEX(frame[DPMI_FRAME_ERROR])) & BYTE_MASK;
        DWORD guestCodeBase  = DpmiSelectorBase(frame[DPMI_FRAME_CS]);
        volatile BYTE *guestInstruction = (volatile BYTE *)(ULONG_PTR)(guestCodeBase + frame[DPMI_FRAME_IP]);
        /* -- [CAUTION] WHAT MAKES THE FRAME'S IP TRUSTWORTHY IS THE
         * SELECTOR'S BASE, NOT ITS WIDTH. (s74)
         * This began as `if (guestCodeBase && ...)`. DpmiSelectorBase() returns
         * g_Ldt[idx].base, so a base of 0 -- exactly what a FLAT
         * 32-bit client runs on (`desc 0x0000ffff:0x00cffa00`, base 0,
         * limit 4 GB, D/B=1) -- was being read as "no selector" and
         * those faults declined by accident.
         *
         * [CAUTION]: The first attempt at this replaced the test with "decline
         * any 32-bit CS", and THAT WAS A REGRESSION, measured: the
         * serviced count on heaven7 fell 62 -> 43, because a 32-bit
         * CS with a NON-ZERO base (0x287, base ~0x48000) has a
         * perfectly good IP and had been serviced all along. Width
         * was never the issue.
         * The real issue is narrow and it is this: NT hands us a
         * 16-BIT exception frame whatever the client is, so when the
         * base is 0 the EIP *is* the linear address and arrives with
         * its top 16 bits gone -- heaven7 reports 0x231c for an
         * instruction living at ~0x0433231c. Then `guestCodeBase + fr[3]` names
         * LOW MEMORY, and a chance `CD nn` match there would make us
         * write C4 C4 into an innocent page: the eager patcher's own
         * failure mode, relocated.
         *
         * So: trust `guestCodeBase + fr[3]` whenever the base is non-zero (as
         * before), and for the flat base-0 case RECONSTRUCT the
         * address from the client's own 0501 blocks, requiring a
         * UNIQUE hit that actually holds `CD <vec>`. Unique-or-decline
         * is evidence; picking the first match would be a guess.
         */
        UINT32 guestAccessRights = 0;
        INT guestPresent = DpmiSelectorDescriptor(frame[DPMI_FRAME_CS], &guestAccessRights, NULL);
        INT guestIs32    = guestPresent && (((guestAccessRights >> X86_DESCRIPTOR_FLAGS_SHIFT) & DPMI_DESCRIPTOR_FLAGS_MASK) & DPMI_DESCRIPTOR_FLAG_BIG);
        INT guestTruncated   = guestIs32 && guestCodeBase == 0;   /* EIP *is* the linear addr */
        INT guestCandidates    = 0;
        DWORD guestLinear   = guestCodeBase + frame[DPMI_FRAME_IP];
        DWORD guestRecovered   = 0;
        INT   guestSource   = GUEST_EIP_FROM_FRAME;                   /* 0 frame, 1 TIB slot, 2 blocks */
        INT   guestSsIs32  = DpmiSelectorIs32(frame[DPMI_FRAME_SS]);
        /* the slot must agree with the frame's low halves, or it is not
         * the slot we calibrated -- then we do not resume on it
         */
        INT   guestEspOk = !guestSsIs32 || ((faultEsp & WORD_MASK_U) == frame[DPMI_FRAME_SP] && (faultSs & WORD_MASK_U) == frame[DPMI_FRAME_SS]);
        /* -- THE FULL-WIDTH REGISTERS ARE IN THE TIB; THE FRAME IS
         * THE TRUNCATED COPY. (s74, second pass) ---------------------
         * The kernel saves the faulting SS:ESP and EIP at full width
         * BEFORE it builds the 16-bit DPMI frame: `faultSs:faultEsp` (from the misnamed
         * VTIB_FLT_SAVCS/SAVEIP) is SS:ESP and `faultEip` (VTIB_FLT_SAV3) is EIP. That is
         * not a reading of the layout, it is three faults with every
         * field known independently, from one heaven7 run:
         *     based CS 0x287:0x0119   sav3=0x00000119  savSS:ESP=0x297:0x5874
         *     flat  CS 0x347 -> lin 0x04332335 (blocks, unique)
         *                             sav3=0x04332335  savSS:ESP=0x34f:0x8610
         *   and the frame's fr[7]:fr[6] equalled the low halves each time.
         * So the flat base-0 EIP need not be REBUILT from the client's
         * 0501 blocks; it is in the slot. The block walk stays as a
         * CROSS-CHECK (logged: agree / disagree / ambiguous) and as the
         * fallback if the slot's address does not hold `CD vec`.
         *
         * [CAUTION]: AND THE SAME TRUNCATION APPLIES TO ESP, which this arm had
         * been restoring from fr[6] -- 16 bits -- while its own comment
         * claimed a flat-SS client was "declined above". Nothing declined
         * it. heaven7 survived only because its SS is BASED with SP <
         * 64 KB; a Watcom flat-model client (Doom: DS=SS=flat, ESP ~
         * 0x0043xxxx) resumed through here would have lost the top half
         * of its stack pointer on the first lazily-serviced INT. Restore
         * ESP from the slot whenever SS is 32-bit, and refuse to resume
         * if the slot and the frame disagree in the low half -- that is
         * the one check that would catch a mis-identified slot.
         */
        if (guestTruncated)
        {
            guestRecovered = DpmiRecoverFlatEip((DWORD)frame[DPMI_FRAME_IP], (BYTE)gateVector, &guestCandidates);

            if ((faultEip & WORD_MASK_U) == frame[DPMI_FRAME_IP]
                && HostReadable((const VOID *)(ULONG_PTR)faultEip, X86_INT_LENGTH)
                && ((const volatile BYTE *)(ULONG_PTR)faultEip)[0] == X86_OP_INT
                && ((const volatile BYTE *)(ULONG_PTR)faultEip)[1] == (BYTE)gateVector)
            {
                guestLinear = faultEip;
                guestSource = GUEST_EIP_FROM_TIB_SLOT;
            }
            else
            {
                guestLinear = guestRecovered;
                guestSource = GUEST_EIP_FROM_BLOCKS;      /* 0 = ambiguous or absent */
            }

            guestInstruction   = (volatile BYTE *)(ULONG_PTR)guestLinear;

            if (!guestLinear && !g_Fault32Warned)
            {
                CHAR wowLine3[288];
                CHAR *wowCursor3 = wowLine3;
                g_Fault32Warned = 1;
                wowCursor3 = LogPut(wowCursor3, "  EXC: #GP(IDT) vec=0x"); wowCursor3 = LogHex(wowCursor3, gateVector);
                wowCursor3 = LogPut(wowCursor3, " in a FLAT base-0 32-bit CS 0x");
                wowCursor3 = LogHex(wowCursor3, frame[DPMI_FRAME_CS]);
                wowCursor3 = LogPut(wowCursor3, ": NT's frame is 16-bit so the EIP arrived"
                              " truncated (0x");
                              wowCursor3 = LogHex(wowCursor3, frame[DPMI_FRAME_IP]);
                wowCursor3 = LogPut(wowCursor3, "); TIB sav3=0x");
                wowCursor3 = LogHex(wowCursor3, faultEip);
                wowCursor3 = LogPut(wowCursor3, " does not hold CD "); wowCursor3 = LogHexByte(wowCursor3, (UINT)gateVector);
                wowCursor3 = LogPut(wowCursor3, ", and reconstruction from the client's"
                              " 0501 blocks found ");
                              wowCursor3 = LogHex(wowCursor3, (DWORD)guestCandidates);
                wowCursor3 = LogPut(wowCursor3, " candidates -- need exactly 1. Reflecting instead.\r\n");
                LogAppend(LOG_PATH, wowLine3, wowCursor3); SerialOut(wowLine3, wowCursor3);
            }
        }

        if (guestSsIs32 && !guestEspOk && !g_Fault32Warned)
        {
            CHAR wowLine4[224];
            CHAR *wowCursor4 = wowLine4;
            g_Fault32Warned = 1;
            wowCursor4 = LogPut(wowCursor4, "  EXC: #GP(IDT) vec=0x"); wowCursor4 = LogHex(wowCursor4, gateVector);
            wowCursor4 = LogPut(wowCursor4, " with a 32-bit SS 0x"); wowCursor4 = LogHex(wowCursor4, frame[DPMI_FRAME_SS]);
            wowCursor4 = LogPut(wowCursor4, ": TIB savSS:savESP=0x"); wowCursor4 = LogHex(wowCursor4, faultSs);
            wowCursor4 = LogPut(wowCursor4, ":0x"); wowCursor4 = LogHex(wowCursor4, faultEsp);
            wowCursor4 = LogPut(wowCursor4, " does not match the frame's 0x"); wowCursor4 = LogHex(wowCursor4, frame[DPMI_FRAME_SS]);
            wowCursor4 = LogPut(wowCursor4, ":0x"); wowCursor4 = LogHex(wowCursor4, frame[DPMI_FRAME_SP]);
            wowCursor4 = LogPut(wowCursor4, " -- cannot restore a full ESP. Reflecting instead.\r\n");
            LogAppend(LOG_PATH, wowLine4, wowCursor4); SerialOut(wowLine4, wowCursor4);
        }

        if (guestPresent && guestLinear && guestEspOk
            && HostReadable((const VOID *)guestInstruction, X86_INT_LENGTH)
            && guestInstruction[0] == X86_OP_INT && guestInstruction[1] == (BYTE)gateVector)
        {
            /* -- THIS IS THE PASS THAT RE-PATCHED CALC'S FP SITE.
             * (session 56 -- the question session 55 left open.)
             * Session 55 put a 34h..3Fh guard in the SCANNER and the
             * guard was right, but it was in the wrong place: the
             * scanner never touched CALC's site. THIS did. Measured,
             * from the session-55 CALC log:
             *
             * EXC: #GP(IDT) is a RAW INT 0x39 at 0x0b77:0x05c6
             *      lin=0x03d15b66 -- servicing + patching     x120,673
             *
             * The comment below is right that the CPU's error code
             * names the vector and the address, and that this is the
             * strongest evidence a byte pair is really an INT. It is
             * still not evidence that the byte pair is an INTERRUPT.
             * `CD 39` in FP-emulator code IS the x87 instruction, and
             * the fault is how the emulator is ENTERED, not a failure.
             * Rewriting it to a BOP destroys the guest's own encoding
             * -- which is exactly what the scanner was stopped from
             * doing, by a guard this path did not have.
             */
            INT guestResult;
            INT isFloatingPointVector = (gateVector >= VECTOR_FLOATING_POINT_FIRST && gateVector <= VECTOR_FLOATING_POINT_LAST);
            INT floatingPointHooked = isFloatingPointVector && g_PmInt[gateVector].Client;
            cursor = LogPut(cursor, "  EXC: #GP(IDT) is a RAW INT 0x"); cursor = LogHex(cursor, gateVector);
            cursor = LogPut(cursor, " at 0x"); cursor = LogHex(cursor, frame[DPMI_FRAME_CS]);
            cursor = LogPut(cursor, ":0x"); cursor = LogHex(cursor, frame[DPMI_FRAME_IP]);
            cursor = LogPut(cursor, " lin=0x"); cursor = LogHex(cursor, guestLinear);

            if (guestTruncated)
            {
                cursor = LogPut(cursor, guestSource == GUEST_EIP_FROM_TIB_SLOT ? " (flat base-0 CS: EIP from TIB sav3,"
                                        " blocks cross-check "
                                      : " (flat base-0 CS: EIP RECONSTRUCTED"
                                        " from blocks, TIB sav3=0x");

                if (guestSource == GUEST_EIP_FROM_TIB_SLOT)
                {
                    if (!guestRecovered)
                    {
                        cursor = LogPut(cursor, "ambiguous n=");
                        cursor = LogHex(cursor, (DWORD)guestCandidates);
                    }
                    else if (guestRecovered == guestLinear)
                        cursor = LogPut(cursor, "AGREE");
                    else
                    {
                        cursor = LogPut(cursor, "DISAGREE 0x");
                        cursor = LogHex(cursor, guestRecovered);
                    }
                }
                else
                    cursor = LogHex(cursor, faultEip);

                cursor = LogPut(cursor, ")");
            }

            if (guestSsIs32)
            {
                cursor = LogPut(cursor, " SS32 esp=0x"); cursor = LogHex(cursor, faultEsp);

                if ((faultEsp & WORD_MASK_U) != frame[DPMI_FRAME_SP])
                    cursor = LogPut(cursor, " ⚠ slot/frame DISAGREE");
            }

            if (floatingPointHooked)
            {
                cursor = LogPut(cursor, " -- FP EMULATOR RANGE: reflecting to the"
                            " handler the guest installed, 0x");
                cursor = LogHex(cursor, g_PmInt[gateVector].Selector);
                cursor = LogPut(cursor, ":0x"); cursor = LogHex(cursor, g_PmInt[gateVector].Offset);
                cursor = LogPut(cursor, " (not patched, not serviced)\r\n");
            }
            else
            {
                cursor = LogPut(cursor, isFloatingPointVector ? " -- FP EMULATOR RANGE but NO handler"
                                  " installed; servicing without patching\r\n"
                                : " -- servicing + patching\r\n");
            }

            LogAppend(LOG_PATH, base, cursor); SerialOut(base, cursor); cursor = base;
            /* put the guest back where it faulted, EIP ON the INT.
             *
             * [CAUTION]: For the flat base-0 case the frame's IP is TRUNCATED, so
             * resume on the full linear address (TIB slot, or the block
             * reconstruction) -- writing fr[3] back there would be a
             * wild jump into low memory. The frame's SP is truncated the
             * same way: for a 32-bit SS take ESP from the TIB slot, which
             * `guestEspOk` has just checked against the frame's low half.
             */
            VDM_SET16(tib, VTIB_SS, frame[DPMI_FRAME_SS]);
            VDM_REG(tib, VTIB_ESP) = guestSsIs32 ? faultEsp : (DWORD)frame[DPMI_FRAME_SP];
            VDM_SET16(tib, VTIB_CS, frame[DPMI_FRAME_CS]);
            VDM_REG(tib, VTIB_EIP) = guestTruncated ? (guestLinear - guestCodeBase) : (DWORD)frame[DPMI_FRAME_IP];
            VDM_SET16(tib, VTIB_EFLAGS, frame[DPMI_FRAME_FLAGS]);
            /* REFLECT IT, DO NOT SERVICE IT (Importance = 4):
             * An FP `CD nn` is not a request to the host; it is the
             * guest's own emulator being entered, and the ONLY thing
             * that knows where to resume is that emulator. It reads
             * the modrm and displacement that FOLLOW the two bytes,
             * then REWRITES THE RETURN IP ON THE STACK to step over
             * them. So build the frame the CPU would have built --
             * flags, CS, IP-past-the-INT, on the GUEST's stack -- and
             * let its IRET decide where it goes. Every host-side
             * mechanism we have (BOP + trampoline, or a service arm
             * that IRETs) throws that adjustment away.
             *
             * [CAUTION]: IF stays as the guest had it. An INT gate would clear
             * it, but the emulator is not an ISR: it runs as part of
             * the guest's own instruction stream, and this host's
             * V86/VME rules make silently clearing IF a real hazard
             * (see [[vme-vif-interrupt-gating]]). TF is cleared,
             * which is what a gate does and costs nothing.
             */
            if (floatingPointHooked)
            {
                DWORD stackBase   = DpmiSelectorBase(frame[DPMI_FRAME_SS]);
                INT   ss32 = guestSsIs32;
                DWORD stackPointer   = ss32 ? faultEsp : (DWORD)frame[DPMI_FRAME_SP];   /* full width, see guestEspOk */
                stackPointer = ss32 ? stackPointer - X86_WORD_SIZE : ((stackPointer - X86_WORD_SIZE) & WORD_MASK);
                PokeWord(stackBase + stackPointer, frame[DPMI_FRAME_FLAGS]);                    /* FLAGS */
                stackPointer = ss32 ? stackPointer - X86_WORD_SIZE : ((stackPointer - X86_WORD_SIZE) & WORD_MASK);
                PokeWord(stackBase + stackPointer, frame[DPMI_FRAME_CS]);                    /* return CS */
                stackPointer = ss32 ? stackPointer - X86_WORD_SIZE : ((stackPointer - X86_WORD_SIZE) & WORD_MASK);
                PokeWord(stackBase + stackPointer, (WORD)(frame[DPMI_FRAME_IP] + X86_INT_LENGTH));        /* return IP */
                VDM_REG(tib, VTIB_ESP) = stackPointer;
                VDM_SET16(tib, VTIB_CS, g_PmInt[gateVector].Selector);
                VDM_REG(tib, VTIB_EIP) = g_PmInt[gateVector].Offset & WORD_MASK;
                VDM_REG(tib, VTIB_EFLAGS) &= ~EFLAGS_TF_U;     /* TF */
                {
                    *cursorIo = cursor;
                    return HOST_FLOW_CONTINUE;
                }
            }

            if (!isFloatingPointVector && HostWritable((VOID *)(ULONG_PTR)guestInstruction, X86_INT_LENGTH))
            {
                guestInstruction[0] = VDM_BOP0;
                guestInstruction[1] = VDM_BOP1;
                PatchMapSet(guestLinear, (BYTE)gateVector);   /* the REAL site (s74) */
            }

            guestResult = DpmiServicePmInt(machine, tib, gateVector, steps);

            if (guestResult > 0) /* serviced -> keep running */
            {
                *cursorIo = cursor;
                return HOST_FLOW_CONTINUE;
            }

            if (guestResult == 0)
            {
                g_DpmiDone = 1;
            }

            {
                *cursorIo = cursor;
                return HOST_FLOW_BREAK;
            }
        }
    }

    *cursorIo = cursor;
    return HOST_FLOW_NEXT;
}

/* Deliver a PM fault to the exception handler the client registered (INT 31h 0203h): build its exception frame -- in the client's width, not the handler selector's -- and re-enter the client there. */
static INT DpmiDeliverToClientHandler(
    PSTR *cursorIo,
    PSTR const base,
    const DWORD esp,
    const DWORD stackBase,
    volatile WORD * const frame,
    volatile BYTE * const tib,
    const INT exception)
{
    PSTR cursor = *cursorIo;

    /* -- THE EXCEPTION FRAME'S WIDTH FOLLOWS THE CLIENT'S
     * MODE, NOT THE HANDLER SELECTOR'S D BIT. This is the same
     * rule DpmiDispatchToPmHandler() already documents for
     * INTERRUPT frames, and it was never applied here -- so a
     * 32-bit client got a 16-bit exception frame and read its
     * fields off the end of it.
     * - MEASURED, ZAR (GH #23). NT builds a SIXTEEN-BIT frame on
     *   the fault stack (8 words; the bytes are only coherent read
     *   that way). DOS/4GW declares itself 32-bit at the mode
     *   switch, so its #GP handler (entered at 0x0f:0x6abb) reads
     *   the faulting EIP and CS at frame +0x0C and +0x10 -- the DPMI
     *   32-BIT frame's slots (observed in its fault registers). In
     *   our 16-byte frame there is nothing at +0x10, so DS loaded
     *   ZERO and the handler faulted on its own first memory read --
     *   which re-entered it, for ever, until the log capped.
     * - AND THE RETURN CONFIRMS IT INDEPENDENTLY: the handler leaves
     *   with a 32-bit far return, popping EIGHT bytes -- the same
     *   tell (as with IRETD) that identified the interrupt-frame
     *   case.
     * - WHY THIS IS A REBUILD AND NOT A WIDER READ: the frame is the
     *   KERNEL'S, and it is 16-bit whatever the client is. So the
     *   32-bit frame is built BELOW NT's (which is left intact, so
     *   the logging above still reads the kernel's own values) and
     *   ESP is moved onto it.
     *
     * [CAUTION]: NT's fields ARE 16-BIT, so a fault in 32-bit code with an
     * EIP above 0xFFFF would arrive here already truncated by the
     * kernel -- this widens the frame, it cannot recover bits that
     * were never handed to us. Both of ZAR's faults are in 16-bit
     * DOS/4GW selectors (0x1a7 and 0x0f, both D/B=0) where the
     * question does not arise; a 32-bit-CS fault that resumes
     * wrongly should suspect this line first.
     */
    if (g_DpmiIsClient32)
    {
        DWORD newSp = (esp - DPMI_FRAME32_SIZE) & WORD_MASK;
        volatile DWORD *d32 =
            (volatile DWORD *)(ULONG_PTR)(stackBase + newSp);

        if (HostReadable((const VOID *)d32, DPMI_FRAME32_SIZE))
        {
            d32[DPMI_FRAME_RETURN_IP] = (DWORD)DPMI_FLTRET_COFF;  /* return EIP */
            d32[DPMI_FRAME_RETURN_CS] = g_DpmiFaultCodeSelector;      /* return CS */
            d32[DPMI_FRAME_ERROR] = frame[DPMI_FRAME_ERROR];                    /* error code */
            d32[DPMI_FRAME_IP] = frame[DPMI_FRAME_IP];                    /* fault EIP */
            d32[DPMI_FRAME_CS] = frame[DPMI_FRAME_CS];                    /* fault CS */
            d32[DPMI_FRAME_FLAGS] = frame[DPMI_FRAME_FLAGS];                    /* EFLAGS */
            d32[DPMI_FRAME_SP] = frame[DPMI_FRAME_SP];                    /* fault ESP */
            d32[DPMI_FRAME_SS] = frame[DPMI_FRAME_SS];                    /* fault SS */
            VDM_REG(tib, VTIB_ESP) =
                (VDM_REG(tib, VTIB_ESP) & HIGH_WORD_MASK_U) | newSp;
        }
        else
        {
            cursor = LogPut(cursor, "  EXC: !! 32-bit frame site unreadable at 0x");
            cursor = LogHex(cursor, stackBase + newSp);
            cursor = LogPut(cursor, " -- delivering the KERNEL'S 16-bit frame, which"
                        " a 32-bit client will misread\r\n");
            frame[DPMI_FRAME_RETURN_IP] = (WORD)DPMI_FLTRET_COFF;
            frame[DPMI_FRAME_RETURN_CS] = g_DpmiFaultCodeSelector;
        }
    }
    else
    {
        frame[DPMI_FRAME_RETURN_IP] = (WORD)DPMI_FLTRET_COFF;      /* return IP */
        frame[DPMI_FRAME_RETURN_CS] = g_DpmiFaultCodeSelector;         /* return CS */
    }

    VDM_SET16(tib, VTIB_CS,  g_PmException[exception].Selector);
    VDM_REG(tib, VTIB_EIP) = g_PmException[exception].Offset;
    cursor = LogPut(cursor, "  EXC: -> client handler for 0x"); cursor = LogHex(cursor, (DWORD)exception);
    cursor = LogPut(cursor, " at 0x"); cursor = LogHex(cursor, g_PmException[exception].Selector);
    cursor = LogPut(cursor, ":0x"); cursor = LogHex(cursor, g_PmException[exception].Offset);
    cursor = LogPut(cursor, " frame{err=0x"); cursor = LogHex(cursor, frame[DPMI_FRAME_ERROR]);
    cursor = LogPut(cursor, " cs:ip=0x"); cursor = LogHex(cursor, frame[DPMI_FRAME_CS]);
    cursor = LogPut(cursor, ":0x"); cursor = LogHex(cursor, frame[DPMI_FRAME_IP]);
    cursor = LogPut(cursor, " fl=0x"); cursor = LogHex(cursor, frame[DPMI_FRAME_FLAGS]);
    cursor = LogPut(cursor, " ss:sp=0x"); cursor = LogHex(cursor, frame[DPMI_FRAME_SS]);
    cursor = LogPut(cursor, ":0x"); cursor = LogHex(cursor, frame[DPMI_FRAME_SP]);
    cursor = LogPut(cursor, "} retf-> 0x"); cursor = LogHex(cursor, g_DpmiFaultCodeSelector);
    cursor = LogPut(cursor, ":0x"); cursor = LogHex(cursor, (DWORD)DPMI_FLTRET_COFF);
    /* WHAT THE FAULTING INSTRUCTION WAS LOOKING AT (Importance = 1):
     * The frame says WHERE it faulted; on a #GP that is half
     * the question, because the other half is always "which
     * selector, and did the offset fit inside it". Session 34
     * spent a run on a krnl386 #GP through ES unable to
     * say whether ES was the wrong selector or the right one
     * with too small a limit -- from a log that had already
     * printed the address. The reflect leaves the guest's GPRs
     * and DS/ES alone (only CS/SS/ESP move), so this is free.
     * Print the bytes at the fault too: a fault you can decode
     * is a fault you can attribute without a second run.
     */
    cursor = LogPut(cursor, "\r\n       fault regs: eax=0x"); cursor = LogHex(cursor, VDM_REG(tib, VTIB_EAX));
    cursor = LogPut(cursor, " ebx=0x"); cursor = LogHex(cursor, VDM_REG(tib, VTIB_EBX));
    cursor = LogPut(cursor, " ecx=0x"); cursor = LogHex(cursor, VDM_REG(tib, VTIB_ECX));
    cursor = LogPut(cursor, " edx=0x"); cursor = LogHex(cursor, VDM_REG(tib, VTIB_EDX));
    cursor = LogPut(cursor, " esi=0x"); cursor = LogHex(cursor, VDM_REG(tib, VTIB_ESI));
    cursor = LogPut(cursor, " edi=0x"); cursor = LogHex(cursor, VDM_REG(tib, VTIB_EDI));
    cursor = LogPut(cursor, " ebp=0x"); cursor = LogHex(cursor, VDM_REG(tib, VTIB_EBP));
    { WORD selectors[2];
    PCSTR registerNames[2] = { " ds", " es" };
    INT selectorIndex;
      selectors[0] = (WORD)VDM_REG16(tib, VTIB_DS);
      selectors[1] = (WORD)VDM_REG16(tib, VTIB_ES);

      for (selectorIndex = 0; selectorIndex < 2; ++selectorIndex)
      {
          UINT32 accessRights = 0;
          UINT32 descriptorLimit = 0;
          cursor = LogPut(cursor, registerNames[selectorIndex]); cursor = LogPut(cursor, "=0x"); cursor = LogHex(cursor, selectors[selectorIndex]);

          if (DpmiSelectorDescriptor(selectors[selectorIndex], &accessRights, &descriptorLimit))
          {
              cursor = LogPut(cursor, "{base=0x"); cursor = LogHex(cursor, DpmiSelectorBase(selectors[selectorIndex]));
              cursor = LogPut(cursor, " lim=0x"); cursor = LogHex(cursor, descriptorLimit);
              cursor = LogPut(cursor, " ar=0x"); cursor = LogHex(cursor, accessRights); cursor = LogPut(cursor, "}");
          }
          else
              cursor = LogPut(cursor, "{NO DESCRIPTOR}");
      } }

    /* -- AND WHAT ES POINTS AT. On a #GP through a selector,
     * the object is the evidence. Session 34's fault reads
     * `es:[0x28]` -- the in-memory module database's
     * ne_modtab -- and got 0x38a where the layout says 0x7c;
     * whether that database is malformed or simply is not at
     * ES cannot be decided from a register dump, only from
     * the bytes. 0x40 of them is the whole NE header.
     */
    { DWORD extraBase = DpmiSelectorBase((WORD)VDM_REG16(tib, VTIB_ES));
      const volatile BYTE *extraOrigin = (const volatile BYTE *)(ULONG_PTR)extraBase;
      cursor = LogPut(cursor, "\r\n       @es:0000 = ");

      if (extraBase && HostReadable((const VOID *)extraOrigin, 0x40))
           cursor = LogDump(cursor, (const VOID *)extraOrigin, 0x40);
      else
          cursor = LogPut(cursor, "<unreadable>"); }

    /* -- AND THE SAME FOR DS, for the same reason. ES is dumped
     * because a #GP is usually ABOUT a selector; DS is dumped
     * because the bottom of a guest's data segment is where its
     * own state lives, and "which branch did it take, and on
     * what" is answerable from those bytes when it is not
     * answerable from the registers. (ZAR, GH #23: DOS/16M keeps
     * its memory-manager INTERRUPT VECTOR at ds:0x34 -- 0x15
     * means "size memory with INT 15h AH=88h", anything else
     * means "use my own manager", and the second path is the one
     * that faults. One dump says which we are on.)
     */
    { DWORD dataBase = DpmiSelectorBase((WORD)VDM_REG16(tib, VTIB_DS));
      const volatile BYTE *dataOrigin = (const volatile BYTE *)(ULONG_PTR)dataBase;
      cursor = LogPut(cursor, "\r\n       @ds:0000 = ");

      if (dataBase && HostReadable((const VOID *)dataOrigin, 0x40))
           cursor = LogDump(cursor, (const VOID *)dataOrigin, 0x40);
      else
          cursor = LogPut(cursor, "<unreadable>");

      /* The named offsets from dsprobe.txt -- see the knob's
       * note. Four bytes each, so a WORD and the word after it
       * (a far pointer's two halves) both read in one line.
       */
      if (g_DsProbeCount && dataBase)
      {
          INT dataProbeIndex;
          cursor = LogPut(cursor, "\r\n       dsprobe:");

          for (dataProbeIndex = 0; dataProbeIndex < g_DsProbeCount; ++dataProbeIndex)
          {
              const volatile BYTE *dataProbeBytes = dataOrigin + g_DsProbe[dataProbeIndex];
              cursor = LogPut(cursor, " ds[0x"); cursor = LogHex(cursor, g_DsProbe[dataProbeIndex]);
              cursor = LogPut(cursor, "]=");

              if (HostReadable((const VOID *)dataProbeBytes, 4))
                   cursor = LogDump(cursor, (const VOID *)dataProbeBytes, 4);
              else
                  cursor = LogPut(cursor, "?? ");
          }
      }

      /* csprobe: the same against the FAULTING CODE SEGMENT, six
       * bytes each -- enough for `e8 rel16` plus what follows, which
       * is the shape being checked against the file on disk.
       */
      if (g_CsProbeCount)
      {
          DWORD codeBase2 = DpmiSelectorBase(frame[DPMI_FRAME_CS]);
          const volatile BYTE *codeOrigin =
              (const volatile BYTE *)(ULONG_PTR)codeBase2;
          INT codeProbeIndex;
          cursor = LogPut(cursor, "\r\n       csprobe:");

          for (codeProbeIndex = 0; codeProbeIndex < g_CsProbeCount; ++codeProbeIndex)
          {
              const volatile BYTE *codeProbeBytes = codeOrigin + g_CsProbe[codeProbeIndex];
              cursor = LogPut(cursor, " cs[0x"); cursor = LogHex(cursor, g_CsProbe[codeProbeIndex]);
              cursor = LogPut(cursor, "]=");

              if (codeBase2 && HostReadable((const VOID *)codeProbeBytes, 6))
                   cursor = LogDump(cursor, (const VOID *)codeProbeBytes, 6);
              else
                  cursor = LogPut(cursor, "?? ");
          }
      } }

    { DWORD frameCodeBase = DpmiSelectorBase(frame[DPMI_FRAME_CS]);
      const volatile BYTE *fi2 =
          (const volatile BYTE *)(ULONG_PTR)(frameCodeBase + frame[DPMI_FRAME_IP]);
      cursor = LogPut(cursor, " bytes@fault=");

      if (HostReadable((const VOID *)fi2, 8))
          cursor = LogDump(cursor, (const VOID *)fi2, 8);
      else
          cursor = LogPut(cursor, "<unreadable>");

      /* -- THE CODE AROUND THE FAULT, AND THE SELECTOR'S BASE.
       * Eight bytes AT the fault identify the instruction; they
       * do not identify WHERE IN THE GUEST'S IMAGE it came from,
       * and that is the question as soon as you start matching a
       * fault against a file on disk. A window either side can be
       * searched for in the binary, which either confirms the
       * file<->guest mapping or refutes it -- and this
       * investigation (GH #23) has now had TWO conclusions rest
       * on a mapping derived from a single 8-byte match.
       * The base is printed for the same reason: it is what turns
       * a selector:offset into the linear address pmbp.txt wants.
       */
      cursor = LogPut(cursor, "\r\n       csbase=0x"); cursor = LogHex(cursor, frameCodeBase);
      cursor = LogPut(cursor, " code[ip-0x20..ip+0x20]=");
      { const volatile BYTE *codeWindow =
            (const volatile BYTE *)(ULONG_PTR)(frameCodeBase + ((frame[DPMI_FRAME_IP] - 0x20) & WORD_MASK));

        if (frame[DPMI_FRAME_IP] >= 0x20 && HostReadable((const VOID *)codeWindow, 0x40))
             cursor = LogDump(cursor, (const VOID *)codeWindow, 0x40);
        else
            cursor = LogPut(cursor, "<unreadable>"); } }

    /* -- AND WHO CALLED. The frame says WHERE it faulted; on a
     * #GP inside a subroutine that is only half the question,
     * because the other half is always "how did the guest get
     * here" -- and a routine that faults on its FIRST memory
     * access has usually been entered in the wrong state rather
     * than gone wrong on its own. The return address is sitting
     * on the faulting stack, a few words up from SS:SP, and it
     * costs one dump to have it instead of a second run and a
     * breakpoint. (ZAR, GH #23: `push 8 / pop es` in DOS/16M.)
     */
    { DWORD sb2 = DpmiSelectorBase(frame[DPMI_FRAME_SS]);
      const volatile BYTE *stackBytes2 =
          (const volatile BYTE *)(ULONG_PTR)(sb2 + frame[DPMI_FRAME_SP]);
      cursor = LogPut(cursor, "\r\n       @ss:sp = ");

      if (sb2 && HostReadable((const VOID *)stackBytes2, 0x20))
           cursor = LogDump(cursor, (const VOID *)stackBytes2, 0x20);
      else
          cursor = LogPut(cursor, "<unreadable>"); }

    cursor = LogPut(cursor, "\r\n");
    LogAppend(LOG_PATH, base, cursor); SerialOut(base, cursor); cursor = base;
    { /* re-arm + re-enter, now in the handler */
        *cursorIo = cursor;
        return HOST_FLOW_CONTINUE;
    }
    *cursorIo = cursor;
    return HOST_FLOW_NEXT;
}

/* The client's exception handler returned through our fault-return address: take IP, CS, FLAGS, SP and SS from the frame it left, drop the error code, and resume the client there. */
static INT DpmiResumeAfterClientHandler(
    PSTR *cursorIo,
    PSTR const base,
    const DWORD event,
    const DWORD currentCs,
    const DWORD eip,
    volatile BYTE * const tib)
{
    PSTR cursor = *cursorIo;

    /* The client's exception handler has finished: its `retf` landed here:
     * DPMI 0.9: the handler returns through the CS:IP at the bottom of the
     * frame, having optionally rewritten the CS:IP / FLAGS / SS:SP above it
     * to say where execution should resume. krnl386's handler does exactly
     * that (observed) -- it points the resume elsewhere rather than back at its
     * own invalid opcode, which is the whole reason it raised one. After the
     * `retf` popped two words, SS:SP is at the error code, so what is left is
     *     +0x00 error code   +0x02 IP   +0x04 CS
     *     +0x06 FLAGS        +0x08 SP   +0x0a SS
     * and the resume is: take those, drop the error code, run.
     *
     * [CAUTION]: FLAGS IS SIXTEEN BITS. Assigning it to EFLAGS whole would clear the
     * high half -- VM, IOPL's neighbours, the lot -- so merge it.
     */
    if (event == VDM_EVENT_BOP && currentCs == (g_DpmiFaultCodeSelector & WORD_MASK)
        && eip == DPMI_FLTRET_COFF)
    {
        DWORD stackBase  = DpmiSelectorBase(g_DpmiFaultSelector);
        DWORD esp = VDM_REG16(tib, VTIB_ESP);
        volatile WORD *frame = (volatile WORD *)(ULONG_PTR)(stackBase + esp);

        if (!HostReadable((const VOID *)frame, DPMI_RETURNED16_SIZE))
        {
            cursor = LogPut(cursor, "GH#128: EXC RETURN but the frame at SS:SP is unreadable "
                        "-- stopping\r\n");
            LogAppend(LOG_PATH, base, cursor); SerialOut(base, cursor); cursor = base;
            {
                *cursorIo = cursor;
                return HOST_FLOW_BREAK;
            }
        }

        /* -- AND THE RETURN IS THE SAME QUESTION, SO IT GETS THE SAME
         * ANSWER. A 32-bit client's handler leaves by RETFD, which pops
         * EIGHT bytes, so ESP lands on the error code of a DWORD frame:
         *     +0x00 err  +0x04 EIP  +0x08 CS
         *     +0x0c EFLAGS  +0x10 ESP  +0x14 SS
         * Reading that as words would take the resume address from the
         * top half of the error code.
         * - THE FRAME IS READ BACK, NOT REMEMBERED, because rewriting it is
         *   the handler's documented lever: DOS/4GW's #GP handler resumes AT
         *   THE FAULTING INSTRUCTION having stored 0 over the bad selector on
         *   the faulting stack (it takes that stack from +0x18/+0x1c, which
         *   only exist in the 32-bit frame), so the `pop es` re-executes and
         *   succeeds. Resuming from anywhere we cached would defeat it.
         */
        if (g_DpmiIsClient32)
        {
            volatile DWORD *d32 = (volatile DWORD *)(ULONG_PTR)(stackBase + esp);

            if (!HostReadable((const VOID *)d32, DPMI_RETURNED32_SIZE))
            {
                cursor = LogPut(cursor, "GH#128: EXC RETURN (32) but the frame at SS:ESP is "
                            "unreadable -- stopping\r\n");
                LogAppend(LOG_PATH, base, cursor); SerialOut(base, cursor); cursor = base;
                {
                    *cursorIo = cursor;
                    return HOST_FLOW_BREAK;
                }
            }

            cursor = LogPut(cursor, "GH#128: EXC RETURN(32) -> resume 0x"); cursor = LogHex(cursor, d32[DPMI_RETURNED_CS]);
            cursor = LogPut(cursor, ":0x"); cursor = LogHex(cursor, d32[DPMI_RETURNED_IP]);
            cursor = LogPut(cursor, " fl=0x"); cursor = LogHex(cursor, d32[DPMI_RETURNED_FLAGS]);
            cursor = LogPut(cursor, " ss:esp=0x"); cursor = LogHex(cursor, d32[DPMI_RETURNED_SS]);
            cursor = LogPut(cursor, ":0x"); cursor = LogHex(cursor, d32[DPMI_RETURNED_SP]);
            cursor = LogPut(cursor, " err=0x"); cursor = LogHex(cursor, d32[DPMI_RETURNED_ERROR]);
            cursor = LogPut(cursor, "\r\n");
            LogAppend(LOG_PATH, base, cursor); SerialOut(base, cursor); cursor = base;
            VDM_SET16(tib, VTIB_SS,  (WORD)d32[DPMI_RETURNED_SS]);
            VDM_REG(tib, VTIB_ESP) = d32[DPMI_RETURNED_SP];
            VDM_SET16(tib, VTIB_CS,  (WORD)d32[DPMI_RETURNED_CS]);
            VDM_REG(tib, VTIB_EIP) = d32[DPMI_RETURNED_IP];
            /* [CAUTION]: FLAGS: still merged as sixteen bits. The high half carries VM
             * and IOPL's neighbours, and this frame's EFLAGS came from a
             * kernel frame that only ever held a word.
             */
            VDM_SET16(tib, VTIB_EFLAGS, (WORD)d32[DPMI_RETURNED_FLAGS]);
            {
                *cursorIo = cursor;
                return HOST_FLOW_CONTINUE;
            }
        }

        cursor = LogPut(cursor, "GH#128: EXC RETURN -> resume 0x"); cursor = LogHex(cursor, frame[DPMI_RETURNED_CS]);
        cursor = LogPut(cursor, ":0x"); cursor = LogHex(cursor, frame[DPMI_RETURNED_IP]);
        cursor = LogPut(cursor, " fl=0x"); cursor = LogHex(cursor, frame[DPMI_RETURNED_FLAGS]);
        cursor = LogPut(cursor, " ss:sp=0x"); cursor = LogHex(cursor, frame[DPMI_RETURNED_SS]);
        cursor = LogPut(cursor, ":0x"); cursor = LogHex(cursor, frame[DPMI_RETURNED_SP]);
        cursor = LogPut(cursor, " err=0x"); cursor = LogHex(cursor, frame[DPMI_RETURNED_ERROR]);
        cursor = LogPut(cursor, "\r\n");
        LogAppend(LOG_PATH, base, cursor); SerialOut(base, cursor); cursor = base;
        VDM_SET16(tib, VTIB_SS,  frame[DPMI_RETURNED_SS]);
        VDM_REG(tib, VTIB_ESP) = frame[DPMI_RETURNED_SP];
        VDM_SET16(tib, VTIB_CS,  frame[DPMI_RETURNED_CS]);
        VDM_REG(tib, VTIB_EIP) = frame[DPMI_RETURNED_IP];
        VDM_SET16(tib, VTIB_EFLAGS, frame[DPMI_RETURNED_FLAGS]);
        {
            *cursorIo = cursor;
            return HOST_FLOW_CONTINUE;
        }
    }

    *cursorIo = cursor;
    return HOST_FLOW_NEXT;
}

/* A PM fault the kernel reflected to our handler code selector (GH #18): decode it and report it, then service an unserviced INT nn, deliver the fault to the client's own handler, or end the run. */
static INT DpmiHandleReflectedFault(
    PSTR *cursorIo,
    PSTR const base,
    const DWORD event,
    const DWORD currentCs,
    const DWORD eip,
    volatile BYTE * const tib,
    DOS_MACHINE *machine,
    const UINT steps)
{
    PSTR cursor = *cursorIo;

    /* GH #18: the raw-PM-#GP reflect landed on our handler code selector. THIS
     * is the proof point: the kernel reflected a fault it used to swallow.
     */
    if (event == VDM_EVENT_BOP && currentCs == (g_DpmiFaultCodeSelector & WORD_MASK)
        && (eip == DPMI_FAULT_COFF
            || (eip >= DPMI_FAULT_SITE(0)
                && eip <  DPMI_FAULT_SITE(DOS_FLTSITE_N)
                && ((eip - DPMI_FAULT_SITE(0)) & (DOS_FLTSITE_SIZE - 1)) == 0)))
    {
        /* Which class the kernel dispatched through -- the site it landed on
         * names it. -1 = the legacy shared site, which now means "a class we
         * did not fill", not "we do not know".
         */
        INT faultClass = (eip == DPMI_FAULT_COFF)
                   ? -1 : (INT)((eip - DPMI_FAULT_SITE(0)) / DOS_FLTSITE_SIZE);
        DWORD faultSs  = *(volatile WORD  *)(tib + VTIB_FLT_SAVCS);
        DWORD faultEsp = *(volatile DWORD *)(tib + VTIB_FLT_SAVEIP);
        DWORD faultEip = *(volatile DWORD *)(tib + VTIB_FLT_SAV3);
        /* [CAUTION]: THESE TWO FIELDS ARE **NOT** CS:EIP, WHATEVER THEIR NAMES SAY.
         * Measured (session 19): the pair reads 0x00c7:0x...6f1e while the
         * guest's CS was 0x6f and its SS:ESP was 0x00c7:0x...6f14 -- i.e.
         * SS and ESP+0xa. Proved by accident and decisively: clearing the
         * junk top half of ESP changed this "EIP" from 0xb33b6f1e to
         * 0x00006f1e. A field that tracks ESP is not EIP. The raw window
         * below shows the layout; `sav3` (tib+0x640, 0x36af here) is the
         * likelier faulting EIP. Do not resume anything on faultSs:faultEsp until
         * the slots are calibrated against a fault at a KNOWN address --
         * pmfault's HLT/INT3 cannot do it (they die without reflecting),
         * so that needs a new variant that loads a bad selector.
         */
        cursor = LogPut(cursor, "GH#18: PM-FAULT REFLECTED class=");

        if (faultClass < 0)
            cursor = LogPut(cursor, "SHARED-SITE");
        else
            cursor = LogHex(cursor, (DWORD)faultClass);

        cursor = LogPut(cursor, " -- savSS:savESP=0x");
        cursor = LogHex(cursor, faultSs); cursor = LogPut(cursor, ":0x"); cursor = LogHex(cursor, faultEsp);
        cursor = LogPut(cursor, " (MISNAMED VTIB_FLT_SAVCS/SAVEIP) sav3=0x"); cursor = LogHex(cursor, faultEip);
        cursor = LogPut(cursor, " nest=0x"); cursor = LogHex(cursor, *(volatile WORD *)(tib + VTIB_FLT_NEST));
        /* THE KERNEL BUILDS A FRAME AND WE HAVE NEVER LOOKED AT IT (Importance = 1):
         * The reflect sets SS:ESP = [TIB+0x638]:0x1000 and then PUSHES:
         * session 33's run came out of it with liveESP = ...0x0fd0, i.e.
         * 0x30 bytes below the top. That frame is the only place the
         * faulting CS, the flags and the trap/error code can be -- the
         * VTIB_FLT_SAV* slots hold three values and we need six.
         *
         * [CAUTION]: THIS IS A DUMP, NOT A DECODE. Nothing here claims to know the
         * layout. It is printed against a fault whose every field is
         * already known independently -- krnl386's deliberate `0f ff`
         * (UD0) at CS:IP 0x01cf:0xc5f0, SS:SP=0x001f:0x0fea, #UD =
         * DPMI exception 6 -- so each slot can be identified by the value
         * in it rather than by a guess about the shape. Offsets are
         * printed with the dwords for exactly that reason: a dump whose
         * columns you have to count is half an instrument.
         * Read it, THEN write the decode.
         */
        { DWORD faultStackBase = (DWORD)(ULONG_PTR)g_FaultStack;
          const volatile DWORD *frame = (const volatile DWORD *)(ULONG_PTR)(faultStackBase + 0x0FC0);
          INT frameIndex;
          cursor = LogPut(cursor, "\r\n  FLTSTK sel=0x"); cursor = LogHex(cursor, g_DpmiFaultSelector);
          cursor = LogPut(cursor, " lin=0x"); cursor = LogHex(cursor, faultStackBase);
          cursor = LogPut(cursor, " top=0x1000 espNOW=0x");
          cursor = LogHex(cursor, VDM_REG(tib, VTIB_ESP));

          if (!HostReadable((const VOID *)frame, 0x40))
              cursor = LogPut(cursor, " <unreadable>");
          else for (frameIndex = 0; frameIndex < 16; ++frameIndex)
          {
              cursor = LogPut(cursor, "\r\n    +0x"); cursor = LogHex(cursor, 0x0FC0 + frameIndex * 4);
              cursor = LogPut(cursor, " = 0x"); cursor = LogHex(cursor, frame[frameIndex]);
          }

          cursor = LogPut(cursor, "\r\n "); }
        /* - READ THE LAYOUT OFF THE TIB INSTEAD OF TRUSTING THE OFFSETS.
         * VTIB_FLT_SAVCS/SAVEIP were reverse-engineered in an earlier
         * session, and what they return does not look like a code
         * address: the argument-run fault reported CS=0x00c7 -- which is
         * the guest's SS, not its CS (0x6f) -- and EIP=0xb33b6f1e, which
         * is that run's ESP (0xb33b6f14) plus 0xa. Dump the window so the
         * real slots can be identified by looking for the KNOWN CS and a
         * plausible EIP, rather than by guessing a frame shape.
         */
        { const BYTE *frameWords = (const BYTE *)(ULONG_PTR)(tib + 0x630);
          cursor = LogPut(cursor, " tib[630..64f]=");

          if (!HostReadable(frameWords, 0x20))
              cursor = LogPut(cursor, "<unreadable>");
          else
              cursor = LogDump(cursor, frameWords, 0x20); }

        cursor = LogPut(cursor, " liveCS=0x"); cursor = LogHex(cursor, VDM_REG16(tib, VTIB_CS));
        cursor = LogPut(cursor, " liveSS=0x"); cursor = LogHex(cursor, VDM_REG16(tib, VTIB_SS));
        cursor = LogPut(cursor, " liveESP=0x"); cursor = LogHex(cursor, VDM_REG(tib, VTIB_ESP));
        cursor = LogPut(cursor, " -- REAL-CPU PM fault reflect WORKS\r\n");
        LogAppend(LOG_PATH, base, cursor); SerialOut(base, cursor); cursor = base;
        /* DELIVER IT TO THE CLIENT'S INT 31h 0203 HANDLER (Importance = 2):
         * This `break` used to end the run here, which is why "DOS/4GW quits
         * on a command-line argument": ANY real PM fault produced a tidy
         * `exec loop exited -> flushing` that read exactly like the client
         * choosing to exit. It was not quitting; we were stopping it.
         *
         * [INFO]: THE FRAME IS ALREADY BUILT, AND NOT BY US. Measured on the rig
         * (session 34, against krnl386's deliberate `0f ff` at 0x01cf:0xc5f0
         * whose every field was known in advance): the kernel switches to
         * [TIB+0x638]:0x1000, pushes 0x10 bytes, and what it pushes IS the
         * DPMI 0.9 16-bit exception frame --
         *
         *   SS:SP+0x00  return IP   } LEFT ZERO for the host to fill
         *   SS:SP+0x02  return CS   }
         *   SS:SP+0x04  error code        0000
         *   SS:SP+0x06  faulting IP       c5f0   <- the UD0
         *   SS:SP+0x08  faulting CS       01cf
         *   SS:SP+0x0a  FLAGS             3246
         *   SS:SP+0x0c  faulting SP       0fea
         *   SS:SP+0x0e  faulting SS       001f
         *
         * -- which is not a coincidence: this machinery exists in NT FOR
         * ntvdm's DPMI, so it emits the shape DPMI specifies. krnl386's own
         * handler confirms the layout independently (observed): it rewrites
         * exactly the faulting IP and CS slots with a resume address and
         * leaves by `retf`, as DPMI 0.9 prescribes.
         *
         * So delivery is: fill the two return words with a BOP of ours,
         * point CS:EIP at the registered handler, and leave the kernel's
         * SS:ESP alone. The handler runs on the host stack the kernel
         * chose, and its `retf` comes back to us at DPMI_FLTRET_COFF.
         *
         * [CAUTION]: THE EXCEPTION NUMBER IS THE TABLE INDEX, AND THAT IS A READING,
         * NOT A MEASUREMENT. The frame carries no trap number; the only
         * channel is which entry of g_FaultTable the kernel dispatched
         * through. #UD (vector 6) arrived at index 6 -- consistent with
         * "index == vector", and equally consistent with "index == an NT
         * fault class that happens to cover #UD as well as the #GP this
         * table was first built for". Hence the guard below: deliver ONLY
         * to an exception the client has actually registered a handler
         * for. krnl386 registers one at a time and faults immediately, so
         * a wrong reading stops the run with a line that says which index
         * had no handler -- it does not call the wrong handler.
         */
        {
            INT exception = faultClass;
            DWORD stackBase  = DpmiSelectorBase(g_DpmiFaultSelector);
            DWORD esp = VDM_REG16(tib, VTIB_ESP);
            volatile WORD *frame = (volatile WORD *)(ULONG_PTR)(stackBase + esp);
            /* A #GP THROUGH AN IDT GATE IS AN UNSERVICED `INT nn` (Importance = 2):
             * Not an exception the client asked for -- a software interrupt
             * the host failed to intercept. The #GP error code says so
             * exactly: bit 1 (IDT) set and bits 3..15 the vector.
             *
             * [CAUTION]: WHY PATCHING ON COMMIT CANNOT COVER THIS. Our INT->BOP scan
             * runs when the client declares a region CODE, and krnl386
             * declares it BEFORE it copies the code in: measured, `04F2 ...
             * installed idx 1 base=0x03b30000 acc=0xfb` is preceded by
             * `code region 0x03b30000..0x03b3045f -> patched 00000000 INT
             * sites` -- the region was empty at declaration time. It then
             * copies SYSTEM.DRV in and executes a raw `cd 21` at
             * 0x000f:0x01f7. No commit-time hook can see content that has
             * not arrived, and session 33 hit the mirror image of this
             * (krnl386 copying code we HAD patched, carrying the `C4 C4`
             * but leaving the address-keyed vector behind).
             *
             * So service it HERE, where the CPU has just told us both the
             * vector and the address, and confirm against the instruction
             * bytes before believing the error code. This retires the whole
             * class: any `CD nn` in protected mode, in any code we never
             * scanned, becomes serviceable instead of fatal. It is also what
             * a DPMI host is supposed to do -- reflect the interrupt -- and
             * it is strictly better than a scan, which can only ever cover
             * memory it was pointed at while the content was there.
             * The site is patched on the way past, so the second execution
             * takes the fast BOP path: `C4 C4` in place (same length, as
             * always) plus the vector in the address-keyed map. That patch
             * needs no length heuristic -- the CPU just executed these two
             * bytes AS an interrupt, which is the strongest evidence the
             * x86len vote was ever trying to approximate.
             */
            {
                INT flow = DpmiServiceIdtGateFault(&cursor, base, frame, faultEsp, faultSs, faultEip, tib, machine, steps);

                if (flow == HOST_FLOW_BREAK)
                {
                    *cursorIo = cursor;
                    return HOST_FLOW_BREAK;
                }

                if (flow == HOST_FLOW_CONTINUE)
                {
                    *cursorIo = cursor;
                    return HOST_FLOW_CONTINUE;
                }
            }
            /* -- (C) THE MACHINE FELL IDLE: START THE PARKED TASK.
             * krnl386's creating task ends itself (observed: its
             * record goes, the current-task word at DGROUP 0x228
             * becomes 0, and it moves to a private kernel stack) --
             * and from then on the machine belongs to the scheduler.
             * The first code to touch the current task then loads
             * that 0 as a selector and reads through it -- a #GP
             * with err=0. That fault is not a defect,
             * it is the cue: nobody is running and somebody is
             * waiting. Resume them instead of reflecting.
             *
             * [CAUTION]: THIS TRUNCATES THE CREATOR. It had already retired, but
             * it was still freeing selectors when we took the machine
             * away, and those leak. Logged as the truncation it is,
             * because the honest fix is to park the creator too and
             * give it the rest of its turn later.
             */
            /* [INFO]: Every PSP we built, and what its environment field
             * holds NOW. Two faults in this epic were that field;
             * printing it at the fault turns "who wrote 1 there"
             * from a reading of candidate code into a reading of
             * the log.
             */
            if (g_WowPspCount)
            {
                INT probeIndex3;
                cursor = LogPut(cursor, "  PSPENV:");

                for (probeIndex3 = 0; probeIndex3 < g_WowPspCount; ++probeIndex3)
                {
                    const volatile BYTE *pspBytes = (const volatile BYTE *)
                        (ULONG_PTR)DpmiSelectorBase(g_WowPspSelector[probeIndex3]);
                    cursor = LogPut(cursor, " sel 0x"); cursor = LogHex(cursor, g_WowPspSelector[probeIndex3]);
                    cursor = LogPut(cursor, "->+0x2c=0x");

                    if (HostReadable((const VOID *)pspBytes, DOS_PSP_ENVIRONMENT + sizeof(WORD)))
                        cursor = LogHex(cursor, (DWORD)(pspBytes[DOS_PSP_ENVIRONMENT] | (pspBytes[DOS_PSP_ENVIRONMENT + 1] << BYTE_SHIFT)));
                    else
                        cursor = LogPut(cursor, "??unreadable");
                }

                cursor = LogPut(cursor, "\r\n");
            }

            INT schedSlot = g_WowSchedOn ? WowSchedPick(0) : -1;

            if (schedSlot >= 0 && WowSchedCurrentTask() == 0)
            {
                cursor = LogPut(cursor, "  WOWSCHED: [0x228]==0 -- the creator retired "
                            "and task 0x");
                cursor = LogHex(cursor, g_WowSchedSlots[schedSlot].Task);
                cursor = LogPut(cursor, " is parked. Resuming it INSTEAD of reflecting this "
                            "fault; the creator's remaining teardown is "
                            "ABANDONED (selectors leak).\r\n");
                /* [INFO]: VERIFY THE LAUNCH RESULT WE INVENTED AT (A). By now
                 * InitTask has NOT run for this task (it runs after we
                 * resume), so TDB+0x1c is still zero -- but the moment
                 * it is not, this line proves or breaks the SS&~1
                 * invariant, and a wrong hInstance is exactly the kind
                 * of thing that would otherwise fail three walls later
                 * with no trace back to here.
                 */
                {   DWORD taskBase = DpmiSelectorBase(g_WowSchedSlots[schedSlot].Task);

                    if (taskBase)
                    {
                        const volatile BYTE *taskBytes =
                            (const volatile BYTE *)(ULONG_PTR)taskBase;
                        WORD handle = (WORD)(taskBytes[WOW_TDB_INSTANCE] | (taskBytes[WOW_TDB_INSTANCE + 1] << BYTE_SHIFT));
                        cursor = LogPut(cursor, "  WOWSCHED: TDB+0x1c now reads 0x");
                        cursor = LogHex(cursor, handle);
                        cursor = LogPut(cursor, " (0 = InitTask has not run yet)\r\n");
                    }
                }
                LogAppend(LOG_PATH, base, cursor); SerialOut(base, cursor); cursor = base;
                WowSchedPoke(g_WowSchedSlots[schedSlot].ModeLinear, WOW32_MODE_ORDINARY);
                {   WORD toTask = g_WowSchedSlots[schedSlot].Task;
                    INT isTopLevel = WowSchedTopLevel(&g_WowSchedSlots[schedSlot]);

                    if (!g_WowSchedShell)
                        g_WowSchedShell = toTask;                     /* s92: WOWEXEC */

                    WowSchedRestore(&g_WowSchedSlots[schedSlot], tib);

                    if (isTopLevel)
                        g_WowSchedCurrentBase = g_WowCallDepth;

                    /* [INFO]: AND PUT THE CURRENT-TASK WORD BACK WITH IT.
                     * The creator zeroed it on its way out; the
                     * frame we are resuming was parked when it
                     * held this task. Restoring registers alone
                     * resumed the new task's code with krnl386
                     * still believing nobody is current -- see
                     * WowSchedSetCurrent for why that is part of the
                     * context and not the ruled-out "write
                     * [0x228] to yield".
                     */
                    WowSchedSetCurrent(toTask);
                    WowTaskChdir(toTask, &cursor);   /* #164 */
                }
                ++g_WowSchedSwitches;
                {
                    *cursorIo = cursor;
                    return HOST_FLOW_CONTINUE;
                }
            }

            if (exception < 0 || exception > X86_EXCEPTIONS - 1)
            {
                cursor = LogPut(cursor, "  EXC: no class (shared site) -- cannot name the "
                            "exception, stopping\r\n");
            }
            else if (!HostReadable((const VOID *)frame, DPMI_FRAME16_SIZE))
            {
                cursor = LogPut(cursor, "  EXC: frame at SS:SP is not readable, stopping\r\n");
            }
            else if (!g_PmException[exception].IsSet)
            {
                cursor = LogPut(cursor, "  EXC: exception 0x"); cursor = LogHex(cursor, (DWORD)exception);
                cursor = LogPut(cursor, " has NO client handler (INT 31h 0203 never called "
                            "for it) -- stopping rather than guessing\r\n");
            }
            else
            {
                {
                    INT flow = DpmiDeliverToClientHandler(&cursor, base, esp, stackBase, frame, tib, exception);

                    if (flow == HOST_FLOW_CONTINUE)
                    {
                        *cursorIo = cursor;
                        return HOST_FLOW_CONTINUE;
                    }
                }
            }

            LogAppend(LOG_PATH, base, cursor); SerialOut(base, cursor); cursor = base;
        }
        {
            *cursorIo = cursor;
            return HOST_FLOW_BREAK;
        }
    }

    *cursorIo = cursor;
    return HOST_FLOW_NEXT;
}

/* An async PM interrupt has come back: the client's handler IRETed into our catcher, so put the client back at the place in its own code that the injection interrupted. */
static INT DpmiFinishAsyncPmInterrupt(
    const DWORD event,
    const DWORD currentCs,
    const DWORD eip,
    volatile BYTE * const tib,
    DOS_MACHINE *machine,
    const UINT steps)
{
    /* AN ASYNC PM INTERRUPT HAS COME BACK:
     * DpmiAsyncInjectPm() rewrote the running thread's context to vector
     * at the client's handler, with the frame's return CS:EIP pointing at
     * our catcher -- so the handler's IRET lands here as a BOP. The IRET
     * restored the FLAGS we pushed but NOT the guest's place in its own
     * code, because that return address was the catcher's. Put the saved
     * context back and let the guest carry on as though nothing happened,
     * which is precisely what "transparent" means for a hardware interrupt.
     */
    if (event == VDM_EVENT_BOP && g_AsyncPmActive
        && currentCs == g_PmReturnSelector && eip == DPMI_PMRET_OFF)
    {
        VDM_SET16(tib, VTIB_CS, g_AsyncPmCs);
        VDM_REG(tib, VTIB_EIP)    = g_AsyncPmEip;
        VDM_SET16(tib, VTIB_SS, g_AsyncPmSs);
        VDM_REG(tib, VTIB_ESP)    = g_AsyncPmEsp;
        VDM_REG(tib, VTIB_EFLAGS) = g_AsyncPmEflags;
        g_DpmiVi = 1;                   /* unmask: the handler has finished */
        g_AsyncPmActive = 0;
        /* DRAIN THE BACKLOG WHILE WE STILL HAVE CONTROL:
         * One asynchronous delivery costs a SuspendThread /
         * GetThreadContext / SetThreadContext round trip -- tens of
         * microseconds. Doom programs the PIT to reload 0x4a, i.e.
         * 16124 Hz, and its millisecond delay waits `ms * scale / 1000`
         * ticks: about 480 for a 30 ms wait. One round trip per tick is
         * not a design at that rate, and the latch saturates at
         * IRQ0_PENDING_MAX=4 anyway, so chasing it delivered THREE ticks
         * against the hundreds owed and the guest never left its spin.
         * The asynchronous path's real job is to get us INTO a guest that
         * would otherwise never come out. Once here, the ISR can be run
         * directly and synchronously -- measured at phases=1, i.e. it
         * enters and IRETs with no excursion -- so one round trip serves a
         * whole batch. This is catch-up, the same shape the PIT already
         * uses when the host falls behind (pit_gaps), not an invention.
         */
        /* - SAY WHETHER THIS RAN, AND HOW FAR. Session 18 recorded
         * "coalescing the tick drain changed nothing (batch 64 -> 5
         * ticks, batch 3 -> 4)" and filed it as a dead end. But the
         * tick counter only ever advances ONE per async round trip,
         * which is what it would do if this batch never delivered --
         * and nothing here says which. Two numbers settle it: was the
         * gate taken, and what was k at exit.
         */
        { INT item = -1, gate;
          gate = (g_DpmiIsClient32 && g_PmAppHookedTimer && !g_PmNoIrq
                  && DpmiSelectorIs32((WORD)VDM_REG16(tib, VTIB_CS)));
          UINT32 dmaReadsBeforeBlock = g_Dma.ChannelCountReads[1];

          if (gate)
          {
            g_InPmIrq = 1;
            /* - DRAIN WHAT IS OWED, NOT A FIXED SIXTY-FOUR. This loop used to
             * run the full DPMI_IRQ0_BATCH every time it was entered, with
             * nothing tying it to elapsed time -- so the client's clock ran
             * at the rate we happened to return from asynchronous
             * injections rather than the rate it programmed into the 8254.
             * Measured on Doom at 140 Hz: 169,032 ISR entries in 45 s
             * against 6,300 owed, i.e. a game running 27x too fast. The
             * batch is still worth having -- one SuspendThread round trip
             * should repay a whole backlog -- but the backlog is a COUNT,
             * and PmTickTake() is where it lives.
             */
            /* - CONSUME A TICK ONLY IF IT WAS ACTUALLY DELIVERED.
             * DpmiInjectPmIrq() declines whenever the guest is in
             * the extender's 16-bit code rather than the application
             * -- a routine and correct refusal -- and taking the tick
             * first threw it away every time that happened. Measured
             * on Doom: 3,349 ISR entries in 45 s against the 6,300 it
             * programmed at 140 Hz, i.e. a game clock running at half
             * speed, which is most of "very laggy". The same mistake
             * as the keyboard's, with the sign reversed: there,
             * consuming late cancelled an interrupt the handler had
             * raised; here, consuming early discarded one nobody ran.
             */
            for (item = 0; item < DPMI_IRQ0_BATCH; ++item)
            {
                if (g_PmTickOwed <= 0)
                    break;

                if (!Irq0PmClaim())
                    break;                     /* last tick not EOI'd (#173) */

                if (!DpmiInjectPmIrq(machine, tib, VECTOR_TIMER, steps))
                {
                    Irq0PmUnclaim();
                    break;
                }

                InterlockedDecrement(&g_PmTickOwed);
            }

            g_InPmIrq = 0;
          }

          g_CooperativeDmaPolls += g_Dma.ChannelCountReads[1] - dmaReadsBeforeBlock;
          { CHAR breakLine[160], *breakCursor = breakLine;
            breakCursor = LogPut(breakCursor, "  BATCH gate="); breakCursor = LogHex(breakCursor, (DWORD)gate);
            breakCursor = LogPut(breakCursor, " k="); breakCursor = LogHex(breakCursor, (DWORD)item);
            breakCursor = LogPut(breakCursor, " c32="); breakCursor = LogHex(breakCursor, (DWORD)g_DpmiIsClient32);
            breakCursor = LogPut(breakCursor, " hooked="); breakCursor = LogHex(breakCursor, (DWORD)g_PmAppHookedTimer);
            breakCursor = LogPut(breakCursor, " cs=0x"); breakCursor = LogHex(breakCursor, VDM_REG16(tib, VTIB_CS));
            breakCursor = LogPut(breakCursor, "\r\n");
            LogAppend(LOG_PATH, breakLine, breakCursor);
            SerialOut(breakLine, breakCursor); } }
        {
            return HOST_FLOW_CONTINUE;
        }
    }

    return HOST_FLOW_NEXT;
}

/* Run the client until its next event: under the kernel's VDM monitor (pmkernel.flag), or on the
 * real CPU through the host's own protected-mode entry (DpmiEnterProtectedMode), timing the stretch.
 */
static INT DpmiRunClientSlice(
    PSTR *cursorIo,
    PSTR const base,
    volatile BYTE * const tib,
    const UINT steps)
{
    PSTR cursor = *cursorIo;

    if (g_DpmiUseKernel)
    {
        /* Hand the PM CONTEXT to the kernel monitor exactly as the V86
         * path does. Same TIB, same call; the only difference is that
         * EFLAGS.VM is clear and CS/SS/DS hold LDT selectors.
         */
        LONG runStatus = 0;
        DWORD runEvent;
        /* - THE TOP HALF OF ESP IS JUNK ON A 16-BIT STACK, AND THE KERNEL
         * READS ALL OF IT. With a 16-bit SS the CPU maintains SP only, so
         * whatever was last in the high half stays there -- the far-jmp
         * path stores that junk into the TIB on exit and reloads it
         * harmlessly with `lss`, because the CPU ignores it. The kernel
         * does not: it takes the CONTEXT's ESP whole. Measured, and it is
         * the difference between the entry that works and the one that
         * never returns:
         *   entry 0  ss:esp=0x1f:0x0000fffe   -> returns ev=4
         *   entry 1  ss:esp=0x17:0xb33afffa   -> never returns
         * 0xb33a is a host thread-stack address, and 0xb33afffa is far
         * outside a 0xFFFF-limit selector. Narrow it to what the
         * descriptor can actually address.
         */
        if (!DpmiSelectorIs32((WORD)VDM_REG16(tib, VTIB_SS)))
            VDM_REG(tib, VTIB_ESP) &= WORD_MASK_U;

        if (steps < 400) {      /* BEFORE the call: an entry that never
                                  returns leaves no other trace at all */
            cursor = LogPut(cursor, "PMKERNEL[");   cursor = LogHex(cursor, (UINT)steps);
            cursor = LogPut(cursor, "] enter cs:eip=0x"); cursor = LogHex(cursor, VDM_REG16(tib, VTIB_CS));
            cursor = LogPut(cursor, ":0x");         cursor = LogHex(cursor, VDM_REG(tib, VTIB_EIP));
            cursor = LogPut(cursor, " ss:esp=0x");  cursor = LogHex(cursor, VDM_REG16(tib, VTIB_SS));
            cursor = LogPut(cursor, ":0x");         cursor = LogHex(cursor, VDM_REG(tib, VTIB_ESP));
            cursor = LogPut(cursor, " efl=0x");     cursor = LogHex(cursor, VDM_REG(tib, VTIB_EFLAGS));
            cursor = LogPut(cursor, " msw=0x");     cursor = LogHex(cursor, *(volatile WORD *)(tib + VTIB_MSW));
            cursor = LogPut(cursor, " [0x714]=0x"); cursor = LogHex(cursor, *(volatile DWORD *)(ULONG_PTR)FIXED_NTVDMSTATE_LINEAR);
            cursor = LogPut(cursor, "\r\n");
            LogAppend(LOG_PATH, base, cursor); SerialOut(base, cursor); cursor = base;
        }

        g_PmEntryEip = (LONG)VDM_REG(tib, VTIB_EIP);
        runEvent = VdmRunGuest(tib, &runStatus);
        g_PmEntryEip = -1;

        if (steps < 400)
        {
            cursor = LogPut(cursor, "PMKERNEL[");     cursor = LogHex(cursor, (UINT)steps);
            cursor = LogPut(cursor, "] VdmStartExecution -> st=0x"); cursor = LogHex(cursor, (DWORD)runStatus);
            cursor = LogPut(cursor, " ev=0x");        cursor = LogHex(cursor, runEvent);
            cursor = LogPut(cursor, " cs:eip=0x");    cursor = LogHex(cursor, VDM_REG16(tib, VTIB_CS));
            cursor = LogPut(cursor, ":0x");           cursor = LogHex(cursor, VDM_REG(tib, VTIB_EIP));
            cursor = LogPut(cursor, " efl=0x");       cursor = LogHex(cursor, VDM_REG(tib, VTIB_EFLAGS));
            cursor = LogPut(cursor, "\r\n");
            LogAppend(LOG_PATH, base, cursor); SerialOut(base, cursor); cursor = base;
        }
    }
    else
    {
        /* HOW LONG DOES THE GUEST RUN WITHOUT GIVING US A TURN?:
         * Session 20's finding is that Doom dies inside a stretch of
         * protected-mode code that never BOPs, and that shortening the
         * stretch makes the VDM survive. That was measured in
         * INSTRUCTIONS, from a disassembly. Nothing has ever measured it
         * in TIME -- and time is what decides between "the guest
         * eventually does something illegal" and "a wall-clock deadline
         * in the kernel expires". One QueryPerformanceCounter pair per
         * PM entry is free next to the CreateFile-per-line logging this
         * host already does, and only stretches past the threshold say
         * anything, so a healthy run pays one comparison.
         * Report the ENTRY point as well as the exit: the entry is the
         * instruction after the BOP we last serviced, i.e. the name of
         * the stretch.
         */
        LARGE_INTEGER qpcStart;
        LARGE_INTEGER qpcEnd;
        DWORD stormCs = VDM_REG16(tib, VTIB_CS);
        DWORD stormEip = DpmiPmEip(tib);
        /* RE-ARM BEFORE EVERY ENTRY, BECAUSE THE TARGET APPEARS LATE (Importance = 2):
         * DpmiBreakpointArm() skips a site that still reads 00 00 -- correctly,
         * since arming into memory the client has not loaded yet was a
         * previous session's silent no-op. It is then only retried when a
         * code region is patched, and that was enough while the CLIENT's
         * own extender declared its modules.
         *
         * [CAUTION]: It is NOT enough now. krnl386 LOADS ITS OWN SEGMENTS: it copies
         * segment 1 to linear 0x20760 and installs the descriptor through
         * 04F2, and the copy may land after the last patch pass -- so a
         * breakpoint inside it is skipped at setup (still zeroes) and never
         * looked at again. Two breakpoints armed inside krnl386's segment 1
         * produced no "armed" line and no hit at all, which reads exactly
         * like "the guest never got there" and means nothing of the sort.
         * Cheap: a handful of entries, and only when a list was loaded. The
         * guest is the ONLY thing still running when the process is killed,
         * so this is the last instrument standing -- it has to actually arm.
         */
        if (g_BreakpointCount)
            DpmiBreakpointArm();

        /* The injections above run the client's own code; if it exited in
         * there, there is nothing left to enter. See g_PmClientExited.
         */
        if (g_PmClientExited)
        {
            *cursorIo = cursor;
            return HOST_FLOW_BREAK;
        }

        QueryPerformanceCounter(&qpcStart);
        DpmiEnterProtectedMode(tib);
        QueryPerformanceCounter(&qpcEnd);
        {
            DWORD microseconds = QpcMicroseconds(qpcEnd.QuadPart - qpcStart.QuadPart);

            if (microseconds > g_PmStretchMaximumMicroseconds)
            {
                g_PmStretchMaximumMicroseconds = microseconds;

                if (microseconds >= PM_STRETCH_LOG_US && g_PmStretchLogged++ < 256)
                {
                    cursor = LogPut(cursor, "PMSTRETCH us="); cursor = LogHex(cursor, microseconds);
                    cursor = LogPut(cursor, " entry=0x"); cursor = LogHex(cursor, stormCs);
                    cursor = LogPut(cursor, ":0x"); cursor = LogHex(cursor, stormEip);
                    cursor = LogPut(cursor, " lin=0x");
                    cursor = LogHex(cursor, DpmiSelectorBase((WORD)stormCs) + stormEip);
                    cursor = LogPut(cursor, " exit=0x");
                    cursor = LogHex(cursor, VDM_REG16(tib, VTIB_CS));
                    cursor = LogPut(cursor, ":0x"); cursor = LogHex(cursor, DpmiPmEip(tib));
                    cursor = LogPut(cursor, " ev=0x"); cursor = LogHex(cursor, VDM_REG(tib, VTIB_EVENT));
                    cursor = LogPut(cursor, " ms="); cursor = LogHex(cursor, GetTickCount());
                    cursor = LogPut(cursor, "\r\n");
                    LogAppend(LOG_PATH, base, cursor); SerialOut(base, cursor); cursor = base;
                }
            }
        }
    }

    *cursorIo = cursor;
    return HOST_FLOW_NEXT;
}

/* Bracket the client's first entries with checkpoints (session 16, Doom): arming the trampoline, entering PM, the first instruction and the return path, so a silent death names its step. */
static PSTR DpmiCheckpointFirstEntries(
    PSTR cursor,
    PSTR const base,
    const UINT steps,
    volatile BYTE * const tib)
{
    /* BRACKET THE FIRST ENTRIES (session 16, Doom):
     * Doom's log ends at the steps==0 [0x714] dump above and the process is
     * gone, with no fault, no banner and no INT 21h. Between that line and
     * the next thing that logs there are FOUR things that can kill us, and
     * nothing said which: arming the trampoline, entering PM, the guest's
     * first instruction, or the return path. So checkpoint each side for the
     * first few iterations -- cheap, self-limiting, and it turns "died
     * somewhere in here" into a named step. Bounded to 4096 so a healthy client
     * (millions of iterations) pays nothing.
     */
    if (steps < g_DpmiCpMaximum)
    {
        /* Dump the descriptor and the actual BYTES we are about to run. Doom
         * dies inside the FIRST DpmiEnterProtectedMode and never returns, so the only
         * thing that can tell a bad mode switch from a specific offending
         * instruction is knowing which instruction it was. Cheap: 4 iterations.
         */
        DWORD currentCodeBase = DpmiSelectorBase((WORD)g_DpmiEnterCs);
        cursor = LogPut(cursor, "DPMI-CP["); cursor = LogHex(cursor, (UINT)steps);
        cursor = LogPut(cursor, "] pre-arm cs:eip=0x"); cursor = LogHex(cursor, g_DpmiEnterCs);
        cursor = LogPut(cursor, ":0x"); cursor = LogHex(cursor, g_DpmiEnterEip);
        cursor = LogPut(cursor, " csbase=0x"); cursor = LogHex(cursor, currentCodeBase);
        cursor = LogPut(cursor, " ss:esp=0x"); cursor = LogHex(cursor, VDM_REG16(tib, VTIB_SS));
        cursor = LogPut(cursor, ":0x"); cursor = LogHex(cursor, VDM_REG(tib, VTIB_ESP));
        cursor = LogPut(cursor, " bytes@cs:eip=");
        /* GUARD THE INSTRUMENT -- with HostReadable(), NOT IsBadReadPtr.
         * csbase+eip need not be readable from the host's flat address
         * space, and an unguarded read would fault in our own diagnostic
         * and destroy the evidence we came for. IsBadReadPtr looks like
         * the guard for that but IS the same bug wearing a coat: it faults
         * on purpose, and DpmiCrashVeh sees the fault first.
         */
        { const BYTE *enterBytes = (const BYTE *)(ULONG_PTR)(currentCodeBase + g_DpmiEnterEip);

          if (!HostReadable(enterBytes, 16))
              cursor = LogPut(cursor, "<unreadable from host>");
          else
              cursor = LogDump(cursor, enterBytes, 16); }

        cursor = LogPut(cursor, "\r\n");
        LogAppend(LOG_PATH, base, cursor); SerialOut(base, cursor); cursor = base;
        /* WHERE DOES CONTROL GO NEXT?:
         * Session 17: Doom now clears every service it asks for and STILL
         * stops at the same instruction (0x0F:0x6644, a `xchg ax,cx / cbw /
         * retn` tail). The bytes at CS:EIP no longer explain anything --
         * they are three harmless instructions -- so the interesting address
         * is the one the RETN goes to, and the interesting state is what the
         * client is carrying into it. The old checkpoint could show neither.
         * Dump the register file and the top of the guest stack: the first
         * stack word IS the return address for the pending RETN, and that
         * turns "died somewhere after here" into a named next basic block.
         * - THE STACK ADDRESS MUST FOLLOW THE SS D/B BIT. With a 16-bit
         *   stack selector the CPU uses SP and leaves the top half of ESP
         *   holding whatever junk was there -- the first run of this dump
         *   read ESP=0xb3371474 against a base of 0x1100 and probed kernel
         *   space. That is not a corrupt guest; it is the architecture, and
         *   the same DpmiSelectorIs32() rule 0204/0205 already use.
         */
        { WORD ss = (WORD)VDM_REG16(tib, VTIB_SS);
          DWORD stackBase = DpmiSelectorBase(ss);
          DWORD stackPointer = DpmiSelectorIs32(ss) ? VDM_REG(tib, VTIB_ESP)
                                       : VDM_REG16(tib, VTIB_ESP);
          const BYTE *stackDump = (const BYTE *)(ULONG_PTR)(stackBase + stackPointer);
          cursor = LogPut(cursor, "DPMI-CP["); cursor = LogHex(cursor, (UINT)steps);
          cursor = LogPut(cursor, "] regs EAX=0x"); cursor = LogHex(cursor, VDM_REG(tib, VTIB_EAX));
          cursor = LogPut(cursor, " EBX=0x"); cursor = LogHex(cursor, VDM_REG(tib, VTIB_EBX));
          cursor = LogPut(cursor, " ECX=0x"); cursor = LogHex(cursor, VDM_REG(tib, VTIB_ECX));
          cursor = LogPut(cursor, " EDX=0x"); cursor = LogHex(cursor, VDM_REG(tib, VTIB_EDX));
          cursor = LogPut(cursor, " ESI=0x"); cursor = LogHex(cursor, VDM_REG(tib, VTIB_ESI));
          cursor = LogPut(cursor, " EDI=0x"); cursor = LogHex(cursor, VDM_REG(tib, VTIB_EDI));
          cursor = LogPut(cursor, " EBP=0x"); cursor = LogHex(cursor, VDM_REG(tib, VTIB_EBP));
          cursor = LogPut(cursor, " DS=0x"); cursor = LogHex(cursor, VDM_REG16(tib, VTIB_DS));
          cursor = LogPut(cursor, " ES=0x"); cursor = LogHex(cursor, VDM_REG16(tib, VTIB_ES));
          cursor = LogPut(cursor, " FS=0x"); cursor = LogHex(cursor, VDM_REG16(tib, VTIB_FS));
          cursor = LogPut(cursor, " GS=0x"); cursor = LogHex(cursor, VDM_REG16(tib, VTIB_GS));
          cursor = LogPut(cursor, " efl=0x"); cursor = LogHex(cursor, VDM_REG(tib, VTIB_EFLAGS));
          cursor = LogPut(cursor, " ssbase=0x"); cursor = LogHex(cursor, stackBase);
          cursor = LogPut(cursor, " sp=0x"); cursor = LogHex(cursor, stackPointer);
          cursor = LogPut(cursor, " stack@ss:sp=");

          if (!HostReadable(stackDump, 32))
              cursor = LogPut(cursor, "<unreadable from host>");
          else
              cursor = LogDump(cursor, stackDump, 32);

          /* AND THE FRAME AT SS:BP:
           * The client dies in DOS/4GW's HANDOFF to the application,
           * just past the last checkpoint: it loads its segment registers
           * and an IRET frame from the frame at SS:BP and enters the app
           * (observed: every value it loads is a word of that frame, all
           * of them selectors or a far entry point). Dumping the frame is
           * therefore the whole question: which descriptors it is about to
           * load, and where it is about to jump. Without it we would be
           * guessing which load faults; with it the answer is a lookup
           * against the descriptor calls already in this log.
           */
          { const BYTE *frame = (const BYTE *)(ULONG_PTR)
                (stackBase + VDM_REG16(tib, VTIB_EBP));
            cursor = LogPut(cursor, " frame@ss:bp=");

            if (!HostReadable(frame, 0x30))
                cursor = LogPut(cursor, "<unreadable from host>");
            else
            {
                cursor = LogDump(cursor, frame, 0x30);
                /* - AND THE CODE IT IS ABOUT TO JUMP TO. This half IS
                 * DOS/4GW-frame-specific and says so: [bp+0x22]:[bp+0x1e]
                 * is the far entry the handoff's IRET takes (as observed
                 * in the frame dump). The first
                 * frame dump proved every descriptor it loads is in range
                 * and correctly typed (SS=0xAF lim 0x7cff, SP=0x6F3E;
                 * DS/ES=0x17; CS=0x8F lim 0x5e3f, IP=0x2C63) -- so the
                 * fault is not the handoff, it is the FIRST INSTRUCTIONS
                 * OF THE MODULE, and those live in a block DOS/4GW read
                 * out of DOOM.EXE at runtime. They are not at any fixed
                 * file offset we can read offline; the only place they
                 * exist is guest memory, here, now. Hence the dump.
                 * Costs nothing when the frame is not a DOS/4GW one: the
                 * selector simply will not resolve to readable memory.
                 */
                { WORD frameCs = *(const WORD *)(frame + 0x22);
                  WORD frameIp = *(const WORD *)(frame + 0x1e);
                  const BYTE *entryBytes = (const BYTE *)(ULONG_PTR)
                      (DpmiSelectorBase(frameCs) + frameIp);
                  cursor = LogPut(cursor, " entry=0x"); cursor = LogHex(cursor, frameCs);
                  cursor = LogPut(cursor, ":0x"); cursor = LogHex(cursor, frameIp);
                  cursor = LogPut(cursor, " base=0x"); cursor = LogHex(cursor, DpmiSelectorBase(frameCs));
                  cursor = LogPut(cursor, " code@entry=");

                  if (!HostReadable(entryBytes, 64))
                      cursor = LogPut(cursor, "<unreadable from host>");
                  else
                      cursor = LogDump(cursor, entryBytes, 64); } } }

          cursor = LogPut(cursor, "\r\n");
          LogAppend(LOG_PATH, base, cursor);
          SerialOut(base, cursor);
          cursor = base; }
    }

    return cursor;
}

/* Deliver pending hardware IRQs to the client's PM vectors -- 08h-0Fh for the master PIC, 70h-77h for
 * the slave -- in g_IrqOrder's priority order; an IRQ whose vector the client never hooked is dropped.
 */
static VOID DpmiDeliverPendingIrqs(
    DOS_MACHINE *machine,
    volatile BYTE * const tib,
    const UINT steps)
{
    if (g_DpmiVi && !g_PmNoIrq && !g_InPmIrq && !g_AsyncPmActive)
    {
        INT scan;
        INT item;

        for (item = 0; item < (INT)sizeof g_IrqOrder; ++item)
        {
            /* DPMI 0.9: hardware interrupts arrive at the PM vectors that
             * match the PIC's -- 08h-0Fh for the master, 70h-77h for the
             * slave -- which is where a client hooks its IRQ 8-15 handler.
             */
            UINT interruptVector;
            scan  = g_IrqOrder[item];
            interruptVector = (scan < PIC_LINES_PER_CHIP) ? PIC_MASTER_VECTOR_BASE + (UINT)scan : PIC_SLAVE_VECTOR_BASE + (UINT)(scan - PIC_LINES_PER_CHIP);

            if (!g_IrqNPending[scan])
                continue;

            /* No PM handler: the client cannot want it. Drop it rather
             * than spin on it forever.
             */
            if (!g_PmInt[interruptVector].Client)
            {
                InterlockedExchange(&g_IrqNPending[scan], 0);
                ++g_PmDeviceIrqDrop;
                continue;
            }

            if (!VddPicCanDeliver(&g_Pic, (BYTE)scan))
                continue;

            /* CLAIM BEFORE RUNNING, HAND BACK ON FAILURE -- the ISR runs
             * inside the call below and the device model re-raises from in
             * there, so clearing afterwards would cancel the interrupt the
             * handler itself just asked for. That is verbatim the mistake
             * the keyboard made.
             */
            /* - AND THE SAME BRACKET ON THE DEVICE LINES, BECAUSE THE
             * TIMER MAY NOT BE THE HANDLER THAT POLLS AT ALL. DMX
             * reads the 8237 count 55 times a second; that was matched
             * against IRQ0's rate first only because IRQ0 was what the
             * previous session was looking at. IRQ5 -- the block
             * completion, which is when a refill is actually DUE -- is
             * the more natural place for a driver to look, and nothing
             * has excluded it. Same snapshot, same exactness.
             */
            /* -- AND THE PIC IS TOLD BEFORE THE HANDLER RUNS, NOT AFTER (#213).
             * The ISR runs inside DpmiInjectPmIrq() and sends its EOI
             * from in there; acknowledging afterwards set the in-service bit
             * AFTER that EOI, so the line stayed in service for good. Until
             * #173 the next timer tick's non-specific EOI happened to clear
             * it (IRQ0 was never in service to take it); once IRQ0 was held
             * properly, ZAR's IRQ5 went dead after its first SB block.
             * Same acknowledge/EOI rule as the async path: in service, and
             * released at once when the vector is still our own stub.
             */
            UINT32 dmaReadsBeforeDevice = g_Dma.ChannelCountReads[SB_DEFAULT_DMA8];
            InterlockedExchange(&g_IrqNPending[scan], 0);

            if (AsyncVectorIsOurStub((UINT)scan))
                VddPicAcknowledgeAutoEoi(&g_Pic, (BYTE)scan);
            else
                VddPicAcknowledge(&g_Pic, (BYTE)scan);

            g_InPmIrq = 1;

            if (DpmiInjectPmIrq(machine, tib, interruptVector, steps))
            {
                ++g_PmDeviceIrqInjected;
            }
            else
            {
                /* No handler ran, so nothing will EOI: hand the line back. */
                VddPicEndOfInterrupt(&g_Pic, (BYTE)scan);
                VddPicRaise(&g_Pic, (BYTE)scan);
                InterlockedExchange(&g_IrqNPending[scan], 1);
                ++g_PmDeviceIrqFail;
            }

            g_InPmIrq = 0;
            g_CooperativeDmaPollsDevice[scan & 7] += g_Dma.ChannelCountReads[SB_DEFAULT_DMA8] - dmaReadsBeforeDevice;
            break;                  /* one per pass: let it IRET first */
        }
    }
}

/* The keyboard's cooperative path, like IRQ0's: a pending IRQ1 (or a scan code waiting in the 8042) is
 * delivered between PM steps when the client has hooked it and can take an interrupt now.
 */
static VOID DpmiDeliverKeyboardIrq(
    volatile BYTE * const tib,
    DOS_MACHINE *machine,
    const UINT steps)
{
    /* AND THE KEYBOARD, WHICH HAD NO COOPERATIVE PATH AT ALL:
     * IRQ0 has had one since #2b; IRQ1 had only the asynchronous
     * injector, and that gets ONE attempt per keystroke: the 8042 model
     * raises on the FIFO's empty->full edge and again as the guest
     * drains it, so if the one raise lands while the CPU thread is
     * inside the host rather than the guest, the attempt bails
     * (why=20, "not executing guest code") and nothing ever retries.
     * Measured, with a scripted key script and every gate open:
     *   KEYIRQ raise gate=1 ok=0 pm=1 pmhook=1 in_exec=0 why=0x14
     * exactly once in a whole run, twelve scancodes pushed, p60=0 --
     * the client never read the keyboard port because it was never told
     * there was anything to read.
     * A pending interrupt is not a moment, it is a STATE: the 8259 holds
     * the request until it can be delivered. So hold it here too and
     * offer it at every pass round the loop, exactly as the timer latch
     * does. The guest reaches this point constantly (every INT 31h,
     * every trapped port access), so the latency is microseconds.
     */
    /* - AND ASK THE 8042, NOT ONLY THE COUNTER. On real hardware the
     * keyboard's request is a STATE -- OBF stays set until the byte is
     * read -- so a byte in the FIFO with no interrupt outstanding is a
     * condition that cannot arise. Deriving the arm from the FIFO as
     * well as the latch makes any future counter slip self-correcting
     * instead of silently swallowing a keystroke.
     */
    if ((g_Irq1Pending > 0 || VddInputScanCodePending(&g_Input))
        && g_PmInt[VECTOR_KEYBOARD].Client && !g_PmNoIrq)
    {
        if (g_Irq1Pending <= 0)
            InterlockedIncrement(&g_Irq1Pending);

        INT virtualIf   = g_DpmiVi;
        INT busy = g_InPmIrq || g_AsyncPmActive;
        INT picCanDeliver  = VddPicCanDeliver(&g_Pic, PIC_IRQ_KEYBOARD);
        INT isCode32  = DpmiSelectorIs32((WORD)VDM_REG16(tib, VTIB_CS));
        INT done1 = 0;

        if (virtualIf && !busy && picCanDeliver)
        {
            /* -- CLAIM THE PENDING INTERRUPT BEFORE RUNNING THE HANDLER,
             * NOT AFTER. The client's ISR reads port 0x60 while we are
             * inside this call, and the 8042 model RE-ASSERTS the line
             * from in there whenever a byte is still queued. Decrementing
             * afterwards therefore cancels the interrupt the ISR itself
             * just raised, and the remaining byte sits in the FIFO with
             * nothing left to announce it. Measured, with a scripted
             * E0-50 (down arrow): three bytes delivered, `scleft=1`, and
             * not one further raise for the rest of the run -- the
             * keyboard simply stopped after the first arrow's prefix.
             * Claim first, hand it back if the injection did not run.
             */
            InterlockedDecrement(&g_Irq1Pending);
            g_InPmIrq = 1;
            done1 = DpmiInjectPmIrq(machine, tib, VECTOR_KEYBOARD, steps);
            g_InPmIrq = 0;

            if (!done1)
                InterlockedIncrement(&g_Irq1Pending);
        }

        /* - WHY A HELD-BACK KEY IS HELD BACK. Five separate conditions
         * stand between a raised IRQ1 and the client's ISR, and a key
         * that never arrives looks the same whichever one said no.
         * Bounded, and only while something IS pending, so a run with
         * no keyboard activity pays nothing.
         */
        if (g_KeyPmLogged < 64
            && (!done1 || g_KeyPmLogged < 8))
        {
            CHAR keyLine[176];
            CHAR *keyCursor = keyLine;
            ++g_KeyPmLogged;
            keyCursor = LogPut(keyCursor, "KEYPM pend=");  keyCursor = LogHex(keyCursor, (DWORD)g_Irq1Pending);
            keyCursor = LogPut(keyCursor, " vi=");         keyCursor = LogHex(keyCursor, (DWORD)virtualIf);
            keyCursor = LogPut(keyCursor, " busy=");       keyCursor = LogHex(keyCursor, (DWORD)busy);
            keyCursor = LogPut(keyCursor, " pic=");        keyCursor = LogHex(keyCursor, (DWORD)picCanDeliver);
            keyCursor = LogPut(keyCursor, " app32=");      keyCursor = LogHex(keyCursor, (DWORD)isCode32);
            keyCursor = LogPut(keyCursor, " done=");       keyCursor = LogHex(keyCursor, (DWORD)done1);
            keyCursor = LogPut(keyCursor, " cs=0x");       keyCursor = LogHex(keyCursor, VDM_REG16(tib, VTIB_CS));
            keyCursor = LogPut(keyCursor, " scleft=");     keyCursor = LogHex(keyCursor, (DWORD)VddInputScanCodesQueued(&g_Input));
            keyCursor = LogPut(keyCursor, "\r\n"); LogAppend(LOG_PATH, keyLine, keyCursor); SerialOut(keyLine, keyCursor);
        }
    }
}

/* A heartbeat from the thread that is demonstrably alive (GH #128): where the client is, for each of
 * the first 255 steps and then every 4096th.
 */
static PSTR DpmiLogHeartbeat(
    PSTR cursor,
    PSTR const base,
    const UINT steps,
    volatile BYTE * const tib)
{
    /* -- HEARTBEAT FROM THE THREAD THAT IS DEMONSTRABLY ALIVE. (GH #128)
     * The watchdog thread is supposed to answer "where is the guest
     * stuck", and on the WOW runs it logs its FIRST sample and then
     * nothing -- twelve were asked for. So it is not a usable
     * instrument here, whatever is wrong with it, and building on it
     * would be building on something that has already been caught
     * lying once. This line comes from the PM loop itself, which is
     * provably still running because everything else in the log does.
     * Sparse (every 4096 steps) so it cannot flood, and it prints the
     * guest position and the last event -- which is the whole question
     * when a run stops producing output but does not die.
     */
    /* Every 16 steps, not every 4096: a WOW run stops after only a few
     * hundred PM entries, so a sparse heartbeat prints nothing at all --
     * which is what the first attempt did. The LAST line before the log
     * ends is the answer this exists for: the CS:EIP we handed to
     * DpmiEnterProtectedMode and never came back from.
     */
    /* EVERY step for the first 256, then sparsely. A WOW run wedges
     * after a few dozen PM entries, so anything sparser prints the
     * position before the interesting one and not the interesting one
     * -- which is what both earlier attempts did. Doom does millions of
     * entries, hence the fallback rate rather than "always".
     */
    if (steps && (steps < 256 || (steps & 0xFFF) == 0))
    {
        /* [CAUTION]: A TIMELINE OF ITS OWN. Session 32 had to borrow wall-clock from
         * the async thread's `ms=` stamps to discover that the whole WOW
         * run is 282 MILLISECONDS -- which is the fact that reframed "it
         * loads and then stops" and cleared the watchdog of a bug it never
         * had. A heartbeat that cannot say WHEN forces that reconstruction
         * every time, and only works while some other instrument happens
         * to be printing timestamps.
         */
        cursor = LogPut(cursor, "PMHB ms=0x");     cursor = LogHex(cursor, GetTickCount());
        cursor = LogPut(cursor, " steps=0x");      cursor = LogHex(cursor, (DWORD)steps);
        cursor = LogPut(cursor, " cs:eip=0x");     cursor = LogHex(cursor, VDM_REG16(tib, VTIB_CS));
        cursor = LogPut(cursor, ":0x");            cursor = LogHex(cursor, DpmiPmEip(tib));
        cursor = LogPut(cursor, " lastev=0x");     cursor = LogHex(cursor, g_DpmiLastEvent);
        cursor = LogPut(cursor, " lastvec=0x");    cursor = LogHex(cursor, g_DpmiLastVector);
        cursor = LogPut(cursor, " wow32{ok=0x");   cursor = LogHex(cursor, g_Wow32Serviced);
        cursor = LogPut(cursor, " decl=0x");       cursor = LogHex(cursor, g_Wow32Declined);
        cursor = LogPut(cursor, " unimpl=0x");     cursor = LogHex(cursor, g_Wow32Unimplemented);
        cursor = LogPut(cursor, "}");
        /* -- THE COUNTERS THAT MATTER TO A CRASH MUST NOT LIVE ONLY IN THE
         * EXIT REPORT. (s72) The async why-histogram was printed at exit
         * and nowhere else -- so for a guest that is KILLED, which is the
         * only kind of run that needs it, the number was unreachable. I
         * asked the user for a run whose whole purpose was to produce a
         * counter the run could not produce. Emit the three that bear on
         * the DPMI mode-switch race on every PM heartbeat instead.
         */
        cursor = LogPut(cursor, " why{simint_rm=0x"); cursor = LogHex(cursor, g_AsyncWhyHistogram[0][ASYNC_WHY_SIMINT_RM]);
        cursor = LogPut(cursor, " hostcs=0x");        cursor = LogHex(cursor, g_AsyncWhyHistogram[0][ASYNC_WHY_HOST_CS]);
        cursor = LogPut(cursor, " inflight=0x");      cursor = LogHex(cursor, g_AsyncWhyHistogram[0][ASYNC_WHY_IN_FLIGHT]);
        cursor = LogPut(cursor, "}");
        /* [INFO]: WHERE IS IT ABOUT TO GO, AND WHAT IS IT ABOUT TO RUN.
         * The resume point alone is not enough. When we hand the
         * guest back at one of our default PM stubs it is sitting on
         * a `CF`, and the interesting address is the one that IRET
         * pops -- so print the stack top as well, and the bytes at
         * the resume point. When a run's last line is a PM entry that
         * never returns, this turns "it died somewhere after the IRET"
         * into an address to disassemble.
         *
         * [CAUTION]: Both reads are guarded. An instrument that faults while
         * reporting a wedge reports nothing, which is how the
         * watchdog thread came to log one sample and stop.
         */
        {   DWORD currentCodeBase = DpmiSelectorBase((WORD)VDM_REG16(tib, VTIB_CS));
            DWORD currentStackBase = DpmiSelectorBase((WORD)VDM_REG16(tib, VTIB_SS));
            DWORD spValue   = VDM_REG16(tib, VTIB_ESP);
            ULONG_PTR codeLinear = (ULONG_PTR)(currentCodeBase + DpmiPmEip(tib));
            ULONG_PTR stackLinear = (ULONG_PTR)(currentStackBase + spValue);
            cursor = LogPut(cursor, " ss:sp=0x"); cursor = LogHex(cursor, VDM_REG16(tib, VTIB_SS));
            cursor = LogPut(cursor, ":0x");       cursor = LogHex(cursor, spValue);
            /* [CAUTION]: THE SELECTOR BASE, because a breakpoint is a LINEAR
             * address and guessing which copy of a module is the
             * executing one is how three runs got spent. The bind
             * stage loads the WOW module set and logs its bases;
             * those are not necessarily the bases the PM run uses,
             * and breakpoints armed against the wrong copy arm
             * SUCCESSFULLY (same bytes, real memory) and never fire.
             * Print it rather than infer it.
             */
            cursor = LogPut(cursor, " csbase=0x"); cursor = LogHex(cursor, currentCodeBase);

            if (currentCodeBase && MemoryReadable(codeLinear, 8))
            {
                cursor = LogPut(cursor, " code="); cursor = LogDump(cursor, (const BYTE *)codeLinear, 8);
            }

            if (currentStackBase && MemoryReadable(stackLinear, 24))
            {
                const BYTE *stackBytes = (const BYTE *)stackLinear;
                INT wordIndex;
                cursor = LogPut(cursor, " iret->0x");
                cursor = LogHex(cursor, (DWORD)(stackBytes[X86_FRAME16_CS] | (stackBytes[X86_FRAME16_CS + 1] << BYTE_SHIFT)));   /* CS */
                cursor = LogPut(cursor, ":0x");
                cursor = LogHex(cursor, (DWORD)(stackBytes[0] | (stackBytes[1] << BYTE_SHIFT)));   /* IP */
                /* ...and the frames ABOVE it. The IRET target turned out
                 * to be a bare `ret` (krnl386 chains INT 31h to us through
                 * an interrupt-style far call from a small helper, so the
                 * interesting address is one frame further up). Print the whole
                 * top of the
                 * stack rather than coming back for it a third time.
                 */
                cursor = LogPut(cursor, " stk");

                for (wordIndex = 0; wordIndex < 12; ++wordIndex)
                {
                    cursor = LogPut(cursor, " "); cursor = LogHex(cursor, (DWORD)(stackBytes[wordIndex*2] | (stackBytes[wordIndex*2+1] << BYTE_SHIFT)));
                }
            }
        }
        cursor = LogPut(cursor, "\r\n");
        LogAppend(LOG_PATH, base, cursor); SerialOut(base, cursor); cursor = base;
    }

    return cursor;
}

/* The PM run is over. A client that exited with a parent waiting in real mode (AH=4Ch from an EXEC'd child) is torn down and terminated there, and the exec loop carries on in V86; otherwise the PM run is marked done and the real-mode loop ends. */
static INT DpmiEndClientSession(
    PSTR *cursorIo,
    PSTR const base,
    volatile BYTE * const tib,
    DOS_MACHINE *machine)
{
    PSTR cursor = *cursorIo;

    /* A CHILD WITH A PARENT GOES BACK TO ITS PARENT. (s80) (Importance = 3):
     * The client said AH=4Ch in protected mode. If something EXEC'd it
     * (COMMAND.COM, a SETUP program) that parent is parked in real mode
     * at its own INT 21h, and ending the VDM here is what made "run Doom
     * from the shell" and "save and launch" come back to nothing. Release
     * the client, leave PM, and let the real-mode terminate do what it
     * does for any child: free its PSP block, unwind its vectors, restore
     * the parent's frame. The exec loop then simply carries on in V86.
     *
     * [CAUTION]: ONLY with a parent. A top-level client's exit still ends the run,
     * unchanged -- nothing is waiting for it.
     */
    if (g_PmClientExited && g_ExecDepth > 0 && g_Running)
    {
        InterlockedIncrement(&g_DpmiWatchdogGeneration);   /* its watchdog stands down */
        DpmiClientTeardown();
        *(volatile WORD *)(tib + VTIB_MSW) &= (WORD)~MSW_PE_BIT;   /* leave PM */
        VDM_SET16(tib, VTIB_FS, 0);
        VDM_SET16(tib, VTIB_GS, 0);
        machine->ExitCode = g_PmExitCode;
        cursor = LogPut(cursor, "DPMI: client exited with a parent waiting (depth=");
        cursor = LogHexByte(cursor, (UINT)g_ExecDepth);
        cursor = LogPut(cursor, ") -- back to real mode, terminating the child there\r\n");
        LogAppend(LOG_PATH, base, cursor); SerialOut(base, cursor); cursor = base;

        if (g_CloseForced)             /* #152: it did not unhook itself */
        {
            g_CloseForced = 0;

            if (g_Routed && g_ExecDepth == 1)
                g_BackToPrompt = 1;                                 /* #208 */

            ExecMachineRestore(g_ExecDepth - 1, &cursor);
            machine->IsTsrPending = 0;
        }

        if (DosTerminate(machine, tib, &cursor, base)) /* parent resumed */
        {
            *cursorIo = cursor;
            return HOST_FLOW_CONTINUE;
        }
    }

    g_CloseForced = 0;
    g_DpmiDone = 1;            /* PM run finished -> watchdog stands down, window persists */
    {
        *cursorIo = cursor;
        return HOST_FLOW_BREAK;
    }
    *cursorIo = cursor;
    return HOST_FLOW_NEXT;
}

enum
{
    INSTALL_EXIT_OK = 0, INSTALL_EXIT_FAILED = 1, INSTALL_STATUS_EXIT_OURS = 0, INSTALL_STATUS_EXIT_NONE = 1, INSTALL_STATUS_EXIT_OTHER = 2
};   /* the install verbs' exit codes */
enum
{
    PENDING_INT_RETRIES_MAX = 0x10000
};   /* event 3 ("interrupt pending, not entered"): retries before giving up */
enum
{
    EXEC_HANDLED_RUN_OVER = 2, EXEC_HANDLED_CHILD_EXITED = 3
};   /* WinMain's DosTerminate outcomes: the run ends, or a child returned to its parent */

/* Run the DPMI client in protected mode until it stops for good: each step delivers pending interrupts, runs the client to its next event, and services what stopped it -- a patched INT nn BOP, a fault the kernel reflected, an async interrupt's return. */
static VOID DpmiRunClient(
    PSTR *cursorIo,
    PSTR const base,
    volatile BYTE * const tib,
    DOS_MACHINE *machine)
{
    PSTR cursor = *cursorIo;
    UINT steps;
    /* --- DPMI protected-mode execution loop -----------------------------------
     * DpmiEnterProtectedMode runs the client in PM until it stops. Two stop kinds:
     * (1) a patched INT nn BOP -- the kernel reflects C4 C4 as VTIB_EVENT=4
     *     (run 32); we look up the original vector by fault EIP and dispatch.
     * (2) GH #18: a raw PM #GP the kernel reflects to our handler code selector
     *     -- also VTIB_EVENT=4, but CS==g_DpmiFaultCodeSelector and EIP==DPMI_FAULT_COFF.
     *     We recover the saved faulting CS:EIP/SS:ESP from the VTIB_FLT_SAV* slots.
     */
    DWORD event3Retries = 0;   /* GH#18: bounded event-3 (pending-int guard) re-entries */
    DWORD pmFaultDumps = 0;              /* rate-limit the PM-fault byte dump (anti-flood) */
    DWORD pmStartTick = GetTickCount();   /* headless wall-clock cap origin */

    for (steps = 0; g_Running && steps < PM_STEPS_MAX; ++steps)    /* run until window close (animation) */
    {
        DWORD event;
        DWORD eip;
        DWORD currentCs;
        DWORD vector;
        INT status;
        /* #152: Close Program ends a PM client exactly as its own AH=4Ch
         * would; the child-with-a-parent path below does the rest.
         */
        if (g_CloseRequest && !g_WowLaunch)
        {
            InterlockedExchange(&g_CloseRequest, 0);
            g_CloseForced = 1;
            g_PmClientExited = 1;
            g_PmExitCode = 0;
            cursor = LogPut(cursor, "CLOSEPROG: ending the DPMI client (File > Close Program)\r\n");
            LogAppend(LOG_PATH, base, cursor); SerialOut(base, cursor); cursor = base;
        }

        if (g_PmClientExited)
            break;                     /* exited inside a nested run -- see the flag */

        /* Headless safety (session-9): an infinite visual demo (pm32irq/animate)
         * never calls INT 21h 4Ch, so under the SMB auto-exit harness the PM loop
         * would run forever and wedge rt.bat's `start /wait`. Bound it by wall clock
         * so the host self-exits and the watcher survives. Sampled sparsely (every
         * 4096 steps) to keep GetTickCount off the hot path. Interactive runs (no
         * marker) are unbounded, as before -- the user closes the window.
         */
        if (g_Headless && (steps & PM_HEADLESS_CHECK_MASK) == 0 && steps
            && GetTickCount() - pmStartTick > PM_HEADLESS_MS)
        {
            cursor = LogPut(cursor, "STAGE3-DPMI: headless time cap (");
            cursor = LogHex(cursor, PM_HEADLESS_MS); cursor = LogPut(cursor, " ms) reached after 0x");
            cursor = LogHex(cursor, (UINT)steps);
            cursor = LogPut(cursor, " steps -> exiting (infinite/visual demo; watch it on the monitor)\r\n");
            LogAppend(LOG_PATH, base, cursor); SerialOut(base, cursor); cursor = base;
            break;
        }

        cursor = DpmiLogHeartbeat(cursor, base, steps, tib);
        /* run 52 heartbeat: publish where we're about to hand off + bump the
         * iteration counter BEFORE entering, so a watchdog sample taken while
         * we're blocked inside DpmiEnterProtectedMode sees a FROZEN iter at this CS:EIP.
         */
        g_DpmiEnterCs  = VDM_REG16(tib, VTIB_CS);
        g_DpmiEnterEip = DpmiPmEip(tib);
        g_DpmiIteration      = (LONG)(steps + 1);
        /* run 66 diagnostic (kept): log FIXED_NTVDMSTATE [0x714] once. Run 66 proved
         * bit3=0 already (classifier is NOT the blocker), so no forcing needed -- for
         * a #GP the kernel uses class 6, which reaches the generic reflect body.
         */
        if (steps == 0)
        {
            DWORD vdmStateAt714 = *(volatile DWORD *)(ULONG_PTR)FIXED_NTVDMSTATE_LINEAR;
            cursor = LogPut(cursor, "GH#18: [0x714]=0x"); cursor = LogHex(cursor, vdmStateAt714);
            cursor = LogPut(cursor, " bit3="); cursor = LogHex(cursor, (vdmStateAt714 >> 3) & 1);
            cursor = LogPut(cursor, " bit4="); cursor = LogHex(cursor, (vdmStateAt714 >> 4) & 1);
            cursor = LogPut(cursor, " bit14="); cursor = LogHex(cursor, (vdmStateAt714 >> 14) & 1);
            cursor = LogPut(cursor, " tib8=0x"); cursor = LogHex(cursor, *(volatile DWORD *)(tib + DPMI_TIB_FLTTBL));
            cursor = LogPut(cursor, "\r\n");
            LogAppend(LOG_PATH, base, cursor); SerialOut(base, cursor); cursor = base;
        }

        /* Timing in PM (polled): the host PIT (UI thread) raises IRQ0 at the 8254
         * rate; here we consume it and run the BIOS tick handler so 0040:006C and
         * INT 1Ah advance with wall-clock even though nothing injects INT 08h into
         * the PM client yet. (Async IRQ0 delivery to a client's PM INT 08h hook is
         * the remaining timing piece.)
         */
        OplPumpTime();

        if (InterlockedExchange(&g_Irq0Pending, 0))
        {
            NTVDD_REGISTERS trapRegisters;
            RegistersLoad(&trapRegisters, tib);
            HOST_LOCK();
            VddBusDeliverInterrupt(&g_Bus, VECTOR_TIMER, &trapRegisters);   /* PitInt08 -> ++0040:006C */
            HOST_UNLOCK();
            g_PmIrq0Latch = 1;    /* #2b: latch a virtual IRQ0 for the PM hook */
        }

        /* #2b async IRQ0 injection: when the client has hooked INT 08h in PM
         * (g_PmInt[8], via INT 31h 0205) and its virtual-IF is enabled, deliver
         * the latched IRQ0 to that handler -- how timer-hooking games get ticks.
         * The latch persists across CLI windows so a masked interrupt isn't lost.
         */
        /* - AND NOT WHILE AN ASYNC ONE IS STILL IN FLIGHT. The two injectors
         * guarded themselves but not each other: the async path can vector the
         * guest into its ISR and return, and if that ISR then leaves PM for a
         * DOS call, control arrives back HERE with the handler still live --
         * and a second tick would re-enter it on top of itself. Measured: the
         * run died on such an injection taken at obj1+0x153dc, i.e. inside the
         * very delay loop the tick exists to release.
         */
        /* #172: which gate turned an OWED tick away. See g_PmCooperativeGate. */
        INT twoTicksOwed = (g_PmTickOwed >= 2);

        if (twoTicksOwed)
        {
            UINT gateIndex = !(g_PmIrq0Latch || g_PmTickOwed > 0) ? PM_GATE_NO_LATCH : !g_DpmiVi ? PM_GATE_VIF_OFF
                        : !g_PmInt[VECTOR_TIMER].Client ? PM_GATE_NO_HOOK : g_InPmIrq ? PM_GATE_IN_PM_IRQ
                        : g_PmNoIrq ? PM_GATE_NO_IRQ : g_AsyncPmActive ? PM_GATE_ASYNC_IN_FLIGHT
                        : (GetTickCount() - g_PmVector8ArmedMs) < DPMI_IRQ0_ARM_QUIET_MS ? PM_GATE_ARMED
                        : PM_GATE_TRIED;
            g_PmCooperativeGate[gateIndex]++;
        }

        /* #172: THE LATCH OPENS THIS, NOT THE OWED COUNT. (s85):
         * s84 opened the arm on `g_PmTickOwed > 0` as well, on the theory
         * that the quit wait's backlog was stuck here. The real cause was
         * ModeYPmRun holding g_Lock (stop reason `irq`), and isolated on the
         * menu-quit route (runs/s85/owed/, interleaved x3) the owed-count arm
         * bought nothing: quit window 139/s either way, REPLAYED_LOUD 48-50
         * with it against 44-45 without, plus ~5,800 injections declined per
         * run in DOS/4GW's 16-bit start-up. Latch only.
         */
        if (g_PmIrq0Latch
            && g_DpmiVi && g_PmInt[VECTOR_TIMER].Client && !g_InPmIrq
            && !g_PmNoIrq && !g_AsyncPmActive
            && (GetTickCount() - g_PmVector8ArmedMs) >= DPMI_IRQ0_ARM_QUIET_MS)
        {
            UINT32 dmaReadsBefore = g_Dma.ChannelCountReads[SB_DEFAULT_DMA8];
            g_PmIrq0Latch = 0;
            g_InPmIrq = 1;

            if (g_PmTickOwed > 0 && Irq0PmClaim())
            {
                if (DpmiInjectPmIrq(machine, tib, VECTOR_TIMER, steps))
                    InterlockedDecrement(&g_PmTickOwed);
                else
                {
                    Irq0PmUnclaim();

                    if (twoTicksOwed)
                        g_PmCooperativeGate[PM_GATE_DECLINED]++;
                }
            }
            else if (twoTicksOwed)
                g_PmCooperativeGate[PM_GATE_CLAIM_REFUSED]++;

            g_InPmIrq = 0;
            g_CooperativeDmaPolls += g_Dma.ChannelCountReads[SB_DEFAULT_DMA8] - dmaReadsBefore;
        }

        /* -- s90 (#278): IRQs RAISED BY A 32-BIT COMPONENT (call_ica_hw_interrupt,
         * through bin\wowshim\NTVDM.EXE) -- winmm raises IRQ 10 to tell
         * MMSYSTEM a callback is queued in their shared buffer, and
         * MMSYSTEM's handler (PM vector 72h) drains it and EOIs both PICs.
         * Delivered here, by the guest thread, on the same gates as IRQ0;
         * a line nobody hooked is counted and dropped (real hardware would
         * reach the default handler, which only EOIs). WOW only.
         */
        if (g_WowLaunch && g_IcaPending && g_DpmiVi)
            WowIcaDeliver(machine, tib, steps);

        DpmiDeliverKeyboardIrq(tib, machine, steps);
        /* AND THE DEVICE LINES, WHICH HAD NO COOPERATIVE PATH EITHER:
         * This is the KEYBOARD BUG ABOVE, one line number over, and it is
         * the PCM click. A device IRQ gets exactly ONE delivery attempt --
         * the synchronous AsyncInjectIrq() inside HostIrqSink(), made
         * from the AUDIO thread at the instant of the raise. If the CPU
         * thread happens to be inside the host rather than in guest code
         * that attempt bails at why=20 and NOTHING RETRIES: the V86 exec
         * loop's drain (the `for (q = 2; q < 8; ...)` above) needs a `tib`
         * from a trapping guest, and a 32-bit DPMI client never goes there.
         * MEASURED on a 45 s Doom run, and it is not marginal:
         *   sb_blocks   0x0de6 = 3558 block completions raised
         *   irq05 (PM)  0x0a37 = 2615 delivered
         *   ASYNC-EARLY 1009 bails, EVERY ONE why=0x14 (g_InExec == 0)
         * A dropped SB completion is not a dropped tick. Each one owns a
         * distinct 256-byte refill: no IRQ means DMX never rewrites that
         * block, so the 8237 laps the ring and we play the PREVIOUS lap's
         * audio verbatim. Proven in the capture -- seams 4096 bytes apart
         * share their preceding bytes exactly, and 96 of 182 seams are
         * preceded by a full 256-byte repeat. At 86 blocks/s that is the
         * buzz. A timer tick can be coalesced; this cannot.
         * So hold the request and offer it every pass, exactly as the
         * timer latch and the keyboard now do. The guest reaches this point
         * constantly (every INT 31h, every trapped port access), so the
         * added latency is microseconds and no new thread is involved.
         */
        /* -- AND THE MOUSE DRIVER'S OWN CALLBACK (INT 33h 0Ch), same gate.
         * (s74c) ZAR's buttons travel only through this.
         */
        if (g_MouseEventPend && MouseAnyHandler()        /* 0Ch's or 18h's (#265) */
            && g_DpmiVi && !g_PmNoIrq && !g_InPmIrq && !g_AsyncPmActive)
        {
            g_InPmIrq = 1;
            DpmiInjectPmMouseCallback(machine, tib, steps);
            g_InPmIrq = 0;
        }

        DpmiDeliverPendingIrqs(machine, tib, steps);
        cursor = DpmiCheckpointFirstEntries(cursor, base, steps, tib);
        DpmiArmFaultTrampoline(tib, 0);   /* re-arm nest/flag/[0x638]/[TIB+8] */

        if (steps < g_DpmiCpMaximum)
        {
            cursor = LogPut(cursor, "DPMI-CP["); cursor = LogHex(cursor, (UINT)steps);
            cursor = LogPut(cursor, "] armed -> entering PM\r\n");
            LogAppend(LOG_PATH, base, cursor); SerialOut(base, cursor); cursor = base;
        }

        /* Give the watchdog a guaranteed turn before the FIRST entry only. If it
         * has logged a sample by the time we hand off, then its silence afterwards
         * means the whole process was killed at once (a kernel VDM terminate),
         * not that the thread never ran. 300 ms, once, on a diagnostic path.
         */
        if (steps == 0)
            Sleep(300);

        /* TELL THE ASYNC INJECTOR THE GUEST IS RUNNING:
         * g_InExec means "the CPU thread is executing GUEST code, so its
         * context is the guest's and may be rewritten"; AsyncInjectIrq()
         * refuses to touch the thread without it, precisely so it cannot race
         * the host manipulating the TIB. It was set only around VdmRunGuest(), so
         * for the whole of a protected-mode session the answer was "no" and
         * every asynchronous delivery bailed at the first line -- which is why
         * a PM guest could never be interrupted at all. Protected-mode
         * execution is execution too.
         */
        /* - NARROW ESP ON A 16-BIT STACK FOR **BOTH** PATHS. This was added
         * for the kernel path (it took the spike from 1 PM entry to 8) on the
         * reasoning that the far-jmp path reloads the junk harmlessly with
         * `lss`. Mostly true -- but measured, it is not always: give Doom a
         * command-line argument and the run dies right after the AH=30h
         * version check with
         *   SS=0x00c7 (SS D/B=0)  ESP=0xb33b6f14
         *   GH#18: PM-FAULT REFLECTED -- saved CS:EIP=0x00c7:0xb33b6f1e
         * and 0xb33b is a HOST THREAD-STACK address sitting in the top half
         * of ESP, because with a 16-bit SS the CPU maintains SP only and
         * whatever the host last had there stays.
         *
         * [CAUTION]: CLEARING IT DOES **NOT** FIX THAT BUG -- measured, the run is
         * identical (467 INT 31h calls either way). Kept anyway because
         * the junk is objectively wrong state that shows up in every dump
         * and there is no case where the high half of ESP is meaningful
         * while SS is 16-bit. Do not read this as the argument fix.
         */
        if (!DpmiSelectorIs32((WORD)VDM_REG16(tib, VTIB_SS)))
            VDM_REG(tib, VTIB_ESP) &= WORD_MASK_U;

        while (g_PauseWant && g_Running) /* #219 */
        {
            ++g_PauseCooperative;
            Sleep(PAUSE_POLL_MS);
        }

        CpuSpeedCooperativePark();                                                  /* #225 */
        InterlockedExchange(&g_InExec, 1);
        ExecEnterMark();   /* guest-execution clock starts (throttle) */
        {
            INT flow = DpmiRunClientSlice(&cursor, base, tib, steps);

            if (flow == HOST_FLOW_BREAK)
                break;
        }
        ExecLeaveMark();   /* ...and stops. Our servicing is not its */
        InterlockedExchange(&g_InExec, 0);

        if (steps < g_DpmiCpMaximum)
        {
            cursor = LogPut(cursor, "DPMI-CP["); cursor = LogHex(cursor, (UINT)steps);
            cursor = LogPut(cursor, "] returned ev=0x"); cursor = LogHex(cursor, VDM_REG(tib, VTIB_EVENT));
            cursor = LogPut(cursor, " cs:eip=0x"); cursor = LogHex(cursor, VDM_REG16(tib, VTIB_CS));
            cursor = LogPut(cursor, ":0x"); cursor = LogHex(cursor, VDM_REG16(tib, VTIB_EIP));
            cursor = LogPut(cursor, "\r\n");
            LogAppend(LOG_PATH, base, cursor); SerialOut(base, cursor); cursor = base;
        }

        event  = VDM_REG(tib, VTIB_EVENT);
        eip = DpmiPmEip(tib);
        currentCs = VDM_REG16(tib, VTIB_CS);
        g_DpmiLastEvent  = event;
        g_DpmiLastEip = eip;
        g_DpmiLastCs = currentCs;
        {
            INT flow = DpmiFinishAsyncPmInterrupt(event, currentCs, eip, tib, machine, steps);

            if (flow == HOST_FLOW_CONTINUE)
                continue;
        }
        /* GH#18 (bare-metal crack, 2026-08-18): dpmi_enter.S reports event 3 when it
         * DECLINES to enter PM -- guest IF=1 AND [0x714]&3 signals a pending hardware
         * interrupt (the FIXED_NTVDMSTATE pending bits; see dpmi_enter.S label 2). We
         * run PM IN-PROCESS (far-jmp), NOT via VdmStartExecution, so while PM executes
         * the kernel does not manage this VDM's interrupt assist -- those pending bits
         * are STALE real-mode state: a timer IRQ0 latched during the DOS INT 21h calls
         * before the mode switch. On real 3.3GHz silicon a tick is essentially always
         * pending at switch time (QEMU+HVF's dilated clock rarely had one), so the
         * monitor bailed with event 3 forever and the PM client never ran a single
         * instruction (EIP stuck at entry) -- THE session-8 "kernel won't run PM" wall.
         * Clear the stale pending bits and re-enter. Bounded so a genuinely re-arming
         * pending can't spin; the BIOS tick still advances via the IRQ0 path above.
         */
        if (event == 3)
        {
            if (++event3Retries <= PENDING_INT_RETRIES_MAX)
            {
                if (event3Retries <= 3)
                {
                    cursor = LogPut(cursor, "GH#18: event3 pending-int guard at CS:EIP=0x");
                    cursor = LogHex(cursor, currentCs); cursor = LogPut(cursor, ":0x"); cursor = LogHex(cursor, eip);
                    cursor = LogPut(cursor, " [0x714]=0x");
                    cursor = LogHex(cursor, *(volatile DWORD *)(ULONG_PTR)FIXED_NTVDMSTATE_LINEAR);
                    cursor = LogPut(cursor, " -> clear stale pending + re-enter\r\n");
                    LogAppend(LOG_PATH, base, cursor); SerialOut(base, cursor); cursor = base;
                }

                *(volatile DWORD *)(ULONG_PTR)FIXED_NTVDMSTATE_LINEAR &= ~VDM_INT_PENDING;
                continue;
            }

            /* retry budget exhausted -> fall through and report an unexpected stop */
        }

        {
            INT flow = DpmiHandleReflectedFault(&cursor, base, event, currentCs, eip, tib, machine, steps);

            if (flow == HOST_FLOW_BREAK)
                break;

            if (flow == HOST_FLOW_CONTINUE)
                continue;
        }
        {
            INT flow = DpmiResumeAfterClientHandler(&cursor, base, event, currentCs, eip, tib);

            if (flow == HOST_FLOW_BREAK)
                break;

            if (flow == HOST_FLOW_CONTINUE)
                continue;
        }
        /* GH#18 run 72: a real-CPU PROTECTED-MODE I/O insn (IN/OUT) reflects as
         * event 0 -- the SAME VDD-trap event as V86 (VM-confirmed by outprobe.com,
         * a PM `OUT DX,AL` to 0x3C8). Service it through the device bus and resume,
         * so PM port I/O (VGA/sound) reaches our VDDs instead of the loop treating
         * event 0 as an "unexpected PM stop" and spinning.
         */
        if (event == VDM_EVENT_IO || event == VDM_EVENT_IO_HW || event == VDM_EVENT_GPFAULT)
        {
            INT isIoHandled;
            HOST_LOCK();
            isIoHandled = HostTryIoPm(tib, &g_Bus);
            HOST_UNLOCK();

            if (isIoHandled)
            {
                /* North star 1: the OUT that just trapped may have opened a
                 * multi-plane window -- Doom's drawers start exactly so.
                 */
                if (ModeYPmNeedsInterp())
                    ModeYPmRun(tib);

                continue;         /* serviced the port op -> keep running */
            }

            /* not a decodable I/O op -> fall through to the normal dispatch/stop */
        }

        /* BARE-METAL diagnostic (GH #18): dump the faulting PM instruction bytes for
         * any non-BOP stop, so we can identify what real hardware reflects as event 3
         * (raw PM #GP) vs the HVF silent-terminate. Rate-limited to the first 32 stops
         * so a client that repeatedly faults can't flood the log (session-9).
         */
        if (event != VDM_EVENT_BOP && pmFaultDumps < 32)
        {
            DWORD faultBase = DpmiSelectorBase((WORD)currentCs);
            ++pmFaultDumps;
            const volatile BYTE *faultInstruction = (const volatile BYTE *)(ULONG_PTR)(faultBase + eip);
            UINT32 selectorAr = 0;
            UINT32 selectorLim = 0;
            DpmiSelectorDescriptor((WORD)currentCs, &selectorAr, &selectorLim);
            cursor = LogPut(cursor, "GH#18 PM-FAULT ev=0x"); cursor = LogHex(cursor, event);
            cursor = LogPut(cursor, " CS:EIP=0x"); cursor = LogHex(cursor, currentCs); cursor = LogPut(cursor, ":0x"); cursor = LogHex(cursor, eip);
            cursor = LogPut(cursor, " base=0x"); cursor = LogHex(cursor, faultBase); cursor = LogPut(cursor, " AR=0x"); cursor = LogHex(cursor, selectorAr);
            cursor = LogPut(cursor, " lim=0x"); cursor = LogHex(cursor, selectorLim); cursor = LogPut(cursor, " bytes=");
            cursor = LogDump(cursor, (const VOID *)faultInstruction, 12); cursor = LogPut(cursor, "\r\n");
            LogAppend(LOG_PATH, base, cursor); SerialOut(base, cursor); cursor = base;
        }

        vector = (event == VDM_EVENT_BOP) ? DpmiBopVector(currentCs, eip) : 0;
        g_DpmiLastVector = vector;
        g_PmTopDispatch = 1;          /* #256: consumed by this dispatch */
        status = DpmiServicePmInt(machine, tib, vector, steps);

        if (status > 0)
            continue;               /* serviced -> keep running the PM client */

        break;                  /* 0 = client exited, <0 = unexpected stop */
    }

    *cursorIo = cursor;
}

static PSTR DpmiInstallFaultReflect(PSTR cursor, PSTR const base)
{
    /* GH #18 (run 67): install the PM-fault reflect machinery, so a RAW (non-BOP)
     * PM #GP -- an SS-retype, HLT, or privileged op the INT->BOP scan cannot
     * pre-patch -- is reflected by the kernel to our handler (code sel : BOP) on a
     * scratch stack, instead of silently terminating the VDM (runs 20-34).
     */
    DpmiInstallFaultTrampoline();
    cursor = LogPut(cursor, "DPMI: PM-fault reflect stkSel=0x"); cursor = LogHex(cursor, g_DpmiFaultSelector);
    cursor = LogPut(cursor, " codeSel=0x"); cursor = LogHex(cursor, g_DpmiFaultCodeSelector);
    cursor = LogPut(cursor, " bop@code:0x"); cursor = LogHex(cursor, DPMI_FAULT_COFF);
    cursor = LogPut(cursor, " tbl@0x"); cursor = LogHex(cursor, (DWORD)(ULONG_PTR)g_FaultTable);
    cursor = LogPut(cursor, " class="); cursor = LogHex(cursor, DPMI_FLT_CLASS_GP);
    cursor = LogPut(cursor, "\r\n");
    LogAppend(LOG_PATH, base, cursor); SerialOut(base, cursor); cursor = base;
    return cursor;
}

/* Read what this PM session is configured to do, on the run that will use it: guest breakpoints, the WOW32 and scheduler switches, the PM watch addresses, and the cfg\ flags (pmkernel, nomouse, nosb, pmvehpass, pmnoirq). */
static PSTR DpmiLoadSessionKnobs(PSTR cursor, PSTR const base)
{
    /* Guest breakpoints (PMBP_PATH). Loaded here rather than at WinMain entry
     * so the list is read on the run that will use it, and armed both now and
     * after every code-region patch -- an address inside a module the client
     * has not loaded yet simply arms later.
     */
    DpmiBreakpointLoad();
    DpmiBreakpointArm();
    Wow32ReturnLoad();
    Wow32ModeLoad();
    WowSchedLoad();
    WowCallLoad();
    /* LAST of the switches, so everything above still gets to say
     * what it armed before the trace goes quiet.
     */
    WowQuietLoad();
    /* pmchg.txt: `<hex offset> [segment]`, segment defaulting to 4
     * (krnl386's DGROUP). Absent file = no watch and no cost.
     */
    { HANDLE configHandle = CreateFileA(PMCHG_PATH, GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                              OPEN_EXISTING, 0, NULL);

      if (configHandle != INVALID_HANDLE_VALUE)
      {
          CHAR pmChangeText[128];
          DWORD commandLength = 0, pmTextIndex = 0, values[PMWATCH_COLUMNS] = { 0, 0 };
          INT column = 0;
          ReadFile(configHandle, pmChangeText, sizeof pmChangeText - 1, &commandLength, NULL);
          CloseHandle(configHandle);

          while (pmTextIndex < commandLength && column < PMWATCH_COLUMNS)
          {
              INT digits = 0;

              while (pmTextIndex < commandLength && (pmChangeText[pmTextIndex] == ' ' || pmChangeText[pmTextIndex] == '\t'))
                  ++pmTextIndex;

              while (pmTextIndex < commandLength)
              {
                  CHAR character = pmChangeText[pmTextIndex];
                  INT pmDigit = (character >= '0' && character <= '9') ? character - '0'
                        : (character >= 'a' && character <= 'f') ? character - 'a' + HEX_DIGIT_A_VALUE
                        : (character >= 'A' && character <= 'F') ? character - 'A' + HEX_DIGIT_A_VALUE : -1;

                  if (pmDigit < 0)
                      break;

                  values[column] = (values[column] << NIBBLE_SHIFT) | (DWORD)pmDigit;
                  ++digits;
                  ++pmTextIndex;
              }

              if (digits)
                  ++column;
              else
                  break;
          }

          if (column >= 1)
          {
              g_PmWatchOffset = values[0];

              if (column >= PMWATCH_COLUMNS && values[1] <= WOW_PMBASE_MAX)
                  g_PmWatchSegment = (UINT)values[1];   /* 0 = already linear */

              cursor = LogPut(cursor, "PMWATCH: watching seg "); cursor = LogHex(cursor, g_PmWatchSegment);
              cursor = LogPut(cursor, " + 0x"); cursor = LogHex(cursor, g_PmWatchOffset);
              cursor = LogPut(cursor, " for changes\r\n");
              LogAppend(LOG_PATH, base, cursor); SerialOut(base, cursor); cursor = base;
          }
      }
    }

    if (GetFileAttributesA(PMVERBOSE_PATH) != INVALID_FILE_ATTRIBUTES)
        g_DpmiCpMaximum = 0x100000;   /* verbose: trace a whole startup */

    { HANDLE watchHandle = CreateFileA(PMWATCH_PATH, GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                              OPEN_EXISTING, 0, NULL);

      if (watchHandle != INVALID_HANDLE_VALUE)
      {
          CHAR watchBuffer[128];
          DWORD watchLength = 0;
          DWORD index2b = 0;
          ReadFile(watchHandle, watchBuffer, sizeof watchBuffer - 1, &watchLength, NULL);
          CloseHandle(watchHandle);

          while (index2b < watchLength && g_PmWatchCount < DPMI_WATCH_MAX)
          {
              DWORD number = 0;
              INT dig = 0;
              INT rel = 0;

              if (index2b < watchLength && watchBuffer[index2b] == '+')
              {
                  rel = 1;
                  ++index2b;
              }

              while (index2b < watchLength)
              {
                  CHAR character = watchBuffer[index2b];
                  INT digit = (character >= '0' && character <= '9') ? character - '0'
                        : (character >= 'a' && character <= 'f') ? character - 'a' + HEX_DIGIT_A_VALUE
                        : (character >= 'A' && character <= 'F') ? character - 'A' + HEX_DIGIT_A_VALUE : -1;

                  if (digit < 0)
                      break;

                  number = (number << NIBBLE_SHIFT) | (DWORD)digit;
                  ++dig;
                  ++index2b;
              }

              if (dig) { g_PmWatchRel[g_PmWatchCount] = (BYTE)rel;
                         g_PmWatch[g_PmWatchCount++] = number; }
              else
                  ++index2b;
          }

          cursor = LogPut(cursor, "DPMI: pmwatch.txt -> ");
          { INT watchIndex2;

          for (watchIndex2 = 0; watchIndex2 < g_PmWatchCount; ++watchIndex2)
          {
                cursor = LogPut(cursor, g_PmWatchRel[watchIndex2] ? "codebase+0x" : "0x");
                cursor = LogHex(cursor, g_PmWatch[watchIndex2]);
                cursor = LogPut(cursor, " "); } }

          cursor = LogPut(cursor, "\r\n");
          LogAppend(LOG_PATH, base, cursor); SerialOut(base, cursor); cursor = base;
      } }

    g_DpmiUseKernel = (GetFileAttributesA(PMKERNEL_PATH) != INVALID_FILE_ATTRIBUTES);

    if (g_DpmiUseKernel)
    {
        cursor = LogPut(cursor, "DPMI: pmkernel.flag -- PM will run under VdmStartExecution\r\n");
        LogAppend(LOG_PATH, base, cursor); SerialOut(base, cursor); cursor = base;
    }

    if (g_MouseAbsent)
    {
        cursor = LogPut(cursor, "MOUSE: nomouse.flag -- INT 33h reports NO driver installed\r\n");
        LogAppend(LOG_PATH, base, cursor); SerialOut(base, cursor); cursor = base;
    }

    g_SbAbsent = (GetFileAttributesA(NOSB_PATH) != INVALID_FILE_ATTRIBUTES);
    g_OplAbsent = g_SbAbsent;      /* one knob, both devices unfitted */

    if (g_SbAbsent)
    {
        cursor = LogPut(cursor, "SB: nosb.flag -- DSP reset will NOT answer; no Sound Blaster fitted\r\n");
        LogAppend(LOG_PATH, base, cursor); SerialOut(base, cursor); cursor = base;
    }

    g_PmVehPass = (GetFileAttributesA(PMVEHPASS_PATH) != INVALID_FILE_ATTRIBUTES);

    if (g_PmVehPass)
    {
        cursor = LogPut(cursor, "DPMI: pmvehpass.flag -- non-INT PM faults will NOT be swallowed by the VEH\r\n");
        LogAppend(LOG_PATH, base, cursor); SerialOut(base, cursor); cursor = base;
    }

    g_PmNoIrq = (GetFileAttributesA(PMNOIRQ_PATH) != INVALID_FILE_ATTRIBUTES);

    if (g_PmNoIrq)
    {
        cursor = LogPut(cursor, "DPMI: pmnoirq.flag present -- IRQ0->PM injection SUPPRESSED\r\n");
        LogAppend(LOG_PATH, base, cursor); SerialOut(base, cursor); cursor = base;
    }

    return cursor;
}

static PSTR DpmiPatchClientIntSitesUpFront(PSTR cursor, PSTR const base)
{
    /* Patch the client's PM `INT nn` (CD nn) -> BOP (C4 C4), recording the original
     * vector per CS offset. Same 2 bytes, so a real unmodified client's INT 31h/21h
     * now reflect to us as BOPs.
     *
     * Why UP-FRONT (not lazy on first fault): a raw `INT 31h` in PM raises a #GP the
     * native kernel cannot reflect to us (runs 20-34) -- that unsolved reflect is the
     * whole reason we patch, so we cannot wait for the fault to catch it. So the scan
     * must find every INT site before the client runs.
     *
     * Hardening (run 42): scan the FULL 64K code selector, not just the first 0x2000.
     * A real program's INT sites live well past 8 KB; the old bound silently missed
     * them (client would #GP-hang on the first unpatched INT 31h). The zeroed stack/BSS
     * tail can't match CD 31/CD 21, so scanning it is harmless. g_int_vec[] doubles as
     * an original-bytes map: g_int_vec[o]!=0 => offset o was `CD g_int_vec[o]` and is
     * now `C4 C4`, so a mis-patch (a CD 31/CD 21 byte-pair that was DATA, not code) is
     * detectable and revertible. Data mis-patch stays possible (x86 isn't
     * self-synchronising; without a disassembler we can't prove a byte is code) -- the
     * map is the mitigation, and an unexpected-BOP path below logs any surprise.
     */
    { volatile BYTE *cs = (volatile BYTE *)(ULONG_PTR)g_DpmiCodeBase;
      DWORD position;
      DWORD count = 0;
      DWORD last = 0;
      DWORD votedCount = 0;

      for (position = 0; position < X86_SEGMENT_LIMIT_64K; ++position)
      {
          /* [CAUTION]: 0x2F IS HERE BECAUSE krnl386 DIED WITHOUT IT (GH #128).
           * A PM guest cannot reach the IVT, so an INT this list does not
           * name stays a raw `CD nn`, and executing it in protected mode
           * goes to the kernel's #GP reflect -- which does not reflect,
           * it SILENTLY TERMINATES THE VDM. That is the #18 signature:
           * no exception, no log line, the process simply gone.
           * krnl386 issues INT 2Fh from PM (168A, the "MS-DOS" vendor
           * query -- its very next interrupt after the 000A alias, per
           * the log) and it died there. No DOS/4GW-class client
           * ever did that, which is why the list never needed 0x2F.
           *
           * [CAUTION]: Every number added here widens the false-positive surface of
           * what is a NAIVE byte-pair scan -- the same shape that once
           * rewrote a `jle` displacement in Doom and cost five sessions.
           * The narrow list is the mitigation. Add a vector only with a
           * guest that provably needs it, and only with a service arm to
           * receive it (see DpmiServicePmInt).
           */
          /* AND EVERY OTHER VECTOR, IF THE DECODER VOUCHES FOR IT (Importance = 3):
           * The list above is the vectors we EXPECTED, and the note beside
           * it is right that widening a naive byte-pair scan is dangerous.
           * But "naive" is the part that changed: x86len.h's vote arrived in
           * session 21 and is already trusted to gate every site in
           * DpmiPatchCodeRegion(). It was simply never wired in here.
           *
           * [CAUTION]: MEASURED, session 32. krnl386 executes an `INT 2` (`cd 02`) in
           * protected mode. A breakpoint armed on that site reported `displaced
           * cd 02`, which
           * is proof it was RAW -- DpmiBreakpointArm() refuses a site that is already
           * an INT site, so it could not have armed otherwise. 0x02 is not on
           * the list, so it was never even a candidate.
           *
           * [CAUTION]: AND THE FIRST DIAGNOSIS WAS WRONG: this was written up as "the
           * boundary vote produces false negatives on real instructions". The
           * vote never ran. The filter in front of it was the defect, and it
           * is a different one in each of the two scanners.
           *
           * [CAUTION]: AND "EVERY VECTOR, GATED BY THE VOTE" WAS TRIED AND IS WRONG.
           * Measured: it patched 0x7d sites instead of 0x6c -- 17 unlisted ones
           * the vote vouched for -- and the WOW run went BACKWARDS, dying at PM
           * step 0x27 instead of 0x46. Residuals fell from 31 to 14 and the
           * INT 2 was correctly claimed, so the widening did what it said; it
           * also broke the guest earlier, which means at least one of those 17
           * is data or mid-instruction that the vote waved through. The vote is
           * good (real sites 19-48 votes, false pairs 0-3) but it is NOT good
           * enough to underwrite all 256 vectors on this binary.
           *
           * SO: EVIDENCE ONLY, which is what the note above already prescribed
           * -- "add a vector only with a guest that provably needs it". The one
           * vector with proof is 0x02, and it is still gated by the vote. A
           * listed vector is patched exactly as before, unvoted, so no existing
           * guest can change behaviour. `votedCount` is counted separately so the
           * next widening is measurable rather than asserted.
           */
          if (cs[position] == X86_OP_INT)
          {
              BYTE siteVector = cs[position+1];
              INT listed = (siteVector == VECTOR_DPMI || siteVector == VECTOR_DOS || siteVector == VECTOR_VIDEO
                            || siteVector == VECTOR_KEYBOARD_SERVICES || siteVector == VECTOR_MOUSE
                            || siteVector == VECTOR_MULTIPLEX
                            || siteVector == VECTOR_EQUIPMENT || siteVector == VECTOR_KERNEL_DEBUGGER
                            || siteVector == VECTOR_TIME || siteVector == VECTOR_TIMER);
              DWORD linear = g_DpmiCodeBase + position;      /* map is linear-keyed now */

              if (!listed && siteVector != VECTOR_NMI)
                  continue;                                         /* evidence only -- see above */

              if (!listed)
              {
                  /* Initial mode-switch selectors are 16-bit even for a 32-bit
                   * client (the RETF-on-failure proof, session 16), so d32=0.
                   */
                  if (!X86IsIntSiteReal((const BYTE *)(ULONG_PTR)cs,
                                            position, X86_SEGMENT_LIMIT_64K, X86_OPERAND_16))
                      continue;

                  ++votedCount;
              }

              PatchMapSet(linear, siteVector);
              cs[position] = VDM_BOP0;
              cs[position+1] = VDM_BOP1;
              ++count;
              last = position;
          }
      }

      cursor = LogPut(cursor, "DPMI: patched "); cursor = LogHex(cursor, count);
      cursor = LogPut(cursor, " INT sites -> BOP (full 64K scan, last off 0x"); cursor = LogHex(cursor, last);
      cursor = LogPut(cursor, "), of which "); cursor = LogHex(cursor, votedCount);
      cursor = LogPut(cursor, " were UNLISTED vectors vouched for by the x86len vote\r\n");
      LogAppend(LOG_PATH, base, cursor); SerialOut(base, cursor); cursor = base;
      /* WHAT DID THE PATCH LEAVE BEHIND?:
       * Every `CD nn` still in this region is a vector we did not claim,
       * and a PM guest cannot reach the IVT -- so if the guest executes
       * one, the kernel #GP reflect silently terminates the VDM. No
       * exception, no log line, the process simply gone. That is how
       * krnl386 died on INT 2Fh, and finding it meant reading this
       * function's constant list by hand afterwards.
       * So say it up front, as a histogram by vector. It is an UPPER
       * BOUND -- a linear byte-pair count over a region that contains data
       * as well as code, so some of these are not instructions at all --
       * but a vector that is ABSENT here cannot kill the guest, which
       * makes the list a genuine shortlist of suspects rather than a
       * guess. Cheap, and it turns the next silent death into a lookup.
       */
      {   DWORD hist[BYTE_VALUES], offset2, total = 0;
      INT number;

          for (number = 0; number < BYTE_VALUES; ++number)
              hist[number] = 0;

          for (offset2 = 0; offset2 < X86_SEGMENT_LIMIT_64K; ++offset2)
              if (cs[offset2] == X86_OP_INT)
              {
                  ++hist[cs[offset2 + 1]];
                  ++total;
              }

          /* AND THE OFFSETS, NOT JUST THE HISTOGRAM (Importance = 2):
           * A histogram says a vector is a suspect; it does not say WHERE,
           * so acting on it still means reading the binary by hand. Session
           * 32 needed exactly that: a `cd 02` in krnl386 turned out to be
           * REAL CODE the guest executes, left RAW by the boundary vote --
           * proved by arming a breakpoint on it and reading back
           * `displaced cd 02` (a patched site is an INT site, and
           * DpmiBreakpointArm refuses those, so it could not have armed at all).
           * A raw `CD nn` in protected mode is not a warning, it is a silent
           * VDM teardown waiting for the guest to take that branch. Print the
           * addresses so the next one is a breakpoint away instead of a
           * disassembly session. Bounded so a data-heavy region cannot flood.
           */
          {   DWORD shown = 0;
              cursor = LogPut(cursor, "DPMI: residual CD nn SITES (linear, first 24):");

              for (offset2 = 0; offset2 < X86_SEGMENT_LIMIT_64K && shown < 24; ++offset2)
                  if (cs[offset2] == X86_OP_INT)
                  {
                      cursor = LogPut(cursor, " 0x"); cursor = LogHex(cursor, (DWORD)(ULONG_PTR)(cs + offset2));
                      cursor = LogPut(cursor, "="); cursor = LogHexByte(cursor, cs[offset2 + 1]);
                      ++shown;
                  }

              cursor = LogPut(cursor, "\r\n");
              LogAppend(LOG_PATH, base, cursor); SerialOut(base, cursor); cursor = base;
          }
          cursor = LogPut(cursor, "DPMI: residual CD nn in the region: "); cursor = LogHex(cursor, total);
          cursor = LogPut(cursor, " (unclaimed vectors, upper bound)");

          for (number = 0; number < BYTE_VALUES; ++number) if (hist[number])
          {
              cursor = LogPut(cursor, " "); cursor = LogHexByte(cursor, (BYTE)number);
              cursor = LogPut(cursor, "h x"); cursor = LogHex(cursor, hist[number]);
          }

          cursor = LogPut(cursor, "\r\n");
          LogAppend(LOG_PATH, base, cursor); SerialOut(base, cursor); cursor = base;
      }
    }
    return cursor;
}

/* The client asked to switch to protected mode (the DPMI entry BOP): build its initial selectors and PSP selector, start the watchdog, patch its INT sites, load the session's switches, run it in PM until it stops for good, and end the session -- or report that the switch failed. */
static INT DpmiStartClientSession(
    PSTR *cursorIo,
    PSTR const base,
    volatile BYTE * const tib,
    DOS_MACHINE *machine)
{
    PSTR cursor = *cursorIo;
    DWORD currentCs = VDM_REG16(tib, VTIB_CS);
    DWORD currentIp = VDM_REG16(tib, VTIB_EIP);
    LONG registerStatus = 0;
    LONG setStatus = 0;
    INT switched;
    /* AX bit0 = the client's declared width (0=16-bit, 1=32-bit e.g. DOS/4GW).
     * Logged and recorded, but it does NOT set the initial selectors' D/B --
     * see DpmiSwitchToProtectedMode(); doing so ran DOS/4GW's 16-bit stub as 32-bit.
     */
    INT is32 = (INT)(VDM_REG(tib, VTIB_EAX) & 1);

    cursor = LogPut(cursor, "STAGE3: DPMI_BOP far-call LANDED @ 0x"); cursor = LogHex(cursor, currentCs);
    cursor = LogPut(cursor, ":0x"); cursor = LogHex(cursor, currentIp);
    cursor = LogPut(cursor, is32 ? " -- switching to PM (32-bit client)\r\n"
                     : " -- switching to PM (16-bit client)\r\n");
    LogAppend(LOG_PATH, base, cursor); SerialOut(base, cursor); cursor = base;
    switched = DpmiSwitchToProtectedMode(tib, is32, &registerStatus, &setStatus);
    cursor = LogPut(cursor, " [svc11=0x"); cursor = LogHex(cursor, (UINT)registerStatus);
    cursor = LogPut(cursor, " svc10=0x"); cursor = LogHex(cursor, (UINT)setStatus); cursor = LogPut(cursor, "]");
    cursor = LogPut(cursor, " retcs=0x"); cursor = LogHex(cursor, g_DpmiDebug[0]);
    cursor = LogPut(cursor, " clo=0x"); cursor = LogHex(cursor, g_DpmiDebug[2]);
    cursor = LogPut(cursor, " chi=0x"); cursor = LogHex(cursor, g_DpmiDebug[3]);

    if (switched == 0)
    {
        g_DpmiPm = 1;
        g_DpmiCodeBase = g_DpmiSegmentBase[0];   /* CS base = the patch-scan target */
        /* Record the switch's code/data/stack selector bases (indices 1/2/3) so
         * DpmiSelectorBase() translates DS:/ES:/SS: through the right base -- essential
         * once CS!=DS!=SS (a real .EXE); for a .COM all three are equal.
         */
        { INT si;

        for (si = 0; si < DPMI_INITIAL_SELECTOR_COUNT; ++si)
        {
            g_Ldt[DPMI_INITIAL_FIRST_INDEX + si].Base   = g_DpmiSegmentBase[si];
            g_Ldt[DPMI_INITIAL_FIRST_INDEX + si].Limit  = X86_SEGMENT_LIMIT_64K;
            g_Ldt[DPMI_INITIAL_FIRST_INDEX + si].Access = (si == 0) ? DPMI_ACCESS_CODE : DPMI_ACCESS_DATA;
            /* Mirror the D/B width DpmiSwitchToProtectedMode ACTUALLY installed, so
             * DpmiSelectorIs32() (I/O decode + EIP-mask gating) agrees with the live
             * descriptor. That is now always 16-bit for these three: the client's
             * post-switch code must also be valid real-mode code on the failure
             * path, so it cannot be 32-bit. A 32-bit client far-jmps to its OWN
             * INT 31h-allocated 32-bit selectors, which are reported correctly.
             */
            g_Ldt[DPMI_INITIAL_FIRST_INDEX + si].Flags  = 0;
        } }

        if (g_LdtNext < DPMI_FIRST_CLIENT_INDEX)
            g_LdtNext = DPMI_FIRST_CLIENT_INDEX;                                           /* client allocs start at index 4 now */

        g_LdtClientMark = g_LdtNext;          /* teardown gives back everything above */
        /* -- DPMI INITIAL CLIENT STATE: ES = PSP SELECTOR, AND THE PSP'S
         *  ENVIRONMENT POINTER CONVERTED TO A SELECTOR. --------------------
         * DpmiSwitchToProtectedMode() sets ES = DS (a second copy of the data selector),
         * and that is simply wrong. DPMI 0.9, "entering protected mode", on the
         * register state at a successful return:
         *   CS = 16-bit selector with base of real mode CS and a 64K limit
         *   SS = Selector with base of real mode SS and a 64K limit
         *   DS = Selector with base of real mode DS and a 64K limit
         *   ES = Selector to program's PSP with a 100h byte limit
         * and, separately: "The environment pointer in the current program's PSP
         * will automatically be converted to a descriptor."
         *
         * THIS IS NOT A SPEC DETAIL WE ARE HONOURING FOR TIDINESS -- it is what
         * killed Doom for four sessions. DOS/4GW's PM module reads the
         * environment field at +0x2c of whatever the initial ES selects and
         * loads it as a SELECTOR (observed). With ES pointing at the data segment
         * instead of the PSP, +0x2c is an arbitrary code byte pair -- measured as
         * 0x8b17, LDT index 4450 -- and that segment load #GPs, which XP answers by
         * terminating the whole VDM with no
         * exception we can catch. The client is thus its own second witness for
         * BOTH halves of the rule, independently of the spec text.
         *
         * The environment field is left holding the SELECTOR from here on. The
         * spec makes restoring it the client's job before it terminates ("it must
         * restore it to the selector created by the DPMI host"), and nothing in
         * our DOS layer reads PSP+0x2C -- dos_psp.h writes it once at load and no
         * reader exists (checked). If one is ever added, it must not assume a
         * segment after a DPMI switch.
         */
        { WORD psp = machine->PspSegment;
          DWORD pspBase = (DWORD)psp << PARAGRAPH_SHIFT;
          WORD pspSelector = 0;
          WORD environmentSelector = 0;

          if (g_LdtNext < DPMI_LDT_MAX)
          {
              INT pspIndex = g_LdtNext++;
              g_Ldt[pspIndex].Base   = pspBase;
              g_Ldt[pspIndex].Limit  = DOS_PSP_SIZE - 1;        /* "a 100h byte limit", exactly */
              g_Ldt[pspIndex].Access = DPMI_ACCESS_DATA;        /* present, DPL3, data R/W */
              g_Ldt[pspIndex].Flags  = 0;
              DpmiInstall(pspIndex);
              pspSelector = (WORD)DPMI_LDT_SELECTOR(pspIndex);
              VDM_SET16(tib, VTIB_ES, pspSelector);
          }

          { volatile WORD *environmentField = (volatile WORD *)(ULONG_PTR)(pspBase + DOS_PSP_ENVIRONMENT);
            WORD environmentSegment = *environmentField;
            /* environmentSegment == 0 is legal and documented: a client may free its
             * environment and zero this word BEFORE switching, in which case
             * there is nothing to convert and we must not invent a descriptor.
             */
            if (environmentSegment && g_LdtNext < DPMI_LDT_MAX)
            {
                INT entryIndex = g_LdtNext++;
                g_Ldt[entryIndex].Base   = (DWORD)environmentSegment << PARAGRAPH_SHIFT;
                g_Ldt[entryIndex].Limit  = DOS_PSP_SIZE - 1;      /* DosEnvBuild fills a 0x10-para block */
                g_Ldt[entryIndex].Access = DPMI_ACCESS_DATA;
                g_Ldt[entryIndex].Flags  = 0;
                DpmiInstall(entryIndex);
                environmentSelector = (WORD)DPMI_LDT_SELECTOR(entryIndex);
                *environmentField = environmentSelector;
            }
          }
          DpmiInstallDefaultPmHandlers(machine);
          cursor = LogPut(cursor, " PSP 0x"); cursor = LogHex(cursor, psp);
          cursor = LogPut(cursor, " -> ES=0x"); cursor = LogHex(cursor, pspSelector);
          cursor = LogPut(cursor, " env -> sel 0x"); cursor = LogHex(cursor, environmentSelector);
        }
        cursor = LogPut(cursor, " segbase C=0x"); cursor = LogHex(cursor, g_DpmiSegmentBase[0]);
        cursor = LogPut(cursor, " D=0x"); cursor = LogHex(cursor, g_DpmiSegmentBase[1]);
        cursor = LogPut(cursor, " S=0x"); cursor = LogHex(cursor, g_DpmiSegmentBase[2]);
        cursor = LogPut(cursor, " -> PM ok (CS=0x"); cursor = LogHex(cursor, VDM_REG16(tib, VTIB_CS));
        cursor = LogPut(cursor, ":0x"); cursor = LogHex(cursor, VDM_REG16(tib, VTIB_EIP));
        cursor = LogPut(cursor, ") -> DPMI PM loop\r\n");
        LogAppend(LOG_PATH, base, cursor); SerialOut(base, cursor); cursor = base;
        /* run 53: the emulation path -- execute PM in the host interpreter instead of
         * the kernel (which deadlocks on a PM #GP, run 52). No BOP patch: the interpreter
         * reads the raw CD nn and stops on it, and we service through the same dispatch.
         * No kernel watchdog here either -- the interpreter has its own guard cap, and the
         * watchdog's 3s TerminateProcess would guillotine a long (millions-of-insn) run.
         */
        if (g_DpmiUseInterp)
        {
            cursor = LogPut(cursor, "DPMI: run 53 -- PM in host interpreter (no kernel, no BOP patch)\r\n");
            LogAppend(LOG_PATH, base, cursor); SerialOut(base, cursor); cursor = base;
            DpmiRunPmInterp(machine, tib);
            {
                *cursorIo = cursor;
                return HOST_FLOW_BREAK;
            }
        }

        /* Safety watchdog (kernel PM path only): an un-terminable spin still self-kills
         * after ~3s so the batch dumps the log.
         */
        { HANDLE watchdogThread = CreateThread(NULL, 0, DpmiWatchdog,
                                   (LPVOID)(ULONG_PTR)g_DpmiWatchdogGeneration, 0, NULL);

          if (watchdogThread)
              CloseHandle(watchdogThread);

          /* Prove creation FROM THIS THREAD. The watchdog's own first line is
           * written by the new thread, so its absence is ambiguous -- it cannot
           * distinguish "thread never created" from "created but the process was
           * killed before it was ever scheduled". Doom's log shows neither that
           * line nor any sample, so we need the difference.
           */
          cursor = LogPut(cursor, "STAGE3-DPMI: watchdog thread created h="); cursor = LogHex(cursor, (DWORD)(ULONG_PTR)watchdogThread);
          cursor = LogPut(cursor, "\r\n");
          LogAppend(LOG_PATH, base, cursor);
          SerialOut(base, cursor);
          cursor = base; }
        cursor = DpmiPatchClientIntSitesUpFront(cursor, base);
        cursor = DpmiLoadSessionKnobs(cursor, base);
        cursor = DpmiInstallFaultReflect(cursor, base);

        DpmiRunClient(&cursor, base, tib, machine);
        {
            INT flow = DpmiEndClientSession(&cursor, base, tib, machine);

            if (flow == HOST_FLOW_BREAK)
            {
                *cursorIo = cursor;
                return HOST_FLOW_BREAK;
            }

            if (flow == HOST_FLOW_CONTINUE)
            {
                *cursorIo = cursor;
                return HOST_FLOW_CONTINUE;
            }
        }
    }

    cursor = LogPut(cursor, " -> SWITCH FAILED (staying real mode, CF=1)\r\n");
    LogAppend(LOG_PATH, base, cursor); SerialOut(base, cursor); cursor = base;
    VDM_REG(tib, VTIB_EFLAGS) |= EFLAGS_CF;         /* CF=1 signals failure to the client */
    VDM_REG(tib, VTIB_EIP) += VDM_BOP_LENGTH;            /* -> the RETF, returns real mode */
    {
        *cursorIo = cursor;
        return HOST_FLOW_CONTINUE;
    }
    *cursorIo = cursor;
    return HOST_FLOW_NEXT;
}

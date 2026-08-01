/* dpmi_enter.S -- monitor PM-entry, a faithful port of ntvdm's 0xf04483c.
 *
 * void dpmi_enter_pm(volatile BYTE *tib);
 *
 * Runs the guest in protected mode the way ntvdm does, so a PM fault / INT is
 * reflected by the KERNEL's VDM trap handler (not Win32 SEH). The trick: save the
 * host register state into the VDM_TIB host-save CONTEXT (at TIB+0x0C, standard
 * i386 CONTEXT offsets) so the kernel can return to us, set the [0x714] "in-monitor"
 * flag, load the guest register file, and FAR-JMP into the guest. When the guest
 * faults the kernel saves the guest state to TIB+0x2D8, restores the host-save
 * CONTEXT, sets VTIB_EVENT, and resumes at our saved return address -- so from C
 * this function simply "returns" once the guest stops, like v86_run().
 *
 * Recovered from XP ntvdm (see docs/research/dpmi-under-ntvdmcontrol.md, VM run 10).
 * VTIB register fields: EDI=0x374 ESI=0x378 EBX=0x37C EDX=0x380 ECX=0x384 EAX=0x388
 * EBP=0x38C EIP=0x390 CS=0x394 EFLAGS=0x398 ESP=0x39C SS=0x3A0; GS=0x364 FS=0x368
 * ES=0x36C DS=0x370. Host-save CONTEXT base = TIB+0x0C. VTIB_EVENT=0x5A8. */

    .intel_syntax noprefix
    .data
    .align 4
g_gsp:  .long 0                 /* guest ESP (for lss) */
g_gss:  .word 0                 /* guest SS  (for lss) */
        .word 0
g_gip:  .long 0                 /* guest EIP (for far jmp) */
g_gcs:  .word 0                 /* guest CS  (for far jmp) */
        .word 0

    .text
    .global _dpmi_enter_pm
    .global dpmi_enter_pm
_dpmi_enter_pm:
dpmi_enter_pm:
    push ebp
    mov  ebp, esp
    push ebx
    push esi
    push edi
    /* fs = 0x3B (the standard user TEB selector), then TIB = fs:[0x18] -> [+0xF18] */
    mov  ebx, 0x3B
    mov  fs, bx
    mov  ebx, fs:[0x18]
    mov  ebx, [ebx + 0xF18]         /* ebx = VDM_TIB */

    /* [0x714] "in-monitor" flag, gated on the guest IF (VTIB_EFLAGS & 0x200) */
    test dword ptr [ebx + 0x398], 0x200
    je   1f
    lock or  dword ptr ds:[0x714], 0x200
    test dword ptr ds:[0x714], 3
    jne  2f                          /* interrupt pending: don't enter, event=3 */
    jmp  3f
1:  lock and dword ptr ds:[0x714], 0xFFFFFDFF
3:
    /* --- save host CONTEXT to [ebx+0x0C] (i386 CONTEXT offsets) --- */
    mov  [ebx + 0xBC], eax          /* Eax  (0x0C+0xB0) */
    mov  eax, [ebp - 4]             /* saved ebx */
    mov  [ebx + 0xB0], eax          /* Ebx  (0x0C+0xA4) */
    mov  [ebx + 0xB8], ecx          /* Ecx  */
    mov  [ebx + 0xB4], edx          /* Edx  */
    mov  eax, [ebp - 8]             /* saved esi */
    mov  [ebx + 0xAC], eax          /* Esi  */
    mov  eax, [ebp - 12]            /* saved edi */
    mov  [ebx + 0xA8], eax          /* Edi  */
    mov  eax, [ebp]                 /* saved ebp */
    mov  [ebx + 0xC0], eax          /* Ebp  (0x0C+0xB4) */
    lea  eax, [ebp + 8]             /* host esp after a normal ret */
    mov  [ebx + 0xD0], eax          /* Esp  (0x0C+0xC4) */
    mov  eax, [ebp + 4]             /* return address */
    mov  [ebx + 0xC4], eax          /* Eip  (0x0C+0xB8) */
    mov  eax, cs
    mov  [ebx + 0xC8], eax          /* SegCs */
    mov  eax, ss
    mov  [ebx + 0xD4], eax          /* SegSs */
    mov  eax, ds
    mov  [ebx + 0xA4], eax          /* SegDs */
    mov  eax, es
    mov  [ebx + 0xA0], eax          /* SegEs */
    mov  eax, fs
    mov  [ebx + 0x9C], eax          /* SegFs */
    mov  eax, gs
    mov  [ebx + 0x98], eax          /* SegGs */
    pushfd
    pop  eax
    mov  [ebx + 0xCC], eax          /* EFlags */

    /* --- enter the guest (TF-clear path) --- */
    mov  eax, [ebx + 0x394]         /* guest CS  */
    mov  word ptr [g_gcs], ax
    mov  eax, [ebx + 0x390]         /* guest EIP */
    mov  [g_gip], eax
    mov  eax, [ebx + 0x3A0]         /* guest SS  */
    mov  word ptr [g_gss], ax
    mov  eax, [ebx + 0x39C]         /* guest ESP */
    mov  [g_gsp], eax

    mov  eax, esp
    push dword ptr [ebx + 0x388]    /* EAX */
    push dword ptr [ebx + 0x384]    /* ECX */
    push dword ptr [ebx + 0x380]    /* EDX */
    push dword ptr [ebx + 0x37C]    /* EBX */
    push eax                        /* ESP placeholder (popal ignores) */
    push dword ptr [ebx + 0x38C]    /* EBP */
    push dword ptr [ebx + 0x378]    /* ESI */
    push dword ptr [ebx + 0x374]    /* EDI */
    push dword ptr [ebx + 0x368]    /* FS */
    push dword ptr [ebx + 0x364]    /* GS */
    push dword ptr [ebx + 0x370]    /* DS */
    push dword ptr [ebx + 0x36C]    /* ES */
    push dword ptr [ebx + 0x398]    /* EFLAGS */
    popfd
    pop  es
    pop  ds
    pop  gs
    pop  fs
    popad
    /* DS/ES are now the guest's (64K limit) -- reach the globals via CS (flat), as
       ntvdm does (cs:[0xf064ad4]); without the override these fault (VM run 11). */
    lss  esp, cs:[g_gsp]            /* guest SS:ESP */
    jmp  fword ptr cs:[g_gip]       /* far jmp to guest CS:EIP */

    /* --- interrupt-pending: report event 3 and return without entering --- */
2:  mov  word ptr [ebx + 0x5A8], 3
    mov  word ptr [ebx + 0x5AC], 0
    mov  word ptr [ebx + 0x5B0], 0
    pop  edi
    pop  esi
    pop  ebx
    mov  esp, ebp
    pop  ebp
    ret

/* dpmi_trapc -- the VDM "trap continue" handler (VDM_INITIALIZE_DATA.TrapcHandler).
   A faithful port of ntvdm 0xf044820: the kernel calls this (with ebx = VDM_TIB) to
   (re)enter the guest -- it far-returns to the guest CS:EIP from the VDM_TIB. Our
   previous empty stub left the kernel's PM trap path unable to complete. */
    .global _dpmi_trapc
    .global dpmi_trapc
_dpmi_trapc:
dpmi_trapc:
    and  esp, 0xFFFF
    push dword ptr [ebx + 0x394]    /* guest CS  */
    push dword ptr [ebx + 0x390]    /* guest EIP */
    mov  ebx, [ebx + 0x37C]         /* guest EBX */
    retf

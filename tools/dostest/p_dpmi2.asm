; p_dpmi2.com -- INT 31h, the #248 remainders (GH #268) and the 0303h callback contract
; (GH #267), from a real 16-bit DPMI client. The sequel to p_dpmi31.com, same harness.
;
; WHAT IT ASKS, in this order (the callback last: it is the one row that can hang a host):
;   0006h  on null / GDT / freed selectors       spec: 8022h        (int31.0006.*)
;   0008h  limits above 1 MB                     spec: 8021h unless the low 12 bits are set
;   0009h  access words                          spec: 8021h for S=0, conforming code, CH bit 5
;          and DPL 0 (8092h, ZAR's call)         spec: 8021h -- OURS ACCEPTS IT ON PURPOSE
;   0007h  base 8000_0000h                       XP: 8025h (past the LDT cap); a DOS host
;                                                with a 4 GB linear space may accept it
;   0100h  BX=2000h (128 KB): a CHAIN of two selectors -- 0003h's increment, 0006h on both,
;          LSL on both (the descriptor the CPU holds, not our table), a byte written through
;          the second read back through the first at +10000h; 0101h frees the chain
;   0500h  the 30h-byte block, dumped; then the free-page delta across a 1 MB 0501h
;   0303h  a callback whose PM procedure records what it was handed (DS:SI, ES:DI, the
;          RMCS SS:SP and CS:IP, AX, the virtual IF), pops the far return through DS:SI
;          AS THE SPEC'S PROCEDURE DOES, and hands back AX=5678h with CF set. A 0301h
;          real-mode procedure far-calls it and records what came back.
;
; ⚠ GRADING. Rows marked `expect` are the spec's answer. Rows marked `info` have none
;   (the spec leaves the value to the host, or XP's cap decides it) and exist to be
;   compared across hosts: stock ntvdm, HDPMI, CWSDPMI, Windows 9x.
; ⚠ A HOST WITH THE PRE-#267 CONTRACT pre-popped the return itself, so this procedure's
;   own pop takes two caller words as CS:IP and the real-mode procedure never comes back:
;   0301h then ends as the host's NO-RET (ours: 128 events) -- `cb.0301` is the row.
;
; nasm -f bin -I tools/dostest/ p_dpmi2.asm -o p_dpmi2.com

        org     100h
        jmp     start
%include "probe.inc"

pm_capture:                             ; probe_capture for PM: DS <- SS (see p_dpmi31)
        pushf
        push    ds
        push    ax
        mov     ax, ss
        mov     ds, ax
        pop     ax
        mov     [__ax], ax
        mov     [__bx], bx
        mov     [__cx], cx
        mov     [__dx], dx
        mov     [__si], si
        mov     [__di], di
        mov     [__es], es
        pop     ax
        mov     [__ds], ax
        pop     ax
        mov     [__fl], ax
        mov     ax, [__ax]
        ret

%macro EXPECT_ERR 2
        call    pm_capture
        xor     bx, bx
        test    word [__fl], 1
        jz      %%n
        cmp     word [__ax], %1
        jne     %%n
        inc     bx
%%n:    mov     [__ax], bx
        EMIT    %2, "AX"
%endmacro
%macro EXPECT_OK 1
        call    pm_capture
        xor     bx, bx
        test    word [__fl], 1
        jnz     %%n
        inc     bx
%%n:    mov     [__ax], bx
        EMIT    %1, "AX"
%endmacro
; AX <- 1 if BX holds 1 (a derived verdict computed by the caller), emit as %1.
%macro VERDICT 1
        mov     ax, bx
        clc
        call    pm_capture
        EMIT    %1, "AX"
%endmacro

start:
        PROBE_BEGIN "dpmi2"
        mov     [rmseg], cs                     ; the real-mode segment (= PSP for a .COM)

        mov     ah, 4Ah                         ; shrink to 64 KB: DOS memory for 0100h
        mov     bx, 1000h
        int     21h

        mov     ax, 1687h
        int     2Fh
        or      ax, ax
        jz      have_dpmi
        call    probe_capture
        EMIT    "nodpmi", "AX"                  ; MS-DOS 6.22 / DOSBox-X / PCem: AX = 1687h
        PROBE_END

have_dpmi:
        mov     [entry], di
        mov     [entry+2], es
        or      si, si
        jz      .nopriv
        mov     bx, si
        mov     ah, 48h
        int     21h
        jc      switch_fail
        mov     es, ax
.nopriv:
        xor     ax, ax                          ; a 16-bit client
        call    far [entry]
        jc      switch_fail
        jmp     in_pm
switch_fail:
        call    probe_capture
        EMIT    "switch.FAILED", "AX,CF"
        PROBE_END

in_pm:
        mov     [datasel], ds
        mov     ax, 0003h
        int     31h
        mov     [selinc], ax

        ; ══ 0006h ═══════════════════════════════════════════════════════════════
        xor     bx, bx
        mov     cx, 0C1C1h
        mov     dx, 0D1D1h
        mov     ax, 0006h
        int     31h
        EXPECT_ERR 8022h, "int31.0006.null"     ; expect 1
        mov     bx, 0028h
        mov     ax, 0006h
        int     31h
        EXPECT_ERR 8022h, "int31.0006.gdt"      ; expect 1
        mov     cx, 1
        mov     ax, 0000h
        int     31h
        mov     [sel1], ax
        mov     bx, ax
        mov     cx, 0012h                       ; control: a base we set reads back
        mov     dx, 3450h
        mov     ax, 0007h
        int     31h
        mov     bx, [sel1]
        mov     ax, 0006h
        int     31h
        call    pm_capture
        xor     bx, bx
        test    word [__fl], 1
        jnz     .b1
        cmp     word [__cx], 0012h
        jne     .b1
        cmp     word [__dx], 3450h
        jne     .b1
        inc     bx
.b1:    VERDICT "int31.0006.live"               ; expect 1
        mov     bx, [sel1]
        mov     ax, 0001h
        int     31h
        mov     bx, [sel1]
        mov     ax, 0006h
        int     31h
        EXPECT_ERR 8022h, "int31.0006.freed"    ; expect 1

        ; ══ 0008h / 0009h / 0007h values ════════════════════════════════════════
        mov     cx, 1
        mov     ax, 0000h
        int     31h
        mov     [sel1], ax
        mov     bx, ax
        mov     cx, 0012h
        mov     dx, 3456h
        mov     ax, 0008h
        int     31h
        EXPECT_ERR 8021h, "int31.0008.gt1mb_lowbits_456"   ; expect 1
        mov     bx, [sel1]
        mov     cx, 0010h
        xor     dx, dx
        mov     ax, 0008h
        int     31h
        EXPECT_ERR 8021h, "int31.0008.exactly_1mb"         ; expect 1
        mov     bx, [sel1]
        mov     cx, 001Fh
        mov     dx, 0FFFFh
        mov     ax, 0008h
        int     31h
        EXPECT_OK "int31.0008.gt1mb_page_granular"         ; expect 1
        mov     bx, [sel1]
        mov     cx, 000Fh
        mov     dx, 0FFFFh
        mov     ax, 0008h
        int     31h
        EXPECT_OK "int31.0008.1mb_minus_1"                 ; expect 1

        mov     bx, [sel1]
        mov     cx, 00E2h                       ; S = 0
        mov     ax, 0009h
        int     31h
        EXPECT_ERR 8021h, "int31.0009.system"   ; expect 1
        mov     bx, [sel1]
        mov     cx, 00FEh                       ; conforming code
        mov     ax, 0009h
        int     31h
        EXPECT_ERR 8021h, "int31.0009.conforming"   ; expect 1
        mov     bx, [sel1]
        mov     cx, 20F2h                       ; CH bit 5
        mov     ax, 0009h
        int     31h
        EXPECT_ERR 8021h, "int31.0009.ch_bit5"  ; expect 1
        mov     bx, [sel1]
        mov     cx, 00F2h
        mov     ax, 0009h
        int     31h
        EXPECT_OK "int31.0009.data_dpl3"        ; expect 1
        mov     bx, [sel1]
        mov     cx, 8092h                       ; ZAR's call: DPL 0, G
        mov     ax, 0009h
        int     31h
        call    pm_capture
        EMIT    "int31.0009.dpl0", "AX,CF"      ; info -- spec 8021h; ours CF=0 ON PURPOSE (ZAR)
        mov     bx, [sel1]
        mov     cx, 00F2h
        mov     ax, 0009h
        int     31h

        mov     bx, [sel1]
        mov     cx, 8000h
        xor     dx, dx
        mov     ax, 0007h
        int     31h
        call    pm_capture
        EMIT    "int31.0007.base_80000000", "AX,CF"   ; info -- XP: 8025h; a 4 GB host may accept
        mov     bx, [sel1]
        mov     ax, 0001h
        int     31h

        ; ══ 0100h, 128 KB: the chain ════════════════════════════════════════════
        mov     bx, 2000h
        mov     ax, 0100h
        int     31h
        mov     [dseg], ax
        mov     [dsel], dx
        EXPECT_OK "int31.0100.128k"             ; expect 1
        test    word [__fl], 1
        jz      .gotblk
        jmp     no_chain
.gotblk:
        mov     ax, [dsel]
        add     ax, [selinc]
        mov     [dsel2], ax
        ; 0006h on both: base, base + 10000h
        mov     bx, [dsel]
        mov     ax, 0006h
        int     31h
        call    pm_capture
        EMIT    "int31.0100.base0", "CX,DX,CF"  ; info (the segment differs per host)
        mov     ax, [dseg]                      ; expected linear = seg * 16
        mov     dx, ax
        shl     ax, 4
        shr     dx, 12
        xor     bx, bx
        test    word [__fl], 1
        jnz     .c1
        cmp     [__dx], ax
        jne     .c1
        cmp     [__cx], dx
        jne     .c1
        inc     bx
.c1:    VERDICT "int31.0100.base0_is_seg"       ; expect 1
        mov     bx, [dsel2]
        mov     ax, 0006h
        int     31h
        call    pm_capture
        mov     ax, [dseg]
        mov     dx, ax
        shl     ax, 4
        shr     dx, 12
        inc     dx                              ; + 10000h
        xor     bx, bx
        test    word [__fl], 1
        jnz     .c2
        cmp     [__dx], ax
        jne     .c2
        cmp     [__cx], dx
        jne     .c2
        inc     bx
.c2:    VERDICT "int31.0100.base1_is_seg_plus_64k"   ; expect 1
        ; LSL on both: the descriptors the CPU holds
        xor     eax, eax
        movzx   ebx, word [dsel]
        lsl     eax, ebx
        mov     [lim0], eax
        xor     eax, eax
        movzx   ebx, word [dsel2]
        lsl     eax, ebx
        mov     [lim1], eax
        mov     ax, [lim0]
        mov     bx, [lim0 + 2]
        mov     cx, [lim1]
        mov     dx, [lim1 + 2]
        clc
        call    pm_capture
        EMIT    "int31.0100.lsl", "AX,BX,CX,DX"   ; expect AX=FFFF BX=0001 CX=FFFF DX=0000
        ; a byte through the second selector, read back through the first at +10000h
        push    es
        mov     es, [dsel2]
        mov     byte [es:0], 5Ah
        mov     es, [dsel]
        xor     bx, bx
        mov     edi, 10000h
        cmp     byte [es:edi], 5Ah
        jne     .c3
        inc     bx
.c3:    pop     es
        VERDICT "int31.0100.chain_aliases"      ; expect 1
        mov     dx, [dsel]
        mov     ax, 0101h
        int     31h
        EXPECT_OK "int31.0101.chain"            ; expect 1
        mov     bx, [dsel2]
        mov     ax, 0006h
        int     31h
        EXPECT_ERR 8022h, "int31.0101.chain_second_freed"   ; expect 1
no_chain:

        ; ══ 0500h ═══════════════════════════════════════════════════════════════
        push    ds
        pop     es
        mov     di, minfo
        mov     cx, 30h
        mov     al, 0A5h
        cld
        rep     stosb
        mov     di, minfo
        mov     ax, 0500h
        int     31h
        call    pm_capture
        EMIT    "int31.0500", "CF"              ; expect CF=0
        EMIT_BUF "int31.0500.buf", minfo, 30h   ; info -- the block, all 48 bytes
        mov     ax, [minfo + 14h]               ; free pages before
        mov     [fp0], ax
        mov     ax, [minfo + 16h]
        mov     [fp0 + 2], ax
        mov     bx, 0010h                       ; 0501h: 1 MB
        xor     cx, cx
        mov     ax, 0501h
        int     31h
        mov     [hnd], di
        mov     [hnd + 2], si
        push    ds
        pop     es
        mov     di, minfo
        mov     ax, 0500h
        int     31h
        mov     ax, [fp0]                       ; delta = before - after (low word)
        sub     ax, [minfo + 14h]
        clc
        call    pm_capture
        EMIT    "int31.0500.freepages_delta_1mb", "AX"   ; info -- ours 0100; spec: a decrease
        mov     si, [hnd + 2]
        mov     di, [hnd]
        mov     ax, 0502h
        int     31h

        ; ══ 0303h: the callback round trip ══════════════════════════════════════
        push    ds
        pop     es
        mov     di, cbrmcs                      ; ES:DI = its RMCS (our data)
        mov     si, cbproc                      ; DS:SI = the PM procedure...
        push    ds
        mov     ax, cs                          ; ...DS = our CODE selector
        mov     ds, ax
        mov     ax, 0303h
        int     31h
        pop     ds                              ; (flags untouched for the capture)
        mov     [cbaddr], dx
        mov     [cbaddr + 2], cx
        EXPECT_OK "cb.0303"                     ; expect 1
        test    word [__fl], 1
        jz      .cbgo
        jmp     cb_done
.cbgo:  mov     di, rmcs                        ; 0301h -> rmproc in V86
        mov     cx, 32h / 2
        xor     ax, ax
        push    ds
        pop     es
        rep     stosw
        mov     ax, [rmseg]
        mov     [rmcs + 2Ch], ax                ; CS
        mov     word [rmcs + 2Ah], rmproc       ; IP
        mov     [rmcs + 30h], ax                ; SS: our own real-mode stack buffer
        mov     word [rmcs + 2Eh], rmstack_top  ; SP
        mov     [rmcs + 24h], ax                ; DS
        mov     word [rmcs + 20h], 0202h
        mov     di, rmcs
        xor     bx, bx
        xor     cx, cx
        mov     ax, 0301h
        int     31h
        EXPECT_OK "cb.0301"                     ; expect 1 (CF=0: the RM procedure returned)
        mov     ax, [cb_count]
        clc
        call    pm_capture
        EMIT    "cb.count", "AX"                ; expect AX=0001
        ; DS:SI = the real-mode SS:SP AT THE CALL (the far return still on it)
        mov     ax, [rm_sp_before]
        sub     ax, 4
        xor     bx, bx
        cmp     [cb_si], ax
        jne     .d1
        inc     bx
.d1:    VERDICT "cb.si_is_rm_sp"                ; expect 1
        mov     bx, [cb_ds]
        mov     ax, 0006h
        int     31h
        call    pm_capture
        mov     ax, [rmseg]
        mov     dx, ax
        shl     ax, 4
        shr     dx, 12
        xor     bx, bx
        test    word [__fl], 1
        jnz     .d2
        cmp     [__dx], ax
        jne     .d2
        cmp     [__cx], dx
        jne     .d2
        inc     bx
.d2:    VERDICT "cb.ds_base_is_rm_ss"           ; expect 1
        ; ES:DI = the RMCS the 0303h was given
        xor     bx, bx
        mov     ax, [cb_es]
        cmp     ax, [datasel]
        jne     .d3
        cmp     word [cb_di], cbrmcs
        jne     .d3
        inc     bx
.d3:    VERDICT "cb.es_di_is_rmcs"              ; expect 1
        ; the RMCS at entry: SS:SP unpopped, the real-mode AX
        mov     ax, [rm_sp_before]
        sub     ax, 4
        xor     bx, bx
        cmp     [cb_rsp], ax
        jne     .d4
        mov     ax, [rmseg]
        cmp     [cb_rss], ax
        jne     .d4
        inc     bx
.d4:    VERDICT "cb.rmcs_ss_sp_at_call"         ; expect 1 (pre-#267 ours: SP 4 higher)
        mov     ax, [cb_rax]
        clc
        call    pm_capture
        EMIT    "cb.rmcs_ax_in", "AX"           ; expect AX=1234
        mov     ax, [cb_rip]
        mov     bx, [cb_rcs]
        mov     cx, [cbaddr]
        mov     dx, [cbaddr + 2]
        clc
        call    pm_capture
        EMIT    "cb.rmcs_csip", "AX,BX,CX,DX"   ; info -- spec leaves CS:IP undefined (ours: = CX:DX)
        ; the far return the procedure popped through DS:SI
        xor     bx, bx
        cmp     word [cb_ret_ip], rm_after_call
        jne     .d5
        mov     ax, [rmseg]
        cmp     [cb_ret_cs], ax
        jne     .d5
        inc     bx
.d5:    VERDICT "cb.popped_far_return"          ; expect 1
        mov     ax, [cb_vif]
        clc
        call    pm_capture
        EMIT    "cb.vif_at_entry", "AX"         ; expect AL=00 (spec: interrupts disabled)
        mov     ax, [cb_ss]
        mov     bx, [cb_sp]
        clc
        call    pm_capture
        EMIT    "cb.pm_stack", "AX,BX"          ; info -- the host's locked stack
        ; what real mode got back
        mov     ax, [rm_ax_after]
        mov     bx, [rm_fl_after]
        and     bx, 1
        xor     cx, cx
        mov     dx, [rm_sp_after]
        cmp     dx, [rm_sp_before]
        jne     .d6
        inc     cx
.d6:    clc
        call    pm_capture
        EMIT    "cb.rm_after", "AX,BX,CX"       ; expect AX=5678 BX=0001 (CF) CX=0001 (SP restored)
        mov     dx, [cbaddr]
        mov     cx, [cbaddr + 2]
        mov     ax, 0304h
        int     31h
        EXPECT_OK "cb.0304"                     ; expect 1
cb_done:
        PROBE_END

; ── the real-mode procedure 0301h runs (V86; CS = DS = SS = rmseg) ──────────────
rmproc:
        mov     [cs:rm_sp_before], sp
        mov     ax, 1234h
        clc
        call    far [cs:cbaddr]
rm_after_call:
        mov     [cs:rm_ax_after], ax
        pushf
        pop     word [cs:rm_fl_after]
        mov     [cs:rm_sp_after], sp
        retf

; ── the PM callback procedure. DS:SI = RM stack, ES:DI = cbrmcs (OUR data -- so the
;    records go through ES), SS:SP = the host's stack. ─────────────────────────────
cbproc:
        mov     [es:cb_ds], ds
        mov     [es:cb_si], si
        mov     [es:cb_es], es
        mov     [es:cb_di], di
        mov     [es:cb_ss], ss
        mov     [es:cb_sp], sp
        mov     ax, 0902h                       ; virtual IF (AL)
        int     31h
        xor     ah, ah
        mov     [es:cb_vif], ax
        mov     ax, [es:di + 2Ah]
        mov     [es:cb_rip], ax
        mov     ax, [es:di + 2Ch]
        mov     [es:cb_rcs], ax
        mov     ax, [es:di + 2Eh]
        mov     [es:cb_rsp], ax
        mov     ax, [es:di + 30h]
        mov     [es:cb_rss], ax
        mov     ax, [es:di + 1Ch]
        mov     [es:cb_rax], ax
        cld                                     ; the spec's procedure: pop the far return
        lodsw
        mov     [es:di + 2Ah], ax
        mov     [es:cb_ret_ip], ax
        lodsw
        mov     [es:di + 2Ch], ax
        mov     [es:cb_ret_cs], ax
        add     word [es:di + 2Eh], 4
        mov     word [es:di + 1Ch], 5678h       ; AX back to real mode
        or      word [es:di + 20h], 1           ; and CF
        inc     word [es:cb_count]
        iret

entry   dw 0, 0
rmseg   dw 0
datasel dw 0
selinc  dw 0
sel1    dw 0
dseg    dw 0
dsel    dw 0
dsel2   dw 0
lim0    dd 0
lim1    dd 0
fp0     dw 0, 0
hnd     dw 0, 0
cbaddr  dw 0, 0
cb_count dw 0
cb_ds   dw 0
cb_si   dw 0
cb_es   dw 0
cb_di   dw 0
cb_ss   dw 0
cb_sp   dw 0
cb_vif  dw 0
cb_rip  dw 0
cb_rcs  dw 0
cb_rsp  dw 0
cb_rss  dw 0
cb_rax  dw 0
cb_ret_ip dw 0
cb_ret_cs dw 0
rm_sp_before dw 0
rm_sp_after  dw 0
rm_ax_after  dw 0
rm_fl_after  dw 0
minfo   times 30h db 0
rmcs    times 32h db 0
cbrmcs  times 32h db 0
rmstack times 200h db 0
rmstack_top:

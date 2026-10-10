; p_crit2.com -- INT 24h for a read/write on an ALREADY-OPEN handle, and for PRN.
; ORACLE-ALSO: pcem
;
; GH #275, the half of #34 that p_crit.asm could not reach: p_crit fails drive A:
; before any handle is open, so every row it has is a PATH call (AH=1Ah, the FAT).
; Here the file is opened first, with the drive working, and only then is the drive
; failed -- so the INT 21h that meets the error is 3Fh / 40h on a live handle and the
; sector that fails is a DATA sector. Questions, per case:
;   .call   AX,CF      what the read/write returned after the handler's answer
;   .int24  AX,BX,CX   handler call count / AH:AL as DOS called it / DI low byte
;   .dev    AX,BX      BP:SI's device header: attribute bit 15 (character device),
;                      and for a character device the first two name bytes ("PR")
;   .59     AX,BX,CX   AH=59h straight after (CH = locus)
; Spec expectation (MS-DOS 4.0 kernel source, see src/dos/dos_err.h -- NOT 6.22):
;   3Fh: AH=3Eh (data area, read, FAIL+RETRY+IGNORE allowed), DI=02 (not ready);
;        FAIL -> AX=0005 CF=1, 59h=0053; IGNORE -> CF=0, AX=0200.
;   40h: AH=3Fh (the same with the write bit).
;   PRN: AH bit 7 set (character device), BP:SI -> the PRN header.
;
; HOW THE FAILURE IS MADE (oracles: A: is the boot floppy and the probe runs from it):
;   * the probe opens ITSELF (P_CRIT2.COM, padded past 1 KB) for reading and creates
;     ZZCRIT2W.TMP with one 512-byte sector in it, with INT 13h untouched;
;   * then, only while armed, its INT 13h hook answers drive 0's read/write/verify
;     with "timeout" (AH=80h) -- and the change line (AH=16h) with NOT CHANGED, so the
;     kernel keeps its buffers and does not go back to the FAT or the boot sector;
;   * each transfer is one whole 512-byte sector at offset 0: DOS moves whole sectors
;     straight to/from the caller (no buffer), and the first cluster is in the SFT, so
;     the only disk access is the DATA sector -- area 3.
;   * PRN: an INT 17h hook answers every call with status AH=[st17] (01h = time-out)
;     while armed; after 200 calls it answers ready (90h) so a kernel that polls for
;     ever cannot hang the run. The count is reported (.n17).
; ⚠ ON THE TEST MACHINE THE DISK HALF CANNOT FAIL: the probe runs from a local folder, Win32's
;   ReadFile/WriteFile there never meet a hardware error, and NTVDMEX's file I/O never
;   goes through INT 13h. Expect n24=0 / CF=0 for crit2.h3f.* / crit2.h40.* on the
;   test machine -- those rows are the CONTRACT, measured on 6.22 and PCem, for the off-VM
;   tests (tests/unit/err_test.c) to be held to. The PRN half does reach the test machine
;   (our PRN driver calls INT 17h through the IVT), and there it is a KNOWN GAP: our
;   driver ignores the BIOS status (#275, src/dos/dos_auxprn.asm).
; ⚠ NOTHING IS PRINTED WHILE ARMED: on the oracles stdout is a file on A: itself.
; ⚠ INT 13h and INT 17h ARE PUT BACK before the end. INT 24h is put back by DOS.
;
; nasm -f bin p_crit2.asm -o p_crit2.com

        org     100h
        jmp     start
%include "probe.inc"

; ---- INT 13h: drive 0 read/write/verify time out while armed ----------------------
hook13:
        cmp     byte [cs:armed], 0
        je      .chain
        cmp     dl, 0
        jne     .chain
        cmp     ah, 16h
        je      .nochange
        cmp     ah, 02h
        je      .timeout
        cmp     ah, 03h
        je      .timeout
        cmp     ah, 04h
        je      .timeout
.chain: jmp     far [cs:old13]
.nochange:
        inc     word [cs:n13]
        mov     ah, 00h                         ; change line: NOT changed
        clc
        retf    2
.timeout:
        inc     word [cs:n13]
        mov     ah, 80h                         ; timeout = drive not ready
        stc
        retf    2

; ---- INT 17h: the printer reports [st17] while armed --------------------------------
hook17:
        cmp     byte [cs:armed], 0
        je      .chain
        inc     word [cs:n17]
        cmp     word [cs:n17], 200
        ja      .ready
        mov     ah, [cs:st17]
        iret
.ready: mov     ah, 90h                         ; not busy, selected: give up failing
        iret
.chain: jmp     far [cs:old17]

; ---- INT 24h: record the first call (+ BP:SI's header), answer act1 then act2 -----
hook24:
        push    ds
        push    bx
        push    es
        push    cs
        pop     ds
        inc     word [n24]
        cmp     word [n24], 1
        jne     .later
        mov     [r24ax], ax
        mov     [r24di], di
        mov     es, bp
        mov     bx, [es:si + 4]                 ; device attribute
        mov     [r24att], bx
        mov     bx, [es:si + 0Ah]               ; first two name bytes
        mov     [r24nm], bx
        mov     al, [act1]
        jmp     .out
.later: mov     al, [act2]
.out:   pop     es
        pop     bx
        pop     ds
        iret

; arm, then run the call at [callp]; CF/AX of it land in c_ax / c_fl
%macro PROVOKE 3                                 ; case, act1, act2
        mov     byte [act1], %2
        mov     byte [act2], %3
        mov     word [n24], 0
        mov     word [n13], 0
        mov     word [n17], 0
        mov     word [r24ax], 0EEEEh
        mov     word [r24di], 0EEEEh
        mov     word [r24att], 0EEEEh
        mov     word [r24nm], 0EEEEh
        mov     ah, 0Dh                          ; flush first: nothing dirty on A:
        int     21h
        mov     byte [armed], 1
        call    [callp]
        pushf
        mov     byte [armed], 0
        mov     [c_ax], ax
        pop     ax
        mov     [c_fl], ax
        ; 59h straight away, before any output can disturb it
        POISON
        mov     ax, 5900h
        xor     bx, bx
        int     21h
        mov     [e_ax], ax
        mov     [e_bx], bx
        mov     [e_cx], cx
        ; ---- report
        mov     ax, [c_ax]
        mov     [__ax], ax
        mov     ax, [c_fl]
        mov     [__fl], ax
        %strcat %%cn_call %1, ".call"
        EMIT    %%cn_call, "AX,CF"
        mov     ax, [n24]
        mov     [__ax], ax
        mov     ax, [r24ax]
        mov     [__bx], ax
        mov     ax, [r24di]
        and     ax, 00FFh                        ; DI's high byte is undefined
        mov     [__cx], ax
        %strcat %%cn_int24 %1, ".int24"
        EMIT    %%cn_int24, "AX,BX,CX"           ; count / AH:AL / DI low byte
        call    devrow
        %strcat %%cn_dev %1, ".dev"
        EMIT    %%cn_dev, "AX,BX"                ; char-device bit / its name
        mov     ax, [e_ax]
        mov     [__ax], ax
        mov     ax, [e_bx]
        mov     [__bx], ax
        mov     ax, [e_cx]
        and     ax, 0FF00h                       ; CH = locus; CL is not written
        mov     [__cx], ax
        %strcat %%cn_59 %1, ".59"
        EMIT    %%cn_59, "AX,BX,CX"
%endmacro

; .dev: AX = attribute AND 8000h (a character device?), BX = its first two name
; bytes when it is one (a block header's name field is a unit count, not a name).
; No handler call -> both keep the poison.
devrow:
        mov     ax, [r24att]
        mov     bx, [r24nm]
        cmp     word [n24], 0
        je      .out
        and     ax, 8000h
        jnz     .out
        xor     bx, bx
.out:   mov     [__ax], ax
        mov     [__bx], bx
        ret

do3f:   mov     ax, 4200h                        ; position 0: no disk access
        mov     bx, [hread]
        xor     cx, cx
        xor     dx, dx
        int     21h
        POISON
        mov     ah, 3Fh
        mov     bx, [hread]
        mov     cx, 200h
        mov     dx, buf
        int     21h
        ret

do40:   mov     ax, 4200h
        mov     bx, [hwrite]
        xor     cx, cx
        xor     dx, dx
        int     21h
        POISON
        mov     ah, 40h
        mov     bx, [hwrite]
        mov     cx, 200h
        mov     dx, buf
        int     21h
        ret

doprn:  POISON
        mov     ah, 40h
        mov     bx, 4                            ; PRN, unredirected
        mov     cx, 1
        mov     dx, prnch
        int     21h
        ret

start:
        PROBE_BEGIN "crit2"

        ; hook INT 13h and INT 17h (both inert unless armed) and INT 24h
        mov     ax, 3513h
        int     21h
        mov     [old13], bx
        mov     [old13 + 2], es
        mov     ax, 3517h
        int     21h
        mov     [old17], bx
        mov     [old17 + 2], es
        push    ds
        pop     es
        mov     ax, 2513h
        mov     dx, hook13
        int     21h
        mov     ax, 2517h
        mov     dx, hook17
        int     21h
        mov     ax, 2524h
        mov     dx, hook24
        int     21h
        mov     byte [act1], 3                   ; FAIL anything the setup meets
        mov     byte [act2], 3

        ; ---- setup, drive working: open ourselves; create + fill the write target
        POISON
        mov     ax, 3D00h
        mov     dx, pself
        int     21h
        call    probe_capture
        mov     [hread], ax
        EMIT    "crit2.setup.open", "CF"
        test    byte [__fl], 1
        jnz     .noread
        POISON
        mov     ah, 3Ch
        xor     cx, cx
        mov     dx, ptmp
        int     21h
        call    probe_capture
        mov     [hwrite], ax
        EMIT    "crit2.setup.create", "CF"
        test    byte [__fl], 1
        jnz     .nowrite
        mov     ah, 40h                          ; one whole sector: the cluster exists
        mov     bx, [hwrite]
        mov     cx, 200h
        mov     dx, buf
        int     21h
        call    probe_capture
        EMIT    "crit2.setup.write", "AX,CF"
        mov     ah, 68h                          ; commit: directory + FAT on the disk
        mov     bx, [hwrite]
        int     21h

        ; ---- 3Fh on the open file: FAIL; RETRY once then FAIL; IGNORE
        mov     word [callp], do3f
        PROVOKE "crit2.h3f.fail", 3, 3
        mov     word [callp], do3f
        PROVOKE "crit2.h3f.retry", 1, 3
        mov     word [callp], do3f
        PROVOKE "crit2.h3f.ignore", 0, 3
        ; ---- 40h on the open file: FAIL; IGNORE
        mov     word [callp], do40
        PROVOKE "crit2.h40.fail", 3, 3
        mov     word [callp], do40
        PROVOKE "crit2.h40.ignore", 0, 3

        mov     ah, 3Eh
        mov     bx, [hwrite]
        int     21h
        mov     ah, 41h
        mov     dx, ptmp
        int     21h
.nowrite:
        mov     ah, 3Eh
        mov     bx, [hread]
        int     21h
.noread:

        ; ---- PRN with the printer timing out: FAIL; IGNORE
        mov     byte [st17], 01h
        mov     word [callp], doprn
        PROVOKE "crit2.prn.fail", 3, 3
        mov     ax, [n17]
        mov     [__ax], ax
        EMIT    "crit2.prn.fail.n17", "AX"       ; how many INT 17h calls DOS made
        mov     word [callp], doprn
        PROVOKE "crit2.prn.ignore", 0, 3

        ; ---- put INT 13h and INT 17h back
        push    ds
        lds     dx, [old13]
        mov     ax, 2513h
        int     21h
        pop     ds
        push    ds
        lds     dx, [old17]
        mov     ax, 2517h
        int     21h
        pop     ds

        PROBE_END

pself   db      'P_CRIT2.COM', 0
ptmp    db      'ZZCRIT2W.TMP', 0
prnch   db      '*'
old13   dw      0, 0
old17   dw      0, 0
callp   dw      0
hread   dw      0
hwrite  dw      0
armed   db      0
act1    db      0
act2    db      0
st17    db      0
n24     dw      0
n13     dw      0
n17     dw      0
r24ax   dw      0
r24di   dw      0
r24att  dw      0
r24nm   dw      0
c_ax    dw      0
c_fl    dw      0
e_ax    dw      0
e_bx    dw      0
e_cx    dw      0
; the transfer buffer, INITIALISED so it is part of the file -- and the file padded
; past two sectors, so the 512-byte read at offset 0 is a whole sector of a file
; that has more after it (never a short, buffered tail read).
buf     times 512 db 0E5h
        times (1280 - ($ - $$)) * ((1280 - ($ - $$)) > 0) db 0

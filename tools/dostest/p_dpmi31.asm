; p_dpmi31.com -- INT 31h from a real 16-bit DPMI client, function by function (GH #248).
;
; WHY. docs/inventory/dpmi.md: "no probe in tools/dostest/ asks INT 31h function by
; function" -- the DPMI host's only verification was the guest shelf, which never walks
; the error paths. #248 is exactly those paths: 0503h/0304h unsupported, four callback
; slots, 0400h's CL disagreeing with 1687h's, and bad selectors answered as success.
;
; ⚠ WHO CAN ANSWER. MS-DOS 6.22, DOSBox-X and PCem have NO DPMI host (p_dpmins.com: AX
;   comes back 1687h on all three), so on them this probe prints one `nodpmi` row and
;   stops -- that row is the whole oracle answer, and it is a real one. Every other row
;   exists only where a host does, i.e. on NTVDMEX, and is graded against the DPMI
;   specification, not a vote: the expected value is written next to each EMIT.
;
; HOW IT REPORTS FROM PROTECTED MODE. A 16-bit client's initial selectors have the
;   real-mode bases (spec), so probe.inc's routines run unchanged in PM -- except
;   probe_capture, which reloads DS from CS: in PM CS is a CODE selector and writing
;   through it faults. pm_capture is the same routine reloading DS from SS (writable
;   data, same base for a .COM). Output is INT 21h AH=02 from PM, which the host prints
;   into the same DOS-output buffer dosdiff reads.
;
; Each derived row puts its verdict in AX: 1 = what the spec asks, 0 = not.
;
; nasm -f bin -I tools/dostest/ p_dpmi31.asm -o p_dpmi31.com

        org     100h
        jmp     start
%include "probe.inc"

; probe_capture for protected mode: DS <- SS, not CS (see the header).
pm_capture:
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

; AX <- 1 if (CF=1 and AX=%1) in the last capture, else 0; emit as %2.
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
; AX <- 1 if CF=0 in the last capture, else 0.
%macro EXPECT_OK 1
        call    pm_capture
        xor     bx, bx
        test    word [__fl], 1
        jnz     %%n
        inc     bx
%%n:    mov     [__ax], bx
        EMIT    %1, "AX"
%endmacro

start:
        PROBE_BEGIN "dpmi31"

        mov     ah, 4Ah                         ; shrink to 64 KB: room for the private data
        mov     bx, 1000h
        int     21h

        POISON
        mov     si, 0DEADh
        mov     di, 0BEEFh
        mov     ax, 0E5E5h
        mov     es, ax
        mov     ax, 1687h
        int     2Fh
        or      ax, ax
        jz      have_dpmi
        call    probe_capture
        EMIT    "nodpmi", "AX"                  ; oracles: AX = 1687h, untouched
        PROBE_END

have_dpmi:
        mov     [entry], di
        mov     [entry+2], es
        mov     [cl1687], cl
        or      si, si                          ; private data paragraphs
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
        mov     [pspsel], es                    ; ES = the PSP selector (spec)

        ; ── 0400h: version. Expect AX=005A, CL = the CPU class 1687h reported.
        POISON
        mov     ax, 0400h
        int     31h
        call    pm_capture
        EMIT    "int31.0400", "AX,BX,CX,DX,CF"
        mov     al, [__cx]
        xor     bx, bx
        cmp     al, [cl1687]
        jne     .cl
        inc     bx
.cl:    mov     [__ax], bx
        EMIT    "int31.0400.cl_eq_1687", "AX"   ; expect 1

        ; ── 0001h on the null selector: 8022h.
        xor     bx, bx
        mov     ax, 0001h
        int     31h
        EXPECT_ERR 8022h, "int31.0001.null"     ; expect 1

        ; ── allocate one, free it (OK), free it again (8022h).
        mov     cx, 1
        mov     ax, 0000h
        int     31h
        mov     [sel1], ax
        EXPECT_OK "int31.0000.alloc"            ; expect 1
        mov     bx, [sel1]
        mov     ax, 0001h
        int     31h
        EXPECT_OK "int31.0001.free"             ; expect 1
        mov     bx, [sel1]
        mov     ax, 0001h
        int     31h
        EXPECT_ERR 8022h, "int31.0001.double_free"  ; expect 1

        ; ── 0007h-000Ah on that FREED selector: all 8022h.
        mov     bx, [sel1]
        xor     cx, cx
        mov     dx, 1000h
        mov     ax, 0007h
        int     31h
        EXPECT_ERR 8022h, "int31.0007.freed"    ; expect 1
        mov     bx, [sel1]
        xor     cx, cx
        mov     dx, 0FFFh
        mov     ax, 0008h
        int     31h
        EXPECT_ERR 8022h, "int31.0008.freed"    ; expect 1
        mov     bx, [sel1]
        mov     cx, 00F2h
        mov     ax, 0009h
        int     31h
        EXPECT_ERR 8022h, "int31.0009.freed"    ; expect 1
        mov     bx, [sel1]
        mov     ax, 000Ah
        int     31h
        EXPECT_ERR 8022h, "int31.000a.freed"    ; expect 1
        mov     bx, 0FFFFh                      ; index 1FFFh: past any table we keep
        xor     cx, cx
        xor     dx, dx
        mov     ax, 0007h
        int     31h
        EXPECT_ERR 8022h, "int31.0007.offtable" ; expect 1
        mov     bx, 0028h                       ; a GDT selector
        xor     cx, cx
        xor     dx, dx
        mov     ax, 0007h
        int     31h
        EXPECT_ERR 8022h, "int31.0007.gdt"      ; expect 1
        ; ...and the control: on a live selector they succeed.
        mov     cx, 1
        mov     ax, 0000h
        int     31h
        mov     [sel1], ax
        mov     bx, ax
        xor     cx, cx
        mov     dx, 1000h
        mov     ax, 0007h
        int     31h
        EXPECT_OK "int31.0007.live"             ; expect 1
        mov     bx, [sel1]
        mov     ax, 000Ah
        int     31h
        mov     [sel2], ax
        EXPECT_OK "int31.000a.live"             ; expect 1
        mov     bx, [sel2]
        mov     ax, 0001h
        int     31h
        mov     bx, [sel1]
        mov     ax, 0001h
        int     31h

        ; ── 0101h: free a 0100h block (OK), again (8022h), the PSP selector (8022h).
        mov     bx, 1
        mov     ax, 0100h
        int     31h
        mov     [sel1], dx
        EXPECT_OK "int31.0100.alloc"            ; expect 1
        mov     dx, [sel1]
        mov     ax, 0101h
        int     31h
        EXPECT_OK "int31.0101.free"             ; expect 1
        mov     dx, [sel1]
        mov     ax, 0101h
        int     31h
        EXPECT_ERR 8022h, "int31.0101.double_free"  ; expect 1

        ; ── 0100h/0101h x 2100: the selector must come back each time. With the leak,
        ;    the table (2048) runs dry part-way and 0100h starts failing.
        mov     word [count], 0
        mov     word [fails], 0
        mov     word [iter], 2100
.loop:  mov     bx, 1
        mov     ax, 0100h
        int     31h
        jc      .bad
        mov     ax, 0101h
        int     31h
        jc      .bad
        inc     word [count]
        jmp     .next
.bad:   inc     word [fails]
.next:  dec     word [iter]
        jnz     .loop
        mov     ax, [count]
        mov     dx, [fails]
        clc
        call    pm_capture
        EMIT    "int31.0100_0101.x2100", "AX,DX"   ; expect AX=0834 DX=0000

        ; ── 0303h x 17: sixteen succeed, the 17th is 8015h.
        mov     word [count], 0
        xor     bp, bp                          ; slot index * 4
.cba:   push    ds
        pop     es
        mov     si, cbhandler
        mov     di, rmcs
        mov     ax, 0303h
        int     31h
        jc      .cbf
        mov     [cbtab + bp], dx
        mov     [cbtab + bp + 2], cx
        inc     word [count]
        add     bp, 4
        cmp     bp, 16 * 4
        jb      .cba
.cbf:   mov     ax, [count]
        clc
        call    pm_capture
        EMIT    "int31.0303.x16", "AX"          ; expect AX=0010
        push    ds
        pop     es
        mov     si, cbhandler
        mov     di, rmcs
        mov     ax, 0303h
        int     31h
        EXPECT_ERR 8015h, "int31.0303.17th"     ; expect 1

        ; ── 0304h: free all sixteen (OK), one again (8024h), inside a stub (8024h).
        mov     word [count], 0
        xor     bp, bp
.cbr:   mov     dx, [cbtab + bp]
        mov     cx, [cbtab + bp + 2]
        mov     ax, 0304h
        int     31h
        jc      .cbrn
        inc     word [count]
.cbrn:  add     bp, 4
        cmp     bp, 16 * 4
        jb      .cbr
        mov     ax, [count]
        clc
        call    pm_capture
        EMIT    "int31.0304.x16", "AX"          ; expect AX=0010
        mov     dx, [cbtab]
        mov     cx, [cbtab + 2]
        mov     ax, 0304h
        int     31h
        EXPECT_ERR 8024h, "int31.0304.again"    ; expect 1
        push    ds
        pop     es
        mov     si, cbhandler
        mov     di, rmcs
        mov     ax, 0303h
        int     31h
        mov     [cbtab], dx
        mov     [cbtab + 2], cx
        EXPECT_OK "int31.0303.reuse"            ; expect 1
        mov     dx, [cbtab]
        inc     dx
        mov     cx, [cbtab + 2]
        mov     ax, 0304h
        int     31h
        EXPECT_ERR 8024h, "int31.0304.inside"   ; expect 1
        mov     dx, [cbtab]
        mov     cx, [cbtab + 2]
        mov     ax, 0304h
        int     31h
        EXPECT_OK "int31.0304.reused"           ; expect 1

        ; ── 0503h. A 4 KB block, a pattern through a selector, then resize.
        xor     bx, bx
        mov     cx, 1000h
        mov     ax, 0501h
        int     31h
        mov     [lin], cx
        mov     [lin + 2], bx
        mov     [hnd], di
        mov     [hnd + 2], si
        EXPECT_OK "int31.0501"                  ; expect 1
        mov     cx, 1
        mov     ax, 0000h
        int     31h
        mov     [sel1], ax
        call    point_sel                       ; sel1 -> [lin], limit 1FFFFh
        mov     es, [sel1]
        mov     byte [es:0], 0A5h
        mov     byte [es:7FFh], 5Ah
        mov     byte [es:0FFFh], 0C3h

        mov     si, [hnd + 2]                   ; shrink: in place
        mov     di, [hnd]
        xor     bx, bx
        mov     cx, 800h
        mov     ax, 0503h
        int     31h
        call    pm_capture
        xor     bx, bx
        test    word [__fl], 1
        jnz     .s1
        mov     ax, [__cx]
        cmp     ax, [lin]
        jne     .s1
        mov     ax, [__bx]
        cmp     ax, [lin + 2]
        jne     .s1
        inc     bx
.s1:    mov     [__ax], bx
        EMIT    "int31.0503.shrink_inplace", "AX"   ; expect 1

        mov     ax, [hnd]                       ; keep the pre-grow handle
        mov     [oldh], ax
        mov     ax, [hnd + 2]
        mov     [oldh + 2], ax
        mov     si, [hnd + 2]                   ; grow to 128 KB: moves
        mov     di, [hnd]
        mov     bx, 2
        xor     cx, cx
        mov     ax, 0503h
        int     31h
        mov     [lin], cx
        mov     [lin + 2], bx
        mov     [hnd], di
        mov     [hnd + 2], si
        EXPECT_OK "int31.0503.grow"             ; expect 1
        test    word [__fl], 1                  ; refused: BX:CX is not an address -- do
        jnz     .g2                             ; not point a selector at it and write
        mov     es, [pspsel]                    ; never leave ES on the selector we re-base
        call    point_sel
        mov     es, [sel1]
        xor     bx, bx
        cmp     byte [es:0], 0A5h
        jne     .g1
        cmp     byte [es:7FFh], 5Ah
        jne     .g1
        cmp     byte [es:0FFFh], 0C3h           ; past the shrink size: still copied
        jne     .g1
        mov     byte [es:0FFFFh], 77h           ; the new size is usable
        cmp     byte [es:0FFFFh], 77h
        jne     .g1
        inc     bx
.g1:    mov     ax, bx
        clc
        call    pm_capture
        EMIT    "int31.0503.contents_kept", "AX"    ; expect 1
.g2:    mov     es, [pspsel]

        mov     si, [hnd + 2]                   ; size 0: 8021h
        mov     di, [hnd]
        xor     bx, bx
        xor     cx, cx
        mov     ax, 0503h
        int     31h
        EXPECT_ERR 8021h, "int31.0503.size0"    ; expect 1
        mov     si, 1234h                       ; a handle nobody issued: 8023h
        mov     di, 5678h
        xor     bx, bx
        mov     cx, 1000h
        mov     ax, 0503h
        int     31h
        EXPECT_ERR 8023h, "int31.0503.badhandle"    ; expect 1
        mov     si, 1234h
        mov     di, 5678h
        mov     ax, 0502h
        int     31h
        EXPECT_ERR 8023h, "int31.0502.badhandle"    ; expect 1
        mov     si, [hnd + 2]
        mov     di, [hnd]
        mov     ax, 0502h
        int     31h
        EXPECT_OK "int31.0502.free"             ; expect 1
        mov     si, [hnd + 2]
        mov     di, [hnd]
        mov     ax, 0502h
        int     31h
        EXPECT_ERR 8023h, "int31.0502.double_free"  ; expect 1
        mov     bx, [sel1]
        mov     ax, 0001h
        int     31h

        ; ── LAST, because a host that gets it wrong frees THIS PROGRAM'S memory block
        ;    (the pre-#248 arm called DOS free on any selector's segment): 0101h on the
        ;    PSP selector is not a 0100h block -> 8022h.
        mov     dx, [pspsel]
        mov     ax, 0101h
        int     31h
        EXPECT_ERR 8022h, "int31.0101.pspsel"   ; expect 1

        PROBE_END

; sel1 <- base [lin], limit 1FFFFh (0007h, 0008h). Clobbers AX BX CX DX.
point_sel:
        mov     bx, [sel1]
        mov     cx, [lin + 2]
        mov     dx, [lin]
        mov     ax, 0007h
        int     31h
        mov     bx, [sel1]
        mov     cx, 1
        mov     dx, 0FFFFh
        mov     ax, 0008h
        int     31h
        ret

; Never called (no real-mode code here far-calls a callback); 0303h needs an address.
cbhandler:
        iret

entry   dw 0, 0
cl1687  db 0
pspsel  dw 0
sel1    dw 0
sel2    dw 0
count   dw 0
fails   dw 0
iter    dw 0
lin     dw 0, 0
hnd     dw 0, 0
oldh    dw 0, 0
cbtab   times 16 dw 0, 0
rmcs    times 32h db 0

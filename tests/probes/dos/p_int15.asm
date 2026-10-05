; p_int15.com -- INT 15h system services beyond AH=88h (GH #54).
;
; ⚠ THE ORACLES ARE NOT TRUTH HERE (see the issue). These are BIOS conventions and
;   several answers DESCRIBE THE MACHINE -- the C0h table's model bytes, whether an
;   EBDA exists. Those rows are recorded so our answer can be read beside a real
;   AMI 486 (PCem) and SeaBIOS/QEMU, not so they must agree. What IS a contract:
;   - C0h: CF=0, AH=0, and a table at ES:BX whose size word is at least 8.
;   - 87h: a block copied out to extended memory and back comes back identical.
;
; ⚠ WHERE 87h WRITES. Extended memory on a DOS box is somebody's: HIMEM's HMA,
;   SMARTDRV's cache, a RAM disk. So the probe never picks an address itself -- it
;   asks XMS for a 1 KB block, LOCKS it for its physical address, round-trips through
;   that, and frees it. On a host with no XMS driver the 87h rows emit the sentinel
;   DEAD and touch nothing.
;
; nasm -f bin p_int15.asm -o p_int15.com

        org     100h
        jmp     start
%include "probe.inc"

start:
        PROBE_BEGIN "int15"

        ; ---- AH=C0h: get system configuration. ES:BX -> table, CF=0, AH=0.
        POISON
        push    es
        mov     ax, 0C000h
        int     15h
        call    probe_capture
        pop     es
        mov     ax, [__ax]
        and     ax, 0FF00h                      ; AH only; AL is undefined
        mov     [__ax], ax
        EMIT    "int15.c0.status", "AX,CF"

        ; ...and the table itself, if there is one: size word, model, submodel,
        ; revision, feature bytes 1-5. Machine-specific -- read, not compared.
        mov     di, tbl
        mov     cx, 10
        mov     al, 0EEh
        rep     stosb                           ; EE = "never filled"
        test    word [__fl], 1
        jnz     .notbl
        push    ds
        mov     si, [__bx]
        mov     ax, [__es]
        mov     ds, ax
        mov     di, tbl
        mov     cx, 10
        rep     movsb
        pop     ds
.notbl:
        EMIT_BUF "int15.c0.table", tbl, 10

        ; contract: the size word covers at least model..feature1 (8 bytes)
        mov     ax, 0DEADh
        test    word [__fl], 1
        jnz     .szd
        xor     ax, ax
        cmp     word [tbl], 8
        jb      .szd
        mov     ax, 1
.szd:   mov     [__ax], ax
        mov     word [__fl], 0
        EMIT    "int15.c0.size_ok", "AX"

        ; ---- AH=C1h: EBDA segment in ES, CF=0 -- or CF=1 on a machine without one.
        ; Whether one exists is a property of the machine: recorded, CF only.
        POISON
        push    es
        mov     ax, 0C100h
        int     15h
        call    probe_capture
        pop     es
        EMIT    "int15.c1.status", "CF"

        ; ---- AH=86h: wait CX:DX microseconds (here 1000). CF=0 = supported.
        mov     ax, 8600h
        xor     cx, cx
        mov     dx, 1000
        int     15h
        call    probe_capture
        EMIT    "int15.86.status", "CF"

        ; ---- AH=87h: move extended memory block, round trip through an XMS block.
        mov     word [okout], 0DEADh
        mov     word [okback], 0DEADh
        mov     word [match], 0DEADh
        mov     ax, 4300h
        int     2Fh
        cmp     al, 80h
        jne     .noxms
        mov     ax, 4310h
        int     2Fh
        mov     [xent], bx
        mov     [xent + 2], es
        push    cs
        pop     es
        mov     ah, 09h                         ; allocate 1 KB
        mov     dx, 1
        call    far [xent]
        cmp     ax, 1
        jne     .noxms
        mov     [xh], dx
        mov     ah, 0Ch                         ; lock -> DX:BX physical address
        mov     dx, [xh]
        call    far [xent]
        cmp     ax, 1
        jne     .free
        mov     [xaddr], bx
        mov     [xaddr + 2], dx

        mov     cx, 256                         ; the pattern: 00 01 .. FF, 256 bytes
        mov     di, pat
        xor     al, al
.fill:  stosb
        inc     al
        loop    .fill

        ; out: pat (conventional) -> the locked block
        mov     ax, cs
        call    seglin                          ; DX:AX = linear of CS:0
        add     ax, pat
        adc     dx, 0
        mov     si, gdt + 10h
        call    setdesc
        mov     ax, [xaddr]
        mov     dx, [xaddr + 2]
        mov     si, gdt + 18h
        call    setdesc
        call    move128
        mov     [okout], ax

        ; clear the local copy so a no-op move cannot pass
        mov     di, back
        mov     cx, 256
        xor     al, al
        rep     stosb

        ; back: the locked block -> back
        mov     ax, [xaddr]
        mov     dx, [xaddr + 2]
        mov     si, gdt + 10h
        call    setdesc
        mov     ax, cs
        call    seglin
        add     ax, back
        adc     dx, 0
        mov     si, gdt + 18h
        call    setdesc
        call    move128
        mov     [okback], ax

        mov     si, pat
        mov     di, back
        mov     cx, 256
        repe    cmpsb
        mov     ax, 1
        je      .same
        xor     ax, ax
.same:  mov     [match], ax

        mov     ah, 0Dh                         ; unlock
        mov     dx, [xh]
        call    far [xent]
.free:  mov     ah, 0Ah                         ; free
        mov     dx, [xh]
        call    far [xent]
.noxms:
        mov     ax, [okout]
        mov     [__ax], ax
        mov     word [__fl], 0
        EMIT    "int15.87.out.status", "AX"
        mov     ax, [okback]
        mov     [__ax], ax
        EMIT    "int15.87.back.status", "AX"
        mov     ax, [match]
        mov     [__ax], ax
        EMIT    "int15.87.roundtrip", "AX"

        PROBE_END

; ---- helpers --------------------------------------------------------------

; seglin: AX = segment -> DX:AX = segment * 16
seglin:
        mov     dx, ax
        shl     ax, 4
        shr     dx, 12
        ret

; setdesc: DX:AX = 32-bit base, SI -> 8-byte descriptor. Limit FFFF, access 93h.
setdesc:
        mov     word [si], 0FFFFh
        mov     [si + 2], ax
        mov     [si + 4], dl
        mov     byte [si + 5], 93h
        mov     byte [si + 6], 0
        mov     [si + 7], dh
        ret

; move128: INT 15h AH=87h, 128 words, ES:SI -> gdt. Returns AX = AH<<8 | CF.
move128:
        push    cs
        pop     es
        mov     si, gdt
        mov     cx, 128
        mov     ah, 87h
        int     15h
        mov     al, 0
        adc     al, 0                           ; CF -> AL
        ret

; ---- data -----------------------------------------------------------------
tbl     times 10 db 0
xent    dd 0
xh      dw 0
xaddr   dd 0
okout   dw 0
okback  dw 0
match   dw 0
gdt     times 48 db 0
pat     times 256 db 0
back    times 256 db 0

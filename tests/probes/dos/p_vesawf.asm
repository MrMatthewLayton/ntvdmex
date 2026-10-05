; p_vesawf.com -- VBE WinFuncPtr, the real-mode window function.  GH #273.
;
; ModeInfoBlock +0Ch is a FAR pointer a real-mode program may call instead of
; INT 10h AX=4F05h, with the same registers (BH=00h set / 01h get, BL=window,
; DX=position). NULL means "use 4F05h". This sets 640x480x256 (101h), reads the
; pointer, and -- only if it is not NULL -- far-calls it to SET bank 3, then asks
; 4F05h for the bank (the two paths must agree), far-calls it again to GET, and
; checks that the far call kept the registers it should keep.
;
; What travels between hosts: whether the pointer is set (not its address -- that
; is the card's), the bank each path reads back, and the preserved registers.
;
; ORACLE-ALSO: pcem-vesa   (the Tseng ET4000/W32p ROM fills WinFuncPtr)
; nasm -f bin p_vesawf.asm -o p_vesawf.com

        org     100h
        jmp     start
%include "probe.inc"

start:
        PROBE_BEGIN "vesawf"

        mov     ax, 4F02h               ; 640x480x256, banked
        mov     bx, 0101h
        int     10h
        call    probe_capture
        EMIT    "int10.4F02.101", "AX"

        push    ds
        pop     es
        mov     di, mib
        mov     cx, 0101h
        mov     ax, 4F01h
        int     10h
        call    probe_capture
        EMIT    "int10.4F01.101", "AX"

        ; AX = 1 when WinFuncPtr is set, 0 when NULL
        mov     ax, [mib + 0Ch]
        or      ax, [mib + 0Eh]
        jz      .null
        mov     ax, 1
.null:  mov     [have], ax
        call    probe_capture
        EMIT    "vbe.winfuncptr.set", "AX"
        cmp     word [have], 0
        je      .done

        ; ---- far-call SET bank 3. BX/CX/SI/DI/BP poisoned to see what survives.
        mov     ax, [mib + 0Ch]
        mov     [fptr], ax
        mov     ax, [mib + 0Eh]
        mov     [fptr + 2], ax
        mov     bx, 0000h               ; set, window A
        mov     dx, 0003h
        mov     cx, 0C1C1h
        mov     si, 5151h
        mov     di, 0D1D1h
        call    far [fptr]
        call    probe_capture
        EMIT    "vbe.winfunc.set3", "BX,CX,SI,DI"

        ; ---- does INT 10h agree? 4F05h BH=01h get -> DX
        mov     ax, 4F05h
        mov     bx, 0100h
        mov     dx, 0D1D1h
        int     10h
        call    probe_capture
        EMIT    "int10.4F05.get.after", "AX,DX"

        ; ---- 4F05h set bank 5, then the far call's GET must say 5
        mov     ax, 4F05h
        mov     bx, 0000h
        mov     dx, 0005h
        int     10h
        mov     bx, 0100h               ; get, window A
        mov     dx, 0D1D1h
        call    far [fptr]
        call    probe_capture
        EMIT    "vbe.winfunc.get", "DX"

.done:
        mov     ax, 0003h               ; back to text
        int     10h
        PROBE_END

have    dw      0
fptr    dd      0
mib     times 256 db 0

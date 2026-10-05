; dos_auxprn.asm -- DOS's AUX and PRN device drivers, as guest code. GH #251.
;
; The source of the bytes in dos_auxprn.h. Rebuild with:
;     nasm -f bin src/dos/dos_auxprn.asm -o /tmp/a.bin -l /tmp/a.lst
; and copy the bytes and entry offsets across (the off-VM test pins both).
;
; ★ WHY GUEST CODE. On MS-DOS, AUX and PRN are drivers in IO.SYS that call the
;   BIOS -- INT 14h and INT 17h -- through the interrupt vector table. That is the
;   contract a printer redirector or a serial TSR relies on: it hooks INT 17h /
;   INT 14h and everything DOS prints passes through it. Our INT 21h is host C, so
;   the host resumes the guest HERE instead of at the stub's IRET, and this code
;   makes the BIOS calls for real, with the INT 21h caller's IRET frame still on
;   the stack. Each entry ends in IRET back to the caller.
; ★ THE CALL SEQUENCES ARE MEASURED, not recalled: tests/probes/dos/p_auxprn hooks
;   INT 14h/17h and logs every call MS-DOS 6.22 makes (QEMU and PCem agree):
;     05h:      17h/02h, 17h/02h, 17h/00h AL=char           -> AX = 05:char
;     04h:      14h/03h, 14h/01h AL=char                    -> AX = 04:char
;     03h:      14h/03h, 14h/02h (AL left from the status)  -> AX = 03:byte
;     40h h=4:  per byte 17h/02h then 17h/00h               -> AX = CX, CF=0
;     40h h=3:  per byte 14h/01h                            -> AX = CX, CF=0
;     3Fh h=3:  per byte 14h/02h (AX=0200h), stops AFTER a CR or at CX -> AX = n
;   DX = 0 (LPT1 / COM1) on every BIOS call. BX, CX, DX, SI come back unchanged.
; ⚠ NOT MODELLED: what DOS does with a BIOS error status (busy / time-out ->
;   retry, then INT 24h). Our BIOS answers ready; a hook that answers otherwise
;   is ignored here. (#275, still open.) What is known: MS-DOS 4.0's MSLPT.ASM
;   (Microsoft's published source) triages INT 17h's AH as I/O error (08h) ->
;   9 out of paper (if 20h) else 0Ah write fault; else time-out (01h) -> 2 not
;   ready; it retries only a time-out, twice, and lets every other error pass.
;   The kernel then calls INT 24h with AH = 87h|38h = BFh (character device,
;   write, F+R+I allowed) and BP:SI -> the PRN header (DISK.ASM, CHARHARD).
;   ⛔ 6.22 is NOT 4.0 here -- its call sequence (17h/02h before 17h/00h, above)
;   already differs -- so measure first: tests/probes/dos/p_crit2.asm crit2.prn.*
;   (INT 17h hooked to answer time-out; reports n17, AH:AL, DI, BP:SI's header).
        bits 16
        org 0

t05:    sti
        push dx
        push ax
        xor dx, dx
        mov ax, 0200h
        int 17h
        mov ax, 0200h
        int 17h
        pop ax
        pop dx
        push dx
        mov al, dl
        push ax
        mov ah, 00h
        xor dx, dx
        int 17h
        pop ax
        pop dx
        iret

t04:    sti
        push dx
        push ax
        xor dx, dx
        mov ax, 0300h
        int 14h
        pop ax
        pop dx
        push dx
        mov al, dl
        push ax
        mov ah, 01h
        xor dx, dx
        int 14h
        pop ax
        pop dx
        iret

t03:    sti
        push dx
        xor dx, dx
        mov ax, 0300h
        int 14h
        mov ah, 02h
        int 14h
        mov ah, 03h
        pop dx
        iret

tw4:    sti
        push si
        push cx
        push dx
        mov si, dx
        jcxz .done
.next:  xor dx, dx
        mov ax, 0200h
        int 17h
        mov al, [si]
        inc si
        mov ah, 00h
        xor dx, dx
        int 17h
        loop .next
.done:  pop dx
        pop cx
        pop si
        mov ax, cx
        jmp short okret

tw3:    sti
        push si
        push cx
        push dx
        mov si, dx
        jcxz .done
.next:  mov al, [si]
        inc si
        mov ah, 01h
        xor dx, dx
        int 14h
        loop .next
.done:  pop dx
        pop cx
        pop si
        mov ax, cx
        jmp short okret

rd3:    sti
        push si
        push bx
        push dx
        mov si, dx
        xor bx, bx
        jcxz .done
.next:  mov ax, 0200h
        xor dx, dx
        int 14h
        mov [si], al
        inc si
        inc bx
        cmp al, 0Dh
        je .done
        cmp bx, cx
        jb .next
.done:  mov ax, bx
        pop dx
        pop bx
        pop si
        ; fall through

okret:  push bp                         ; IRET with the caller's CF cleared
        mov bp, sp
        and byte [bp+6], 0FEh           ; [bp+2]=IP [bp+4]=CS [bp+6]=FLAGS
        pop bp
        iret

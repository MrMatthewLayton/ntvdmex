; p_disk.com -- INT 13h disk services and INT 25h absolute read.  GH #44.
;
; NTVDMEX exposes no raw sectors at all: INT 13h answers AH=01 "bad command" for
; anything past reset/status, and INT 25h/26h answer AL=07 "drive parameter
; error". That is honest, and it is also a wall in front of installers, disk
; utilities, and anything that reads a boot sector.
;
; This measures what a real machine says, so the implementation has a target
; rather than a plausible reading. The oracle's drive 0 is a genuine 1.44MB FAT12
; floppy -- the scratch disk the harness delivers this program on -- so every
; answer below is about real geometry and a real boot sector.
;
; ⚠ INT 25h IS THE ODD ONE AND IT IS EASY TO GET WRONG: it returns with the
;   ORIGINAL FLAGS STILL PUSHED ON THE STACK. A caller that forgets the POPF
;   corrupts its own stack, and the corruption shows up later somewhere else
;   entirely. The POPF below is not tidiness, it is the contract.
;
; nasm -f bin p_disk.asm -o p_disk.com

        org     100h
        jmp     start
%include "probe.inc"

start:
        PROBE_BEGIN "disk"

        ; ---- AH=00h: reset the disk system.
        POISON
        mov     ax, 0000h
        mov     dl, 0                           ; drive 0 = A:
        int     13h
        call    probe_capture
        EMIT    "int13.00.reset", "AX,CF"

        ; ---- AH=08h: get drive parameters. The geometry answer:
        ;   CH = max cylinder (low 8), CL bits 7-6 = cylinder high, 5-0 = sectors
        ;   DH = max head, DL = number of drives, BL = drive type
        POISON
        mov     ax, 0800h
        mov     dl, 0
        int     13h
        call    probe_capture
        EMIT    "int13.08.params", "AX,BX,CX,DX,CF"

        ; ---- AH=15h: get disk type. AH=01 no change-line, 02 change-line, 03 fixed.
        POISON
        mov     ax, 1500h
        mov     dl, 0
        int     13h
        call    probe_capture
        EMIT    "int13.15.type", "AX,CX,DX,CF"

        ; ---- AH=02h: read ONE sector -- cylinder 0, head 0, SECTOR 1, which is
        ; the boot sector. Sector numbers are 1-based and that is the classic
        ; off-by-one in this interface.
        POISON
        mov     ax, 0201h                       ; AH=02 read, AL=1 sector
        mov     cx, 0001h                       ; CH=cyl 0, CL=sector 1
        mov     dx, 0000h                       ; DH=head 0, DL=drive 0
        push    ds
        pop     es
        mov     bx, buf
        int     13h
        call    probe_capture
        EMIT    "int13.02.read.boot", "AX,CF"
        ; The FAT12 boot sector's OEM name lives at offset 3, and the signature
        ; 55AA at 510. Both are checked because a read that returns success with
        ; an untouched buffer is the failure mode worth catching.
        EMIT_BUF "disk.boot.oem", buf + 3, 8
        EMIT_BUF "disk.boot.sig", buf + 510, 2

        ; ---- INT 25h: DOS absolute disk read. AL=drive (0=A:), CX=sectors,
        ; DX=first sector, DS:BX=buffer. Returns CF and leaves FLAGS pushed.
        mov     word [buf], 0                   ; poison, so "unchanged" is visible
        mov     al, 0                           ; drive 0 = A:
        mov     cx, 1
        mov     dx, 0
        mov     bx, buf
        int     25h
        pushf                                   ; capture OUR flags before POPF
        pop     word [f25]
        add     sp, 2                           ; ★ discard INT 25h's pushed FLAGS
        call    probe_capture
        mov     ax, [f25]
        mov     [__fl], ax
        EMIT    "int25.absread", "AX,CF"
        EMIT_BUF "disk25.boot.oem", buf + 3, 8

        PROBE_END

f25      dw 0
buf      times 512 db 0

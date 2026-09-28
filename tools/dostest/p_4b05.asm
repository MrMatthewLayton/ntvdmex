; p_4b05.com -- INT 21h AX=4B05h (set execution state) and AX=6901h (set volume
; serial). #165. Both were refused on NTVDMEX; this measures what DOS does first.
;
; 4B05h is the second half of a loader's own EXEC: AX=4B01h loads a program without
; running it, the loader does its own work, then 4B05h tells DOS "control is about to
; go to this program" before it jumps. So the questions are about STATE, not values:
;   4b01.psp_switched   after 4B01, is the CURRENT PSP (AH=62h) the child's?
;   4b01.cs_is_child    is the returned entry CS the current PSP (a .COM)?
;   4b05                does 4B05h succeed (CF), given the child's state block?
;   4b05.psp_is_child   with the current PSP put back to ours first, is it the
;                       child's after 4B05h?
; The child is never run: the current PSP is put back with AH=50h and the child's
; blocks are freed, so nothing here changes what the next EXEC sees.
;
; 6901h writes the serial into the disk's boot record on real DOS, so this reads the
; original first, sets a marker, reads it back, and PUTS THE ORIGINAL BACK.
;   6900.get / 6901.set / 6901.readback (1 = the marker came back) / 6901.restore
;
; nasm -f bin p_4b05.asm -o p_4b05.com
        org     100h
        jmp     start
%include "probe.inc"

start:
        PROBE_BEGIN "exec4b05"

        mov     ax, 4A00h               ; a .COM owns all memory: shrink first
        mov     bx, 1000h
        push    ds
        pop     es
        int     21h

        mov     ax, 3C00h               ; write the child: mov ax,4C00h / int 21h
        xor     cx, cx
        mov     dx, cname
        int     21h
        mov     bx, ax
        mov     ax, 4000h
        mov     cx, 5
        mov     dx, cimg
        int     21h
        mov     ax, 3E00h
        int     21h

        mov     ah, 62h
        int     21h
        mov     [p0], bx

        ; ---- 4B01h: load, do not run
        mov     word [pb], 0            ; env: a copy of ours
        mov     word [pb+2], ctail
        mov     [pb+4], ds
        mov     word [pb+6], cfcb
        mov     [pb+8], ds
        mov     word [pb+10], cfcb
        mov     [pb+12], ds
        push    ds
        pop     es
        mov     bx, pb
        mov     dx, cname
        mov     ax, 4B01h
        int     21h
        call    probe_capture
        EMIT    "int21.4B01.load", "CF"

        mov     ah, 62h
        int     21h
        mov     [p1], bx
        xor     ax, ax
        cmp     bx, [p0]
        je      .n1
        inc     ax
.n1:    call    probe_capture
        EMIT    "int21.4B01.psp_switched", "AX"

        mov     ax, [pb+14h]            ; entry CS
        mov     [child], ax
        xor     ax, ax
        mov     bx, [child]
        cmp     bx, [p1]
        jne     .n2
        inc     ax
.n2:    call    probe_capture
        EMIT    "int21.4B01.cs_is_curpsp", "AX"

        ; ---- 4B05h, with OUR PSP current again, so any switch is 4B05h's own
        mov     ah, 50h
        mov     bx, [p0]
        int     21h
        mov     ax, [child]
        mov     [st+8], ax              ; +08 new PSP
        mov     ax, [pb+12h]
        mov     [st+0Ah], ax            ; +0A entry IP
        mov     ax, [pb+14h]
        mov     [st+0Ch], ax            ; +0C entry CS
        mov     word [st+4], cname      ; +04 far pointer to the name
        mov     [st+6], ds
        mov     bx, 0B1B1h
        mov     cx, 0C1C1h
        mov     dx, st
        mov     ax, 4B05h
        int     21h
        call    probe_capture
        EMIT    "int21.4B05.set", "CF"

        mov     ah, 62h
        int     21h
        xor     ax, ax
        cmp     bx, [child]
        jne     .n3
        inc     ax
.n3:    call    probe_capture
        EMIT    "int21.4B05.psp_is_child", "AX"

        ; ---- put everything back: our PSP, the child's two blocks, the file
        mov     ah, 50h
        mov     bx, [p0]
        int     21h
        mov     es, [child]
        mov     ax, [es:2Ch]            ; the child's environment copy
        push    ax
        mov     ah, 49h
        int     21h                     ; ES = child PSP
        pop     ax
        or      ax, ax
        jz      .noenv
        mov     es, ax
        mov     ah, 49h
        int     21h
.noenv: mov     ah, 41h
        mov     dx, cname
        int     21h

        ; ---- 6900h / 6901h on the default drive
        push    ds
        pop     es
        mov     ax, 6900h
        xor     bx, bx
        mov     dx, orig
        int     21h
        call    probe_capture
        EMIT    "int21.6900.get", "CF"

        mov     si, orig                ; marker copy, new serial
        mov     di, mark
        mov     cx, 25
        rep     movsb
        mov     word [mark+2], 5678h
        mov     word [mark+4], 1234h
        mov     ax, 6901h
        xor     bx, bx
        mov     dx, mark
        int     21h
        call    probe_capture
        EMIT    "int21.6901.set", "CF"

        mov     ax, 6900h
        xor     bx, bx
        mov     dx, back
        int     21h
        xor     ax, ax
        cmp     word [back+2], 5678h
        jne     .n4
        cmp     word [back+4], 1234h
        jne     .n4
        inc     ax
.n4:    call    probe_capture
        EMIT    "int21.6901.readback", "AX"

        mov     ax, 6901h
        xor     bx, bx
        mov     dx, orig
        int     21h
        call    probe_capture
        EMIT    "int21.6901.restore", "CF"

        PROBE_END

cname   db      "C4B05.COM", 0
cimg    db      0B8h, 00h, 4Ch, 0CDh, 21h
ctail   db      0, 0Dh
cfcb    times 16 db 0
p0      dw      0
p1      dw      0
child   dw      0
pb      times 22 db 0
st      times 18 db 0
orig    times 26 db 0
mark    times 26 db 0
back    times 26 db 0

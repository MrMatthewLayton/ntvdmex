; p_tsr.com -- TSR residency: does a program's handler survive its death?  GH #49.
;
; Residency is three claims, and a host can fail any one of them while looking
; fine on the other two:
;
;   1. the terminating program's MEMORY BLOCK stays allocated
;   2. the INTERRUPT VECTORS it installed survive the termination
;   3. control returns to whatever EXEC'd it, with the image intact
;
; So this EXECs a child that hooks INT 60h and TSRs, and then -- back in the
; parent, after the child has terminated -- CALLS INT 60h. If the handler
; answers with its magic value, all three held. If residency is not honoured,
; either the vector points at freed memory (and we execute whatever is there
; now) or it was never installed.
;
; ⚠ THE VECTOR ALONE IS NOT THE TEST. A host could leave the vector installed
;   and still free the block underneath it, which is worse than not doing it at
;   all -- the guest then jumps into memory the allocator has handed to someone
;   else. So the probe also asks DOS for the largest free block BEFORE and AFTER
;   and checks it did NOT grow back: memory that is still resident is memory the
;   allocator must not offer.
;
; It writes its own resident child out of bytes embedded below (incbin of
; p_tsrc.com), like p_ovl does, so nothing has to be staged alongside it.
;
; nasm -f bin p_tsr.asm -o p_tsr.com   (build p_tsrc.com FIRST)

        org     100h
        jmp     start
%include "probe.inc"

start:
        PROBE_BEGIN "tsr"

        ; ---- shrink, so there is a free pool for the child to be loaded into
        ; and for the before/after comparison to mean something.
        mov     ax, 4A00h
        mov     bx, 800h
        push    ds
        pop     es
        int     21h
        call    probe_capture
        EMIT    "int21.4A.shrink", "CF"

        ; ---- write the resident child to disk.
        mov     ax, 3C00h
        mov     cx, 0
        mov     dx, cname
        int     21h
        mov     [fh], ax
        mov     bx, ax
        mov     ax, 4000h
        mov     cx, cend - cimg
        mov     dx, cimg
        int     21h
        call    probe_capture
        EMIT    "int21.40.wrote.child", "AX,CF"
        mov     bx, [fh]
        mov     ax, 3E00h
        int     21h

        ; ---- how much memory is free BEFORE. Asking for 0xFFFF paragraphs
        ; always fails and reports the largest free block in BX, which is the
        ; standard idiom and does not disturb anything.
        mov     ax, 4800h
        mov     bx, 0FFFFh
        int     21h
        call    probe_capture
        mov     ax, [__bx]
        mov     [freebefore], ax
        EMIT    "int21.48.free.before", "BX"

        ; ---- point INT 60h at a known-bad value first, so "the handler
        ; answered" cannot be satisfied by whatever happened to be in the IVT.
        mov     ax, 2560h
        push    ds
        push    cs
        pop     ds
        mov     dx, poison
        int     21h
        pop     ds

        ; ---- EXEC the child. It hooks INT 60h and TSRs.
        mov     word [pb + 0], 0                ; inherit the environment
        mov     word [pb + 2], tailz
        mov     [pb + 4], ds
        mov     word [pb + 6], 5Ch
        mov     [pb + 8], ds
        mov     word [pb + 10], 6Ch
        mov     [pb + 12], ds
        push    ds
        pop     es
        mov     bx, pb
        mov     dx, cname
        mov     ax, 4B00h
        int     21h
        call    probe_capture
        EMIT    "int21.4B00.exec.child", "CF"

        ; ---- ★ THE TEST. Call the vector the dead child installed.
        ; AX = BEEF means its handler is still there and still runs.
        ; AX = DEAD means the vector came back to our poison, i.e. the
        ; termination unwound it.
        mov     ax, 0
        int     60h
        call    probe_capture
        EMIT    "tsr.int60.answers", "AX"

        ; ---- and the memory must NOT have come back. If free space is the same
        ; as before the EXEC, the child's block was released and the vector
        ; above is pointing into the free list -- which is the dangerous way to
        ; pass the previous check.
        mov     ax, 4800h
        mov     bx, 0FFFFh
        int     21h
        call    probe_capture
        mov     ax, [__bx]
        mov     [freeafter], ax
        EMIT    "int21.48.free.after", "BX"

        mov     ax, [freebefore]
        sub     ax, [freeafter]                 ; paragraphs still held
        mov     [__ax], ax
        mov     word [__fl], 0
        EMIT    "tsr.paras.still.held", "AX"

        ; ---- clean up the file (the resident block is deliberately left).
        mov     ax, 4100h
        mov     dx, cname
        int     21h

        PROBE_END

poison:
        mov     ax, 0DEADh
        iret

cname       db 'ZZTSR.COM', 0
tailz       db 0, 0Dh
fh          dw 0
freebefore  dw 0
freeafter   dw 0
pb          times 14 db 0

cimg:
        incbin  "p_tsrc.com"
cend:

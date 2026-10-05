; p_exmem.com -- what EXEC does with an MZ header's e_minalloc / e_maxalloc.  GH #255.
; ORACLE-ALSO: pcem
;
; EXEC used to hand every child ALL of free memory whatever its header said: a program
; linked to leave room (for its own children, a TSR, an overlay) got everything, one
; that needs more than is free was loaded anyway instead of refused with error 8, and
; "load high" (minalloc = maxalloc = 0) put the image at the bottom.
;
; Each case EXECs a child built from p_exmemc.asm with a different header, and the
; child reports what it was given (see that file). Only RELATIONS are compared --
; the block size for a bounded maxalloc, where the image sits inside the block --
; never an absolute segment or "how much is free", which describe the machine.
;
; nasm -f bin p_exmem.asm -o p_exmem.com     (children: see p_exmemc.asm)

        org     100h
        jmp     start
%include "probe.inc"

; RUNCHILD name, case, sig -- EXEC it, report EXEC's own answer, then the child's words
; ⚠ AX AFTER A SUCCESSFUL EXEC IS NOT AN ANSWER: 6.22 and DOSBox-X both leave 3E01h
;   there (leftovers of the child's own exit path). Only the refusal's AX=0008 is.
%macro RUNCHILD 3
        xor     ax, ax
        mov     es, ax
        mov     word [es:180h], 0EEEEh
        mov     word [es:182h], 0EEEEh
        mov     word [es:184h], 0EEEEh
        mov     word [es:186h], 0EEEEh
        push    ds
        pop     es
        POISON
        mov     bx, pblock
        mov     dx, %1
        mov     ax, 4B00h
        int     21h
        call    probe_capture
        EMIT    %2, %3
        xor     ax, ax
        mov     es, ax
        mov     ax, [es:180h]
        mov     [r_blk], ax
        mov     ax, [es:182h]
        mov     [r_cs], ax
        mov     ax, [es:184h]
        mov     [r_free], ax
        mov     ax, [es:186h]
        mov     [r_ran], ax
        push    ds
        pop     es
%endmacro

start:
        PROBE_BEGIN "exmem"

        ; save INT 60h's slot, which the children write their report into
        xor     ax, ax
        mov     es, ax
        mov     ax, [es:180h]
        mov     [sv60], ax
        mov     ax, [es:182h]
        mov     [sv60 + 2], ax
        mov     ax, [es:184h]
        mov     [sv60 + 4], ax
        mov     ax, [es:186h]
        mov     [sv60 + 6], ax

        ; a .COM owns all of memory: give it back, as every shell does before EXEC
        mov     ax, cs
        mov     es, ax
        mov     bx, 1000h
        mov     ah, 4Ah
        int     21h

        mov     ax, cs
        mov     [pb_tail + 2], ax
        mov     [pb_fcb1 + 2], ax
        mov     [pb_fcb2 + 2], ax

        ; ---- A: minalloc 100h, maxalloc 200h -> a BOUNDED block
        RUNCHILD cha, "exmem.a.exec", "CF"
        mov     ax, [r_ran]
        mov     [__ax], ax
        mov     ax, [r_blk]
        mov     [__bx], ax
        mov     ax, [r_cs]
        mov     [__cx], ax
        EMIT    "exmem.a.child", "AX,BX,CX"     ; ran / block paras / CS-PSP

        ; ---- B: minalloc F000h -- more than any host has free -> error 8, not run
        RUNCHILD chb, "exmem.b.exec", "AX,CF"
        mov     ax, [r_ran]
        mov     [__ax], ax
        EMIT    "exmem.b.child", "AX"           ; EEEE = it never ran

        ; ---- C: minalloc = maxalloc = 0 -> LOAD HIGH: the image at the TOP of the
        ;      block. Relation: memtop - CS (the image's paragraphs from the top).
        RUNCHILD chc, "exmem.c.exec", "CF"
        mov     ax, [r_ran]
        mov     [__ax], ax
        mov     ax, [r_blk]
        sub     ax, [r_cs]
        mov     [__bx], ax
        mov     ax, [r_cs]
        mov     bx, 10h
        cmp     ax, bx
        mov     ax, 0                           ; CX = 1 if the image is NOT right after the PSP
        je      .c_low
        mov     ax, 1
.c_low: mov     [__cx], ax
        EMIT    "exmem.c.child", "AX,BX,CX"

        ; ---- D: maxalloc FFFFh -> all of the largest block; what is left after
        RUNCHILD chd, "exmem.d.exec", "CF"
        mov     ax, [r_ran]
        mov     [__ax], ax
        mov     ax, [r_cs]
        mov     [__cx], ax
        mov     ax, [r_free]                    ; BX = 1 if what is left is < 100h paras
        cmp     ax, 100h
        mov     ax, 0
        jae     .d_big
        mov     ax, 1
.d_big: mov     [__bx], ax
        EMIT    "exmem.d.child", "AX,BX,CX"

        ; ---- E: minalloc 0, maxalloc 10h -> a block just past the image
        RUNCHILD che, "exmem.e.exec", "CF"
        mov     ax, [r_ran]
        mov     [__ax], ax
        mov     ax, [r_blk]
        mov     [__bx], ax
        mov     ax, [r_cs]
        mov     [__cx], ax
        EMIT    "exmem.e.child", "AX,BX,CX"

        ; put INT 60h's slot back
        xor     ax, ax
        mov     es, ax
        mov     ax, [sv60]
        mov     [es:180h], ax
        mov     ax, [sv60 + 2]
        mov     [es:182h], ax
        mov     ax, [sv60 + 4]
        mov     [es:184h], ax
        mov     ax, [sv60 + 6]
        mov     [es:186h], ax
        push    ds
        pop     es

        PROBE_END

cha     db      'XMEMA.COM', 0
chb     db      'XMEMB.COM', 0
chc     db      'XMEMC.COM', 0
chd     db      'XMEMD.COM', 0
che     db      'XMEME.COM', 0
tail    db      0, 0Dh
fcb     times 16 db 0
pblock:
        dw      0                               ; inherit the environment
pb_tail dw      tail, 0
pb_fcb1 dw      fcb, 0
pb_fcb2 dw      fcb, 0
sv60    dw      0, 0, 0, 0
r_blk   dw      0
r_cs    dw      0
r_free  dw      0
r_ran   dw      0

; p_mcb.com -- walk the MCB chain exactly as MEM.EXE does.  GH #47.
;
; MEM reports Conventional 640K total / 640K USED / 0K free on NTVDMEX, and files
; the 549K that IS free under "Upper" instead. Two fixes aimed at how MEM
; CLASSIFIES memory (the UMB link, then the A20 line) were both measured against
; the oracle, both correct, and NEITHER MOVED THE NUMBERS.
;
; So stop theorising about the classifier and read the thing it classifies. This
; walks the chain from the same place MEM starts -- the word at ES:BX-2 returned
; by INT 21h AH=52h -- and dumps every block's signature, owner and size.
;
;   * signature 'M' (4Dh) = another block follows, 'Z' (5Ah) = last
;   * owner 0 = FREE; anything else is the owning PSP
;   * size is in PARAGRAPHS, and the next MCB is at this one + 1 + size
;
; A chain that does not add up to 640K, or that ends early, or whose free block
; never appears, is visible here and nowhere else.
;
; ⚠ It walks at most 16 blocks and stops on anything that is not 'M'/'Z'. An
;   unterminated chain is the failure being hunted, so the walker must not be
;   able to run away with it.
;
; nasm -f bin p_mcb.asm -o p_mcb.com

        org     100h
        jmp     start
%include "probe.inc"

start:
        PROBE_BEGIN "mcb"

        ; ---- shrink first, exactly as MEM's own startup does. Without this a
        ; .COM owns everything and the chain has no free block to find -- which
        ; would make the dump look like the bug even on a host that is fine.
        mov     ax, 4A00h
        mov     bx, 1000h
        push    ds
        pop     es
        int     21h
        call    probe_capture
        EMIT    "int21.4A.shrink", "CF"

        ; ---- AH=52h, then step BACK two bytes for the first MCB segment.
        mov     ax, 5200h
        int     21h
        call    probe_capture
        mov     ax, [__es]
        mov     ds, ax
        mov     bx, [cs:__bx]
        sub     bx, 2
        mov     ax, [bx]                        ; first MCB paragraph
        push    cs
        pop     ds
        mov     [mcb], ax
        mov     [__ax], ax
        mov     word [__fl], 0
        EMIT    "mcb.head", "AX"

        ; ---- walk it.
        mov     byte [n], 0
.loop:
        mov     ax, [mcb]
        mov     ds, ax
        mov     al, [0]                         ; signature
        mov     bx, [1]                         ; owner PSP (0 = free)
        mov     cx, [3]                         ; size in paragraphs
        push    cs
        pop     ds
        mov     ah, 0
        mov     [__ax], ax                      ; AL = 'M' or 'Z'
        mov     [__bx], bx
        mov     [__cx], cx
        mov     ax, [mcb]
        mov     [__dx], ax                      ; where this block lives
        mov     word [__fl], 0
        EMIT    "mcb.block", "AX,BX,CX,DX"

        ; stop on the last block, or on anything that is not an MCB at all
        mov     ax, [__ax]
        cmp     al, 5Ah                         ; 'Z'
        je      .done
        cmp     al, 4Dh                         ; 'M'
        jne     .bad
        ; next = this + 1 + size
        mov     ax, [mcb]
        inc     ax
        add     ax, [__cx]
        mov     [mcb], ax
        inc     byte [n]
        cmp     byte [n], 16
        jb      .loop
        mov     word [__ax], 0FFFFh             ; ran out of patience, not chain
        mov     word [__fl], 0
        EMIT    "mcb.walk.overran", "AX"
        jmp     .end
.bad:
        mov     word [__ax], 0BAADh
        mov     word [__fl], 0
        EMIT    "mcb.walk.badsig", "AX"
        jmp     .end
.done:
        ; ---- the arithmetic that matters: the last block must END at 0xA000.
        ; this MCB + 1 + its size = the paragraph just past conventional memory.
        mov     ax, [mcb]
        inc     ax
        add     ax, [__cx]
        mov     [__ax], ax
        mov     word [__fl], 0
        EMIT    "mcb.chain.ends.at", "AX"
.end:
        PROBE_END

mcb      dw 0
n        db 0

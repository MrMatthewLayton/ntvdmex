; p_tsrc.com -- the resident half of the TSR probe.  GH #49.
;
; Hooks INT 60h (an unused user vector), then terminates-and-stays-resident
; keeping 0x20 paragraphs. If residency works, the handler below survives this
; program's death and answers when the PARENT calls INT 60h.
;
; The handler must live inside the paragraphs we keep: it sits at ~0x110, well
; inside 0x20 paragraphs (512 bytes) measured from the PSP at offset 0.
;
; nasm -f bin p_tsrc.asm -o p_tsrc.com
        org     100h
start:
        mov     ax, 2560h               ; set interrupt vector 60h
        mov     dx, handler             ; DS:DX -- DS is our PSP
        int     21h
        mov     ax, 3100h               ; TSR, exit code 0
        mov     dx, 20h                 ; keep 32 paragraphs
        int     21h
handler:
        mov     ax, 0BEEFh              ; the proof it is still here
        iret

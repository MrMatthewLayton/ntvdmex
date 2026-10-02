; p_exmemc.asm -- the CHILD of p_exmem.asm: a minimal MZ image whose header's
; e_minalloc / e_maxalloc are set at assembly time. GH #255.
;
; It reports what EXEC gave it, through four words at 0000:0180 (INT 60h's vector
; slot, a user vector; the parent saves and restores it):
;   +0  PSP:[0002] - PSP     the size of the block EXEC allocated, in paragraphs
;   +2  CS - PSP             where the image was put inside it (load high moves it)
;   +4  largest free block   INT 21h AH=48h BX=FFFFh, after the allocation
;   +6  1234h                "I ran"
;
; ⚠ THE FILES ARE NAMED .COM AND ARE MZ IMAGES. DOS picks the loader by the 'MZ'
;   signature, never by the extension, and *.exe is gitignored in this repo. That
;   is itself part of the contract under test.
; ⚠ THE STACK IS INSIDE THE IMAGE (the 256 zero bytes at the end), so the program
;   runs with e_minalloc = 0 and when loaded high: no host is asked for memory past
;   the image just to have somewhere to push.
;
;   nasm -f bin -DMINA=0100h -DMAXA=0200h p_exmemc.asm -o xmema.com
;   nasm -f bin -DMINA=0F000h -DMAXA=0FFFFh p_exmemc.asm -o xmemb.com
;   nasm -f bin -DMINA=0     -DMAXA=0     p_exmemc.asm -o xmemc.com
;   nasm -f bin -DMINA=0     -DMAXA=0FFFFh p_exmemc.asm -o xmemd.com
;   nasm -f bin -DMINA=0     -DMAXA=0010h p_exmemc.asm -o xmeme.com

%ifndef MINA
%define MINA 0
%endif
%ifndef MAXA
%define MAXA 0FFFFh
%endif

        org     0
hdr:
        db      'MZ'
        dw      (fend - hdr) % 512              ; e_cblp: bytes in the last page
        dw      (fend - hdr + 511) / 512        ; e_cp:   pages
        dw      0                               ; e_crlc: no relocations
        dw      2                               ; e_cparhdr: 32-byte header
        dw      MINA                            ; e_minalloc
        dw      MAXA                            ; e_maxalloc
        dw      0                               ; e_ss (relative)
        dw      stktop - img                    ; e_sp
        dw      0                               ; e_csum
        dw      0                               ; e_ip
        dw      0                               ; e_cs (relative)
        dw      1Ch                             ; e_lfarlc
        dw      0                               ; e_ovno
        times   32 - ($ - hdr) db 0

img:                                            ; CS = load segment, IP = 0
        ; DS = ES = PSP on entry
        mov     bx, ds                          ; BX = PSP
        xor     ax, ax
        mov     es, ax                          ; ES = 0000 (the report slot)
        mov     ax, [2]                         ; memtop
        sub     ax, bx
        mov     [es:180h], ax
        mov     ax, cs
        sub     ax, bx
        mov     [es:182h], ax
        mov     ah, 48h
        mov     bx, 0FFFFh
        int     21h
        mov     [es:184h], bx
        mov     word [es:186h], 1234h
        mov     ax, 4C00h
        int     21h
        align   16
stk:    times   256 db 0
stktop:
fend:

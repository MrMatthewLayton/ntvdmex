; p_devchn.com -- the low-memory LAYOUT and the DOS-internal structures that
; describe it, as PROPERTIES that compare across hosts.  GH #207, GH #48.
;
; p_sysvar dumps the List of Lists byte for byte, and every row is abstained
; because the bytes are far pointers into each host's own memory.  This asks the
; questions those dumps were read offline to answer, in a form every host can be
; graded on:
;
;   layout.mcb.above.sysvars   AX=1 if the first MCB (ES:BX-2 of AH=52h) lies
;                              ABOVE the SysVars segment.  MEM /D prints "MSDOS
;                              System Data" = SysVars seg .. first MCB; ours was
;                              5Fh vs 72h and printed a negative size (#207).
;   layout.env.above.sysvars   AX=1 if this program's environment (PSP:2Ch) lies
;                              above the SysVars segment -- the block #207 moved.
;   dev.<NAME>                 for each of IO.SYS's eleven character devices:
;                              AX = its attribute word, BX = its ordinal in the
;                              chain counting only IO.SYS's devices (0000/FFFF if
;                              absent).  Installed drivers (HIMEM's XMSXXXX0 sits
;                              between NUL and CON on the oracle) are skipped, so
;                              the ordinal is comparable.  #48: our attributes
;                              are RBIL's, not measured -- this measures them.
;   dev.block                  the first block driver: AX = attribute, BX =
;                              ordinal.  Its unit count is per machine (DL, not
;                              in the signature).
;   dev.chain.terminates       AX=1 if the walk from NUL ends at offset FFFFh
;                              within 64 headers.
;   dpb.c.layout               AH=32h for C:.  AX=1 if the DPB's FAT layout is
;                              self-consistent: sectors/FAT nonzero, root start =
;                              reserved + FATs x sectors/FAT, data start = root
;                              start + ceil(root entries x 32 / bytes/sector).
;                              BX = number of FATs, DX = media byte.  #48: ours
;                              derives these (dos_dpb_fat_layout); this checks the
;                              derivation against a real FAT16 hard disk.
;   ext.sysvars45              INFORMATIONAL (SIG=CF): AX = SysVars+45h (extended
;                              memory at boot, KB), BX = INT 15h AH=88h now.  On
;                              6.22 with HIMEM these DIFFER (HIMEM hooks 88h and
;                              answers 0); the figures are per machine.
;
; ⚠ A PROPERTY ROW PASSES BY LUCK IF THE PROPERTY IS TRIVIAL.  None of these is:
;   each was 0, wrong or absent on NTVDMEX before #207/#48 (MCB 5Fh < 72h; env 60h
;   < 72h; NUL terminated the chain, so every dev.* row was 0000/FFFF; the DPB's
;   FAT sectors and data start were 0).
;
; nasm -f bin p_devchn.asm -o p_devchn.com

        org     100h
        jmp     start
%include "probe.inc"

NSTD    equ     11

; STDROW <index>, <case name> -- emit one IO.SYS device's attribute and ordinal.
%macro STDROW 2
        mov     ax, [stdattr + %1*2]
        mov     bx, [stdord + %1*2]
        clc
        call    probe_capture
        EMIT    %2, "AX,BX"
%endmacro

start:
        PROBE_BEGIN "devchn"

        ; ---- AH=52h.  ES:BX is a host address: only CF is compared.
        POISON
        mov     ax, 5200h
        int     21h
        call    probe_capture
        EMIT    "int21.52", "CF"
        mov     ax, [__es]
        mov     [svseg], ax
        mov     ax, [__bx]
        mov     [svoff], ax

        ; ---- the first MCB against the SysVars segment
        mov     es, [svseg]
        mov     bx, [svoff]
        mov     dx, [es:bx-2]           ; first MCB segment
        xor     ax, ax
        cmp     dx, [svseg]
        jbe     .mcb_no
        inc     ax
.mcb_no:
        mov     bx, 0                   ; nothing else in the signature
        clc
        call    probe_capture
        EMIT    "layout.mcb.above.sysvars", "AX"

        ; ---- this program's environment against the SysVars segment
        mov     dx, [2Ch]               ; PSP:2Ch, DS = our PSP (a .COM)
        xor     ax, ax
        cmp     dx, [svseg]
        jbe     .env_no
        inc     ax
.env_no:
        clc
        call    probe_capture
        EMIT    "layout.env.above.sysvars", "AX"

        ; ---- walk the device chain from NUL (inline at SysVars+22h)
        cld
        mov     cx, NSTD
        mov     di, stdattr
        push    ds
        pop     es
        xor     ax, ax
        rep     stosw                   ; attributes: 0000 = absent
        mov     cx, NSTD
        mov     di, stdord
        mov     ax, 0FFFFh
        rep     stosw                   ; ordinals:  FFFF = absent
        mov     word [battr], 0
        mov     word [bord], 0FFFFh
        mov     word [ordn], 0
        mov     word [hops], 0
        mov     word [term], 0
        mov     ax, [svoff]
        add     ax, 22h
        mov     [cur], ax
        mov     ax, [svseg]
        mov     [cur + 2], ax
.walk:
        les     bx, [cur]
        mov     ax, [es:bx + 4]         ; attribute
        mov     [cattr], ax
        test    ax, 8000h
        jnz     .chr
        cmp     word [bord], 0FFFFh     ; block driver: record the FIRST only
        jne     .next
        mov     [battr], ax
        mov     ax, [ordn]
        mov     [bord], ax
        inc     word [ordn]
        jmp     .next
.chr:
        xor     dx, dx                  ; dx = index into names
        mov     si, names
.cmp:
        push    si
        lea     di, [bx + 0Ah]          ; ES:DI = the header's name
        mov     cx, 8
        repe    cmpsb                   ; DS:SI = our table entry
        pop     si
        je      .hit
        add     si, 8
        inc     dx
        cmp     dx, NSTD
        jb      .cmp
        jmp     .next
.hit:
        mov     di, dx
        shl     di, 1
        cmp     word [stdord + di], 0FFFFh
        jne     .next                   ; a duplicate name: keep the first
        mov     ax, [cattr]
        mov     [stdattr + di], ax
        mov     ax, [ordn]
        mov     [stdord + di], ax
        inc     word [ordn]
.next:
        inc     word [hops]
        cmp     word [hops], 64
        jae     .done                   ; no terminator in 64: say so (term=0)
        mov     ax, [es:bx]             ; next offset
        mov     cx, [es:bx + 2]         ; next segment
        cmp     ax, 0FFFFh
        je      .ended
        mov     [cur], ax
        mov     [cur + 2], cx
        jmp     .walk
.ended:
        mov     word [term], 1
.done:
        STDROW  0,  "dev.CON"
        STDROW  1,  "dev.AUX"
        STDROW  2,  "dev.PRN"
        STDROW  3,  "dev.CLOCK$"
        STDROW  4,  "dev.COM1"
        STDROW  5,  "dev.LPT1"
        STDROW  6,  "dev.LPT2"
        STDROW  7,  "dev.LPT3"
        STDROW  8,  "dev.COM2"
        STDROW  9,  "dev.COM3"
        STDROW  10, "dev.COM4"
        mov     ax, [battr]
        mov     bx, [bord]
        clc
        call    probe_capture
        EMIT    "dev.block", "AX,BX"
        mov     ax, [term]
        mov     bx, [hops]
        clc
        call    probe_capture
        EMIT    "dev.chain.terminates", "AX"

        ; ---- the DPB for C:, through AH=32h (DS:BX -> DPB, AL=0)
        mov     word [dpbok], 0
        mov     byte [dnfats], 0
        mov     byte [dmedia], 0
        push    ds
        mov     ah, 32h
        mov     dl, 3
        int     21h
        push    ds
        pop     es                      ; ES:BX = the DPB
        pop     ds
        cmp     al, 0
        jne     .dpbout
        mov     al, [es:bx + 8]
        mov     [dnfats], al
        mov     al, [es:bx + 17h]
        mov     [dmedia], al
        mov     ax, [es:bx + 0Fh]       ; sectors per FAT (a WORD on DOS 4+)
        or      ax, ax
        jz      .dpbout
        xor     cx, cx
        mov     cl, [es:bx + 8]         ; number of FATs
        mul     cx                      ; DX:AX = FATs x sectors/FAT
        or      dx, dx
        jnz     .dpbout
        add     ax, [es:bx + 6]         ; + reserved sectors
        jc      .dpbout
        cmp     ax, [es:bx + 11h]       ; == root start?
        jne     .dpbout
        mov     ax, [es:bx + 9]         ; root entries
        mov     cx, 32
        mul     cx                      ; DX:AX = root bytes
        mov     cx, [es:bx + 2]         ; bytes per sector
        jcxz    .dpbout
        dec     cx
        add     ax, cx
        adc     dx, 0
        inc     cx
        div     cx                      ; AX = root sectors (rounded up)
        add     ax, [es:bx + 11h]
        cmp     ax, [es:bx + 0Bh]       ; == data start?
        jne     .dpbout
        mov     word [dpbok], 1
.dpbout:
        mov     ax, [dpbok]
        xor     bx, bx
        mov     bl, [dnfats]
        xor     dx, dx
        mov     dl, [dmedia]
        clc
        call    probe_capture
        EMIT    "dpb.c.layout", "AX,BX,DX"

        ; ---- SysVars+45h against INT 15h AH=88h (informational)
        mov     es, [svseg]
        mov     bx, [svoff]
        mov     ax, [es:bx + 45h]
        mov     [sv45], ax
        mov     ah, 88h
        int     15h
        mov     bx, ax
        mov     ax, [sv45]
        clc
        call    probe_capture
        EMIT    "ext.sysvars45", "CF"

        PROBE_END

names   db      "CON     ", "AUX     ", "PRN     ", "CLOCK$  "
        db      "COM1    ", "LPT1    ", "LPT2    ", "LPT3    "
        db      "COM2    ", "COM3    ", "COM4    "
svseg   dw      0
svoff   dw      0
cur     dd      0
cattr   dw      0
ordn    dw      0
hops    dw      0
term    dw      0
battr   dw      0
bord    dw      0
dpbok   dw      0
dnfats  db      0
dmedia  db      0
sv45    dw      0
stdattr times NSTD dw 0
stdord  times NSTD dw 0

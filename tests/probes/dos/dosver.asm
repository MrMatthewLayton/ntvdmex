; dosver.com -- what does DOS actually report for its version?
;
; Ground truth for GH #28 (configurable DOS version).  NTVDMEX currently answers
; INT 21h AH=30h with AX=0x0005 ("5.0", the same lie stock ntvdm tells); this
; probe asks a real MS-DOS kernel the same questions so the expected values in
; the tests come from the oracle rather than from anybody's memory.
;
; Reports, as raw hex so nothing is interpreted on the way out:
;   INT 21h AH=30h  AL=major AH=minor BH=OEM BL:CX=serial
;   INT 21h AX=3306h  BL=major BH=minor DL=revision DH=flags, and CF
;
; nasm -f bin dosver.asm -o dosver.com

        org     100h

start:
        ; ---- INT 21h AH=30h, AL=00 (AL=00 asks for the OEM number in BH)
        mov     ax, 3000h
        int     21h
        mov     [r30_al], al
        mov     [r30_ah], ah
        mov     [r30_bh], bh
        mov     [r30_bl], bl
        mov     [r30_cx], cx

        ; ---- INT 21h AX=3306h, "get true version" (DOS 5.0+)
        ; Undocumented-ish and not present on every DOS, so the carry flag is
        ; part of the answer, not an error to hide.
        mov     ax, 3306h
        clc
        int     21h
        mov     [r33_bl], bl
        mov     [r33_bh], bh
        mov     [r33_dl], dl
        mov     [r33_dh], dh
        mov     byte [r33_cf], 0
        jnc     .nocf
        mov     byte [r33_cf], 1
.nocf:

        ; ---- report
        mov     si, m30
        call    puts
        mov     al, [r30_al]
        call    puthex
        mov     si, m_min
        call    puts
        mov     al, [r30_ah]
        call    puthex
        mov     si, m_oem
        call    puts
        mov     al, [r30_bh]
        call    puthex
        mov     si, m_ser
        call    puts
        mov     al, [r30_bl]
        call    puthex
        mov     al, ':'
        call    putc
        mov     ax, [r30_cx]
        call    puthex16
        call    crlf

        mov     si, m33
        call    puts
        mov     al, [r33_bl]
        call    puthex
        mov     si, m_min
        call    puts
        mov     al, [r33_bh]
        call    puthex
        mov     si, m_rev
        call    puts
        mov     al, [r33_dl]
        call    puthex
        mov     si, m_flg
        call    puts
        mov     al, [r33_dh]
        call    puthex
        mov     si, m_cf
        call    puts
        mov     al, [r33_cf]
        call    puthex
        call    crlf

        mov     ax, 4C00h
        int     21h

; ---------------------------------------------------------------- helpers

putc:                                   ; AL = character
        push    ax
        push    dx
        mov     dl, al
        mov     ah, 02h
        int     21h
        pop     dx
        pop     ax
        ret

puts:                                   ; SI = NUL-terminated string
        push    ax
.loop:
        lodsb
        or      al, al
        jz      .done
        call    putc
        jmp     .loop
.done:
        pop     ax
        ret

puthex:                                 ; AL = byte
        push    ax
        push    cx
        mov     cl, al
        shr     al, 4
        call    .nyb
        mov     al, cl
        and     al, 0Fh
        call    .nyb
        pop     cx
        pop     ax
        ret
.nyb:
        and     al, 0Fh
        add     al, '0'
        cmp     al, '9'
        jbe     .out
        add     al, 7                   ; 'A'..'F'
.out:
        call    putc
        ret

puthex16:                               ; AX = word
        push    ax
        mov     al, ah
        call    puthex
        pop     ax
        call    puthex
        ret

crlf:
        push    ax
        mov     al, 13
        call    putc
        mov     al, 10
        call    putc
        pop     ax
        ret

; ------------------------------------------------------------------- data

m30     db      'INT21.30 major=', 0
m33     db      'INT21.3306 major=', 0
m_min   db      ' minor=', 0
m_oem   db      ' oem=', 0
m_ser   db      ' serial=', 0
m_rev   db      ' rev=', 0
m_flg   db      ' flags=', 0
m_cf    db      ' cf=', 0

r30_al  db      0
r30_ah  db      0
r30_bh  db      0
r30_bl  db      0
r30_cx  dw      0
r33_bl  db      0
r33_bh  db      0
r33_dl  db      0
r33_dh  db      0
r33_cf  db      0

; w_kstr.asm -- KERNEL's and USER's string API. (GH #163)
;
; EXPECTED VALUES are the Windows 3.1 SDK contract; comparisons are reduced to their
; sign (SIGN), because "less than zero / zero / greater than zero" is all lstrcmp
; promises. Ordinals read off guest/ne/{krnl386,user}.exe -- note lstrcmp and
; lstrcmpi are USER's (430, 471), not KERNEL's.
;
;   case                    expect  contract
;   kstr.lstrlen            0005    lstrlen("hello")
;   kstr.lstrlen.empty      0000    lstrlen("")
;   kstr.lstrcpy.ret        0001    lstrcpy returns its destination pointer
;   kstr.lstrcpy.len        0005    ...and copied the whole string
;   kstr.lstrcat.len        000B    lstrcat(dst, " world") -> "hello world"
;   kstr.lstrcat.ret        0001    ...returns its destination pointer
;   kstr.lstrcmp.lt         FFFF    lstrcmp("abc", "abd") < 0
;   kstr.lstrcmp.gt         0001    lstrcmp("abd", "abc") > 0
;   kstr.lstrcmp.eq         0000    lstrcmp("hello world", copy) == 0
;   kstr.lstrcmpi.eq        0000    lstrcmpi("HeLLo", "hello") == 0
;   kstr.lstrcpyn.len       0003    lstrcpyn(dst, "abcdef", 4): 3 chars + NUL
;   kstr.ansiupper.char     0041    AnsiUpper(MAKELP(0,'a')): high word 0 = one character
;   kstr.ansilower.char     0071    AnsiLower(MAKELP(0,'Q'))
;   kstr.ansiupper.str      0000    AnsiUpper("hello world") then lstrcmp vs "HELLO WORLD"
;   kstr.ischaralpha        0001    IsCharAlpha('a') nonzero
;   kstr.ischarupper        0000    IsCharUpper('a') zero
;   kstr.wvsprintf.len      000B    wvsprintf(buf, "%d-%04X-%s", {123, 0xAB, "xy"}) = 11
;   kstr.wvsprintf.eq       0000    ...and the text is "123-00AB-xy"
;
; build: tools/wintest/build.sh w_kstr     run: tools/wintest/run.sh w_kstr
        org     0
%include "w16.inc"
W16_HEAD
        IMP     K, 90, LSTRLEN
        IMP     K, 88, LSTRCPY
        IMP     K, 89, LSTRCAT
        IMP     K, 353, LSTRCPYN
        IMP     U, 430, LSTRCMP
        IMP     U, 471, LSTRCMPI
        IMP     U, 431, ANSIUPPER
        IMP     U, 432, ANSILOWER
        IMP     U, 433, ISCHARALPHA
        IMP     U, 435, ISCHARUPPER
        IMP     U, 421, WVSPRINTF
W16_IAT

DST     equ     D_BUF                   ; 64 bytes
DST2    equ     D_BUF + 64              ; 64 bytes
ARGS    equ     D_BUF + 128             ; wvsprintf's argument list

s_hello:  db 'hello', 0
s_empty:  db 0
s_world:  db ' world', 0
s_abc:    db 'abc', 0
s_abd:    db 'abd', 0
s_HeLLo:  db 'HeLLo', 0
s_abcdef: db 'abcdef', 0
s_HW:     db 'HELLO WORLD', 0
s_fmt:    db '%d-%04X-%s', 0
s_xy:     db 'xy', 0
s_want:   db '123-00AB-xy', 0

; AX := 1 if DX:AX == DS:BX (a far pointer equals our DGROUP buffer)
%macro ISDST 1
        cmp     ax, %1
        jne     %%no
        mov     ax, ds
        cmp     dx, ax
        jne     %%no
        mov     ax, 1
        jmp     %%o
%%no:   xor     ax, ax
%%o:
%endmacro

cases:
        PUSHCS  s_hello
        API     LSTRLEN
        OUT     "kstr.lstrlen"
        PUSHCS  s_empty
        API     LSTRLEN
        OUT     "kstr.lstrlen.empty"

        PUSHDS  DST
        PUSHCS  s_hello
        API     LSTRCPY
        ISDST   DST
        OUT     "kstr.lstrcpy.ret"
        PUSHDS  DST
        API     LSTRLEN
        OUT     "kstr.lstrcpy.len"

        PUSHDS  DST
        PUSHCS  s_world
        API     LSTRCAT
        ISDST   DST
        mov     [D_T0], ax              ; ⚠ NOT in BX: a Win16 API preserves DS/SI/DI/BP
                                        ;   only. The first run reported 15F4 here.
        PUSHDS  DST
        API     LSTRLEN
        OUT     "kstr.lstrcat.len"
        mov     ax, [D_T0]
        OUT     "kstr.lstrcat.ret"

        PUSHCS  s_abc
        PUSHCS  s_abd
        API     LSTRCMP
        SIGN
        OUT     "kstr.lstrcmp.lt"
        PUSHCS  s_abd
        PUSHCS  s_abc
        API     LSTRCMP
        SIGN
        OUT     "kstr.lstrcmp.gt"
        PUSHDS  DST2
        PUSHDS  DST
        API     LSTRCPY
        PUSHDS  DST
        PUSHDS  DST2
        API     LSTRCMP
        SIGN
        OUT     "kstr.lstrcmp.eq"
        PUSHCS  s_HeLLo
        PUSHCS  s_hello
        API     LSTRCMPI
        SIGN
        OUT     "kstr.lstrcmpi.eq"

        PUSHDS  DST2
        PUSHCS  s_abcdef
        push    word 4
        API     LSTRCPYN
        PUSHDS  DST2
        API     LSTRLEN
        OUT     "kstr.lstrcpyn.len"

        push    word 0                  ; MAKELP(0, 'a'): one character, not a string
        push    word 'a'
        API     ANSIUPPER
        OUT     "kstr.ansiupper.char"
        push    word 0
        push    word 'Q'
        API     ANSILOWER
        OUT     "kstr.ansilower.char"
        PUSHDS  DST                     ; "hello world" in place
        API     ANSIUPPER
        PUSHDS  DST
        PUSHCS  s_HW
        API     LSTRCMP
        SIGN
        OUT     "kstr.ansiupper.str"

        push    word 'a'                ; IsCharAlpha(char) -- a char, pushed as a word
        API     ISCHARALPHA
        BOOLN
        OUT     "kstr.ischaralpha"
        push    word 'a'
        API     ISCHARUPPER
        BOOLN
        OUT     "kstr.ischarupper"

        ; the argument list is laid out as C would push it: int, int, far pointer
        mov     word [ARGS], 123
        mov     word [ARGS+2], 0ABh
        mov     word [ARGS+4], s_xy
        mov     [ARGS+6], cs
        PUSHDS  DST2
        PUSHCS  s_fmt
        PUSHDS  ARGS
        API     WVSPRINTF
        OUT     "kstr.wvsprintf.len"
        PUSHDS  DST2
        PUSHCS  s_want
        API     LSTRCMP
        SIGN
        OUT     "kstr.wvsprintf.eq"
        jmp     w16_fin

W16_TAIL "w16kstr"

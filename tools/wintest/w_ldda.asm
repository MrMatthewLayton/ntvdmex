; w_ldda.asm -- GDI LineDDA (ordinal 100) through a 16-bit callback, against stock (s91).
;
; LineDDA(x1, y1, x2, y2, lpLineFunc, lParam) calls LineFunc(x, y, lParam) -- FAR
; PASCAL, 8 argument bytes -- once per point of the line, end point excluded. The last
; GDI path the score called "implemented, not exercised by a guest". Cases: a shallow
; line, a steep one, a backwards one and a single point; per case the number of calls,
; the first and last point, and that lParam arrived intact. Counted through SS (= DGROUP).

        org     0
%include "w16.inc"
W16_HEAD
        IMP     G, 100, LINEDDA
W16_IAT

CNT     equ     D_T0
FX      equ     D_T0+2
FY      equ     D_T0+4
LX      equ     D_T0+6
LY      equ     D_T0+8
LPOK    equ     D_T0+10

        jmp     cases

; void FAR PASCAL LineFunc(int x, int y, LPARAM lParam)
linefunc:
        push    bp
        mov     bp, sp
        push    ds
        push    ss
        pop     ds
        ; PASCAL: x pushed first (highest), then y, then lParam hi, lo
        ;   [bp+0Ch] x   [bp+0Ah] y   [bp+08h] lParam hi   [bp+06h] lParam lo
        cmp     word [CNT], 0
        jne     .n
        mov     ax, [bp+0Ch]
        mov     [FX], ax
        mov     ax, [bp+0Ah]
        mov     [FY], ax
.n:     inc     word [CNT]
        mov     ax, [bp+0Ch]
        mov     [LX], ax
        mov     ax, [bp+0Ah]
        mov     [LY], ax
        cmp     word [bp+08h], 1234h
        jne     .bad
        cmp     word [bp+06h], 5678h
        jne     .bad
        jmp     .r
.bad:   mov     word [LPOK], 0
.r:     pop     ds
        pop     bp
        retf    8

%macro LINE 5                           ; x1 y1 x2 y2 name
        mov     word [CNT], 0
        mov     word [LPOK], 1
        push    word %1
        push    word %2
        push    word %3
        push    word %4
        push    cs
        push    word linefunc
        push    word 1234h
        push    word 5678h
        API     LINEDDA
        mov     ax, [CNT]
        %strcat %%c_count %5, ".count"
        OUT     %%c_count
        mov     ax, [FX]
        %strcat %%c_first_x %5, ".first.x"
        OUT     %%c_first_x
        mov     ax, [FY]
        %strcat %%c_first_y %5, ".first.y"
        OUT     %%c_first_y
        mov     ax, [LX]
        %strcat %%c_last_x %5, ".last.x"
        OUT     %%c_last_x
        mov     ax, [LY]
        %strcat %%c_last_y %5, ".last.y"
        OUT     %%c_last_y
        mov     ax, [LPOK]
        %strcat %%c_lparam_ok %5, ".lparam.ok"
        OUT     %%c_lparam_ok
%endmacro

cases:
        LINE    0, 0, 10, 4, "shallow"
        LINE    5, 2, 7, 12, "steep"
        LINE    20, 10, 3, 1, "backwards"
        LINE    8, 8, 8, 8, "point"
        jmp     w16_fin

W16_TAIL "w16ldda"

; w_tm.asm -- GetTextMetrics / GetTextExtent on the screen DC, against stock (s91).
;
; Cardfile lays its card out from the system font's metrics and draws it taller than
; stock does (runs/stockshot/s91_cardfile_*). This dumps the Win16 TEXTMETRIC (31 bytes:
; 8 INT16s at +0, 9 BYTEs at +16 (italic underlined struckout first last default break
; pitch&family charset), 3 INT16s at +25 (overhang, aspect x/y)) for the default (SYSTEM)
; font, SYSTEM_FIXED_FONT and
; ANSI_VAR_FONT, and the extent of a short string -- the numbers layout is built from.

        org     0
%include "w16.inc"
W16_HEAD
        IMP     U, 66,  GETDC
        IMP     U, 68,  RELEASEDC
        IMP     G, 93,  GETTEXTMETRICS
        IMP     G, 87,  GETSTOCKOBJECT
        IMP     G, 45,  SELECTOBJECT
        IMP     G, 91,  GETTEXTEXTENT
W16_IAT

HDC_    equ     D_T0
OLDF    equ     D_T0+2
TM      equ     D_BUF                   ; 31 bytes

%macro TMW 2                            ; offset, name: a WORD field
        mov     ax, [TM+%1]
        OUT     %2
%endmacro
%macro TMB 2                            ; offset, name: a BYTE field
        mov     al, [TM+%1]
        xor     ah, ah
        OUT     %2
%endmacro
%macro DUMP 1                           ; prefix
        push    word [HDC_]
        PUSHDS  TM
        API     GETTEXTMETRICS
        BOOLN
        %strcat %%n_ok %1, ".ok"
        OUT     %%n_ok
        %strcat %%theight %1, ".height"
        TMW     0, %%theight
        %strcat %%tascent %1, ".ascent"
        TMW     2, %%tascent
        %strcat %%tdescent %1, ".descent"
        TMW     4, %%tdescent
        %strcat %%tintlead %1, ".intlead"
        TMW     6, %%tintlead
        %strcat %%textlead %1, ".extlead"
        TMW     8, %%textlead
        %strcat %%tavewidth %1, ".avewidth"
        TMW     10, %%tavewidth
        %strcat %%tmaxwidth %1, ".maxwidth"
        TMW     12, %%tmaxwidth
        %strcat %%tweight %1, ".weight"
        TMW     14, %%tweight
        %strcat %%titalic %1, ".italic"
        TMB     16, %%titalic
        %strcat %%tunder %1, ".underlined"
        TMB     17, %%tunder
        %strcat %%tstruck %1, ".struckout"
        TMB     18, %%tstruck
        %strcat %%tfirst %1, ".firstchar"
        TMB     19, %%tfirst
        %strcat %%tlast %1, ".lastchar"
        TMB     20, %%tlast
        %strcat %%tdef %1, ".defaultchar"
        TMB     21, %%tdef
        %strcat %%tbreak %1, ".breakchar"
        TMB     22, %%tbreak
        %strcat %%tpitchfam %1, ".pitchfam"
        TMB     23, %%tpitchfam
        %strcat %%tcharset %1, ".charset"
        TMB     24, %%tcharset
        %strcat %%toverhang %1, ".overhang"
        TMW     25, %%toverhang
        %strcat %%taspx %1, ".aspectx"
        TMW     27, %%taspx
        %strcat %%taspy %1, ".aspecty"
        TMW     29, %%taspy
        push    word [HDC_]
        PUSHCS  s_card
        push    word 4
        API     GETTEXTEXTENT
        push    dx
        %strcat %%n_extent_cx %1, ".extent.cx"
        OUT     %%n_extent_cx
        pop     ax
        %strcat %%n_extent_cy %1, ".extent.cy"
        OUT     %%n_extent_cy
%endmacro

        jmp     cases
cases:
        push    word 0
        API     GETDC
        mov     [HDC_], ax
        BOOLN
        OUT     "getdc"

        DUMP    "sys"

        push    word 16                 ; SYSTEM_FIXED_FONT
        API     GETSTOCKOBJECT
        push    word [HDC_]
        push    ax
        API     SELECTOBJECT
        mov     [OLDF], ax
        DUMP    "fixed"

        push    word 12                 ; ANSI_VAR_FONT
        API     GETSTOCKOBJECT
        push    word [HDC_]
        push    ax
        API     SELECTOBJECT
        DUMP    "ansivar"

        push    word [HDC_]
        push    word [OLDF]
        API     SELECTOBJECT
        push    word 0
        push    word [HDC_]
        API     RELEASEDC
        jmp     w16_fin

s_card: db 'Card'

W16_TAIL "w16tm"

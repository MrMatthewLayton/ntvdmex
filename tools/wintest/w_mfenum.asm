; w_mfenum.asm -- EnumMetaFile (GDI.175) and PlayMetaFileRecord (GDI.176), #295.
;
; A metafile is recorded (SelectObject(red pen) + SelectObject(green brush) +
; Rectangle(1,1,7,7)), then EnumMetaFile walks it with a callback that, per record:
; counts it, keeps its rdFunction (the first eight -> mfe.rec.0..7, BEEF = no such
; record), checks the lParam and the hdc it was handed, and plays the record into a
; memory DC holding a black 8x8 colour bitmap with PlayMetaFileRecord. The pixels
; then say whether the played records really drew, with the pen and brush the
; HANDLETABLE carried from record to record.
; The cases settle, against stock:
;   * the record sequence, and whether the final META_EOF (function 0000) is handed
;     to the callback at all -- mfe.count and the last mfe.rec.N. ⚠ ntvdmex's answer
;     is a single constant (WOWMF_PASS_EOF, src/wow/wowgdi.h) set from THIS run;
;   * nObj (mtNoObjects) and the first record's rdSize;
;   * the handle table persisting: lpht[0] is nonzero right after record 1 (a
;     CreatePenIndirect) was played, and STILL nonzero when record 2 is entered;
;   * a callback returning 0 at record 2 stops the walk and EnumMetaFile returns 0;
;   * EnumMetaFile on a NULL metafile handle;
;   * PlayMetaFileRecord OUTSIDE an enumeration on a hand-built record pair: a
;     CreateBrushIndirect writes a handle into the caller's table, and a
;     DeleteObject of that slot zeroes it.
; ⚠ PlayMetaFileRecord is VOID in Win16 (3.1 SDK; Wine's gdi.exe.spec), so
;   mfe.play.ret1 / mfe.solo.ret are whatever the thunk leaves in AX (poisoned to
;   BEEF first): informative, not a contract.
; ⚠ The colour cases assume a display of 16bpp or more (pure red/green are exact);
;   both hosts run on the same box, so they must agree exactly.
; Ordinals: gdi.exe's resident/non-resident name tables (read off guest/ne/gdi.exe);
; GetDC/ReleaseDC are USER.66/68.

        org     0
%include "w16.inc"
W16_HEAD
        IMP     U, 66,  GETDC
        IMP     U, 68,  RELEASEDC
        IMP     G, 175, ENUMMETAFILE
        IMP     G, 176, PLAYMETAFILERECORD
        IMP     G, 61,  CREATEPEN
        IMP     G, 66,  CREATESOLIDBRUSH
        IMP     G, 52,  CREATECOMPATIBLEDC
        IMP     G, 51,  CREATECOMPATIBLEBITMAP
        IMP     G, 45,  SELECTOBJECT
        IMP     G, 29,  PATBLT
        IMP     G, 83,  GETPIXEL
        IMP     G, 27,  RECTANGLE
        IMP     G, 125, CREATEMETAFILE
        IMP     G, 126, CLOSEMETAFILE
        IMP     G, 127, DELETEMETAFILE
        IMP     G, 68,  DELETEDC
        IMP     G, 69,  DELETEOBJECT
W16_IAT

SDC     equ     D_T0                    ; screen DC
MDC     equ     D_T0+2                  ; memory DC: the played records land here
HBM     equ     D_T0+4
MF      equ     D_T0+6
RDC     equ     D_T0+8                  ; the recording DC
PEN     equ     D_T0+10
BRUSH   equ     D_T0+12
CNT     equ     D_T0+14                 ; callbacks made
STOPAT  equ     D_T0+16                 ; return 0 once CNT reaches this (0 = never)
PLAY    equ     D_T0+18                 ; 1 = play each record
NOBJ    equ     D_T0+20                 ; nObj as the first callback saw it
LPOK    equ     D_T0+22                 ; 1 until a callback sees the wrong lParam
HDCOK   equ     D_BUF                   ; 1 until a callback sees the wrong hdc
PRET1   equ     D_BUF+2                 ; AX after playing record 1
HT0A    equ     D_BUF+4                 ; lpht[0] != 0 right after record 1 played
HT0B    equ     D_BUF+6                 ; lpht[0] != 0 on entry to record 2
SIZE0   equ     D_BUF+8                 ; record 1's rdSize (low word)
OBM     equ     D_BUF+10                ; the memory DC's original bitmap
SEQ     equ     D_BUF+16                ; 8 words: rdFunction of records 0..7
SREC    equ     D_BUF+32                ; a hand-built CreateBrushIndirect (14 bytes)
SDEL    equ     D_BUF+48                ; a hand-built DeleteObject(0)       (8 bytes)
SHT     equ     D_BUF+64                ; their one-entry HANDLETABLE

        jmp     cases

; int FAR PASCAL cb(HDC hdc, HANDLETABLE FAR *lpht, METARECORD FAR *lpmr,
;                   int nObj, LPARAM lParam)                         -- 14 bytes
;   [bp+6] lParam lo, [bp+8] lParam hi, [bp+10] nObj, [bp+12] lpmr (off, seg),
;   [bp+16] lpht (off, seg), [bp+20] hdc. Counts into DGROUP through SS.
cb_mf:
        push    bp
        mov     bp, sp
        push    ds
        push    ss
        pop     ds
        push    es
        push    bx
        push    si
        les     bx, [bp+12]             ; the record
        mov     si, [CNT]
        cmp     si, 8
        jae     .noseq
        mov     ax, [es:bx+4]           ; rdFunction
        shl     si, 1
        mov     [SEQ+si], ax
.noseq: cmp     word [CNT], 0
        jne     .nf
        mov     ax, [es:bx]             ; rdSize, low word
        mov     [SIZE0], ax
        mov     ax, [bp+10]
        mov     [NOBJ], ax
.nf:    cmp     word [bp+6], 5678h
        jne     .lpbad
        cmp     word [bp+8], 1234h
        je      .lpok
.lpbad: mov     word [LPOK], 0
.lpok:  mov     ax, [bp+20]
        cmp     ax, [MDC]
        je      .hdcok
        mov     word [HDCOK], 0
.hdcok: cmp     word [CNT], 1           ; entering record 2: does slot 0 persist?
        jne     .nob
        les     bx, [bp+16]
        mov     ax, [es:bx]
        BOOLN
        mov     [HT0B], ax
.nob:   cmp     word [PLAY], 0
        je      .noplay
        push    word [bp+20]            ; hdc
        push    word [bp+18]            ; lpht
        push    word [bp+16]
        push    word [bp+14]            ; lpmr
        push    word [bp+12]
        push    word [bp+10]            ; nHandles = nObj
        mov     ax, 0BEEFh
        API     PLAYMETAFILERECORD
        cmp     word [CNT], 0
        jne     .noplay
        mov     [PRET1], ax
        les     bx, [bp+16]
        mov     ax, [es:bx]
        BOOLN
        mov     [HT0A], ax
.noplay:
        inc     word [CNT]
        mov     ax, 1
        mov     cx, [STOPAT]
        jcxz    .r
        cmp     [CNT], cx
        jb      .r
        xor     ax, ax
.r:     pop     si
        pop     bx
        pop     es
        pop     ds
        pop     bp
        retf    16                      ; hdc 2 + lpht 4 + lpmr 4 + nObj 2 + lParam 4

; RESET stopat, play -- every output poisoned to BEEF before the call.
%macro RESET 2
        mov     word [CNT], 0
        mov     word [STOPAT], %1
        mov     word [PLAY], %2
        mov     word [LPOK], 1
        mov     word [HDCOK], 1
        mov     ax, 0BEEFh
        mov     [NOBJ], ax
        mov     [PRET1], ax
        mov     [HT0A], ax
        mov     [HT0B], ax
        mov     [SIZE0], ax
        push    ds
        pop     es
        mov     di, SEQ
        mov     cx, 8
        cld
        rep     stosw
%endmacro

%macro ENUM 1                           ; EnumMetaFile(MDC, %1, cb_mf, 12345678h)
        push    word [MDC]
        push    word %1
        push    cs
        push    word cb_mf
        push    word 1234h
        push    word 5678h
        mov     ax, 0BEEFh
        API     ENUMMETAFILE
%endmacro

%macro PIXEL 3                          ; x, y, case-name prefix: AX then DX
        push    word [MDC]
        push    word %1
        push    word %2
        mov     ax, 0BEEFh
        mov     dx, 0BEEFh
        API     GETPIXEL
        OUT     {%3, ".lo"}
        mov     ax, dx
        OUT     {%3, ".hi"}
%endmacro

cases:
        push    word 0
        API     GETDC
        mov     [SDC], ax

        ; ── the target: a black 8x8 colour bitmap in a memory DC ──
        push    word [SDC]
        API     CREATECOMPATIBLEDC
        mov     [MDC], ax
        push    word [SDC]
        push    word 8
        push    word 8
        API     CREATECOMPATIBLEBITMAP
        mov     [HBM], ax
        push    word [MDC]
        push    word [HBM]
        API     SELECTOBJECT
        mov     [OBM], ax
        push    word [MDC]
        push    word 0
        push    word 0
        push    word 8
        push    word 8
        push    word 0                  ; BLACKNESS
        push    word 0042h
        API     PATBLT

        ; ── the recording: red pen, green brush, one rectangle ──
        push    word 0
        push    word 0
        API     CREATEMETAFILE          ; NULL = memory metafile
        mov     [RDC], ax
        BOOLN
        OUT     "mfe.create"
        push    word 0                  ; PS_SOLID
        push    word 1                  ; width 1
        push    word 0                  ; RGB(255,0,0) = 0x000000FF
        push    word 00FFh
        API     CREATEPEN
        mov     [PEN], ax
        push    word 0                  ; RGB(0,255,0) = 0x0000FF00
        push    word 0FF00h
        API     CREATESOLIDBRUSH
        mov     [BRUSH], ax
        push    word [RDC]
        push    word [PEN]
        API     SELECTOBJECT
        push    word [RDC]
        push    word [BRUSH]
        API     SELECTOBJECT
        push    word [RDC]
        push    word 1
        push    word 1
        push    word 7
        push    word 7
        API     RECTANGLE
        push    word [RDC]
        API     CLOSEMETAFILE
        mov     [MF], ax
        BOOLN
        OUT     "mfe.close"

        ; ── 1: the whole walk, every record played ──
        RESET   0, 1
        ENUM    [MF]
        OUT     "mfe.ret"
        mov     ax, [CNT]
        OUT     "mfe.count"
        mov     ax, [SEQ]
        OUT     "mfe.rec.0"
        mov     ax, [SEQ+2]
        OUT     "mfe.rec.1"
        mov     ax, [SEQ+4]
        OUT     "mfe.rec.2"
        mov     ax, [SEQ+6]
        OUT     "mfe.rec.3"
        mov     ax, [SEQ+8]
        OUT     "mfe.rec.4"
        mov     ax, [SEQ+10]
        OUT     "mfe.rec.5"
        mov     ax, [SEQ+12]
        OUT     "mfe.rec.6"
        mov     ax, [SEQ+14]
        OUT     "mfe.rec.7"
        mov     ax, [NOBJ]
        OUT     "mfe.nobj"
        mov     ax, [SIZE0]
        OUT     "mfe.size0"
        mov     ax, [LPOK]
        OUT     "mfe.lparam"
        mov     ax, [HDCOK]
        OUT     "mfe.hdc"
        mov     ax, [PRET1]
        OUT     "mfe.play.ret1"
        mov     ax, [HT0A]
        OUT     "mfe.ht0.after1"
        mov     ax, [HT0B]
        OUT     "mfe.ht0.at2"
        PIXEL   4, 4, "mfe.px.inside"   ; FF00/0000: the green brush
        PIXEL   1, 1, "mfe.px.edge"     ; 00FF/0000: the red pen
        PIXEL   0, 0, "mfe.px.outside"  ; 0000/0000: untouched

        ; ── 2: the callback says stop at record 2 ──
        RESET   2, 0
        ENUM    [MF]
        OUT     "mfe.stop.ret"
        mov     ax, [CNT]
        OUT     "mfe.stop.count"

        ; ── 3: not a metafile -- NULL (w_genum's PlayMetaFile case does the same) ──
        RESET   0, 0
        ENUM    0
        OUT     "mfe.bad.ret"
        mov     ax, [CNT]
        OUT     "mfe.bad.count"

        ; ── 4: PlayMetaFileRecord on its own: create into slot 0, then delete it ──
        mov     word [SREC], 7          ; rdSize = 7 WORDS
        mov     word [SREC+2], 0
        mov     word [SREC+4], 02FCh    ; META_CREATEBRUSHINDIRECT
        mov     word [SREC+6], 0        ; LOGBRUSH16: BS_SOLID
        mov     word [SREC+8], 00FFh    ;   red
        mov     word [SREC+10], 0
        mov     word [SREC+12], 0       ;   hatch
        mov     word [SDEL], 4          ; rdSize = 4 WORDS
        mov     word [SDEL+2], 0
        mov     word [SDEL+4], 01F0h    ; META_DELETEOBJECT
        mov     word [SDEL+6], 0        ;   slot 0
        mov     word [SHT], 0
        push    word [MDC]
        PUSHDS  SHT
        PUSHDS  SREC
        push    word 1
        mov     ax, 0BEEFh
        API     PLAYMETAFILERECORD
        OUT     "mfe.solo.ret"
        mov     ax, [SHT]
        BOOLN
        OUT     "mfe.solo.ht0.made"
        push    word [MDC]
        PUSHDS  SHT
        PUSHDS  SDEL
        push    word 1
        API     PLAYMETAFILERECORD
        mov     ax, [SHT]
        OUT     "mfe.solo.ht0.deleted"

        push    word [MF]
        API     DELETEMETAFILE
        BOOLN
        OUT     "mfe.delete"
        push    word [MDC]
        push    word [OBM]
        API     SELECTOBJECT
        push    word [HBM]
        API     DELETEOBJECT
        push    word [MDC]
        API     DELETEDC
        push    word [PEN]
        API     DELETEOBJECT
        push    word [BRUSH]
        API     DELETEOBJECT
        push    word 0
        push    word [SDC]
        API     RELEASEDC
        jmp     w16_fin

W16_TAIL "w16mfenum"

; w16.inc -- the shared skeleton of a deterministic Win16 test (GH #163).
;
; w_kernel.asm and w_cwd.asm each carry a full copy of the skeleton; every rule in it
; was paid for (read w_kernel.asm's header before changing anything here):
;   * the manifest at offset 0 is what tools/ne/mkne.py links from;
;   * imports are an IMPORT ADDRESS TABLE of far pointers called `call far [cs:slot]`
;     -- ⛔ a Win16 API's RETF pops TWO words, so a near call into a stub is broken;
;   * InitTask is the first instruction after `xor bp,bp / push bp` (TASKMAN's order);
;   * the REPORT goes out through INT 21h, never through the KERNEL under test, with a
;     progress line per startup step, so a short file names the step that failed;
;   * the output path is ABSOLUTE and 8.3 (see w_kernel.asm's string note).
; This file is that skeleton once, so a new test is its imports and its cases.
;
; A test looks like:
;
;       org 0
;       %include "w16.inc"
;       W16_HEAD
;               IMP K, 90, LSTRLEN          ; module, ordinal, slot name
;       W16_IAT
;       cases:  ...                         ; DS = DGROUP; compute AX, then
;               OUT "kstr.lstrlen"          ; -> CASE=kstr.lstrlen SIG=AX AX=nnnn
;               ...
;               jmp w16_fin
;       W16_TAIL "w16kstr"
;
; ⚠ EVERY ORDINAL IS READ OFF THE BINARY: guest/ne/{krnl386,user,gdi}.exe, resident +
;   non-resident name tables (a twenty-line script does it). Never
;   from memory -- lstrcmp is USER.430 in 3.1, not KERNEL, and memory says otherwise.
; ⚠ CALLING CONVENTION: PASCAL. Arguments are pushed LEFT TO RIGHT and the callee pops
;   them. A DWORD (or a far pointer) is pushed HIGH word first (segment, then offset).
;   A small struct passed BY VALUE (POINT) is pushed so it lies in memory in field
;   order: last field first (y, then x). Results: AX, or DX:AX for a DWORD/far pointer.
;   The callee preserves DS, SI, DI, BP.

K       equ 1                           ; module index: KERNEL
U       equ 2                           ;               USER
G       equ 3                           ;               GDI
X       equ 4                           ;               the optional 4th: W16_HEAD 'SOUND'
Y       equ 5                           ;               the optional 5th: W16_HEAD 'SHELL','KEYBOARD'

; ── DGROUP scratch. mkne.py zero-fills the data segment (0x200 bytes by default);
;    offsets 0..0Fh belong to Windows (the instance-data header), so start at 40h.
D_HINST         equ 0x40
D_HFILE         equ 0x42
D_HEX           equ 0x44                ; four bytes for whex
D_T0            equ 0x48                ; free words for a test: 48h..5Fh
D_BUF           equ 0x60                ; 0x60..0x1FF free for a test (416 bytes)

%assign __nimp 0
%macro IMP 3                            ; module, ordinal, slot name
        dw      %1, %2
%3      equ     iat + 4*__nimp
%assign __nimp __nimp + 1
%endmacro

; W16_HEAD [name [, name2]] -- extra imported modules, indices X and Y.
%macro W16_HEAD 0-2
manifest:
        db      'NEIM'
        dw      start                   ; entry point
        dw      iat                     ; import address table
        dw      NIMP
%if %0 == 2
        dw      5
        dw      m_kernel, m_user, m_gdi, m_extra, m_extra2
%define W16_EXTRA %1
%define W16_EXTRA2 %2
%elif %0 == 1
        dw      4                       ; KERNEL, USER, GDI, %1
        dw      m_kernel, m_user, m_gdi, m_extra
%define W16_EXTRA %1
%else
        dw      3                       ; KERNEL, USER, GDI
        dw      m_kernel, m_user, m_gdi
%endif
imports:
        IMP     K, 91, INITTASK
        IMP     K, 30, WAITEVENT
        IMP     U,  5, INITAPP
%endmacro

%macro W16_IAT 0
NIMP    equ     __nimp
m_kernel:  db   'KERNEL', 0
m_user:    db   'USER', 0
m_gdi:     db   'GDI', 0
%ifdef W16_EXTRA
m_extra:   db   W16_EXTRA, 0
%endif
%ifdef W16_EXTRA2
m_extra2:  db   W16_EXTRA2, 0
%endif
iat:
%rep NIMP
        dw      0FFFFh                  ; FF FF 00 00: the unrelocated far pointer
        dw      0
%endrep
%endmacro

%macro API 1                            ; ⛔ FAR. See the header.
        call    far [cs:%1]
%endmacro

; CASE=<name> SIG=AX AX=<AX>. Preserves every register.
%macro OUT 1
        jmp     %%o
%%s:    db      'CASE=', %1, ' SIG=AX AX=', 0
%%o:    push    si
        mov     si, %%s
        call    wcs
        call    whex
        call    wnl
        pop     si
%endmacro

; AX := (AX != 0) -- for a BOOL whose documented contract is only "nonzero".
%macro BOOLN 0
        neg     ax
        sbb     ax, ax
        neg     ax
%endmacro

; AX := sign(AX) as 0001 / 0000 / FFFF -- for a comparison whose magnitude is not
; part of the contract (lstrcmp: "less than zero / zero / greater than zero").
%macro SIGN 0
        or      ax, ax
        jz      %%z
        js      %%n
        mov     ax, 1
        jmp     %%z
%%n:    mov     ax, 0FFFFh
%%z:
%endmacro

; push a far pointer to a CS-resident string literal
%macro PUSHCS 1
        push    cs
        push    %1
%endmacro

; push a far pointer to DGROUP offset
%macro PUSHDS 1
        push    ds
        push    %1
%endmacro

%macro W16_TAIL 1
start:
        xor     bp, bp
        push    bp                      ; BP=0 terminates the frame chain
        API     INITTASK
        mov     bx, ax                  ; InitTask's verdict, kept across the open
        mov     [D_HINST], di           ; ⚠ AFTER InitTask: it RETURNS hInstance in DI
        push    ds
        push    cs
        pop     ds
        mov     dx, w16_s_file
        xor     cx, cx
        mov     ah, 3Ch                 ; create/truncate -- through DOS, not KERNEL
        int     21h
        pop     ds
        jc      w16_bail
        mov     [D_HFILE], ax
        mov     si, w16_s_head
        call    wcs
        mov     si, w16_s_i
        call    wcs
        mov     ax, bx
        call    whex
        call    wnl
        or      bx, bx
        jz      w16_fin
        xor     ax, ax
        push    ax
        API     WAITEVENT
        mov     si, w16_s_w
        call    wcs
        push    word [D_HINST]
        API     INITAPP
        or      ax, ax
        jz      w16_fin
        mov     si, w16_s_a
        call    wcs
        jmp     cases
w16_fin:
        mov     si, w16_s_end
        call    wcs
        mov     bx, [D_HFILE]
        mov     ah, 3Eh
        int     21h
w16_bail:
        mov     ax, 4C00h
        int     21h

; SI = CS-relative NUL-terminated string. Preserves everything.
wcs:
        push    ax
        push    bx
        push    cx
        push    dx
        push    si
        push    ds
        mov     bx, [D_HFILE]           ; ⚠ while DS is still DGROUP
        xor     cx, cx
        mov     dx, si
.len:   cmp     byte [cs:si], 0
        je      .go
        inc     si
        inc     cx
        jmp     .len
.go:    push    cs
        pop     ds
        mov     ah, 40h
        int     21h
        pop     ds
        pop     si
        pop     dx
        pop     cx
        pop     bx
        pop     ax
        ret

wnl:
        push    si
        mov     si, w16_s_nl
        call    wcs
        pop     si
        ret

; AX -> four hex digits. Preserves everything.
whex:
        push    ax
        push    bx
        push    cx
        push    dx
        push    di
        mov     bx, ax
        mov     di, D_HEX
        mov     cx, 4
.d:     rol     bx, 4
        mov     al, bl
        and     al, 0Fh
        add     al, '0'
        cmp     al, '9'
        jbe     .ok
        add     al, 7
.ok:    mov     [di], al
        inc     di
        loop    .d
        mov     bx, [D_HFILE]
        mov     cx, 4
        mov     dx, D_HEX
        mov     ah, 40h
        int     21h
        pop     di
        pop     dx
        pop     cx
        pop     bx
        pop     ax
        ret

w16_s_file: db 'C:\DOCUME~1\ALLUSE~1\DOCUME~1\NTVDMEX\DEBUG\OUT\W16OUT.TXT', 0
w16_s_head: db '#PROBE ', %1, 13, 10, 0
w16_s_i:    db 'STEP=I AX=', 0
w16_s_w:    db 'STEP=W', 13, 10, 0
w16_s_a:    db 'STEP=A', 13, 10, 0
w16_s_nl:   db 13, 10, 0
w16_s_end:  db '#END', 13, 10, 0
%endmacro

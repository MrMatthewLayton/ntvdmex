; w_comm.asm -- OpenComm's answers, for #334 item 9. Built by tools/ne/mkne.py.
;
; WowCommOpen returns -2 for a port that does not exist and -5 for one already
; open, with comments naming them IE_BADID and IE_OPEN. Win16's documented values
; are IE_BADID -1, IE_OPEN -2, IE_DEFAULT -5, so the names and the values disagree,
; and nothing ever measured what WOW itself answers. This asks, on stock and on
; ours: a port that may exist (COM1, then COM1 again), one that does not (COM9),
; and a name that is not a port at all.
;
; Ordinals from guest/win16/user.exe's non-resident names: OPENCOMM @200,
; CLOSECOMM @207. Both are PASCAL: OpenComm(LPCSTR lpszDevControl, UINT cbInQueue,
; UINT cbOutQueue) -> int; CloseComm(int idComDev) -> int.
;
; Everything else -- the start-up handshake, the INT 21h report, the IAT -- is
; w_kernel.asm's, unchanged; see the notes there.
;
; build: see tests/probes/win16/build.sh

        org     0

NMOD            equ 2
K               equ 1                   ; module index of KERNEL
U               equ 2                   ; module index of USER

; ── the manifest mkne.py reads. See tools/ne/mkne.py for the layout. ─────────
manifest:
        db      'NEIM'
        dw      start                   ; entry point
        dw      iat                     ; import address table
        dw      NIMP
        dw      NMOD
        dw      m_kernel, m_user
        ; imports, in IAT slot order -- (module index, ordinal)
imports:
        dw      K, 91                   ; 0  InitTask
        dw      K, 30                   ; 1  WaitEvent
        dw      U,  5                   ; 2  InitApp
        dw      U, 200                  ; 3  OpenComm
        dw      U, 207                  ; 4  CloseComm
        dw      U, 205                  ; 5  WriteComm
        dw      U, 204                  ; 6  ReadComm
        dw      U, 206                  ; 7  TransmitCommChar
        dw      U, 203                  ; 8  GetCommError
        dw      U, 215                  ; 9  FlushComm
        dw      U, 208                  ; 10 SetCommEventMask
        dw      U, 209                  ; 11 GetCommEventMask
        dw      U, 214                  ; 12 EscapeCommFunction
NIMP    equ     ($ - imports) / 4

m_kernel:  db   'KERNEL', 0
m_user:    db   'USER', 0

; ── the import address table. mkne.py points a relocation at each slot. ──────
;    Each slot is a 4-byte FAR POINTER initialised to FF FF 00 00 -- offset word
;    0xFFFF is the relocation chain terminator, segment 0 -- byte for byte what
;    TASKMAN.EXE carries at its own call sites.
;
; ⛔⛔⛔ THESE ARE CALLED WITH `call far [cs:slot]`, AND THE `far` IS THE WHOLE
;   POINT. The first cut made each slot a 5-byte `jmp far` stub reached by a NEAR
;   `call`, on the reasoning that "its RETF returns straight to the near caller".
;   It does not. A Win16 API ends in RETF, which pops TWO words; a near call
;   pushed one. The API returned to 0000:<our offset> and wandered off.
;   The evidence fit perfectly once the cause was known: the version that opened
;   its file BEFORE the first thunk call wrote everything up to that call and
;   nothing after it, and the version that called the thunk first wrote nothing
;   at all. I read the first of those as "InitTask returned 0" and reordered the
;   startup around a diagnosis that was mine, not the machine's.
;   ⇒ An indirect far call needs no thunk, costs one relocation per FUNCTION
;     rather than per call site, and cannot get the call kind wrong.
iat:
%rep NIMP
        dw      0FFFFh
        dw      0
%endrep

INITTASK        equ iat + 4*0
WAITEVENT       equ iat + 4*1
INITAPP         equ iat + 4*2
OPENCOMM        equ iat + 4*3
CLOSECOMM       equ iat + 4*4
WRITECOMM       equ iat + 4*5
READCOMM        equ iat + 4*6
TRANSMITCHAR    equ iat + 4*7
GETCOMMERROR    equ iat + 4*8
FLUSHCOMM       equ iat + 4*9
SETEVENTMASK    equ iat + 4*10
GETEVENTMASK    equ iat + 4*11
ESCAPECOMM      equ iat + 4*12

; ── DGROUP (segment 2) offsets. mkne.py zero-fills it; nothing else uses it. ─
D_HINST         equ 0x40
D_HFILE         equ 0x42
D_HEX           equ 0x44                ; four bytes of scratch for whex
D_COM1          equ 0x48                ; the first COM1 open's answer

; ════════════════════════════════════════════════════════════════════════════
start:
; ── InitTask IS THE FIRST INSTRUCTION AFTER THE FRAME TERMINATOR. ────────────
;   Standard Win16 start-up code zeroes BP, pushes it and calls InitTask with
;   nothing in between, and InitTask hands back the register state the loader set up (CX stack size,
;   SI hPrevInstance, DI hInstance, BX:ES command line, DX nCmdShow). Keeping
;   that order costs nothing and matches the only working example we have.
;
;   ⚠ THIS ORDERING WAS ARRIVED AT FOR THE WRONG REASON AND IS KEPT FOR THE RIGHT
;     ONE. When the file still stopped at the first API call, I concluded the
;     intervening INT 21h had clobbered InitTask's input registers, and moved the
;     call first. It made no difference, because the actual fault was the near
;     call into a far-returning stub (see the IAT note above). The reorder was a
;     fix for a defect that did not exist -- kept only because matching TASKMAN
;     is worth doing on its own merits, and now labelled as such rather than
;     left looking like a diagnosis that worked.
        xor     bp, bp
        push    bp                      ; BP=0 terminates the frame chain
        call far [cs:INITTASK]
        mov     bx, ax                  ; InitTask's verdict, kept across the open
        mov     [D_HINST], di           ; ⚠ AFTER InitTask: it RETURNS hInstance in DI

; ── now the report file, through DOS -- see the header note ──────────────────
        push    ds
        push    cs
        pop     ds
        mov     dx, s_file
        xor     cx, cx                  ; normal attributes
        mov     ah, 3Ch                 ; create/truncate
        int     21h
        pop     ds
        jc      .bail                   ; nothing can be reported; just leave
        mov     [D_HFILE], ax

        mov     si, s_head
        call    wcs
        mov     si, s_i
        call    wcs
        mov     ax, bx
        call    whex                    ; InitTask's return value, reported
        call    wnl
        or      bx, bx
        jz      .fin                    ; ⚠ every exit goes through .fin

        xor     ax, ax
        push    ax
        call far [cs:WAITEVENT]
        mov     si, s_w
        call    wcs

        push    word [D_HINST]
        call far [cs:INITAPP]
        or      ax, ax
        jz      .fin
        mov     si, s_a
        call    wcs

; ── the cases ────────────────────────────────────────────────────────────────
        mov     si, s_com1
        mov     dx, d_com1
        call    opencase
        mov     [D_COM1], ax

        mov     si, s_com1b
        mov     dx, d_com1
        call    opencase

; #334: each comm call once on the open port, so the host log's frame dumps say where
; USER.EXE's thunk puts every argument (it adds words of its own in front: OpenComm
; and CloseComm were measured). The answers are reported, but the FRAMES are the point.
        mov     si, s_write
        call    wcs
        push    word [D_COM1]
        push    cs
        mov     ax, d_at
        push    ax
        mov     ax, 3
        push    ax
        call far [cs:WRITECOMM]               ; WriteComm(id, lpBuf, 3)
        call    whex
        call    wnl

        mov     si, s_tx
        call    wcs
        push    word [D_COM1]
        mov     ax, 'X'
        push    ax
        call far [cs:TRANSMITCHAR]            ; TransmitCommChar(id, 'X')
        call    whex
        call    wnl

        mov     si, s_err
        call    wcs
        push    word [D_COM1]
        xor     ax, ax
        push    ax
        push    ax
        call far [cs:GETCOMMERROR]            ; GetCommError(id, NULL)
        call    whex
        call    wnl

        mov     si, s_read
        call    wcs
        push    word [D_COM1]
        push    ds
        mov     ax, D_HEX
        push    ax
        mov     ax, 1
        push    ax
        call far [cs:READCOMM]                ; ReadComm(id, ds:D_HEX, 1)
        call    whex
        call    wnl

        mov     si, s_flush
        call    wcs
        push    word [D_COM1]
        mov     ax, 1
        push    ax
        call far [cs:FLUSHCOMM]               ; FlushComm(id, 1 = receive queue)
        call    whex
        call    wnl

        mov     si, s_setev
        call    wcs
        push    word [D_COM1]
        mov     ax, 1
        push    ax
        call far [cs:SETEVENTMASK]            ; SetCommEventMask(id, EV_RXCHAR) -> DX:AX
        mov     ax, dx                        ; the selector half: 0 = failed
        call    whex
        call    wnl

        mov     si, s_getev
        call    wcs
        push    word [D_COM1]
        mov     ax, 1
        push    ax
        call far [cs:GETEVENTMASK]            ; GetCommEventMask(id, EV_RXCHAR)
        call    whex
        call    wnl

        mov     si, s_esc
        call    wcs
        push    word [D_COM1]
        mov     ax, 5
        push    ax
        call far [cs:ESCAPECOMM]              ; EscapeCommFunction(id, SETDTR=5)
        call    whex
        call    wnl

        mov     si, s_close
        call    wcs
        push    word [D_COM1]
        call far [cs:CLOSECOMM]               ; USER.207
        call    whex
        call    wnl

        mov     si, s_com9
        mov     dx, d_com9
        call    opencase

        mov     si, s_bad
        mov     dx, d_bad
        call    opencase

.fin:
        mov     si, s_end
        call    wcs
        mov     bx, [D_HFILE]
        mov     ah, 3Eh
        int     21h                     ; close
.bail:
        mov     ax, 4C00h
        int     21h                     ; the standard Win16 task exit

; SI = the case label, DX = the CS-relative device name. OpenComm(name, 1024, 128); AX
; is written and returned.
opencase:
        call    wcs
        push    cs
        push    dx
        mov     ax, 1024
        push    ax
        mov     ax, 128
        push    ax
        call far [cs:OPENCOMM]                ; USER.200
        call    whex
        call    wnl
        ret

; ── write helpers. All output is INT 21h AH=40h -- see the header note. ─────
; SI = CS-relative NUL-terminated string.
wcs:
        push    ax
        push    bx
        push    cx
        push    dx
        push    si
        push    ds
        ; ⚠ READ THE HANDLE WHILE DS IS STILL DGROUP. Switching DS to CS first
        ;   and then loading [D_HFILE] reads a byte of our own code instead.
        mov     bx, [D_HFILE]
        xor     cx, cx
        mov     dx, si
.len:   cmp     byte [cs:si], 0
        je      .go
        inc     si
        inc     cx
        jmp     .len
.go:    push    cs
        pop     ds                      ; the string is CS-relative
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
        mov     si, s_nl
        call    wcs
        pop     si
        ret

; AX -> four hex digits, written out of DGROUP scratch.
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
        int     21h                     ; DS is DGROUP here, which is where D_HEX is
        pop     di
        pop     dx
        pop     cx
        pop     bx
        pop     ax
        ret

; ── strings (code segment) ───────────────────────────────────────────────────
; ⚠ ABSOLUTE, AND 8.3-MANGLED. A plain 'W16OUT.TXT' is created successfully --
;   the host log shows `INT21h AH=3c open "W16OUT.TXT" -> AX=5 CF=0` -- but it
;   does NOT land in the launch directory: the WOW command fetch reports
;   cur=[...\demo\win16\w16kern] while the DOS kernel resolves the relative name
;   against its OWN current directory, and the file went somewhere unreachable.
;   ⇒ Open question recorded in docs/inventory/win16.md: whether the DOS CDS
;     should follow the `cur=` the WOW fetch hands us. Until that is answered,
;     the probe names the file outright rather than depending on the answer.
;   krnl386 sees 8.3 names only, which is why this is the mangled form -- the
;   same form the log shows for W16KERN.EXE itself.
s_file:   db 'C:\DOCUME~1\ALLUSE~1\DOCUME~1\NTVDMEX\DEBUG\OUT\W16OUT.TXT', 0
s_head:   db '#PROBE w16comm', 13, 10, 0
s_i:      db 'STEP=I AX=', 0
s_w:      db 'STEP=W', 13, 10, 0
s_a:      db 'STEP=A', 13, 10, 0
s_com1:   db 'CASE=comm.open.com1 SIG=AX AX=', 0
s_com1b:  db 'CASE=comm.open.com1.again SIG=AX AX=', 0
s_close:  db 'CASE=comm.close.first SIG=AX AX=', 0
s_write:  db 'CASE=comm.write SIG=AX AX=', 0
s_tx:     db 'CASE=comm.transmitchar SIG=AX AX=', 0
s_err:    db 'CASE=comm.geterror SIG=AX AX=', 0
s_read:   db 'CASE=comm.read SIG=AX AX=', 0
s_flush:  db 'CASE=comm.flush SIG=AX AX=', 0
s_setev:  db 'CASE=comm.seteventmask SIG=DX DX=', 0
s_getev:  db 'CASE=comm.geteventmask SIG=AX AX=', 0
s_esc:    db 'CASE=comm.escape SIG=AX AX=', 0
d_at:     db 'AT', 13
s_com9:   db 'CASE=comm.open.com9 SIG=AX AX=', 0
s_bad:    db 'CASE=comm.open.notaport SIG=AX AX=', 0
d_com1:   db 'COM1', 0
d_com9:   db 'COM9', 0
d_bad:    db 'XYZ', 0
s_nl:     db 13, 10, 0
s_end:    db '#END', 13, 10, 0

; w_kernel.asm -- the first deterministic Win16 test. Built by tools/ne/mkne.py.
;
; ── WHAT THIS IS FOR ─────────────────────────────────────────────────────────
; Every Win16 check this project has ever run was "launch Notepad and look at
; it". That needs a human, needs a screenshot, and cannot say WHICH of the four
; hundred calls behind the window is wrong. Deterministic Win16 tests have been
; owed since the spec-first directive; what blocked them was that nothing here
; could BUILD a Win16 binary.
;
; ⚠ THIS FIRST ONE DELIBERATELY ASSERTS ALMOST NOTHING. Its job was to prove the
;   chain works end to end -- NE linker, relocations, the import table, the
;   startup handshake, and a file written out of a running Win16 task. Until a
;   file appears, no assertion in a later probe would mean anything.
;   ✅ MEASURED on the bare-metal rig, 2026-09-24:
;       STEP=I AX=0BAF / STEP=W / STEP=A
;       kernel.getversion  AX=5F03
;       kernel.getwinflags AX=4C25
;   ⚠ THOSE TWO VALUES ARE NOT YET VERIFIED -- they are OURS, and nothing has
;     asked another machine. The oracle for them is stock ntvdm on the same rig
;     (docs/research/stock-vdm-dump-oracle), which is the documented authority
;     for everything WOW. A number this probe produces is a MEASUREMENT, not a
;     pass, until that comparison is run.
;
; ── EVERYTHING BELOW WAS READ OFF REAL BINARIES, NOT REMEMBERED ──────────────
; * The startup handshake is TASKMAN.EXE's own __astart, disassembled with
;   tools/ne/nedis.py at seg1:0x04b9, and its three imports resolved from the
;   relocation table: KERNEL.91 InitTask, KERNEL.30 WaitEvent, USER.5 InitApp.
;   The entry-point register contract falls straight out of it:
;       CX = stack size   SI = hPrevInstance   DI = hInstance
;       BX:ES = command line          DX = nCmdShow
; * Every ordinal below was resolved from guest/win16/krnl386.exe's own export
;   tables, not from memory.
; * The exit is INT 21h AH=4Ch, which is the path TASKMAN itself falls back to
;   at seg1:0x04b7.
;
; ⚠ STRINGS LIVE IN THE CODE SEGMENT and are passed as CS:offset far pointers.
;   mkne.py zero-fills the data segment, so there is no initialised data there --
;   and a far pointer into CS is just as good to KERNEL.
;
; ── ⛔⛔ THE REPORT GOES OUT THROUGH INT 21h, NOT THROUGH KERNEL. ─────────────
; The first cut wrote its output with KERNEL's _lcreat/_lwrite/_lclose. It ran --
; the host log shows this task reaching ExitKernelThunk -- and produced NO FILE,
; and there was then no way to tell WHICH of InitTask, InitApp or _lcreat had
; failed, because the thing that would have reported it was the thing under test.
; probe.inc has said this from the beginning: "nothing here uses a BIOS call, on
; purpose: this scaffolding must not itself depend on the layer under test."
; That rule is not about DOS; it is about scaffolding, and I broke it.
;
; So the report goes out through INT 21h AH=3Ch/40h/3Eh, and a PROGRESS LINE is
; written at each step of the startup. Whatever arrives says how far we got:
;       STEP=I AX=nnnn   InitTask returned (AX is its verdict, reported not assumed)
;       STEP=W           WaitEvent returned
;       STEP=A           InitApp returned non-zero
;   then the cases, then #END. A file that stops early names the step that failed,
;   where before there was an empty directory and a guess.
;
; ⚠ AND THE PROGRESS LINES EARNED THEIR KEEP IMMEDIATELY -- but only once the
;   call kind was right. Before that they produced a confident wrong reading (see
;   the IAT note below): the file stopped after the first API call, and I took
;   that as "InitTask returned 0" and restructured the startup around it. The
;   markers were telling the truth; my inference from them was the fiction.
;
; build: see tools/wintest/build.sh

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
        dw      K,  3                   ; 3  GetVersion
        dw      K, 132                  ; 4  GetWinFlags
        dw      K, 83                   ; 5  _lcreat
        dw      K, 86                   ; 6  _lwrite
        dw      K, 81                   ; 7  _lclose
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
GETVERSION      equ iat + 4*3
GETWINFLAGS     equ iat + 4*4
LCREAT          equ iat + 4*5
LWRITE          equ iat + 4*6
LCLOSE          equ iat + 4*7

; ── DGROUP (segment 2) offsets. mkne.py zero-fills it; nothing else uses it. ─
D_HINST         equ 0x40
D_HFILE         equ 0x42
D_HEX           equ 0x44                ; four bytes of scratch for whex

; ════════════════════════════════════════════════════════════════════════════
start:
; ── InitTask IS THE FIRST INSTRUCTION AFTER THE FRAME TERMINATOR. ────────────
;   TASKMAN does `xor bp,bp / push bp / lcall InitTask` with nothing in between,
;   and InitTask hands back the register state the loader set up (CX stack size,
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
        mov     si, s_ver
        call    wcs
        call far [cs:GETVERSION]              ; KERNEL.3
        call    whex
        call    wnl

        mov     si, s_flags
        call    wcs
        call far [cs:GETWINFLAGS]             ; KERNEL.132 -- DX:AX; the bits are in AX
        call    whex
        call    wnl

.fin:
        mov     si, s_end
        call    wcs
        mov     bx, [D_HFILE]
        mov     ah, 3Eh
        int     21h                     ; close
.bail:
        mov     ax, 4C00h
        int     21h                     ; TASKMAN's own fallback exit path

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
s_head:   db '#PROBE w16kernel', 13, 10, 0
s_i:      db 'STEP=I AX=', 0
s_w:      db 'STEP=W', 13, 10, 0
s_a:      db 'STEP=A', 13, 10, 0
s_ver:    db 'CASE=kernel.getversion SIG=AX AX=', 0
s_flags:  db 'CASE=kernel.getwinflags SIG=AX AX=', 0
s_nl:     db 13, 10, 0
s_end:    db '#END', 13, 10, 0

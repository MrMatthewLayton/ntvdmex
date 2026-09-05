; p_redir.com -- the redirection batch: what handle does DOS hand out, and does a
; redirected low handle behave like a file in every respect?  GH #133, #131.
;
; ── WHY THIS PROBE EXISTS ────────────────────────────────────────────────────
; #133 has had THREE fixes aimed at the write end (AH=40h honouring a bound
; handle, AH=02h/OUTC routed through handle 1, lowest-free-handle allocation).
; All three are right and none was sufficient, and the issue closes with:
;
;     "THE ONE FACT STILL MISSING is what AH=3Ch RETURNS.  Instrument that
;      first -- a trace that prints the REQUEST but not the ANSWER is half an
;      instrument.  Do not guess a fourth time."
;
; So this measures it, on real MS-DOS 6.22, instead of reasoning about it.  The
; whole redirection idiom is three facts and this probe takes all three:
;
;   1. what 3Ch returns with the standard handles open        (baseline: 5)
;   2. what 3Ch returns after handle 1 has been CLOSED        <- the fact
;   3. whether a low handle bound to a file then behaves as a file everywhere
;      -- specifically AH=42h lseek, which is how `>>` finds end-of-file
;
; ── THE HAZARD THIS PROBE HAS TO STEP AROUND ─────────────────────────────────
; probe.inc emits through INT 21h AH=02, i.e. through handle 1 -- the very
; handle under test.  Every host runs this probe with stdout ALREADY redirected
; (the oracle appends to A:\OUT.TXT), so from the moment we close handle 1 until
; we put it back, an EMIT would write the dump into the file we are testing and
; the measurement would eat itself.
;
; So the hijacked section takes its captures into memory and emits NOTHING; the
; register blocks are replayed into probe.inc's own slots once stdout is back.
; That is what save9/load9 are for, and why they are worth the twelve bytes.
;
; nasm -f bin p_redir.asm -o p_redir.com

        org     100h
        jmp     start
%include "probe.inc"

; ---- replay the 9-word capture block (__ax..__fl) into and out of a save area.
; probe_capture writes those slots; EMIT reads them.  Between the two we need to
; do a whole redirect dance, so the block is parked and restored.
save9:                                  ; DI = destination
        cld
        push    ds
        pop     es
        mov     si, __ax
        mov     cx, 9
        rep     movsw
        ret
load9:                                  ; SI = source
        cld
        push    ds
        pop     es
        mov     di, __ax
        mov     cx, 9
        rep     movsw
        ret

start:
        PROBE_BEGIN "redir"

        ; ---- 3Ch baseline: create with 0,1,2,3,4 all open.
        ; DOS hands out the LOWEST FREE handle, and with the five standard ones
        ; taken that is 5.  Establishes the allocator's starting point, so case
        ; two below is a delta rather than an isolated number.
        mov     ax, 3C00h
        mov     cx, 0
        mov     dx, f_base
        int     21h
        call    probe_capture
        mov     [h_base], ax
        EMIT    "int21.3c.baseline", "AX,CF"

        mov     bx, [h_base]
        mov     ax, 3E00h
        int     21h

        ; ================= stdout is hijacked from here ======================
        ; Save it FIRST.  45h duplicates handle 1; the copy shares the file
        ; position, so putting it back with 46h later resumes the dump exactly
        ; where it left off rather than at byte 0.
        mov     bx, 1
        mov     ax, 4500h
        int     21h
        call    probe_capture
        mov     [h_save], ax
        mov     di, blk_dup
        call    save9

        ; Close handle 1.  This is the step COMMAND.COM's trace shows and the
        ; textbook idiom does not: no 45h/46h pair around the command, just a
        ; close and a create.
        mov     bx, 1
        mov     ax, 3E00h
        int     21h

        ; ---- THE FACT.  3Ch with handle 1 free: does the new file land in the
        ; slot stdout just vacated?  If it does, redirection needs no dup at all
        ; and every write to handle 1 is already a write to the file.
        mov     ax, 3C00h
        mov     cx, 0
        mov     dx, f_red
        int     21h
        call    probe_capture
        mov     di, blk_create
        call    save9

        ; ---- write through handle 1 BOTH ways, because DOS programs do.
        ; ECHO prints with AH=02h and never goes near AH=40h -- fixing only the
        ; 40h end is one of the three attempts that did not move #133.
        mov     ah, 02h
        mov     dl, 'A'
        int     21h                     ; -> "A"
        mov     ax, 4000h
        mov     bx, 1
        mov     cx, 3
        mov     dx, s_bcd
        int     21h                     ; -> "BCD"

        ; ---- 42h lseek-to-end ON HANDLE 1.  This is how `>>` appends: the
        ; shell opens the target, seeks to the end, and writes.  A host that
        ; treats handles below 5 as "never a file" fails here even when the
        ; create and the write are both right.  DX:AX = the new position, which
        ; after four bytes is 4.
        mov     ax, 4202h
        mov     bx, 1
        mov     cx, 0
        mov     dx, 0
        int     21h
        call    probe_capture
        mov     di, blk_seek
        call    save9

        ; ---- put stdout back: close the file, dup the saved copy onto 1, drop
        ; the copy.  From here EMIT is safe again.
        mov     bx, 1
        mov     ax, 3E00h
        int     21h
        mov     bx, [h_save]
        mov     cx, 1
        mov     ax, 4600h
        int     21h
        mov     bx, [h_save]
        mov     ax, 3E00h
        int     21h
        ; ================= stdout is ours again ==============================

        mov     si, blk_dup
        call    load9
        EMIT    "int21.45.dup.stdout", "CF"

        mov     si, blk_create
        call    load9
        EMIT    "int21.3c.after.close1", "AX,CF"

        mov     si, blk_seek
        call    load9
        EMIT    "int21.42.end.on.h1", "AX,DX,CF"

        ; ---- and the outcome that matters: is the text actually IN the file?
        ; The three failed attempts all left an empty file on disk while the
        ; text went to the screen, so "AX said 1" is not the acceptance test --
        ; four bytes read back off the disk is.
        mov     ax, 3D00h
        mov     dx, f_red
        int     21h
        mov     [h_red], ax
        mov     bx, ax
        mov     ax, 3F00h
        mov     cx, 8
        mov     dx, rbuf
        int     21h
        call    probe_capture
        EMIT    "int21.3f.readback", "AX,CF"
        EMIT_BUF "redir.contents", rbuf, 4

        mov     bx, [h_red]
        mov     ax, 3E00h
        int     21h

        ; ---- self-cleaning, like p_file: nothing is left on the rig or under
        ; DOSBox, neither of which has a snapshot to roll back.
        mov     ax, 4100h
        mov     dx, f_red
        int     21h
        mov     ax, 4100h
        mov     dx, f_base
        int     21h

        PROBE_END

; ------------------------------------------------------------------- data
f_base   db 'RDRBASE.TMP', 0
f_red    db 'RDRTGT.TMP', 0
s_bcd    db 'BCD'
h_base   dw 0
h_save   dw 0
h_red    dw 0
blk_dup     times 9 dw 0
blk_create  times 9 dw 0
blk_seek    times 9 dw 0
rbuf     times 8 db 0

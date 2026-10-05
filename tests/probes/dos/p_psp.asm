; p_psp.com -- the PSP's saved interrupt vectors, and the INT 24h contract.
;
; GH #34 (the error model) and GH #30 (EXEC). When DOS builds a PSP it SAVES the
; current INT 22h (terminate), INT 23h (Ctrl-Break) and INT 24h (critical error)
; vectors into it, at offsets 0x0A, 0x0E and 0x12 -- and restores them from there
; when the program ends. That save/restore is the whole reason a child process
; cannot leave a parent's handlers broken, and it is the contract a program
; installing its own INT 24h relies on.
;
; ── THE TRICK THAT MAKES THIS COMPARABLE ACROSS HOSTS ────────────────────────
; The vectors are ADDRESSES, so their values differ on every host and mean
; nothing in a diff. But there is an invariant that does not: whatever DOS put in
; the PSP must be WHAT THE LIVE VECTOR ACTUALLY IS at the moment the program
; starts. So this reads both and compares them itself, and emits a 1/0 -- which
; is host-independent and is the thing worth asserting.
;
; A zeroed field fails that comparison unless the live vector is also 0:0, so a
; host that simply never fills them in cannot pass by accident.
;
; nasm -f bin p_psp.asm -o p_psp.com

        org     100h
        jmp     start
%include "probe.inc"

; cmpvec -- BX = PSP offset of the saved vector, AL = interrupt number.
; Sets [res] to 1 if the saved far pointer equals the live IVT entry, else 0,
; and parks both so they can be dumped.
cmpvec:
        push    ds
        ; the saved copy, from our own PSP (DS still = PSP at .COM entry, and we
        ; have not changed it)
        mov     si, bx
        mov     cx, [si]                ; offset
        mov     dx, [si + 2]            ; segment
        mov     [sav_off], cx
        mov     [sav_seg], dx
        ; the live vector, from the IVT at 0000:n*4
        xor     ah, ah
        shl     ax, 1
        shl     ax, 1                   ; AX = n * 4
        mov     si, ax
        xor     ax, ax
        mov     ds, ax
        mov     ax, [si]
        mov     bx, [si + 2]
        pop     ds
        mov     [liv_off], ax
        mov     [liv_seg], bx
        mov     byte [res], 0
        cmp     ax, [sav_off]
        jne     .done
        cmp     bx, [sav_seg]
        jne     .done
        mov     byte [res], 1
.done:
        ret

; emit4 -- put the four words where probe_emit will print them, so one EMIT
; carries saved seg:off, live seg:off and the verdict.
emit4:
        mov     ax, [sav_seg]
        mov     [__ax], ax
        mov     ax, [sav_off]
        mov     [__bx], ax
        mov     ax, [liv_seg]
        mov     [__cx], ax
        mov     ax, [liv_off]
        mov     [__dx], ax
        xor     ax, ax
        mov     al, [res]
        mov     [__si], ax
        mov     word [__fl], 0
        ret

start:
        PROBE_BEGIN "psp"

        ; ---- PSP+0x02: the segment just past this program's memory. Zero here
        ; means the program cannot tell how much memory it owns, which is what a
        ; .COM's own stack setup and every "shrink to fit" idiom reads.
        mov     ax, [0002h]
        mov     [__ax], ax
        mov     word [__fl], 0
        EMIT    "psp.02.memtop", "AX"

        ; ---- PSP+0x0A: INT 22h, the terminate address.
        mov     bx, 000Ah
        mov     al, 22h
        call    cmpvec
        call    emit4
        EMIT    "psp.0A.int22", "SI"

        ; ---- PSP+0x0E: INT 23h, Ctrl-Break.
        mov     bx, 000Eh
        mov     al, 23h
        call    cmpvec
        call    emit4
        EMIT    "psp.0E.int23", "SI"

        ; ---- ★ PSP+0x12: INT 24h, the critical-error handler. The one GH #34
        ; is about. SI=1 means the PSP's copy matches the live vector, i.e. DOS
        ; really did save it and a program can rely on getting it back.
        mov     bx, 0012h
        mov     al, 24h
        call    cmpvec
        call    emit4
        EMIT    "psp.12.int24", "SI"

        ; ---- and is there a handler there AT ALL? A live INT 24h of 0000:0000
        ; means a critical error would jump into the IVT. Emitting the segment
        ; separately because "saved correctly" and "points at something" are two
        ; different questions and a zeroed host passes the first trivially.
        mov     ax, [liv_seg]
        mov     [__ax], ax
        mov     ax, [liv_off]
        mov     [__bx], ax
        mov     word [__fl], 0
        EMIT    "psp.int24.live", "AX,BX"

        ; ---- PSP+0x16: the parent's PSP segment. Host-specific, but a zero
        ; says nobody owns us, which no real DOS reports.
        mov     ax, [0016h]
        mov     [__ax], ax
        mov     word [__fl], 0
        EMIT    "psp.16.parent.nonzero", ""

        ; ---- the two byte patterns every PSP carries, as a sanity anchor that
        ; we are reading a PSP at all and not some other page.
        EMIT_BUF "psp.00.int20", 0000h, 2
        EMIT_BUF "psp.50.dispatch", 0050h, 3

        PROBE_END

sav_off  dw 0
sav_seg  dw 0
liv_off  dw 0
liv_seg  dw 0
res      db 0

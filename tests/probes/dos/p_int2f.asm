; p_int2f.com -- the DOS-internal INT 2Fh services COMMAND.COM needs at startup.
;                docs/research/xp-command-com.md.
;
; ── WHAT THIS IS FOR ─────────────────────────────────────────────────────────
; XP's own COMMAND.COM loads and runs under NTVDMEX, gets through XMS detection
; and the COMMAND.COM install check, then makes five INT 2Fh AX=122Eh calls and
; terminates without printing. Read off its own code (file offsets 0x7d2b-0x7d85):
;
;       xor cx,cx / mov es,cx / xor di,di    ; ES:DI = 0000:0000 going in
;       mov ax,0x122e
;       mov dl,0x0                           ; DL selects the table: 0,2,4,6,8
;       int 0x2f
;       mov word [0x917a],es                 ; ...and it STORES what comes back
;       mov [0x9178],di
;
; So AX=122Eh/DL=n is a GET returning a far pointer. Our INT 2Fh handler passes
; unrecognised calls straight through, so the guest reads back the 0000:0000 it
; supplied, stores five null pointers, and dies when it first needs one.
;
; ⚠ THIS PROBE DOES EXACTLY WHAT COMMAND.COM DOES, byte for byte -- same AX, same
;   DL values, ES:DI zeroed first. That is deliberate: reproducing the caller we
;   are trying to serve is the only way to be sure the answer we measure is the
;   answer it would get. It also makes the call unambiguously a GET, which is
;   what makes it safe (see below).
;
; ── SAFETY ───────────────────────────────────────────────────────────────────
; ⚠ 122Eh is documented in places as GET **or SET**, and a probe that guessed
;   wrong could install null tables into a live DOS. It does not guess: it issues
;   precisely the register state COMMAND.COM issues at startup on every machine
;   that boots, so whatever that does is already happening on every boot.
; * Nothing else here writes anything. Every other case is a pure query.
; * The pointers are DEREFERENCED to dump the first bytes of each table, which is
;   the thing we actually need -- but only when non-null, and only 16 bytes.
;
; ORACLE-ALSO: pcem
; nasm -f bin p_int2f.asm -o p_int2f.com

        org     100h
        jmp     start
%include "probe.inc"

; ---------------------------------------------------------------- state
seg0    dw      0
off0    dw      0
tbl     times 32 db 0
seen    dw      0
p0      dd      0
p2      dd      0
p4      dd      0
p6      dd      0
p8      dd      0

; Ask AX=122Eh for the table selected by DL (in AL on entry here).
; Returns ES:DI exactly as the service left it.
ask:
        push    ax
        xor     cx, cx
        mov     es, cx
        xor     di, di                  ; ⚠ ZEROED FIRST, as COMMAND.COM does
        pop     dx                      ; DL = the selector
        xor     dh, dh
        mov     ax, 122Eh
        int     2Fh
        ret

start:
        PROBE_BEGIN "int2f"

; ════════════════════════════════════════════════════════════════════════════
; A..E. ★★★ THE FIVE TABLE POINTERS COMMAND.COM ASKS FOR.
;
;    DL = 0, 2, 4, 6, 8. Emits the far pointer as AX=segment, and separately the
;    offset, because a model that returns a plausible segment with a junk offset
;    is the "runs but lies" shape and one word would hide it.
;    ⇒ 0000:0000 from every one of them is our current answer, and is what kills
;      COMMAND.COM.
; ════════════════════════════════════════════════════════════════════════════
; ⛔⛔ THE RAW POINTERS ARE NOT EMITTED, AND THAT IS THE WHOLE POINT OF THIS
;   SECTION. They are ADDRESSES: there is no reason ours should equal another
;   machine's, and the two oracles do not agree with each other either (DL=8 is
;   03E7 on 6.22 and 03EA on PCem -- same offset, different segment, because the
;   tables sit wherever that build put them). Emitting them produced SEVEN rows
;   that read MISMATCH for ever and could never be anything else -- which is
;   precisely the "score excludes what stopped asking" damage in reverse: seven
;   permanent false failures parked in the parity number.
;   ⇒ What IS machine-independent is the RELATIONSHIPS, so those are the cases:
;       * which selectors answer with a NON-NULL pointer (a bitmask)
;       * whether DL=0 and DL=4 hand back the SAME pointer
;     Both are facts about the service. Neither depends on where DOS was loaded.
;   The addresses are still visible when they are wanted -- the host logs them
;   (STAGE2: 2F/122E) and `seen` below proves each one dereferenced.

%macro ASK_PTR 2
        mov     al, %1
        call    ask
        mov     [%2], es
        mov     [%2+2], di
%endmacro

        ASK_PTR 0, p0
        ASK_PTR 2, p2
        ASK_PTR 4, p4
        ASK_PTR 6, p6
        ASK_PTR 8, p8

; ── bitmask of "answered with a non-null pointer": bit0=DL0 .. bit4=DL8.
;    Real DOS: 0x17 (DL0, DL2, DL4, DL8 set; DL6 clear).
        xor     bx, bx
%macro NONNULL_BIT 2
        mov     ax, [%1]
        or      ax, [%1+2]
        jz      %%skip
        or      bx, %2
%%skip:
%endmacro
        NONNULL_BIT p0, 1
        NONNULL_BIT p2, 2
        NONNULL_BIT p4, 4
        NONNULL_BIT p6, 8
        NONNULL_BIT p8, 16
        mov     ax, bx
        POISON
        call    probe_capture
        EMIT    "int2f.122e.nonnull", "AX"

; ── DL=0 and DL=4 return the SAME pointer on both real kernels. 1 = same.
        mov     ax, 0
        mov     bx, [p0]
        cmp     bx, [p4]
        jne     .ne04
        mov     bx, [p0+2]
        cmp     bx, [p4+2]
        jne     .ne04
        mov     ax, 1
.ne04:
        POISON
        call    probe_capture
        EMIT    "int2f.122e.dl0eq4", "AX"

; ════════════════════════════════════════════════════════════════════════════
; F. ★★ WHAT IS ACTUALLY IN THE TABLES.
;
;    The pointer alone does not say what shape the thing is, and the shape is
;    what we have to reproduce. Dumps 32 bytes from each non-null table.
;
;    ⛔⛔ AND EACH DUMP IS PRECEDED BY A "DID WE DUMP" FLAG, WHICH IS NOT
;      DECORATION. The first cut emitted only the bytes, with a guard that left
;      the buffer zeroed when the pointer was null -- so OUR null answer emitted
;      sixteen zeros, the real machines emitted sixteen zeros they had actually
;      read, and the row came back AGREE. A manufactured agreement out of an
;      absent answer: the same shape as the floppy DOR mask, twice in one day.
;      The flag makes "did not look" and "looked, and it was zeros" different
;      values.
; ════════════════════════════════════════════════════════════════════════════
%macro DUMP_CASE 3
        mov     al, %1
        call    ask
        mov     word [seen], 0
        mov     cx, 32
        mov     di, tbl
        push    es
        pop     ax
        push    ax
        or      ax, di
        pop     ax
        jz      %%zero
        ; non-null: copy 32 bytes from ES:DI(table) to CS:tbl
        mov     si, di
        mov     di, tbl
        push    ds
        push    es
        pop     ds
        push    cs
        pop     es
        rep     movsb
        pop     ds
        mov     word [seen], 1
        jmp     %%emit
%%zero:
        ; null pointer: leave tbl as it was and say so
        push    cs
        pop     es
        mov     di, tbl
        xor     al, al
        rep     stosb
%%emit:
        mov     ax, [seen]
        POISON
        call    probe_capture
        EMIT    %2, "AX"
        EMIT_BUF %3, tbl, 32
%endmacro

        DUMP_CASE 0, "int2f.122e.dl0.seen", "int2f.122e.dl0.bytes"
        DUMP_CASE 2, "int2f.122e.dl2.seen", "int2f.122e.dl2.bytes"
        DUMP_CASE 4, "int2f.122e.dl4.seen", "int2f.122e.dl4.bytes"
        DUMP_CASE 8, "int2f.122e.dl8.seen", "int2f.122e.dl8.bytes"

; ════════════════════════════════════════════════════════════════════════════
; G. THE COMMAND.COM INTERFACE CHECK, AX=5501h.
;
;    COMMAND.COM issues this once during the same startup, before the table
;    calls. Asked here so the whole startup conversation is on one page.
;    Input registers are left as COMMAND.COM leaves them (BX=0044h was measured);
;    emitted as AX so a host that answers is distinguishable from one that does
;    not touch it.
; ════════════════════════════════════════════════════════════════════════════
        mov     ax, 5501h
        mov     bx, 0044h
        int     2Fh
        POISON
        call    probe_capture
        EMIT    "int2f.5501", "AX"

        PROBE_END

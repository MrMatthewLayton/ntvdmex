; p_sysvar.com -- INT 21h AH=52h: dump the List of Lists and follow every chain
; it points at.  GH #48, and the likely cause of GH #47 (MEM.EXE lies).
;
; AH=52h returns ES:BX pointing INTO a structure whose useful part starts two
; bytes EARLIER: the word at ES:BX-2 is the first MCB segment, which is the field
; a memory walker actually follows.  Everything after ES:BX is DOS's own
; bookkeeping -- the DPB chain, the system file table, the CDS array and the
; device driver chain.
;
; ── WHY A DUMP AND NOT A READING OF THE DOCUMENTATION ────────────────────────
; #48 says it in as many words: "The structures are version-specific and must not
; be written from documentation alone -- the layout dumps have twice caught
; errors that a plausible reading would have missed."  So this takes the bytes.
;
; ⚠ The pointers INSIDE are host-specific addresses and mean nothing across
;   hosts, so no case here puts ES/BX in its signature.  What is comparable is
;   the SHAPE: which offsets hold far pointers, which hold counts, how long the
;   NUL device header is, and what the first DPB and SFT actually contain.
;
; nasm -f bin p_sysvar.asm -o p_sysvar.com

        org     100h
        jmp     start
%include "probe.inc"

; farcopy -- copy CX bytes from the far pointer in [fptr] to DS:DI.
; A chain terminator is FFFFh in the segment (or the offset), and following one
; walks off into nonsense; a probe that dumped that would report noise as
; structure.  So a terminated pointer fills the destination with EE instead,
; which is unmistakable in the dump and cannot be confused with real data.
farcopy:
        cld
        push    ds
        pop     es                      ; destination is always our own segment
        cmp     word [fptr + 2], 0FFFFh
        je      .none
        ; ⚠ AND A NULL POINTER IS A TERMINATOR TOO. The first rig run followed a
        ; 0000:0000 DPB pointer and dumped the INTERRUPT VECTOR TABLE -- 57A300F0
        ; ... -- which reads exactly like a populated structure. An absent chain
        ; must be visibly absent, not plausibly full.
        cmp     word [fptr + 2], 0
        jne     .go
        cmp     word [fptr], 0
        je      .none
.go:
        push    ds
        lds     si, [fptr]
        rep     movsb
        pop     ds
        ret
.none:
        mov     al, 0EEh
        rep     stosb
        ret

; setptr -- load the far pointer stored at DS:SI into [fptr].
setptr:
        mov     ax, [si]
        mov     [fptr], ax
        mov     ax, [si + 2]
        mov     [fptr + 2], ax
        ret

start:
        PROBE_BEGIN "sysvar"

        ; ---- 52h itself.  ES:BX is a host address, so only CF is significant;
        ; the answer this probe is really after is in the buffers below.
        POISON
        mov     ax, 5200h
        int     21h
        call    probe_capture
        EMIT    "int21.52", "CF"

        ; ---- the block itself, from ES:BX-2 forward.  Two bytes of MCB head
        ; plus 0x40 of SysVars covers every field a DOS 4+ layout defines, with
        ; room to see where it stops being meaningful.
        cld
        push    ds
        pop     es
        mov     ax, [__es]
        mov     si, [__bx]
        sub     si, 2                   ; the first MCB word lives BEFORE the pointer
        mov     di, sbuf
        mov     cx, 42h
        push    ds
        mov     ds, ax
        rep     movsb
        pop     ds
        EMIT_BUF "sysvars.raw", sbuf, 42h

        ; ---- the DPB chain.  sbuf+0 is the MCB word, so SysVars+0 is sbuf+2.
        ; A drive parameter block per drive, linked; MEM and CHKDSK walk it.
        mov     si, sbuf + 2
        call    setptr
        mov     di, dbuf
        mov     cx, 30h
        call    farcopy
        EMIT_BUF "sysvars.dpb0", dbuf, 30h

        ; ---- the system file table.  SysVars+4.  Header is a far link plus a
        ; count, then that many file entries; SHARE and debuggers walk it.
        mov     si, sbuf + 6
        call    setptr
        mov     di, fbuf
        mov     cx, 30h
        call    farcopy
        EMIT_BUF "sysvars.sft0", fbuf, 30h

        ; ---- the CDS array (current directory structure), SysVars+16h: one
        ; entry per drive, each carrying that drive's current path.
        mov     si, sbuf + 18h
        call    setptr
        mov     di, cbuf
        mov     cx, 60h
        call    farcopy
        EMIT_BUF "sysvars.cds0", cbuf, 60h

        ; ---- and the entry for C:, index 2, which is the drive that actually
        ; EXISTS. Entry 0 is A: and is the absent case on most machines, so
        ; dumping only that proves the zeroed path and never the populated one.
        ; 2 * 88 = 176 = 0B0h into the array.
        mov     si, sbuf + 18h
        call    setptr
        add     word [fptr], 0B0h
        mov     di, cbuf
        mov     cx, 58h
        call    farcopy
        EMIT_BUF "sysvars.cds2", cbuf, 58h

        ; ---- the device driver chain.  The NUL device header is INLINE in
        ; SysVars (not a pointer to one), so this is a dump of where it starts
        ; and the name field that identifies it -- the check that we have the
        ; offset right at all.
        EMIT_BUF "sysvars.nul", sbuf + 24h, 12h

        PROBE_END

fptr     dd 0
sbuf     times 42h db 0
dbuf     times 30h db 0
fbuf     times 30h db 0
cbuf     times 60h db 0

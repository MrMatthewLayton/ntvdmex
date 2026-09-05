; p_ovl.com -- INT 21h AH=4Bh AL=01 (load without execute) and AL=03 (overlay).
;
; GH #50. Both are loud-unimplemented on NTVDMEX; this measures what DOS does.
;
; ── IT BUILDS ITS OWN TEST EXECUTABLE ────────────────────────────────────────
; An overlay test needs a second file, and a probe that depends on one is a probe
; that only runs where someone remembered to stage it. So this WRITES a 36-byte
; .EXE out of bytes embedded below, loads it, and deletes it -- self-contained on
; every host in the panel, exactly like p_file creates its own scratch file.
;
; The test executable is the smallest thing that can prove a relocation was
; applied: four bytes of load module, of which one WORD is a relocation target.
;
;     image+0  CB        retf          -- so it can be far-called if wanted
;     image+1  90        nop
;     image+2  00 00     <- the relocation
;
; After an AL=03 overlay load at segment S with relocation factor F, the word at
; S:0002 must read F. That is the whole of the overlay contract in one number,
; and it distinguishes the three ways to get it wrong -- not relocating at all
; (0), relocating by the load segment instead of the factor (S), or relocating
; twice (S+F).
;
; nasm -f bin p_ovl.asm -o p_ovl.com

        org     100h
        jmp     start
%include "probe.inc"

start:
        PROBE_BEGIN "ovl"

        ; ---- write the test .EXE. A .COM owns all of memory, so shrink first;
        ; otherwise 48h below has nothing to hand out.
        mov     ax, 4A00h
        mov     bx, 1000h                       ; keep 64K, release the rest
        push    ds
        pop     es                              ; ES = our PSP
        int     21h
        call    probe_capture
        EMIT    "int21.4A.shrink", "CF"

        mov     ax, 3C00h
        mov     cx, 0
        mov     dx, ovlname
        int     21h
        mov     [fh], ax
        mov     bx, ax
        mov     ax, 4000h
        mov     cx, exeend - exeimg
        mov     dx, exeimg
        int     21h
        call    probe_capture
        EMIT    "int21.40.wrote.exe", "AX,CF"
        mov     bx, [fh]
        mov     ax, 3E00h
        int     21h

        ; ---- a segment to load the overlay INTO, which the caller supplies.
        mov     ax, 4800h
        mov     bx, 20h                         ; 512 bytes, plenty for 4
        int     21h
        call    probe_capture
        mov     [ovlseg], ax
        EMIT    "int21.48.alloc", "CF"

        ; ---- ★ AL=03: THE OVERLAY LOAD. Parameter block is only two words --
        ; the load segment and the relocation factor -- and there is no PSP, no
        ; memory allocation and no transfer of control. Deliberately passing a
        ; relocation factor that is NOT the load segment, so a host that
        ; relocates by the wrong one is caught rather than accidentally right.
        mov     ax, [ovlseg]
        mov     [ovlpb], ax                     ; +0 load segment
        mov     word [ovlpb + 2], 1234h         ; +2 relocation factor
        push    ds
        pop     es
        mov     bx, ovlpb
        mov     dx, ovlname
        mov     ax, 4B03h
        int     21h
        call    probe_capture
        EMIT    "int21.4B03.overlay", "CF"

        ; ---- read the relocated word back out of the overlay.
        ; AX = 1234h means the factor was applied. 0 = never relocated,
        ; the load segment = relocated by the wrong value.
        push    ds
        mov     ds, [cs:ovlseg]
        mov     ax, [0002h]
        pop     ds
        mov     [__ax], ax
        mov     ax, [ovlseg]
        mov     [__bx], ax                      ; for context: where it loaded
        mov     word [__fl], 0
        EMIT    "ovl.relocated.word", "AX"

        ; ---- and the first two bytes, to prove the image arrived at all rather
        ; than the word being right by accident in untouched memory.
        push    ds
        mov     ds, [cs:ovlseg]
        mov     ax, [0000h]
        pop     ds
        mov     [__ax], ax
        mov     word [__fl], 0
        EMIT    "ovl.image.head", "AX"          ; expect 90CB

        ; ---- AL=01: load WITHOUT executing. DOS builds the PSP, loads the
        ; image, and hands the entry point back in the parameter block at +0x0E
        ; (SS:SP) and +0x12 (CS:IP) -- then returns instead of transferring.
        ; The fields are poisoned first so "DOS wrote nothing" and "DOS wrote
        ; zero" cannot be confused.
        mov     word [pb01 + 0], 0              ; env: inherit
        mov     word [pb01 + 2], tailz
        mov     [pb01 + 4], ds
        mov     word [pb01 + 6], 5Ch
        mov     [pb01 + 8], ds
        mov     word [pb01 + 10], 6Ch
        mov     [pb01 + 12], ds
        mov     word [pb01 + 14], 0EEEEh        ; +0x0E SS:SP, poisoned
        mov     word [pb01 + 16], 0EEEEh
        mov     word [pb01 + 18], 0EEEEh        ; +0x12 CS:IP, poisoned
        mov     word [pb01 + 20], 0EEEEh
        push    ds
        pop     es
        mov     bx, pb01
        mov     dx, ovlname
        mov     ax, 4B01h
        int     21h
        call    probe_capture
        EMIT    "int21.4B01.loadonly", "CF"

        ; the returned CS:IP. IP comes from the header (0), CS is the load
        ; segment, so the significant question is whether CS is a real segment
        ; rather than the poison.
        mov     ax, [pb01 + 20]                 ; CS
        mov     [__ax], ax
        mov     ax, [pb01 + 18]                 ; IP
        mov     [__bx], ax
        mov     ax, [pb01 + 16]                 ; SS
        mov     [__cx], ax
        mov     ax, [pb01 + 14]                 ; SP
        mov     [__dx], ax
        mov     word [__fl], 0
        EMIT    "ovl.4B01.entry", "BX,DX"

        ; ---- clean up after ourselves.
        mov     ax, 4100h
        mov     dx, ovlname
        int     21h

        PROBE_END

; ------------------------------------------------------------------- data
ovlname  db 'ZZOVL.EXE', 0
tailz    db 0, 0Dh
fh       dw 0
ovlseg   dw 0
ovlpb    times 4 db 0
pb01     times 22 db 0

; The 36-byte test executable: a 32-byte header (2 paragraphs) declaring ONE
; relocation at image offset 2, then four bytes of load module.
exeimg:
        db      'M', 'Z'
        dw      36                              ; e_cblp  bytes in last page
        dw      1                               ; e_cp    pages
        dw      1                               ; e_crlc  relocation count
        dw      2                               ; e_cparhdr  header paragraphs
        dw      0                               ; e_minalloc
        dw      0FFFFh                          ; e_maxalloc
        dw      0                               ; e_ss
        dw      100h                            ; e_sp
        dw      0                               ; e_csum
        dw      0                               ; e_ip
        dw      0                               ; e_cs
        dw      1Ch                             ; e_lfarlc  relocation table
        dw      0                               ; e_ovno
        dw      2                               ; reloc[0] offset
        dw      0                               ; reloc[0] segment
        db      0CBh, 90h                       ; retf ; nop
        dw      0                               ; <- the relocation target
exeend:

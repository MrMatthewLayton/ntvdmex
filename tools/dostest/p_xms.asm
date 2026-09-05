; p_xms.com -- the XMS driver, called the way MEM.EXE calls it.  GH #47.
;
; MEM reports "Extended (XMS) 0K" on NTVDMEX against 15,232K on the 6.22 oracle,
; and the host trace says where it stops:
;
;     BOP2F ax=4300      is an XMS driver installed?
;     BOP2F ax=4310      give me its entry point
;     XMS AH=0x00                      <- version
;     XMS AH=0x00                      <- version, again
;     ...and nothing. AH=08h, query free extended memory, is NEVER CALLED.
;
; So MEM asks the driver who it is, does not like the answer, and gives up before
; asking how much memory there is. This probe makes exactly those calls and dumps
; exactly those answers, on both hosts, so the difference is a diff rather than a
; hypothesis. The obvious suspect is DX -- the HMA flag, which we report as 0 --
; but "obvious suspect" is how the last three MEM theories started, so it is
; MEASURED here and not assumed.
;
; nasm -f bin p_xms.asm -o p_xms.com

        org     100h
        jmp     start
%include "probe.inc"

start:
        PROBE_BEGIN "xms"

        ; ---- INT 2Fh AX=4300: is an XMS driver installed? AL=80h means yes.
        POISON
        mov     ax, 4300h
        int     2Fh
        call    probe_capture
        EMIT    "int2F.4300.installed", "AX"

        ; ---- INT 2Fh AX=4310: the driver's entry point comes back in ES:BX.
        ; The ADDRESS is host-specific, so what matters in the diff is that it is
        ; non-zero -- a null entry means the far call below lands in the IVT.
        mov     ax, 4310h
        int     2Fh
        call    probe_capture
        mov     [xent], bx
        mov     [xent + 2], es
        mov     ax, es
        mov     [__ax], ax
        mov     word [__fl], 0
        EMIT    "int2F.4310.entry.seg.nonzero", ""

        ; ---- AH=00h: get version. THE CALL MEM MAKES TWICE AND THEN STOPS ON.
        ;   AX = XMS version in BCD (0300h = 3.0)
        ;   BX = driver internal revision
        ;   DX = 1 if the HMA exists, 0 if not      <- the prime suspect
        POISON
        mov     ah, 00h
        call    far [xent]
        call    probe_capture
        EMIT    "xms.00.version", "AX,BX,DX"

        ; ---- AH=08h: query free extended memory. MEM never gets here on
        ; NTVDMEX; on the oracle its answer is what fills the report in.
        ;   AX = largest free block in KB
        ;   DX = total free in KB
        ;   BL = 0 on success
        POISON
        mov     ah, 08h
        mov     bl, 0
        call    far [xent]
        call    probe_capture
        EMIT    "xms.08.queryfree", "AX,DX,BX"

        ; ---- AH=01h: request the HMA. Whether it is available is exactly what
        ; DX above claims, so asking closes the loop: a host that says DX=0 must
        ; also refuse here, and one that says DX=1 must grant it.
        POISON
        mov     ah, 01h
        mov     dx, 0FFFFh                      ; DX=FFFF: an application asking
        call    far [xent]
        call    probe_capture
        EMIT    "xms.01.request.hma", "AX,BX"

        ; ---- AH=07h: is the A20 line enabled? A driver that reports no HMA and
        ; no A20 control is one MEM may reasonably ignore.
        POISON
        mov     ah, 07h
        call    far [xent]
        call    probe_capture
        EMIT    "xms.07.query.a20", "AX,BX"

        ; ---- ★ THE BYTES INSIDE THE DRIVER, AT THE ENTRY POINT. (GH #47)
        ; MEM.EXE does not only CALL the XMS driver, it READS IT. Disassembled:
        ;     07B1  les bx,[0x2cdc]          ; ES:BX = the XMS entry point
        ;     07B5  cmp word [es:bx+0x45],0
        ;     07BA  jnz  0x7bf
        ;     07BC  jmp  0x907               ; skip the whole extended report
        ; So a zero word at entry+0x45 makes MEM print "Extended (XMS) 0K" no
        ; matter how correctly the driver answers AH=08h -- which is exactly what
        ; we do, because our entry is a four-byte BOP/RETF stub with zeros after.
        ; This dumps what a real HIMEM.SYS has there so the value is measured
        ; rather than invented.
        push    ds
        mov     ds, [cs:xent + 2]
        mov     si, [cs:xent]
        add     si, 40h
        push    cs
        pop     es
        mov     di, ebuf
        mov     cx, 20h
        cld
        rep     movsb
        pop     ds
        EMIT_BUF "xms.entry.plus40", ebuf, 20h

        PROBE_END

xent     dd 0
ebuf     times 20h db 0

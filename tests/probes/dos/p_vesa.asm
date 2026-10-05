; p_vesa.com -- the VBE controller block and EVERY ModeInfoBlock, byte for byte.
;
; ── WHY THIS EXISTS (s74b) ──────────────────────────────────────────────────────
; Every VESA defect found in s74 came from reading the VBE 2.0 PDF against code
; written from memory: the OEM string overrunning a 256-byte block (Heretic's MCB),
; NumberOfBanks, the +28..+30 off-by-one, 4F05's BH/BL swap.  A spec audit catches
; what the spec states; it cannot catch what every real BIOS does that the spec
; leaves open.  This probe dumps what a REAL VBE BIOS answers so the two can be
; diffed, the way p_video/p_dos diff us against MS-DOS 6.22.
;
; ⚠ The oracle is a video BIOS, not MS-DOS.  Rows are expected to DIFFER where the
;   card differs -- TotalMemory, the mode list, PhysBasePtr -- and the diff is read
;   for SHAPE (field layout, flags, which fields are filled) rather than equality.
;   Pointers into the BIOS (OemStringPtr, VideoModePtr, WinFuncPtr, PhysBasePtr)
;   are masked to zero before emitting; they can never agree between hosts.
;
; Rows:
;   int10.4F00.vbe            AX after 4F00 with "VBE2" preset
;   vesa.info                 VbeInfoBlock bytes 0..33 with the two pointers zeroed
;   vesa.modes                the mode list (words, up to 48, then FFFF)
;   int10.4F01.<mode>         AX for that mode
;   vesa.mi.<mode>            ModeInfoBlock bytes 0..49, WinFuncPtr/PhysBasePtr zeroed
;
; ORACLE-ALSO: pcem-vesa
;   (paritysweep.sh reads this: the Tseng ET4000/W32p ROM under PCem votes alongside
;    QEMU's Bochs VBE, so a split between a synthetic VBE and a real one is DISPUTED
;    rather than a verdict.)
;
; nasm -f bin p_vesa.asm -o p_vesa.com

        org     100h
        jmp     start
%include "probe.inc"

start:
        PROBE_BEGIN "vesa"

        ; ---- 4F00 with the VBE 2.0 request: "VBE2" in the signature first.
        push    ds
        pop     es
        mov     di, infob
        mov     byte [di], 'V'
        mov     byte [di+1], 'B'
        mov     byte [di+2], 'E'
        mov     byte [di+3], '2'
        mov     ax, 4F00h
        int     10h
        call    probe_capture
        EMIT    "int10.4F00.vbe", "AX"

        ; copy the mode list out BEFORE masking the pointer that names it
        push    ds
        lds     si, [infob+14]                  ; VideoModePtr (far)
        mov     di, modes
        mov     cx, 48
.ml:    lodsw
        mov     [es:di], ax
        add     di, 2
        cmp     ax, 0FFFFh
        je      .mld
        loop    .ml
        mov     word [es:di], 0FFFFh            ; cap: terminate ourselves
.mld:   pop     ds

        mov     dword [infob+6], 0              ; OemStringPtr
        mov     dword [infob+14], 0             ; VideoModePtr
        mov     dword [infob+22], 0             ; OemVendorNamePtr   (VBE2)
        mov     dword [infob+26], 0             ; OemProductNamePtr  (VBE2)
        mov     dword [infob+30], 0             ; OemProductRevPtr   (VBE2)
        EMIT_BUF "vesa.info", infob, 34
        EMIT_BUF "vesa.modes", modes, 98

        ; ---- every mode in the list: 4F01, AX and the block
        mov     si, modes
.each:
        mov     ax, [si]
        cmp     ax, 0FFFFh
        je      .done
        mov     [curmode], ax
        push    si
        call    setname                         ; "int10.4F01.XXXX" / "vesa.mi.XXXX"
        mov     di, mib
        mov     cx, 256
        mov     al, 0
        push    di
        rep     stosb
        pop     di
        mov     cx, [curmode]
        mov     ax, 4F01h
        int     10h
        call    probe_capture
        mov     word [__casep], name1
        mov     word [__sigp], sigax
        call    probe_emit
        mov     dword [mib+12], 0               ; WinFuncPtr
        mov     dword [mib+40], 0               ; PhysBasePtr
        mov     word [__casep], name2
        mov     si, mib
        mov     cx, 50
        call    probe_emit_buf
        pop     si
        add     si, 2
        jmp     .each
.done:
        PROBE_END

; setname -- write [curmode] as four hex digits into both dynamic names
setname:
        mov     ax, [curmode]
        mov     di, name1+11
        call    hex4
        mov     ax, [curmode]
        mov     di, name2+8
        call    hex4
        ret
hex4:                                           ; AX -> 4 ASCII hex at DS:DI
        mov     cx, 4
.h:     rol     ax, 4
        push    ax
        and     al, 0Fh
        add     al, '0'
        cmp     al, '9'
        jbe     .d
        add     al, 'A'-'0'-10
.d:     mov     [di], al
        inc     di
        pop     ax
        loop    .h
        ret

sigax:  db      "AX", 0
name1:  db      "int10.4F01.", "0000", 0
name2:  db      "vesa.mi.", "0000", 0
curmode: dw     0
modes:  times   50 dw 0
infob:  times   512 db 0
mib:    times   256 db 0

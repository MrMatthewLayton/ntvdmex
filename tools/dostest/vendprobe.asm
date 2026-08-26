; vendprobe.asm -- what does a DPMI host return for INT 2Fh 168A "MS-DOS"?  GH #128.
;
; krnl386 asks for the vendor-specific API entry point with DS:SI -> "MS-DOS" and
; refuses to run without it:
;       d6e1  mov ax,168Ah / mov si,0x172a / int 2Fh
;       d6e9  cmp al,0x8a / jz 0xd71b        <- 0xd71b prints "Inadequate DPMI Server"
; NTVDMEX answers by leaving AL alone, which IS 0x8A, which IS the abort.  So: what
; does the real one say?  Run under STOCK ntvdm and read it off.
;
; It asks TWICE, because the answer may differ by mode and krnl386 only ever asks
; from protected mode:
;   1. in real mode / V86, straight away
;   2. after using INT 2Fh 1687 to switch itself to 16-bit protected mode, which is
;      exactly what krnl386 does before asking
BITS 16
ORG 0x100

start:      mov dx,m_rm
            mov ah,9
            int 0x21
            mov ax,0x168a
            mov si,vendor
            xor di,di
            mov es,di
            int 0x2f
            call report

            ; ---- find the DPMI host ------------------------------------------
            mov dx,m_chk
            mov ah,9
            int 0x21
            mov ax,0x1687
            int 0x2f
            or ax,ax
            jz .have
            mov dx,m_nodpmi
            mov ah,9
            int 0x21
            jmp done
.have:      mov [sw_off],di
            mov [sw_seg],es
            mov [paras],si
            mov ax,cx                       ; CL = processor type
            call hex16
            mov dx,m_paras
            mov ah,9
            int 0x21
            mov ax,[paras]
            call hex16
            call crlf

            ; ---- private data area, then switch ------------------------------
            mov bx,[paras]
            or bx,bx
            jz .nopara
            mov ah,0x48
            int 0x21
            jc .allocbad
            mov es,ax
            jmp .go
.nopara:    push ds
            pop es
.go:        mov dx,m_sw
            mov ah,9
            int 0x21
            xor ax,ax                       ; AX=0 -> 16-BIT client, as krnl386 does
            call far [sw_off]
            jc .swbad

            ; ---- we are in protected mode now --------------------------------
            mov dx,m_pm
            mov ah,9
            int 0x21
            mov ax,0x168a
            mov si,vendor
            int 0x2f
            call report
            or al,al
            jnz done
            ; ---- the entry point exists: describe and dump it -----------------
            mov [ep_off],di
            mov [ep_seg],es
            mov dx,m_lar
            mov ah,9
            int 0x21
            mov bx,es
            lar ax,bx
            jnz .nolar
            call hex16
            jmp .dump
.nolar:     mov dx,m_larbad
            mov ah,9
            int 0x21
            jmp done
.dump:      call crlf
            mov dx,m_bytes
            mov ah,9
            int 0x21
            mov si,[ep_off]
            mov cx,64
.db1:       mov al,[es:si]
            call hex8
            mov al,' '
            call putc
            inc si
            loop .db1
            call crlf
            jmp done

.allocbad:  mov dx,m_allocbad
            mov ah,9
            int 0x21
            jmp done
.swbad:     mov dx,m_swbad
            mov ah,9
            int 0x21
done:       mov ax,0x4c00
            int 0x21

; ---- print "AL=xx ES:DI=xxxx:xxxx" then CRLF -------------------------------
report:     push ax
            push ax
            mov dx,m_al
            mov ah,9
            int 0x21
            pop ax
            call hex8                        ; AL as returned
            mov dx,m_esdi
            mov ah,9
            int 0x21
            mov ax,es
            call hex16
            mov al,':'
            call putc
            mov ax,di
            call hex16
            call crlf
            pop ax
            ret

putc:       push ax
            push dx
            mov dl,al
            mov ah,2
            int 0x21
            pop dx
            pop ax
            ret
nib:        and al,0x0f
            add al,'0'
            cmp al,'9'
            jbe .p
            add al,7
.p:         call putc
            ret
hex8:       push ax
            mov ah,al
            shr al,4
            call nib
            mov al,ah
            call nib
            pop ax
            ret
hex16:      push ax
            xchg al,ah
            call hex8
            pop ax
            push ax
            call hex8
            pop ax
            ret
crlf:       push ax
            mov al,13
            call putc
            mov al,10
            call putc
            pop ax
            ret

vendor      db 'MS-DOS',0
m_rm        db 13,10,'-- INT 2Fh 168A in REAL mode --',13,10,'$'
m_chk       db '-- INT 2Fh 1687 DPMI check --',13,10,'  CPU/entry: $'
m_paras     db '  private paras: $'
m_sw        db '-- switching to 16-bit PM --',13,10,'$'
m_pm        db '-- INT 2Fh 168A in PROTECTED mode --',13,10,'$'
m_al        db '  AL=$'
m_esdi      db '  ES:DI=$'
m_nodpmi    db '  NO DPMI HOST',13,10,'$'
m_allocbad  db '  private-area alloc FAILED',13,10,'$'
m_swbad     db '  MODE SWITCH FAILED (CF)',13,10,'$'
m_lar       db '  LAR(ES)=$'
m_larbad    db '  LAR FAILED -- selector not readable',13,10,'$'
m_bytes     db '  code at entry: $'
ep_off      dw 0
ep_seg      dw 0
sw_off      dw 0
sw_seg      dw 0
paras       dw 0

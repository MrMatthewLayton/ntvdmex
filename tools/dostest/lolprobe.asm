; lolprobe.asm -- what does INT 21h AH=52h actually hand back, and what is at the
; offsets krnl386's init reads?  GH #128 / #35.
;
; krnl386.exe seg1:0xc041 does:
;       mov ah,52h / int 21h          ; ES:BX = SysVars ("list of lists")
;       mov ax,es / shl ax,4 / add ax,86h / sub ax,400h   ; -> a 0040:xxxx pointer
;       mov di,[es:bx+6Ah]                                 ; a SUB-STRUCTURE pointer
;       mov ax,[es:di+10h] / +0Ch / +00h / +24h / +18h / +28h
; NTVDMEX plants a SysVars stub whose only real field is the first-MCB word at BX-2
; (GH #35), so every one of those reads lands on zero.  This dumps the same region
; from genuine MS-DOS 6.22 so the stub can be filled from measurement, not memory.
BITS 16
ORG 0x100

start:      mov ah,0x52
            int 0x21                    ; ES:BX
            mov [cs:lolseg],es
            mov [cs:loloff],bx
            mov ax,[es:bx+0x6a]
            mov [cs:sub6a],ax
            push cs
            pop ds

            mov dx,m_es
            mov ah,9
            int 0x21
            mov ax,[lolseg]
            call hex16
            mov dx,m_bx
            mov ah,9
            int 0x21
            mov ax,[loloff]
            call hex16
            mov dx,m_p6a
            mov ah,9
            int 0x21
            mov ax,[sub6a]
            call hex16
            call crlf
            call crlf

            mov dx,m_seg                ; the whole segment from 0 -- krnl386 hard
            mov ah,9                    ; codes ES:0086, so BX-relative is not enough
            int 0x21
            call crlf
            xor si,si
            mov cx,32
.l1:        push cx
            call dumpline
            pop cx
            loop .l1

            call crlf
            mov dx,m_sub
            mov ah,9
            int 0x21
            call crlf
            mov si,[sub6a]
            mov cx,4
.l2:        push cx
            call dumpline
            pop cx
            loop .l2

            mov ax,0x4c00
            int 0x21

; ---- print 16 bytes of lolseg:si, advancing si ----
dumpline:   mov ax,si
            call hex16
            mov al,':'
            call putc
            mov al,' '
            call putc
            mov cx,16
            push es
            mov es,[lolseg]
.b:         mov al,[es:si]
            call hex8
            mov al,' '
            call putc
            inc si
            loop .b
            pop es
            call crlf
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

m_es        db 'ES=$'
m_bx        db ' BX=$'
m_p6a       db ' [ES:BX+6A]=$'
m_seg       db '-- SysVars segment, ES:0000..01FF --$'
m_sub       db '-- the [ES:BX+6A] sub-structure --$'
lolseg      dw 0
loloff      dw 0
sub6a       dw 0

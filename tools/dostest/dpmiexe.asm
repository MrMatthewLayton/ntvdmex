; dpmiexe.asm -- 16-bit DPMI client in a REAL MZ .EXE (GH #2, run 48).
;
; Unlike dpmitest.com (single .COM segment, CS=DS=SS=PSP), this is a genuine
; multi-segment MZ .EXE: distinct CODE / DATA / STACK segments, loaded with a
; relocation applied by dos_load's MZ path. It proves two things at once:
;   1. the .EXE LOAD path (MZ header parse + relocation fixup + CS!=PSP entry), and
;   2. the .EXE SWITCH path -- dpmi_switch_to_pm builds three DISTINCT selectors
;      (CS=0x0F based at CODE, DS=0x17 based at DATA, SS=0x1F based at STACK), so a
;      DS-relative INT 21h in PM resolves through the DATA base, not the code base.
;
; Hand-built with NASM -f bin (no 16-bit linker needed). The image is one flat blob,
; but the loader places it at (PSP+0x10):0 and the DPMI switch bases CS at the code
; paragraph and DS at the DATA paragraph -- so every DATA reference must be a DATA-
; segment-relative offset (`sym - DATA`), exactly as a real segmented .EXE emits.
; DATA is paragraph-aligned so (DATA-IMAGE)>>4 is exact.
;
; Flow: real-mode marker -> INT 2Fh 1687 detect -> far-call the switch entry ->
;   (now PM) INT 31h 0400 version -> INT 21h AH=09 PM print (DS-relative) -> verify
;   version 005A -> INT 21h 4Ch exit.  Assemble: nasm -f bin -> .exe
bits 16

HDR_PARAS equ 2                    ; 32-byte header (28 MZ fields + 1 reloc entry)

; ------------------------------------------------------------------ MZ header ---
mz_start:
    db 'M', 'Z'
    dw FILESIZE % 512               ; e_cblp  bytes in last 512-byte page
    dw (FILESIZE + 511) / 512       ; e_cp    total pages
    dw 1                            ; e_crlc  relocation count
    dw HDR_PARAS                    ; e_cparhdr  header size in paragraphs
    dw 0x0010                       ; e_minalloc
    dw 0xFFFF                       ; e_maxalloc
    dw STACK_SEG                    ; e_ss    stack segment (relative to load segment)
    dw STACK_SZ                     ; e_sp    initial SP (top of stack)
    dw 0                            ; e_csum
    dw 0                            ; e_ip    entry IP (start is at CODE:0)
    dw 0                            ; e_cs    entry CS (relative to load segment = image base)
    dw reloc_table                  ; e_lfarlc  reloc table file offset
    dw 0                            ; e_ovno
reloc_table:
    dw ds_imm - IMAGE               ; offset of the fixup within the CODE segment
    dw 0                            ;   segment = 0 (the image-base / CODE segment)
    times (HDR_PARAS * 16) - ($ - mz_start) db 0

; -------------------------------------------------------------- load image ------
IMAGE:

; ---- CODE segment (image base, e_cs = 0) ----
start:
    ; DOS entry: SS:SP from header, CS from header, DS/ES = PSP. Load DS = DATA segment.
    ; The immediate is relocated by the loader (reloc_table entry above): it assembles as
    ; DATA_SEG (data paragraph rel to image) and dos_load adds the load segment -> the
    ; absolute DATA paragraph.
    mov ax, DATA_SEG
ds_imm equ $ - 2                    ; the imm16 just emitted (the relocation target)
    mov ds, ax

    mov dx, msg_start - DATA        ; real-mode marker (DS=DATA; refs are DATA-relative)
    mov ah, 0x09
    int 0x21

    mov ax, 0x1687                 ; detect DPMI
    int 0x2F
    test ax, ax
    jnz .nodpmi
    mov [entry - DATA], di         ; save ES:DI mode-switch entry
    mov [entry + 2 - DATA], es

    mov dx, msg_switch - DATA
    mov ah, 0x09
    int 0x21

    xor ax, ax                     ; AX=0: 16-bit client
    call far [entry - DATA]        ; far-call the switch entry -> host switches to PM

    ; ---- now in PROTECTED MODE: CS=0x0F(code), DS=0x17(data), SS=0x1F(stack) ----
    mov ax, 0x0400                 ; INT 31h get DPMI version
    int 0x31
    mov [ver - DATA], ax           ; store through DS=0x17 -> DATA base (the .EXE proof)

    mov dx, pmmsg - DATA           ; PM INT 21h AH=09 print, DS-relative through DATA base
    mov ah, 0x09
    int 0x21

    mov ax, [ver - DATA]
    cmp ax, 0x005A
    jne .verbad
    mov dx, okmsg - DATA
    mov ah, 0x09
    int 0x21

    ; --- run 49: INT 31h 0300 (simulate real-mode INT) from a multi-segment .EXE ---
    ; The RMCS carries real-mode SEGMENTS. In PM `SEG data` is a selector, not a
    ; paragraph, so query the DS (0x17) selector base via INT 31h 0006 and shift it to
    ; a paragraph -- the proper DPMI way an .EXE addresses its data in a real-mode call.
    mov bx, 0x0017                 ; the DS data selector
    mov ax, 0x0006                 ; get base of selector BX -> CX:DX
    int 0x31
    mov ax, dx
    shr ax, 4                      ; DX >> 4
    shl cx, 12                     ; CX << 12  (high word of the base)
    or ax, cx                      ; AX = base >> 4 = real-mode DATA paragraph
    mov [rmcs + 0x24 - DATA], ax   ; RMCS.DS = the real-mode DATA paragraph
    mov word [rmcs + 0x1C - DATA], 0x0900   ; RMCS.AX = AH=09 print-string
    mov word [rmcs + 0x14 - DATA], rmsg - DATA   ; RMCS.DX = rmsg offset within DATA
    mov word [rmcs + 0x20 - DATA], 0x0202        ; RMCS.Flags
    push ds
    pop es                         ; ES = DS = 0x17
    mov di, rmcs - DATA            ; ES:DI -> RMCS
    mov ax, 0x0300                 ; simulate real-mode interrupt
    mov bx, 0x0021                 ; BL = INT 21h
    xor cx, cx                     ; no stack words to copy
    int 0x31                       ; host runs real-mode INT 21h AH=09 with the RMCS regs
    mov dx, simokmsg - DATA        ; PM print -- proves we resumed cleanly in PM
    mov ah, 0x09
    int 0x21
    jmp .done
.verbad:
    mov dx, badmsg - DATA
    mov ah, 0x09
    int 0x21
.done:
    mov ax, 0x4C00                 ; terminate
    int 0x21
.nodpmi:
    mov dx, msg_nodpmi - DATA
    mov ah, 0x09
    int 0x21
    mov ax, 0x4C00
    int 0x21

; ---- DATA segment (paragraph-aligned so DATA_SEG is exact) ----
    align 16
DATA:
DATA_SEG equ (DATA - IMAGE) >> 4
entry:      dd 0
ver:        dw 0
msg_start:  db 'DPMI .EXE: real-mode start (multi-seg MZ), detecting 2Fh/1687...', 13, 10, '$'
msg_switch: db 'DPMI present -> far-calling switch entry (CS!=DS!=SS)...', 13, 10, '$'
pmmsg:      db '  [PM INT 21h AH=09 printed via the DATA selector 0x17]', 13, 10, '$'
okmsg:      db 'DPMI .EXE: PROTECTED MODE OK -- INT 31h ver 005A, DS-relative print!', 13, 10, '$'
badmsg:     db 'DPMI .EXE: version mismatch (switch/dispatch FAILED).', 13, 10, '$'
msg_nodpmi: db 'DPMI .EXE: DPMI not present.', 13, 10, '$'
rmsg:       db '  [0300 sim-int -> real-mode INT 21h AH=09 from the .EXE]', 13, 10, '$'
simokmsg:   db 'DPMI .EXE: 0300 simulate-real-mode-int OK!', 13, 10, '$'
    align 2
rmcs:       times 50 db 0                     ; DPMI real-mode call structure (INT 31h 0300)

; ---- STACK segment (paragraph-aligned) ----
    align 16
STACK:
STACK_SEG equ (STACK - IMAGE) >> 4
    times 0x100 db 0
STACK_SZ equ $ - STACK             ; SP = top of the 256-byte stack

FILEEND:
FILESIZE equ FILEEND - mz_start

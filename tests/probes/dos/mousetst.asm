; mousetst.asm -- interactive INT 33h mouse test in mode 12h.
;
; Resets the mouse driver, shows the (host-drawn) cursor, then loops reading the
; pointer position + buttons via INT 33h AX=03. Holding the LEFT button paints a
; white pixel at the cursor (INT 10h AX=0C0F) -- so you can draw, which proves
; position + button tracking end-to-end. The arrow cursor (drawn by our driver)
; tracks the host mouse. Press any key to quit.
;
; Assemble: nasm -f bin mousetst.asm -o mousetst.com
bits 16
org 0x100

start:
    mov ax, 0x0012              ; SCREEN 12 (640x480x16)
    int 0x10
    xor ax, ax                  ; INT 33h AX=0: reset driver
    int 0x33
    mov ax, 1                   ; INT 33h AX=1: show cursor (host draws it)
    int 0x33

.loop:
    mov ax, 3                   ; INT 33h AX=3: BX=buttons, CX=x, DX=y
    int 0x33
    test bl, 1                  ; left button down?
    jz .key
    mov ax, 0x0C0F              ; INT 10h AH=0C write pixel, AL=colour 15
    int 0x10                    ;   at (CX,DX) -> draw under the cursor
.key:
    mov ah, 0x01                ; INT 16h: any key -> quit
    int 0x16
    jz .loop

    mov ax, 0x0003              ; back to text mode
    int 0x10
    mov ax, 0x4C00
    int 0x21

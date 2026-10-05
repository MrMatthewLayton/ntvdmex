; modeswitch.asm -- regression test for GH #14
;   "No text rendering after a graphics->text mode switch".
;
; Reproduces the exact failure: enter mode 13h, WIPE the DAC palette to black
; (a real graphics app such as QuickBASIC SCREEN 13 always reprograms the DAC),
; fill the screen so the graphics mode is unmistakable, then return to text
; mode 3 and print a message via INT 21h.
;
;   * With the bug   -> INT 10h AH=00 never reloads the DAC, so the text is
;                       drawn in the leftover all-black 13h palette: INVISIBLE.
;   * With the fix   -> mode-set reloads the default DAC (gray-on-black), so the
;                       message is READABLE.
;
; Screendump the Luna window during the final key-wait to verify. Press any key
; to exit. Assemble: nasm -f bin modeswitch.asm -o modeswitch.com
bits 16
org 0x100
start:
    ; --- 1. enter mode 13h (320x200x256) --------------------------------
    mov ax, 0x0013
    int 0x10

    ; --- 2. wipe DAC entries 0..15 to black (clobber the text colours) ---
    mov dx, 0x3C8
    xor al, al
    out dx, al                  ; DAC write index = 0
    mov dx, 0x3C9
    mov cx, 16*3                ; 16 entries x (R,G,B)
.wipe:
    xor al, al
    out dx, al                  ; component = 0 (black)
    loop .wipe

    ; --- 3. fill the visible screen so mode 13h is unmistakable ----------
    mov ax, 0xA000
    mov es, ax
    xor di, di
    mov al, 40                  ; some mid palette index
    mov cx, 32000
    rep stosb

    ; --- 4. hold the graphics frame until a key (deterministic, no PIT) ---
    ;        (screendump here shows mode 13h; press a key to continue.)
    mov ah, 0x00
    int 0x16                    ; block for a key

    ; --- 5. back to text mode 3 -----------------------------------------
    mov ax, 0x0003
    int 0x10

    ; --- 6. print the verification message -------------------------------
    mov dx, msg
    mov ah, 0x09
    int 0x21

    ; --- 7. wait for a key (screendump here shows text), then exit -------
    mov ah, 0x00
    int 0x16
    mov ax, 0x4C00
    int 0x21

msg:
    db 13,10,13,10
    db '  ==== GH#14 REGRESSION TEST ====', 13,10,13,10
    db '  Mode 13h -> DAC wiped -> text mode 3.', 13,10
    db '  If you can READ this line, the fix works', 13,10
    db '  (mode-set reloaded the default DAC palette).', 13,10,13,10
    db '  Press any key to exit.', 13,10, '$'

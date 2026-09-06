; spktest.asm -- PLAY SOMETHING ON THE PC SPEAKER, so a human can hear it.
;
; WHY THIS EXISTS. The speaker's synthesis is measured off-VM (audio_test.c: the
; tone comes back out of the mix at the frequency the PIT was programmed to, the
; gate bit alone is silent, a tone past 20 kHz is refused). None of that is a
; measurement of whether it SOUNDS right, and the rig has no ears. So this is the
; one-file A/B: an ascending C-major scale, then a C-major arpeggio, both loud
; enough and long enough to hear a wrong pitch or a wrong duration.
;
; It does exactly what a DOS program does and nothing else: program PIT channel 2
; with a divisor, set port 0x61 bits 0 (timer-2 gate) and 1 (speaker data), wait,
; clear them. If NTVDMEX is silent while this runs, either port 0x61 is not
; reaching the VDD or the mixer is not reading it -- the host log's `hot ports`
; line tells the two apart.
;
; ⚠ TIMING COMES FROM INT 1Ah, NOT A DELAY LOOP. A loop calibrated for period
;   hardware finishes instantly on this machine -- we run 16-bit code on the REAL
;   CPU -- so every note would be a click. The BIOS tick is 18.2 Hz and is the
;   only clock here that means the same thing on both.
;
; Assemble: nasm -f bin -o spktest.com spktest.asm

bits 16
org 0x100

TICK_NOTE   equ 5           ; ~275 ms: long enough to hear the pitch
TICK_GAP    equ 1           ; ~55 ms of silence, so notes do not slur

start:
    mov ah, 0x09
    mov dx, msg
    int 0x21

    mov word [idx], tune
.next:
    mov si, [idx]
    mov ax, [si]                ; divisor, 0 = end of tune
    or ax, ax
    jz .done
    mov [divisor], ax
    mov ax, [si+2]              ; duration in BIOS ticks
    mov [dur], ax
    add word [idx], 4

    ; --- PIT channel 2, mode 3 (square wave), lo/hi divisor -------------------
    mov al, 0xB6
    out 0x43, al
    mov ax, [divisor]
    out 0x42, al                ; low byte
    mov al, ah
    out 0x42, al                ; high byte

    ; --- gate the speaker on: BOTH bits, because both gate --------------------
    in al, 0x61
    or al, 0x03
    out 0x61, al

    mov ax, [dur]
    call wait_ticks

    ; --- off between notes ----------------------------------------------------
    in al, 0x61
    and al, 0xFC
    out 0x61, al
    mov ax, TICK_GAP
    call wait_ticks
    jmp .next

.done:
    in al, 0x61                 ; leave the speaker as we found it
    and al, 0xFC
    out 0x61, al
    mov ah, 0x09
    mov dx, msgdone
    int 0x21
    mov ax, 0x4C00
    int 0x21

; ── wait AX BIOS ticks. Everything crosses INT 1Ah in MEMORY, not in registers:
;    the tick service is free to clobber whatever it likes and this must not
;    depend on which registers our host happens to preserve.
wait_ticks:
    mov [want], ax
    mov ah, 0x00
    int 0x1A                    ; CX:DX = ticks since midnight
    mov [t0], dx
.w:
    mov ah, 0x00
    int 0x1A
    mov ax, dx
    sub ax, [t0]                ; wraps correctly at midnight for short waits
    cmp ax, [want]
    jb .w
    ret

; ── The tune. Divisor = 1193182 / frequency, computed here so a wrong pitch is
;    a wrong number in this table rather than a mystery.
;      C5 523 -> 2280   D5 587 -> 2032   E5 659 -> 1810   F5 698 -> 1708
;      G5 784 -> 1522   A5 880 -> 1356   B5 988 -> 1208   C6 1047 -> 1140
tune:
    dw 2280, TICK_NOTE          ; C5   ── the scale: any pitch error is obvious
    dw 2032, TICK_NOTE          ; D5
    dw 1810, TICK_NOTE          ; E5
    dw 1708, TICK_NOTE          ; F5
    dw 1522, TICK_NOTE          ; G5
    dw 1356, TICK_NOTE          ; A5
    dw 1208, TICK_NOTE          ; B5
    dw 1140, TICK_NOTE          ; C6
    dw 2280, TICK_NOTE          ; C5   ── then the arpeggio, to hear intervals
    dw 1810, TICK_NOTE          ; E5
    dw 1522, TICK_NOTE          ; G5
    dw 1140, TICK_NOTE * 3      ; C6, held
    dw 0                        ; end

idx      dw 0
divisor  dw 0
dur      dw 0
want     dw 0
t0       dw 0

msg      db 'SPKTEST: C major scale, then an arpeggio. Listen.', 13, 10, '$'
msgdone  db 'SPKTEST: done.', 13, 10, '$'

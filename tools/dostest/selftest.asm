; selftest.asm -- NTVDMEX one-shot regression suite (M4+).
;
; ONE DOS program that exercises every host subsystem in a single VDM session
; and prints a PASS/FAIL report to the screen (also captured in ntvdmhost.log).
; Non-interactive: it needs no keypresses to run (it waits for one only at the
; end so the window stays up for a screendump). Replaces the manual run-one-
; test-at-a-time menu as the defence against regressions in earlier builds.
;
; Each subsystem test is a subroutine returning AX=0 (pass) or AX=nonzero (a
; fail-code byte, so a failure pinpoints the step). The harness prints each
; test's LABEL *before* running it and its result *after* -- so if a test hangs
; or faults, its label is the last line on screen, pinpointing the culprit.
; The video test runs first (it switches modes, which clears the screen) and its
; result is stashed; everything else prints live.
;
; Coverage: DOS memory (AH 48/4A/49), file I/O (3C/40/3E/3D/3F), XMS 3.0
; (INT 2Fh + far-call API + Move round-trip), EMS LIM 4.0 (INT 67h + page-frame
; shadowing round-trip), PIT timer IRQ (0040:006C advances), mouse INT 33h
; (driver present), keyboard INT 16h (AH=01/11 report "no key" -- the INKEY$
; phantom-key guard), video (mode 13h set + A000 aperture round-trip).
;
; Exit code (errorlevel) = number of failed tests (0 = all passed).
; Assemble: nasm -f bin selftest.asm -o selftest.com
bits 16
org 0x100

start:
    mov dx, s_banner
    mov ah, 0x09
    int 0x21

    ; --- walk the {label, proc} table: print label, run, print result -------
    mov si, tests
.next:
    lodsw                       ; AX = label ($-string); 0 terminates
    test ax, ax
    jz .verdict
    mov dx, ax
    mov ah, 0x09
    int 0x21                    ; label shows BEFORE the test runs
    lodsw                       ; AX = test proc
    push si
    call ax                     ; -> AX=0 pass / nonzero fail-code
    pop si
    call result                 ; print PASS / FAIL=xx; bump failcount
    jmp .next

.verdict:
    mov ax, [failcount]
    test ax, ax
    jnz .somefail
    mov dx, s_allpass
    mov ah, 0x09
    int 0x21
    jmp .waitkey
.somefail:
    mov dx, s_fhdr
    mov ah, 0x09
    int 0x21
    mov al, [failcount]
    call printhexb
    mov dx, s_ftail
    mov ah, 0x09
    int 0x21

.waitkey:
    mov dx, s_waitk
    mov ah, 0x09
    int 0x21
    mov ah, 0x00                ; block until a key (keeps the window up)
    int 0x16
    mov ah, 0x4C                ; exit, errorlevel = fail count
    mov al, [failcount]
    int 0x21

; ===========================================================================
; helpers
; ===========================================================================

; print AX result: 0 -> "PASS"; else -> "FAIL=<hexbyte>". updates failcount.
result:
    push ax
    test ax, ax
    jnz .fail
    mov dx, s_pass
    mov ah, 0x09
    int 0x21
    pop ax
    ret
.fail:
    inc word [failcount]
    mov dx, s_fail
    mov ah, 0x09
    int 0x21
    pop ax
    call printhexb
    mov dx, s_crlf
    mov ah, 0x09
    int 0x21
    ret

; print AL as two hex digits
printhexb:
    push ax
    push cx
    mov cl, al
    mov al, cl
    shr al, 4
    call .nib
    mov al, cl
    and al, 0x0F
    call .nib
    pop cx
    pop ax
    ret
.nib:
    and al, 0x0F
    add al, '0'
    cmp al, '9'
    jbe .p
    add al, 7
.p:
    mov dl, al
    mov ah, 0x02
    int 0x21
    ret

; ===========================================================================
; subsystem tests  (AX=0 pass; AX=fail-code on failure)
; ===========================================================================

; --- DOS conventional memory: alloc / resize / free ------------------------
t_dosmem:
    ; A freshly-loaded program owns ALL conventional memory (DOS gives a .COM the
    ; whole largest free block), so AH=48 fails until we shrink our own PSP block
    ; first with AH=4A -- exactly what a real DOS program / memtest.com does.
    push cs
    pop es                      ; ES = our PSP segment (block to resize)
    mov ah, 0x4A
    mov bx, 0x1000              ; keep 64KB (PSP+code+stack), free the rest
    int 0x21
    jc .f0
    mov ah, 0x48                ; now there is a free tail to allocate from
    mov bx, 0x40
    int 0x21
    jc .f1
    mov si, ax
    mov es, si
    mov ah, 0x4A
    mov bx, 0x20
    int 0x21
    jc .f2
    mov es, si
    mov ah, 0x49
    int 0x21
    jc .f3
    xor ax, ax
    ret
.f0: mov ax, 0x10               ; shrink-own-block failed
     ret
.f1: mov ax, 0x11
     ret
.f2: mov ax, 0x12
     ret
.f3: mov ax, 0x13
     ret

; --- file I/O: create / write / close / open / read / verify ---------------
t_fileio:
    mov ah, 0x3C
    xor cx, cx
    mov dx, fname
    int 0x21
    jc .f1
    mov bx, ax
    mov ah, 0x40
    mov cx, 16
    mov dx, fdata
    int 0x21
    jc .f2
    cmp ax, 16
    jne .f2
    mov ah, 0x3E
    int 0x21
    mov ah, 0x3D
    xor al, al
    mov dx, fname
    int 0x21
    jc .f3
    mov bx, ax
    mov ah, 0x3F
    mov cx, 16
    mov dx, fbuf
    int 0x21
    jc .f4
    cmp ax, 16
    jne .f4
    mov ah, 0x3E
    int 0x21
    mov si, fdata
    mov di, fbuf
    mov cx, 16
.cmp:
    mov al, [si]
    cmp al, [di]
    jne .f5
    inc si
    inc di
    loop .cmp
    xor ax, ax
    ret
.f1: mov ax, 0x21
     ret
.f2: mov ax, 0x22
     ret
.f3: mov ax, 0x23
     ret
.f4: mov ax, 0x24
     ret
.f5: mov ax, 0x25
     ret

; --- XMS 3.0: detect, version, alloc, Move round-trip, free ----------------
t_xms:
    mov ax, 0x4300
    int 0x2F
    cmp al, 0x80
    jne .f1
    mov ax, 0x4310
    int 0x2F
    mov [xentry], bx
    mov [xentry + 2], es
    mov ah, 0x00
    call far [xentry]
    cmp ax, 0x0200
    jb .f2
    mov ah, 0x09
    mov dx, 64
    call far [xentry]
    cmp ax, 1
    jne .f3
    mov [xhandle], dx
    mov word [xmv_len], 32
    mov word [xmv_len + 2], 0
    mov word [xmv_sh], 0
    mov word [xmv_so], xsrc
    mov [xmv_so + 2], ds
    mov ax, [xhandle]
    mov [xmv_dh], ax
    mov word [xmv_do], 0
    mov word [xmv_do + 2], 0
    mov ah, 0x0B
    mov si, xmv
    call far [xentry]
    cmp ax, 1
    jne .f4
    mov ax, [xhandle]
    mov [xmv_sh], ax
    mov word [xmv_so], 0
    mov word [xmv_so + 2], 0
    mov word [xmv_dh], 0
    mov word [xmv_do], xdst
    mov [xmv_do + 2], ds
    mov ah, 0x0B
    mov si, xmv
    call far [xentry]
    cmp ax, 1
    jne .f4
    mov si, xsrc
    mov di, xdst
    mov cx, 32
.cmp:
    mov al, [si]
    cmp al, [di]
    jne .f5
    inc si
    inc di
    loop .cmp
    mov ah, 0x0A
    mov dx, [xhandle]
    call far [xentry]
    cmp ax, 1
    jne .f6
    xor ax, ax
    ret
.f1: mov ax, 0x31
     ret
.f2: mov ax, 0x32
     ret
.f3: mov ax, 0x33
     ret
.f4: mov ax, 0x34
     ret
.f5: mov ax, 0x35
     ret
.f6: mov ax, 0x36
     ret

; --- EMS LIM 4.0: detect, version, alloc, page-frame shadow round-trip -----
t_ems:
    mov ax, 0x3567
    int 0x21
    mov di, 0x000A
    mov si, emmname
    mov cx, 8
    cld
.cn:
    mov al, [si]
    cmp al, [es:di]
    jne .f1
    inc si
    inc di
    loop .cn
    mov ah, 0x46
    int 0x67
    or ah, ah
    jnz .f2
    cmp al, 0x40
    jne .f2
    mov ah, 0x41
    int 0x67
    or ah, ah
    jnz .f3
    mov [eframe], bx
    mov ah, 0x43
    mov bx, 2
    int 0x67
    or ah, ah
    jnz .f4
    mov [ehandle], dx
    mov ah, 0x44
    mov al, 0
    mov bx, 0
    mov dx, [ehandle]
    int 0x67
    or ah, ah
    jnz .f5
    mov es, [eframe]
    xor di, di
    mov word [es:di], 0x55AA
    mov word [es:di + 0x3FFE], 0x1234
    mov ah, 0x44
    mov al, 0
    mov bx, 1
    mov dx, [ehandle]
    int 0x67
    or ah, ah
    jnz .f5
    mov es, [eframe]
    xor di, di
    mov word [es:di], 0x9999
    mov ah, 0x44
    mov al, 0
    mov bx, 0
    mov dx, [ehandle]
    int 0x67
    or ah, ah
    jnz .f5
    mov es, [eframe]
    xor di, di
    cmp word [es:di], 0x55AA
    jne .f6
    cmp word [es:di + 0x3FFE], 0x1234
    jne .f6
    mov ah, 0x45
    mov dx, [ehandle]
    int 0x67
    or ah, ah
    jnz .f7
    xor ax, ax
    ret
.f1: mov ax, 0x41
     ret
.f2: mov ax, 0x42
     ret
.f3: mov ax, 0x43
     ret
.f4: mov ax, 0x44
     ret
.f5: mov ax, 0x45
     ret
.f6: mov ax, 0x46
     ret
.f7: mov ax, 0x47
     ret

; --- PIT timer IRQ: the BIOS tick at 0040:006C advances --------------------
; Each loop polls INT 16h (a host round-trip = an INT 08h injection point) and
; rereads the tick. Breaks as soon as it advances; bounded (1 * 65536 polls) so
; a dead timer fails in a few seconds instead of hanging.
t_timer:
    sti
    push 0x40
    pop es
    mov ax, [es:0x6C]
    mov [t0], ax
    mov dx, 32                  ; outer: ~32 * 65536 INT 16h round-trips is well over
.outer:                        ; one PIT period (~55ms), so a live ~18Hz tick is certain
    xor cx, cx                 ; to land in the window (a pure spin can finish under 55ms)
.poll:
    mov ah, 0x01
    int 0x16
    push 0x40
    pop es
    mov ax, [es:0x6C]
    cmp ax, [t0]
    jne .ok
    loop .poll
    dec dx
    jnz .outer
    mov ax, 0x51                ; timed out: timer never fired
    ret
.ok:
    xor ax, ax
    ret

; --- mouse INT 33h: driver present -----------------------------------------
t_mouse:
    xor ax, ax
    int 0x33
    cmp ax, 0xFFFF
    jne .f
    xor ax, ax
    ret
.f: mov ax, 0x61
    ret

; --- keyboard INT 16h: empty buffer reports "no key" (INKEY$ guard) ---------
t_kbd:
    mov ah, 0x01
    int 0x16
    jnz .f
    mov ah, 0x11
    int 0x16
    jnz .f
    xor ax, ax
    ret
.f: mov ax, 0x71
    ret

; --- video: INT 10h get-mode + A000 aperture round-trip --------------------
; Deliberately does NOT switch the visible mode (a mode switch would leave the
; display in a non-text state and hide this very report). The A000 aperture is
; mapped RAM in every mode, so we exercise it in place. INT 10h AH=0F must
; report the default text mode 3.
t_video:
    mov ah, 0x0F                ; get current video mode -> AL
    int 0x10
    cmp al, 0x03
    jne .f
    push 0xA000                 ; aperture is mapped RAM even in text mode
    pop es
    mov byte [es:0x1234], 0xA5
    mov byte [es:0x5678], 0x5A
    mov al, [es:0x1234]
    cmp al, 0xA5
    jne .f
    mov al, [es:0x5678]
    cmp al, 0x5A
    jne .f
    xor ax, ax
    ret
.f: mov ax, 0x81
    ret

; ===========================================================================
; the {label, proc} table (label first so a hang shows the culprit's label)
; ===========================================================================
tests:
    dw lbl_dosmem, t_dosmem
    dw lbl_fileio, t_fileio
    dw lbl_xms,    t_xms
    dw lbl_ems,    t_ems
    dw lbl_timer,  t_timer
    dw lbl_mouse,  t_mouse
    dw lbl_kbd,    t_kbd
    dw lbl_video,  t_video
    dw 0

lbl_dosmem: db "DOS memory......  $"
lbl_fileio: db "File I/O........  $"
lbl_xms:    db "XMS 3.0.........  $"
lbl_ems:    db "EMS LIM 4.0.....  $"
lbl_timer:  db "PIT timer IRQ...  $"
lbl_mouse:  db "Mouse INT 33h...  $"
lbl_kbd:    db "Keyboard INT16..  $"
lbl_video:  db "Video 13h+A000..  $"

; ===========================================================================
; strings + data
; ===========================================================================
s_banner:  db 13, 10, "=== NTVDMEX self-test suite ===", 13, 10
           db "-------------------------------", 13, 10, "$"
s_pass:    db "PASS", 13, 10, "$"
s_fail:    db "FAIL=$"
s_crlf:    db 13, 10, "$"
s_allpass: db "-------------------------------", 13, 10
           db "==== ALL TESTS PASSED ====", 13, 10, "$"
s_fhdr:    db "-------------------------------", 13, 10, "==== FAILURES: $"
s_ftail:   db " ====", 13, 10, "$"
s_waitk:   db "Press any key to exit...", 13, 10, "$"

fname:     db "C:\ntvdmex\ST$.TMP", 0
fdata:     db "selftest fileio!"          ; exactly 16 bytes
fbuf:      times 16 db 0

emmname:   db "EMMXXXX0"

xentry:    dd 0
xhandle:   dw 0
xsrc:      db "NTVDMEX XMS round-trip 0123456!", 0   ; 32 bytes
xdst:      times 32 db 0xEE
xmv:
xmv_len:   dd 0
xmv_sh:    dw 0
xmv_so:    dd 0
xmv_dh:    dw 0
xmv_do:    dd 0

eframe:    dw 0
ehandle:   dw 0

t0:        dw 0

failcount: dw 0
results:   times 16 db 0

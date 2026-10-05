; xmstest.asm -- prove the XMS driver works end-to-end in V86 on the real CPU.
;
; Mirrors the off-VM xms_test.c battery, but exercises the *full* path: the
; INT 2Fh AX=4300/4310 multiplex (install check + get entry point), the FAR-CALL
; XMS API entry (BOP 0x43 ; RETF), and the Move function copying between an EMB
; on the host heap and the guest's conventional memory. Steps:
;   1. INT 2Fh 4300 -> AL must be 80h (installed)
;   2. INT 2Fh 4310 -> save ES:BX as the API entry point
;   3. fn 00h get version -> AX must be nonzero (>= 0x0200)
;   4. fn 09h allocate 64KB -> AX=1, handle in DX
;   5. fn 0Bh move conv buffer -> EMB, then EMB -> a second conv buffer
;   6. verify the round-tripped bytes match the source pattern
;   7. fn 0Ah free the handle -> AX=1
; Prints "XMS PASS" on success or "XMS FAIL @n" (n = failing step) otherwise.
;
; Assemble: nasm -f bin xmstest.asm -o xmstest.com
bits 16
org 0x100

start:
    mov ah, 0x09
    mov dx, banner
    int 0x21

    ; --- step 1: installation check -------------------------------------
    mov ax, 0x4300
    int 0x2F
    cmp al, 0x80
    mov bl, '1'
    jne fail

    ; --- step 2: get the API entry point (ES:BX) ------------------------
    mov ax, 0x4310
    int 0x2F
    mov [entry], bx
    mov [entry+2], es

    ; --- step 3: get version (AH=00) ------------------------------------
    mov ah, 0x00
    call far [entry]
    cmp ax, 0x0200
    mov bl, '3'
    jb fail

    ; --- step 4: allocate a 64KB EMB (AH=09, DX=KB) ---------------------
    mov ah, 0x09
    mov dx, 64
    call far [entry]
    cmp ax, 1
    mov bl, '4'
    jne fail
    mov [handle], dx                ; save the handle

    ; --- step 5a: move conv buffer (src) -> EMB offset 0 ----------------
    mov word [mv_len],    32        ; 32 bytes (even)
    mov word [mv_len+2],  0
    mov word [mv_srch],   0         ; source = conventional
    mov word [mv_srco],   src       ; offset
    mov [mv_srco+2],      ds         ; segment
    mov ax, [handle]
    mov [mv_dsth], ax               ; dest = the EMB
    mov word [mv_dsto],   0
    mov word [mv_dsto+2], 0
    mov ah, 0x0B
    mov si, mv
    push ds
    pop  es                         ; DS:SI = move struct (ES unused but tidy)
    call far [entry]
    cmp ax, 1
    mov bl, '5'
    jne fail

    ; --- step 5b: move EMB offset 0 -> conv buffer (dst) ----------------
    mov ax, [handle]
    mov [mv_srch], ax               ; source = the EMB
    mov word [mv_srco],   0
    mov word [mv_srco+2], 0
    mov word [mv_dsth],   0         ; dest = conventional
    mov word [mv_dsto],   dst
    mov [mv_dsto+2],      ds
    mov ah, 0x0B
    mov si, mv
    call far [entry]
    cmp ax, 1
    mov bl, '5'
    jne fail

    ; --- step 6: verify the round trip ----------------------------------
    mov si, src
    mov di, dst
    mov cx, 32
.cmp:
    mov al, [si]
    cmp al, [di]
    mov bl, '6'
    jne fail
    inc si
    inc di
    loop .cmp

    ; --- step 7: free the EMB -------------------------------------------
    mov ah, 0x0A
    mov dx, [handle]
    call far [entry]
    cmp ax, 1
    mov bl, '7'
    jne fail

    mov ah, 0x09
    mov dx, passmsg
    int 0x21
    mov ax, 0x4C00
    int 0x21

fail:
    mov [failn], bl
    mov ah, 0x09
    mov dx, failmsg
    int 0x21
    mov ax, 0x4C01
    int 0x21

banner:  db "XMS test (INT 2Fh 43xx + far-call API + Move)", 13, 10, "$"
passmsg: db "XMS PASS", 13, 10, "$"
failmsg: db "XMS FAIL @"
failn:   db "?", 13, 10, "$"

entry:   dd 0                       ; far pointer to the XMS API entry
handle:  dw 0

; a recognisable 32-byte source pattern; dst starts as garbage (uninitialised)
src:     db "NTVDMEX XMS round-trip 0123456!", 0
dst:     times 32 db 0xEE

; the 16-byte XMS Move structure (fn 0Bh)
mv:
mv_len:  dd 0
mv_srch: dw 0
mv_srco: dd 0
mv_dsth: dw 0
mv_dsto: dd 0

; emstest.asm -- prove the EMS (LIM 4.0) driver works end-to-end in V86.
;
; Mirrors the off-VM ems_test.c battery, exercising the full INT 67h path and,
; crucially, page-frame *shadowing*: map a logical page into a physical window,
; write it through the frame, swap the window to another page and back, and
; verify the first page's bytes survived (proving the host wrote them back to
; the logical page's host buffer). Steps:
;   1. detect the EMM via the INT 67h vector's "EMMXXXX0" device name
;   2. fn 46h get version  -> AL must be 40h
;   3. fn 41h page frame segment -> save it
;   4. fn 43h allocate 2 logical pages -> handle
;   5. fn 44h map logical 0 -> window 0; write a pattern through the frame
;   6. fn 44h map logical 1 -> window 0 (writes page 0 back); mark it
;   7. fn 44h map logical 0 back; verify the pattern survived the swap
;   8. fn 45h deallocate the handle
; Prints "EMS PASS" or "EMS FAIL @n".
;
; Assemble: nasm -f bin emstest.asm -o emstest.com
bits 16
org 0x100

start:
    mov ah, 0x09
    mov dx, banner
    int 0x21

    ; --- step 1: find the EMM and check its device name -----------------
    mov ax, 0x3567              ; get INT 67h vector -> ES:BX
    int 0x21
    mov di, 0x000A              ; device header name at ES:000Ah
    mov si, emmname
    mov cx, 8
    cld
.cn:
    mov al, [si]
    cmp al, [es:di]
    mov bl, '1'
    jne fail
    inc si
    inc di
    loop .cn

    ; --- step 2: get version (fn 46h) -----------------------------------
    mov ah, 0x46
    int 0x67
    or  ah, ah
    mov bl, '2'
    jnz fail
    cmp al, 0x40
    mov bl, '2'
    jne fail

    ; --- step 3: page frame segment (fn 41h) ----------------------------
    mov ah, 0x41
    int 0x67
    or  ah, ah
    mov bl, '3'
    jnz fail
    mov [frameseg], bx

    ; --- step 4: allocate 2 pages (fn 43h) ------------------------------
    mov ah, 0x43
    mov bx, 2
    int 0x67
    or  ah, ah
    mov bl, '4'
    jnz fail
    mov [handle], dx

    ; --- step 5: map logical 0 -> window 0, write a pattern -------------
    mov ah, 0x44
    mov al, 0                   ; physical window 0
    mov bx, 0                   ; logical page 0
    mov dx, [handle]
    int 0x67
    or  ah, ah
    mov bl, '5'
    jnz fail
    mov es, [frameseg]
    xor di, di
    mov word [es:di], 0x55AA    ; pattern at frame:0000
    mov word [es:di+0x3FFE], 0x1234

    ; --- step 6: swap logical 1 into window 0 (writes page 0 back) ------
    mov ah, 0x44
    mov al, 0
    mov bx, 1                   ; logical page 1
    mov dx, [handle]
    int 0x67
    or  ah, ah
    mov bl, '6'
    jnz fail
    mov es, [frameseg]
    xor di, di
    mov word [es:di], 0x9999    ; mark page 1

    ; --- step 7: bring page 0 back, verify it survived ------------------
    mov ah, 0x44
    mov al, 0
    mov bx, 0                   ; logical page 0
    mov dx, [handle]
    int 0x67
    or  ah, ah
    mov bl, '7'
    jnz fail
    mov es, [frameseg]
    xor di, di
    cmp word [es:di], 0x55AA
    mov bl, '7'
    jne fail
    cmp word [es:di+0x3FFE], 0x1234
    mov bl, '7'
    jne fail

    ; --- step 8: deallocate (fn 45h) ------------------------------------
    mov ah, 0x45
    mov dx, [handle]
    int 0x67
    or  ah, ah
    mov bl, '8'
    jnz fail

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

banner:   db "EMS test (INT 67h + page-frame shadowing)", 13, 10, "$"
passmsg:  db "EMS PASS", 13, 10, "$"
failmsg:  db "EMS FAIL @"
failn:    db "?", 13, 10, "$"
emmname:  db "EMMXXXX0"
frameseg: dw 0
handle:   dw 0

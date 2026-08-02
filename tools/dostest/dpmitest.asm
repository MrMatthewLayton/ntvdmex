; dpmitest.asm -- DPMI 16-bit spike client (M4 slice 3, GH #1).
;
; Proves the real->protected-mode switch via NTVDMEX's kernel-monitor reuse:
;   1. print a real-mode marker (so we know it started)
;   2. INT 2Fh AX=1687h  -> detect DPMI, get the mode-switch entry in ES:DI
;   3. FAR-CALL the entry with AX=0 (16-bit client) -> host switches VDM to PM
;   4. now in PM: INT 31h AX=0000 (allocate a descriptor) -> surfaces a PM event
;
; Increment 1 stops the host at the first PM event and dumps the raw taxonomy to
; ntvdmhost.log (how the monitor reflects a PM INT). Later increments service
; INT 31h and thunk INT 21h back to real mode. Assemble: nasm -f bin.
bits 16
org 0x100
start:
    ; --- 1. real-mode marker ---------------------------------------------
    mov dx, msg_start
    mov ah, 0x09
    int 0x21

    ; --- 1b. shrink our PSP block to 64 KB so conventional memory is FREE for
    ;         INT 31h 0100 later (a .COM owns all memory on load; without this
    ;         0100 correctly reports ENOMEM). ES already = PSP for a .COM. -----
    mov ah, 0x4A
    mov bx, 0x1000              ; keep 0x1000 paras = 64 KB (stack at top stays valid)
    int 0x21

    ; --- 2. detect DPMI (INT 2Fh AX=1687h) -------------------------------
    mov ax, 0x1687
    int 0x2F
    test ax, ax                 ; AX=0 => DPMI present
    jnz .nodpmi
    mov [entry], di             ; save ES:DI mode-switch entry
    mov [entry+2], es

    mov dx, msg_switch
    mov ah, 0x09
    int 0x21

    ; --- 3. far-call the mode-switch entry (AX=0: 16-bit client) ---------
    xor ax, ax
    call far [entry]

    ; --- 4. we are now in PROTECTED MODE (run 41) ------------------------
    ; Unmodified DPMI client using INT 31h 0300 (simulate real-mode interrupt): the way
    ; extenders route DOS/BIOS. Build a real-mode call structure (RMCS) for INT 21h AH=09
    ; and let the host run it in V86 with the RMCS registers, then resume us in PM.
    mov ax, 0x0400             ; get DPMI version
    int 0x31
    mov [0x600], ax

    ; --- run 42: exercise the new INT 31h table services ----------------
    ; These INT 31h sites sit well past the old 0x2000 patch bound, so a clean
    ; pass also proves the hardened full-64K scan caught them.
    mov bx, 0x0010             ; 16 paras (256 bytes) of conventional memory
    mov ax, 0x0100             ; allocate DOS memory block
    int 0x31                   ; -> AX=real seg, DX=selector
    mov [0x602], ax            ; stash the DOS segment
    mov [0x604], dx            ; stash the selector

    mov bl, 0x1C               ; install a PM handler vector for INT 1Ch
    mov cx, 0x000F             ; selector = our PM code selector
    mov dx, .pmhandler         ; offset
    mov ax, 0x0205             ; set PM interrupt vector
    int 0x31
    mov bl, 0x1C
    mov ax, 0x0204             ; get PM interrupt vector -> CX:DX (verify round-trip)
    int 0x31
    mov [0x606], cx
    mov [0x608], dx

    mov ax, 0x0900             ; get + disable virtual interrupts
    int 0x31
    mov ax, 0x0901             ; get + enable virtual interrupts
    int 0x31

    mov word [.rmcs + 0x1C], 0x0900   ; RMCS.AX = AH=09 (print string)
    mov word [.rmcs + 0x24], 0x0100   ; RMCS.DS = 0x0100 (our real-mode segment)
    mov word [.rmcs + 0x14], .rmsg    ; RMCS.DX = message offset
    mov word [.rmcs + 0x20], 0x0202   ; RMCS.Flags
    push ds
    pop es                     ; ES = DS (the RMCS lives in our data)
    mov di, .rmcs              ; ES:DI -> RMCS
    mov ax, 0x0300             ; simulate real-mode interrupt
    mov bx, 0x0021             ; BL = INT 21h
    xor cx, cx                 ; no stack words to copy
    int 0x31                   ; -> host runs real-mode INT 21h AH=09 (prints .rmsg)
    mov dx, .pmsg              ; direct PM print to confirm we resumed cleanly
    mov ah, 0x09
    int 0x21

    ; --- run 44: INT 31h 0301 -- call a REAL-MODE far proc (PM->V86->PM) --
    ; The host switches us back to V86, runs .rmproc (which writes a sentinel and
    ; RETFs), then resumes us in PM. Proves the round-trip both ways.
    mov word [.rmcs2 + 0x2C], 0x0100   ; RMCS.CS = our real-mode segment
    mov word [.rmcs2 + 0x2A], .rmproc  ; RMCS.IP = proc offset
    mov word [.rmcs2 + 0x24], 0x0100   ; RMCS.DS
    mov word [.rmcs2 + 0x30], 0x0100   ; RMCS.SS
    mov word [.rmcs2 + 0x2E], 0xFC00   ; RMCS.SP = scratch stack
    mov word [.rmcs2 + 0x20], 0x0202   ; RMCS.Flags
    mov word [0x620], 0x0000           ; clear the sentinel (PM data selector)
    push ds
    pop es
    mov di, .rmcs2
    mov ax, 0x0301             ; call real-mode procedure
    xor bx, bx                 ; BH=0 flags
    xor cx, cx                 ; no stack words to copy
    int 0x31                   ; -> host runs .rmproc in V86, resumes us in PM
    mov ax, [0x620]            ; the RM proc should have written 0xBEEF
    cmp ax, 0xBEEF
    jne .rm_bad
    mov dx, .rmok
    mov ah, 0x09
    int 0x21
    jmp .rm_done
.rm_bad:
    mov dx, .rmbad
    mov ah, 0x09
    int 0x21
.rm_done:

    ; --- run 46: INT 31h 0303 real-mode callback (nested V86->PM->V86) ----
    ; Allocate a real-mode callback whose PM handler writes 0xCAFE, then use 0301
    ; to run an RM proc that far-calls it. Exercises PM->V86->PM->V86->PM.
    push ds                    ; DS:SI = PM handler (CS-based: handler is code)
    mov ax, cs                 ; cs = 0x0F (code selector)
    mov ds, ax
    mov si, .pmcb              ; handler offset
    mov ax, 0x0017             ; ES:DI = RMCS buffer (data selector)
    mov es, ax
    mov di, .cbrmcs
    mov ax, 0x0303             ; allocate real-mode callback
    int 0x31                   ; -> CX:DX = real-mode callback address
    pop ds                     ; restore DS = data selector
    mov [0x630], dx            ; store the callback far pointer (offset, then segment)
    mov [0x632], cx
    mov word [.rmcs2 + 0x2C], 0x0100   ; 0301 -> run .rmproc2 in V86
    mov word [.rmcs2 + 0x2A], .rmproc2
    mov word [.rmcs2 + 0x24], 0x0100
    mov word [.rmcs2 + 0x30], 0x0100
    mov word [.rmcs2 + 0x2E], 0xFC00
    mov word [.rmcs2 + 0x20], 0x0202
    mov word [0x626], 0x0000           ; clear the handler sentinel
    push ds
    pop es
    mov di, .rmcs2
    mov ax, 0x0301
    xor bx, bx
    xor cx, cx
    int 0x31                   ; -> .rmproc2 far-calls the callback; PM handler runs; resumes
    mov ax, [0x626]            ; the PM handler should have written 0xCAFE
    cmp ax, 0xCAFE
    jne .cb_bad
    mov dx, .cbok
    mov ah, 0x09
    int 0x21
    jmp .cb_done
.cb_bad:
    mov dx, .cbbad
    mov ah, 0x09
    int 0x21
.cb_done:
    mov ax, 0x4C00             ; terminate
    int 0x21
.pmspin:
    jmp .pmspin
.pmhandler:                    ; dummy PM handler target for the 0205/0204 round-trip (never called)
    iret
.rmproc:                       ; run in V86 by INT 31h 0301: DOS call + sentinel, far-return
    mov word [0x620], 0xBEEF
    mov ah, 0x02               ; DOS print char -- a REAL-MODE INT 21h during the excursion
    mov dl, 'R'                ; (proves the un/re-patch: this CD 21 vectors natively in V86)
    int 0x21
    retf
.rmproc2:                      ; V86 (via 0301): far-call the 0303 callback, then RETF
    call far [0x630]
    retf
.pmcb:                         ; PM 0303 callback handler (entered by the host on the RM far-call)
    mov word [0x626], 0xCAFE   ; write a sentinel (DS=0x17 base 0x1000 -> linear 0x1626)
    iret                       ; -> the host's PM-return catcher
.rmsg:  db '  [printed via INT 31h 0300 -> real-mode INT 21h AH=09]', 13, 10, '$'
.pmsg:  db 'DPMI: 0300 simulate-real-mode-int OK!', 13, 10, '$'
.rmok:  db 'DPMI: 0301 real-mode far-call OK (sentinel BEEF)!', 13, 10, '$'
.rmbad: db 'DPMI: 0301 FAILED (no sentinel).', 13, 10, '$'
.cbok:  db 'DPMI: 0303 real-mode callback OK (handler wrote CAFE)!', 13, 10, '$'
.cbbad: db 'DPMI: 0303 callback FAILED.', 13, 10, '$'
.rmcs:  times 50 db 0
.rmcs2: times 50 db 0
.cbrmcs: times 50 db 0

.switchfail:
    mov dx, msg_fail
    mov ah, 0x09
    int 0x21
    jmp .exit
.nodpmi:
    mov dx, msg_nodpmi
    mov ah, 0x09
    int 0x21
.exit:
    mov ax, 0x4C00
    int 0x21

entry:      dd 0
msg_start:  db 'DPMI spike: real-mode start, detecting via 2Fh/1687...', 13,10, '$'
msg_switch: db 'DPMI present -> far-calling the mode-switch entry...', 13,10, '$'
msg_fail:   db 'DPMI switch FAILED (still real mode).', 13,10, '$'
msg_nodpmi: db 'DPMI not present.', 13,10, '$'

    ;--- DPMIBACK.ASM: 16bit DPMI application written in MASM syntax.
    ;--- this sample temporarily switches back to real-mode.
    ;--- Third-party GH #2 test: authored by Japheth (Baron von Riedesel, author of
    ;--- HX / JWasm), sourced verbatim from the bttr-software DPMI tutorial. Assembled
    ;--- with the author's own assembler (JWasm) so it is a genuinely third-party
    ;--- artifact, NOT written to our host's expectations. It exercises the proper DPMI
    ;--- init protocol our hand-written clients cheated on: AH=4Ah shrink from SP, the
    ;--- SI host-data-paragraph allocation, BX preserved across the switch (real-mode CS),
    ;--- and an RMCS built on the STACK (ES:DI = SS:BP) driven through INT 31h 0301.
    ;--- Assemble:  jwasm -bin -Fo dpmiback.com dpmiback.asm

    LF  equ 10
    CR  equ 13

    .286
    .model tiny

    ;--- DPMI real-mode call structure
    RMCS struct
    rEDI    dd ?
    rESI    dd ?
    rEBP    dd ?
            dd ?
    rEBX    dd ?
    rEDX    dd ?
    rECX    dd ?
    rEAX    dd ?
    rFlags  dw ?
    rES     dw ?
    rDS     dw ?
    rFS     dw ?
    rGS     dw ?
    rIP     dw ?
    rCS     dw ?
    rSP     dw ?
    rSS     dw ?
    RMCS ends

    .data
    szWelcome db "welcome in protected-mode",CR,LF,0
    dBack db "back in real-mode",CR,LF,'$'
    dErr1 db "no DPMI host installed",CR,LF,'$'
    dErr2 db "not enough DOS memory for initialisation",CR,LF,'$'
    dErr3 db "DPMI initialisation failed",CR,LF,'$'

    .code
    org 100h

    start:
        pop ax
        mov bx, sp
        shr bx, 4
        jnz @F
        mov bx,1000h
    @@:
        mov ah, 4Ah
        int 21h

        mov ax, 1687h
        int 2Fh
        and ax, ax
        jnz nohost

        push es
        push di
        and si, si
        jz nomemneeded

        mov bx, si
        mov ah, 48h
        int 21h
        jc nomem
        mov es, ax

    nomemneeded:
        mov bp, sp
        mov bx, cs
        mov ax, 0000
        call far ptr [bp]
        jc initfailed

        push bx
        mov si, offset szWelcome
        call printstring
        pop bx

        sub sp, sizeof RMCS
        mov bp,sp
        mov [bp].RMCS.rIP, offset backtoreal
        mov [bp].RMCS.rCS, bx
        mov [bp].RMCS.rFlags, 0
        lea ax,[bp-20h]
        mov [bp].RMCS.rSP, ax
        mov [bp].RMCS.rSS, bx
        xor bx,bx
        xor cx,cx
        mov di,bp
        push ss
        pop es
        mov ax,0301h
        int 31h
        mov ax, 4C00h
        int 21h

    backtoreal:
        push cs
        pop ds
        mov dx,offset dBack
        mov ah,9
        int 21h
        retf

    nohost:
        mov dx, offset dErr1
        jmp error
    nomem:
        mov dx, offset dErr2
        jmp error
    initfailed:
        mov dx, offset dErr3
    error:
        push cs
        pop ds
        mov ah, 9
        int 21h
        mov ax, 4C00h
        int 21h

    printstring:
        lodsb
        and al,al
        jz stringdone
        mov dl,al
        mov ah,2
        int 21h
        jmp printstring
    stringdone:
        ret

    end start

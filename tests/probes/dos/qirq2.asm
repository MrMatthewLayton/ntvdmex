; qirq2.asm -- CAN the kernel's APC reach a RUNNING guest, or only a trapping one?
;
; This settles the last live hypothesis behind the sound epic's blocker. Disassembly of
; the VdmQueueInterrupt APC's kernel routine (ntoskrnl 0x46fdfb) says its FIRST pass always
; takes the requeue branch -- it is entered with NormalContext = 0 and only a non-zero
; NormalContext reaches the dispatch -- re-queuing itself as a USER-mode APC. A user APC is
; only delivered to a thread in an alertable wait, and a thread spinning inside
; VdmStartExecution never is. If that reading is right, the service cannot preempt a
; running V86 guest at all: it can only take effect at the VDM's next kernel transition,
; which is exactly what we already had.
;
; THE DISCRIMINATOR: the host runs this probe with the kernel's virtual PIC retargeted to
; vector base 0x60, so IRQ 5 arrives as **INT 65h if the KERNEL dispatched it** and as
; **INT 0Dh if our own host injected it** (host injection keeps the PC's 8+irq mapping).
; Both are hooked, with separate counters. The main line records which PHASE it is in and
; the first ISR to fire latches that, so we learn not just whether but WHEN:
;
;   phase 1  a long spin in pure memory -- NO trap of any kind can occur here
;   phase 2  exactly ONE `INT 1Ah` -- a BOP, i.e. a kernel transition + a host turn
;   phase 3  a second pure-memory spin
;
; READING THE RESULT (printed as "c0d=.. c65=.. first=..") :
;   c65 > 0, first=1  -> the kernel DELIVERED ASYNCHRONOUSLY into a spinning guest. The
;                        APC is a real preemption lever; wire it up for real.
;   c65 > 0, first=2/3-> the kernel delivers, but only at a transition. Async is dead;
;                        take the forced-yield route instead.
;   c65 = 0, c0d > 0  -> only our own injection at the trap; the kernel never dispatched.
;   both 0            -> nothing was delivered at all.
;
; Assemble: nasm -f bin qirq2.asm -o qirq2.com
bits 16
org 0x100

SPIN_A  equ 0xA000                  ; ~3 s on the 3.3 GHz rig (dx units of 65536 inner)
SPIN_B  equ 0x4000                  ; ~1.2 s

start:
    cld
    mov dx, s_banner
    mov ah, 0x09
    int 0x21

    xor ax, ax
    mov es, ax
    cli
    mov word [es:0x0D*4], isr0d
    mov [es:0x0D*4+2], cs
    mov word [es:0x65*4], isr65
    mov [es:0x65*4+2], cs
    sti

    ; ---- phase 1: pure memory spin, cannot trap ----
    mov byte [phase], 1
    xor dx, dx
.a_outer:
    xor cx, cx
.a_inner:
    loop .a_inner
    inc dx
    cmp dx, SPIN_A
    jb .a_outer

    ; ---- phase 2: exactly one trap (BOP -> host turn + kernel transition) ----
    mov byte [phase], 2
    mov ah, 0x00
    int 0x1A                        ; read tick count; we do not care about the value

    ; ---- phase 3: pure memory spin again ----
    mov byte [phase], 3
    xor dx, dx
.b_outer:
    xor cx, cx
.b_inner:
    loop .b_inner
    inc dx
    cmp dx, SPIN_B
    jb .b_outer

    cli
    xor ax, ax
    mov es, ax
    mov word [es:0x0D*4], 0
    mov word [es:0x0D*4+2], 0
    mov word [es:0x65*4], 0
    mov word [es:0x65*4+2], 0
    sti

    mov dx, s_res
    mov ah, 0x09
    int 0x21
    mov al, [c0d]
    call puthex
    mov dx, s_c65
    mov ah, 0x09
    int 0x21
    mov al, [c65]
    call puthex
    mov dx, s_first
    mov ah, 0x09
    int 0x21
    mov al, [firstph]
    call puthex
    mov dx, s_crlf
    mov ah, 0x09
    int 0x21

    mov ax, 0x4C00
    int 0x21

; ---- ISRs: bump a counter, latch the phase of the FIRST delivery, nothing else ----
isr0d:
    inc byte [cs:c0d]
    cmp byte [cs:firstph], 0
    jne .out
    mov al, [cs:phase]
    mov [cs:firstph], al
.out:
    iret

isr65:
    inc byte [cs:c65]
    cmp byte [cs:firstph], 0
    jne .out
    mov al, [cs:phase]
    add al, 0x10                    ; 0x1n marks "the KERNEL delivered, in phase n"
    mov [cs:firstph], al
.out:
    iret

puthex:
    push ax
    shr al, 4
    call .nib
    pop ax
    and al, 0x0F
    call .nib
    ret
.nib:
    add al, '0'
    cmp al, '9'
    jbe .o
    add al, 7
.o:
    mov dl, al
    mov ah, 0x02
    int 0x21
    ret

s_banner db 'qirq2: can the kernel APC preempt a SPINNING guest? (INT 65h=kernel, 0Dh=host)',13,10,'$'
s_res    db 'QIRQ2 RESULT c0d=$'
s_c65    db ' c65=$'
s_first  db ' first=$'
s_crlf   db 13,10,'$'
phase    db 0
firstph  db 0
c0d      db 0
c65      db 0

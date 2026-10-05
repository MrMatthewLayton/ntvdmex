; qirq.asm -- ASYNC INTERRUPT PREEMPTION probe (GH #20/#21 blocker, session 11).
;
; THE QUESTION: can a hardware interrupt reach a V86 guest that is spinning in pure
; guest code and never traps? Our exec loop only regains control when the guest
; faults/BOPs, so a device IRQ raised from the host's audio thread is latched and
; never delivered -- Skyroads sits in its INT 1Ch handler waiting for the Sound
; Blaster's block-completion IRQ that we cannot inject (irqn_inj=0, session 10).
;
; The kernel-side lever recovered by RE this session is
; NtVdmControl(VdmQueueInterrupt=1, <thread handle>), which queues an APC to the VDM
; thread. Its kernel routine (ntoskrnl 0x46fdfb) reads the FIXED_NTVDMSTATE pending
; bits at [0x714] and the interrupted thread's trap frame, then either
;   - dispatches the interrupt itself through the kernel's virtual ICA (8259 model),
;     vectoring at the ICA's programmed base -- our ICA is zeroed, so base 0 => the
;     IRQ 5 we raise would arrive as INT 05h; or
;   - writes VTIB_EVENT(0x5A8)=3 and stops the VDM, handing our exec loop the
;     event-3 "interrupt pending" notification it already services -- in which case
;     the host injects it as INT 0Dh (8 + IRQ 5), its normal mapping.
;
; So this probe hooks BOTH vectors with distinct counters, and the one that fires
; tells us which kernel path we are on -- and therefore what to build:
;   c05 != 0  -> the kernel's ICA dispatched: program the virtual ICA properly.
;   c0d != 0  -> we were handed event 3: our existing injection path is enough.
;   both 0    -> the APC never delivered; the lever is elsewhere.
;
; The host raises IRQ 5 every ~250 ms (enabled by qimode.txt bit 2 on the share).
;
; DISCIPLINE: the wait loop must not trap, or the host's exec loop would get a turn
; and inject the latched IRQ by itself -- which would prove nothing. So the wait is a
; pure memory spin (no INT, no port I/O), and the ISRs only bump a counter and IRET:
; no EOI, because the EOI `OUT` would itself be a trap. The spin is bounded (~5 s on
; the 3.3 GHz rig) so a negative result still exits cleanly through DOS and leaves a
; readable log instead of hitting the harness's 30 s kill.
;
; Assemble: nasm -f bin qirq.asm -o qirq.com
bits 16
org 0x100

start:
    cld
    mov dx, s_banner
    mov ah, 0x09
    int 0x21

    ; Hook INT 05h and INT 0Dh in the IVT (see header: the vector that fires is the
    ; experiment's answer). Saved and restored so DOS gets its vectors back.
    xor ax, ax
    mov es, ax
    cli
    mov ax, [es:0x05*4]
    mov [old05], ax
    mov ax, [es:0x05*4+2]
    mov [old05+2], ax
    mov ax, [es:0x0D*4]
    mov [old0d], ax
    mov ax, [es:0x0D*4+2]
    mov [old0d+2], ax
    mov word [es:0x05*4], isr05
    mov [es:0x05*4+2], cs
    mov word [es:0x0D*4], isr0d
    mov [es:0x0D*4+2], cs
    sti

    ; ---- the pure-memory wait: no trap of any kind can happen in here ----
    xor dx, dx
.outer:
    xor cx, cx
.inner:
    mov al, [c05]
    or  al, [c0d]
    jnz .got
    loop .inner
    inc dx
    jnz .outer
    jmp .done                       ; spun out: nothing was ever delivered
.got:
    ; let a couple more land so the count is clearly not a one-off fluke
    xor dx, dx
.settle:
    xor cx, cx
.settle2:
    loop .settle2
    inc dx
    cmp dx, 0x2000
    jb .settle

.done:
    cli
    xor ax, ax
    mov es, ax
    mov ax, [old05]
    mov [es:0x05*4], ax
    mov ax, [old05+2]
    mov [es:0x05*4+2], ax
    mov ax, [old0d]
    mov [es:0x0D*4], ax
    mov ax, [old0d+2]
    mov [es:0x0D*4+2], ax
    sti

    mov dx, s_res
    mov ah, 0x09
    int 0x21
    mov al, [c05]
    call puthex
    mov dx, s_c0d
    mov ah, 0x09
    int 0x21
    mov al, [c0d]
    call puthex
    mov dx, s_crlf
    mov ah, 0x09
    int 0x21

    mov ax, 0x4C00
    int 0x21

; ---- ISRs: bump a counter, nothing else (no EOI -- an OUT would trap) ----
isr05:
    inc byte [cs:c05]
    iret
isr0d:
    inc byte [cs:c0d]
    iret

; ---- print AL as two hex digits ----
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
    jbe .out
    add al, 7
.out:
    mov dl, al
    mov ah, 0x02
    int 0x21
    ret

s_banner db 'qirq: async IRQ preemption probe (spinning in pure V86, IF=1)',13,10,'$'
s_res    db 'QIRQ RESULT c05=$'
s_c0d    db ' c0d=$'
s_crlf   db 13,10,'$'
c05      db 0
c0d      db 0
old05    dd 0
old0d    dd 0

; pmstep.asm -- WHERE, EXACTLY, DOES THE ONE FAULT PER PM ENTRY LAND?
;
; WHY THIS EXISTS. Under `pmkernel.flag` every PM entry produces EXACTLY ONE fault
; (six exc=0xC0000005, two 0xC000001E across dpmitest + pmtick), the guest resumes,
; and it then runs cleanly to its next BOP. Reading the fault addresses back against
; whatever code the client happened to have there gave four rules that all fit the
; ten samples I had:
;
;   H-FIRST   the fault is at the entry instruction            -> at = E+0
;   H-FIXED3  the kernel advances a fixed 3 bytes (its BOP is
;             `C4 C4 nn`; ours is two bytes)                   -> at = E+3
;   H-SECOND  the fault is at the SECOND instruction           -> at = E+2
;   H-STORE   the fault is at the first memory STORE           -> at = E+11
;
; The samples cannot separate them because the clients' instruction lengths happen to
; make several coincide. So stop reading incidental code and CHOOSE the code: put a
; known sequence at the PM entry whose four predicted addresses are all DIFFERENT.
; The VEH already logs the fault address, so this client needs no self-reporting --
; the answer is one `at=0x...` in the host log.
;
; THE LAYOUT AT THE FIRST PM ENTRY (E = the `jc` right after the mode-switch call,
; which is what the kernel is handed as the entry EIP -- confirmed by pmtick, whose
; entry 0 was its own `jc` at 0x127):
;
;   E+0    72 xx         jc noswitch       <- 2 bytes (nasm shortens it), which is
;                                              exactly the case that misbehaved
;   E+2    b8 11 11      mov ax,0x1111     <- 3 bytes, so E+3 is MID-INSTRUCTION
;   E+5    bb 22 22      mov bx,0x2222
;   E+8    b9 33 33      mov cx,0x3333
;   E+11   a3 xx xx      mov [marker],ax   <- the first STORE, deliberately last
;
; Read the log: at=E+0, E+2, E+3 or E+11 picks the rule outright. Anything else means
; all four are wrong, which is also worth knowing in one run.
;
; ► PREDICTION REGISTERED BEFORE THE RUN: E+3. It is the only rule that explains the
;   two samples that land one byte PAST a real instruction boundary, and those are the
;   two that desynchronise the guest and killed pmtick.com.
;
; Run with `pmkernel.flag`. Assemble: nasm -f bin -o pmstep.com pmstep.asm

bits 16
org 0x100

start:
    mov     dx, msg_start
    mov     ah, 0x09
    int     0x21

    mov     ah, 0x4A                ; shrink to 64 KB
    mov     bx, 0x1000
    int     0x21

    mov     ax, 0x1687              ; detect DPMI
    int     0x2F
    test    ax, ax
    jnz     nodpmi
    mov     [entry], di
    mov     [entry+2], es

    xor     ax, ax                  ; 16-bit client
    call    far [entry]
; ---------------------------------------------------------------------------
; EVERYTHING FROM HERE TO THE FIRST INT IS THE EXPERIMENT. Do not "tidy" it:
; the instruction LENGTHS are the measurement.
;   E+0   jc noswitch        2 bytes -- a SHORT jcc, matching the 2-byte entry
;                            instruction that produced the anomaly in both real
;                            samples. nasm shortens it whatever we write.
;   E+2   mov ax,0x1111      3 bytes -- so a fixed-3 advance lands at E+3, i.e.
;   E+5   mov bx,0x2222      3 bytes    INSIDE this one. That is the signature.
;   E+8   mov cx,0x3333      3 bytes
;   E+11  mov [marker],ax    3 bytes -- the first STORE, deliberately last
; ---------------------------------------------------------------------------
pm_entry:
    jc      noswitch                ; E+0
    mov     ax, 0x1111              ; E+2
    mov     bx, 0x2222              ; E+5
    mov     cx, 0x3333              ; E+8
    mov     [marker], ax            ; E+11  <- the first store
    mov     dx, msg_inpm            ; the first BOP follows, ending the entry
    mov     ah, 0x09
    int     0x21

    mov     ax, 0x4C00
    int     0x21

noswitch:
    mov     dx, msg_noswitch
    mov     ah, 0x09
    int     0x21
    mov     ax, 0x4C01
    int     0x21
nodpmi:
    mov     dx, msg_nodpmi
    mov     ah, 0x09
    int     0x21
    mov     ax, 0x4C02
    int     0x21

entry:          dd 0
marker:         dw 0

msg_start:      db 'PMSTEP: start', 13, 10, '$'
msg_inpm:       db 'PMSTEP: reached the print -- entry survived', 13, 10, '$'
msg_noswitch:   db 'PMSTEP: mode switch FAILED (CF=1)', 13, 10, '$'
msg_nodpmi:     db 'PMSTEP: no DPMI host', 13, 10, '$'

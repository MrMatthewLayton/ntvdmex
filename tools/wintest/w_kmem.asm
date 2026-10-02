; w_kmem.asm -- KERNEL's global and local heaps, as round trips. (GH #163)
;
; EXPECTED VALUES come from the Windows 3.1 SDK contract, NOT from a run: each case
; reduces a call to the part the documentation promises (nonzero / at least N /
; contents preserved / NULL on success). Raw values the contract leaves open
; (GlobalSize's rounding, GlobalFlags) are emitted as `*.raw` rows and their stock
; value is OWED -- tools/wintest/stock.sh, under supervision.
;
;   case                    expect  contract
;   kmem.galloc.ok          0001    GlobalAlloc(GMEM_MOVEABLE, 1000) -> a handle
;   kmem.gsize.ge           0001    GlobalSize >= the size asked ("may be larger")
;   kmem.gsize.raw          owed    the rounded size itself
;   kmem.glock.ok           0001    GlobalLock -> a non-NULL far pointer
;   kmem.gmem.persist       0001    bytes written under one lock read back under the next
;   kmem.ghandle.round      0001    GlobalHandle(selector) low word == the handle
;   kmem.gflags.raw         owed    GlobalFlags of the unlocked moveable block
;   kmem.grealloc.ok        0001    GlobalReAlloc(h, 4000, GMEM_MOVEABLE) -> a handle
;   kmem.grealloc.ge        0001    ...GlobalSize >= 4000
;   kmem.grealloc.keeps     0001    ...the first 1000 bytes survive the move
;   kmem.gfree              0000    GlobalFree -> NULL on success
;   kmem.zeroinit           0001    GMEM_ZEROINIT block reads all zero (256 bytes)
;   kmem.lalloc.ok          0001    LocalAlloc(LMEM_FIXED, 100) -> a near pointer
;   kmem.lsize.ge           0001    LocalSize >= 100
;   kmem.lmem.persist       0001    bytes written there read back
;   kmem.lfree              0000    LocalFree -> NULL on success
;
; build: tools/wintest/build.sh w_kmem     run: tools/wintest/run.sh w_kmem
        org     0
%include "w16.inc"
W16_HEAD
        IMP     K, 15, GLOBALALLOC
        IMP     K, 16, GLOBALREALLOC
        IMP     K, 17, GLOBALFREE
        IMP     K, 18, GLOBALLOCK
        IMP     K, 19, GLOBALUNLOCK
        IMP     K, 20, GLOBALSIZE
        IMP     K, 21, GLOBALHANDLE
        IMP     K, 22, GLOBALFLAGS
        IMP     K,  5, LOCALALLOC
        IMP     K,  7, LOCALFREE
        IMP     K, 10, LOCALSIZE
W16_IAT

GMEM_MOVEABLE   equ 0002h
GMEM_ZEROINIT   equ 0040h
H               equ D_T0                ; the moveable block's handle
H2              equ D_T0+2              ; the zero-init block's handle
LP              equ D_T0+4              ; a local pointer

; fill ES:0..CX-1 with (i & FFh) ^ 5Ah
fill:
        push    di
        push    cx
        xor     di, di
.f:     mov     ax, di
        xor     al, 5Ah
        mov     [es:di], al
        inc     di
        loop    .f
        pop     cx
        pop     di
        ret
; AX = 1 if ES:0..CX-1 still holds that pattern
check:
        push    di
        push    cx
        xor     di, di
.c:     mov     ax, di
        xor     al, 5Ah
        cmp     [es:di], al
        jne     .bad
        inc     di
        loop    .c
        mov     ax, 1
        jmp     .r
.bad:   xor     ax, ax
.r:     pop     cx
        pop     di
        ret

cases:
        push    word GMEM_MOVEABLE
        push    word 0
        push    word 1000
        API     GLOBALALLOC
        mov     [H], ax
        BOOLN
        OUT     "kmem.galloc.ok"
        cmp     word [H], 0
        je      .lheap

        push    word [H]
        API     GLOBALSIZE              ; DX:AX
        mov     bx, ax
        or      dx, dx
        jnz     .ge1
        cmp     bx, 1000
        jae     .ge1
        xor     ax, ax
        jmp     .ge1o
.ge1:   mov     ax, 1
.ge1o:  OUT     "kmem.gsize.ge"
        mov     ax, bx
        OUT     "kmem.gsize.raw"

        push    word [H]
        API     GLOBALLOCK              ; DX:AX = far pointer
        mov     es, dx
        mov     bx, ax
        or      ax, dx
        BOOLN
        OUT     "kmem.glock.ok"
        or      dx, dx
        jz      .free
        mov     cx, 1000
        call    fill
        push    word [H]
        API     GLOBALUNLOCK
        push    word [H]
        API     GLOBALLOCK
        mov     es, dx
        mov     cx, 1000
        call    check
        OUT     "kmem.gmem.persist"

        push    es                      ; the selector
        API     GLOBALHANDLE            ; AX = handle, DX = selector
        cmp     ax, [H]
        mov     ax, 0
        jne     .gh
        inc     ax
.gh:    OUT     "kmem.ghandle.round"
        push    word [H]
        API     GLOBALUNLOCK

        push    word [H]
        API     GLOBALFLAGS
        OUT     "kmem.gflags.raw"

        push    word [H]
        push    word 0
        push    word 4000
        push    word GMEM_MOVEABLE
        API     GLOBALREALLOC
        or      ax, ax
        jz      .rfail
        mov     [H], ax
.rfail: BOOLN
        OUT     "kmem.grealloc.ok"
        push    word [H]
        API     GLOBALSIZE
        or      dx, dx
        jnz     .ge2
        cmp     ax, 4000
        jae     .ge2
        xor     ax, ax
        jmp     .ge2o
.ge2:   mov     ax, 1
.ge2o:  OUT     "kmem.grealloc.ge"
        push    word [H]
        API     GLOBALLOCK
        mov     es, dx
        mov     cx, 1000
        call    check
        OUT     "kmem.grealloc.keeps"
        push    word [H]
        API     GLOBALUNLOCK

.free:  push    ds
        pop     es                      ; ⚠ never leave ES on a selector about to die
        push    word [H]
        API     GLOBALFREE
        OUT     "kmem.gfree"

        ; a GMEM_ZEROINIT block must read all zero
        push    word GMEM_MOVEABLE | GMEM_ZEROINIT
        push    word 0
        push    word 256
        API     GLOBALALLOC
        mov     [H2], ax
        or      ax, ax
        jz      .zbad
        push    ax
        API     GLOBALLOCK
        mov     es, dx
        xor     di, di
        mov     cx, 256
        xor     al, al
        repe    scasb
        mov     ax, 1
        je      .zok
.zbad:  xor     ax, ax
.zok:   OUT     "kmem.zeroinit"
        push    ds
        pop     es
        cmp     word [H2], 0
        je      .lheap
        push    word [H2]
        API     GLOBALUNLOCK
        push    word [H2]
        API     GLOBALFREE

.lheap: ; the task's own local heap (mkne.py gives DGROUP a 0x200-byte heap)
        push    word 0                  ; LMEM_FIXED
        push    word 100
        API     LOCALALLOC
        mov     [LP], ax
        BOOLN
        OUT     "kmem.lalloc.ok"
        cmp     word [LP], 0
        je      .done
        push    word [LP]
        API     LOCALSIZE
        cmp     ax, 100
        mov     ax, 0
        jb      .ls
        inc     ax
.ls:    OUT     "kmem.lsize.ge"
        push    ds
        pop     es
        mov     di, [LP]
        mov     cx, 100
        mov     al, 0C3h
        rep     stosb
        mov     di, [LP]
        mov     cx, 100
        repe    scasb
        mov     ax, 1
        je      .lp
        xor     ax, ax
.lp:    OUT     "kmem.lmem.persist"
        push    word [LP]
        API     LOCALFREE
        OUT     "kmem.lfree"
.done:  jmp     w16_fin

W16_TAIL "w16kmem"

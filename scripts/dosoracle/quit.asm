; quit.com -- exit QEMU from inside the guest.
;
; QEMU's isa-debug-exit device turns a port write into a host process exit with
; status (value << 1) | 1.  We write 0x2A so the harness sees 0x55 (85) and can
; tell "the guest asked to stop" apart from "QEMU died" or "the timeout fired".
;
; If the device is absent the OUT is a no-op on an unclaimed port, the RET runs,
; and we fall back to DOS -- so this is safe to leave in a batch file.
;
; nasm -f bin quit.asm -o quit.com

        org     100h

        mov     al, 2Ah
        out     0F4h, al
        ret

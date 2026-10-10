; w_kfile.asm -- KERNEL's file API: _lcreat/_lwrite/_llseek/_lread/_lclose, _lopen,
;               OpenFile's OF_EXIST / OF_DELETE, GetDriveType, the two directories.
;               (GH #163)
;
; ⚠ THE SCRATCH FILE IS ABSOLUTE, under the share's debug\out\ -- a RELATIVE name from
;   a Win16 task can land in the test machine's C:\WINDOWS (s86), and this test DELETES its file
;   at the end through OpenFile(OF_DELETE), which is itself a case. run.sh's caller
;   should still check debug\out\ for W16F.TMP afterwards: if the delete case failed,
;   the file is left there, visibly, rather than somewhere nobody looks.
;
;   case                    expect  contract (Windows 3.1 SDK)
;   kfile.lcreat.ok         0001    _lcreat -> a handle, not HFILE_ERROR (FFFFh)
;   kfile.lwrite            000A    _lwrite returns the bytes written (10)
;   kfile.llseek.end        000A    _llseek(h, 0, 2) returns the new offset = the size
;   kfile.lclose            0000    _lclose returns 0 on success
;   kfile.lopen.ok          0001    _lopen(OF_READ) -> a handle
;   kfile.lread             000A    _lread(h, buf, 100) returns 10 (short read at EOF)
;   kfile.lread.same        0001    ...and the 10 bytes are the ones written
;   kfile.llseek.set        0003    _llseek(h, 3, 0) returns 3
;   kfile.lread.at3         3334    _lread 2 bytes there: '3','4' (AH=byte 3, AL=byte 4)
;   kfile.lread.eof         0000    _lread at end of file returns 0
;   kfile.lclose2           0000
;   kfile.of.exist          0001    OpenFile(OF_EXIST) on it: not HFILE_ERROR
;   kfile.of.cbytes         0088    ...and OFSTRUCT.cBytes = sizeof(OFSTRUCT) = 136
;   kfile.of.delete         0001    OpenFile(OF_DELETE): not HFILE_ERROR
;   kfile.of.gone           FFFF    OpenFile(OF_EXIST) afterwards: HFILE_ERROR
;   kfile.of.errcode        0002    ...nErrCode = the DOS error, 2 = file not found
;   kfile.lopen.missing     FFFF    _lopen of the deleted file: HFILE_ERROR
;   kfile.drivetype.c       0003    GetDriveType(2) = DRIVE_FIXED for C:
;   kfile.windir.colon      0001    GetWindowsDirectory: "X:..." (a drive-rooted path)
;   kfile.windir.len        owed    its length (machine fact: "C:\WINDOWS" = 000A)
;   kfile.sysdir.len        owed    its length (machine fact: "C:\WINDOWS\SYSTEM" = 0011)
;   kfile.sysdir.colon      0001    GetSystemDirectory: the same shape
;
; build: tests/probes/win16/build.sh w_kfile     run: tests/probes/win16/run.sh w_kfile
        org     0
%include "w16.inc"
W16_HEAD
        IMP     K, 83, LCREAT
        IMP     K, 86, LWRITE
        IMP     K, 84, LLSEEK
        IMP     K, 81, LCLOSE
        IMP     K, 85, LOPEN
        IMP     K, 82, LREAD
        IMP     K, 74, OPENFILE
        IMP     K, 136, GETDRIVETYPE
        IMP     K, 134, GETWINDOWSDIRECTORY
        IMP     K, 135, GETSYSTEMDIRECTORY
W16_IAT

HF      equ     D_T0
OFS     equ     D_BUF                   ; OFSTRUCT, 136 bytes
RBUF    equ     D_BUF + 0x90            ; 100 bytes
DIRB    equ     D_BUF + 0x100           ; 144 bytes would overrun: ask for 0x90 at most

s_path: db 'C:\DOCUME~1\ALLUSE~1\DOCUME~1\NTVDMEX\DEBUG\OUT\W16F.TMP', 0
s_data: db '0123456789'

cases:
        PUSHCS  s_path
        push    word 0                  ; normal attributes
        API     LCREAT
        mov     [HF], ax
        inc     ax                      ; FFFF -> 0
        BOOLN
        OUT     "kfile.lcreat.ok"
        cmp     word [HF], 0FFFFh
        je      .reopen

        push    word [HF]
        PUSHCS  s_data
        push    word 10
        API     LWRITE
        OUT     "kfile.lwrite"
        push    word [HF]
        push    word 0
        push    word 0
        push    word 2                  ; from the end
        API     LLSEEK                  ; DX:AX
        OUT     "kfile.llseek.end"
        push    word [HF]
        API     LCLOSE
        OUT     "kfile.lclose"

.reopen:
        PUSHCS  s_path
        push    word 0                  ; OF_READ
        API     LOPEN
        mov     [HF], ax
        inc     ax
        BOOLN
        OUT     "kfile.lopen.ok"
        cmp     word [HF], 0FFFFh
        je      .of
        push    word [HF]
        PUSHDS  RBUF
        push    word 100
        API     LREAD
        OUT     "kfile.lread"
        push    ds
        pop     es
        mov     di, RBUF
        mov     si, s_data
        mov     cx, 10
.cmp:   mov     al, [cs:si]
        cmp     al, [es:di]
        jne     .diff
        inc     si
        inc     di
        loop    .cmp
        mov     ax, 1
        jmp     .same
.diff:  xor     ax, ax
.same:  OUT     "kfile.lread.same"
        push    word [HF]
        push    word 0
        push    word 3
        push    word 0                  ; from the start
        API     LLSEEK
        OUT     "kfile.llseek.set"
        mov     word [RBUF], 0
        push    word [HF]
        PUSHDS  RBUF
        push    word 2
        API     LREAD
        mov     ah, [RBUF]
        mov     al, [RBUF+1]
        OUT     "kfile.lread.at3"
        push    word [HF]
        push    word 0
        push    word 0
        push    word 2
        API     LLSEEK
        push    word [HF]
        PUSHDS  RBUF
        push    word 100
        API     LREAD
        OUT     "kfile.lread.eof"
        push    word [HF]
        API     LCLOSE
        OUT     "kfile.lclose2"

.of:    PUSHCS  s_path
        PUSHDS  OFS
        push    word 4000h              ; OF_EXIST
        API     OPENFILE
        inc     ax
        BOOLN
        OUT     "kfile.of.exist"
        xor     ah, ah
        mov     al, [OFS]               ; cBytes
        OUT     "kfile.of.cbytes"
        PUSHCS  s_path
        PUSHDS  OFS
        push    word 0200h              ; OF_DELETE
        API     OPENFILE
        inc     ax
        BOOLN
        OUT     "kfile.of.delete"
        PUSHCS  s_path
        PUSHDS  OFS
        push    word 4000h              ; OF_EXIST, again
        API     OPENFILE
        OUT     "kfile.of.gone"
        mov     ax, [OFS+2]             ; nErrCode (WORD at offset 2)
        OUT     "kfile.of.errcode"
        PUSHCS  s_path
        push    word 0
        API     LOPEN
        OUT     "kfile.lopen.missing"

        push    word 2                  ; C:
        API     GETDRIVETYPE
        OUT     "kfile.drivetype.c"

        PUSHDS  DIRB
        push    word 0x90
        API     GETWINDOWSDIRECTORY
        mov     bx, ax
        xor     ax, ax
        cmp     byte [DIRB+1], ':'
        jne     .wd
        inc     ax
.wd:    OUT     "kfile.windir.colon"
        mov     ax, bx
        OUT     "kfile.windir.len"
        mov     byte [DIRB+1], 0
        PUSHDS  DIRB
        push    word 0x90
        API     GETSYSTEMDIRECTORY
        OUT     "kfile.sysdir.len"
        xor     ax, ax
        cmp     byte [DIRB+1], ':'
        jne     .sd
        inc     ax
.sd:    OUT     "kfile.sysdir.colon"
        jmp     w16_fin

W16_TAIL "w16kfile"

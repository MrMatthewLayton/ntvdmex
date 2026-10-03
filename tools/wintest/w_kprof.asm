; w_kprof.asm -- the profile API, both halves, read back (#293, s89).
;
; The s89 inventory found WriteProfileString / WritePrivateProfileString unanswered
; with 14 shelf programs importing them. Charmap's write then landed in the right place
; (HKCU\Software\Microsoft\Charmap, through XP's IniFileMapping) but with an EMPTY
; value, which is either the argument order or Charmap's own data. This settles the
; argument order with values whose right answer is known, against stock:
;   write -> read back the same string; delete the key (NULL string) -> the default
;   comes back; the private-file twin; an integer through GetPrivateProfileInt; a
;   whole section deleted (NULL key) -> the default again.
; ⚠ The private file is in the rig folder, not on C:\ (one folder, nothing on C:).
; ⚠ The WIN.INI section name is not in IniFileMapping, so it goes to the file, and is
;   deleted again at the end.
; Ordinals read off guest/ne/krnl386.exe and user.exe (both name tables).

        org     0
%include "w16.inc"
W16_HEAD
        IMP     K, 58,  GETPROFILESTRING
        IMP     K, 59,  WRITEPROFILESTRING
        IMP     K, 128, GETPRIVATEPROFILESTRING
        IMP     K, 129, WRITEPRIVATEPROFILESTRING
        IMP     K, 127, GETPRIVATEPROFILEINT
        IMP     U, 430, LSTRCMP
W16_IAT

BUF     equ     D_BUF                   ; 64 bytes

s_sec:   db 'NtvdmexProbe', 0
s_key:   db 'K1', 0
s_hello: db 'hello', 0
s_dflt:  db 'dflt', 0
s_psec:  db 'Sec', 0
s_k2:    db 'K2', 0
s_world: db 'world', 0
s_n:     db 'N', 0
s_42:    db '42', 0
s_empty: db 0
s_file:  db 'C:\DOCUME~1\ALLUSE~1\DOCUME~1\NTVDMEX\DEBUG\OUT\W16PROF.INI', 0

; GetProfileString(sec, key, dflt, BUF, 64) -> AX = length
%macro GPS 2
        PUSHCS  s_sec
        PUSHCS  %1
        PUSHCS  %2
        PUSHDS  BUF
        mov     ax, 64
        push    ax
        API     GETPROFILESTRING
%endmacro

; lstrcmp(BUF, cs:str) -> sign
%macro CMPBUF 1
        PUSHDS  BUF
        PUSHCS  %1
        API     LSTRCMP
        SIGN
%endmacro

cases:
        ; 1. write, then read back
        PUSHCS  s_sec
        PUSHCS  s_key
        PUSHCS  s_hello
        API     WRITEPROFILESTRING
        BOOLN
        OUT     "kprof.write"                   ; 0001
        GPS     s_key, s_dflt
        OUT     "kprof.read.len"                ; 0005
        CMPBUF  s_hello
        OUT     "kprof.read.eq"                 ; 0000
        ; 2. a NULL string deletes the key -> the default comes back
        PUSHCS  s_sec
        PUSHCS  s_key
        xor     ax, ax
        push    ax
        push    ax
        API     WRITEPROFILESTRING
        BOOLN
        OUT     "kprof.delkey"                  ; 0001
        GPS     s_key, s_dflt
        OUT     "kprof.delkey.len"              ; 0004 ("dflt")
        CMPBUF  s_dflt
        OUT     "kprof.delkey.eq"               ; 0000
        ; 3. the private twin
        PUSHCS  s_psec
        PUSHCS  s_k2
        PUSHCS  s_world
        PUSHCS  s_file
        API     WRITEPRIVATEPROFILESTRING
        BOOLN
        OUT     "kprof.pwrite"                  ; 0001
        PUSHCS  s_psec
        PUSHCS  s_k2
        PUSHCS  s_empty
        PUSHDS  BUF
        mov     ax, 64
        push    ax
        PUSHCS  s_file
        API     GETPRIVATEPROFILESTRING
        OUT     "kprof.pread.len"               ; 0005
        CMPBUF  s_world
        OUT     "kprof.pread.eq"                ; 0000
        ; 4. an integer through the private file
        PUSHCS  s_psec
        PUSHCS  s_n
        PUSHCS  s_42
        PUSHCS  s_file
        API     WRITEPRIVATEPROFILESTRING
        PUSHCS  s_psec
        PUSHCS  s_n
        mov     ax, 7
        push    ax
        PUSHCS  s_file
        API     GETPRIVATEPROFILEINT
        OUT     "kprof.pint"                    ; 002A
        ; 5. a NULL key deletes the whole section -> the default
        PUSHCS  s_psec
        xor     ax, ax
        push    ax
        push    ax
        push    ax
        push    ax
        PUSHCS  s_file
        API     WRITEPRIVATEPROFILESTRING
        BOOLN
        OUT     "kprof.delsec"                  ; 0001
        PUSHCS  s_psec
        PUSHCS  s_n
        mov     ax, 7
        push    ax
        PUSHCS  s_file
        API     GETPRIVATEPROFILEINT
        OUT     "kprof.delsec.int"              ; 0007
        ; tidy WIN.INI: delete the probe's section
        PUSHCS  s_sec
        xor     ax, ax
        push    ax
        push    ax
        push    ax
        push    ax
        API     WRITEPROFILESTRING
        jmp     w16_fin

W16_TAIL "w16kprof"

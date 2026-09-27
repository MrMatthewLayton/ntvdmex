; p_fcb.com -- the FCB file interface: 0Fh, 10h, 11h, 12h, 13h, 16h, 29h.
;
; The pre-1983 interface.  Little 6.22-era software uses it -- but TREE.COM
; does (it asked for AH=11h), and it is 19 of the services 6.22 defines, so
; leaving it out caps DOS coverage at about 80%.  Feeds GH #36.
;
; Self-cleaning: creates its own file and deletes it.
;
; nasm -f bin p_fcb.asm -o p_fcb.com

        org     100h
        jmp     start
%include "probe.inc"

start:
        PROBE_BEGIN "fcb"

        ; ---- create the scratch file through the handle API first
        mov     ax, 3C00h
        mov     cx, 0
        mov     dx, hname
        int     21h
        mov     bx, ax
        mov     ax, 3E00h
        int     21h

        ; ---- 29h: parse "C:PZFCB.TMP" into an FCB.  DS:SI = text, ES:DI = FCB.
        mov     di, fcb
        mov     cx, 40
        mov     al, 0EEh
        push    es
        mov     bx, cs
        mov     es, bx
        cld
        rep     stosb
        pop     es

        POISON
        push    ds
        pop     es
        mov     si, pname
        mov     di, fcb
        mov     ax, 2900h
        int     21h
        call    probe_capture
        EMIT    "int21.29.parse", "AX"
        EMIT_BUF "fcb.parsed", fcb, 16

        ; ---- ★ 29h AND THE WILDCARD. DOS EXPANDS `*` INTO `?`s as it parses: the
        ; FCB has no room for a star, so "*.BAS" must land as ????????BAS and "*.*"
        ; as ???????????. A program that then matches directory entries against the
        ; parsed FCB -- which is what QBasic's Open dialog does to fill its file
        ; list -- matches NOTHING if the star is stored literally, and shows an
        ; empty list while the directory list beside it is perfectly correct.
        ; ⚠ ITS OWN BUFFER: parsing into the shared `fcb` left ??????????? behind and
        ; the open/close cases further down then failed on a wildcard FCB.
        mov     di, fcbw
        mov     cx, 40
        mov     al, 0EEh
        push    es
        mov     bx, cs
        mov     es, bx
        cld
        rep     stosb
        pop     es
        POISON
        push    ds
        pop     es
        mov     si, pwild
        mov     di, fcbw
        mov     ax, 2900h
        int     21h
        call    probe_capture
        EMIT    "int21.29.wild", "AX"
        EMIT_BUF "fcb.wild", fcbw, 12

        mov     di, fcbw
        mov     cx, 40
        mov     al, 0EEh
        push    es
        mov     bx, cs
        mov     es, bx
        cld
        rep     stosb
        pop     es
        POISON
        push    ds
        pop     es
        mov     si, pstar
        mov     di, fcbw
        mov     ax, 2900h
        int     21h
        call    probe_capture
        EMIT    "int21.29.starstar", "AX"
        EMIT_BUF "fcb.starstar", fcbw, 12

        ; The parse above deliberately used "C:" to exercise the drive field, and
        ; fcb.parsed proves it landed. But the scratch file was created in the
        ; CURRENT directory, which is not C:\ on every host, so clear the drive
        ; byte to "default" before opening it.
        mov     byte [fcb], 0

        ; ---- 0Fh: open it through that parsed FCB
        POISON
        mov     dx, fcb
        mov     ax, 0F00h
        int     21h
        call    probe_capture
        EMIT    "int21.0F.open", "AX"
        EMIT_BUF "fcb.opened", fcb, 37

        ; ---- 10h: close
        POISON
        mov     dx, fcb
        mov     ax, 1000h
        int     21h
        call    probe_capture
        EMIT    "int21.10.close", "AX"

        ; ---- 0Fh on a name that does not exist
        POISON
        mov     dx, fcbmiss
        mov     ax, 0F00h
        int     21h
        call    probe_capture
        EMIT    "int21.0F.missing", "AX"

        ; ---- 11h: find first through an FCB, into the DTA
        mov     di, dta
        mov     cx, 64
        mov     al, 0EEh
        push    es
        mov     bx, cs
        mov     es, bx
        cld
        rep     stosb
        pop     es
        mov     ah, 1Ah
        mov     dx, dta
        int     21h

        POISON
        mov     dx, fcbwild
        mov     ax, 1100h
        int     21h
        call    probe_capture
        ; AL is the result for every FCB call; CF is NOT -- the oracle returned
        ; CF=1 from a SUCCESSFUL open, so carry is undefined here and comparing
        ; it would flag noise as a defect.
        EMIT    "int21.11.findfirst", "AX"
        EMIT_BUF "fcb.dta.found", dta, 40

        ; ---- 11h with a pattern that matches nothing
        POISON
        mov     dx, fcbnone
        mov     ax, 1100h
        int     21h
        call    probe_capture
        EMIT    "int21.11.nomatch", "AX"

        ; ---- 13h: delete through an FCB
        POISON
        mov     dx, fcbdel
        mov     ax, 1300h
        int     21h
        call    probe_capture
        EMIT    "int21.13.delete", "AX"

        ; ---- 13h again: it is gone now
        POISON
        mov     dx, fcbdel
        mov     ax, 1300h
        int     21h
        call    probe_capture
        EMIT    "int21.13.missing", "AX"

        ; ---- ★ 29h's AL CONTROL BITS, AND WHERE SI ENDS UP. (s81 sweep) XP's own
        ; COMMAND.COM builds DIR's search FCB by pre-filling ???????????, then parsing
        ; "*" with AL=0Eh -- bit 3 "leave the extension alone if none is given". A
        ; parser that ignores the bits blanks the extension, the search template
        ; becomes ????????+3 spaces, and DIR lists only `.` and `..`.
        ;   keepext: FCB = 05 'KEEPNAME' 'EXT', parse "*"<CR>, AL=0Eh
        ;   keepall: same FCB, parse <CR>, AL=0Eh -- drive, name and ext all kept
        ;   blank:   same FCB, parse <CR>, AL=00h -- drive 0, name/ext blanked
        ;   si:      parse "ABC.TXT REST", AL=00h -- SI must end after "ABC.TXT"
        ;   sep:     parse ",;ABC", AL=01h -- leading separators skipped
%macro P29 4                                    ; label, text, AL, prefill?
        mov     di, fcbw
        mov     cx, 40
        mov     al, 0EEh
        push    es
        mov     bx, cs
        mov     es, bx
        cld
        rep     stosb
%if %4
        mov     si, fcbkeep
        mov     di, fcbw
        mov     cx, 12
        rep     movsb
%endif
        pop     es
        POISON
        push    ds
        pop     es
        mov     si, %2
        mov     di, fcbw
        mov     ax, 2900h | %3
        int     21h
        call    probe_capture
        EMIT    %1, "AX"
        EMIT    %1, "SI"
        EMIT_BUF %1, fcbw, 12
%endmacro
        P29 "int21.29.keepext", pstarcr, 0Eh, 1
        P29 "int21.29.keepall", pcr, 0Eh, 1
        P29 "int21.29.blank", pcr, 00h, 1
        P29 "int21.29.si", prest, 00h, 0
        P29 "int21.29.sep", psep, 01h, 0

        PROBE_END

hname   db 'PZFCB.TMP', 0
pname   db 'C:PZFCB.TMP', 0
; drive 0 = default, then 8-char name and 3-char extension, space padded
fcbmiss db 0, 'ZZNOSUCH', 'XYZ', 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0
fcbwild db 0, '????????', '???', 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0
fcbnone db 0, 'ZZNOMTCH', 'ZZZ', 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0
fcbdel  db 0, 'PZFCB   ', 'TMP', 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0
fcbw:
        times 40 db 0
pwild   db '*.BAS', 0
pstar   db '*.*', 0
pstarcr db '*', 0Dh, 0
pcr     db 0Dh, 0
prest   db 'ABC.TXT REST', 0
psep    db ',;ABC', 0
fcbkeep db 05h, 'KEEPNAME', 'EXT'
fcb:
        times 40 db 0
dta:
        times 64 db 0

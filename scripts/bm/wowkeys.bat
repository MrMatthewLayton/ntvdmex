@echo off
rem wowkeys.bat -- drive a RUNNING guest's window from the build machine, by
rem                keyboard, and screenshot what happened.  GH #128, session 44.
rem
rem   wowkeys.bat "Notepad - (Untitled)" 0x12 0x48 0x41
rem                ^ exact caption          ^Alt ^H   ^A     = Help > About Notepad
rem
rem WHY KEYS AND NOT PostMessage(WM_COMMAND). Posting the command straight at the
rem window would test the host's WM_COMMAND translation while SKIPPING the thing
rem that produces a WM_COMMAND in the first place -- the OS's own menu. Alt opens
rem the menu bar through DefWindowProc, the mnemonics walk it, and the WM_COMMAND
rem that comes out is the real one a finger would produce. That is the path a user
rem takes, so it is the path worth testing.
rem   ⚠ It only works if the host lets Alt REACH DefWindowProc. It did not until
rem     session 44 -- WM_SYSKEYDOWN was relayed to the guest and swallowed, so a
rem     perfect menu had no keyboard route into it at all. See wowwin.h.
rem
rem ⚠ THE CAPTION MUST BE EXACT -- FindWindow matches the whole string. Get it from
rem   `rigshot list`, which this script runs first and logs, so a miss says which
rem   captions were actually on screen rather than just "not found".
rem ⚠ IT LAUNCHES NOTHING AND KILLS NOTHING. Something must already be running --
rem   wowlive.bat or wowcompare.bat.
setlocal
set RES=C:\Documents and Settings\All Users\Documents\ntvdmex
set BM=%RES%\bm
set LOG=%RES%\wowkeys.txt
set CAP=%~1

del /q "%LOG%" >nul 2>&1
del /q "%RES%\wowkeys.bmp" >nul 2>&1
del /q "%RES%\wowkeys_done.txt" >nul 2>&1

echo ==== windows on screen BEFORE ==== > "%LOG%"
"%BM%\rigshot.exe" list >> "%LOG%" 2>&1

echo. >> "%LOG%"
echo ==== bringing "%CAP%" to the front ==== >> "%LOG%"
"%BM%\rigshot.exe" fg "%CAP%" >> "%LOG%" 2>&1
if errorlevel 1 (
    echo [wowkeys] !! CAPTION NOT FOUND -- see the list above for what IS there >> "%LOG%"
    echo done> "%RES%\wowkeys_done.txt"
    goto :eof
)
ping -n 3 127.0.0.1 >nul

rem -- One key at a time, with a pause between: a menu bar has to open before the
rem    next mnemonic means anything, and rigshot's own 120 ms is not enough for a
rem    guest whose message pump is a cooperative host.
for %%K in (%2 %3 %4 %5 %6 %7 %8 %9) do (
    if not "%%K"=="" (
        echo [wowkeys] key %%K >> "%LOG%"
        "%BM%\rigshot.exe" key %%K >> "%LOG%" 2>&1
        ping -n 3 127.0.0.1 >nul
    )
)

ping -n 4 127.0.0.1 >nul
echo. >> "%LOG%"
echo ==== windows on screen AFTER ==== >> "%LOG%"
"%BM%\rigshot.exe" list >> "%LOG%" 2>&1
"%BM%\rigshot.exe" shot "%RES%\wowkeys.bmp" >> "%LOG%" 2>&1
copy /y C:\ntvdmex\ntvdmhost.log "%RES%\wowkeys_host.txt" >nul 2>&1
echo done> "%RES%\wowkeys_done.txt"

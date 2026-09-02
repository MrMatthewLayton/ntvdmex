@echo off
rem stocksysedit.bat -- run SYSEDIT.EXE under STOCK ntvdm, as the ORACLE.
rem
rem THE QUESTION, and it is a narrow one. Under NTVDMEX, SYSEDIT loads
rem C:\WINDOWS\SYSTEM.INI and WIN.INI and says "Cannot read this file." about
rem C:\CONFIG.SYS and C:\AUTOEXEC.BAT -- which are 0 BYTES on this box (measured:
rem seek-to-end returns 0). A perfect correlation on four samples is a lead, not a
rem cause: an empty CONFIG.SYS is the NORMAL state on XP, so "the application says
rem that about an empty file" is a live possibility and has to be EXCLUDED rather
rem than assumed.
rem
rem THE DISCRIMINATOR is what stock's SYSEDIT puts on the screen with the same four
rem files: `rigshot list` names every top-level window, so a modal error box shows up
rem as a caption, and `rigshot shot` is the picture behind it. If stock shows the same
rem message, our behaviour is CORRECT and there is nothing here to fix.
rem
rem THE IFEO KEY IS RESTORED ON EVERY EXIT PATH. Leaving it absent would silently
rem turn every later test into a stock run, with logs that look entirely plausible --
rem the single most reliable way to waste a day on this project.
setlocal
set RES=C:\Documents and Settings\All Users\Documents\ntvdmex
set BM=%RES%\bm
set IFEO=HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Image File Execution Options\ntvdm.exe

taskkill /f /im ntvdmhost.exe >nul 2>&1
taskkill /f /im ntvdm.exe >nul 2>&1
taskkill /f /im sysedit.exe >nul 2>&1

rem -- DELETE THE DESTINATIONS FIRST. A stale artefact is worse than a missing one:
rem    an absent file is a loud failure, an hours-old one is a silent wrong answer.
del /q "%RES%\stocksysedit_windows.txt" >nul 2>&1
del /q "%RES%\stocksysedit.bmp" >nul 2>&1
del /q "%RES%\stocksysedit_done.txt" >nul 2>&1

rem -- Record the sizes the question is ABOUT, from the box itself, so the evidence
rem    and the claim about it live in the same file.
echo [stocksysedit] the four files SYSEDIT opens: > "%RES%\stocksysedit_state.txt"
dir C:\WINDOWS\SYSTEM.INI C:\WINDOWS\WIN.INI C:\CONFIG.SYS C:\AUTOEXEC.BAT >> "%RES%\stocksysedit_state.txt" 2>&1

reg delete "%IFEO%" /v Debugger /f >nul 2>&1
echo [stocksysedit] IFEO Debugger REMOVED -- this is a STOCK run >> "%RES%\stocksysedit_state.txt"

start "" C:\WINDOWS\SYSTEM32\SYSEDIT.EXE
ping -n 12 127.0.0.1 >nul

rem -- What is on the screen. `list` first: a modal error box IS a top-level window,
rem    so its caption answers the question even if the screenshot is unreadable.
"%BM%\rigshot.exe" list > "%RES%\stocksysedit_windows.txt" 2>&1
"%BM%\rigshot.exe" shot "%RES%\stocksysedit.bmp" >> "%RES%\stocksysedit_state.txt" 2>&1

taskkill /f /im ntvdm.exe >nul 2>&1
ping -n 3 127.0.0.1 >nul
reg add "%IFEO%" /v Debugger /t REG_SZ /d "C:\ntvdmex\ntvdmhost.exe" /f >nul
echo [stocksysedit] IFEO Debugger RESTORED >> "%RES%\stocksysedit_state.txt"
reg query "%IFEO%" /v Debugger >> "%RES%\stocksysedit_state.txt" 2>&1
echo done > "%RES%\stocksysedit_done.txt"

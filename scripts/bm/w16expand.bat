@echo off
rem w16expand.bat -- decompress the Windows 3.1x distribution files into C:\WIN16.
rem GH #128, session 43.
rem
rem WHY NOT XP's EXPAND.EXE. Measured: it does not understand KWAJ, which is what
rem these files are ("KWAJ" is literally the first four bytes of NOTEPAD.EX_). It
rem does not SAY so either -- it reports "25112 bytes copied" and writes the input
rem verbatim under a name with the underscore stripped. A tool that succeeds while
rem doing nothing is the worst kind, so the size check at the end of this file
rem exists to catch exactly that: an expanded file that is the same size as its
rem input was not expanded.
rem
rem WHY THE 16-BIT EXPAND.EXE FROM THE DISKS. It is Microsoft's own, it is on
rem Disk06 UNCOMPRESSED (it has to be -- it is what unpacks the rest), and it is a
rem plain DOS MZ program, so it understands the format its own installer wrote.
rem
rem WHY STOCK ntvdm AND NOT OURS. Ours cannot redirect DOS output yet and did not
rem produce the file when tried (session 43; see the console/stdio blocker in
rem docs/STATE.md). Running EXPAND under NTVDMEX is a good test to come BACK to --
rem it is a real DOS workload with arguments and file I/O -- but this script exists
rem to get the guests onto the box, and a tooling task should use the tool that
rem works.
rem
rem THE IFEO KEY IS RESTORED ON EVERY EXIT PATH. Leaving it absent silently turns
rem every later test into a stock run, with logs that look entirely plausible --
rem the single most reliable way to waste a day on this project. Check that
rem w16expand_state.txt says "IFEO Debugger RESTORED" before trusting any run made
rem afterwards.
setlocal
set RES=C:\Documents and Settings\All Users\Documents\ntvdmex
set OUT=C:\WIN16
set IFEO=HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Image File Execution Options\ntvdm.exe

del /q "%RES%\w16expand.txt" >nul 2>&1
del /q "%RES%\w16expand_state.txt" >nul 2>&1
del /q "%RES%\w16expand_done.txt" >nul 2>&1
if not exist %OUT% md %OUT%
del /q %OUT%\*.* >nul 2>&1

copy /y "%RES%\win16src\*.*" %OUT%\ >nul 2>&1

echo [w16expand] starting > "%RES%\w16expand_state.txt"
reg delete "%IFEO%" /v Debugger /f >nul 2>&1
echo [w16expand] IFEO Debugger REMOVED -- these are STOCK ntvdm runs >> "%RES%\w16expand_state.txt"

cd /d %OUT%
echo ==== expanding ==== > "%RES%\w16expand.txt"

rem -- EXPAND STRAIGHT TO THE FINAL NAME, one loop per extension. The first cut
rem    expanded everything to %%~nf.tmp and renamed afterwards, and %%~nf is the
rem    stem WITHOUT the extension -- so CALC.EX_ and CALC.HL_ both became CALC.tmp,
rem    the help file overwrote the program, and the rename then published a .HLP as
rem    CALC.EXE. Four files came out valid and they were exactly the four with no
rem    matching .HL_ in the set. A temp name that is not unique is not a temp name.
for %%f in (%OUT%\*.EX_) do echo --- %%~nxf >> "%RES%\w16expand.txt" & %OUT%\EXPAND.EXE %%f %OUT%\%%~nf.EXE >> "%RES%\w16expand.txt" 2>&1
for %%f in (%OUT%\*.DL_) do echo --- %%~nxf >> "%RES%\w16expand.txt" & %OUT%\EXPAND.EXE %%f %OUT%\%%~nf.DLL >> "%RES%\w16expand.txt" 2>&1
for %%f in (%OUT%\*.HL_) do echo --- %%~nxf >> "%RES%\w16expand.txt" & %OUT%\EXPAND.EXE %%f %OUT%\%%~nf.HLP >> "%RES%\w16expand.txt" 2>&1
for %%f in (%OUT%\*.FO_) do echo --- %%~nxf >> "%RES%\w16expand.txt" & %OUT%\EXPAND.EXE %%f %OUT%\%%~nf.FON >> "%RES%\w16expand.txt" 2>&1
for %%f in (%OUT%\*.DR_) do echo --- %%~nxf >> "%RES%\w16expand.txt" & %OUT%\EXPAND.EXE %%f %OUT%\%%~nf.DRV >> "%RES%\w16expand.txt" 2>&1

taskkill /f /im ntvdm.exe >nul 2>&1
ping -n 3 127.0.0.1 >nul
reg add "%IFEO%" /v Debugger /t REG_SZ /d "C:\ntvdmex\ntvdmhost.exe" /f >nul
echo [w16expand] IFEO Debugger RESTORED >> "%RES%\w16expand_state.txt"
reg query "%IFEO%" /v Debugger >> "%RES%\w16expand_state.txt" 2>&1

rem -- THE PROOF. Compressed input beside expanded output: if the two sizes match,
rem    nothing was decompressed and the run is a failure that looked like a success.
echo. >> "%RES%\w16expand.txt"
echo ==== compressed in, expanded out ==== >> "%RES%\w16expand.txt"
dir %OUT%\*.??_ >> "%RES%\w16expand.txt" 2>&1
dir %OUT%\*.EXE %OUT%\*.DLL %OUT%\*.HLP >> "%RES%\w16expand.txt" 2>&1

rem -- Ship the results back for the NE check, which a batch cannot do.
if not exist "%RES%\win16" md "%RES%\win16"
del /q "%RES%\win16\*.*" >nul 2>&1
copy /y %OUT%\*.EXE "%RES%\win16\" >nul 2>&1
copy /y %OUT%\*.DLL "%RES%\win16\" >nul 2>&1

echo done > "%RES%\w16expand_done.txt"

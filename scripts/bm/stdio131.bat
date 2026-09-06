@echo off
rem stdio131.bat -- GH #131, and the question is now a COMPARISON, not a hypothesis.
rem
rem A DOS program named at a cmd prompt with a redirect -- `prog > file` -- is the
rem entire shape this issue is about. Run exactly that twice: once through NTVDMEX
rem and once through STOCK ntvdm, with nothing else different. Stock is the oracle
rem for "what should happen", and this project has twice been saved by asking it
rem before calling something a defect.
rem
rem ⚠ NOT `start /wait`. `start` passes CREATE_NEW_CONSOLE, so the child gets a
rem   console of its own and every DOS run this project has ever logged reports
rem   `inherited console`. That launch shape flatters us and is not the one #131 is
rem   about. rt.bat uses it, which is why this gap survived fifty sessions unseen.
rem
rem ⚠ NO dosstub/target.txt EITHER. The program named on the command line WINS over
rem   target.txt (GH #130, session 52), so a dosstub launch runs dosstub -- which
rem   prints nothing, and whose empty output file is indistinguishable from the bug
rem   being tested. Name the real program.
setlocal
set RES=C:\Documents and Settings\All Users\Documents\ntvdmex
set BM=%RES%\bm
set K=HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Image File Execution Options\ntvdm.exe
taskkill /f /im ntvdmhost.exe >nul 2>&1
tskill ntvdmhost >nul 2>&1
taskkill /f /im ntvdm.exe >nul 2>&1
if not exist C:\ntvdmex md C:\ntvdmex
copy /y "%BM%\ntvdmhost.exe" C:\ntvdmex\ >nul
if not exist C:\test md C:\test
copy /y "%BM%\tests\hello.com" C:\test\ >nul
echo.> C:\ntvdmex\autoexit
del /q C:\ntvdmex\target.txt >nul 2>&1
del /q "%RES%\stdio131.txt" >nul 2>&1
del /q "%RES%\stdio131_done.txt" >nul 2>&1
del /q C:\test\out1.txt C:\test\out2.txt >nul 2>&1
cd /d C:\test

echo ==== GH #131: `hello.com ^> file` from cmd, NTVDMEX vs STOCK ====> "%RES%\stdio131.txt"

rem --- NTVDMEX ---------------------------------------------------------------
reg add "%K%" /v Debugger /t REG_SZ /d "C:\ntvdmex\ntvdmhost.exe" /f >nul
del /q C:\ntvdmex\ntvdmhost.log >nul 2>&1
C:\test\hello.com > C:\test\out1.txt 2>&1
echo. >> "%RES%\stdio131.txt"
echo ---- NTVDMEX ---- >> "%RES%\stdio131.txt"
if exist C:\test\out1.txt (dir /-c C:\test\out1.txt | findstr /c:"out1.txt" >> "%RES%\stdio131.txt") else (echo ^(no file at all^) >> "%RES%\stdio131.txt")
echo [contents] >> "%RES%\stdio131.txt"
type C:\test\out1.txt >> "%RES%\stdio131.txt" 2>&1
if not exist C:\ntvdmex\ntvdmhost.log echo ^*^*^* NO HOST LOG -- stock ran this, the IFEO key is not in force >> "%RES%\stdio131.txt"
findstr /c:"STAGE1: program" /c:"STAGE1: vdm handles" /c:"STAGE1: stdout" /c:"STAGE2: stdout" /c:"STAGE2: start mode" C:\ntvdmex\ntvdmhost.log >> "%RES%\stdio131.txt" 2>&1
copy /y C:\ntvdmex\ntvdmhost.log "%RES%\stdio131_ours.log" >nul 2>&1

rem --- STOCK ntvdm: the oracle. The key is DELETED, not renamed -- Windows
rem     validates the VDM image's identity, so there is no other way to stand down.
reg delete "%K%" /v Debugger /f >nul 2>&1
del /q C:\ntvdmex\ntvdmhost.log >nul 2>&1
C:\test\hello.com > C:\test\out2.txt 2>&1
echo. >> "%RES%\stdio131.txt"
echo ---- STOCK ntvdm ---- >> "%RES%\stdio131.txt"
if exist C:\test\out2.txt (dir /-c C:\test\out2.txt | findstr /c:"out2.txt" >> "%RES%\stdio131.txt") else (echo ^(no file at all^) >> "%RES%\stdio131.txt")
echo [contents] >> "%RES%\stdio131.txt"
type C:\test\out2.txt >> "%RES%\stdio131.txt" 2>&1
if exist C:\ntvdmex\ntvdmhost.log echo ^*^*^* A HOST LOG EXISTS -- OURS ran this, the key was not removed >> "%RES%\stdio131.txt"

rem --- put the machine back the way we found it, and PROVE it ------------------
reg add "%K%" /v Debugger /t REG_SZ /d "C:\ntvdmex\ntvdmhost.exe" /f >nul
echo. >> "%RES%\stdio131.txt"
echo ---- the IFEO key, restored ---- >> "%RES%\stdio131.txt"
reg query "%K%" /v Debugger >> "%RES%\stdio131.txt" 2>&1
echo done> "%RES%\stdio131_done.txt"
endlocal

@echo off
rem verver.bat -- GH #28 acceptance: the DOS version is CONFIGURABLE, proved by
rem what the GUEST reports, not by what the host says it applied. Three cases in
rem ONE batch. Oracle (real MS-DOS 6.22): INT21.30 major=06 minor=16.
setlocal
set RES=C:\Documents and Settings\All Users\Documents\ntvdmex
set OUT=%RES%\verver.txt
del /q "%OUT%" >nul 2>&1
del /q "%RES%\verver_done.txt" >nul 2>&1

echo ==== CASE 1: dosver.txt = 6.22  (expect major=06 minor=16) ==== > "%OUT%"
echo 6.22> "%RES%\dosver.txt"
call C:\WINDOWS\rt.bat dosver.com
findstr /c:"INT21.30" /c:"INT21.3306" /c:"dosver override" "%RES%\result_dosver.com.log" >> "%OUT%" 2>&1

echo ==== CASE 2: dosver.txt = 3.31  (expect major=03 minor=1f) ==== >> "%OUT%"
echo 3.31> "%RES%\dosver.txt"
call C:\WINDOWS\rt.bat dosver.com
findstr /c:"INT21.30" /c:"INT21.3306" /c:"dosver override" "%RES%\result_dosver.com.log" >> "%OUT%" 2>&1

echo ==== CASE 3: no dosver.txt -- registry default ==== >> "%OUT%"
del /q "%RES%\dosver.txt" >nul 2>&1
call C:\WINDOWS\rt.bat dosver.com
findstr /c:"INT21.30" /c:"INT21.3306" /c:"dosver override" "%RES%\result_dosver.com.log" >> "%OUT%" 2>&1

echo DONE > "%RES%\verver_done.txt"

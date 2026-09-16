@echo off
rem qbmake.bat -- "QBasic cannot build EXEs" (s72, user-reported), reproduced headless.
rem
rem   qbmake.bat cli host      BC then LINK from a command line, under NTVDMEX
rem   qbmake.bat cli stock     the same two commands under STOCK ntvdm (the oracle)
rem   qbmake.bat qb            QB.EXE itself: Run > Make EXE File..., by key script,
rem                            with every INT 21h traced (the user's actual shape)
rem
rem What the user left behind in demo\msdos\qb45 on the 15th: CAVE.OBJ compiled /O
rem (stand-alone, default library BCOM45), CAVE.EXE of 3,772 bytes with NO relocations
rem (a stand-alone QB EXE carries the ~50 KB BCOM runtime -- this one was linked
rem WITHOUT the library), and QB's LINK response file ~QBLNK.TMP not cleaned up.
rem QB.INI's LIB path is C:\LIB, which does not exist on this box; the library is in
rem qb45\LIB\. Whether stock LINK finds it is the oracle question.
rem
rem Outputs land in debug\out\: qbmake_<arm>.txt (the report), qbmake_done.txt.
rem Build products are T_CAVE.* in qb45\, never the user's CAVE.* -- those are evidence.
set SH=C:\Documents and Settings\All Users\Documents\ntvdmex
set BIN=%SH%\bin
set RIG=%SH%\debug\rig
set CFG=%SH%\cfg
set OUT=%SH%\debug\out
set QB=%SH%\demo\msdos\qb45
set IFEO=HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Image File Execution Options\ntvdm.exe
set ARM=%1
set WHO=%2
if "%ARM%"=="" set ARM=cli
if "%WHO%"=="" set WHO=host
set R=%OUT%\qbmake_%ARM%_%WHO%%3.txt
if "%ARM%"=="qb" set R=%OUT%\qbmake_qb.txt
del /q "%OUT%\qbmake_done.txt" >nul 2>&1
> "%R%" echo == qbmake %ARM% %WHO%  %DATE% %TIME%
if not exist "%QB%\QB.EXE" (
  >> "%R%" echo NO QB AT %QB%
  goto done
)
taskkill /f /im ntvdmhost.exe >nul 2>&1
taskkill /f /im ntvdm.exe >nul 2>&1
del /q "%OUT%\startfail.txt" >nul 2>&1
del /q "%QB%\T_CAVE.*" >nul 2>&1
if "%ARM%"=="qb" goto qb

rem ---- cli: BC then LINK, named at a cmd line with a redirect (GH #131 shape) ----
if "%WHO%"=="stock" (
  reg delete "%IFEO%" /v Debugger /f >nul 2>&1
  >> "%R%" echo [stock] IFEO Debugger removed
) else (
  reg add "%IFEO%" /v Debugger /t REG_SZ /d "\"%BIN%\ntvdmhost.exe\"" /f >nul
  del /q "%OUT%\ntvdmhost.log" >nul 2>&1
  > "%CFG%\autoexit" echo.
  > "%CFG%\dostrace.flag" echo.
)
cd /d "%QB%"
rem LIB is what QB's Options > Set Paths would hand the linker; the OBJ names BCOM45
rem as its default library and LINK looks in the cwd, then along LIB. %3=long hands
rem it the long name with spaces (what QB.INI would say if it pointed here at all);
rem the default is the 8.3 form a 1988 linker can actually parse.
for %%i in ("%QB%") do set QB8=%%~si
set LIB=%QB8%\LIB
if "%3"=="long" set LIB=%QB%\LIB
>> "%R%" echo LIB=%LIB%
>> "%R%" echo -- BC PERSONAL\CAVE.BAS, T_CAVE.OBJ /O;
BC.EXE PERSONAL\CAVE.BAS, T_CAVE.OBJ /O; > "%OUT%\qbmake_bc_%WHO%.txt" 2>&1
>> "%R%" echo rc=%ERRORLEVEL%
type "%OUT%\qbmake_bc_%WHO%.txt" >> "%R%"
dir /-c T_CAVE.OBJ 2>nul | findstr /c:"T_CAVE" >> "%R%"
if not exist T_CAVE.OBJ >> "%R%" echo *** NO T_CAVE.OBJ
>> "%R%" echo -- LINK /EX T_CAVE.OBJ, T_CAVE.EXE, NUL, ;
LINK.EXE /EX T_CAVE.OBJ, T_CAVE.EXE, NUL, ; > "%OUT%\qbmake_link_%WHO%.txt" 2>&1 < nul
>> "%R%" echo rc=%ERRORLEVEL%
type "%OUT%\qbmake_link_%WHO%.txt" >> "%R%"
dir /-c T_CAVE.EXE 2>nul | findstr /c:"T_CAVE" >> "%R%"
if not exist T_CAVE.EXE >> "%R%" echo *** NO T_CAVE.EXE
if "%WHO%"=="stock" goto restore
del /q "%CFG%\autoexit" >nul 2>&1
del /q "%CFG%\dostrace.flag" >nul 2>&1
copy /y "%OUT%\ntvdmhost.log" "%OUT%\qbmake_cli_host.log" >nul 2>&1
if not exist "%OUT%\ntvdmhost.log" >> "%R%" echo *** NO HOST LOG -- stock ran this
goto done

rem ---- qb: QB.EXE loads PERSONAL\CAVE.BAS; Alt+R, Down x3 (Make EXE File... -- this
rem      QB.INI has Easy Menus: Start, Restart, Continue, Make EXE File), Enter,
rem      Enter (< Make EXE >, the default: EXE requiring BRUN45.EXE). BC and LINK are
rem      then EXEC'd by QB. Screens are dumped as text about once a second. ----
:qb
reg add "%IFEO%" /v Debugger /t REG_SZ /d "\"%BIN%\ntvdmhost.exe\"" /f >nul
del /q "%OUT%\ntvdmhost.log" >nul 2>&1
del /q "%OUT%\shot??.bmp" >nul 2>&1
del /q "%OUT%\shot??.txt" >nul 2>&1
del /q "%QB%\~QB*.TMP" >nul 2>&1
> "%CFG%\target.txt" echo "%QB%\QB.EXE" PERSONAL\T_CAVE.BAS
copy /y "%QB%\Personal\CAVE.BAS" "%QB%\Personal\T_CAVE.BAS" >nul
> "%CFG%\qimode.txt" echo 20
> "%CFG%\dostrace.flag" echo.
> "%CFG%\textdump.flag" echo.
> "%CFG%\capture.flag" echo 150
if exist "%RIG%\qbkeys_make.txt" (
  copy /y "%RIG%\qbkeys_make.txt" "%CFG%\keys.txt" >nul
) else (
  > "%CFG%\keys.txt" echo w6000 01 w1500 d38 13 u38 w1000 e50 e50 e50 w1000 1c w3000 1c w45000 1c w2000 01 w1000 d38 21 u38 w1000 2d w3000
)
> "%CFG%\autoexit" echo.
rem LIB reaches QB through the launcher's environment (s73), and QB hands its own
rem environment to BC and LINK. This is what QB's SETUP put in AUTOEXEC.BAT on a
rem real DOS box (SET LIB=C:\QB45\LIB); %2=nolib omits it to reproduce the user's box.
for %%i in ("%QB%") do set QB8=%%~si
set LIB=%QB8%\LIB
if "%2"=="nolib" set LIB=
if "%2"=="nolib" set R=%OUT%\qbmake_qb_nolib.txt
cd /d "%QB%"
start /wait "" "%RIG%\dosstub.com"
copy /y "%OUT%\ntvdmhost.log" "%OUT%\qbmake_qb.log" >nul 2>&1
for %%f in ("%OUT%\shot??.bmp" "%OUT%\shot??.txt") do copy /y "%%f" "%OUT%\qbmake_%%~nxf" >nul 2>&1
del /q "%CFG%\autoexit" "%CFG%\keys.txt" "%CFG%\qimode.txt" "%CFG%\dostrace.flag" "%CFG%\textdump.flag" "%CFG%\capture.flag" "%CFG%\target.txt" >nul 2>&1
>> "%R%" echo -- products in qb45\:
dir /-c "%QB%\T_CAVE.*" "%QB%\~QB*.TMP" 2>nul | findstr /i /c:"T_CAVE" /c:".TMP" >> "%R%"
for %%f in ("%QB%\~QB*.TMP") do (
  >> "%R%" echo -- %%~nxf:
  type "%%f" >> "%R%"
)
>> "%R%" echo -- STAGE lines:
findstr /c:"STAGE0: root" /c:"STAGE1: program" /c:"STAGE2: exec loop" /c:"HOSTFAULT" /c:"EXEC" "%OUT%\ntvdmhost.log" >> "%R%" 2>&1
goto done

:restore
reg add "%IFEO%" /v Debugger /t REG_SZ /d "\"%BIN%\ntvdmhost.exe\"" /f >nul
>> "%R%" echo [stock] IFEO Debugger restored
reg query "%IFEO%" /v Debugger >> "%R%" 2>&1
:done
>> "%R%" echo == done %TIME%
> "%OUT%\qbmake_done.txt" echo done

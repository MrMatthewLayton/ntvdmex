@echo off
rem QB45.BAT -- start QuickBASIC 4.5 with LIB and INCLUDE pointing at this folder,
rem in 8.3 form, so Run > Make EXE File finds BCOM45.LIB / BRUN45.LIB and the
rem linker can read the path. This is what QB's own SETUP put in AUTOEXEC.BAT.
rem   QB45          the editor  (QB alone would run QB.EXE: .EXE beats .BAT)
rem   QB45 PROG.BAS the editor with a program loaded
for %%i in ("%~dp0.") do set QBDIR=%%~si
set LIB=%QBDIR%\LIB
set INCLUDE=%QBDIR%\INC
cd /d "%~dp0"
QB.EXE %1 %2 %3

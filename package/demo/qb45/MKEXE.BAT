@echo off
rem MKEXE.BAT <name>  -- compile <name>.BAS to a standalone <name>.EXE, no QB UI.
rem   MKEXE CAVE            builds CAVE.EXE from CAVE.BAS (or Personal\CAVE.BAS)
rem Sets LIB to this folder's LIB dir (8.3) so LINK finds BCOM45.LIB -- the one
rem thing a build needs, and the thing QB launched as plain QB.EXE does not have.
if "%1"=="" echo usage: MKEXE ^<name^>  (compiles ^<name^>.BAS to ^<name^>.EXE)& goto :eof
for %%i in ("%~dp0.") do set QBDIR=%%~si
set LIB=%QBDIR%\LIB
set INCLUDE=%QBDIR%\INC
cd /d "%~dp0"
set SRC=%1.BAS
if not exist "%SRC%" if exist "Personal\%1.BAS" set SRC=Personal\%1.BAS
if not exist "%SRC%" echo MKEXE: no %1.BAS here or in Personal\& goto :eof
echo Compiling %SRC% ...
BC.EXE %SRC%, %1.OBJ /O;
if errorlevel 1 echo MKEXE: BC failed& goto :eof
echo Linking %1.EXE ...
LINK.EXE /EX %1.OBJ, %1.EXE, NUL, ;
if exist "%1.EXE" echo MKEXE: wrote %1.EXE & dir %1.EXE | find "%1"

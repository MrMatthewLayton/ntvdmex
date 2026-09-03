@echo off
rem w16grab.bat -- pull Win16 programs off a Windows 3.1x floppy (or a folder of
rem them) and expand them into C:\WIN16. GH #128, session 43.
rem
rem THE FORMAT, MEASURED RATHER THAN ASSUMED. Windows 3.1x distribution files are
rem single-file compressed, not cabinets: the last character of the extension is
rem replaced by an underscore (NOTEPAD.EX_) and the payload carries its own name.
rem This file first said that format was SZDD. It is not -- the first four bytes of
rem NOTEPAD.EX_ off the Windows 3.11 disks are "KWAJ", the other Microsoft
rem single-file format, the one MS-DOS 6's EXPAND.EXE was built for. Whether XP's
rem EXPAND.EXE still understands it is a question about THIS machine, so the run
rem below answers it rather than this comment claiming to.
rem   -r  = restore each file's real name from its header, rather than guessing it
rem         from the underscore. Guessing is how NOTEPAD.EX_ becomes NOTEPAD.EX.
rem
rem   w16grab.bat            -- share's win16src\ if it has anything, else A:\
rem   w16grab.bat D:\WIN311  -- a folder instead
rem
rem YOU DO NOT NEED TO INSTALL WINDOWS 3.11. WOW *is* the Windows 3.1 environment;
rem a Win16 .EXE runs against krnl386/user/gdi, which are already on this machine.
rem Bring the .EXE, and any .DLL or .HLP it names, and nothing else.
setlocal
set RES=C:\Documents and Settings\All Users\Documents\ntvdmex
set OUT=C:\WIN16

rem -- WHERE TO READ FROM, in the order a run is most likely to want it:
rem      1. an explicit argument;
rem      2. the share's win16src\ -- files already pulled out of disk IMAGES by
rem         tools/fat12.py, which is how this gets driven from the dev machine;
rem      3. A:\, a real floppy in the drive.
rem    THE ARGUMENT FORM IS AWKWARD REMOTELY: controld runs this through
rem    WinExec("cmd /c ..."), and cmd mangles a command line carrying two quoted
rem    paths. That is why (2) exists -- it needs no argument at all.
set SRC=%~1
if not "%SRC%"=="" goto havesrc
set SRC=%RES%\win16src\
if exist "%RES%\win16src\*.*" goto havesrc
set SRC=A:\
:havesrc

if not exist %OUT% md %OUT%
del /q "%RES%\w16grab.txt" >nul 2>&1
del /q "%RES%\w16grab_done.txt" >nul 2>&1

echo [w16grab] source = %SRC%              > "%RES%\w16grab.txt"
echo [w16grab] destination = %OUT%        >> "%RES%\w16grab.txt"
echo.                                     >> "%RES%\w16grab.txt"
echo ==== what is on the source ====      >> "%RES%\w16grab.txt"
dir "%SRC%" >> "%RES%\w16grab.txt" 2>&1
echo.                                     >> "%RES%\w16grab.txt"

rem -- EXPAND EVERYTHING, ONE FILE AT A TIME. `expand -r "<dir>\*.*" <out>` is the
rem    documented bulk form and XP's expand.exe answers it with "Can't open input
rem    file" (measured, on a directory whose contents it had just listed), so the
rem    loop is not belt-and-braces -- it is the form that works. It also reports
rem    per file, which is what turns "it failed" into "these three failed".
echo ==== expand -r, per file ====        >> "%RES%\w16grab.txt"
for %%f in ("%SRC%*.*") do echo --- %%~nxf >> "%RES%\w16grab.txt" & expand -r "%%f" %OUT% >> "%RES%\w16grab.txt" 2>&1

echo.                                     >> "%RES%\w16grab.txt"
echo ==== what landed in %OUT% ====       >> "%RES%\w16grab.txt"
dir %OUT% >> "%RES%\w16grab.txt" 2>&1

rem -- COPY THEM BACK TO THE SHARE so the NE check can be done off-box against the
rem    file. A batch cannot read a header; tools/ne can, exactly.
if not exist "%RES%\win16" md "%RES%\win16"
del /q "%RES%\win16\*.*" >nul 2>&1
copy /y "%OUT%\*.exe" "%RES%\win16\" >> "%RES%\w16grab.txt" 2>&1
copy /y "%OUT%\*.dll" "%RES%\win16\" >> "%RES%\w16grab.txt" 2>&1

echo done > "%RES%\w16grab_done.txt"

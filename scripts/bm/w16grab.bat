@echo off
rem w16grab.bat -- pull Win16 programs off a Windows 3.1x floppy (or a folder of
rem them) and expand them into C:\WIN16. GH #128, session 43.
rem
rem WHY THIS IS ONE COMMAND. Windows 3.1x distribution files are single-file
rem compressed, not cabinets: the last character of the extension is replaced by an
rem underscore (NOTEPAD.EX_, SHELL.DL_) and the payload carries its own original
rem name. XP's own EXPAND.EXE understands that format AND cabinets, so the same
rem invocation covers both and there is nothing to install.
rem   -r  = restore each file's real name from its header, rather than guessing it
rem         from the underscore. Guessing is how NOTEPAD.EX_ becomes NOTEPAD.EX.
rem
rem   w16grab.bat            -- read A:\
rem   w16grab.bat D:\WIN311  -- read a folder instead (imaged disks, a share, a CD)
rem
rem YOU DO NOT NEED TO INSTALL WINDOWS 3.11. WOW *is* the Windows 3.1 environment;
rem a Win16 .EXE runs against krnl386/user/gdi, which are already on this machine.
rem Bring the .EXE, and any .DLL or .HLP it names, and nothing else.
setlocal
set SRC=%1
if "%SRC%"=="" set SRC=A:\
set RES=C:\Documents and Settings\All Users\Documents\ntvdmex
set OUT=C:\WIN16

if not exist %OUT% md %OUT%
del /q "%RES%\w16grab.txt" >nul 2>&1
del /q "%RES%\w16grab_done.txt" >nul 2>&1

echo [w16grab] source = %SRC%              > "%RES%\w16grab.txt"
echo [w16grab] destination = %OUT%        >> "%RES%\w16grab.txt"
echo.                                     >> "%RES%\w16grab.txt"
echo ==== what is on the source ====      >> "%RES%\w16grab.txt"
dir "%SRC%" >> "%RES%\w16grab.txt" 2>&1
echo.                                     >> "%RES%\w16grab.txt"

rem -- EXPAND EVERYTHING, and let expand.exe decide what is compressed. A file that
rem    is already plain is copied through, so one pass handles a mixed disk.
echo ==== expand -r ====                  >> "%RES%\w16grab.txt"
expand -r "%SRC%*.*" %OUT% >> "%RES%\w16grab.txt" 2>&1

echo.                                     >> "%RES%\w16grab.txt"
echo ==== what landed in %OUT% ====       >> "%RES%\w16grab.txt"
dir %OUT% >> "%RES%\w16grab.txt" 2>&1

rem -- AND SAY WHICH OF THEM ARE ACTUALLY 16-BIT. A batch cannot read an NE header,
rem    so this only lists; the check itself is done off-box against the file. The
rem    listing is copied to the share so that can happen without a second trip.
echo done > "%RES%\w16grab_done.txt"

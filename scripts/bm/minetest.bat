@echo off
rem minetest.bat -- LAUNCH Minesweeper fresh and prove it reacts to a mouse click.
rem GH #128, session 50.
rem
rem ⚠ ONE BATCH, LAUNCH INCLUDED, AND THAT IS THE POINT. The first cut drove a
rem   guest that was ALREADY running and compared before/after: both shots came
rem   back byte-identical AND already showing a played board, because someone had
rem   played it between the launch and the test. The rig is not necessarily
rem   unattended, so a test that does not create the state it measures is
rem   measuring somebody else's.
rem ⚠ AND IT DELETES winmine.ini FIRST, so the window lands at the DEFAULT
rem   position every time. Minesweeper saves Xpos/Ypos on exit, so without this
rem   the coordinates below drift out from under the test after the first run.
setlocal
set RES=C:\Documents and Settings\All Users\Documents\ntvdmex
set BM=%RES%\bm

del /q "%RES%\minetest.txt" "%RES%\mine_before.bmp" "%RES%\mine_after.bmp" >nul 2>&1
del /q C:\WINDOWS\winmine.ini >nul 2>&1

call "%RES%\wowlive.bat" C:\WIN16\WINMINE.EXE
ping -n 4 127.0.0.1 >nul

tasklist /fi "imagename eq ntvdmhost.exe" | find "ntvdmhost" >nul
if errorlevel 1 (
    echo [minetest] !! NO HOST RUNNING -- nothing was clicked. > "%RES%\minetest.txt"
    goto :eof
)
echo [minetest] host is up >  "%RES%\minetest.txt"

"%BM%\rigshot.exe" fg "Minesweeper"
ping -n 2 127.0.0.1 >nul
"%BM%\rigshot.exe" shot "%RES%\mine_before.bmp"
ping -n 2 127.0.0.1 >nul

rem -- The centre of the 8x8 grid at the default window position (frame origin
rem    ~80,36; grid ~92,112 to ~222,268). One click there reveals at least one
rem    cell, and on an empty cell it cascades -- either way the board CANNOT look
rem    the same afterwards if mouse input is reaching the guest.
"%BM%\rigshot.exe" click 157 190
ping -n 4 127.0.0.1 >nul
"%BM%\rigshot.exe" click 125 145
ping -n 4 127.0.0.1 >nul

"%BM%\rigshot.exe" shot "%RES%\mine_after.bmp"
copy /y C:\ntvdmex\ntvdmhost.log "%RES%\minetest_host.txt" >nul 2>&1
echo [minetest] done -- mine_before.bmp vs mine_after.bmp MUST differ >> "%RES%\minetest.txt"

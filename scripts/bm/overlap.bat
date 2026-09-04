@echo off
rem overlap.bat -- launch Solitaire, then repeatedly cover and uncover it, which
rem is the workload the user describes as slow ("GDI redraw of overlapped WoW16
rem app windows"). Every raise forces the guest to repaint its whole table.
setlocal
set RES=C:\Documents and Settings\All Users\Documents\ntvdmex
set BM=%RES%\bm
call "%RES%\wowlive.bat" C:\WIN16\SOL.EXE
ping -n 4 127.0.0.1 >nul
for /L %%i in (1,1,10) do (
  "%BM%\rigshot.exe" fg "cmd.exe"
  ping -n 2 127.0.0.1 >nul
  "%BM%\rigshot.exe" fg "Solitaire"
  ping -n 2 127.0.0.1 >nul
)
copy /y C:\ntvdmex\ntvdmhost.log "%RES%\overlap_host.txt" >nul 2>&1
echo done > "%RES%\overlap.txt"

@echo off
rem ifeochk.bat -- READ-ONLY: what does the IFEO Debugger value say, and is a VDM up?
rem  !! It used to `reg add` C:\ntvdmex\ntvdmhost.exe first -- a path that no longer
rem    exists since the s61 relayout -- so RUNNING THE CHECK BROKE THE RIG. A check
rem    must not change what it checks; `rt.bat setup` is the thing that re-adds the key.
set SH=C:\Documents and Settings\All Users\Documents\ntvdmex
set OUT=%SH%\debug\out
set IFEO=HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Image File Execution Options\ntvdm.exe
if not exist "%OUT%" md "%OUT%"
> "%OUT%\ifeo_check.txt" reg query "%IFEO%" /v Debugger
tasklist /fi "imagename eq ntvdmhost.exe" >> "%OUT%\ifeo_check.txt" 2>&1
tasklist /fi "imagename eq ntvdm.exe" >> "%OUT%\ifeo_check.txt" 2>&1
if exist "%OUT%\startfail.txt" (echo startfail: & type "%OUT%\startfail.txt") >> "%OUT%\ifeo_check.txt"
echo DONE > "%OUT%\ifeo_done.txt"

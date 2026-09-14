NTVDMEX -- a replacement MS-DOS / Win16 virtual machine for Windows XP (32-bit)
==============================================================================

WHAT IT IS
  Windows XP runs MS-DOS and 16-bit Windows programs inside ntvdm.exe. NTVDMEX
  is a drop-in replacement: after install.bat, every such program runs under
  NTVDMEX instead, and uninstall.bat puts Windows' own ntvdm.exe back. Nothing
  in Windows is overwritten. The whole thing lives in this folder plus ONE
  registry value that install.bat adds and uninstall.bat removes.

REQUIREMENTS
  Windows XP, 32-bit, Service Pack 3. An administrator account for install.bat
  and uninstall.bat (they write HKEY_LOCAL_MACHINE). Running programs afterwards
  needs no special rights.

INSTALL
  1. Extract the zip anywhere -- the folder name and drive do not matter.
     Do not move the folder after installing; if you must, run uninstall.bat
     first, move it, then install.bat again.
  2. Run install.bat.
  3. Run smoke.bat. It runs the built-in self-test and tells you PASS or FAIL.
  4. Run any DOS or 16-bit Windows program as you normally would.

FILES
  install.bat      register NTVDMEX          (bm\ntvdmhost.exe /install)
  uninstall.bat    restore Windows' own VDM  (bm\ntvdmhost.exe /uninstall)
  status.bat       which VDM is in force, and the last run's log summary
  smoke.bat        the built-in self-test
  bm\              the program itself
  cfg\             optional settings files; empty means all defaults
  out\             the log of the LAST run (ntvdmhost.log) and screenshots

IF SOMETHING GOES WRONG
  - Run status.bat. It says whether NTVDMEX is installed and shows the key
    lines of the last log.
  - The log of the last run is out\ntvdmhost.log. It is the one file that
    explains a failure; send it back with a one-line description of what you
    ran and what you saw.
  - If DOS programs stop working entirely: NTVDMEX removes itself after three
    starts in a row that did not end cleanly (closing a game's window with the
    X counts as clean; a crash does not). Run status.bat -- if it says "not
    installed", run install.bat again.
  - Nothing works even after that: run uninstall.bat. Windows' own VDM is back
    and the machine is as it was.

WHAT TO EXPECT (tested by hand on real hardware before this build)
  MS-DOS: COMMAND.COM, DOOM (with sound and mouse), Skyroads, Lemmings, the
  QBasic and EDIT text-mode editors (typing, menus, mouse).
  Win16: Notepad and Paint from Windows 3.11 (Paint draws and saves files).
  Win16 programs use XP's own 16-bit system files from system32; nothing is
  bundled. Bring the programs themselves (NOTEPAD.EXE, PBRUSH.EXE ...).
  Programs outside that list may or may not work; the log says why when they
  do not.

KNOWN LIMITS
  Hardware-level access is slower than a real PC (every port access is a trap),
  so timing-critical games run at roughly a 386-class pace. There is no
  direct-memory-access hardware for user programs. A DOS program's screen
  output does not reach a command prompt's redirection (> file) yet.

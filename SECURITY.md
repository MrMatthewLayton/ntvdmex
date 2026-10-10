# Security

NTVDMEX runs with more reach than an ordinary program, and a report about it is welcome.

## What it does to a machine

- **Installing it sets one registry value**: an Image File Execution Options `Debugger`
  value on `ntvdm.exe`, under `HKLM`. While it is set, every 16-bit program on the machine
  runs under NTVDMEX instead of Windows' own NTVDM. `uninstall.bat` removes the value;
  `status.bat` says which is in charge.
- Nothing else is changed: no system file is replaced or patched, no driver is installed,
  and settings live under `HKCU\Software\NTVDMEX` and in the installation's `cfg\` folder.
- It runs the guest's 16-bit code on the real processor, in Virtual-8086 mode and in
  protected mode through its DPMI host, inside the process of the user who started the
  program -- with that user's rights, no more.
- It targets Windows XP, which has had no security updates since 2014. Do not run it, or
  XP, on a machine whose safety matters.

## Reporting a vulnerability

Report it privately through GitHub: on the repository's **Security** tab, choose
**Report a vulnerability**. Please do not open a public issue for it.

Say what an attacker can do and from where (a 16-bit program it runs, a file it reads, a
setting), the build (the zip's name or the commit), and how to reproduce it. You will get an
answer within two weeks. There is no bounty.

Worth reporting, for example: a 16-bit program that can run code outside its own process's
rights, write outside the memory NTVDMEX gives it in a way that takes over the host, or
leave the machine's DOS support broken after `uninstall.bat`.

## Supported versions

There has been no release yet. Fixes go into `main`.

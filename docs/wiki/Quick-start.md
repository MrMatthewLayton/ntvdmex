# Quick start and keyboard shortcuts

NTVDMEX replaces Windows XP's own `ntvdm.exe`. Once it is installed, every MS-DOS and
Windows 3.x program you start on the machine runs in NTVDMEX instead, with no extra step.

> The Help menu does not link to this page yet ([#151](https://github.com/MrMatthewLayton/ntvdmex/issues/151)).

## Install and uninstall

Unzip the release anywhere. Then, as an administrator:

| Command | What it does |
|---|---|
| `ntvdmhost.exe /install` | Makes NTVDMEX this machine's DOS machine. Stock NTVDM stays on disk, untouched. |
| `ntvdmhost.exe /status` | Says whether NTVDMEX, stock NTVDM or something else runs DOS programs here. |
| `ntvdmhost.exe /uninstall` | Gives DOS programs back to stock NTVDM. |

The install is one registry value (an Image File Execution Options `Debugger` entry on
`ntvdm.exe`), so uninstalling is complete. Don't move or delete the folder while it's
installed: Windows would then have nothing to start DOS programs with.

## Starting things

- **A DOS prompt:** open `ntvdmhost.exe` with no arguments. You get a shell (XP's own
  `COMMAND.COM` by default; *Settings > General* can choose another).
- **A DOS or Windows 3.x program:** start it the way you normally would: double-click it,
  type its name at a prompt, or use a shortcut.
- **File > Open Executable…** starts a program from inside the window.
- **File > Close Program** ends the running program and returns to the prompt it was
  started from. Its sound stops with it.

## Keyboard shortcuts

These are the only keys the host keeps for itself. Everything else goes to the program,
and **while the mouse is captured only Alt+Enter and the Windows key work**: the other
three then belong to the program (F11 is Doom's gamma).

| Key | Does |
|---|---|
| **Alt+Enter** | Fullscreen on/off. Works even while the mouse is captured. |
| **Windows key** | Releases a captured mouse. It only releases, and the Start menu does not open. |
| **Alt+F4** | Closes the window, but not while the mouse is captured, since the program may use Alt. |
| **F11** | Fullscreen on/off. |
| **Ctrl+F5** | Takes a screenshot (*Tools > Capture > Open Capture Folder* shows where). |

## The mouse

A program that uses the mouse captures it when you click in the window, or when the
window regains focus. Press the **Windows key** to get the pointer back. A message at
the top of the picture reminds you; *Settings > Display > Show on-screen messages*
turns such messages off.

## Copy and paste

*Edit > Mark / Select Region* lets you drag over text on a text-mode screen. **Enter**
copies it and **Esc** cancels, the same keys as the Windows console. *Edit > Copy Whole
Screen* copies everything and *Edit > Paste* types the clipboard into the program.

## Several programs at once

Each DOS program runs in its own NTVDMEX window, and **only the window you're using
runs**. When a window loses focus, its program pauses completely (picture, sound,
timers) and carries on from the same moment when you click back into it. Windows 3.x
programs are not paused this way.

## When something goes wrong

- `ntvdmhost.exe /status` first: it says what is actually handling DOS programs.
- If NTVDMEX fails to *start* (it never gets as far as a window) twice in a row, the
  next start is in a safe mode; after three, it uninstalls itself and hands DOS back to
  stock NTVDM rather than leave the machine unable to run DOS programs. Run `/install`
  again once the cause is fixed. A program that crashes or is closed by its X button
  does not count.
- Report problems at the [issue tracker](https://github.com/MrMatthewLayton/ntvdmex/issues).

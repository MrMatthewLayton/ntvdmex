/* host_strings.h -- the host's strings that are not log text (#333). Defines only.
 *
 * Names Windows is given (modules, exports, devices, registry paths, a mutex), files and
 * environment names the host reads or writes, and the text the user reads: menus, message
 * boxes, the Settings dialog. One name per string, defined once (docs/STYLE.md section 3).
 *
 * Not here: log and trace text, whose wording IS the log format (rig scripts parse it); a
 * long message written where it is shown (its #define is at the site); string tables kept
 * as tables (a switch from a setting to its explanation, the Win16 module list).
 */
#ifndef NTVDMEX_HOST_STRINGS_H
#define NTVDMEX_HOST_STRINGS_H

/* ── Modules and the exports bound from them by name ─────────────────────────────────── */
#define HOST_MODULE_KERNEL32        "kernel32.dll"
#define HOST_MODULE_USER32          "user32.dll"
#define HOST_MODULE_NTDLL           "ntdll.dll"
#define HOST_MODULE_NETAPI32        "netapi32.dll"
#define HOST_MODULE_WINMM           "winmm.dll"
#define HOST_MODULE_UXTHEME         "uxtheme.dll"
#define HOST_EXPORT_ADD_VECTORED_EXCEPTION_HANDLER "AddVectoredExceptionHandler"
#define HOST_EXPORT_ATTACH_CONSOLE  "AttachConsole"
#define HOST_EXPORT_REGISTER_RAW_INPUT_DEVICES "RegisterRawInputDevices"
#define HOST_EXPORT_GET_RAW_INPUT_DATA "GetRawInputData"
#define HOST_EXPORT_NETBIOS         "Netbios"
#define HOST_EXPORT_NT_QUERY_INFORMATION_PROCESS "NtQueryInformationProcess"
#define HOST_EXPORT_TIME_SET_EVENT  "timeSetEvent"
#define HOST_EXPORT_TIME_BEGIN_PERIOD "timeBeginPeriod"
#define HOST_EXPORT_JOY_GET_POS_EX  "joyGetPosEx"
#define HOST_EXPORT_ENABLE_THEME_DIALOG_TEXTURE "EnableThemeDialogTexture"
#define HOST_EXPORT_SHIM_INIT       "NtvdmexShimInit"   /* each WOW shim DLL's entry */
#define HOST_EXPORT_INTERNAL_GET_WINDOW_TEXT "InternalGetWindowText"

/* ── Devices, kernel objects, registry ───────────────────────────────────────────────── */
#define HOST_DEVICE_COM1            "\\\\.\\COM1"
#define HOST_DEVICE_CONSOLE_OUTPUT  "CONOUT$"
#define HOST_INSTANCE_MUTEX         "Global\\ntvdmex_host_single"
#define HOST_REG_CPU_KEY            "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0"
#define HOST_REG_CPU_MHZ            "~MHz"
#define HOST_REG_CPU_NAME           "ProcessorNameString"
#define HOST_REG_RECENT_PREFIX      "Recent"            /* MRU values: Recent0, Recent1, ... */
#define HOST_WINDOW_CLASS           "NtvdmexHostWindow"

/* ── Files, programs and environment names ───────────────────────────────────────────── */
#define HOST_OUT_SUBDIRECTORY       "debug\\out\\"
#define HOST_HARNESS_STUB_NAME      "dosstub.com"       /* the rig harness's launch stub */
#define HOST_COMMAND_COM            "COMMAND.COM"
#define HOST_CMD_EXE                "CMD.EXE"
#define HOST_DEFAULT_SHELL_PATH     "C:\\WINDOWS\\SYSTEM32\\COMMAND.COM"
#define HOST_DEFAULT_DRIVE_ROOT     "C:\\"
#define HOST_EXTENSION_COM          ".COM"
#define HOST_EXTENSION_BAT          ".BAT"
#define HOST_EXTENSION_PIF          ".PIF"
#define HOST_PATH_SEPARATOR         "\\"
#define HOST_EXPLORER_COMMAND       "explorer.exe \""
#define HOST_ENV_PATH               "PATH"
#define HOST_ENV_PATH_ASSIGN        "PATH="
#define HOST_ENV_COMSPEC_ASSIGN     "COMSPEC="
#define HOST_PROGRAM_NONE           "(none)"            /* the status strip before a program */
#define HOST_STOCK_NTVDM_NAME       "ntvdm.exe"         /* Windows' own VDM, looked for by process name */
#define HOST_FLOPPY_A_ROOT          "A:\\"
#define HOST_FLOPPY_B_ROOT          "B:\\"
#define HOST_SHIM_SUBDIRECTORY      "bin\\wowshim\\"
#define HOST_MANUAL_SHOT_NAME       "shot_manual_00.bmp" /* the digits are overwritten in sequence */
#define HOST_SHOT_NAME              "shot00.bmp"
#define HOST_SHOT_TEXT_NAME         "shot00.txt"
#define HOST_ENV_LINE_FORMAT        "%s=%s\n"
#define HOST_ENV_SYSTEMROOT_ASSIGN  "SYSTEMROOT="
#define HOST_ENV_ULTRASND_ASSIGN    "ULTRASND="
#define HOST_ENV_ULTRASND_FORMAT    "ULTRASND=%X,%u,%u,%u,%u\n"
#define HOST_DEFAULT_KRNL386_PATH   "C:\\WINDOWS\\SYSTEM32\\KRNL386.EXE"
#define HOST_DEFAULT_WIN16_PATH     "C:\\WINDOWS\\SYSTEM32;C:\\WINDOWS"
#define HOST_DEFAULT_PROGRAM_PATH   "C:\\PROGRAM.COM"   /* the environment's program name when none was loaded */
#define HOST_SHELL_ARGUMENTS_FORMAT "/P %s"             /* COMMAND.COM: permanent, in this directory */
#define HOST_STARTUP_BATCH_FORMAT   "%sNTVDMEXS.BAT"
#define HOST_STARTUP_BATCH_BODY     "@echo off\r\n" \
                                    "if exist C:\\AUTOEXEC.BAT call C:\\AUTOEXEC.BAT\r\n"
#define HOST_AUTOEXEC_PATH          "C:\\AUTOEXEC.BAT"

/* ── Text the user reads ─────────────────────────────────────────────────────────────── */
#define HOST_PRODUCT_NAME           "NTVDMEX"           /* message box titles */
#define HOST_ABOUT_TITLE            "NTVDMEX#NTVDMEX -- New Technology Virtual DOS Manager, Extended"
#define HOST_WOW16_REFUSED_TITLE    "NTVDMEX - 16-bit Windows not supported"
#define HOST_PROGRAM_NOT_FOUND_TEXT "This program could not be found:\n\n"
#define HOST_PROGRAM_START_FAILED_TEXT "NTVDMEX could not start:\n\n"
#define HOST_WINDOWS_ERROR_TEXT     "\n\nWindows error 0x"
#define HOST_STUB_WRITE_FAILED_TEXT "NTVDMEX could not write its launch stub to:\r\n\r\n"
#define HOST_STUB_INCOMPLETE_TEXT   "NTVDMEX wrote an incomplete launch stub to:\r\n\r\n"
#define HOST_SESSION_START_FAILED_TEXT "NTVDMEX could not start a DOS session.\r\n\r\nCreateProcess on:\r\n"
#define HOST_SESSION_ERROR_TEXT     "\r\nfailed with error 0x"
#define HOST_SPEED_UNLIMITED        "Unlimited"
#define HOST_MANAGER_WIN16_NAME     "16-bit Windows"    /* the manager's entry for this host */
#define HOST_MANAGER_DOS_NAME       "MS-DOS Prompt"
#define HOST_TRAY_WIN16_TIP         "NTVDMEX - 16-bit Windows"
#define HOST_OPEN_FILTER            "Programs (*.exe;*.com;*.bat)\0*.exe;*.com;*.bat\0All files (*.*)\0*.*\0"
#define HOST_OPEN_TITLE             "Open Executable"
#define HOST_CAPTURE_HINT_TEXT      "Mouse captured -- press the Windows key to release it"
#define HOST_MARK_HINT_TEXT         "Mark: drag over the text, then Enter (or Edit > Copy). Esc cancels."

/* The status strip. */
#define STATUS_TEXT_PM32            "32-bit Protected Mode"
#define STATUS_TEXT_PM16            "16-bit Protected Mode"
#define STATUS_TEXT_REAL_MODE       "16-bit Real Mode"
#define STATUS_TEXT_RELEASE_MOUSE   "Press WIN to release mouse"
#define STATUS_TEXT_CAPTURE_MOUSE   "Click video to capture mouse"
#define STATUS_TEXT_NONE            ""

/* Menus. */
#define MENU_TEXT_FILE              "File"
#define MENU_TEXT_OPEN_EXECUTABLE   "Open Executable..."
#define MENU_TEXT_OPEN_RECENT       "Open Recent"
#define MENU_TEXT_RECENT_EMPTY      "(empty)"
#define MENU_TEXT_CLOSE_PROGRAM     "Close Program"
#define MENU_TEXT_EXIT_ACCELERATOR  "Exit\tAlt+F4"
#define MENU_TEXT_EXIT              "Exit"
#define MENU_TEXT_EDIT              "Edit"
#define MENU_TEXT_MARK              "Mark / Select Region"
#define MENU_TEXT_COPY              "Copy"
#define MENU_TEXT_COPY_SCREEN       "Copy Whole Screen"
#define MENU_TEXT_PASTE             "Paste"
#define MENU_TEXT_SELECT_ALL        "Select All"
#define MENU_TEXT_VIEW              "View"
#define MENU_TEXT_FULLSCREEN        "Fullscreen\tAlt+Enter"
#define MENU_TEXT_WINDOW_SIZE       "Window Size"
#define MENU_TEXT_RENDERER          "Renderer"
#define MENU_TEXT_SCALER            "Scaler"
#define MENU_TEXT_FILTERING         "Filtering"
#define MENU_TEXT_FRAME_SKIP        "Frame Skip"
#define MENU_TEXT_ASPECT_RATIO      "Aspect Ratio"
#define MENU_TEXT_FULL_SCREEN_FIT   "Full Screen Fit"
#define MENU_TEXT_COLOUR_FILTER     "Colour Filter"
#define MENU_TEXT_FORCE_VSYNC       "Force VSync"
#define MENU_TEXT_BLINK_CURSOR      "Blink Text Cursor"
#define MENU_TEXT_TOOLS             "Tools"
#define MENU_TEXT_LIMIT_SPEED       "Limit Speed"
#define MENU_TEXT_CAPTURE_MOUSE     "Capture Mouse\tWin releases"
#define MENU_TEXT_CAPTURE           "Capture"
#define MENU_TEXT_SCREENSHOT_ACCELERATOR "Take Screenshot\tCtrl+F5"
#define MENU_TEXT_SCREENSHOT        "Take Screenshot"
#define MENU_TEXT_RECORD_VIDEO      "Record Video (AVI)"
#define MENU_TEXT_RECORD_AUDIO      "Record Audio (WAV)"
#define MENU_TEXT_RECORD_MUSIC      "Record OPL / MIDI"
#define MENU_TEXT_START_STOP        "Start / Stop"
#define MENU_TEXT_CAPTURE_FOLDER    "Open Capture Folder"
#define MENU_TEXT_CAPTURE_SETTINGS  "Capture Settings..."
#define MENU_TEXT_INSTALL           "Install as System VDM..."
#define MENU_TEXT_UNINSTALL         "Uninstall..."
#define MENU_TEXT_INSTALL_STATUS    "Installation Status..."
#define MENU_TEXT_SETTINGS          "Settings..."
#define MENU_TEXT_HELP              "Help"
#define MENU_TEXT_ABOUT             "About"

/* The Settings dialog. */
#define SETTINGS_FORMAT_NUMBER      "%u"
#define SETTINGS_FORMAT_PERCENT     "%u%%"
#define SETTINGS_FILTER_SHELL       "COMMAND.COM\0COMMAND.COM\0DOS programs (*.com;*.exe)\0*.com;*.exe\0" \
                                    "All files (*.*)\0*.*\0"
#define SETTINGS_TITLE_SHELL        "Choose the DOS prompt's COMMAND.COM"
#define SETTINGS_FILTER_FLOPPY      "Floppy disk images (*.img;*.ima;*.flp)\0*.img;*.ima;*.flp\0" \
                                    "All files (*.*)\0*.*\0"
#define SETTINGS_TITLE_FLOPPY       "Choose a floppy disk image"
#define SETTINGS_FILTER_ISO         "ISO disk images (*.iso)\0*.iso\0All files (*.*)\0*.*\0"
#define SETTINGS_TITLE_ISO          "Choose an ISO disk image"
#define SETTINGS_FILTER_SOUNDFONT   "SoundFonts (*.sf2)\0*.sf2\0All files (*.*)\0*.*\0"
#define SETTINGS_TITLE_SOUNDFONT    "Choose a SoundFont"
#define SETTINGS_FONT_CODE_PAGE_FORMAT "This font draws code page %d, not 437: some line-drawing characters " \
                                    "will show as letters."
#define SETTINGS_SOURCE_CONV_KB     "a program too big for it"   /* why conventional memory was capped */
#define CFG_TEXT(file)              "cfg\\" file                 /* a knob's path as the Settings dialog shows it */
#define SETTINGS_SOURCE_MS          " (ms)"
#define SETTINGS_FORMAT_VERSION     "%u.%02u"
#define SETTINGS_FORMAT_THIS_PC     " - this PC, %u MHz"
#define SETTINGS_DOS_VERSION_FORMAT "The current session is reporting: MS-DOS %u.%02u"
#define SETTINGS_DOS_VERSION_FORCED " (set by cfg\\dosver.txt)"
#define SETTINGS_DOS_VERSION_WHY    "The file cfg\\dosver.txt overrides the setting above."
#define SETTINGS_FONT_DEFAULT_TEXT  "Fixedsys for text and Terminal (code page 437) for line drawing."
#define SETTINGS_FONT_PARTIAL_FORMAT "%d of 253 characters come from this font; the rest from the default."
#define SETTINGS_FONT_NO_8_PIXEL    "This font has no 8-pixel-wide size, so the default is used."
#define SETTINGS_FONT_MISSING       "This font is not installed on this computer, so the default is used."
#define SETTINGS_FONT_437_MISSING   " Warning: a code page 437 font file is missing; line drawing may be wrong."
#define SETTINGS_FONT_SAMPLE_TEXT   "Hello, DOS!  0123456789  "

/* The knob files the Settings dialog names as an override's source. */
#define KNOB_FILE_NOGUS             "nogus.flag"
#define KNOB_FILE_DDRAWFS           "ddrawfs.flag"
#define KNOB_FILE_DOSVER            "dosver.txt"
#define KNOB_FILE_PITPACE           "pitpace.txt"
#define KNOB_FILE_UITICK            "uitick.txt"
#define KNOB_FILE_MSENS             "msens.txt"
#define KNOB_FILE_CPUSPD            "cpuspd.txt"

#endif /* NTVDMEX_HOST_STRINGS_H */

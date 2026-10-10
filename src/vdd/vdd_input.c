/* NTVDMEX -- An NTVDM replacement for Microsoft Windows
 *
 * See vdd_input.h.  Keyboard ring buffer + INT 16h servicer, on
 * the VDD bus.  Pure C, no <windows.h>; non-blocking (reports empty via ZF).
 *
 *
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Matthew Layton
 */

#include "vdd_input.h"

/* Bytes and words. */
#define INPUT_LOW_BYTE                          0xFF
#define INPUT_HIGH_BYTE                         0xFF00
#define INPUT_RING_MINIMUM_SIZE                 4       /* The smallest ring that holds one key */
#define INPUT_FAILED                            (-1)
#define INPUT_NO_SCAN_CODE                      (-1)

/* Scan code set 1. */
#define INPUT_SCAN_CODE_MASK                    0x7F
#define INPUT_SCAN_PREFIX_E1                    0xE1
#define INPUT_PAUSE_CODES_AFTER_E1              2       /* E1 1D 45: the 1D and the 45 */
#define INPUT_PAUSE_SEQUENCE_LENGTH             6       /* E1 1D 45 E1 9D C5 */
#define INPUT_CTRL_BREAK_SEQUENCE_LENGTH        4       /* E0 46 E0 C6 */
#define INPUT_SCAN_FIRST_TYPED                  0x02    /* The 1 key: the first one searched for a character */
#define INPUT_SCAN_SLASH                        0x35    /* Keypad slash when E0-prefixed */
#define INPUT_SCAN_PRINT_SCREEN                 0x37    /* E0 37, the grey key */
#define INPUT_SCAN_CAPS_LOCK                    0x3A
#define INPUT_SCAN_NUM_LOCK                     0x45    /* Plain 45 with Ctrl, or the host's Pause */
#define INPUT_SCAN_SCROLL_LOCK                  0x46    /* E0 46 = Break */
#define INPUT_SCAN_SCROLL_LOCK_BREAK            0xC6
#define INPUT_SCAN_KEYPAD_FIRST                 0x47
#define INPUT_SCAN_KEYPAD_MINUS                 0x4A
#define INPUT_SCAN_KEYPAD_5                     0x4C
#define INPUT_SCAN_KEYPAD_PLUS                  0x4E
#define INPUT_SCAN_INSERT                       0x52
#define INPUT_SCAN_KEYPAD_LAST                  0x53
#define INPUT_SCAN_SYSREQ                       0x54

/* The BIOS key word: AH = scan code, AL = character. */
#define INPUT_SCANCODE_COLUMNS                  4       /* Plain, Shift, Ctrl, Alt */
#define INPUT_COLUMN_PLAIN                      0
#define INPUT_COLUMN_SHIFT                      1
#define INPUT_COLUMN_CTRL                       2
#define INPUT_COLUMN_ALT                        3
#define INPUT_CHARACTER_COLUMNS                 2       /* Plain and Shift */
#define INPUT_KEYBOARD_LAYOUTS                  4       /* US, UK, DE, FR */
#define INPUT_KEY_EXTENDED_MARKER               0xE0    /* AL=E0h grey keys, AH=E0h keypad Enter/slash */
#define INPUT_KEY_KEYPAD_ENTER                  0xE00D
#define INPUT_KEY_KEYPAD_SLASH                  0xE02F
#define INPUT_KEY_CTRL_KEYPAD_ENTER             0xE00A
#define INPUT_KEY_CTRL_KEYPAD_SLASH             0x9500
#define INPUT_KEY_ENTER_SCAN_CODE               0x1C00
#define INPUT_KEY_SLASH_SCAN_CODE               0x3500
#define INPUT_KEY_CTRL_BREAK                    0x0000
#define INPUT_KEY_CTRL_PRINT_SCREEN             0x7200
#define INPUT_KEY_LAST_COMPATIBLE_SCAN          0x84    /* Above it: an enhanced-only code */
#define INPUT_KEY_FILL_IN                       0xF0
#define INPUT_CHAR_CARRIAGE_RETURN              0x0D
#define INPUT_CHAR_LINE_FEED                    0x0A
#define INPUT_CTRL_CHARACTER_MASK               0x1F
#define INPUT_ALT_KEYPAD_RADIX                  10
#define INPUT_NOT_A_DIGIT                       (-1)
#define INPUT_BREAK_FLAG_SET                    0x80    /* 0040:0071 bit 7 */
#define INPUT_FLAGS3_KEPT_ON_RESET              0xF0
#define INPUT_FLAGS3_ENHANCED_KEYBOARD          0x10    /* 0040:0096 bit 4: 101/102-key keyboard */

/* The 8042 keyboard controller. */
#define INPUT_PORT_DATA                         0x60
#define INPUT_PORT_COMMAND                      0x64    /* status on a read */
#define INPUT_PORT_SYSTEM_CONTROL               0x92
#define INPUT_STATUS_IDLE                       0x14    /* SYS (POST done) + INH (not held) */
#define INPUT_STATUS_OUTPUT_FULL                0x01    /* OBF */
#define INPUT_STATUS_LAST_WRITE_COMMAND         0x08    /* A2 */
#define INPUT_KBC_READ_COMMAND_BYTE             0x20
#define INPUT_KBC_WRITE_COMMAND_BYTE            0x60
#define INPUT_KBC_DISABLE_AUX                   0xA7
#define INPUT_KBC_ENABLE_AUX                    0xA8
#define INPUT_KBC_SELF_TEST                     0xAA
#define INPUT_KBC_SELF_TEST_PASSED              0x55
#define INPUT_KBC_INTERFACE_TEST                0xAB
#define INPUT_KBC_INTERFACE_TEST_OK             0x00
#define INPUT_KBC_DISABLE_KEYBOARD              0xAD
#define INPUT_KBC_ENABLE_KEYBOARD               0xAE
#define INPUT_KBC_READ_OUTPUT_PORT              0xD0
#define INPUT_KBC_WRITE_OUTPUT_PORT             0xD1
#define INPUT_KBC_WRITE_KEYBOARD_BUFFER         0xD2
#define INPUT_KBC_PULSE_RESET                   0xFE
#define INPUT_COMMAND_BYTE_KEYBOARD_DISABLED    0x10
#define INPUT_COMMAND_BYTE_AUX_DISABLED         0x20
#define INPUT_COMMAND_BYTE_POST                 0x45    /* IRQ1 on, translation on, SYS set */
#define INPUT_OUTPUT_PORT_RESET                 0x01    /* Active low */
#define INPUT_OUTPUT_PORT_A20                   0x02
#define INPUT_OUTPUT_PORT_POST                  0x03    /* Reset line high, A20 enabled */
#define INPUT_PORT92_FAST_RESET                 0x01
#define INPUT_PORT92_A20                        0x02
#define INPUT_KEYBOARD_SET_TYPEMATIC            0xF3

/* INT 16h. */
#define INPUT_INT16_VECTOR                      0x16
#define INPUT_INT16_READ                        0x00
#define INPUT_INT16_STATUS                      0x01
#define INPUT_INT16_SHIFT_STATUS                0x02
#define INPUT_INT16_SET_TYPEMATIC               0x03
#define INPUT_INT16_PUSH_KEY                    0x05
#define INPUT_INT16_CAPABILITIES                0x09
#define INPUT_INT16_KEYBOARD_ID                 0x0A
#define INPUT_INT16_READ_ENHANCED               0x10
#define INPUT_INT16_STATUS_ENHANCED             0x11
#define INPUT_INT16_SHIFT_STATUS_ENHANCED       0x12
#define INPUT_INT16_GROUP_READ                  0       /* Int16Calls[] */
#define INPUT_INT16_GROUP_STATUS                1
#define INPUT_INT16_GROUP_SHIFT_STATUS          2
#define INPUT_INT16_GROUP_OTHER                 3
#define INPUT_INT16_SHIFT2_BITS                 0x73    /* AH=12h: 0018's bits that keep their place */
#define INPUT_INT16_RIGHT_KEY_BITS              0x0C    /* AH=12h: RCtrl, RAlt from 0096 */
#define INPUT_INT16_SYSREQ_HELD                 0x80
#define INPUT_INT16_PUSH_STORED                 0x00
#define INPUT_INT16_PUSH_FULL                   0x01
#define INPUT_INT16_CAPABILITY_BITS             0xB1
#define INPUT_KEYBOARD_ID_MF2                   0x41AB  /* MF2 behind a translating 8042 */

/* scancode set 1 -> the BIOS keycode (US layout): */
/* FOUR COLUMNS, BECAUSE THE BIOS HAS FOUR (Importance = 1):
 * Indexed by make code 0x00..0x58: what INT 09h stores for the key plain, with
 * Shift, with Ctrl and with Alt. A 0 entry means the BIOS stores NOTHING for that
 * combination (Ctrl+1 on a real keyboard is silent). This is the IBM table as every
 * BIOS carries it (SeaBIOS's scan_to_scanascii is the same data).
 *
 * [CAUTION]: The old code had only a plain and a shifted ASCII column and derived Ctrl by
 * masking; it never looked at Alt at all. So Alt+F arrived as AH=21h AL='f' -- a
 * letter -- where the BIOS says 2100h, and every DOS editor's menu accelerator
 * (edit.com, QBasic, Turbo Pascal's IDE, Norton) typed a letter instead of opening
 * its menu. The F-key rows matter just as much: Shift+F1 is 5400h, Ctrl+F1 5E00h,
 * Alt+F1 6800h, and a program that binds them (every editor) needs those exact codes.
 * The keypad rows hold the NAVIGATION codes in the plain column and the DIGITS in the
 * Shift column, because that is how NumLock works: it swaps the two, and Shift undoes
 * the swap. The Alt column of the keypad digits is 0 because those keys feed the BIOS's
 * Alt+numpad accumulator at 0040:0019 instead of storing anything (#274, kb_altnum).
 */
#define INPUT_SCANCODE_TABLE_LAST               0x58

/* Shift-state bits in 0040:0017, as every DOS program expects to find them. */
#define INPUT_SHIFT_RIGHT_SHIFT                 0x01
#define INPUT_SHIFT_LEFT_SHIFT                  0x02
#define INPUT_SHIFT_CTRL                        0x04
#define INPUT_SHIFT_ALT                         0x08
#define INPUT_SHIFT_SCROLL_LOCK                 0x10
#define INPUT_SHIFT_NUM_LOCK                    0x20
#define INPUT_SHIFT_CAPS_LOCK                   0x40

/* #254: THE REST OF THE BIOS's KEYBOARD STATE:
 * 0040:0018 (KB_FLAG_1): bit 0 LEFT Ctrl held, 1 LEFT Alt held, 2 SysReq held,
 * 3 PAUSE active, 4 Scroll held, 5 NumLock held, 6 Caps held, 7 Insert held.
 * 0040:0096 (KB_FLAG_3): bit 0 last code was E1h, 1 last code was E0h, 2 RIGHT Ctrl
 * held, 3 RIGHT Alt held, 4 enhanced keyboard (set at reset).
 * 0040:0017 bit 2/3 (Ctrl/Alt) are "either side", so they follow the two held bits
 * rather than the last make/break -- releasing left Ctrl while right is held used to
 * clear Ctrl. The lock keys toggle once per PRESS: a held key's typematic repeats
 * arrive as more makes, and the "held" bit is what stops them re-toggling.
 */
#define INPUT_SHIFT2_LEFT_CTRL                  0x01
#define INPUT_SHIFT2_LEFT_ALT                   0x02
#define INPUT_SHIFT2_SYSREQ                     0x04
#define INPUT_SHIFT2_PAUSE                      0x08
#define INPUT_SHIFT2_SCROLL_HELD                0x10
#define INPUT_SHIFT2_NUM_HELD                   0x20
#define INPUT_SHIFT2_CAPS_HELD                  0x40
#define INPUT_SHIFT2_INSERT_HELD                0x80
#define INPUT_FLAGS3_E1                         0x01
#define INPUT_FLAGS3_E0                         0x02
#define INPUT_FLAGS3_RIGHT_CTRL                 0x04
#define INPUT_FLAGS3_RIGHT_ALT                  0x08
#define INPUT_SHIFT_INSERT                      0x80

/* #136: KEYBOARD LAYOUTS, TAKEN FROM WINDOWS XP'S OWN TABLES:
 * The table above is the US BIOS. A layout only changes what the PLAIN and SHIFT
 * columns produce for some keys, so each layout is an overlay of (scan code, plain,
 * shift) in the DOS code page. The rows were NOT typed from memory: `kbdmap
 * <KLID>` asked XP (LoadKeyboardLayout -> ToAsciiEx -> CharToOem) what every scan
 * code produces on the test machine, and the tables are the differences from its US answer
 * (s82; the US dump matches this BIOS table except Shift+Tab and
 * keypad 5, where the BIOS deliberately differs). Dead keys (German ^ and the accent
 * key, French ^) keep the US character: composition is not modelled. Ctrl and Alt
 * columns are unchanged, except that a letter which MOVED (German Y/Z, French A/Q/
 * W/Z/M) gets the Ctrl code of the letter it now types. Index = SET_KBLAYOUT's item:
 * US | United Kingdom | German | French.
 */
typedef struct _KEYBOARD_OVERRIDE
{
    BYTE ScanCode;
    BYTE Plain;
    BYTE Shifted;
} KEYBOARD_OVERRIDE, *PKEYBOARD_OVERRIDE;

static const WORD g_InputScanCodeTable[INPUT_SCANCODE_TABLE_LAST + 1][INPUT_SCANCODE_COLUMNS] = {
    { 0x0000, 0x0000, 0x0000, 0x0000 },                 /* 00: none */
    { 0x011B, 0x011B, 0x011B, 0x0100 },                 /* 01: Esc */
    { 0x0231, 0x0221, 0x0000, 0x7800 },                 /* 02: 1 ! */
    { 0x0332, 0x0340, 0x0300, 0x7900 },                 /* 03: 2 @  (Ctrl+2=NUL) */
    { 0x0433, 0x0423, 0x0000, 0x7A00 },                 /* 04: 3 # */
    { 0x0534, 0x0524, 0x0000, 0x7B00 },                 /* 05: 4 $ */
    { 0x0635, 0x0625, 0x0000, 0x7C00 },                 /* 06: 5 % */
    { 0x0736, 0x075E, 0x071E, 0x7D00 },                 /* 07: 6 ^  (Ctrl+6=RS) */
    { 0x0837, 0x0826, 0x0000, 0x7E00 },                 /* 08: 7 & */
    { 0x0938, 0x092A, 0x0000, 0x7F00 },                 /* 09: 8 * */
    { 0x0A39, 0x0A28, 0x0000, 0x8000 },                 /* 0A: 9 ( */
    { 0x0B30, 0x0B29, 0x0000, 0x8100 },                 /* 0B: 0 ) */
    { 0x0C2D, 0x0C5F, 0x0C1F, 0x8200 },                 /* 0C: - _  (Ctrl+-=US) */
    { 0x0D3D, 0x0D2B, 0x0000, 0x8300 },                 /* 0D: = + */
    { 0x0E08, 0x0E08, 0x0E7F, 0x0E00 },                 /* 0E: Backspace */
    { 0x0F09, 0x0F00, 0x9400, 0xA500 },                 /* 0F: Tab */
    { 0x1071, 0x1051, 0x1011, 0x1000 },                 /* 10: Q */
    { 0x1177, 0x1157, 0x1117, 0x1100 },                 /* 11: W */
    { 0x1265, 0x1245, 0x1205, 0x1200 },                 /* 12: E */
    { 0x1372, 0x1352, 0x1312, 0x1300 },                 /* 13: R */
    { 0x1474, 0x1454, 0x1414, 0x1400 },                 /* 14: T */
    { 0x1579, 0x1559, 0x1519, 0x1500 },                 /* 15: Y */
    { 0x1675, 0x1655, 0x1615, 0x1600 },                 /* 16: U */
    { 0x1769, 0x1749, 0x1709, 0x1700 },                 /* 17: I */
    { 0x186F, 0x184F, 0x180F, 0x1800 },                 /* 18: O */
    { 0x1970, 0x1950, 0x1910, 0x1900 },                 /* 19: P */
    { 0x1A5B, 0x1A7B, 0x1A1B, 0x1A00 },                 /* 1A: [ { */
    { 0x1B5D, 0x1B7D, 0x1B1D, 0x1B00 },                 /* 1B: ] } */
    { 0x1C0D, 0x1C0D, 0x1C0A, 0x1C00 },                 /* 1C: Enter */
    { 0x0000, 0x0000, 0x0000, 0x0000 },                 /* 1D: Ctrl */
    { 0x1E61, 0x1E41, 0x1E01, 0x1E00 },                 /* 1E: A */
    { 0x1F73, 0x1F53, 0x1F13, 0x1F00 },                 /* 1F: S */
    { 0x2064, 0x2044, 0x2004, 0x2000 },                 /* 20: D */
    { 0x2166, 0x2146, 0x2106, 0x2100 },                 /* 21: F */
    { 0x2267, 0x2247, 0x2207, 0x2200 },                 /* 22: G */
    { 0x2368, 0x2348, 0x2308, 0x2300 },                 /* 23: H */
    { 0x246A, 0x244A, 0x240A, 0x2400 },                 /* 24: J */
    { 0x256B, 0x254B, 0x250B, 0x2500 },                 /* 25: K */
    { 0x266C, 0x264C, 0x260C, 0x2600 },                 /* 26: L */
    { 0x273B, 0x273A, 0x0000, 0x2700 },                 /* 27: ; : */
    { 0x2827, 0x2822, 0x0000, 0x2800 },                 /* 28: ' " */
    { 0x2960, 0x297E, 0x0000, 0x2900 },                 /* 29: ` ~ */
    { 0x0000, 0x0000, 0x0000, 0x0000 },                 /* 2A: LShift */
    { 0x2B5C, 0x2B7C, 0x2B1C, 0x2B00 },                 /* 2B: \ | */
    { 0x2C7A, 0x2C5A, 0x2C1A, 0x2C00 },                 /* 2C: Z */
    { 0x2D78, 0x2D58, 0x2D18, 0x2D00 },                 /* 2D: X */
    { 0x2E63, 0x2E43, 0x2E03, 0x2E00 },                 /* 2E: C */
    { 0x2F76, 0x2F56, 0x2F16, 0x2F00 },                 /* 2F: V */
    { 0x3062, 0x3042, 0x3002, 0x3000 },                 /* 30: B */
    { 0x316E, 0x314E, 0x310E, 0x3100 },                 /* 31: N */
    { 0x326D, 0x324D, 0x320D, 0x3200 },                 /* 32: M */
    { 0x332C, 0x333C, 0x0000, 0x3300 },                 /* 33: , < */
    { 0x342E, 0x343E, 0x0000, 0x3400 },                 /* 34: . > */
    { 0x352F, 0x353F, 0x0000, 0x3500 },                 /* 35: / ? */
    { 0x0000, 0x0000, 0x0000, 0x0000 },                 /* 36: RShift */
    { 0x372A, 0x372A, 0x9600, 0x3700 },                 /* 37: keypad * */
    { 0x0000, 0x0000, 0x0000, 0x0000 },                 /* 38: Alt */
    { 0x3920, 0x3920, 0x3920, 0x3920 },                 /* 39: Space */
    { 0x0000, 0x0000, 0x0000, 0x0000 },                 /* 3A: CapsLock */
    { 0x3B00, 0x5400, 0x5E00, 0x6800 },                 /* 3B: F1 */
    { 0x3C00, 0x5500, 0x5F00, 0x6900 },                 /* 3C: F2 */
    { 0x3D00, 0x5600, 0x6000, 0x6A00 },                 /* 3D: F3 */
    { 0x3E00, 0x5700, 0x6100, 0x6B00 },                 /* 3E: F4 */
    { 0x3F00, 0x5800, 0x6200, 0x6C00 },                 /* 3F: F5 */
    { 0x4000, 0x5900, 0x6300, 0x6D00 },                 /* 40: F6 */
    { 0x4100, 0x5A00, 0x6400, 0x6E00 },                 /* 41: F7 */
    { 0x4200, 0x5B00, 0x6500, 0x6F00 },                 /* 42: F8 */
    { 0x4300, 0x5C00, 0x6600, 0x7000 },                 /* 43: F9 */
    { 0x4400, 0x5D00, 0x6700, 0x7100 },                 /* 44: F10 */
    { 0x0000, 0x0000, 0x0000, 0x0000 },                 /* 45: NumLock */
    { 0x0000, 0x0000, 0x0000, 0x0000 },                 /* 46: ScrollLock */
    { 0x4700, 0x4737, 0x7700, 0x0000 },                 /* 47: keypad 7 / Home */
    { 0x4800, 0x4838, 0x8D00, 0x0000 },                 /* 48: keypad 8 / Up */
    { 0x4900, 0x4939, 0x8400, 0x0000 },                 /* 49: keypad 9 / PgUp */
    { 0x4A2D, 0x4A2D, 0x8E00, 0x4A00 },                 /* 4A: keypad - */
    { 0x4B00, 0x4B34, 0x7300, 0x0000 },                 /* 4B: keypad 4 / Left */
    { 0x4CF0, 0x4C35, 0x8F00, 0x0000 },                 /* 4C: keypad 5 */
    { 0x4D00, 0x4D36, 0x7400, 0x0000 },                 /* 4D: keypad 6 / Right */
    { 0x4E2B, 0x4E2B, 0x9000, 0x4E00 },                 /* 4E: keypad + */
    { 0x4F00, 0x4F31, 0x7500, 0x0000 },                 /* 4F: keypad 1 / End */
    { 0x5000, 0x5032, 0x9100, 0x0000 },                 /* 50: keypad 2 / Down */
    { 0x5100, 0x5133, 0x7600, 0x0000 },                 /* 51: keypad 3 / PgDn */
    { 0x5200, 0x5230, 0x9200, 0x0000 },                 /* 52: keypad 0 / Ins */
    { 0x5300, 0x532E, 0x9300, 0x0000 },                 /* 53: keypad . / Del */
    { 0x0000, 0x0000, 0x0000, 0x0000 },                 /* 54: SysRq */
    { 0x0000, 0x0000, 0x0000, 0x0000 },                 /* 55 */
    { 0x565C, 0x567C, 0x0000, 0x0000 },                 /* 56: 102-key \ | */
    { 0x8500, 0x8700, 0x8900, 0x8B00 },                 /* 57: F11 */
    { 0x8600, 0x8800, 0x8A00, 0x8C00 },                 /* 58: F12 */
};
/* United Kingdom (XP layout 00000809): 5 keys differ from US. */
static const KEYBOARD_OVERRIDE g_KeyboardUk[] = {
    { 0x03, 0x32, 0x22 },
    { 0x04, 0x33, 0x9C },
    { 0x28, 0x27, 0x40 },
    { 0x29, 0x60, 0xAA },
    { 0x2B, 0x23, 0x7E },
    { 0, 0, 0 }
};
/* German (XP layout 00000407): 19 keys differ from US. */
static const KEYBOARD_OVERRIDE g_KeyboardDe[] = {
    { 0x03, 0x32, 0x22 },
    { 0x04, 0x33, 0xF5 },
    { 0x07, 0x36, 0x26 },
    { 0x08, 0x37, 0x2F },
    { 0x09, 0x38, 0x28 },
    { 0x0A, 0x39, 0x29 },
    { 0x0B, 0x30, 0x3D },
    { 0x0C, 0xE1, 0x3F },
    { 0x15, 0x7A, 0x5A },
    { 0x1A, 0x81, 0x9A },
    { 0x1B, 0x2B, 0x2A },
    { 0x27, 0x94, 0x99 },
    { 0x28, 0x84, 0x8E },
    { 0x2B, 0x23, 0x27 },
    { 0x2C, 0x79, 0x59 },
    { 0x33, 0x2C, 0x3B },
    { 0x34, 0x2E, 0x3A },
    { 0x35, 0x2D, 0x5F },
    { 0x56, 0x3C, 0x3E },
    { 0, 0, 0 }
};
/* French (XP layout 0000040C): 25 keys differ from US. */
static const KEYBOARD_OVERRIDE g_KeyboardFr[] = {
    { 0x02, 0x26, 0x31 },
    { 0x03, 0x82, 0x32 },
    { 0x04, 0x22, 0x33 },
    { 0x05, 0x27, 0x34 },
    { 0x06, 0x28, 0x35 },
    { 0x07, 0x2D, 0x36 },
    { 0x08, 0x8A, 0x37 },
    { 0x09, 0x5F, 0x38 },
    { 0x0A, 0x87, 0x39 },
    { 0x0B, 0x85, 0x30 },
    { 0x0C, 0x29, 0xF8 },
    { 0x10, 0x61, 0x41 },
    { 0x11, 0x7A, 0x5A },
    { 0x1B, 0x24, 0x9C },
    { 0x1E, 0x71, 0x51 },
    { 0x27, 0x6D, 0x4D },
    { 0x28, 0x97, 0x25 },
    { 0x29, 0xFD, 0x00 },
    { 0x2B, 0x2A, 0xE6 },
    { 0x2C, 0x77, 0x57 },
    { 0x32, 0x2C, 0x3F },
    { 0x33, 0x3B, 0x2E },
    { 0x34, 0x3A, 0x2F },
    { 0x35, 0x21, 0xF5 },
    { 0x56, 0x3C, 0x3E },
    { 0, 0, 0 }
};

static const KEYBOARD_OVERRIDE *const g_KeyboardLayouts[INPUT_KEYBOARD_LAYOUTS] = { 0, g_KeyboardUk, g_KeyboardDe, g_KeyboardFr };

static INT InputNextIndex(INT index)
{
    return (index + 1) % INPUT_SCANCODE_QUEUE_SIZE;
}

/* the BIOS keyboard ring, in guest memory at 0040:001E: */

static WORD InputBdaReadWord(PCINPUT_STATE state, INT offset)
{
    return (WORD)(state->BiosData[offset] | (state->BiosData[offset + 1] << BYTE_SHIFT));
}

static VOID InputBdaWriteWord(PINPUT_STATE state, INT offset, WORD value)
{
    state->BiosData[offset] = (BYTE)value;
    state->BiosData[offset + 1] = (BYTE)(value >> BYTE_SHIFT);
}

/* #274: the ring's bounds, from 0040:0080/0082 (see vdd_input.h). A pair that cannot
 * describe a ring -- odd, empty, inverted, or too small to hold one key -- is POST's
 * 001E/003E instead: a BIOS that trusted it would write keys through a wild pointer.
 */
static VOID InputKeyboardBounds(PCINPUT_STATE state, WORD *bufferStart, WORD *bufferEnd)
{
    WORD startPointer = InputBdaReadWord(state, BIOS_BDA_KEYBOARD_START_POINTER);
    WORD endPointer = InputBdaReadWord(state, BIOS_BDA_KEYBOARD_END_POINTER);

    if ((startPointer & 1) || (endPointer & 1) || startPointer >= endPointer || (WORD)(endPointer - startPointer) < INPUT_RING_MINIMUM_SIZE)
    {
        startPointer = BIOS_BDA_KEYBOARD_BUFFER;
        endPointer = BIOS_BDA_KEYBOARD_BUFFER_END;
    }

    *bufferStart = startPointer;
    *bufferEnd = endPointer;
}

/* Advance a ring pointer, wrapping at the end of the buffer. */
static WORD InputBdaNext(WORD pointer, WORD bufferStart, WORD bufferEnd)
{
    pointer += BIOS_BDA_KEY_ENTRY_SIZE;
    return (pointer >= bufferEnd) ? bufferStart : pointer;
}

INT VddInputPush(PINPUT_STATE state, WORD key)
{
    WORD head;
    WORD tail;
    WORD nextTail;
    WORD bufferStart;
    WORD bufferEnd;

    if (!state->BiosData)
        return 0;                                 /* no guest memory yet: nowhere to put it */

    InputKeyboardBounds(state, &bufferStart, &bufferEnd);
    head = InputBdaReadWord(state, BIOS_BDA_KEYBOARD_HEAD);
    tail = InputBdaReadWord(state, BIOS_BDA_KEYBOARD_TAIL);
    /* A pointer pair the guest has not initialised (or has scribbled on) would send the
     * writes anywhere in the BDA, so validate before trusting them.
     */
    if (head < bufferStart || head >= bufferEnd || ((head - bufferStart) & 1) ||
        tail < bufferStart || tail >= bufferEnd || ((tail - bufferStart) & 1))
    {
        head = tail = bufferStart;
        InputBdaWriteWord(state, BIOS_BDA_KEYBOARD_HEAD, head);
    }

    nextTail = InputBdaNext(tail, bufferStart, bufferEnd);

    if (nextTail == head)
        return 0;              /* full -> discard the NEW key: the real BIOS

                                             beeps and throws it away. Dropping the OLDEST
                                             instead would split a keystroke stream.
                                             The RESULT is the answer INT 16h AH=05h owes
                                             its caller (AL=1 = full), so it is returned
                                             rather than swallowed. */
    InputBdaWriteWord(state, tail, key);
    InputBdaWriteWord(state, BIOS_BDA_KEYBOARD_TAIL, nextTail);
    return 1;
}

INT VddInputPop(PINPUT_STATE state, WORD *key)
{
    WORD head;
    WORD tail;
    WORD bufferStart;
    WORD bufferEnd;

    if (!state->BiosData)
        return 0;

    InputKeyboardBounds(state, &bufferStart, &bufferEnd);
    head = InputBdaReadWord(state, BIOS_BDA_KEYBOARD_HEAD);
    tail = InputBdaReadWord(state, BIOS_BDA_KEYBOARD_TAIL);

    if (head == tail)
        return 0;

    if (head < bufferStart || head >= bufferEnd || ((head - bufferStart) & 1))
        return 0;

    *key = InputBdaReadWord(state, head);
    InputBdaWriteWord(state, BIOS_BDA_KEYBOARD_HEAD, InputBdaNext(head, bufferStart, bufferEnd));
    return 1;
}

INT VddInputPeek(PINPUT_STATE state, WORD *key)
{
    WORD head;
    WORD tail;
    WORD bufferStart;
    WORD bufferEnd;

    if (!state->BiosData)
        return 0;

    InputKeyboardBounds(state, &bufferStart, &bufferEnd);
    head = InputBdaReadWord(state, BIOS_BDA_KEYBOARD_HEAD);
    tail = InputBdaReadWord(state, BIOS_BDA_KEYBOARD_TAIL);

    if (head == tail)
        return 0;

    if (head < bufferStart || head >= bufferEnd || ((head - bufferStart) & 1))
        return 0;

    *key = InputBdaReadWord(state, head);
    return 1;
}

/* raw AT keyboard: scancode FIFO + ports 0x60/0x64: */
/* Push a scancode and assert IRQ1 the way an 8042 does: the controller holds ONE byte in
 * its output buffer and raises the line on the empty->full transition; the next byte is not
 * presented (and no further interrupt occurs) until the guest reads port 0x60. Pacing the
 * interrupt off the guest's own reads is what keeps scancodes and interrupts in step.
 * Getting this wrong is very visible in a game: with one latched interrupt per byte and no
 * pacing the backlog outran delivery, and with a cap on that latch the surplus bytes lost
 * their interrupts altogether -- so E0-prefixed keys (every arrow) never arrived, and a
 * key's BREAK code could be stranded in the FIFO, leaving the game convinced it was still
 * held. That is exactly "arrows do nothing, and space sticks on after you let go".
 */
/* Is the byte at the head of the FIFO presented in the output buffer yet? */
static INT InputScanCodeAvailable(PCINPUT_STATE state)
{
    if (state->ScanCodeHead == state->ScanCodeTail)
        return 0;

    if (state->TimeMicroseconds && state->TimeMicroseconds() < state->ScanCodeHoldUntil)
        return 0;

    return 1;
}

/* Take the head byte out of the output buffer: it becomes the re-read value, the
 * BIOS arm is owed it, and the keyboard starts sending the next one (the hold).
 */
static BYTE InputScanCodePop(PINPUT_STATE state)
{
    BYTE scanCode = state->ScanCodeQueue[state->ScanCodeTail];

    state->ScanCodeTail = InputNextIndex(state->ScanCodeTail);
    state->LastScanCode = scanCode;
    state->IsScanCodeIrqUp = 0;

    if (state->TimeMicroseconds)
        state->ScanCodeHoldUntil = state->TimeMicroseconds() + INPUT_KEYBOARD_TRANSFER_US;

    return scanCode;
}

/* Raise IRQ1 for the head byte if it is presented and nobody has announced it. */
VOID VddInputPoll(PINPUT_STATE state)
{
    if (state->IsScanCodeIrqUp || !InputScanCodeAvailable(state))
        return;

    state->IsScanCodeIrqUp = 1;
    state->BiosScanCodesOwed = 0;       /* the buffer now holds THIS byte; the old one is gone */

    if (state->Bus)
        VddRaiseIrq(state->Bus, 1);
}

INT VddInputScanCodesQueued(PCINPUT_STATE state)
{
    INT depth = state->ScanCodeHead - state->ScanCodeTail;

    if (depth < 0)
        depth += (INT)INPUT_SCANCODE_QUEUE_SIZE;

    return depth;
}

VOID VddInputPushScanCode(PINPUT_STATE state, BYTE scanCode)
{
    INT wasEmpty = (state->ScanCodeHead == state->ScanCodeTail);
    INT nextHead = InputNextIndex(state->ScanCodeHead);

    state->ScanCodesPushed++;

    if (nextHead == state->ScanCodeTail) /* full -> drop oldest */
    {
        state->ScanCodesDropped++;
        state->ScanCodeTail = InputNextIndex(state->ScanCodeTail);
    }

    state->ScanCodeQueue[state->ScanCodeHead] = scanCode;
    state->ScanCodeHead = nextHead;
    /* HOW FULL DID IT EVER GET. sc_dropped only fires once the ring has ALREADY
     * overflowed, which makes it a pass/fail light with nothing before the failure.
     * A held arrow auto-repeats at the OS rate and costs TWO bytes a repeat (E0 +
     * code), while IRQ1 is delivered only when the exec loop gets a turn -- so the
     * interesting question is how close a guest that stops trapping for a while
     * comes to filling 32 bytes. This answers it without needing an overflow.
     * - It matters because of what overflow DOES here: dropping the oldest byte can
     *   strand an E0 prefix, and a bare 0x48 is keypad-8, not up-arrow. A held arrow
     *   would quietly stop being an arrow.
     */
    { INT depth = state->ScanCodeHead - state->ScanCodeTail;

      if (depth < 0)
          depth += (INT)INPUT_SCANCODE_QUEUE_SIZE;

      if ((UINT32)depth > state->ScanCodeHighWater)
          state->ScanCodeHighWater = (UINT32)depth; }

    /* empty -> full: announce it now if the keyboard may send yet, else the poll will
     * when the transfer delay from the last pop has passed.
     */
    if (wasEmpty)
        VddInputPoll(state);
}

/* The E0-prefixed (grey) keys: arrows, the nav cluster, keypad Enter and slash. Plain
 * and Shift are the scancode with AL=0 -- the AL=0 is how a guest tells LEFT from '4'.
 * Ctrl takes the keypad's Ctrl column above (Ctrl+Left = 7300h, the word-left of every
 * editor). Alt has its OWN codes on the enhanced BIOS (Alt+Left = 9B00h ...), listed
 * here for the scancodes that have one.
 */
static WORD InputExtendedAlt(BYTE code)
{
    switch (code)
    {
    case 0x47:
        return 0x9700;

    case 0x48:
        return 0x9800;

    case 0x49:
        return 0x9900;

    case 0x4B:
        return 0x9B00;

    case 0x4D:
        return 0x9D00;

    case 0x4F:
        return 0x9F00;

    case 0x50:
        return 0xA000;

    case 0x51:
        return 0xA100;

    case 0x52:
        return 0xA200;

    case 0x53:
        return 0xA300;

    case 0x1C:
        return 0xA600;

    case 0x35:
        return 0xA400;

    default:
        return 0;
    }
}

/* #254: A PLAIN E0 KEY, IN THE ENHANCED BIOS's OWN FORM:
 * The grey cluster is what AH=10h/11h exist to tell apart from the keypad, and the
 * enhanced BIOS marks it in the code itself: AL=E0h for the grey arrows / nav keys
 * (grey Left = 4BE0h, keypad Left = 4B00h), and AH=E0h for keypad Enter (E00Dh) and
 * keypad slash (E02Fh). This stored the 83-key forms (4B00h, 1C0Dh, 352Fh), so the
 * two clusters were indistinguishable. InputKeyCompatible() folds these back for AH=00h/01h,
 * as IBM's K1S translation does; DOS's CON reads through VddInputDosKey().
 */
static WORD InputExtendedPlain(BYTE code)
{
    if (code == INPUT_SCAN_ENTER)
        return INPUT_KEY_KEYPAD_ENTER;                                           /* keypad Enter */

    if (code == INPUT_SCAN_SLASH)
        return INPUT_KEY_KEYPAD_SLASH;                                           /* keypad / */

    return (WORD)((code << BYTE_SHIFT) | INPUT_KEY_EXTENDED_MARKER);
}

/* ...and with Ctrl: the keypad's Ctrl code with AL=E0h for the grey nav keys (Ctrl+
 * grey Left = 73E0h), keypad Enter E00Ah, keypad slash 9500h (RBIL's INT 16h table).
 */
static WORD InputExtendedCtrl(BYTE code)
{
    if (code == INPUT_SCAN_ENTER)
        return INPUT_KEY_CTRL_KEYPAD_ENTER;

    if (code == INPUT_SCAN_SLASH)
        return INPUT_KEY_CTRL_KEYPAD_SLASH;

    if (code >= INPUT_SCAN_KEYPAD_FIRST && code <= INPUT_SCAN_KEYPAD_LAST && code != INPUT_SCAN_KEYPAD_MINUS && code != INPUT_SCAN_KEYPAD_5 && code != INPUT_SCAN_KEYPAD_PLUS)
        return (WORD)((g_InputScanCodeTable[code][INPUT_COLUMN_CTRL] & INPUT_HIGH_BYTE) | INPUT_KEY_EXTENDED_MARKER);

    return 0;
}

/* The plain (shift=0) or shifted character of a key on the active layout; 0 = none. */
static BYTE InputKeyboardChar(PCINPUT_STATE state, BYTE code, INT shift)
{
    const KEYBOARD_OVERRIDE *override = (state->Layout < INPUT_KEYBOARD_LAYOUTS) ? g_KeyboardLayouts[state->Layout] : 0;

    for (; override && override->ScanCode; ++override)
        if (override->ScanCode == code)
            return shift ? override->Shifted : override->Plain;

    if (code > INPUT_SCANCODE_TABLE_LAST)
        return 0;

    return (BYTE)(g_InputScanCodeTable[code][shift ? INPUT_COLUMN_SHIFT : INPUT_COLUMN_PLAIN] & INPUT_LOW_BYTE);
}

static INT InputIsLetterOn(PCINPUT_STATE state, BYTE code)
{
    BYTE character = InputKeyboardChar(state, code, INPUT_KEY_UNSHIFTED);

    return character >= 'a' && character <= 'z';
}

INT VddInputCharToKey(PCINPUT_STATE state, BYTE character, BYTE *scanCode, INT *isShift)
{
    BYTE code;
    INT isShifted;

    for (isShifted = 0; isShifted < INPUT_CHARACTER_COLUMNS; ++isShifted)
        for (code = INPUT_SCAN_FIRST_TYPED; code <= INPUT_SCANCODE_TABLE_LAST; ++code)
        {
            if (code == INPUT_SCAN_TAB && isShifted)
                continue;                                               /* Shift+Tab is back-tab, not a char */

            if (code >= INPUT_SCAN_KEYPAD_FIRST && code <= INPUT_SCAN_KEYPAD_LAST)
                continue;                                                                      /* the keypad: NumLock decides it */

            if (InputKeyboardChar(state, code, isShifted) == character)
            {
                *scanCode = code;
                *isShift = isShifted;
                return 1;
            }
        }

    return 0;
}

static BYTE InputShiftFlags(PCINPUT_STATE state)
{
    return state->BiosData ? state->BiosData[BIOS_BDA_SHIFT_FLAGS] : 0;
}

static VOID InputSetShiftFlag(PINPUT_STATE state, BYTE bit, INT isOn)
{
    if (!state->BiosData)
        return;

    if (isOn)
        state->BiosData[BIOS_BDA_SHIFT_FLAGS] = (BYTE)(state->BiosData[BIOS_BDA_SHIFT_FLAGS] | bit);
    else
        state->BiosData[BIOS_BDA_SHIFT_FLAGS] = (BYTE)(state->BiosData[BIOS_BDA_SHIFT_FLAGS] & ~bit);
}

static BYTE InputBdaByte(PCINPUT_STATE state, INT offset)
{
    return state->BiosData ? state->BiosData[offset] : 0;
}

static VOID InputSetBdaFlag(PINPUT_STATE state, INT offset, BYTE bit, INT isOn)
{
    if (!state->BiosData)
        return;

    if (isOn)
        state->BiosData[offset] = (BYTE)(state->BiosData[offset] | bit);
    else
        state->BiosData[offset] = (BYTE)(state->BiosData[offset] & ~bit);
}

/* A lock key: toggle `lock` in 0017 on the first make only; track `held` in 0018. */
static VOID InputLockKey(PINPUT_STATE state, BYTE lockBit, BYTE heldBit, INT isBreak)
{
    if (isBreak)
    {
        InputSetBdaFlag(state, BIOS_BDA_SHIFT_FLAGS2, heldBit, FALSE);
        return;
    }

    if (InputBdaByte(state, BIOS_BDA_SHIFT_FLAGS2) & heldBit)
        return;                                                                 /* typematic repeat */

    InputSetBdaFlag(state, BIOS_BDA_SHIFT_FLAGS2, heldBit, TRUE);
    InputSetShiftFlag(state, lockBit, !(InputShiftFlags(state) & lockBit));
}

/* #274: ALT + KEYPAD DIGITS = THE CHARACTER WITH THAT DECIMAL CODE:
 * While Alt is held, each keypad digit (the non-E0 keypad, whatever NumLock says)
 * does `0040:0019 = 0040:0019 * 10 + digit` -- a BYTE, so it wraps mod 256 as the
 * BIOS's does -- and stores nothing. When Alt is released, a non-zero accumulator is
 * stored as AH=00h AL=value (Alt+2+4+0 -> 00F0h, the code InputKeyCompatible already lets
 * through) and the accumulator is cleared; zero stores nothing. Any OTHER key pressed
 * with Alt held throws the accumulated value away and is translated as usual.
 * (IBM PC/AT TR, KB_INT "ALT-INPUT-TABLE" / K32 "zero anything that's been
 * accumulated"; RBIL MEMORY.LST 0040:0019.) Unmeasured: no oracle can hold Alt.
 *
 * [CAUTION]: With BOTH Alts down the value is stored when the last one comes up -- 0017 bit 3
 * is "either Alt", and that is the bit this follows. Ctrl+Alt still accumulates: the
 * AT BIOS checks Ctrl+Alt only for Del, which a VDD cannot honour anyway.
 */
static INT InputKeypadDigit(BYTE code)
{
    switch (code)
    {
    case 0x52:
        return 0;

    case 0x4F:
        return 1;

    case 0x50:
        return 2;

    case 0x51:
        return 3;

    case 0x4B:
        return 4;

    case 0x4C:
        return 5;

    case 0x4D:
        return 6;

    case 0x47:
        return 7;

    case 0x48:
        return 8;

    case 0x49:
        return 9;

    default:
        return INPUT_NOT_A_DIGIT;
    }
}

/* One scancode -> the BIOS's view of it: update the shift state, and for a make code
 * that denotes a character or a named key, store AH=scancode AL=ascii in the ring.
 * Returns a KB_ACT_* for the caller to run (#254).
 */
static INT InputBiosTranslate(PINPUT_STATE state, BYTE scanCode)
{
    INT isBreak = (scanCode & INPUT_SCAN_BREAK_BIT) != 0;
    BYTE code = (BYTE)(scanCode & INPUT_SCAN_CODE_MASK);
    INT isExtended = state->IsExtendedPending;
    BYTE shiftFlags;
    BYTE ascii = 0;
    WORD key;

    if (scanCode == INPUT_SCAN_PREFIX_E0)                                    /* prefix: the next code is extended */
    {
        state->IsExtendedPending = 1;
        InputSetBdaFlag(state, BIOS_BDA_KEYBOARD_FLAGS3, INPUT_FLAGS3_E0, TRUE);
        return INPUT_ACTION_NONE;
    }

    if (scanCode == INPUT_SCAN_PREFIX_E1)                                    /* Pause: E1 1D 45 / E1 9D C5 */
    {
        state->IsExtendedPending = 0;
        state->E1Pending = INPUT_PAUSE_CODES_AFTER_E1;
        InputSetBdaFlag(state, BIOS_BDA_KEYBOARD_FLAGS3, INPUT_FLAGS3_E1, TRUE);
        return INPUT_ACTION_NONE;
    }

    state->IsExtendedPending = 0;
    InputSetBdaFlag(state, BIOS_BDA_KEYBOARD_FLAGS3, INPUT_FLAGS3_E0 | INPUT_FLAGS3_E1, FALSE);

    if (state->E1Pending)
    {
        /* PAUSE. The make sequence is E1 1D 45, the break E1 9D C5, and neither is
         * a Ctrl or a NumLock (the E1 is there so an old BIOS reads it as
         * Ctrl+NumLock, the 83-key pause). The BIOS sets 0018 bit 3 and spins in
         * its handler, interrupts on, until the next keystroke.
         */
        if (--state->E1Pending)
            return INPUT_ACTION_NONE;                          /* the 1D / 9D */

        if (isBreak || (InputBdaByte(state, BIOS_BDA_SHIFT_FLAGS2) & INPUT_SHIFT2_PAUSE))
            return INPUT_ACTION_NONE;

        InputSetBdaFlag(state, BIOS_BDA_SHIFT_FLAGS2, INPUT_SHIFT2_PAUSE, TRUE);
        state->BiosActions[INPUT_ACTION_PAUSE]++;
        return INPUT_ACTION_PAUSE;
    }

    switch (code)                                      /* modifiers: state, never a keystroke */
    {
    case INPUT_SCAN_LEFT_SHIFT:
        if (!isExtended)
            InputSetShiftFlag(state, INPUT_SHIFT_LEFT_SHIFT, !isBreak);

    return INPUT_ACTION_NONE;

               /* E0 2A is the fake shift the controller brackets some extended keys
                * with -- not a shift. Likewise E0 36.
                */
    case INPUT_SCAN_RIGHT_SHIFT:
        if (!isExtended)
            InputSetShiftFlag(state, INPUT_SHIFT_RIGHT_SHIFT, !isBreak);

    return INPUT_ACTION_NONE;

    case INPUT_SCAN_CTRL:
        if (isExtended)
            InputSetBdaFlag(state, BIOS_BDA_KEYBOARD_FLAGS3, INPUT_FLAGS3_RIGHT_CTRL, !isBreak);
        else
            InputSetBdaFlag(state, BIOS_BDA_SHIFT_FLAGS2, INPUT_SHIFT2_LEFT_CTRL, !isBreak);

        InputSetShiftFlag(state, INPUT_SHIFT_CTRL, (InputBdaByte(state, BIOS_BDA_SHIFT_FLAGS2) & INPUT_SHIFT2_LEFT_CTRL) || (InputBdaByte(state, BIOS_BDA_KEYBOARD_FLAGS3) & INPUT_FLAGS3_RIGHT_CTRL));
        return INPUT_ACTION_NONE;

    case INPUT_SCAN_ALT:
        if (isExtended)
            InputSetBdaFlag(state, BIOS_BDA_KEYBOARD_FLAGS3, INPUT_FLAGS3_RIGHT_ALT, !isBreak);
        else
            InputSetBdaFlag(state, BIOS_BDA_SHIFT_FLAGS2, INPUT_SHIFT2_LEFT_ALT, !isBreak);

        InputSetShiftFlag(state, INPUT_SHIFT_ALT, (InputBdaByte(state, BIOS_BDA_SHIFT_FLAGS2) & INPUT_SHIFT2_LEFT_ALT) || (InputBdaByte(state, BIOS_BDA_KEYBOARD_FLAGS3) & INPUT_FLAGS3_RIGHT_ALT));

        if (isBreak && !(InputShiftFlags(state) & INPUT_SHIFT_ALT) && state->BiosData)     /* #274: Alt+keypad ends */
        {
            BYTE value = state->BiosData[BIOS_BDA_ALT_KEYPAD];
            state->BiosData[BIOS_BDA_ALT_KEYPAD] = 0;

            if (value)
                VddInputPush(state, value);                     /* AH=00h AL=the code */
        }

        return INPUT_ACTION_NONE;

    case INPUT_SCAN_CAPS_LOCK:
        InputLockKey(state, INPUT_SHIFT_CAPS_LOCK, INPUT_SHIFT2_CAPS_HELD, isBreak);
    return INPUT_ACTION_NONE;

    case INPUT_SCAN_NUM_LOCK:
        /* [CAUTION]: Plain 45 is NumLock on the keyboard; our host sends NumLock as E0 45 (the
         * Win32 extended bit), so both forms are NumLock here. Ctrl + a plain 45 is
         * the 83-key PAUSE.
         */
        if (!isExtended && !isBreak && (InputShiftFlags(state) & INPUT_SHIFT_CTRL))
        {
            if (InputBdaByte(state, BIOS_BDA_SHIFT_FLAGS2) & INPUT_SHIFT2_PAUSE)
                return INPUT_ACTION_NONE;

            InputSetBdaFlag(state, BIOS_BDA_SHIFT_FLAGS2, INPUT_SHIFT2_PAUSE, TRUE);
            state->BiosActions[INPUT_ACTION_PAUSE]++;
            return INPUT_ACTION_PAUSE;
        }

        InputLockKey(state, INPUT_SHIFT_NUM_LOCK, INPUT_SHIFT2_NUM_HELD, isBreak);
        return INPUT_ACTION_NONE;

    case INPUT_SCAN_SCROLL_LOCK:
        /* CTRL-BREAK. The Break key sends E0 46 with Ctrl held (and Ctrl+Scroll Lock
         * is Break on the 83-key board). The BIOS empties the ring, sets 0040:0071
         * bit 7, calls INT 1Bh, and stores 0000h. Not a Scroll Lock toggle.
         */
        if (!isBreak && (InputShiftFlags(state) & INPUT_SHIFT_CTRL))
        {
            if (state->BiosData)
            {
                state->BiosData[BIOS_BDA_KEYBOARD_HEAD] = state->BiosData[BIOS_BDA_KEYBOARD_TAIL];
                state->BiosData[BIOS_BDA_KEYBOARD_HEAD + 1] = state->BiosData[BIOS_BDA_KEYBOARD_TAIL + 1];
                state->BiosData[BIOS_BDA_BREAK_FLAG] = (BYTE)(state->BiosData[BIOS_BDA_BREAK_FLAG] | INPUT_BREAK_FLAG_SET);
            }

            VddInputPush(state, INPUT_KEY_CTRL_BREAK);
            state->BiosActions[INPUT_ACTION_BREAK]++;
            return INPUT_ACTION_BREAK;
        }

        if (isExtended)
            return INPUT_ACTION_NONE;                               /* E0 46 without Ctrl: nothing */

        InputLockKey(state, INPUT_SHIFT_SCROLL_LOCK, INPUT_SHIFT2_SCROLL_HELD, isBreak);
        return INPUT_ACTION_NONE;

    case INPUT_SCAN_SYSREQ:
        /* SYSREQ (Alt+Print Screen). Held bit 0018 bit 2; INT 15h AX=8500h on the
         * press, 8501h on the release. Stores nothing.
         */
        if (isBreak)
        {
            if (!(InputBdaByte(state, BIOS_BDA_SHIFT_FLAGS2) & INPUT_SHIFT2_SYSREQ))
                return INPUT_ACTION_NONE;

            InputSetBdaFlag(state, BIOS_BDA_SHIFT_FLAGS2, INPUT_SHIFT2_SYSREQ, FALSE);
            state->BiosActions[INPUT_ACTION_SYSREQ_UP]++;
            return INPUT_ACTION_SYSREQ_UP;
        }

        if (InputBdaByte(state, BIOS_BDA_SHIFT_FLAGS2) & INPUT_SHIFT2_SYSREQ)
            return INPUT_ACTION_NONE;                                                                     /* repeat */

        InputSetBdaFlag(state, BIOS_BDA_SHIFT_FLAGS2, INPUT_SHIFT2_SYSREQ, TRUE);
        state->BiosActions[INPUT_ACTION_SYSREQ_DOWN]++;
        return INPUT_ACTION_SYSREQ_DOWN;

    case INPUT_SCAN_INSERT:
        /* INSERT. 0018 bit 7 while held; 0017 bit 7 toggles on the press when the
         * key is acting as Insert (grey, or keypad with NumLock and Shift agreeing)
         * and Alt/Ctrl are up. The keystroke is stored as well.
         */
        if (isBreak)
        {
            InputSetBdaFlag(state, BIOS_BDA_SHIFT_FLAGS2, INPUT_SHIFT2_INSERT_HELD, FALSE);
            return INPUT_ACTION_NONE;
        }

        shiftFlags = InputShiftFlags(state);

        if (!(InputBdaByte(state, BIOS_BDA_SHIFT_FLAGS2) & INPUT_SHIFT2_INSERT_HELD) && !(shiftFlags & (INPUT_SHIFT_ALT | INPUT_SHIFT_CTRL))
            && (isExtended || !(shiftFlags & INPUT_SHIFT_NUM_LOCK) == !(shiftFlags & (INPUT_SHIFT_LEFT_SHIFT | INPUT_SHIFT_RIGHT_SHIFT))))
            InputSetShiftFlag(state, INPUT_SHIFT_INSERT, !(shiftFlags & INPUT_SHIFT_INSERT));

        InputSetBdaFlag(state, BIOS_BDA_SHIFT_FLAGS2, INPUT_SHIFT2_INSERT_HELD, TRUE);
        break;

    default:
        break;
    }

    if (isBreak)
        return INPUT_ACTION_NONE;                           /* releases change no buffer content */

    /* WHILE PAUSED, the next keystroke ends the pause and is thrown away. */
    if (InputBdaByte(state, BIOS_BDA_SHIFT_FLAGS2) & INPUT_SHIFT2_PAUSE)
    {
        InputSetBdaFlag(state, BIOS_BDA_SHIFT_FLAGS2, INPUT_SHIFT2_PAUSE, FALSE);
        return INPUT_ACTION_NONE;
    }

    /* #274: ALT + A KEYPAD DIGIT ACCUMULATES; ANY OTHER KEY UNDER ALT CLEARS IT. */
    if ((InputShiftFlags(state) & INPUT_SHIFT_ALT) && state->BiosData)
    {
        INT keypadDigit = isExtended ? INPUT_NOT_A_DIGIT : InputKeypadDigit(code);

        if (keypadDigit >= 0)
        {
            state->BiosData[BIOS_BDA_ALT_KEYPAD] = (BYTE)(state->BiosData[BIOS_BDA_ALT_KEYPAD] * INPUT_ALT_KEYPAD_RADIX + keypadDigit);
            return INPUT_ACTION_NONE;
        }

        state->BiosData[BIOS_BDA_ALT_KEYPAD] = 0;
    }

    /* PRINT SCREEN: E0 37 (the grey key). Ctrl+PrtSc is the 7200h keystroke; on
     * its own it calls INT 05h and stores nothing (it used to store 3700h).
     */
    if (isExtended && code == INPUT_SCAN_PRINT_SCREEN)
    {
        if (InputShiftFlags(state) & INPUT_SHIFT_CTRL)
        {
            VddInputPush(state, INPUT_KEY_CTRL_PRINT_SCREEN);
            return INPUT_ACTION_NONE;
        }

        if (InputShiftFlags(state) & INPUT_SHIFT_ALT)
            return INPUT_ACTION_NONE;

        state->BiosActions[INPUT_ACTION_PRINT_SCREEN]++;
        return INPUT_ACTION_PRINT_SCREEN;
    }

    if (code > INPUT_SCANCODE_TABLE_LAST || code == 0)
        return INPUT_ACTION_NONE;

    shiftFlags = InputShiftFlags(state);
    /* THE COLUMN IS DECIDED BY PRECEDENCE: Alt beats Ctrl beats Shift:
     * That is the BIOS's order (Ctrl+Alt+Del is the Alt column of Del), and it is
     * what makes Alt+Shift+F still 2100h.
     */
    if (isExtended)
    {
        if      (shiftFlags & INPUT_SHIFT_ALT)
            key = InputExtendedAlt(code);
        else if (shiftFlags & INPUT_SHIFT_CTRL)
            key = InputExtendedCtrl(code);
        else
            key = InputExtendedPlain(code);                      /* Shift changes nothing */
    }
    else if (shiftFlags & INPUT_SHIFT_ALT)
    {
        key = g_InputScanCodeTable[code][INPUT_COLUMN_ALT];
    }
    else if (shiftFlags & INPUT_SHIFT_CTRL)
    {
        key = g_InputScanCodeTable[code][INPUT_COLUMN_CTRL];

        if (state->Layout && InputIsLetterOn(state, code))           /* #136: a moved letter */
            key = (WORD)((code << BYTE_SHIFT) | (InputKeyboardChar(state, code, INPUT_KEY_UNSHIFTED) & INPUT_CTRL_CHARACTER_MASK));
    }
    else
    {
        INT shifted = (shiftFlags & (INPUT_SHIFT_LEFT_SHIFT | INPUT_SHIFT_RIGHT_SHIFT)) != 0;
        /* CapsLock inverts Shift for LETTERS only; NumLock inverts it for the KEYPAD
         * only. Neither touches anything else, so '1' stays '1' with Caps on.
         */
        if ((shiftFlags & INPUT_SHIFT_CAPS_LOCK) && InputIsLetterOn(state, code))
            shifted = !shifted;

        if ((shiftFlags & INPUT_SHIFT_NUM_LOCK) && code >= INPUT_SCAN_KEYPAD_FIRST && code <= INPUT_SCAN_KEYPAD_LAST)
            shifted = !shifted;

        key = g_InputScanCodeTable[code][shifted ? INPUT_COLUMN_SHIFT : INPUT_COLUMN_PLAIN];

        if (state->Layout && !(code >= INPUT_SCAN_KEYPAD_FIRST && code <= INPUT_SCAN_KEYPAD_LAST) && !(code == INPUT_SCAN_TAB && shifted))
        {
            BYTE character = InputKeyboardChar(state, code, shifted);        /* #136: the layout's char */

            if ((key & INPUT_LOW_BYTE) != character)
                key = character ? (WORD)((code << BYTE_SHIFT) | character) : 0;
        }
    }

    if (key)
        VddInputPush(state, key);                       /* 0 = the BIOS stores nothing */

    (VOID)ascii;
    return INPUT_ACTION_NONE;
}

VOID VddInputPauseCancel(PINPUT_STATE state)
{
    InputSetBdaFlag(state, BIOS_BDA_SHIFT_FLAGS2, INPUT_SHIFT2_PAUSE, FALSE);
}

WORD VddInputDosKey(WORD key)
{
    BYTE scanCode = (BYTE)(key >> BYTE_SHIFT);
    BYTE character = (BYTE)key;

    if (scanCode == INPUT_KEY_EXTENDED_MARKER)
        return (WORD)(((character == INPUT_CHAR_CARRIAGE_RETURN || character == INPUT_CHAR_LINE_FEED) ? INPUT_KEY_ENTER_SCAN_CODE : INPUT_KEY_SLASH_SCAN_CODE) | character);

    if (character == INPUT_KEY_EXTENDED_MARKER && scanCode != 0)
        return (WORD)(scanCode << BYTE_SHIFT);

    return key;
}

/* The BIOS INT 09h arm. Normally the byte is still in the FIFO; but a guest that hooked
 * INT 09h ahead of us and read port 0x60 itself has already popped it (see sc_bios_owed),
 * and the real BIOS would simply read the same byte again from the 8042. So: FIFO first,
 * the owed byte second, and nothing if neither -- a spurious INT 09h with no key must
 * not re-translate a stale byte.
 */
INT VddInputBiosFetch(PINPUT_STATE state)
{
    BYTE scanCode;

    /* Chained after a hook that read the byte: the output buffer still shows that
     * byte (the keyboard has not sent the next one yet), so that is what the BIOS
     * reads -- NOT the next queued byte, which belongs to the next interrupt.
     */
    if (state->BiosScanCodesOwed)
    {
        state->BiosScanCodesOwed = 0;
        state->OwedScanCodesServed++;
        return state->LastScanCode;
    }

    if (!InputScanCodeAvailable(state))
        return INPUT_NO_SCAN_CODE;                                           /* spurious: nothing presented */

    scanCode = InputScanCodePop(state);
    VddInputPoll(state);                     /* next byte: now (no clock) or after the hold */
    return scanCode;
}

INT VddInputBiosTranslate(PINPUT_STATE state, BYTE scanCode)
{
    return InputBiosTranslate(state, scanCode);          /* <- what the stub never did: make it a KEY */
}

INT VddInputBiosConsume(PINPUT_STATE state)
{
    INT scanCode = VddInputBiosFetch(state);

    return scanCode < 0 ? INPUT_ACTION_NONE : InputBiosTranslate(state, (BYTE)scanCode);
}

INT VddInputHostKeyBytes(
    BYTE rawScanCode,
    INT isExtended,
    INT isBreak,
    BYTE bytes[INPUT_HOST_KEY_BYTES_MAX],
    INT *isNoRepeat)
{
    INT byteCount = 0;

    *isNoRepeat = 0;

    if (rawScanCode == INPUT_SCAN_NUM_LOCK && !isExtended)              /* Pause: the whole sequence on the press */
    {
        static const BYTE pauseSequence[INPUT_PAUSE_SEQUENCE_LENGTH] = { 0xE1, 0x1D, 0x45, 0xE1, 0x9D, 0xC5 };
        *isNoRepeat = 1;

        if (isBreak)
            return 0;

        for (byteCount = 0; byteCount < INPUT_PAUSE_SEQUENCE_LENGTH; ++byteCount)
            bytes[byteCount] = pauseSequence[byteCount];

        return INPUT_PAUSE_SEQUENCE_LENGTH;
    }

    if (rawScanCode == INPUT_SCAN_SCROLL_LOCK && isExtended)               /* Ctrl+Break: make AND break on the press */
    {
        *isNoRepeat = 1;

        if (isBreak)
            return 0;

        bytes[0] = INPUT_SCAN_PREFIX_E0;
        bytes[1] = INPUT_SCAN_SCROLL_LOCK;
        bytes[2] = INPUT_SCAN_PREFIX_E0;
        bytes[3] = INPUT_SCAN_SCROLL_LOCK_BREAK;
        return INPUT_CTRL_BREAK_SEQUENCE_LENGTH;
    }

    if (isExtended)
        bytes[byteCount++] = INPUT_SCAN_PREFIX_E0;

    bytes[byteCount++] = isBreak ? (BYTE)(rawScanCode | INPUT_SCAN_BREAK_BIT) : rawScanCode;
    return byteCount;
}

/* "Is a byte presented?" -- OBF, as the host's delivery gates ask it. */
INT VddInputScanCodePending(PCINPUT_STATE state)
{
    return InputScanCodeAvailable(state);
}

/* Present a controller reply at port 60h. One byte deep, which is what the part
 * is: a second command before the first is read simply overwrites it.
 */
static VOID InputControllerReply(PINPUT_STATE state, BYTE value)
{
    state->ControllerReply = value;
    state->IsControllerReplyReady = 1;
}

/* A20: ONE WIRE, AND EVERY DOOR READS THE SAME BIT:
 * The 8042's output port (bit 1), System Control Port A (port 92h bit 1) and
 * the XMS driver's AH=03h..07h are three interfaces to ONE line, and software
 * picks whichever it likes. We used to keep a flag per door and only XMS had
 * one, so a guest that opened the gate the hardware way and then asked XMS was
 * told it was shut -- and concluded the machine could not do XMS.
 *
 * [CAUTION]: THIS IS THE FLAG, NOT THE ADDRESS WRAP. Not modelling the wrap is a separate
 * decision recorded in dos_xms.h and in main.c, and it still stands; what was
 * wrong was that the three ways of ASKING disagreed with each other.
 */
VOID VddInputSetA20(PINPUT_STATE state, INT isOn)
{
    if (isOn)
        state->ControllerOutputPort |= INPUT_OUTPUT_PORT_A20;
    else
        state->ControllerOutputPort &= (BYTE)~INPUT_OUTPUT_PORT_A20;
}

INT  VddInputGetA20(PCINPUT_STATE state)
{
    return (state->ControllerOutputPort & INPUT_OUTPUT_PORT_A20) ? 1 : 0;
}

/* SYSTEM CONTROL PORT A (92h) -- THE FAST A20 GATE:
 * Bit 1 is A20, the same wire as the output port's bit 1, and bit 0 is the
 * PS/2 FAST RESET. Nothing claimed this port at all, so a write vanished and a
 * read returned the bus's absent-device 0xFF -- whose bit 1 is SET, so a guest
 * checking the gate here was told "A20 is on" by an empty bus. Right answer,
 * no mechanism, and wrong the moment anything turned it off.
 *
 * [CAUTION]: 6.22-under-QEMU and dosbox-x both decode 92h (p_kbc kbc.port92.read = 0x02);
 * PCem's AT-class config answers 0xFF, which is period-correct for a machine
 * that predates it. We target XP-era hardware, where the port exists.
 *
 * [WARNING]: BIT 0 IS A RESET AND IS COUNTED, NOT OBEYED -- same reasoning as the 8042's
 * FEh: a VDD cannot reboot the machine it is a guest on.
 */
static VOID InputSystemControlPortIn(PVOID context, WORD port, BYTE width, UINT32 *value)
{
    PINPUT_STATE state = (PINPUT_STATE)context;

    (VOID)port;
    (VOID)width;
    *value = (BYTE)(VddInputGetA20(state) ? INPUT_PORT92_A20 : 0x00);
}

static VOID InputSystemControlPortOut(PVOID context, WORD port, BYTE width, UINT32 value)
{
    PINPUT_STATE state = (PINPUT_STATE)context;

    (VOID)port;
    (VOID)width;

    if (value & INPUT_PORT92_FAST_RESET)
        state->ControllerResetsAsked++;

    VddInputSetA20(state, (value & INPUT_PORT92_A20) ? 1 : 0);
}

/* IN 0x60 = keyboard data (pop one scancode; re-reads see the last byte).
 * IN 0x64 = 8042 status: bit0 (OBF) set while a scancode waits. Writes to
 * either (LED/8042 commands) are accepted and ignored.
 */
static VOID InputKeyboardPortIn(PVOID context, WORD port, BYTE width, UINT32 *value)
{
    PINPUT_STATE state = (PINPUT_STATE)context;

    (VOID)width;

    if (port == INPUT_PORT_COMMAND)                         /* status register */
    {
        /* SEVEN OF THE EIGHT STATUS BITS USED TO BE ZERO (Importance = 3):
         * We answered OBF and nothing else. The one that matters is bit 2, SYS,
         * which POST sets on any machine DOS is running on; bit 3 (A2) says the
         * last write went to 64h rather than 60h, and bit 4 (INH) says the
         * keyboard is not inhibited.
         *
         * [INFO]: ALL THREE ORACLES ANSWER 0x1C for the idle status with OBF and AUXB
         * masked out (p_kbc kbc.status.idle) -- 6.22 under QEMU, dosbox-x AND
         * PCem. Unanimous, so no judgement was required; we answered 0x00.
         *
         * [CAUTION]: IBF (bit 1) STAYS 0 ON PURPOSE. The canonical driver loop is "wait
         * until IBF is clear, then write", so a constant 0 means every write is
         * accepted at once. We have no transfer delay to model and inventing
         * one would only make drivers wait. Recorded in inventory/kbc.md as
         * N/A-by-luck rather than left to be rediscovered.
         */
        BYTE status = INPUT_STATUS_IDLE;                /* SYS (POST done) + INH (not held) */

        if (state->IsControllerReplyReady || InputScanCodeAvailable(state))
            status |= INPUT_STATUS_OUTPUT_FULL;                                                                   /* OBF */

        if (state->IsLastWriteCommand)
            status |= INPUT_STATUS_LAST_WRITE_COMMAND;                                           /* A2 */

        *value = status;
        return;
    }

    /* port 0x60: data register.
     *
     * [WARNING]: A CONTROLLER REPLY COMES OUT BEFORE ANY SCANCODE, and it is kept in its
     *  own byte rather than in sc_last. On the real part they share one output
     *  buffer; keeping them apart here is what stops a discarded command handing
     *  the guest a KEYSTROKE where it asked for the output port -- which is
     *  exactly what we used to do, and what a driver reads bit 1 of as A20.
     */
    if (state->IsControllerReplyReady)
    {
        state->IsControllerReplyReady = 0;
        *value = state->ControllerReply;
        return;
    }

    state->Port60Reads++;

    if (InputScanCodeAvailable(state))
    {
        (VOID)InputScanCodePop(state);
        state->BiosScanCodesOwed = 1;               /* the BIOS arm may still chain and want it */
        /* Still more queued? The controller presents the next byte and re-asserts the
         * line -- after the keyboard's transfer time (poll), or at once with no clock --
         * so the guest gets exactly one interrupt per scancode, at its own pace.
         */
        VddInputPoll(state);
    }
    else if (state->ScanCodeHead != state->ScanCodeTail)
    {
        state->ScanCodeHeldReads++;                /* re-read inside the hold: same byte */
    }

    *value = state->LastScanCode;
}

/* 8042 command/data writes. Still ACCEPTED-AND-IGNORED behaviourally -- changing
 * what the keyboard does on the strength of an untested guess is how the last two
 * attempts at this went -- but no longer SILENTLY. See the fields in vdd_input.h:
 * what we want out of a run is whether the guest ever sets its own typematic rate,
 * and to what.
 */
static VOID InputKeyboardPortOut(PVOID context, WORD port, BYTE width, UINT32 value)
{
    PINPUT_STATE state = (PINPUT_STATE)context;
    BYTE byteValue = (BYTE)value;

    (VOID)width;

    if (!state)
        return;

    state->KeyboardPortWrites++;

    if (state->KeyboardPortLogCount < INPUT_PORT_LOG_ENTRIES)
    {
        state->KeyboardPortLog[state->KeyboardPortLogCount][0] = (BYTE)(port & INPUT_LOW_BYTE);
        state->KeyboardPortLog[state->KeyboardPortLogCount][1] = byteValue;
        state->KeyboardPortLogCount++;
    }

    state->IsLastWriteCommand = (BYTE)(port == INPUT_PORT_COMMAND);      /* status bit 3 (A2) */

    if (port == INPUT_PORT_COMMAND)
    {
        /* 8042 COMMANDS. They used to be counted and dropped:
         *
         * [INFO]: PCem, on a real AMI BIOS, answers all of these; 6.22-under-QEMU and
         * dosbox-x answer none of them (p_kbc kbc.selftest.55, kbc.outport.d0).
         * The period-correct machine is the one with the feature here.
         */
        state->ControllerCommand = 0;

        switch (byteValue)
        {
        case INPUT_KBC_SELF_TEST:
            InputControllerReply(state, INPUT_KBC_SELF_TEST_PASSED);
        break;      /* self test passed */

        case INPUT_KBC_INTERFACE_TEST:
            InputControllerReply(state, INPUT_KBC_INTERFACE_TEST_OK);
        break;      /* interface test: no error */

        case INPUT_KBC_READ_COMMAND_BYTE:
            InputControllerReply(state, state->ControllerCommandByte);
        break;

        case INPUT_KBC_READ_OUTPUT_PORT:
            InputControllerReply(state, state->ControllerOutputPort);
        break;

        case INPUT_KBC_WRITE_COMMAND_BYTE:
        case INPUT_KBC_WRITE_OUTPUT_PORT:                        /* a parameter byte follows */

        case INPUT_KBC_WRITE_KEYBOARD_BUFFER:                                   /* #244: write kbd output buf */
            state->ControllerCommand = byteValue;
            break;

        case INPUT_KBC_DISABLE_KEYBOARD:
            state->ControllerCommandByte |= INPUT_COMMAND_BYTE_KEYBOARD_DISABLED;
        break;   /* disable keyboard clock */

        case INPUT_KBC_ENABLE_KEYBOARD:
            state->ControllerCommandByte &= (BYTE)~INPUT_COMMAND_BYTE_KEYBOARD_DISABLED;
        break;

        case INPUT_KBC_DISABLE_AUX:
            state->ControllerCommandByte |= INPUT_COMMAND_BYTE_AUX_DISABLED;
        break;   /* disable aux clock */

        case INPUT_KBC_ENABLE_AUX:
            state->ControllerCommandByte &= (BYTE)~INPUT_COMMAND_BYTE_AUX_DISABLED;
        break;

        /* [WARNING]: FEh PULSES THE CPU RESET LINE, and we do not have one to pulse. It is
         * COUNTED rather than obeyed: a VDD cannot reboot the machine it is a
         * guest on, and pretending otherwise would be worse than the count. The
         * guest sees no reset and the run says it was asked for.
         */
        case INPUT_KBC_PULSE_RESET:
            state->ControllerResetsAsked++;
        break;

        default:
            break;
        }

        return;
    }

    /* port 0x60: either the parameter of an 8042 command, or a KEYBOARD command. */
    if (state->ControllerCommand == INPUT_KBC_WRITE_COMMAND_BYTE)
    {
        state->ControllerCommandByte = byteValue;
        state->ControllerCommand = 0;
        return;
    }

    if (state->ControllerCommand == INPUT_KBC_WRITE_KEYBOARD_BUFFER)
    {
        /* #244: D2h, WRITE KEYBOARD OUTPUT BUFFER. The byte comes out at port 60h
         * exactly as if the keyboard had sent it, IRQ1 included -- which is what
         * makes the BIOS's INT 09h path testable without a finger on a key (p_kbd3
         * injects through it). PS/2-class controllers and AMI's KBC have it; the
         * original AT 8042 did not (IBM PS/2 TR "Keyboard/Auxiliary Device
         * Controller"; RBIL PORTS.LST 64h D2h). [CAUTION] The byte bypasses set-2 -> set-1
         * translation on real parts; ours is set 1 throughout, so it goes straight
         * into the scancode FIFO. Which oracles answer it is the probe's question.
         */
        state->ControllerCommand = 0;
        VddInputPushScanCode(state, byteValue);
        return;
    }

    if (state->ControllerCommand == INPUT_KBC_WRITE_OUTPUT_PORT)
    {
        /* THE OUTPUT PORT, WHICH IS WHERE A20 LIVES (Importance = 3):
         * Bit 1 is the A20 gate and bit 0 is CPU reset, active low. A20 is
         * written straight through to the one flag the whole host shares --
         * see vdd_input_a20 -- because THE THREE DOORS ARE ONE WIRE: a guest
         * that opens the gate here and then asks XMS "is A20 on" has to be
         * told yes, or it concludes the machine cannot do XMS at all.
         */
        state->ControllerCommand = 0;

        if (!(byteValue & INPUT_OUTPUT_PORT_RESET))
            state->ControllerResetsAsked++;                                              /* bit 0 low = reset request */

        state->ControllerOutputPort = (BYTE)(byteValue | INPUT_OUTPUT_PORT_RESET);       /* we never actually reset */
        return;
    }

    if (state->IsKeyboardRateExpected)                /* the byte after 0xF3 is the rate */
    {
        state->TypematicByte = byteValue;
        state->IsTypematicSet  = 1;
        state->IsKeyboardRateExpected    = 0;
    }
    else if (byteValue == INPUT_KEYBOARD_SET_TYPEMATIC)
    {
        state->IsKeyboardRateExpected = 1;
    }
}

/* #188: WHAT AN 83-KEY PROGRAM IS ALLOWED TO SEE:
 * AH=00h/01h are the pre-101-key calls, and IBM's BIOS filters the ring for them
 * (the K1S translation in the PC/AT BIOS listing): a code only an enhanced keyboard
 * can make is DISCARDED -- consumed, head moved on -- and the gray-key E0 forms are
 * rewritten to their 83-key equivalents. AH=10h/11h see everything unaltered.
 * Measured (p_kbd 16.01.enh, F11 = 8500h): PCem's genuine AMI BIOS answers "empty"
 * with the head advanced past it; QEMU's SeaBIOS hands 8500h back. The genuine ROM
 * is the reference. Returns 0 = discard, 1 = deliver *key (possibly rewritten).
 */
static INT InputKeyCompatible(WORD *key)
{
    BYTE scanCode = (BYTE)(*key >> BYTE_SHIFT);
    BYTE character = (BYTE)*key;

    if (scanCode == INPUT_KEY_EXTENDED_MARKER)                         /* keypad Enter / keypad '/' */
    {
        *key = (WORD)(((character == INPUT_CHAR_CARRIAGE_RETURN || character == INPUT_CHAR_LINE_FEED) ? INPUT_KEY_ENTER_SCAN_CODE : INPUT_KEY_SLASH_SCAN_CODE) | character);
        return 1;
    }

    if (scanCode > INPUT_KEY_LAST_COMPATIBLE_SCAN)
        return 0;                                                           /* F11/F12, Ctrl+arrows, Alt+Enter ... */

    if (character == INPUT_KEY_FILL_IN)
        return scanCode == 0 ? 1 : 0;                                 /* fill-ins; 00F0 is Alt+keypad 240 */

    if (character == INPUT_KEY_EXTENDED_MARKER && scanCode != 0)
        *key = (WORD)(scanCode << BYTE_SHIFT);                                                            /* gray arrows etc. */

    return 1;
}

/* INT 16h -- BIOS keyboard. ZF semantics: AH=01 sets ZF=1 when no key is ready.
 * AH=00 here is non-blocking (the host loops + waits on a key event, re-issuing
 * until ZF=0); it sets ZF=1 + leaves AX when the buffer is empty.
 */
static VOID InputInt16(PVOID context, PNTVDD_REGISTERS registers)
{
    PINPUT_STATE state = (PINPUT_STATE)context;
    WORD key;

    switch (VddGetAh(registers))
    {
    case INPUT_INT16_READ:
    case INPUT_INT16_READ_ENHANCED:
        state->Int16Calls[INPUT_INT16_GROUP_READ]++;
    break;

    case INPUT_INT16_STATUS:
    case INPUT_INT16_STATUS_ENHANCED:
        state->Int16Calls[INPUT_INT16_GROUP_STATUS]++;
    break;

    case INPUT_INT16_SHIFT_STATUS:
    case INPUT_INT16_SHIFT_STATUS_ENHANCED:
        state->Int16Calls[INPUT_INT16_GROUP_SHIFT_STATUS]++;
    break;

    default:
        state->Int16Calls[INPUT_INT16_GROUP_OTHER]++;
    break;
    }

    switch (VddGetAh(registers))
    {
    case INPUT_INT16_READ:                              /* read key (host blocks on empty) */
        registers->ZeroFlag = 1;                          /* #188: enhanced-only codes discarded */

        while (VddInputPop(state, &key))
            if (InputKeyCompatible(&key))
            {
                VddSetAx(registers, key);
                registers->ZeroFlag = 0;
                break;
            }

        break;

    case INPUT_INT16_READ_ENHANCED:                              /* read key, enhanced (101-key) */
        if (VddInputPop(state, &key))
        {
            VddSetAx(registers, key);
            registers->ZeroFlag = 0;
        }
        else
            registers->ZeroFlag = 1;

        break;

    case INPUT_INT16_STATUS:                              /* check key (non-blocking) */
        registers->ZeroFlag = 1;                          /* ZF=1 => no key (QB's INKEY$ -> "") */

        while (VddInputPeek(state, &key))    /* #188: a discard CONSUMES the entry */
        {
            if (InputKeyCompatible(&key))
            {
                VddSetAx(registers, key);
                registers->ZeroFlag = 0;
                break;
            }

            (VOID)VddInputPop(state, &key);
        }

        break;

    case INPUT_INT16_STATUS_ENHANCED:                              /* check key, enhanced (101-key) */
        if (VddInputPeek(state, &key))
        {
            VddSetAx(registers, key);
            registers->ZeroFlag = 0;
        }
        else
            registers->ZeroFlag = 1;

        break;

    case INPUT_INT16_SHIFT_STATUS:                              /* shift status, from 0040:0017 */
        VddSetAl(registers, InputShiftFlags(state));
        registers->ZeroFlag = 0;
        break;

    case INPUT_INT16_SHIFT_STATUS_ENHANCED:                              /* extended shift status */
    {
        /* #254: AH IS ITS OWN LAYOUT, NOT A COPY OF 0040:0018:
         * AH: 0 LCtrl 1 LAlt 2 RCtrl 3 RAlt 4 Scroll 5 Num 6 Caps 7 SysReq (held).
         * 0018 has SysReq at bit 2, Pause at 3 and Insert at 7; the right-hand keys
         * live in 0096 bits 2/3. This copied 0018 whole, which nothing wrote.
         */
        BYTE shiftFlags2 = InputBdaByte(state, BIOS_BDA_SHIFT_FLAGS2);
        BYTE flags3 = InputBdaByte(state, BIOS_BDA_KEYBOARD_FLAGS3);
        VddSetAl(registers, InputShiftFlags(state));
        VddSetAh(registers, (BYTE)((shiftFlags2 & INPUT_INT16_SHIFT2_BITS) | (flags3 & INPUT_INT16_RIGHT_KEY_BITS) | ((shiftFlags2 & INPUT_SHIFT2_SYSREQ) ? INPUT_INT16_SYSREQ_HELD : 0)));
        registers->ZeroFlag = 0;
        break; }

    case INPUT_INT16_SET_TYPEMATIC:                              /* set typematic rate/delay (AL=05) */
        /* There is nothing to store: the repeat rate is the host OS's, and the BIOS
         * keeps no readable copy of it. What matters is that the call is ANSWERED --
         * measured on 6.22 (p_kbd.asm 16.03.typematic): AX unchanged, CF=0. A guest
         * that sets the rate and gets an error back can conclude the BIOS is not
         * there at all.
         */
        registers->CarryFlag = 0;
        registers->ZeroFlag = 0;
        break;

    case INPUT_INT16_PUSH_KEY:                              /* push a keystroke: CH=scan CL=ascii */
        /* [INFO]: THE WRITE SIDE OF THE RING, and it was missing entirely -- the `default`
         * arm below left AX exactly as the caller passed it, so a program read its
         * own byte back and called it success (p_kbd.asm caught it only once the
         * probe POISONED AL; without the poison the row was a false match).
         * This is how DOSKEY, installers that pre-answer their own prompts, and
         * every key-stuffing TSR put keys in. Oracle: AL=0 stored, AL=1 full.
         */
        VddSetAl(registers, (BYTE)(VddInputPush(state, VddGetCx(registers)) ? INPUT_INT16_PUSH_STORED : INPUT_INT16_PUSH_FULL));
        registers->CarryFlag = 0;
        registers->ZeroFlag = 0;
        break;

    case INPUT_INT16_CAPABILITIES:                              /* which INT 16h functions exist -> AL */
        /* #188: 0xB1, MEASURED on PCem's genuine AMI BIOS (DOSBox-X agrees); 0x30 was
         * QEMU's SeaBIOS. Bits: 0 = 0300h default rate, 4 = 0Ah keyboard ID (below),
         * 5 = 10h-12h enhanced, 7 = as the AMI ROM sets it.
         */
        VddSetAl(registers, INPUT_INT16_CAPABILITY_BITS);
        registers->CarryFlag = 0;
        registers->ZeroFlag = 0;
        break;

    case INPUT_INT16_KEYBOARD_ID:                              /* #188: get keyboard ID -> BX */
        /* 41ABh = an MF2 (101/102-key) keyboard behind a translating 8042, the ID
         * bit 4 of AH=09h promises.
         */
        VddSetBx(registers, INPUT_KEYBOARD_ID_MF2);
        registers->CarryFlag = 0;
        registers->ZeroFlag = 0;
        break;

    default:                                /* unknown fn: report "no key", never */
        registers->ZeroFlag = 1;                          /* a phantom keystroke (was a bug) */
        break;
    }
}

VOID VddInputReset(PVOID context)
{
    PINPUT_STATE state = (PINPUT_STATE)context;

    state->ScanCodeHead = state->ScanCodeTail = 0;
    state->LastScanCode = 0;
    state->BiosScanCodesOwed = 0;
    state->ScanCodeHoldUntil = 0;
    state->IsScanCodeIrqUp = 0;
    state->IsExtendedPending = 0;
    state->E1Pending = 0;
    /* THE CONTROLLER'S POST STATE:
     * A machine DOS is running on has been through POST, so: the keyboard
     * interrupt and translation are enabled in the command byte, and the
     * output port has the reset line HIGH (bit 0 -- it would be resetting
     * otherwise) and **A20 ALREADY OPEN**, which is what every BIOS since the
     * AT leaves behind. Coming up with A20 shut would make our own reset a
     * guest-visible event that no real machine has.
     */
    state->ControllerCommand = 0;
    state->ControllerReply = 0;
    state->IsControllerReplyReady = 0;
    /* [CAUTION]: A2 (status bit 3) STARTS SET, and it took the probe to notice. It says
     * "the last write went to 64h rather than 60h", and on a machine DOS is
     * running on that write was POST's own last command to the controller --
     * all three oracles read 0x1C at probe start, we read 0x14, and the single
     * missing bit was this one. A reset that leaves it clear is claiming the
     * last thing anyone wrote was keyboard data, which has never been true.
     */
    state->IsLastWriteCommand = 1;
    state->ControllerCommandByte = INPUT_COMMAND_BYTE_POST;                 /* IRQ1 on, translation on, SYS set */
    state->ControllerOutputPort = INPUT_OUTPUT_PORT_POST;                 /* reset line high, A20 enabled */

    if (state->BiosData)                            /* an empty ring is head==tail at its start */
    {
        InputBdaWriteWord(state, BIOS_BDA_KEYBOARD_HEAD, BIOS_BDA_KEYBOARD_BUFFER);
        InputBdaWriteWord(state, BIOS_BDA_KEYBOARD_TAIL, BIOS_BDA_KEYBOARD_BUFFER);
        state->BiosData[BIOS_BDA_SHIFT_FLAGS]  = 0;
        state->BiosData[BIOS_BDA_SHIFT_FLAGS2] = 0;
        state->BiosData[BIOS_BDA_ALT_KEYPAD] = 0;         /* #274 */
        /* #274: POST's ring bounds, which push/pop/peek now read (vdd_input.h). */
        InputBdaWriteWord(state, BIOS_BDA_KEYBOARD_START_POINTER, BIOS_BDA_KEYBOARD_BUFFER);
        InputBdaWriteWord(state, BIOS_BDA_KEYBOARD_END_POINTER,   BIOS_BDA_KEYBOARD_BUFFER_END);
        /* 0040:0096 bit 4 = "enhanced (101/102-key) keyboard present". It is what a
         * program checks before it uses INT 16h AH=10h/11h and the F11/F12 and grey
         * key codes -- edit.com and QBasic among them. We serve those functions, so
         * say so; a zero here makes them fall back to the 83-key subset.
         */
        state->BiosData[BIOS_BDA_KEYBOARD_FLAGS3] = (BYTE)((state->BiosData[BIOS_BDA_KEYBOARD_FLAGS3] & INPUT_FLAGS3_KEPT_ON_RESET) | INPUT_FLAGS3_ENHANCED_KEYBOARD);   /* #254: E0/E1/RCtrl/RAlt clear */
    }
}

INT VddInputInitialize(PVDD_BUS bus, PVOID context)
{
    PINPUT_STATE state = (PINPUT_STATE)context;

    state->Bus = bus;
    VddInputReset(state);

    if (VddClaimInterrupt(bus, INPUT_INT16_VECTOR, InputInt16, state))
        return INPUT_FAILED;

    if (VddClaimPorts(bus, INPUT_PORT_DATA, INPUT_PORT_DATA, InputKeyboardPortIn, InputKeyboardPortOut, state))
        return INPUT_FAILED;                                                                                                          /* data */

    if (VddClaimPorts(bus, INPUT_PORT_COMMAND, INPUT_PORT_COMMAND, InputKeyboardPortIn, InputKeyboardPortOut, state))
        return INPUT_FAILED;                                                                                                                /* status */

    if (VddClaimPorts(bus, INPUT_PORT_SYSTEM_CONTROL, INPUT_PORT_SYSTEM_CONTROL, InputSystemControlPortIn, InputSystemControlPortOut, state))
        return INPUT_FAILED;                                                                                                                                        /* A20 */

    return 0;
}

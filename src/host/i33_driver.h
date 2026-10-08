/* i33_driver.h -- the INT 33h driver's pure logic: the guest graphics cursor (09h),
 * the acceleration-profile / settings blocks (2Bh-2Eh, 33h) and the shift-qualified
 * alternate handlers (18h/19h). GH #264, #265.
 *
 * Header-only and dependency-free (stdint only: no windows.h, no string.h -- the host
 * is built -nostdlib), so the off-VM battery
 * (tests/unit/mouse_test.c) compiles exactly the code the host runs. The register
 * plumbing -- which register carries what, ES:DX vs a PM selector -- stays in
 * mouse_int33 (main.c); nothing here touches a guest.
 *
 * ── SPEC, AND HOW MUCH OF IT IS MEASURED ────────────────────────────────────────────
 *   RBIL INT 33h (MS Mouse 6.0+ for 18h/19h, 7.0+ for 2Bh-2Dh, 7.05+ for 33h, 8.0+
 *   for 34h, 8.10+ for 2Eh) and RBIL's tables #03168 (cursor bitmap), #03176 (the
 *   alternate call mask), #03182 (acceleration profile data), #03184 (settings).
 *   ⚠ UNMEASURED unless a row says otherwise. The 6.22 oracle runs MOUSE.COM 6.24,
 *   which predates 2Bh-34h; DOSBox-X is an emulator's opinion. tests/probes/dos/
 *   p_mouse3.asm asks every question below that a probe can ask, so the first oracle
 *   run turns these comments into measurements.
 */
#ifndef NTVDMEX_I33_DRIVER_H
#define NTVDMEX_I33_DRIVER_H

#include "../ntvdmex_types.h"

/* ══ 09h: THE GRAPHICS CURSOR ════════════════════════════════════════════════════════
     ES:DX -> 16 words of SCREEN mask then 16 words of CURSOR mask (RBIL #03168: "each
     word defines the sixteen pixels of a row, low bit rightmost" -- so bit 15 is the
     LEFTMOST pixel). The driver ANDs the screen under the pointer with the screen mask
     and then XORs the cursor mask into it; the hot spot (BX,CX, -16..16) is the bitmap
     point that sits on the pointer position.
   ► PER PIXEL, the AND keeps the pixel where the screen bit is 1 and clears it to 0
     where it is 0 -- a mask bit stands for EVERY bit of that pixel (all four planes in
     a 16-colour mode, both bits of a CGA pixel pair). The XOR bit likewise flips every
     bit the pixel has: `ones` below.
   ⚠ HOW MANY BITS A 256-COLOUR PIXEL HAS, AS FAR AS THE DRIVER IS CONCERNED, IS
     UNMEASURED. DOSBox's driver XORs 0Fh in every mode (mouse.cpp DrawCursor: `pixel ^
     0x0F`), which keeps the default arrow white (index 15) in mode 13h on the default
     palette; a driver that expanded the bit to FFh would draw it in index 255, black on
     that palette. We follow DOSBox (0Fh); p_mouse3's `i33.09.13h.row2` reads the answer
     out of the oracle's VRAM.
   ⚠ CGA 4-COLOUR (04h/05h) IS 2 BITS PER PIXEL, so a 16-bit mask word covers EIGHT
     screen pixels, each bit PAIR (high pair leftmost) one pixel's AND/XOR -- the shape a
     driver gets for free by applying the word straight to the packed video bytes, and
     what the MS Mouse Programmer's Reference describes for modes 4/5 (from memory, not
     held in the repo: UNMEASURED). Every other mode is one bit per pixel, 16 wide.
   ⚠ THE HOT SPOT IS IN SCREEN PIXELS (DOSBox: `POS_X/xratio - hotx`), i.e. NOT in the
     driver's doubled 640-wide virtual X of a 320-wide mode. In CGA 4-colour a hot spot
     counts mask BITS, so it is halved into pixels. Both UNMEASURED; p_mouse3's
     `i33.09.13h.hot.*` rows read where the oracle put the bitmap. */
#define I33_GC_ROWS 16

/* What MouseInt33 answers with, as its comments name them. */
#define I33_INSTALLED                   0xFFFF  /* 00h AX: a driver is present          */
#define I33_FAILED                      0xFFFF  /* 18h / 1Fh AX                         */
#define I33_NO_BALLPOINT                0xFFFF  /* 30h AX                               */
#define I33_HARDWARE_RESET_DONE         0xFFFF  /* 2Fh AX                               */
#define I33_NOT_INSTALLED               0x0000  /* 00h AX: no driver                    */
#define I33_SUCCESS                     0x0000  /* 2Bh-2Eh, 33h, 34h AX                 */
#define I33_NO_ALTERNATE_HANDLER        0x0000  /* 19h CX                               */
#define I33_DRIVER_ENABLED              0x0000  /* 26h BX: the driver is not disabled   */
#define I33_NOT_BUSY                    0x0000  /* 25h BX/CX/DX: lock, in-driver, busy  */
#define I33_MODE_LIST_END               0x0000  /* 29h CX (DX: no name)                 */
#define I33_RESERVED                    0x0000  /* 32h BX/CX/DX                         */
#define I33_NO_SWIFT                    0x0000  /* 53C1h AX: no SWIFT/CyberMan support  */
#define I33_BUTTON_COUNT                0x0002
#define I33_BUTTON_LEFT_BIT             1       /* the button-state bits (03h BX)        */
#define I33_BUTTON_RIGHT_BIT            2
#define I33_BUTTON_MIDDLE_BIT           4
#define I33_DRIVER_VERSION              0x0800  /* 24h BX: 8.00                         */
#define I33_MOUSE_TYPE_PS2              0x04    /* 24h CH, 2Ah DX, 33h's type           */
#define I33_PS2_IRQ                     0xFF    /* 24h CL: what a real driver says for PS/2 */
#define I33_LANGUAGE_ENGLISH            0
#define I33_VIDEO_MODE_FAILED           0xFF    /* 28h CL: nonzero = failed             */
#define I33_RATE_MAX                    4       /* 1Ch's rate codes, 0-4                */
#define I33_ACCELERATION_RESTORE_DEFAULTS 0xFFFF  /* 2Bh BX                             */
#define I33_ACCELERATION_QUERY          0xFFFF  /* 2Dh BX: only ask                     */
#define I33_ACCELERATION_ERROR          0xFFFE  /* 2Bh-2Eh AX                           */
/* 25h AX: bit 14 = the integrated driver, 13-12 = the cursor type, 11-8 = 1Ch's rate. */
#define I33_INFO_INTEGRATED_DRIVER      0x4000u
#define I33_INFO_CURSOR_TYPE_SHIFT      12
#define I33_INFO_RATE_MASK              0x0F
#define I33_CURSOR_SOFTWARE_TEXT        0
#define I33_CURSOR_HARDWARE_TEXT        1
#define I33_CURSOR_GRAPHICS             2
/* 09h's ES:DX: the screen mask, then the cursor mask, one WORD per row. */
#define I33_GC_CURSOR_MASK_OFFSET       (I33_GC_ROWS * X86_WORD_SIZE)
#define I33_GC_DEFINITION_SIZE          (2 * I33_GC_CURSOR_MASK_OFFSET)

/* INT 33h functions (AX), as MouseInt33's comments name them. */
#define I33_FN_RESET                          0x0000
#define I33_FN_SHOW_CURSOR                    0x0001
#define I33_FN_HIDE_CURSOR                    0x0002
#define I33_FN_GET_POSITION                   0x0003
#define I33_FN_SET_POSITION                   0x0004
#define I33_FN_GET_PRESS_DATA                 0x0005
#define I33_FN_GET_RELEASE_DATA               0x0006
#define I33_FN_SET_X_RANGE                    0x0007
#define I33_FN_SET_Y_RANGE                    0x0008
#define I33_FN_DEFINE_GRAPHICS_CURSOR         0x0009
#define I33_FN_DEFINE_TEXT_CURSOR             0x000A
#define I33_FN_READ_MOTION                    0x000B
#define I33_FN_SET_EVENT_HANDLER              0x000C
#define I33_FN_LIGHT_PEN_ON                   0x000D
#define I33_FN_LIGHT_PEN_OFF                  0x000E
#define I33_FN_SET_MICKEY_RATIO               0x000F
#define I33_FN_CONDITIONAL_OFF                0x0010
#define I33_FN_SET_DOUBLE_SPEED               0x0013
#define I33_FN_EXCHANGE_EVENT_HANDLER         0x0014
#define I33_FN_GET_STATE_SIZE                 0x0015
#define I33_FN_SAVE_STATE                     0x0016
#define I33_FN_RESTORE_STATE                  0x0017
#define I33_FN_SET_ALTERNATE_HANDLER          0x0018
#define I33_FN_GET_ALTERNATE_HANDLER          0x0019
#define I33_FN_SET_SENSITIVITY                0x001A
#define I33_FN_GET_SENSITIVITY                0x001B
#define I33_FN_SET_INTERRUPT_RATE             0x001C
#define I33_FN_SET_DISPLAY_PAGE               0x001D
#define I33_FN_GET_DISPLAY_PAGE               0x001E
#define I33_FN_DISABLE_DRIVER                 0x001F
#define I33_FN_ENABLE_DRIVER                  0x0020
#define I33_FN_SOFTWARE_RESET                 0x0021
#define I33_FN_SET_LANGUAGE                   0x0022
#define I33_FN_GET_LANGUAGE                   0x0023
#define I33_FN_GET_DRIVER_VERSION             0x0024
#define I33_FN_GET_DRIVER_INFO                0x0025
#define I33_FN_GET_MAXIMUM_VIRTUAL            0x0026
#define I33_FN_GET_MASKS_AND_MICKEYS          0x0027
#define I33_FN_SET_VIDEO_MODE                 0x0028
#define I33_FN_ENUMERATE_VIDEO_MODES          0x0029
#define I33_FN_GET_HOT_SPOT                   0x002A
#define I33_FN_LOAD_ACCELERATION_PROFILES     0x002B
#define I33_FN_GET_ACCELERATION_PROFILES      0x002C
#define I33_FN_SELECT_ACCELERATION_PROFILE    0x002D
#define I33_FN_SET_ACCELERATION_PROFILE_NAMES 0x002E
#define I33_FN_HARDWARE_RESET                 0x002F
#define I33_FN_BALLPOINT_INFO                 0x0030
#define I33_FN_GET_CURRENT_VIRTUAL            0x0031
#define I33_FN_GET_ACTIVE_ADVANCED            0x0032
#define I33_FN_SWITCH_SETTINGS                0x0033
#define I33_FN_GET_INI_FILE_NAME              0x0034
#define I33_FN_SWIFT_SUPPORT                  0x53C1
#define I33_GC_PIXELS         16      /* one mask word: sixteen pixels           */
#define I33_GC_LEFT_BIT       0x8000u /* bit 15 is the leftmost pixel             */
#define I33_GC_CGA_PIXELS     8       /* CGA 4-colour: eight 2-bit pixels a word  */
#define I33_GC_CGA_BITS       2
#define I33_GC_ONES_COLOUR    0x0F    /* a 16-colour cursor's set pixels: white  */
/* What 00h/21h reset to. */
#define I33_DEFAULT_MICKEYS_X      8       /* mickeys per 8 pixels                   */
#define I33_DEFAULT_MICKEYS_Y      16
#define I33_DEFAULT_DOUBLE_SPEED   64      /* mickeys/second                         */
#define I33_DEFAULT_TEXT_AND       0x77FF  /* the software text cursor's masks       */
#define I33_DEFAULT_TEXT_XOR       0x7700
#define I33_DEFAULT_SPEED          50
#define I33_DEFAULT_RATE           3
#define I33_INI_FILE_NAME          "MOUSE.INI"   /* 34h: the initialization file a driver names */
#define I33_TEXT_VIRTUAL_MAX_Y     199     /* text modes: 25 rows of 8 virtual pixels */
#define I33_TEXT_VIRTUAL_HEIGHT    200
#define I33_DEFAULT_WIDTH          640     /* no graphics mode yet: a 640x480 screen  */
#define I33_DEFAULT_HEIGHT         480
#define I33_NARROW_MODE_WIDTH      320     /* modes this narrow double X (mode 13h)   */
#define I33_TEXT_CELL_MASK         7       /* text modes snap to the 8x8 cell         */
#define I33_GC_CGA_LEFT_SHIFT 14      /* the leftmost pixel's pair: bits 15-14    */
#define I33_GC_CGA_MASK       3u
#define I33_GC_CGA_COLOURS    4

/* One row of the cursor over one row of the FRAME (palette indices, as the presenter's
   8-bpp snapshot holds them). `x0` = the frame column of the bitmap's left edge, which
   may be negative or run past `w`: clipped here, never trusted.
   map4 == NULL: 1 bit per pixel, 16 pixels; v' = (s ? v : 0) ^ (c ? ones : 0).
   map4 != NULL: CGA 4-colour, 2 bits per pixel, 8 pixels. The frame holds the RENDERED
     colour (render_cga maps the 2-bit value through its palette), so the 2-bit value is
     recovered through map4 first, masked, and mapped back -- the mask is applied to
     what the video memory holds, not to what the presenter shows. A frame value map4
     does not contain (nothing the CGA renderer writes) is taken as colour 0. */
static VOID I33GraphicsCursorRow(BYTE *row, INT width, INT left, WORD screenMask, WORD cursorMask,
                       BYTE ones, const BYTE *colourMap)
{
    INT index;
    if (!colourMap) {
        for (index = 0; index < I33_GC_PIXELS; ++index) {
            INT column = left + index;
            WORD bit = (WORD)(I33_GC_LEFT_BIT >> index);
            BYTE value;
            if (column < 0 || column >= width) continue;
            value = (screenMask & bit) ? row[column] : 0;
            if (cursorMask & bit) value = (BYTE)(value ^ ones);
            row[column] = value;
        }
        return;
    }
    for (index = 0; index < I33_GC_CGA_PIXELS; ++index) {
        INT column = left + index, colour;
        UINT shift = (UINT)(I33_GC_CGA_LEFT_SHIFT - I33_GC_CGA_BITS * index);
        UINT screenBits = (screenMask >> shift) & I33_GC_CGA_MASK, cursorBits = (cursorMask >> shift) & I33_GC_CGA_MASK, valueBits = 0;
        if (column < 0 || column >= width) continue;
        for (colour = 0; colour < I33_GC_CGA_COLOURS; ++colour) if (colourMap[colour] == row[column]) { valueBits = (UINT)colour; break; }
        valueBits = (valueBits & screenBits) ^ cursorBits;
        row[column] = colourMap[valueBits & I33_GC_CGA_MASK];
    }
}

/* The whole bitmap. (px,py) = the pointer in frame pixels, (hx,hy) = 09h's hot spot. */
static VOID I33GraphicsCursorDraw(BYTE *pixels, INT width, INT height, INT stride, INT pointerX, INT pointerY,
                        INT hotX, INT hotY, const WORD *screenMask, const WORD *cursorMask,
                        BYTE ones, const BYTE *colourMap)
{
    INT rowIndex;
    INT left = pointerX - (colourMap ? hotX / I33_GC_CGA_BITS : hotX);
    INT top = pointerY - hotY;
    if (!pixels || width <= 0 || height <= 0) return;
    for (rowIndex = 0; rowIndex < I33_GC_ROWS; ++rowIndex) {
        INT row = top + rowIndex;
        if (row < 0 || row >= height) continue;
        I33GraphicsCursorRow(pixels + (long)row * stride, width, left, screenMask[rowIndex], cursorMask[rowIndex], ones, colourMap);
    }
}

/* The MS driver's default arrow, as DOSBox carries it (mouse.cpp defaultScreenMask /
   defaultCursorMask -- transcribed from memory, not from a file in the repo). NOT what we draw after a
   reset -- the host draws its own artwork arrow until a guest defines a shape (main.c
   MS_CURSOR) -- but it is what the test proves the mask arithmetic against, and the
   shape a guest gets if it hands back what it never set. */
static const WORD g_I33DefaultScreenMask[I33_GC_ROWS] = {
    0x3FFF, 0x1FFF, 0x0FFF, 0x07FF, 0x03FF, 0x01FF, 0x00FF, 0x007F,
    0x003F, 0x001F, 0x01FF, 0x00FF, 0x30FF, 0xF87F, 0xF87F, 0xFCFF };
static const WORD g_I33DefaultCursorMask[I33_GC_ROWS] = {
    0x0000, 0x4000, 0x6000, 0x7000, 0x7800, 0x7C00, 0x7E00, 0x7F00,
    0x7F80, 0x7C00, 0x6C00, 0x4600, 0x0600, 0x0300, 0x0300, 0x0000 };

/* ══ 2Bh-2Eh, 33h: ACCELERATION PROFILES AND THE SETTINGS BLOCK ═════════════════════
     RBIL #03182, the profile data -- 324 (144h) bytes, one block, four profiles:
       00h  4 BYTEs   length of profiles 1-4 (entries used of the 32)
       04h  4x32      threshold speeds, profile 1..4 (unused = 7Fh)
       84h  4x32      speedup factors,  profile 1..4 (10h = 1.0, 14h = 1.25, 20h = 2.0;
                      unused = 10h)
      104h  4x16      profile names, blank-padded
   ⛔ ACCELERATION IS NOT APPLIED, and that is the same decision as 0Fh/13h/1Ah (see
     docs/inventory/mouse.md): the pointer is the HOST's, with Windows' own ballistics,
     and 0Bh reports device counts scaled by msens. So the block is STORED and handed
     back exactly as loaded -- 2Bh in, 2Ch/33h out -- and selecting a profile changes
     which number 2Ch/2Dh report, not how the mouse moves.
   ⚠ THE DEFAULT CURVES ARE OURS, NOT MICROSOFT'S (UNMEASURED). Four profiles of one
     entry each, threshold 7Fh, factor 10h -- i.e. "1.0 at every speed", which is the
     truth about what this driver applies. The names are MS Mouse 8.x/9.x's
     control-panel names (from memory). p_mouse3 dumps the oracle's 2Ch block so the
     defaults can be replaced by measured ones the day an 8.x driver answers. */
#define I33_ACC_LEN      0x144
#define I33_ACC_LENS     0x000
#define I33_ACC_THRESH   0x004
#define I33_ACC_FACTOR   0x084
#define I33_ACC_NAMES    0x104
#define I33_ACC_NAMELEN  16
#define I33_ACC_N        4
#define I33_ACC_DEFAULT  1            /* active profile after a reset -- UNMEASURED */
#define I33_ACC_ENTRIES  32           /* each profile's curve                       */
#define I33_ACC_UNUSED_THRESHOLD 0x7F
#define I33_ACC_FACTOR_ONE 0x10         /* 1.0                                        */

static const CHAR g_I33AccelerationDefaultNames[I33_ACC_N][I33_ACC_NAMELEN + 1] = {
    "Slow            ", "Moderate        ", "Fast            ", "Unaccelerated   " };

static VOID I33AccelerationDefaultNames(BYTE *names)
{
    INT profile, index;
    for (profile = 0; profile < I33_ACC_N; ++profile)
        for (index = 0; index < I33_ACC_NAMELEN; ++index)
            names[profile * I33_ACC_NAMELEN + index] = (BYTE)g_I33AccelerationDefaultNames[profile][index];
}

static VOID I33AccelerationDefaults(BYTE *acceleration)
{
    INT profile, index;
    for (profile = 0; profile < I33_ACC_N; ++profile) {
        acceleration[I33_ACC_LENS + profile] = 1;
        for (index = 0; index < I33_ACC_ENTRIES; ++index) {
            acceleration[I33_ACC_THRESH + profile * I33_ACC_ENTRIES + index] = I33_ACC_UNUSED_THRESHOLD;
            acceleration[I33_ACC_FACTOR + profile * I33_ACC_ENTRIES + index] = I33_ACC_FACTOR_ONE;
        }
    }
    I33AccelerationDefaultNames(acceleration + I33_ACC_NAMES);
}

/* RBIL #03184: 33h's buffer -- a 16-byte switch-settings header, then the profile data.
   The header bytes are filled from the state 1Ah/1Ch/24h already keep; the fields this
   driver has no notion of (laptop adjustment, memory type, SuperVGA, rotation, the
   button map, click lock) are 0. ⚠ The CODING of each byte (is 06h the 1Ch code or a
   rate in Hz; is 0Dh "left" 0 or 1) is UNMEASURED -- p_mouse3 dumps the oracle's. */
#define I33_SET_HDR      0x10
#define I33_SET_LEN      (I33_SET_HDR + I33_ACC_LEN)       /* 154h = 340 bytes */
#define I33_SET_TYPE             0x00
#define I33_SET_LANGUAGE         0x01
#define I33_SET_HORIZONTAL_SPEED 0x02
#define I33_SET_VERTICAL_SPEED   0x03
#define I33_SET_DOUBLE_SPEED     0x04
#define I33_SET_CURVE            0x05
#define I33_SET_RATE             0x06
typedef struct _I33_SETTINGS {
    BYTE Type;           /* 00h: as 24h's CH -- 4 = PS/2                         */
    BYTE Language;       /* 01h: as 23h -- 0 = English                           */
    BYTE HorizontalSpeed, VerticalSpeed;   /* 02h/03h: 1Ah's horizontal/vertical speed (0-100)     */
    BYTE DoubleSpeed;         /* 04h: 1Ah's double-speed threshold (0-100)            */
    BYTE Curve;          /* 05h: the active acceleration profile (2Dh)           */
    BYTE Rate;           /* 06h: 1Ch's code                                      */
} I33_SETTINGS, *PI33_SETTINGS; typedef const I33_SETTINGS *PCI33_SETTINGS;

/* Fill `out` with at most `cap` bytes of the block; returns the count written (33h's
   CX on return). A short buffer gets the first `cap` bytes, not an error -- the call
   hands the size in and the count back, so truncation is the contract's own answer. */
static UINT I33SettingsBlock(BYTE *out, UINT capacity, const I33_SETTINGS *settings,
                                   const BYTE *acceleration)
{
    BYTE block[I33_SET_LEN];
    UINT count = capacity < I33_SET_LEN ? capacity : I33_SET_LEN, index;
    for (index = 0; index < I33_SET_HDR; ++index) block[index] = 0;
    block[I33_SET_TYPE] = settings->Type;  block[I33_SET_LANGUAGE] = settings->Language;
    block[I33_SET_HORIZONTAL_SPEED] = settings->HorizontalSpeed; block[I33_SET_VERTICAL_SPEED] = settings->VerticalSpeed; block[I33_SET_DOUBLE_SPEED] = settings->DoubleSpeed;
    block[I33_SET_CURVE] = settings->Curve; block[I33_SET_RATE] = settings->Rate;
    for (index = 0; index < I33_ACC_LEN; ++index) block[I33_SET_HDR + index] = acceleration[index];
    for (index = 0; index < count; ++index) out[index] = block[index];
    return count;
}

/* ══ 18h/19h: THE ALTERNATE (SHIFT-QUALIFIED) HANDLERS ═══════════════════════════════
     RBIL #03176, the call mask: bits 0-4 the events (motion, L press/release, R
     press/release -- NO middle button, its 0Ch bits are reused), bits 5/6/7 "call if
     Shift / Ctrl / Alt is pressed during the event". "Up to three handlers can be
     defined by separate calls to this function, each with at least one of bits 5-7
     set." 18h answers AX=0018h on success, FFFFh on error; 19h finds the handler whose
     call mask matches CX and answers BX:DX = its address, CX = its mask (0 = none).
   ► THE SHIFT BITS ARE THE KEY. A handler is identified by its shift combination: a
     second 18h with the same bits 5-7 REPLACES the first (it is the same slot), a new
     combination takes a free slot, and a fourth combination is the error. 19h matches
     on bits 5-7 for the same reason.
   ⚠ UNMEASURED, and both choices are ours: (a) "replace on the same combination" -- RBIL
     only says three may be defined; (b) 19h matching on the shift bits rather than the
     whole mask.
   ► DELIVERY: the shift state is BDA 0040:0017 (bit 0 right Shift, 1 left Shift, 2 Ctrl,
     3 Alt), folded into the mask's bit positions. The handler whose combination is
     EXACTLY the keys held, and whose event bits include the event, is called with AX =
     the event bits | the shift bits ("same bit assignments as call mask"). Otherwise
     the event goes to the 0Ch handler as before.
   ⚠ UNMEASURED: exact-match (Shift+Ctrl held does not call a Shift-only handler), and
     ONE call per event (a matching alternate handler REPLACES the 0Ch call rather than
     preceding it). Both are the readings under which a program's own event queue sees
     each event once. */
#define I33_ALT_N        3
#define I33_ALT_SHIFTS   0x00E0u
#define I33_ALT_EVENTS   0x001Fu
#define I33_ALT_SHIFT    0x0020u      /* the mask's shift-state bits              */
#define I33_ALT_CTRL     0x0040u
#define I33_ALT_ALT      0x0080u
#define I33_KB_SHIFT     0x03         /* BDA 0040:0017: right/left Shift          */
#define I33_KB_CTRL      0x04
#define I33_KB_ALT       0x08
#define I33_PICK_MAIN    (-1)         /* I33PickHandler: the 0Ch handler          */
#define I33_PICK_NOBODY  (-2)
typedef struct _I33_ALTERNATE { WORD Mask; WORD Segment; UINT32 Offset; } I33_ALTERNATE, *PI33_ALTERNATE; typedef const I33_ALTERNATE *PCI33_ALTERNATE;

static UINT I33ShiftBits(BYTE keyboardFlags)
{
    return ((keyboardFlags & I33_KB_SHIFT) ? I33_ALT_SHIFT : 0u) | ((keyboardFlags & I33_KB_CTRL) ? I33_ALT_CTRL : 0u)
         | ((keyboardFlags & I33_KB_ALT) ? I33_ALT_ALT : 0u);
}

/* 18h. Returns 1 = installed (AX=0018h), 0 = refused (AX=FFFFh). */
static INT I33AlternateSet(I33_ALTERNATE *alternates, WORD mask, WORD segment, UINT32 offset)
{
    INT index, freeIndex = -1;
    UINT shifts = mask & I33_ALT_SHIFTS;
    if (!shifts) return 0;                                  /* needs one of Shift/Ctrl/Alt */
    for (index = 0; index < I33_ALT_N; ++index) {
        if (alternates[index].Mask && (alternates[index].Mask & I33_ALT_SHIFTS) == shifts) break;
        if (!alternates[index].Mask && freeIndex < 0) freeIndex = index;
    }
    if (index == I33_ALT_N) { if (freeIndex < 0) return 0; index = freeIndex; }
    alternates[index].Mask = mask; alternates[index].Segment = segment; alternates[index].Offset = offset;
    return 1;
}

/* 19h. Returns the slot, or -1 (CX=0). */
static INT I33AlternateFind(const I33_ALTERNATE *alternates, WORD mask)
{
    INT index;
    UINT shifts = mask & I33_ALT_SHIFTS;
    if (!shifts) return -1;
    for (index = 0; index < I33_ALT_N; ++index)
        if (alternates[index].Mask && (alternates[index].Mask & I33_ALT_SHIFTS) == shifts) return index;
    return -1;
}

static INT I33AlternateAny(const I33_ALTERNATE *alternates)
{
    INT index;
    for (index = 0; index < I33_ALT_N; ++index) if (alternates[index].Mask & I33_ALT_EVENTS) return 1;
    return 0;
}

/* WHO GETS THIS EVENT. ev = the event bits (0Ch layout: bit 5/6 are the MIDDLE button
   there), main_mask = 0Ch's call mask or 0 when no 0Ch handler is installed. Returns
   0..2 = that alternate slot, -1 = the 0Ch handler, -2 = nobody asked for it. *conditions = the
   condition word the chosen handler is called with. */
static INT I33PickHandler(const I33_ALTERNATE *alternates, UINT events, BYTE keyboardFlags, UINT mainMask,
                    UINT *conditions)
{
    UINT shifts = I33ShiftBits(keyboardFlags);
    INT index;
    if (shifts)
        for (index = 0; index < I33_ALT_N; ++index) {
            UINT mask = alternates[index].Mask;
            if (mask && (mask & I33_ALT_SHIFTS) == shifts && (events & mask & I33_ALT_EVENTS)) {
                *conditions = (events & mask & I33_ALT_EVENTS) | shifts;
                return index;
            }
        }
    if (events & mainMask) { *conditions = events & mainMask; return I33_PICK_MAIN; }
    *conditions = 0;
    return I33_PICK_NOBODY;
}

#endif /* NTVDMEX_I33_DRIVER_H */

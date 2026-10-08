/* vdd_video.c -- see vdd_video.h.  Text mode 3 + graphics mode 13h over the
 * shared video aperture (vmem), with the DAC palette, on the VDD bus.  Pure C. */
#include "vdd_video.h"
#include "VGA_MODEDEFs.h"
#include "vga_font.h"

/* #322: the one copy of each table -- filled at start-up (src/host/sysfont.h). */
BYTE g_VgaFont8x8[VGA_FONT_CHARACTERS][VGA_FONT8_HEIGHT];
BYTE g_VgaFont8x14[VGA_FONT_CHARACTERS][VGA_FONT14_HEIGHT];
BYTE g_VgaFont8x16[VGA_FONT_CHARACTERS][VGA_FONT16_HEIGHT];
#include "vga_defaults.h"
#include "vbe_pm.h"
#include "../dos/bios_bda_fields.h"   /* the BDA's fields */

/* ── NAMED VALUES (#333). The VGA's own register indices and bits, and this model's
     arithmetic; the mode tables below stay tables. */
/* Text. */
#define VIDEO_CELL_BYTES              2       /* character, attribute                    */
#define VIDEO_PAGE_MASK               7       /* eight display pages                     */
#define VIDEO_TEXT_WINDOW_MASK        0x7FFFu /* the 32KB colour-text window             */
#define VIDEO_TEXT_SCAN_LINES         400
#define VIDEO_TEXT_DEFAULT_ROWS       25
#define VIDEO_TEXT_PAGE_MIN           0x800u
#define VIDEO_PAGE_ALIGN_MASK         0xFFu
/* Modes. */
#define VIDEO_MODE_TEXT_80            0x03
#define VIDEO_MODE_VGA_256            0x13
/* The DAC and colour. */
#define VIDEO_DAC_WIDTH_8             8       /* 4F08: an 8-bit DAC                      */
#define VIDEO_DAC_6BIT_MASK           0x3Fu
#define VIDEO_DAC_6TO8_SHIFT          2
#define VIDEO_ARGB_OPAQUE             0xFF000000u
#define VIDEO_RED_SHIFT               16
#define VIDEO_GREEN_SHIFT             8
#define VIDEO_CHANNEL_MASK            0xFF
#define VIDEO_GREY_RED                30      /* grey summing, percent                   */
#define VIDEO_GREY_GREEN              59
#define VIDEO_GREY_BLUE               11
/* vga_defaults.h: g_VgaDefaultsByMode's rows. */
#define VIDEO_DEFAULTS_MODE_ROW       0
#define VIDEO_DEFAULTS_DAC_ROW        2
#define VIDEO_DEFAULTS_CRTC_ROW       3
/* The Attribute Controller (3C0h / 3C1h). */
#define VIDEO_PORT_AC_WRITE           0x3C0
#define VIDEO_PORT_AC_READ            0x3C1
#define VIDEO_AR_INDEX_MASK           0x1F    /* bit 5 is "video enable", not the index  */
#define VIDEO_AR_MODE                 0x10
#define VIDEO_AR_OVERSCAN             0x11
#define VIDEO_AR_PAN                  0x13
#define VIDEO_AR_COLOR_SELECT         0x14
#define VIDEO_AR_MODE_P54S            0x80    /* AR10 bit 7: P5-4 from AR14              */
#define VIDEO_AR_BLINK_SHIFT          3
#define VIDEO_AR_PALETTE_MASK         0x3F
#define VIDEO_AR_PALETTE_LOW          0x0F
#define VIDEO_AR_CSE_P54              0x03
#define VIDEO_AR_CSE_P76              0x0C
#define VIDEO_AR_CSE_SHIFT            4
#define VIDEO_EGA_PALETTE_REGISTERS   16
#define VIDEO_OVERSCAN_REGISTER       16      /* PaletteRegisters[16]: the border        */
/* CRTC register indices. */
#define VIDEO_CR_VERTICAL_TOTAL       0x06
#define VIDEO_CR_OVERFLOW             0x07
#define VIDEO_CR_MAX_SCAN             0x09
#define VIDEO_CR_VERTICAL_DISPLAY_END 0x12
#define VIDEO_CR_VERTICAL_BLANK_START 0x15
#define VIDEO_CR_LINE_COMPARE         0x18
/* Raster-split bookkeeping (VideoPaletteSplitNote). */
#define VIDEO_ROW_NONE                0xFFFFu
#define VIDEO_SPLIT_MIN_ROW           2u
#define VIDEO_SPLIT_LIVE_FRAMES       2u

/* More modes, by what they display. */
#define VIDEO_MODE_MDA                0x07
#define VIDEO_MODE_EGA_MONO_350       0x0F
#define VIDEO_MODE_EGA_350            0x10
#define VIDEO_MODE_VGA_MONO_480       0x11
#define VIDEO_MODE_VGA_480            0x12
#define VIDEO_PAGES                   8
#define VIDEO_LINES_200               200
#define VIDEO_LINES_350               350
#define VIDEO_PORT_CRTC_MONO          0x3B4u
#define VIDEO_PORT_CRTC_COLOUR        0x3D4u

/* The font vectors in the IVT, and the upper half of the 8x8 font. */
#define VIDEO_VECTOR_FONT_HIGH        0x1F
#define VIDEO_VECTOR_GRAPHICS_FONT    0x43
#define VIDEO_FONT_HIGH_FIRST         0x80

/* The BIOS data area's video fields (offsets from 0040:0000). */
#define VIDEO_EGA_INFO_BASE           0x60
#define VIDEO_EGA_INFO_NO_CLEAR       0x80
#define VIDEO_EGA_INFO_CURSOR_EMULATION_OFF 0x01
#define VIDEO_EGA_SWITCHES            0x09
#define VIDEO_VGA_FLAGS_200           0x81
#define VIDEO_VGA_FLAGS_350           0x01
#define VIDEO_VGA_FLAGS_400           0x11

/* CGA: the colour-select register (port 3D9h) and the interleaved 2bpp buffer. */
#define VIDEO_CGA_SELECT_KEEP         0xE0u
#define VIDEO_CGA_SELECT_BACKGROUND   0x1Fu
#define VIDEO_CGA_SELECT_NO_PALETTE   0xDFu
#define VIDEO_CGA_SELECT_PALETTE_SHIFT 5
#define VIDEO_CGA_INTENSITY           0x10u
#define VIDEO_CGA_RGB                 0x07u
#define VIDEO_CGA_PALETTE_BASE        0x02u
#define VIDEO_CGA_PALETTE_SECOND      2u
#define VIDEO_CGA_PALETTE_THIRD       4u
#define VIDEO_CGA_PALETTE_COLOURS     3
#define VIDEO_CGA_ODD_BANK            0x2000u
#define VIDEO_CGA_BUFFER_SIZE         0x4000u
#define VIDEO_CGA_BYTES_PER_LINE      80u
#define VIDEO_CGA_GLYPH_BYTES         2
#define VIDEO_CGA_BITS_PER_PIXEL      2
#define VIDEO_CGA_PIXEL_MASK          3
#define VIDEO_CGA_TOP_SHIFT           14
#define VIDEO_CGA_FILL_REPEAT         0x55

/* Glyphs drawn into graphics modes. */
#define VIDEO_GLYPH_WIDTH             8
#define VIDEO_GLYPH_LEFT_BIT          0x80
#define VIDEO_COLOUR_XOR              0x80
#define VIDEO_FILL_SET                0xFF

/* Teletype control characters, and the attribute DOS's CON driver writes with. */
#define VIDEO_CHAR_BELL               0x07
#define VIDEO_CHAR_BACKSPACE          0x08
#define VIDEO_CHAR_LINE_FEED          0x0A
#define VIDEO_CHAR_CARRIAGE_RETURN    0x0D
#define VIDEO_ATTRIBUTE_NORMAL        0x07

/* Bytes and words, assembled and taken apart. */
#define VIDEO_OFFSET_NONE             0xFFFFFFFFu
#define VIDEO_RGB_BYTES               3
#define VIDEO_RGB_BLUE                2

/* INT 10h functions this file calls itself. */
#define VIDEO_FUNCTION_SET_MODE       0x00
#define VIDEO_FUNCTION_VESA           0x4F
#define VIDEO_MODE_NO_CLEAR           0x80

/* VESA: mode numbers, pixel depths, direct-colour layouts and timing. */
#define VIDEO_VBE_SET_MODE            0x02
#define VIDEO_VBE_MODE_NUMBER_MASK    0x3FFF
#define VIDEO_VBE_MODE_LFB            0x4000u
#define VIDEO_VBE_MODE_NO_CLEAR       0x8000u
#define VIDEO_VBE_SEGMENT_BYTES       256
#define VIDEO_BPP_15                  15
#define VIDEO_BPP_16                  16
#define VIDEO_BPP_24                  24
#define VIDEO_BYTES_16BPP             2u
#define VIDEO_BYTES_24BPP             3u
#define VIDEO_BYTES_32BPP             4u
#define VIDEO_RGB555_RED_SHIFT        10
#define VIDEO_RGB565_RED_SHIFT        11
#define VIDEO_RGB5_GREEN_SHIFT        5
#define VIDEO_RGB_5BIT_MASK           0x1F
#define VIDEO_RGB_6BIT_MASK           0x3F
#define VIDEO_EXPAND5_SHIFT           3
#define VIDEO_EXPAND5_REFILL          2
#define VIDEO_EXPAND6_SHIFT           2
#define VIDEO_EXPAND6_REFILL          4
#define VIDEO_BGR_GREEN               1
#define VIDEO_BGR_RED                 2
#define VIDEO_DOUBLE_SCAN             2u
#define VIDEO_DOUBLE_SCAN_MAX_LINES   240u
#define VIDEO_VESA_REFRESH_HZ         60u
#define VIDEO_VESA_400_REFRESH_HZ     70u
#define VIDEO_VGA_480_LINES           480u
#define VIDEO_VGA_480_TOTAL           525u

/* The VBE/PM register ports (vbe_pm.h's code reaches the device through them). */
#define VIDEO_PORT_VBE_INDEX          0x1CE
#define VIDEO_PORT_WIDTH_WORD         2
#define VIDEO_VBE_INDEX_BPP           0x03
#define VIDEO_VBE_INDEX_BANK          0x05
#define VIDEO_VBE_INDEX_VIRTUAL_WIDTH 0x06
#define VIDEO_VBE_INDEX_START_LOW     0x10
#define VIDEO_VBE_INDEX_START_HIGH    0x11
#define VIDEO_VBE_START_UNIT          4u

/* VESA BIOS Extension functions (INT 10h AH=4Fh, AL = the function) and their answers. */
#define VIDEO_VBE_CONTROLLER_INFO     0x00
#define VIDEO_VBE_MODE_INFO           0x01
#define VIDEO_VBE_GET_MODE            0x03
#define VIDEO_VBE_STATE               0x04
#define VIDEO_VBE_SCAN_LENGTH         0x06
#define VIDEO_VBE_DISPLAY_START       0x07
#define VIDEO_VBE_FUNCTION_COUNT      0x16
#define VIDEO_VBE_OK                  0x004F
#define VIDEO_VBE_FAILED              0x014F
#define VIDEO_VBE_NOT_SUPPORTED       0x024F
#define VIDEO_VBE_INVALID_IN_MODE     0x034F
#define VIDEO_VESA_TALLY_BL_LIMIT     15
#define VIDEO_VESA_TALLY_BL_80        0x80
#define VIDEO_VESA_TALLY_BIT_80       0x8000u

/* 4F00h: the VbeInfoBlock (VBE 2.0 section 4.3). */
#define VIDEO_VBE_INFO_BYTES          256u
#define VIDEO_VBE_INFO_BYTES_V2       512u
#define VIDEO_VBE_INFO_VERSION        4
#define VIDEO_VBE_INFO_OEM_STRING     6
#define VIDEO_VBE_INFO_CAPABILITIES   10
#define VIDEO_VBE_INFO_MODE_LIST      14
#define VIDEO_VBE_INFO_TOTAL_MEMORY   18
#define VIDEO_VBE_INFO_OEM_SOFTWARE_REVISION 20
#define VIDEO_VBE_INFO_OEM_VENDOR     22
#define VIDEO_VBE_INFO_OEM_PRODUCT    26
#define VIDEO_VBE_INFO_OEM_PRODUCT_REVISION 30
#define VIDEO_VBE_INFO_OEM_DATA       0x100
#define VIDEO_VBE_VERSION_2           0x0200
#define VIDEO_VBE_CAPABILITY_DAC_SWITCHABLE 1
#define VIDEO_VBE_MEMORY_UNIT         0x10000
#define VIDEO_VBE_OEM_STRINGS         4
#define VIDEO_VBE_OEM_REVISION_1_00   0x0100
#define VIDEO_VBE_MODE_NUMBER_BYTES   2
#define VIDEO_VBE_MODE_LIST_END       0xFFFF

/* 4F01h: the ModeInfoBlock (VBE 2.0 section 4.4). */
#define VIDEO_MODE_INFO_BYTES         256
#define VIDEO_MODE_INFO_ATTRIBUTES    0
#define VIDEO_MODE_INFO_WINDOW_A_ATTRIBUTES 2
#define VIDEO_MODE_INFO_WINDOW_B_ATTRIBUTES 3
#define VIDEO_MODE_INFO_GRANULARITY   4
#define VIDEO_MODE_INFO_WINDOW_SIZE   6
#define VIDEO_MODE_INFO_WINDOW_A_SEGMENT 8
#define VIDEO_MODE_INFO_WINDOW_B_SEGMENT 10
#define VIDEO_MODE_INFO_WINDOW_FUNCTION 12
#define VIDEO_MODE_INFO_BYTES_PER_LINE 16
#define VIDEO_MODE_INFO_X_RESOLUTION  18
#define VIDEO_MODE_INFO_Y_RESOLUTION  20
#define VIDEO_MODE_INFO_X_CHARACTER   22
#define VIDEO_MODE_INFO_Y_CHARACTER   23
#define VIDEO_MODE_INFO_PLANES        24
#define VIDEO_MODE_INFO_BITS_PER_PIXEL 25
#define VIDEO_MODE_INFO_BANKS         26
#define VIDEO_MODE_INFO_MEMORY_MODEL  27
#define VIDEO_MODE_INFO_BANK_SIZE     28
#define VIDEO_MODE_INFO_IMAGE_PAGES   29
#define VIDEO_MODE_INFO_RESERVED      30
#define VIDEO_MODE_INFO_RED_SIZE      31
#define VIDEO_MODE_INFO_RED_POSITION  32
#define VIDEO_MODE_INFO_GREEN_SIZE    33
#define VIDEO_MODE_INFO_GREEN_POSITION 34
#define VIDEO_MODE_INFO_BLUE_SIZE     35
#define VIDEO_MODE_INFO_BLUE_POSITION 36
#define VIDEO_MODE_INFO_RESERVED_SIZE 37
#define VIDEO_MODE_INFO_RESERVED_POSITION 38
#define VIDEO_MODE_INFO_DIRECT_COLOUR_INFO 39
#define VIDEO_MODE_INFO_PHYSICAL_BASE 40
#define VIDEO_MODE_INFO_LINEAR_BYTES_PER_LINE 50
#define VIDEO_MODE_INFO_BANKED_IMAGE_PAGES 52
#define VIDEO_MODE_INFO_LINEAR_IMAGE_PAGES 53
#define VIDEO_MODE_INFO_LINEAR_RED_SIZE 54
#define VIDEO_MODE_INFO_LINEAR_RED_POSITION 55
#define VIDEO_MODE_INFO_LINEAR_GREEN_SIZE 56
#define VIDEO_MODE_INFO_LINEAR_GREEN_POSITION 57
#define VIDEO_MODE_INFO_LINEAR_BLUE_SIZE 58
#define VIDEO_MODE_INFO_LINEAR_BLUE_POSITION 59
#define VIDEO_MODE_INFO_LINEAR_RESERVED_SIZE 60
#define VIDEO_MODE_INFO_LINEAR_RESERVED_POSITION 61
#define VIDEO_MODE_ATTRIBUTES_TEXT    0x000F   /* supported, info, BIOS output, colour */
#define VIDEO_MODE_ATTRIBUTES_GRAPHICS 0x009B  /* D0|D1|D3|D4|D7 */
#define VIDEO_WINDOW_READ_WRITE       0x07
#define VIDEO_TEXT_WINDOW_KB          32
#define VIDEO_TEXT_WINDOW_BYTES       0x8000u
#define VIDEO_GRAPHICS_WINDOW_KB      64
#define VIDEO_SEGMENT_COLOUR_TEXT     0xB800
#define VIDEO_SEGMENT_GRAPHICS        0xA000
#define VIDEO_TEXT_ATTRIBUTE_BITS     4
#define VIDEO_MEMORY_MODEL_TEXT       0
#define VIDEO_MEMORY_MODEL_PACKED     4
#define VIDEO_MEMORY_MODEL_DIRECT     6
#define VIDEO_VBE_MAX_IMAGE_PAGES     256u
#define VIDEO_DIRECT_RESERVED_USABLE  0x02

/* 4F02h, 4F04h, 4F06h and 4F07h subfunctions. */
#define VIDEO_MODE_NOT_STANDARD       0xFF
#define VIDEO_DAC_WIDTH_6             6
#define VIDEO_STATE_GET_SIZE          0x00
#define VIDEO_STATE_SAVE          0x01
#define VIDEO_STATE_RESTORE       0x02
#define VIDEO_SCAN_SET_PIXELS         0x00
#define VIDEO_SCAN_GET                0x01
#define VIDEO_SCAN_SET_BYTES          0x02
#define VIDEO_SCAN_GET_MAXIMUM        0x03
#define VIDEO_START_SET               0x00
#define VIDEO_START_GET               0x01
#define VIDEO_START_SCHEDULE          0x02
#define VIDEO_START_FLIP_STATUS       0x04
#define VIDEO_START_WAIT_RETRACE      0x80

/* 4F05h and 4F08h-4F15h. */
#define VIDEO_VBE_WINDOW              0x05
#define VIDEO_VBE_DAC_WIDTH           0x08
#define VIDEO_VBE_PALETTE             0x09
#define VIDEO_VBE_PM_INTERFACE        0x0A
#define VIDEO_VBE_POWER               0x10
#define VIDEO_VBE_DDC                 0x15
#define VIDEO_VBE_NO_SUCH_FUNCTION    0x0100
#define VIDEO_WINDOW_SET              0x00
#define VIDEO_WINDOW_GET              0x01
#define VIDEO_WINDOW_A                0x00
#define VIDEO_DAC_SET                 0x00
#define VIDEO_DAC_GET                 0x01
#define VIDEO_PALETTE_SET             0x00
#define VIDEO_PALETTE_GET             0x01
#define VIDEO_PALETTE_SET_SECONDARY   0x02
#define VIDEO_PALETTE_GET_SECONDARY   0x03
#define VIDEO_PALETTE_SET_IN_RETRACE  0x80
#define VIDEO_PALETTE_ENTRY_BYTES     4      /* B, G, R, alignment */
#define VIDEO_PALETTE_BLUE            0
#define VIDEO_PALETTE_GREEN           1
#define VIDEO_PALETTE_RED             2
#define VIDEO_PALETTE_ALIGN           3
#define VIDEO_PM_INTERFACE_GET        0x00
#define VIDEO_POWER_REPORT            0x00
#define VIDEO_POWER_SET               0x01
#define VIDEO_POWER_GET               0x02
#define VIDEO_POWER_VERSION           0x10   /* VBE/PM 1.0, BCD */
#define VIDEO_POWER_STATES            0x0Fu  /* standby, suspend, off, reduced-on */
#define VIDEO_DDC_REPORT              0x00
#define VIDEO_DDC_READ_EDID           0x01
#define VIDEO_DDC_CAPABILITIES        0x0103 /* 1 s per block, DDC1 + DDC2 */
#define VIDEO_EDID_BYTES              128
#define VIDEO_EDID_CHECKSUM           127
#define VIDEO_EDID_CHECKSUM_MODULUS   0x100u
#define VIDEO_EDID_CHECKSUM_MASK      0xFFu

/* INT 10h AH=00h: the mode set. */
#define VIDEO_MODE_NUMBER_MASK        0x7F
#define VIDEO_MODE_CGA_640            0x06
#define VIDEO_CURSOR_SHAPE_DEFAULT    0x0607
#define VIDEO_CGA_SELECT_DEFAULT      0x30
#define VIDEO_CGA_SELECT_MODE_6       0x3F
#define VIDEO_CGA_BUFFER_BYTES        16384
#define VIDEO_AR_MODE_BLINK           0x08u
#define VIDEO_SCAN_SELECT_200         0
#define VIDEO_SCAN_SELECT_400         2
#define VIDEO_DAC_MASK_ALL            0xFF
#define VIDEO_SET_MODE_FLAG_EGA       0x20
#define VIDEO_SET_MODE_FLAG_CGA       0x30
#define VIDEO_SET_MODE_FLAG_MODE_6    0x3F
#define VIDEO_MODE_QUERIES            8

/* INT 10h functions (AH). */
#define VIDEO_FUNCTION_SET_CURSOR_SHAPE 0x01
#define VIDEO_FUNCTION_SET_CURSOR     0x02
#define VIDEO_FUNCTION_GET_CURSOR     0x03
#define VIDEO_FUNCTION_LIGHT_PEN      0x04
#define VIDEO_FUNCTION_SELECT_PAGE    0x05
#define VIDEO_FUNCTION_SCROLL_UP      0x06
#define VIDEO_FUNCTION_SCROLL_DOWN    0x07
#define VIDEO_FUNCTION_READ_CHARACTER 0x08
#define VIDEO_FUNCTION_WRITE_CHARACTER_ATTRIBUTE 0x09
#define VIDEO_FUNCTION_WRITE_CHARACTER 0x0A
#define VIDEO_FUNCTION_CGA_PALETTE    0x0B
#define VIDEO_FUNCTION_WRITE_PIXEL    0x0C
#define VIDEO_FUNCTION_READ_PIXEL     0x0D
#define VIDEO_FUNCTION_TELETYPE       0x0E
#define VIDEO_FUNCTION_GET_MODE       0x0F
#define VIDEO_FUNCTION_PALETTE        0x10
#define VIDEO_FUNCTION_FONT           0x11
#define VIDEO_FUNCTION_ALTERNATE_SELECT 0x12
#define VIDEO_FUNCTION_WRITE_STRING   0x13
#define VIDEO_FUNCTION_DISPLAY_COMBINATION 0x1A
#define VIDEO_FUNCTION_FUNCTIONALITY  0x1B
#define VIDEO_FUNCTION_SAVE_STATE     0x1C
#define VIDEO_LIGHT_PEN_NOT_TRIGGERED 0x0000

/* Pixels: one bit a pixel in a plane, two in CGA's 4-colour modes. */
#define VIDEO_PIXEL_XOR               0x80
#define VIDEO_PIXEL_LEFT_BIT          0x80
#define VIDEO_PIXEL_IN_BYTE_MASK      7
#define VIDEO_PIXELS_PER_PLANE_BYTE   8
#define VIDEO_PLANE_BYTE_SHIFT        3
#define VIDEO_PLANE1_BIT              0x02
#define VIDEO_PLANE2_BIT              0x04
#define VIDEO_PLANE3_BIT              0x08
#define VIDEO_CGA_BYTE_SHIFT          2
#define VIDEO_CGA_PIXEL_TOP_SHIFT     6
#define VIDEO_MODE_CGA_320            0x04
#define VIDEO_MODE_CGA_320_GREY       0x05

/* AH=10h: the attribute palette and the DAC. */
#define VIDEO_AC_SET_REGISTER         0x00
#define VIDEO_AC_SET_BORDER           0x01
#define VIDEO_AC_SET_ALL              0x02
#define VIDEO_AC_BLINK                0x03
#define VIDEO_AC_GET_REGISTER         0x07
#define VIDEO_AC_GET_BORDER           0x08
#define VIDEO_AC_GET_ALL              0x09
#define VIDEO_DAC_SET_REGISTER        0x10
#define VIDEO_DAC_SET_BLOCK           0x12
#define VIDEO_DAC_SELECT_PAGE         0x13
#define VIDEO_DAC_GET_REGISTER        0x15
#define VIDEO_DAC_GET_BLOCK           0x17
#define VIDEO_DAC_GET_PAGE            0x1A
#define VIDEO_DAC_GREY                0x1B
#define VIDEO_DAC_BLOCK_SHIFT         4
#define VIDEO_DAC_BLOCK_MASK          15
#define VIDEO_DAC_BLOCK_BASE_MASK     0xF0
#define VIDEO_DAC_TRACKED_BLOCK       0x30
#define VIDEO_AR_PALETTE_0            0x00
#define VIDEO_AR_PALETTE_1            0x01
#define VIDEO_AR_PALETTE_2            0x02
#define VIDEO_AR_PALETTE_3            0x03

/* AH=13h's AL bits. */
#define VIDEO_STRING_MOVE_CURSOR      0x01
#define VIDEO_STRING_HAS_ATTRIBUTES   0x02

/* AH=11h: the character generator. */
#define VIDEO_FONT_LOAD_MASK          0x0F
#define VIDEO_FONT_LOAD_USER          0x00
#define VIDEO_FONT_LOAD_8X14          0x01
#define VIDEO_FONT_LOAD_8X8           0x02
#define VIDEO_FONT_SET_BLOCK          0x03
#define VIDEO_FONT_LOAD_8X16          0x04
#define VIDEO_FONT_RECALCULATE        0x10
#define VIDEO_FONT_GRAPHICS_INT1F     0x20
#define VIDEO_FONT_GRAPHICS_USER      0x21
#define VIDEO_FONT_GRAPHICS_8X14      0x22
#define VIDEO_FONT_GRAPHICS_8X8       0x23
#define VIDEO_FONT_GRAPHICS_8X16      0x24
#define VIDEO_FONT_GET_INFO           0x30
#define VIDEO_FONT_INFO_INT1F         0x00
#define VIDEO_FONT_INFO_INT43         0x01
#define VIDEO_FONT_INFO_ROM_8X14      0x02
#define VIDEO_FONT_INFO_ROM_8X8       0x03
#define VIDEO_FONT_INFO_ROM_8X8_HIGH  0x04
#define VIDEO_FONT_INFO_ROM_9X14      0x05
#define VIDEO_FONT_QUERIES            4
#define VIDEO_SR_CLOCKING_MODE        1
#define VIDEO_SR_CHARACTER_MAP        3
#define VIDEO_SR1_SCREEN_OFF          0x20u
#define VIDEO_MISC_RAM_ENABLE         0x02u

/* AH=12h: alternate select (BL). */
#define VIDEO_ALTERNATE_GET_INFO      0x10
#define VIDEO_ALTERNATE_SCAN_LINES    0x30
#define VIDEO_ALTERNATE_PALETTE_LOADING 0x31
#define VIDEO_ALTERNATE_VIDEO_ACCESS  0x32
#define VIDEO_ALTERNATE_GREY_SUMMING  0x33
#define VIDEO_ALTERNATE_CURSOR_EMULATION 0x34
#define VIDEO_ALTERNATE_VIDEO_REFRESH 0x36
#define VIDEO_ALTERNATE_INFO_COLOUR_256K 0x0003
#define VIDEO_ALTERNATE_INFO_SWITCHES 0x0009

/* AH=1Ah/1Bh: display combination and the functionality/state block. */
#define VIDEO_DCC_VGA_COLOUR          0x0008
#define VIDEO_STATE_INFO_BYTES        64
#define VIDEO_STATE_INFO_STATIC_POINTER 0
#define VIDEO_STATE_INFO_MODE         4
#define VIDEO_STATE_INFO_COLUMNS      5
#define VIDEO_STATE_INFO_ROWS         0x22
#define VIDEO_STATE_INFO_CHARACTER_HEIGHT 0x23
#define VIDEO_STATE_INFO_ACTIVE_DCC   0x25
#define VIDEO_STATE_INFO_COLOURS      0x27
#define VIDEO_STATE_INFO_PAGES        0x29
#define VIDEO_STATE_INFO_SCAN_LINES   0x2A
#define VIDEO_STATE_INFO_PRIMARY_BLOCK 0x2B
#define VIDEO_STATE_INFO_SECONDARY_BLOCK 0x2C
#define VIDEO_STATE_INFO_MISC         0x2D
#define VIDEO_STATE_INFO_MISC_FLAGS   0x21
#define VIDEO_STATE_INFO_STATIC_TABLE 0x2E   /* ours: the reserved tail */
#define VIDEO_STATIC_ALL_MODES        0xFF
#define VIDEO_STATIC_SCAN_LINES       7
#define VIDEO_STATIC_SCAN_LINES_200_350_400 0x07
#define VIDEO_STATIC_CHARACTER_BLOCKS 8
#define VIDEO_STATIC_ACTIVE_BLOCKS    9
#define VIDEO_STATIC_FONT_BLOCKS      8
#define VIDEO_STATIC_CAPABILITIES     0x0A
#define VIDEO_STATIC_CAPABILITIES_2   0x0B
#define VIDEO_STATIC_CAPABILITY_BITS  0xFF
#define VIDEO_STATIC_CAPABILITY_BITS_2 0x07

/* The VGA's I/O ports. */
#define VIDEO_PORT_MISC_WRITE         0x3C2
#define VIDEO_PORT_INPUT_STATUS_0     0x3C2
#define VIDEO_PORT_VGA_ENABLE         0x3C3
#define VIDEO_PORT_SEQUENCER_INDEX    0x3C4
#define VIDEO_PORT_DAC_MASK           0x3C6
#define VIDEO_PORT_DAC_READ_INDEX     0x3C7
#define VIDEO_PORT_DAC_WRITE_INDEX    0x3C8
#define VIDEO_PORT_DAC_DATA           0x3C9
#define VIDEO_PORT_FEATURE_READ       0x3CA
#define VIDEO_PORT_MISC_READ          0x3CC
#define VIDEO_PORT_GC_INDEX           0x3CE
#define VIDEO_FLOATING_BUS            0xFF
#define VIDEO_STATUS0_SWITCH_SENSE    0x10u
#define VIDEO_STATUS0_VINT            0x80

/* DAC writes, by where the beam was (DacRowHistogram). */
#define VIDEO_ROW_UNKNOWN             0xFFFE
#define VIDEO_DAC_ROW_BIN_TOP         0
#define VIDEO_DAC_ROW_BIN_UPPER       1
#define VIDEO_DAC_ROW_BIN_LOWER       2
#define VIDEO_DAC_ROW_BIN_BLANK       3
#define VIDEO_DAC_ROW_TOP_ROWS        2
#define VIDEO_DAC_ROW_SPLIT           160

/* The Graphics Controller. */
#define VIDEO_GR_INDEX_MASK           15
#define VIDEO_GR_SET_RESET            0
#define VIDEO_GR_ENABLE_SET_RESET     1
#define VIDEO_GR_COLOR_COMPARE        2
#define VIDEO_GR_DATA_ROTATE          3
#define VIDEO_GR_READ_MAP             4
#define VIDEO_GR_MODE                 5
#define VIDEO_GR_COLOR_DONT_CARE      7
#define VIDEO_GR_BIT_MASK             8
#define VIDEO_GR3_MASK                0x1F
#define VIDEO_GR5_READ_MODE_SHIFT     3
#define VIDEO_GR5_UNMODELLED          0x74   /* test, odd/even, shift register, 256-colour */
#define VIDEO_GC_ALU_SHIFT            3
#define VIDEO_GC_ALU_MASK             3
#define VIDEO_GC_ALU_AND              1
#define VIDEO_GC_ALU_OR               2
#define VIDEO_GC_ALU_XOR              3
#define VIDEO_GC_WRITE_MODE_MASK      3
#define VIDEO_GC_READ_MAP_MASK        3
#define VIDEO_WRITE_MODE_1            1
#define VIDEO_WRITE_MODE_2            2
#define VIDEO_WRITE_MODE_3            3
#define VIDEO_ROTATE_MASK             7
#define VIDEO_PLANE_3                 3
#define VIDEO_COMPARE_ALL             0xFF
#define VIDEO_DWORD_BYTES             4

/* The Sequencer. */
#define VIDEO_SR_INDEX_MASK           7
#define VIDEO_SR_MAP_MASK             2
#define VIDEO_SR_MEMORY_MODE          4
#define VIDEO_SR4_CHAIN4_SHIFT        3
#define VIDEO_MAP_MASK_VALUES         16

/* The CRTC. */
#define VIDEO_CR_INDEX_MASK           31
#define VIDEO_CR_HORIZONTAL_DISPLAY_END 0x01
#define VIDEO_CR_CURSOR_START         0x0A
#define VIDEO_CR_CURSOR_END           0x0B
#define VIDEO_CR_START_HIGH           0x0C
#define VIDEO_CR_START_LOW            0x0D
#define VIDEO_CR_CURSOR_HIGH          0x0E
#define VIDEO_CR_CURSOR_LOW           0x0F
#define VIDEO_CR_VERTICAL_RETRACE_END 0x11
#define VIDEO_CR_OFFSET               0x13
#define VIDEO_CR_MODE_CONTROL         0x17
#define VIDEO_CR0A_CURSOR_START_MASK  0x3F
#define VIDEO_CR0B_CURSOR_END_MASK    0x1F
#define VIDEO_CR07_VERTICAL_TOTAL_8   0
#define VIDEO_CR07_DISPLAY_END_8      1
#define VIDEO_CR07_BLANK_START_8      3
#define VIDEO_CR07_LINE_COMPARE_8     4
#define VIDEO_CR07_VERTICAL_TOTAL_9   5
#define VIDEO_CR07_DISPLAY_END_9      6
#define VIDEO_CR09_BLANK_START_9      5
#define VIDEO_CR09_LINE_COMPARE_9     6
#define VIDEO_BIT8_SHIFT              8
#define VIDEO_BIT9_SHIFT              9
#define VIDEO_CR_VERTICAL_TOTAL_BIAS  2
#define VIDEO_VERTICAL_TOTAL_MIN      100u
#define VIDEO_VERTICAL_TOTAL_MAX      1200u
#define VIDEO_CR11_VINT_CLEAR         0x10
#define VIDEO_CR11_VINT_DISABLE       0x20
#define VIDEO_CR11_PROTECT            0x80
#define VIDEO_START_GAP_LAST          4u

/* The register file g_VgaModeDefinitions carries, and the parameter table built from it. */
#define VIDEO_PARAMETER_ENTRY_BYTES   64
#define VIDEO_PARAMETER_COLUMNS       0x00
#define VIDEO_PARAMETER_ROWS          0x01
#define VIDEO_PARAMETER_CHARACTER_HEIGHT 0x02
#define VIDEO_PARAMETER_PAGE_SIZE     0x03
#define VIDEO_PARAMETER_SEQUENCER     0x05
#define VIDEO_PARAMETER_SEQUENCER_COUNT 4
#define VIDEO_PARAMETER_MISC          0x09
#define VIDEO_PARAMETER_CRTC          0x0A
#define VIDEO_PARAMETER_ATTRIBUTE     0x23
#define VIDEO_PARAMETER_ATTRIBUTE_COUNT 20
#define VIDEO_PARAMETER_GC            0x37

/* Frame timing. */
#define VIDEO_PER_MILLE               1000u
#define VIDEO_TALL_FRAME_LINES        500u
#define VIDEO_PRESENT_PER_MILLE_TALL  914
#define VIDEO_PRESENT_PER_MILLE_SHORT 891
#define VIDEO_LATCH_STALE_FRAMES      4u
#define VIDEO_FRAMES_TO_AFTER_NEXT    2u

/* More ports, and the status register 1 bits. */
#define VIDEO_PORT_SEQUENCER_DATA     0x3C5
#define VIDEO_PORT_GC_DATA            0x3CF
#define VIDEO_PORT_CRTC_MONO_DATA     0x3B5
#define VIDEO_PORT_CRTC_COLOUR_DATA   0x3D5
#define VIDEO_PORT_STATUS1_MONO       0x3BA
#define VIDEO_PORT_STATUS1_COLOUR     0x3DA
#define VIDEO_PORT_VBE_DATA           0x1CF
#define VIDEO_INT10_VECTOR            0x10
#define VIDEO_STATUS1_DISPLAY_DISABLED 0x01u
#define VIDEO_STATUS1_VERTICAL_RETRACE 0x08u
#define VIDEO_UINT32_MAX              0xFFFFFFFFu
#define VIDEO_DT3DA_LAST_BUCKET       7
#define VIDEO_DT3DA_BUCKET_BASE       4
#define VIDEO_DT3DA_BUCKET_SHIFT      2

/* The BIOS video tables at VDD_VIDTAB_SEG (save pointer, secondary table, DCC). */
#define VIDEO_FAR_SEGMENT             2      /* a far pointer: offset, then segment */
#define VIDEO_SAVE_POINTER_PARAMETERS 0x00
#define VIDEO_SAVE_POINTER_SECONDARY  0x10
#define VIDEO_SECONDARY_LENGTH        0x00
#define VIDEO_SECONDARY_DCC           0x02
#define VIDEO_SECONDARY_TABLE_BYTES   0x001A
#define VIDEO_DCC_ENTRIES             16
#define VIDEO_DCC_VERSION             1
#define VIDEO_DCC_MAX_CODE            8
#define VIDEO_DCC_HEADER_BYTES        4
#define VIDEO_DCC_ENTRY_BYTES         2

/* Text rendering: character clocks, attributes, blink and the cursor. */
#define VIDEO_CHARACTER_CLOCK_8       8
#define VIDEO_CHARACTER_CLOCK_9       9
#define VIDEO_SR1_8_DOT               0x01
#define VIDEO_AR_MODE_LINE_GRAPHICS   0x04
#define VIDEO_AR_MODE_PPM_SHIFT       5
#define VIDEO_AR_MODE_PIXEL_DOUBLE    0x40
#define VIDEO_LINE_GRAPHICS_FIRST     0xC0
#define VIDEO_LINE_GRAPHICS_LAST      0xDF
#define VIDEO_GLYPH_RIGHT_BIT         0x01
#define VIDEO_ATTRIBUTE_COLOUR_MASK   0x0F
#define VIDEO_ATTRIBUTE_BACKGROUND_SHIFT 4
#define VIDEO_ATTRIBUTE_BACKGROUND_DIM 0x07
#define VIDEO_ATTRIBUTE_BLINK         0x80
#define VIDEO_BLINK_PERIOD_US         1066000u
#define VIDEO_BLINK_HALF_US           533000u
#define VIDEO_CURSOR_BLINK_PERIOD_US  533000u
#define VIDEO_CURSOR_BLINK_HALF_US    266500u
#define VIDEO_CURSOR_LINE_MASK        0x1Fu
#define VIDEO_CURSOR_HIDDEN           0x20u
#define VIDEO_CURSOR_START_FIELD      0x3Fu
#define VIDEO_CGA_CELL_HEIGHT         8u
#define VIDEO_CURSOR_UNDERLINE_LINES  2u
#define VIDEO_CGA_COLOURS             4
#define VIDEO_CGA_PIXELS_PER_BYTE     4

/* The text snapshot and the register dump. */
#define VIDEO_SNAPSHOT_MIN_CAPACITY   16
#define VIDEO_SNAPSHOT_LINE_ROOM      2
#define VIDEO_SNAPSHOT_HEX_ROOM       3
#define VIDEO_SNAPSHOT_HEADER_ROOM    8
#define VIDEO_CHAR_PRINTABLE_FIRST    32
#define VIDEO_CHAR_PRINTABLE_END      127
#define VIDEO_NIBBLE_MASK             0xF
#define VIDEO_DECIMAL_DIGITS          12
#define VIDEO_DECIMAL_BASE            10
#define VIDEO_REGISTER_DUMP_MIN_CAPACITY 1600
#define VIDEO_SR_DECODED              8
#define VIDEO_CR_DECODED              32
#define VIDEO_GR_DECODED              16
#define VIDEO_AR_DECODED              32

/* The renderers: geometry from the CRTC, panning, and mode Y. */
#define VIDEO_CR09_DOUBLE_SCAN        0x80
#define VIDEO_CR09_MAX_SCAN_MASK      0x1Fu
#define VIDEO_CR17_COMPATIBILITY      0x01
#define VIDEO_PIXEL_DOUBLE            2u
#define VIDEO_GEOMETRY_MIN_WIDTH      64u
#define VIDEO_GEOMETRY_MIN_HEIGHT     50u
#define VIDEO_CRTC_OFFSET_UNIT        2u
#define VIDEO_AR_PAN_MASK             0x0Fu
#define VIDEO_PAN_PIXELS              8u
#define VIDEO_PAN_SHIFT_MASK          7u
#define VIDEO_MODE_Y_PAGES            4
#define VIDEO_MODE_Y_PAGE_BYTES       0x4000u
#define VIDEO_MODE_Y_PAGE_USED        16000u
#define VIDEO_MODE_Y_SAMPLE_STRIDE    8
#define VIDEO_MODE_Y_DEFAULT_OFFSET   40
#define VIDEO_MODE_Y_PLANE_SHIFT      2
#define VIDEO_BPP_32                  32

/* Reset. */
#define VIDEO_VGA_ENABLED             0x01
#define VIDEO_BIT_MASK_ALL            0xFF

static VOID VideoLoadModeDefinition(PVIDEO_STATE state, BYTE mode);

/* ⚠ THE ega16 TABLE THAT WAS HERE IS GONE, and so is the ega64_rgb() that replaced
   it. Both hardcoded the sixteen colours a 16-colour mode renders with, which is
   exactly the assumption this file had to stop making: those colours are
   dac[vpal[i]], and both halves belong to the guest. The defaults now come from
   vga_defaults.h, MEASURED per mode; tools/gen/gen-vgadefs.py refuses to generate it
   unless mode 10h's measured DAC matches what ega64_rgb() computed entry for entry,
   so retiring the formula shifted no colour anywhere. */

/* DAC component (0..63) -> 8-bit; pack/unpack a palette entry. (This was dac_pack();
   it is VideoDacPackWidth() below now, which also knows the 4F08h width.) */
/* ⚠ This is <<2, so a guest writing 0x3F gets 252 and not 255 -- it disagrees by a
   few counts with the (v<<2)|(v>>4) the generated defaults use. Pre-existing, and
   left alone deliberately: it is on the path of every guest that programs a palette,
   so it wants its own before/after rather than riding along with this one. It is also
   what makes the width switch lossless: v<<2 >>2 is v again. */

/* ── ★★ THE DAC WIDTH IS A PROPERTY OF THE RAMDAC, SO EVERY PATH INTO IT OBEYS IT. (#226)
     VBE 4F08h switches the DAC between 6 and 8 bits per primary. We accepted the switch
     and honoured it in 4F09h ONLY: port 3C9h still stored `value & 3Fh` and read back
     `>> 2`, so a guest that did what VBE 2.0 §4.11/§4.12 tell it to -- check
     Capabilities D0, set 8 bits, then load its palette the usual way, through the
     ports -- lost the top two bits of every primary (80h became 00h) with a 004Fh in
     hand. Capabilities D0 = 1 was a promise kept for one function out of three.
   ► THE REPRESENTATION DOES NOT CHANGE. dac[] has always held 8 bits per primary; a
     6-bit write is stored as v<<2 (dac_pack) and read back >>2. That is how a
     switchable RAMDAC behaves too: in 6-bit mode the value occupies the top six bits
     of an 8-bit register, so switching the width re-interprets what is there rather
     than rescaling it (6-bit 3Fh reads back FCh at 8 bits). So the renderer, pal[],
     the raster split and the 4F04/AH=1Ch state block are width-blind by construction,
     and only the two conversions below know the width.
   ► THE RESET IS THE SPEC'S: "The DAC palette width is assumed to be reset to the
     standard VGA value of 6 bits per primary color during any mode set" (VBE 2.0
     §4.11) -- INT 10h AH=00h, 4F02h (both arms) and power-on all put it back to 6.
     A width of 0 (a state nothing initialised) reads as 6. */
static INT VideoDacIs8(PCVIDEO_STATE state) { return state->VesaDacWidth == VIDEO_DAC_WIDTH_8; }
/* one primary as the guest wrote it -> the 8-bit component dac[] holds */
static BYTE VideoDacTo8(PCVIDEO_STATE state, BYTE value)
{ return VideoDacIs8(state) ? value : (BYTE)((value & VIDEO_DAC_6BIT_MASK) << VIDEO_DAC_6TO8_SHIFT); }
/* ...and back, for a read */
static BYTE VideoDacFrom8(PCVIDEO_STATE state, BYTE component)
{ return VideoDacIs8(state) ? component : (BYTE)(component >> VIDEO_DAC_6TO8_SHIFT); }
static UINT32 VideoDacPackWidth(PCVIDEO_STATE state, BYTE red, BYTE green, BYTE blue)
{
    return VIDEO_ARGB_OPAQUE | ((UINT32)VideoDacTo8(state, red) << VIDEO_RED_SHIFT)
                       | ((UINT32)VideoDacTo8(state, green) << VIDEO_GREEN_SHIFT) | (UINT32)VideoDacTo8(state, blue);
}

/* ── ★★ THE ATTRIBUTE CONTROLLER, WHICH IS WHERE A 16-COLOUR PIXEL GETS ITS COLOUR.
     A 4-bit pixel does NOT index the DAC. It indexes one of the AC's sixteen palette
     registers (st->PaletteRegisters), and THAT six-bit value indexes the DAC. We stored vpal --
     INT 10h AH=10h has always written it -- and then rendered straight from a fixed
     ega16 table, so nothing a guest did to the palette had any effect. Lemmings sets
     its own palette, which is exactly why its menu came out structurally perfect and
     the wrong colours. */

/* ── WHAT A MODE SET LEAVES BEHIND IS PER MODE. ─────────────────────────────────
     There is no single default AC palette and no single default DAC; the BIOS has a
     table for each mode and they genuinely differ:

         00h-03h, 10h, 12h   AC 00 01 02 03 04 05 14 07 38..3F   DAC = the EGA 64
         0Dh, 0Eh            AC 00 01 02 03 04 05 06 07 10..17   DAC = CGA 16, x4
         13h                 AC 00..0F (identity)                DAC = the VGA 256
         04h/05h/06h/07h/0Fh/11h  each different again

     All of it is measured by tests/probes/dos/vgadefs.asm, which sets each mode and
     reads the registers BACK -- the AC through 0x3C1, the DAC through 0x3C7/0x3C9 --
     on genuine MS-DOS 6.22. tools/gen/gen-vgadefs.py turns that dump into
     vga_defaults.h. Bit 7 of AL ("do not clear the buffer") changes none of it; the
     probe measures 8Dh and 90h to prove that rather than assume it.

   ⚠⚠ THE SINGLE TABLE THAT WAS HERE WAS INFERRED FROM A PICTURE, AND BOTH HALVES OF
     THE INFERENCE WERE WRONG. Card 11 reprogrammed DAC 0..15 to a ramp in mode 0Dh,
     left the AC alone, and read the bars off the screen: bar 6 followed the ramp, so
     index 6 was concluded to be 6 rather than the 0x14 every reference quotes, and
     bars 8..15 showed the EGA brights, so the high eight were concluded to be
     0x38..0x3F. In mode 0Dh the high eight are really 0x10..0x17 -- and the card
     could not tell, because mode 0Dh's default DAC repeats the same sixteen colours
     four times across 0..0x3F, so 0x10..0x17 and 0x38..0x3F hold identical values.
     A bar's colour is a reading of the whole chain and cannot say which link differs.
   ⚠ And the card ran in 0Dh while the guest that prompted it plays in 10h, where
     index 6 IS 0x14. Lemmings carries that exact table inside VGALEMMI.EXE and uses
     it to choose which DAC entries to program, so with 6 there its colour 6 was
     written to DAC 0x14 and read back from DAC 0x06. */
static INT VideoDefaultsRow(BYTE mode)
{
    UINT index;
    for (index = 0; index < VGA_DEFAULT_MODES; ++index)
        if (g_VgaDefaultsByMode[0][index] == mode) return (INT)index;
    /* A mode the BIOS has no table for: a VESA mode, or one nobody defines. Above
       13h means 256 colours, so 13h's defaults; below it, mode 3's. st->ModeKind is
       not set yet at the point this runs, so the mode number is all there is. */
    for (index = 0; index < VGA_DEFAULT_MODES; ++index)
        if (g_VgaDefaultsByMode[VIDEO_DEFAULTS_MODE_ROW][index] == (mode >= VIDEO_MODE_VGA_256 ? VIDEO_MODE_VGA_256 : VIDEO_MODE_TEXT_80)) return (INT)index;
    return 0;
}

static VOID VideoDefaultsFor(BYTE mode,
                             const BYTE **attributes, const unsigned long **dacTable)   /* the generated g_VgaDacDefaults type: ULONG is 32 bits off-VM */
{
    INT index = VideoDefaultsRow(mode);
    *attributes  = g_VgaAttributeDefaults [g_VgaDefaultsByMode[1][index]];
    *dacTable = g_VgaDacDefaults[g_VgaDefaultsByMode[VIDEO_DEFAULTS_DAC_ROW][index]];
}

/* ── ★ A MODE SET REPROGRAMS THE CRTC, and not doing so lets one screen inherit the
     geometry of the one before it. Lemmings' gameplay sets Offset=22 (44 bytes to
     the line, for a 352-pixel-wide scrolling window); the mode 10h screen AFTER it
     was then drawn 44 bytes to the line instead of 80 and came out as diagonal
     noise. The BIOS writes all 25 registers on every mode set -- these values are
     measured per mode by tests/probes/dos/vgadefs.asm.
   ▶ THE VERTICAL TIMING IS NOW APPLIED TOO, and it had to be: the BIOS sets these
     registers, the guest does not, so a mode set is the ONLY place a BIOS-set mode
     ever learns its own geometry. Leaving it out was what kept 0x3DA on the old
     two-case guess -- and that guess is wrong by 45 lines in 640x350 (which is
     Lemmings' MENU; its gameplay is 0Dh). The renderer still takes its picture size from the mode
     table; what these feed is the CRT timing, which is a different question.
   ⚠ Line Compare's three registers are loaded here as well, so it holds the BIOS's
     all-ones "no split" rather than a zero we merely happen to treat as inert. */
static VOID VideoCrtcLineCompareUpdate(PVIDEO_STATE state);
static VOID VideoCrtcVerticalTimingUpdate(PVIDEO_STATE state);
static VOID VideoLatch(PVIDEO_STATE state, INT atFrame);   /* display start/pan schedule (s83) */
static UINT64 VideoVesaVblRelease(PVIDEO_STATE state, INT *isNowInVbl);   /* 4F07 BL=80h (#226) */
static INT VideoBeam(PCVIDEO_STATE state, UINT64 *now, UINT32 *frameUs,
                    UINT32 *verticalTotal, UINT32 *verticalDisplay, UINT32 *verticalBlank,
                    UINT32 *frameNumber, UINT32 *line);
static VOID VideoLoadDefaultCrtc(PVIDEO_STATE state)
{
    const BYTE *crtc = g_VgaCrtcDefaults[g_VgaDefaultsByMode[VIDEO_DEFAULTS_CRTC_ROW][VideoDefaultsRow(state->Mode)]];
    state->CrtcIndex = 0;
    state->CrtcOffset = crtc[VGA_CRTC_OFFSET];
    state->CrtcStart = (UINT32)(((UINT)crtc[VGA_CRTC_START_HI] << BYTE_SHIFT) | crtc[VGA_CRTC_START_LO]);
    state->CrtcStartLive = (WORD)state->CrtcStart;   /* the display follows at once */
    state->StartVs        = (WORD)state->CrtcStart;
    state->LatchTime         = 0;                           /* VideoLatch: take everything as-is */
    state->IsCrtcStartPending = 0;
    state->IsCrtcOffsetSeen = 0;
    state->CrtcOverflow   = crtc[VIDEO_CR_OVERFLOW];
    state->CrtcMaxScan    = crtc[VIDEO_CR_MAX_SCAN];
    state->CrtcLineCompareLow     = crtc[VIDEO_CR_LINE_COMPARE];
    state->CrtcVerticalTotalLow  = crtc[VIDEO_CR_VERTICAL_TOTAL];
    state->CrtcVerticalDisplayEndLow     = crtc[VIDEO_CR_VERTICAL_DISPLAY_END];
    state->CrtcVerticalBlankStartLow     = crtc[VIDEO_CR_VERTICAL_BLANK_START];
    VideoCrtcLineCompareUpdate(state);
    VideoCrtcVerticalTimingUpdate(state);
    state->IsDirty = 1;
}

/* ── ★★★ THE RENDER PALETTE IS DERIVED, NOT STORED. ─────────────────────────────
     pal[] is what the 8-bit framebuffer indexes; dac[] is what the guest programmed.
     In a 16-colour mode they differ, because the hardware puts the Attribute
     Controller between them:
         pixel (4 bits) -> vpal[pixel] -> 6-bit DAC index -> dac[]
     Rebuilding pal[0..15] through that chain is what makes a guest's palette
     actually appear. Mode 13h bypasses the AC (its 8-bit pixel goes straight to the
     DAC), so there pal[] is simply dac[].

   ⚠ THE FIRST CUT OF THIS WAS WRONG IN A WAY WORTH RECORDING: it wrote a COMPUTED
     EGA colour into pal[0..15] and never consulted dac[] at all. That made the test
     card pass -- the card uses the default DAC -- while leaving Lemmings exactly as
     broken, because Lemmings programs the DAC and then points the AC at it. It also
     clobbered the guest's DAC entries, since pal[] and dac[] were the same array.
     A derived table has to derive from something; substituting a plausible value for
     the real one is the same mistake as a stepped-over call returning a sentinel. */
static VOID VideoPaletteSplitNote(PVIDEO_STATE state, const UINT32 *prev);
static INT  VideoBeam(PCVIDEO_STATE state, UINT64 *now, UINT32 *frameUs,
                     UINT32 *verticalTotal, UINT32 *verticalDisplay, UINT32 *verticalBlank,
                     UINT32 *frameNumber, UINT32 *line);   /* fwd: defined with VideoStatusIn */
static VOID VideoPaletteRefresh(PVIDEO_STATE state)
{
    INT index;
    UINT32 prev[NTVDD_PALETTE_ENTRIES];
    for (index = 0; index < NTVDD_PALETTE_ENTRIES; ++index) prev[index] = state->Palette[index];
    /* A packed 8bpp VESA mode indexes the DAC directly, exactly as mode 13h does.
       (s74b) It used to fall through to the attribute-controller path below because a
       4F02 mode set never touches mkind, so pixels 0..15 went through the EGA remap
       (6 -> DAC 0x14, 8..15 -> 0x38..0x3F) and a VESA guest's first sixteen colours
       were somebody else's. */
    if (state->ModeKind == VIDEO_KIND_LINEAR8 || (state->IsVesa && state->VesaBpp <= VIDEO_BPP_INDEXED)) {
        for (index = 0; index < NTVDD_PALETTE_ENTRIES; ++index) state->Palette[index] = state->Dac[index];
    } else {
        for (index = 0; index < VIDEO_EGA_PALETTE_REGISTERS; ++index) {
            BYTE value = (BYTE)(state->PaletteRegisters[index] & VIDEO_AR_PALETTE_MASK);
            /* AR14 Color Select supplies the high DAC bits on a real VGA; zero by
               default, which makes this the identity. */
            if (state->AttributeMode & VIDEO_AR_MODE_P54S)
                value = (BYTE)((value & VIDEO_AR_PALETTE_LOW) | ((state->AttributeColorSelect & VIDEO_AR_CSE_P54) << VIDEO_AR_CSE_SHIFT));
            value = (BYTE)(value | ((state->AttributeColorSelect & VIDEO_AR_CSE_P76) << VIDEO_AR_CSE_SHIFT));
            state->Palette[index] = state->Dac[value];
        }
        for (index = VIDEO_EGA_PALETTE_REGISTERS; index < NTVDD_PALETTE_ENTRIES; ++index) state->Palette[index] = state->Dac[index];
    }
    VideoPaletteSplitNote(state, prev);
    state->IsDirty = 1;
}

/* ── ★★★★ A PALETTE WRITE MID-FRAME IS A RASTER SPLIT, NOT A NEW PALETTE. ─────────
     s70, the user's "flickers between the right and wrong colours" (Lemmings #1/#2).
     Lemmings' HP-mode timer tick is calibrated to land 320 scanlines after the
     retrace -- pixel row 160, the top of the toolbar -- and the tick's first act is
     to push one set of DAC entries 16..23 (guest ds:2668); after the retrace it
     pushes another (ds:2650). The level is drawn in one set, the toolbar in the
     other, and the beam position is what separates them. The presenter took ONE
     palette per frame, so whichever set the snapshot caught coloured the whole
     screen, and the alternation between the two was the flicker. (The real-DOS
     oracle under QEMU shows one set everywhere too: its default 0x3DA model makes
     the retrace wait return at once, so the two writes land back to back. The
     oracle is not truth for a raster effect; the game's own tables and timing are.)
   ► So per entry: the value at the START of the frame (`PaletteBase`), the value written
     MID-frame and the row it was written at (`PaletteSplit`/`PaletteSplitRow`), stamped
     with the frame number so a guest that stops splitting is back to one palette a
     frame later. Writes during blanking or on the first two rows are the frame's
     base (Lemmings' post-retrace push lands on row 0). The first write of a new
     frame rebases: what the palette held before it IS what the DAC held at the
     frame start. VddFramePaletteAt resolves a row; the presenter and the capture
     apply it, video_test pins it with a fake clock. */
#define VIDEO_SPLIT_STICK 12u   /* rows: IRQ jitter measured at +-5 rows (s70), with margin */
static VOID VideoPaletteSplitNote(PVIDEO_STATE state, const UINT32 *prev)
{
    UINT64 now; UINT32 frameUs, verticalTotal, verticalDisplay, verticalBlank, frameNumber, line, row;
    INT index, split;
    if (!VideoBeam(state, &now, &frameUs, &verticalTotal, &verticalDisplay, &verticalBlank, &frameNumber, &line)
        || !state->GraphicsHeight || !verticalDisplay) {
        for (index = 0; index < NTVDD_PALETTE_ENTRIES; ++index) if (state->Palette[index] != prev[index]) state->PaletteBase[index] = state->Palette[index];
        return;
    }
    if (frameNumber != state->PaletteFrameNumber) {           /* first write of this frame: rebase */
        for (index = 0; index < NTVDD_PALETTE_ENTRIES; ++index) state->PaletteBase[index] = prev[index];
        state->PaletteFrameNumber = frameNumber;
    }
    row   = (line < verticalDisplay) ? (UINT32)(((UINT64)line * state->GraphicsHeight) / verticalDisplay) : VIDEO_ROW_NONE;
    split = (row != VIDEO_ROW_NONE && row >= VIDEO_SPLIT_MIN_ROW);
    for (index = 0; index < NTVDD_PALETTE_ENTRIES; ++index) {
        if (state->Palette[index] == prev[index]) continue;
        if (split) {
            /* ── THE BOUNDARY IS STICKY (s70, the user's HP run). The tick that writes
                 this split is an IRQ we deliver with ~100-300 us of jitter, so its row
                 wandered 160..169 frame to frame (heartbeat `lastrow`). Drawn faithfully
                 that is a ten-row band flickering between palettes -- worse than the
                 single-palette picture it replaced. A real PC's IRQ lands within a
                 microsecond of the same row every frame. So a mid-frame write within
                 VIDEO_SPLIT_STICK rows of this entry's live split keeps the OLD row: the
                 boundary stays where the guest first put it, and only a genuinely
                 different row (a new effect) moves it. */
            UINT32 oldRow = state->PaletteSplitRow[index];
            INT live = oldRow && (UINT32)(frameNumber - state->PaletteSplitFrame[index]) <= VIDEO_SPLIT_LIVE_FRAMES;
            UINT32 distance = oldRow > row ? oldRow - row : row - oldRow;
            state->PaletteSplit[index]       = state->Palette[index];
            state->PaletteSplitRow[index]   = (live && distance <= VIDEO_SPLIT_STICK) ? (WORD)oldRow : (WORD)row;
            state->PaletteSplitFrame[index] = frameNumber;
            state->PaletteSplitNotes++;
        } else {
            state->PaletteBase[index] = state->Palette[index];
        }
    }
}

VOID VddVideoFrameTouch(PVIDEO_STATE state)
{
    UINT64 now; UINT32 frameUs, verticalTotal, verticalDisplay, verticalBlank, frameNumber, line;
    state->Frame.PaletteBase  = state->PaletteBase;
    state->Frame.PaletteSplit = state->PaletteSplit;
    state->Frame.SplitRow     = state->IsBlanked ? 0 : state->PaletteSplitRow;   /* SR1.5 (#266) */
    state->Frame.SplitFrame   = state->PaletteSplitFrame;
    state->Frame.FrameNumber = VideoBeam(state, &now, &frameUs, &verticalTotal, &verticalDisplay, &verticalBlank, &frameNumber, &line)
                       ? frameNumber : state->PaletteFrameNumber;
}

/* AH=10h AL=1Bh's sum (30% red, 59% green, 11% blue), over DAC entries [first,
   first+n) -- and since #252 also what AH=12h BL=33h's summing applies to a mode
   set's palette and to AH=10h AL=10h/12h loads. */
static VOID VideoDacGrey(PVIDEO_STATE state, UINT first, UINT count)
{
    UINT index;
    for (index = 0; index < count && (first + index) < NTVDD_PALETTE_ENTRIES; ++index) {
        UINT32 value = state->Dac[first + index];
        UINT32 grey = ((((value >> VIDEO_RED_SHIFT) & VIDEO_CHANNEL_MASK) * VIDEO_GREY_RED) + (((value >> VIDEO_GREEN_SHIFT) & VIDEO_CHANNEL_MASK) * VIDEO_GREY_GREEN)
                      + ((value & VIDEO_CHANNEL_MASK) * VIDEO_GREY_BLUE)) / PERCENT;
        state->Dac[first + index] = VIDEO_ARGB_OPAQUE | (grey << VIDEO_RED_SHIFT) | (grey << VIDEO_GREEN_SHIFT) | grey;
    }
}

/* A mode set reloads the DAC and the AC palette. Real hardware does this, and
   without it a program that reprogrammed the palette leaves the NEXT program (or
   the text screen it returns to) drawn in its colours -- usually near-black, so
   text mode looks dead. */
static VOID VideoLoadDefaultPalette(PVIDEO_STATE state)
{
    INT index;
    /* The guest asked us not to (AH=12h BL=31h). Leave both the DAC and the
       attribute palette exactly as it left them. */
    const BYTE *attributes; const unsigned long *dacTable;   /* not ULONG: see VideoDefaultsFor */
    for (index = 0; index < NTVDD_PALETTE_ENTRIES; ++index) state->PaletteSplitRow[index] = 0;   /* a mode set ends any raster split */
    if (state->IsDefaultPaletteOff) return;
    state->PaletteResets++;
    /* ⚠ DacHighSinceReset ON ITS OWN CANNOT REPORT ANYTHING. The counters are
       printed after the guest has exited, and a guest exits through a mode set back
       to text -- so "since the last reset" is always "since a moment after the last
       thing the guest drew", and the answer is always zero. It read zero for
       Lemmings and was taken as evidence that a mode set had wiped a palette the
       game never rewrote; the port trace shows the game sets the mode and THEN
       writes the palette, which is the only order that can work on real hardware.
       Carry the running maximum too, so the epoch that had the writes survives. */
    if (state->DacHighSinceReset > state->DacHighMax)
        state->DacHighMax = state->DacHighSinceReset;
    state->DacHighSinceReset = 0;
    VideoDefaultsFor(state->Mode, &attributes, &dacTable);
    for (index = 0; index < VIDEO_EGA_PALETTE_REGISTERS; ++index)   state->PaletteRegisters[index] = attributes[index];
    state->PaletteRegisters[VIDEO_OVERSCAN_REGISTER] = 0;
    state->AttributeFlipFlop = state->AttributeIndex = state->AttributeMode = state->AttributeColorSelect = 0;
    for (index = 0; index < NTVDD_PALETTE_ENTRIES; ++index)  state->Dac[index] = VIDEO_ARGB_OPAQUE | (UINT32)dacTable[index];
    if (state->IsGreySum) VideoDacGrey(state, 0, NTVDD_PALETTE_ENTRIES);           /* AH=12h BL=33h (#252) */
    VideoPaletteRefresh(state);
}

/* ── 0x3C0 / 0x3C1: index and data on ONE port, alternating. ─────────────────────
     Write to 0x3C0 and the flip-flop decides whether it lands in the index or the
     data half. The flip-flop is reset by READING 0x3DA -- every guest does that read
     first, which is why VideoStatusIn resets it and why claiming 0x3DA was already
     necessary for this to work at all.
   ⚠ Bit 5 of the INDEX is "video enable" and is not part of the register number: a
     guest programming the palette clears it and sets it again when it has finished.
     Masking it off the index (0x1F) is the difference between writing register 0 and
     writing register 32. */
static VOID VideoAttributeOut(PVOID context, WORD port, BYTE width, UINT32 value)
{
    PVIDEO_STATE state = (PVIDEO_STATE)context;
    BYTE byteValue = (BYTE)(value & VIDEO_CHANNEL_MASK);
    (VOID)width;
    if (port == VIDEO_PORT_AC_READ) return;                       /* data port is read-only       */
    if (!state->AttributeFlipFlop) { state->AttributeIndex = byteValue; state->AttributeFlipFlop = 1; return; }
    state->AttributeFlipFlop = 0;
    if ((state->AttributeIndex & VIDEO_AR_INDEX_MASK) == VIDEO_AR_PAN) VideoLatch(state, 0);   /* pel panning: see VideoLatch */
    state->AttributeRegisters[state->AttributeIndex & VIDEO_AR_INDEX_MASK] = byteValue;
    state->AttributeWrites  [state->AttributeIndex & VIDEO_AR_INDEX_MASK]++;
    switch (state->AttributeIndex & VIDEO_AR_INDEX_MASK) {
    case 0x00: case 0x01: case 0x02: case 0x03:
    case 0x04: case 0x05: case 0x06: case 0x07:
    case 0x08: case 0x09: case 0x0A: case 0x0B:
    case 0x0C: case 0x0D: case 0x0E: case 0x0F:
        state->PaletteRegisters[state->AttributeIndex & VIDEO_AR_PALETTE_LOW] = (BYTE)(byteValue & VIDEO_AR_PALETTE_MASK);
        state->AcPortWrites++;
        VideoPaletteRefresh(state);
        break;
    case VIDEO_AR_MODE:
        /* ── ★ BIT 3 IS BLINK ENABLE, AND IT IS THE HARDWARE'S ANSWER, NOT OURS. ──
             We stored this register and kept a PRIVATE st->IsBlink beside it that only
             INT 10h 1003h could move -- so a program that turns blink off the usual
             way, by writing the attribute controller directly, was ignored and every
             character with attribute bit 7 went on blinking.
             Found in QBasic: its dialogs mark the accelerator letter with bit 7, so
             `Files` rendered as `iles` and `Help` as `elp` every other half-second --
             the letter was there, it was blinking. Two layers with their own copy of
             one fact, disagreeing; AR10 is now the single source. */
        state->AttributeMode = byteValue;
        state->IsBlink = (BYTE)((byteValue >> VIDEO_AR_BLINK_SHIFT) & 1);
        VideoPaletteRefresh(state);
        break;
    case VIDEO_AR_OVERSCAN: state->PaletteRegisters[VIDEO_OVERSCAN_REGISTER] = state->Overscan = (BYTE)(byteValue & VIDEO_AR_PALETTE_MASK); state->IsDirty = 1; break;
    case VIDEO_AR_COLOR_SELECT: state->AttributeColorSelect = byteValue;   VideoPaletteRefresh(state); break;
    default: break;                                  /* 12h plane enable, 13h pan    */
    }
}

static VOID VideoAttributeIn(PVOID context, WORD port, BYTE width, UINT32 *value)
{
    PVIDEO_STATE state = (PVIDEO_STATE)context;
    BYTE index = (BYTE)(state->AttributeIndex & VIDEO_AR_INDEX_MASK);
    (VOID)width;
    if (port == VIDEO_PORT_AC_WRITE) { *value = state->AttributeIndex; return; }   /* index reads back at 3C0 */
    switch (index) {
    case VIDEO_AR_MODE: *value = state->AttributeMode; break;
    case VIDEO_AR_OVERSCAN: *value = state->PaletteRegisters[VIDEO_OVERSCAN_REGISTER];  break;
    case VIDEO_AR_COLOR_SELECT: *value = state->AttributeColorSelect;  break;
    default:   *value = (index < VIDEO_EGA_PALETTE_REGISTERS) ? state->PaletteRegisters[index] : state->AttributeRegisters[index]; break;  /* AR12/AR13 */
    }
}

/* The standard BIOS mode set.  Dimensions are the modes' documented geometry;
   what makes them right for US is that the renderer now honours them instead of
   forcing 80x25 text.  CGA modes 4/5/6 are marked UNSUPPORTED rather than
   approximated: they use a two-bank interleaved layout at B800 that shares
   nothing with the planar path, and quietly showing a text screen instead is the
   silent failure GH #27 exists to remove. */
typedef struct _VIDEO_MODE_ENTRY { BYTE Mode, Kind, Columns, Rows; WORD Width, Height; } VIDEO_MODE_ENTRY;
static const VIDEO_MODE_ENTRY g_VideoModes[] = {
    { 0x00, VIDEO_KIND_TEXT,    40, 25,   320, 400 },
    { 0x01, VIDEO_KIND_TEXT,    40, 25,   320, 400 },
    { 0x02, VIDEO_KIND_TEXT,    80, 25,   640, 400 },
    { 0x03, VIDEO_KIND_TEXT,    80, 25,   640, 400 },
    { 0x04, VIDEO_KIND_CGA,     40, 25,   320, 200 },   /* CGA 4-colour   */
    { 0x05, VIDEO_KIND_CGA,     40, 25,   320, 200 },   /* 4-colour, grey */
    { 0x06, VIDEO_KIND_CGA,     80, 25,   640, 200 },   /* CGA 2-colour   */
    { 0x07, VIDEO_KIND_TEXT,    80, 25,   640, 400 },   /* MDA mono text  */
    { 0x0D, VIDEO_KIND_PLANAR,  40, 25,   320, 200 },
    { 0x0E, VIDEO_KIND_PLANAR,  80, 25,   640, 200 },
    { 0x0F, VIDEO_KIND_PLANAR,  80, 25,   640, 350 },
    { 0x10, VIDEO_KIND_PLANAR,  80, 25,   640, 350 },
    { 0x11, VIDEO_KIND_PLANAR,  80, 30,   640, 480 },
    { 0x12, VIDEO_KIND_PLANAR,  80, 30,   640, 480 },
    { 0x13, VIDEO_KIND_LINEAR8, 40, 25,   320, 200 },
};

/* ── #252: THREE WAYS TO NAME A TEXT CELL, because a page is three things. ──────────
     pcell  page N's (r,c): where the character services write when the caller names
            a page (AH=09h/0Ah/02h... BH).
     cell   the ACTIVE page's (r,c): teletype, scroll, read -- what AH=05h selected.
     dcell  the DISPLAYED (r,c): from the CRTC start address, which is what the card
            shows. AH=05h moves it to the active page, and so does a guest that writes
            CR0C/CR0D itself (a text-mode page flip or smooth scroll).
   All three used to be one function that always meant page 0 at B800:0 -- so 05h
   flipped the BDA and nothing else, and a write to page 1 landed on page 0. The text
   window is 32 KB (B800-BFFF); an address past it wraps, as the card's does. */
static UINT VideoPageSize(PCVIDEO_STATE state);
static BYTE *VideoPageCell(PVIDEO_STATE state, INT page, INT row, INT column)
{
    UINT offset = (UINT)(page & VIDEO_PAGE_MASK) * VideoPageSize(state) + (UINT)(row * state->Columns + column) * VIDEO_CELL_BYTES;
    return state->VideoMemory + VIDEO_TEXT_OFFSET + (offset & VIDEO_TEXT_WINDOW_MASK);
}
static BYTE *VideoCell(PVIDEO_STATE state, INT row, INT column)   /* -> char byte of (row,column) */
{ return VideoPageCell(state, state->Page, row, column); }
static BYTE *VideoDisplayCell(PVIDEO_STATE state, INT row, INT column)
{
    UINT offset = (UINT)state->CrtcStartLive * VIDEO_CELL_BYTES + (UINT)(row * state->Columns + column) * VIDEO_CELL_BYTES;
    return state->VideoMemory + VIDEO_TEXT_OFFSET + (offset & VIDEO_TEXT_WINDOW_MASK);
}

/* ── ONE BACKING STORE FOR A BIT-PLANE, WHOEVER WRITES IT. (s74b) ─────────────────
     The mode-12h engine wrote its four planes into st->Planes[] and VideoRenderPlanar read
     them back -- fine for a real-mode guest, whose pixel loop runs in the host
     interpreter and reaches the engine. A PROTECTED-MODE guest runs natively: its
     `memcpy` to A0000 lands wherever the host has mapped that window, which under the
     mode-Y remap is the SELECTED PLANE'S OWN SECTION (YMapPlane) -- the map-mask port
     handler moves the window on every mask write whenever chain-4 is off, mode 12h
     included. So Hexen's and Heretic's 640x480 loaders were written, in full, into
     memory nothing rendered: `planar hi_water=0`, black screen, for two guests.
     When the host supplies plane sections, the engine, the BIOS pixel services and
     the renderer all use them; without a host (the off-VM harness) st->Planes[] as
     before. The sections are MODEY_WIN = VIDEO_PLANE_SIZE bytes, so nothing changes size. */
static BYTE *VideoPlaneBytes(PVIDEO_STATE state, INT plane)
{ return state->YMapPlane ? state->YMapPlane(state->YMapContext, plane & VIDEO_PLANE_INDEX_MASK) : state->Planes[plane & VIDEO_PLANE_INDEX_MASK]; }

static VOID VideoClearText(PVIDEO_STATE state, BYTE attribute)
{
    INT count = state->Columns * state->Rows, index;
    BYTE *text = state->VideoMemory + VIDEO_TEXT_OFFSET;
    for (index = 0; index < count; ++index) { text[index*VIDEO_CELL_BYTES] = ' '; text[index*VIDEO_CELL_BYTES+1] = attribute; }
}

/* The number of text rows the loaded font gives on a 400-line VGA text screen. */
static BYTE VideoTextRowsFor(BYTE cellHeight)
{ return (BYTE)(cellHeight ? (VIDEO_TEXT_SCAN_LINES / cellHeight) : VIDEO_TEXT_DEFAULT_ROWS); }

/* ── THE BDA DESCRIBES THE DISPLAY, AND A TEXT APPLICATION BELIEVES IT. ──────────
     0040:0049 mode, 004A columns, 004C page size, 004E page offset, 0050 the cursor
     per page, 0060 the cursor shape, 0062 active page, 0063 the CRTC base, 0084 rows
     minus one, 0085 the character height, 0087-0089 the EGA/VGA info bytes. All of
     it read as ZERO before this: rows-1 = 0 is a one-row screen to a program that
     sizes itself from 0040:0084, and the video-info bytes said "no EGA/VGA" to one
     that checks before it asks for 50 lines. Cheap enough to redo after every INT
     10h call and every frame -- a few dozen byte writes -- and correct by
     construction, since it is derived rather than maintained. */
/* The BIOS page size, 0040:004C -- and (#252) the stride AH=05h moves the CRTC start
   by and the character services address a page at. One function, so the BDA, the
   display and the writes cannot disagree about where page N is. */
static UINT VideoModePageSize(BYTE mode, BYTE kind, UINT columns, UINT rows)
{
    UINT pageSize;
    if (kind == VIDEO_KIND_TEXT) {
        pageSize = (columns * rows * VIDEO_CELL_BYTES + VIDEO_PAGE_ALIGN_MASK) & ~VIDEO_PAGE_ALIGN_MASK;
        if (pageSize < VIDEO_TEXT_PAGE_MIN) pageSize = VIDEO_TEXT_PAGE_MIN;
    } else {
        /* ── THE GRAPHICS PAGE SIZE IS PER MODE, and this was a flat 0x2000 for all
             of them. Measured on 6.22: mode 06h is 0x4000 and mode 12h is 0xA000,
             where we said 0x2000 either way. A program that pages by adding this to
             its offset lands inside the previous page. 06h/12h/13h are the
             oracle-verified rows (p_video.asm); the rest are the standard VGA BIOS
             table and are marked unverified in docs/inventory/video-bios.md. */
        switch (mode) {
        case 0x04: case 0x05: case 0x06: pageSize = 0x4000u; break;  /* 06h verified   */
        case 0x0D:                       pageSize = 0x2000u; break;
        case 0x0E:                       pageSize = 0x4000u; break;
        case 0x0F: case 0x10:            pageSize = 0x8000u; break;
        case 0x11: case 0x12:            pageSize = 0xA000u; break;  /* 12h verified   */
        case 0x13:                       pageSize = 0x2000u; break;  /* 13h verified   */
        default:                         pageSize = 0x2000u; break;
        }
    }
    return pageSize;
}
static UINT VideoPageSize(PCVIDEO_STATE state)
{ return VideoModePageSize(state->Mode, state->ModeKind, state->Columns, state->Rows); }

/* The cell height a BIOS mode set gives each standard mode: 8x16 in the VGA text modes
   and the 480-line graphics modes, 8x14 at 350 lines, 8x8 at 200. AH=00h and the
   parameter table (#266) both read it here, so they cannot disagree. */
static BYTE VideoModeCellHeight(BYTE mode)
{
    return (BYTE)((mode <= VIDEO_MODE_TEXT_80 || mode == VIDEO_MODE_MDA || mode == VIDEO_MODE_VGA_MONO_480 || mode == VIDEO_MODE_VGA_480) ? VGA_FONT16_HEIGHT
                   : (mode == VIDEO_MODE_EGA_MONO_350 || mode == VIDEO_MODE_EGA_350) ? VGA_FONT14_HEIGHT : VGA_FONT8_HEIGHT);
}

/* ── #266: THE VECTORS THE VIDEO BIOS OWNS, KEPT IN THE IVT. INT 43h (the graphics
     character table) and INT 1Fh (8x8 characters 80h-FFh) are pointers a program
     READS -- to draw text itself, or to find the font the BIOS will draw with -- and
     nothing in src/ wrote either: the IVT held whatever the VDM started with, which
     on the rig is the real machine's BIOS, not the tables our INT 10h draws from.
     Written through the bus so the off-VM battery's flat memory gets them too. */
static VOID VideoSetVector(PVIDEO_STATE state, BYTE vector, WORD segment, WORD offset)
{
    BYTE *value;
    if (!state->Bus) return;
    value = (BYTE *)VddMapFlat(state->Bus, 0, (WORD)(vector * IVT_ENTRY_SIZE_U));
    if (!value) return;
    value[0] = (BYTE)offset; value[1] = (BYTE)(offset >> BYTE_SHIFT);
    value[2] = (BYTE)segment; value[3] = (BYTE)(segment >> BYTE_SHIFT);
}
/* INT 43h -> our ROM copy of the font a cell of `height` lines draws with. */
static VOID VideoInt43Rom(PVIDEO_STATE state, BYTE height)
{
    state->Int43Segment = height == VGA_FONT8_HEIGHT ? VDD_FONT8X8_SEG : height == VGA_FONT14_HEIGHT ? VDD_FONT8X14_SEG : VDD_FONT8X16_SEG;
    state->Int43Offset = 0;
    state->IsGraphicsFontUser = 0;
    VideoSetVector(state, VIDEO_VECTOR_GRAPHICS_FONT, state->Int43Segment, state->Int43Offset);
}
/* INT 1Fh -> the upper half of our 8x8 table (power-on; AH=11h AL=20h replaces it). */
static VOID VideoInt1FRom(PVIDEO_STATE state)
{
    state->Int1FSegment = VDD_FONT8X8_SEG; state->Int1FOffset = VIDEO_FONT_HIGH_FIRST * VGA_FONT8_HEIGHT; state->IsInt1FUser = 0;
    VideoSetVector(state, VIDEO_VECTOR_FONT_HIGH, state->Int1FSegment, state->Int1FOffset);
}

/* ── #266: AH=0Bh's arithmetic, from the CGA it emulates. 0040:0066 is the CGA's colour
     select register (3D9h): bits 0-3 the background (border in text, the 320x200
     background, the 640x200 foreground), bit 4 the intensity of palette colours 1-3,
     bit 5 the palette. A VGA does not decode 3D9h; the BIOS keeps the byte in the BDA
     and turns it into attribute-controller values (vdd_video.h). The arithmetic is the
     one DOSBox's INT10_SetBackgroundBorder/SetColorSelect carry; what pins it is that
     it reproduces the MEASURED mode 04h table from the mode set's own 0066 = 30h:
     AR01-03 = 13h/15h/17h (VGA_MODEDEFs.h, PCem's IBM ROM). */
BYTE VddCgaColourSelect(BYTE current66, BYTE bh, BYTE bl)
{
    if (bh == 0) return (BYTE)((current66 & VIDEO_CGA_SELECT_KEEP) | (bl & VIDEO_CGA_SELECT_BACKGROUND));
    return (BYTE)((current66 & VIDEO_CGA_SELECT_NO_PALETTE) | ((bl & 1u) << VIDEO_CGA_SELECT_PALETTE_SHIFT));
}
BYTE VddCgaBackgroundAr(BYTE bl)
{ return (BYTE)(((bl << 1) & VIDEO_CGA_INTENSITY) | (bl & VIDEO_CGA_RGB)); }
VOID VddCgaPaletteAr(BYTE select66, BYTE attributes[VIDEO_CGA_PALETTE_COLOURS])
{
    BYTE value = (BYTE)((select66 & VIDEO_CGA_INTENSITY) | VIDEO_CGA_PALETTE_BASE | ((select66 >> VIDEO_CGA_SELECT_PALETTE_SHIFT) & 1u));
    attributes[0] = value; attributes[1] = (BYTE)(value + VIDEO_CGA_PALETTE_SECOND); attributes[2] = (BYTE)(value + VIDEO_CGA_PALETTE_THIRD);
}
BYTE VddGraphicsFontRows(BYTE bl, BYTE dl)
{
    switch (bl) {
    case 0x00: return dl;
    case 0x01: return 14;
    case 0x03: return 43;
    default:   return 25;               /* 02h, and SeaVGABIOS's answer to the rest */
    }
}

/* One AC palette register as the BIOS writes it: the register file AND the shadow the
   renderer reads (vpal; 11h = vpal[16]/overscan). Not a guest write: AttributeWrites untouched. */
static VOID VideoAttributeBiosSet(PVIDEO_STATE state, BYTE index, BYTE value)
{
    value = (BYTE)(value & VIDEO_AR_PALETTE_MASK);
    state->AttributeRegisters[index & VIDEO_AR_INDEX_MASK] = value;
    if (index < VIDEO_EGA_PALETTE_REGISTERS) state->PaletteRegisters[index] = value;
    else if (index == VIDEO_AR_OVERSCAN) state->PaletteRegisters[VIDEO_OVERSCAN_REGISTER] = state->Overscan = value;
    state->AcBiosWrites++;
}

VOID VddVideoBdaSync(PVIDEO_STATE state)
{
    BYTE *bda = state->BiosData;
    UINT pageSize;
    if (!bda) return;
    bda[BIOS_BDA_VIDEO_MODE] = state->Mode;
    bda[BIOS_BDA_VIDEO_COLUMNS] = state->Columns; bda[BIOS_BDA_VIDEO_COLUMNS_HIGH] = 0;
    pageSize = VideoPageSize(state);
    bda[BIOS_BDA_VIDEO_PAGE_SIZE] = (BYTE)pageSize; bda[BIOS_BDA_VIDEO_PAGE_SIZE_HIGH] = (BYTE)(pageSize >> BYTE_SHIFT);
    { UINT pageOffset = (UINT)state->Page * pageSize;
      bda[BIOS_BDA_VIDEO_PAGE_OFFSET] = (BYTE)pageOffset; bda[BIOS_BDA_VIDEO_PAGE_OFFSET_HIGH] = (BYTE)(pageOffset >> BYTE_SHIFT); }
    /* All eight cursors (#252) -- the active page's from CursorRow/CursorColumn, the rest
       from the per-page store. Only the active slot used to be written, so a guest
       reading another page's cursor from the BDA read whatever was left there. */
    { UINT page;
      for (page = 0; page < VIDEO_PAGES; ++page) {
          INT isActive = (page == (UINT)(state->Page & VIDEO_PAGE_MASK));
          bda[BIOS_BDA_CURSOR_COLUMN + page * BIOS_BDA_CURSOR_ENTRY_SIZE] = isActive ? state->CursorColumn : state->PageColumn[page];
          bda[BIOS_BDA_CURSOR_ROW + page * BIOS_BDA_CURSOR_ENTRY_SIZE] = isActive ? state->CursorRow : state->PageRow[page];
      } }
    {   /* 0040:0060 follows the same rule as AH=03h: no text cursor in graphics. */
        WORD shape = (state->ModeKind == VIDEO_KIND_TEXT) ? state->CursorShape : 0;
        bda[BIOS_BDA_CURSOR_SHAPE] = (BYTE)shape; bda[BIOS_BDA_CURSOR_SHAPE_HIGH] = (BYTE)(shape >> BYTE_SHIFT); }
    bda[BIOS_BDA_ACTIVE_PAGE] = state->Page;
    { UINT crtc = (state->Mode == VIDEO_MODE_MDA) ? VIDEO_PORT_CRTC_MONO : VIDEO_PORT_CRTC_COLOUR;
      bda[BIOS_BDA_CRTC_PORT] = (BYTE)crtc; bda[BIOS_BDA_CRTC_PORT_HIGH] = (BYTE)(crtc >> BYTE_SHIFT); }
    bda[BIOS_BDA_VIDEO_ROWS] = (BYTE)(state->Rows ? state->Rows - 1 : VIDEO_TEXT_DEFAULT_ROWS - 1);
    bda[BIOS_BDA_CHARACTER_HEIGHT] = state->CellHeight; bda[BIOS_BDA_CHARACTER_HEIGHT_HIGH] = 0;
    /* 256K, EGA/VGA active, cursor emulation on; bit 7 = the last mode set (AH=00h AL
       bit 7, or 4F02h D15) did not clear memory -- see VideoInt10 AH=00h and vesa 4F02h. */
    bda[BIOS_BDA_EGA_INFO] = (BYTE)(VIDEO_EGA_INFO_BASE | (state->IsModeSetNoClear ? VIDEO_EGA_INFO_NO_CLEAR : 0x00)
                        | (state->IsCursorEmulationOff ? VIDEO_EGA_INFO_CURSOR_EMULATION_OFF : 0x00));      /* bit 0: 12h BL=34h (#252) */
    bda[BIOS_BDA_EGA_SWITCHES] = VIDEO_EGA_SWITCHES;                                    /* feature/switch bits: enhanced colour */
    /* 0089: bit 0 = VGA active; bits 7,4 = scan lines (0,0 = 350; 0,1 = 400; 1,0 = 200). */
    bda[BIOS_BDA_VGA_FLAGS] = (BYTE)((state->GraphicsHeight == VIDEO_LINES_200) ? VIDEO_VGA_FLAGS_200 : (state->GraphicsHeight == VIDEO_LINES_350) ? VIDEO_VGA_FLAGS_350 : VIDEO_VGA_FLAGS_400);
}

/* ══ #252: THE CHARACTER SERVICES IN A GRAPHICS MODE DRAW, IN THAT MODE'S LAYOUT. ══
     AH=09h/0Ah/0Eh/13h used to write (char, attr) pairs through cell() -- B800:0 --
     in EVERY mode. In mode 13h that is memory the screen does not show (text
     vanished); in the CGA modes 04h-06h it IS the frame buffer, so the pairs came out
     as pixel noise; and the planar modes alone got a glyph, always 8x16 at a 640-pixel
     stride, which is right for 11h/12h only. A VGA BIOS draws the ROM glyph for the
     mode's own cell (8x8 at 200 lines, 8x14 at 350, 8x16 at 480 -- CellHeight) into the
     mode's own memory:
       13h      one byte a pixel, `GraphicsWidth` bytes a line, foreground BL, background 0;
       04h/05h  two bits a pixel across the two interleaved CGA banks (even lines at
                B800:0, odd at B800:2000), 80 bytes a line, colour BL&3;
       06h      one bit a pixel, same banks, colour BL&1;
       0Dh-12h  one bit a pixel per PLANE, gw/8 bytes a line, page N at N*pagesize,
                colour BL&0Fh written plane by plane (bits 4-6 are NOT a background
                -- the old planar path took one from them).
     BL bit 7 = XOR the glyph onto what is there, in every mode but 13h (p_vidtxt:
     PCem's IBM VGA ROM is the authority). Background pixels are written 0 otherwise.
     Measured against PCem's IBM VGA ROM, DOSBox-X and SeaVGABIOS by p_vidtxt.asm. */
static const BYTE *VideoGraphicsFont(PCVIDEO_STATE state, BYTE character, INT *height)
{
    /* #266: a CALLER-SUPPLIED table (AH=11h AL=21h -> INT 43h, AL=20h -> INT 1Fh) is
       drawn from where the vector points, as the BIOS draws: SeaVGABIOS's get_font_data
       takes INT 1Fh for characters 80h-FFh of an 8-line cell, INT 43h for the rest. The
       ROM case is unchanged -- our vectors point at copies of these same tables. */
    if (state->Bus && (state->IsGraphicsFontUser || state->IsInt1FUser) && state->CellHeight >= 1 && state->CellHeight <= VGA_FONT16_HEIGHT) {
        const BYTE *table = 0;
        *height = state->CellHeight;
        if (*height == VGA_FONT8_HEIGHT && character >= VIDEO_FONT_HIGH_FIRST && state->IsInt1FUser)
            table = (const BYTE *)VddMapFlat(state->Bus, state->Int1FSegment,
                                              (WORD)(state->Int1FOffset + (character - VIDEO_FONT_HIGH_FIRST) * VGA_FONT8_HEIGHT));
        else if (state->IsGraphicsFontUser)
            table = (const BYTE *)VddMapFlat(state->Bus, state->Int43Segment,
                                              (WORD)(state->Int43Offset + (UINT)character * (UINT)*height));
        if (table) return table;
    }
    *height = state->CellHeight == VGA_FONT14_HEIGHT ? VGA_FONT14_HEIGHT : state->CellHeight == VGA_FONT16_HEIGHT ? VGA_FONT16_HEIGHT : VGA_FONT8_HEIGHT;
    return *height == VGA_FONT8_HEIGHT ? g_VgaFont8x8[character] : *height == VGA_FONT14_HEIGHT ? g_VgaFont8x14[character] : g_VgaFont8x16[character];
}

/* One glyph row's worth of pixels at character cell (col,row) of page pg -- the
   address arithmetic for every graphics kind lives here and in VideoGraphicsRowAddress, so the
   draw, the scroll and the read-back cannot disagree. */
static VOID VideoGraphicsGlyph(PVIDEO_STATE state, INT page, INT column, INT row, BYTE character, BYTE colour)
{
    INT height, glyphY, plane, isXor = (colour & VIDEO_COLOUR_XOR) != 0;
    const BYTE *glyph = VideoGraphicsFont(state, character, &height);
    /* A VESA mode: our ModeInfoBlock says D2 = 0, "BIOS TTY output not supported",
       and the A0000 window is one bank of a bigger picture -- draw nothing. */
    /* ...nor in an UNCHAINED 256-colour mode (mode Y: chain-4 off): A0000 is then the
       plane-mapped window and a chained byte store would land in one plane. */
    if (state->IsVesa || (state->ModeKind == VIDEO_KIND_LINEAR8 && !state->IsChain4)) return;
    if (column < 0 || row < 0 || column >= state->Columns || row >= state->Rows) return;
    if (state->ModeKind == VIDEO_KIND_LINEAR8) {
        UINT32 width = state->GraphicsWidth ? state->GraphicsWidth : VIDEO_MODE13_WIDTH;
        for (glyphY = 0; glyphY < height; ++glyphY) {
            UINT32 offset = (UINT32)(row * height + glyphY) * width + (UINT32)column * VIDEO_GLYPH_WIDTH;
            INT glyphX;
            if (offset + VIDEO_GLYPH_WIDTH > VIDEO_APERTURE_SIZE) return;
            for (glyphX = 0; glyphX < VIDEO_GLYPH_WIDTH; ++glyphX)
                state->VideoMemory[offset + glyphX] = (glyph[glyphY] & (VIDEO_GLYPH_LEFT_BIT >> glyphX)) ? colour : 0;
        }
    } else if (state->ModeKind == VIDEO_KIND_PLANAR) {
        UINT32 bytesPerRow = (UINT32)(state->GraphicsWidth ? state->GraphicsWidth : VIDEO_MODE12_WIDTH) / VIDEO_GLYPH_WIDTH;
        UINT32 base = (UINT32)(page & VIDEO_PAGE_MASK) * VideoPageSize(state);
        for (glyphY = 0; glyphY < height; ++glyphY) {
            UINT32 offset = base + (UINT32)(row * height + glyphY) * bytesPerRow + (UINT32)column;
            if (offset >= VIDEO_PLANE_SIZE) return;
            for (plane = 0; plane < VIDEO_PLANES; ++plane) {
                BYTE foreground = (BYTE)(((colour >> plane) & 1) ? glyph[glyphY] : 0);
                if (isXor) VideoPlaneBytes(state,plane)[offset] ^= foreground; else VideoPlaneBytes(state,plane)[offset] = foreground;
            }
        }
    } else if (state->ModeKind == VIDEO_KIND_CGA) {
        BYTE *memory = state->VideoMemory + VIDEO_TEXT_OFFSET;
        for (glyphY = 0; glyphY < height; ++glyphY) {
            INT line = row * height + glyphY;
            UINT32 offset = ((line & 1) ? VIDEO_CGA_ODD_BANK : 0u) + (UINT32)(line >> 1) * VIDEO_CGA_BYTES_PER_LINE;
            BYTE bits = glyph[glyphY];
            if (state->CgaBpp == 1) {
                BYTE value = (BYTE)((colour & 1) ? bits : 0);
                offset += (UINT32)column;
                if (isXor) memory[offset] ^= value; else memory[offset] = value;
            } else {
                WORD value = 0; INT bit;
                for (bit = 0; bit < VIDEO_GLYPH_WIDTH; ++bit)              /* 8 pixels -> 16 bits, MSB first */
                    if (bits & (VIDEO_GLYPH_LEFT_BIT >> bit)) value |= (WORD)((colour & VIDEO_CGA_PIXEL_MASK) << (VIDEO_CGA_TOP_SHIFT - VIDEO_CGA_BITS_PER_PIXEL * bit));
                offset += (UINT32)column * VIDEO_CGA_GLYPH_BYTES;
                if (isXor) { memory[offset] ^= (BYTE)(value >> BYTE_SHIFT); memory[offset + 1] ^= (BYTE)value; }
                else   { memory[offset]  = (BYTE)(value >> BYTE_SHIFT); memory[offset + 1]  = (BYTE)value; }
            }
        }
    }
    state->IsDirty = 1;
}

/* AH=08h in a graphics mode: there is no character code in memory, so the BIOS reads
   the cell's pixels back (non-zero = foreground) and looks the pattern up in the font
   it draws with. No match = 0. */
static BYTE VideoGraphicsReadChar(PVIDEO_STATE state, INT page, INT column, INT row)
{
    BYTE pattern[VGA_FONT16_HEIGHT];
    INT height, glyphY, candidate;
    (VOID)VideoGraphicsFont(state, 0, &height);
    if (state->IsVesa || column < 0 || row < 0 || column >= state->Columns || row >= state->Rows) return 0;
    for (glyphY = 0; glyphY < height; ++glyphY) {
        INT line = row * height + glyphY, glyphX;
        BYTE bits = 0;
        if (state->ModeKind == VIDEO_KIND_LINEAR8) {
            UINT32 width = state->GraphicsWidth ? state->GraphicsWidth : VIDEO_MODE13_WIDTH, offset = (UINT32)line * width + (UINT32)column * VIDEO_GLYPH_WIDTH;
            if (offset + VIDEO_GLYPH_WIDTH > VIDEO_APERTURE_SIZE) return 0;
            for (glyphX = 0; glyphX < VIDEO_GLYPH_WIDTH; ++glyphX) if (state->VideoMemory[offset + glyphX]) bits |= (BYTE)(VIDEO_GLYPH_LEFT_BIT >> glyphX);
        } else if (state->ModeKind == VIDEO_KIND_PLANAR) {
            UINT32 bytesPerRow = (UINT32)(state->GraphicsWidth ? state->GraphicsWidth : VIDEO_MODE12_WIDTH) / VIDEO_GLYPH_WIDTH;
            UINT32 offset = (UINT32)(page & VIDEO_PAGE_MASK) * VideoPageSize(state) + (UINT32)line * bytesPerRow + (UINT32)column;
            INT plane;
            if (offset >= VIDEO_PLANE_SIZE) return 0;
            for (plane = 0; plane < VIDEO_PLANES; ++plane) bits |= VideoPlaneBytes(state, plane)[offset];
        } else if (state->ModeKind == VIDEO_KIND_CGA) {
            const BYTE *memory = state->VideoMemory + VIDEO_TEXT_OFFSET + ((line & 1) ? VIDEO_CGA_ODD_BANK : 0u) + (UINT32)(line >> 1) * VIDEO_CGA_BYTES_PER_LINE;
            if (state->CgaBpp == 1) bits = memory[column];
            else {
                WORD value = (WORD)((memory[column * VIDEO_CGA_GLYPH_BYTES] << BYTE_SHIFT) | memory[column * VIDEO_CGA_GLYPH_BYTES + 1]);
                for (glyphX = 0; glyphX < VIDEO_GLYPH_WIDTH; ++glyphX) if ((value >> (VIDEO_CGA_TOP_SHIFT - VIDEO_CGA_BITS_PER_PIXEL * glyphX)) & VIDEO_CGA_PIXEL_MASK) bits |= (BYTE)(VIDEO_GLYPH_LEFT_BIT >> glyphX);
            }
        } else return 0;
        pattern[glyphY] = bits;
    }
    for (candidate = 0; candidate < VGA_FONT_CHARACTERS; ++candidate) {
        INT glyphHeight; const BYTE *glyph = VideoGraphicsFont(state, (BYTE)candidate, &glyphHeight);
        for (glyphY = 0; glyphY < height && glyph[glyphY] == pattern[glyphY]; ++glyphY) ;
        if (glyphY == height) return (BYTE)candidate;
    }
    return 0;
}

/* The bytes of one pixel line y (0..gh-1) for character columns [left, right], and
   how many: the unit VideoGraphicsScroll moves. Planar returns plane 0's; the caller adds
   the same offset into the other three. */
static BYTE *VideoGraphicsRowAddress(PVIDEO_STATE state, INT plane, INT line, INT left, INT right, UINT32 *count)
{
    if (state->ModeKind == VIDEO_KIND_LINEAR8) {
        UINT32 width = state->GraphicsWidth ? state->GraphicsWidth : VIDEO_MODE13_WIDTH, offset = (UINT32)line * width + (UINT32)left * VIDEO_GLYPH_WIDTH;
        *count = (UINT32)(right - left + 1) * VIDEO_GLYPH_WIDTH;
        return (offset + *count <= VIDEO_APERTURE_SIZE) ? state->VideoMemory + offset : 0;
    }
    if (state->ModeKind == VIDEO_KIND_PLANAR) {
        UINT32 bytesPerRow = (UINT32)(state->GraphicsWidth ? state->GraphicsWidth : VIDEO_MODE12_WIDTH) / VIDEO_GLYPH_WIDTH;
        UINT32 offset = (UINT32)(state->Page & VIDEO_PAGE_MASK) * VideoPageSize(state) + (UINT32)line * bytesPerRow + (UINT32)left;
        *count = (UINT32)(right - left + 1);
        return (offset + *count <= VIDEO_PLANE_SIZE) ? VideoPlaneBytes(state, plane) + offset : 0;
    }
    if (state->ModeKind == VIDEO_KIND_CGA) {
        UINT32 bytesPerPixelGroup = state->CgaBpp == 1 ? 1u : VIDEO_CGA_GLYPH_BYTES;
        UINT32 offset = ((line & 1) ? VIDEO_CGA_ODD_BANK : 0u) + (UINT32)(line >> 1) * VIDEO_CGA_BYTES_PER_LINE + (UINT32)left * bytesPerPixelGroup;
        *count = (UINT32)(right - left + 1) * bytesPerPixelGroup;
        return (offset + *count <= VIDEO_CGA_BUFFER_SIZE) ? state->VideoMemory + VIDEO_TEXT_OFFSET + offset : 0;
    }
    *count = 0; return 0;
}

/* AH=06h/07h and the teletype's scroll in a graphics mode: whole character rows of
   the window move by `lines` (up or down), and the rows uncovered are filled with
   colour `fill` -- BH for 06h/07h, 0 for the teletype. lines = 0 (or more than the
   window) clears it. */
static VOID VideoGraphicsScroll(PVIDEO_STATE state, INT lines, INT top, INT left, INT bottom, INT right,
                       BYTE fill, INT isUp)
{
    INT cellHeight = state->CellHeight == VGA_FONT14_HEIGHT ? VGA_FONT14_HEIGHT : state->CellHeight == VGA_FONT16_HEIGHT ? VGA_FONT16_HEIGHT : VGA_FONT8_HEIGHT;
    INT line, firstLine, lastLine, plane, planeCount = state->ModeKind == VIDEO_KIND_PLANAR ? VIDEO_PLANES : 1;
    if (state->IsVesa || (state->ModeKind == VIDEO_KIND_LINEAR8 && !state->IsChain4)) return;   /* as VideoGraphicsGlyph */
    if (right >= state->Columns) right = state->Columns - 1;
    if (bottom >= state->Rows) bottom = state->Rows - 1;
    if (left > right || top > bottom) return;
    if (lines <= 0 || lines > bottom - top + 1) lines = bottom - top + 1;
    firstLine = top * cellHeight; lastLine = (bottom + 1) * cellHeight - 1;
    for (plane = 0; plane < planeCount; ++plane) {
        BYTE fillByte;
        if (state->ModeKind == VIDEO_KIND_PLANAR)    fillByte = (BYTE)(((fill >> plane) & 1) ? VIDEO_FILL_SET : 0x00);
        else if (state->ModeKind == VIDEO_KIND_CGA)  fillByte = (BYTE)(state->CgaBpp == 1 ? ((fill & 1) ? VIDEO_FILL_SET : 0)
                                                                            : (fill & VIDEO_CGA_PIXEL_MASK) * VIDEO_CGA_FILL_REPEAT);
        else                                 fillByte = fill;
        for (line = isUp ? firstLine : lastLine; isUp ? (line <= lastLine) : (line >= firstLine); line += isUp ? 1 : -1) {
            INT sourceLine = isUp ? line + lines * cellHeight : line - lines * cellHeight;
            UINT32 count, sourceCount, byteIndex;
            BYTE *destination = VideoGraphicsRowAddress(state, plane, line, left, right, &count), *source;
            if (!destination) continue;
            source = (sourceLine >= firstLine && sourceLine <= lastLine) ? VideoGraphicsRowAddress(state, plane, sourceLine, left, right, &sourceCount) : 0;
            for (byteIndex = 0; byteIndex < count; ++byteIndex) destination[byteIndex] = source ? source[byteIndex] : fillByte;
        }
    }
    state->IsDirty = 1;
}

static VOID VideoScrollUp(PVIDEO_STATE state, INT lines, INT top, INT left,
                      INT bottom, INT right, BYTE attribute)
{
    INT row, column;
    if (state->ModeKind != VIDEO_KIND_TEXT) { VideoGraphicsScroll(state, lines, top, left, bottom, right, attribute, 1); return; }
    if (lines <= 0 || lines > (bottom - top + 1)) {
        for (row = top; row <= bottom; ++row)
            for (column = left; column <= right; ++column) { BYTE *cellPointer = VideoCell(state, row, column); cellPointer[0]=' '; cellPointer[1]=attribute; }
        return;
    }
    for (row = top; row <= bottom - lines; ++row)
        for (column = left; column <= right; ++column) {
            BYTE *destination = VideoCell(state, row, column), *source = VideoCell(state, row + lines, column);
            destination[0] = source[0]; destination[1] = source[1];
        }
    for (row = bottom - lines + 1; row <= bottom; ++row)
        for (column = left; column <= right; ++column) { BYTE *cellPointer = VideoCell(state, row, column); cellPointer[0]=' '; cellPointer[1]=attribute; }
}

/* The teletype's scroll fill: attribute 07h in text (as before), colour 0 -- the
   background -- in a graphics mode. */
static BYTE VideoTeletypeFill(PCVIDEO_STATE state) { return (BYTE)(state->ModeKind == VIDEO_KIND_TEXT ? VIDEO_ATTRIBUTE_NORMAL : 0x00); }

static VOID VideoAdvance(PVIDEO_STATE state)
{
    if (++state->CursorColumn >= state->Columns) {
        state->CursorColumn = 0;
        if (++state->CursorRow >= state->Rows) {
            VideoScrollUp(state, 1, 0, 0, state->Rows - 1, state->Columns - 1, VideoTeletypeFill(state));
            state->CursorRow = state->Rows - 1;
        }
    }
}

/* AH=0Eh, and DOS console output (VddVideoPutChar), on the ACTIVE page. `colour` is
   BL -- the glyph's foreground in a graphics mode, unused in text (the cell keeps
   its attribute). DOS's CON driver calls 0Eh with BL=07h, so that is what
   VddVideoPutChar passes. */
static VOID VideoTeletypeChar(PVIDEO_STATE state, BYTE character, BYTE colour)
{
    switch (character) {
    case VIDEO_CHAR_CARRIAGE_RETURN: state->CursorColumn = 0; break;
    case VIDEO_CHAR_LINE_FEED:
        if (++state->CursorRow >= state->Rows) {
            VideoScrollUp(state, 1, 0, 0, state->Rows - 1, state->Columns - 1, VideoTeletypeFill(state));
            state->CursorRow = state->Rows - 1;
        }
        break;
    case VIDEO_CHAR_BACKSPACE: if (state->CursorColumn) state->CursorColumn--; break;
    case VIDEO_CHAR_BELL: break;
    default:
        if (state->ModeKind == VIDEO_KIND_TEXT) VideoCell(state, state->CursorRow, state->CursorColumn)[0] = character;
        else VideoGraphicsGlyph(state, state->Page, state->CursorColumn, state->CursorRow, character, colour);
        VideoAdvance(state);
    }
}
static VOID VideoTeletype(PVIDEO_STATE state, BYTE character) { VideoTeletypeChar(state, character, VIDEO_ATTRIBUTE_NORMAL); }

/* --- VESA VBE 2.0 (banked, packed-256) ----------------------------------- */
/* supported modes: {VBE number, width, height} (all 8bpp packed) */
/* ── ★★ THE MODE LIST IS THE INTERFACE, AND A GUEST FILTERS ON IT. (s74) ──────────
     This was three 8bpp modes. heaven7 calls 4F00, walks the list we publish, asks
     4F01 about every entry -- we answered OK for all three -- and then printed its
     own "VESA error" WITHOUT EVER CALLING 4F02. Measured, in that order. A guest that
     wants direct colour will not settle for 640x400x8 however cheerfully we describe
     it, so the fix is the list and the ModeInfoBlock behind it, not the answer code.
     Numbers are the VBE standard assignments; 0x11x direct-colour modes are what
     anything from the late 90s actually asks for. Every entry here must fit in
     VIDEO_VESA_VRAM -- see the note there. */
typedef struct _VIDEO_VESA_MODE { WORD Number, Width, Height; BYTE Bpp; } VIDEO_VESA_MODE;
static const VIDEO_VESA_MODE g_VideoVesaModes[] = {
    /* packed-pixel 256-colour */
    { 0x100, 640, 400,  8 }, { 0x101, 640, 480,  8 },
    { 0x103, 800, 600,  8 }, { 0x105, 1024, 768, 8 },
    /* 1024x768 arrived with NTVDD_FRAME_MAX_WIDTH/H (s74b): the presenter's snapshot and
       this list are now sized from ONE number, so a mode cannot be offered that the
       presenter would drop. 0x105 is the 1024x768x8 every VESA game's setup lists. */
    /* ── 320x240, THE MODE HEAVEN7 ASKS FOR BY DEFAULT. (s74b) ───────────────────
         Not a VBE-numbered mode; it is an OEM mode, and the numbers below are the S3
         Trio's (0x151 8bpp, 0x160 15bpp, 0x170 16bpp), which is the set DOSBox and
         every game of the period expects. heaven7's render table is (320,176)
         (512,280) (640,352) (800,440) -- one letterboxed picture per screen height,
         and 320x176 belongs to 320x240, its default. Without this mode it fell back
         to 640x480 and drew its 320x176 picture in the bottom quarter of the screen,
         which read as "the geometry is wrong" for a whole session. A guest picks by
         XResolution/YResolution from 4F01, so the number itself is not load-bearing. */
    { 0x151, 320, 240,  8 }, { 0x160, 320, 240, 15 }, { 0x170, 320, 240, 16 },
    /* direct colour: 15/16/24bpp at the resolutions VRAM can back */
    { 0x10D, 320, 200, 15 }, { 0x10E, 320, 200, 16 }, { 0x10F, 320, 200, 24 },
    { 0x110, 640, 480, 15 }, { 0x111, 640, 480, 16 }, { 0x112, 640, 480, 24 },
    { 0x113, 800, 600, 15 }, { 0x114, 800, 600, 16 }, { 0x115, 800, 600, 24 },
    { 0x116, 1024, 768, 15 }, { 0x117, 1024, 768, 16 }, { 0x118, 1024, 768, 24 },
    { 0x107, 1280, 1024, 8 },
    { 0x119, 1280, 1024, 15 }, { 0x11A, 1280, 1024, 16 }, { 0x11B, 1280, 1024, 24 },
};
/* ── VESA TEXT MODES (VBE 1.2 §, mode numbers 108h..10Ch). (s74b) ─────────────────
     132-column text is what editors and file managers of the period ask for. A VESA
     text mode is an ordinary text mode with a different geometry: it lives at B800,
     goes through VIDEO_KIND_TEXT and every INT 10h text service unchanged, and the
     renderer already sizes the frame from cols x 8 and rows x CellHeight. Only the
     bookkeeping is new: 4F01 answers in characters, 4F02 lands in text kind, 4F03
     remembers the VESA number (a standard mode set forgets it). */
typedef struct _VIDEO_VESA_TEXT_MODE { WORD Number; BYTE Columns, Rows, CellHeight; } VIDEO_VESA_TEXT_MODE;
static const VIDEO_VESA_TEXT_MODE g_VideoVesaTextModes[] = {
    { 0x108,  80, 60,  8 }, { 0x109, 132, 25, 16 }, { 0x10A, 132, 43,  8 },
    { 0x10B, 132, 50,  8 }, { 0x10C, 132, 60,  8 },
};
static INT VideoVesaFindText(WORD modeNumber, BYTE *columns, BYTE *rows, BYTE *cellHeight)
{
    UINT index;
    for (index = 0; index < sizeof(g_VideoVesaTextModes)/sizeof(g_VideoVesaTextModes[0]); ++index)
        if (g_VideoVesaTextModes[index].Number == (modeNumber & VIDEO_VBE_MODE_NUMBER_MASK)) {
            *columns = g_VideoVesaTextModes[index].Columns; *rows = g_VideoVesaTextModes[index].Rows;
            *cellHeight = g_VideoVesaTextModes[index].CellHeight; return 1;
        }
    return 0;
}
/* bytes per pixel as VBE counts them: 15bpp occupies 2 bytes, like 16. */
static UINT32 VideoVesaBytesPerPixel(BYTE bitsPerPixel) { return bitsPerPixel <= VIDEO_BPP_INDEXED ? 1u : bitsPerPixel <= VIDEO_BPP_16 ? VIDEO_BYTES_16BPP : bitsPerPixel <= VIDEO_BPP_24 ? VIDEO_BYTES_24BPP : VIDEO_BYTES_32BPP; }
/* Byte offset into VesaVram of the pixel shown top-left, as the start REGISTER holds
   it (4F07 at the 4F06 pitch; 0 after a mode set). What is actually on screen is
   VesaOriginLive -- the same value once the retrace has loaded it (VideoLatch). */
static UINT32 VideoVesaOrigin(PCVIDEO_STATE state) { return state->VesaOrigin; }
/* (x, y) at the current pitch -> the byte offset the register holds. */
static UINT32 VideoVesaXyOrigin(PCVIDEO_STATE state, UINT32 column, UINT32 line)
{ return line * state->VesaStride + column * VideoVesaBytesPerPixel(state->VesaBpp); }
/* §4.10: "if the requested Display Start coordinates do not allow for a full page of
   video memory ... the Function call should fail and no changes should be made". */
static INT VideoVesaOriginFits(PCVIDEO_STATE state, UINT32 origin)
{
    UINT64 end = (UINT64)origin + (UINT64)(state->VesaHeight ? state->VesaHeight - 1u : 0u) * state->VesaStride
                 + (UINT64)state->VesaWidth * VideoVesaBytesPerPixel(state->VesaBpp);
    return end <= VIDEO_VESA_VRAM;
}

/* ── ★ THE CRT A VESA MODE RUNS ON (#226). ────────────────────────────────────────────
     The retrace model (VideoBeam) took its geometry from the CRTC registers the LAST
     STANDARD MODE left behind -- a 4F02h programs none of them -- so a 640x480 VESA
     mode set from text mode "ran" at text mode's 449 lines and 70 Hz, and from mode
     12h at 60 Hz. Harmless while nothing in a VESA mode asked the beam anything but
     3DAh; 4F07h BL=80h now waits for it, so the answer must be the mode's own.
   ► THE VESA MODES' OWN TIMINGS, not the VGA CRTC's: a VBE BIOS programs its own
     (extended) timing for these, and the published ones are the VESA DMT set at
     60 Hz -- 525 total lines for 480, 628 for 600, 806 for 768, 1066 for 1024 --
     with the two VGA-derived heights kept at the VGA's numbers: 400 lines is the
     449-line 70 Hz frame, and the 200/240-line modes are DOUBLE-SCANNED to 400/480
     as mode 13h is (so 320x200 is 70 Hz and 320x240 is 60 Hz). Blanking starts at
     the end of the picture: the DMT modes carry no border. Anything else scales the
     480-line frame. Only the ratios and the rate matter to the model.
   Returns 0 outside a VESA graphics mode: the caller keeps the VGA path unchanged. */
static INT VideoVesaGeometry(PCVIDEO_STATE state, UINT32 *verticalTotal, UINT32 *verticalDisplay,
                      UINT32 *verticalBlank, UINT32 *hz)
{
    UINT32 lines;
    if (!state->IsVesa || !state->VesaHeight) return 0;
    lines = state->VesaHeight <= VIDEO_DOUBLE_SCAN_MAX_LINES ? state->VesaHeight * VIDEO_DOUBLE_SCAN : state->VesaHeight;   /* double scan */
    *hz = VIDEO_VESA_REFRESH_HZ;
    switch (lines) {
    case 400:  *verticalTotal = 449u;  *hz = VIDEO_VESA_400_REFRESH_HZ; break;
    case VIDEO_VGA_480_LINES:  *verticalTotal = VIDEO_VGA_480_TOTAL;  break;
    case 600:  *verticalTotal = 628u;  break;
    case 768:  *verticalTotal = 806u;  break;
    case 1024: *verticalTotal = 1066u; break;
    default:   *verticalTotal = lines * VIDEO_VGA_480_TOTAL / VIDEO_VGA_480_LINES; if (*verticalTotal <= lines) *verticalTotal = lines + 1u; break;
    }
    *verticalDisplay = *verticalBlank = lines;
    return 1;
}
/* Record a VESA mode query and its answer -- see VesaQueries[] in vdd_video.h. */
static VOID VideoVesaNote(PVIDEO_STATE state, BYTE function, WORD mode, INT isOk)
{
    UINT slot;
    for (slot = 0; slot < state->VesaQueryCount; ++slot)                  /* collapse repeats */
        if (state->VesaQueries[slot] == mode && state->VesaQueryFunction[slot] == function) return;
    if (state->VesaQueryCount >= sizeof(state->VesaQueries)/sizeof(state->VesaQueries[0])) return;
    slot = state->VesaQueryCount++;
    state->VesaQueries[slot] = mode; state->VesaQueryOk[slot] = (BYTE)(isOk ? 1 : 0); state->VesaQueryFunction[slot] = function;
}

static INT VideoVesaFind(WORD modeNumber, WORD *width, WORD *height, BYTE *bitsPerPixel)
{
    UINT index;
    for (index = 0; index < sizeof(g_VideoVesaModes)/sizeof(g_VideoVesaModes[0]); ++index)
        if (g_VideoVesaModes[index].Number == (modeNumber & VIDEO_VBE_MODE_NUMBER_MASK)) {
            UINT32 need = (UINT32)g_VideoVesaModes[index].Width * g_VideoVesaModes[index].Height
                          * VideoVesaBytesPerPixel(g_VideoVesaModes[index].Bpp);
            /* ⚠ A MODE WE CANNOT STORE IS NOT A MODE WE SUPPORT. Answering 4F01 for
                 geometry that does not fit VIDEO_VESA_VRAM invites a 4F02 we would have
                 to fail, or worse, blits off the end of the buffer. Checked here so
                 the list and the answer can never disagree. */
            if (need > VIDEO_VESA_VRAM) return 0;
            *width = g_VideoVesaModes[index].Width; *height = g_VideoVesaModes[index].Height;
            if (bitsPerPixel) *bitsPerPixel = g_VideoVesaModes[index].Bpp;
            return 1;
        }
    return 0;
}
static VOID VideoWrite16(BYTE *bytes, WORD value) { bytes[0] = (BYTE)value; bytes[1] = (BYTE)(value >> BYTE_SHIFT); }
static VOID VideoWrite32(BYTE *bytes, UINT32 value) { bytes[0]=(BYTE)value; bytes[1]=(BYTE)(value >> BYTE_SHIFT); bytes[2]=(BYTE)(value >> WORD_SHIFT); bytes[3]=(BYTE)(value >> TOP_BYTE_SHIFT); }

/* sync the live A0000 window into VesaVram[current bank]. */
static VOID VideoVesaSync(PVIDEO_STATE state)
{
    UINT32 offset = (UINT32)state->VesaBank * VIDEO_VESA_WINDOW; UINT index;
    /* ⚠ AN LFB GUEST NEVER WRITES A0000, so copying that window into vram would
         paint a stale (usually blank) 64KB hole over the frame it just drew. */
    if (state->IsVesaLfb) return;
    if (offset + VIDEO_VESA_WINDOW > VIDEO_VESA_VRAM) return;
    for (index = 0; index < VIDEO_VESA_WINDOW; ++index) state->VesaVram[offset + index] = state->VideoMemory[index];
}

/* Window A to bank `n` (64 KB units): flush the live window into its bank, load the new
   one. 0 = refused (no banked VESA mode, or past the end of VRAM) and nothing changes.
   ONE implementation for INT 10h 4F05h and the 4F0Ah protected-mode code's port write
   (#53), so the two cannot disagree about what a bank switch is. */
static INT VideoVesaSetBank(PVIDEO_STATE state, UINT32 bank)
{
    UINT32 offset = bank * VIDEO_VESA_WINDOW; UINT byteIndex;
    if (!state->IsVesa || state->IsVesaLfb) return 0;
    if (offset + VIDEO_VESA_WINDOW > VIDEO_VESA_VRAM) return 0;
    VideoVesaSync(state);                            /* flush the current bank first */
    state->VesaBank = (WORD)bank;
    for (byteIndex = 0; byteIndex < VIDEO_VESA_WINDOW; ++byteIndex) state->VideoMemory[byteIndex] = state->VesaVram[offset + byteIndex];
    state->IsDirty = 1;
    return 1;
}

/* ══ #53: THE 4F0Ah PROTECTED-MODE INTERFACE. ══════════════════════════════════════
     4F0Ah answered AX=0100h ("no such function") on purpose: there was no code to hand
     out, and 004Fh with a null pointer would have had a client call into nothing. Now
     there is: src/vdd/vbe_pm.asm, assembled into vbe_pm.h -- relocatable 32-bit code
     that drives the card through two ports, as a real card's block drives its own
     registers. A protected-mode client copies it and calls SetWindow / SetDisplayStart /
     SetPalette with a near call: no INT 10h, no mode switch, so a banked frame stops
     costing a DPMI 0300h round trip per bank.
   ► THE PORTS (ours; index at 01CEh, data at 01CFh -- the pair Bochs's VBE uses, and 05h
     is its bank index too; nothing else here is Bochs's, so index 00h, its ID register,
     reads 0 and no Bochs driver will take us for one):
       05h  bank: what 4F05h BH=00h BL=00h does (VideoVesaSetBank); reads back the bank
       10h  display start, bits 0-15, in DWORDS (byte address / 4)
       11h  display start, bits 16-31 -- the write that COMMITS, as 4F07h BL=00h: at once.
            The "during retrace" form waited on 3DAh in the guest's own code before this.
       03h  (read) bits per pixel, 06h (read) logical line in pixels -- what a client that
            wants to compute a start for itself needs; writes ignored.
     A write when no banked VESA mode is set (or one that would not fit) is refused and
     counted (VbePmRejected), and changes nothing -- the INT 10h forms answer 03h/02h there. */
static VOID VideoVbePmInstall(PVIDEO_STATE state)
{
    BYTE *block;
    UINT index;
    if (!state || !state->Bus) return;
    block = (BYTE *)VddMapFlat(state->Bus, VDD_VBEPM_SEG, 0);
    if (!block) return;
    for (index = 0; index < VBE_PM_LEN; ++index) block[index] = g_VbePmBlock[index];
    for (index = 0; index < VBE_RM_LEN; ++index) block[VDD_VBERM_OFF + index] = g_VbeRmWindowFunction[index];   /* #273 */
}
/* both fit their 256 bytes without overlapping (a compile error otherwise) */
typedef char VIDEO_VBE_RM_FITS[(VBE_PM_LEN <= VDD_VBERM_OFF && VDD_VBERM_OFF + VBE_RM_LEN <= VIDEO_VBE_SEGMENT_BYTES) ? 1 : -1];

static VOID VideoVbePortOut(PVOID context, WORD port, BYTE width, UINT32 value)
{
    PVIDEO_STATE state = (PVIDEO_STATE)context;
    WORD data = (WORD)(width >= VIDEO_PORT_WIDTH_WORD ? value : (value & BYTE_MASK));
    if (port == VIDEO_PORT_VBE_INDEX) { state->VbeIndex = data; return; }
    switch (state->VbeIndex) {
    case VIDEO_VBE_INDEX_BANK:
        if (VideoVesaSetBank(state, data)) state->VbePmBankCount++; else state->VbePmRejected++;
        break;
    case VIDEO_VBE_INDEX_START_LOW:
        state->VbeStartLow = data;
        break;
    case VIDEO_VBE_INDEX_START_HIGH: {
        UINT32 origin = (((UINT32)data << WORD_SHIFT) | state->VbeStartLow) * VIDEO_VBE_START_UNIT;
        UINT32 bytesPerPixel = VideoVesaBytesPerPixel(state->VesaBpp);
        if (!state->IsVesa || !state->VesaStride || !VideoVesaOriginFits(state, origin)) { state->VbePmRejected++; break; }
        VideoLatch(state, 0);                     /* boundaries already passed keep the old start */
        state->VesaStartY = (WORD)(origin / state->VesaStride);
        state->VesaStartX = (WORD)((origin % state->VesaStride) / bytesPerPixel);
        state->VesaOrigin = state->VesaOriginVs = state->VesaOriginLive = origin;
        state->VbePmStartCount++;
        state->IsDirty = 1;
        break; }
    default:
        break;                                /* not a register here */
    }
}

static VOID VideoVbePortIn(PVOID context, WORD port, BYTE width, UINT32 *value)
{
    PVIDEO_STATE state = (PVIDEO_STATE)context;
    UINT32 registers = 0;
    (VOID)width;
    if (port == VIDEO_PORT_VBE_INDEX) { *value = state->VbeIndex; return; }
    switch (state->VbeIndex) {
    case VIDEO_VBE_INDEX_BPP: registers = state->IsVesa ? state->VesaBpp : 0; break;
    case VIDEO_VBE_INDEX_BANK: registers = state->VesaBank; break;
    case VIDEO_VBE_INDEX_VIRTUAL_WIDTH: registers = (state->IsVesa && state->VesaBpp) ? state->VesaStride / VideoVesaBytesPerPixel(state->VesaBpp) : 0; break;
    case VIDEO_VBE_INDEX_START_LOW: registers = (state->VesaOrigin / VIDEO_VBE_START_UNIT) & WORD_MASK_U; break;
    case VIDEO_VBE_INDEX_START_HIGH: registers = (state->VesaOrigin / VIDEO_VBE_START_UNIT) >> WORD_SHIFT; break;
    default:   registers = 0; break;                  /* 00h (ID) and the rest: 0 */
    }
    *value = registers;
}

/* ── DIRECT COLOUR -> ARGB, ONCE PER FRAME. (s74) ─────────────────────────────────
     The frame contract has a bpp field, but every consumer of it indexed a palette --
     so a 15/16/24bpp mode has to be converted somewhere, and this is the only place
     that knows the guest's pixel format. Channels are expanded by REPLICATING the
     high bits into the low ones (5 bits -> 8 as (v<<3)|(v>>2)), not by shifting and
     leaving zeros: a white pixel must come out 0xFF, and 0x1F<<3 is 0xF8, which is a
     visibly grey white and the classic giveaway of a lazy 5-to-8 expansion. */
/* Bounding offsets of everything non-zero in the framebuffer. Answers, without any
   assumption about stride or origin, WHERE the guest put its pixels. */
static VOID VideoVesaScanWritten(PVIDEO_STATE state)
{
    UINT32 index, low = VIDEO_OFFSET_NONE, high = 0, nonZero = 0;
    UINT32 end = state->VesaOriginLive + state->VesaStride * state->VesaHeight;   /* the displayed page */
    if (!end || end > VIDEO_VESA_VRAM) end = VIDEO_VESA_VRAM;
    for (index = 0; index < end; ++index)
        if (state->VesaVram[index]) { if (low == VIDEO_OFFSET_NONE) low = index; high = index; ++nonZero; }
    state->VramLow = (low == VIDEO_OFFSET_NONE) ? 0 : low;
    state->VramHigh = high; state->VramNonZero = nonZero;
}

static VOID VideoVesaToArgb(PVIDEO_STATE state)
{
    UINT32 line, column, width = state->VesaWidth, height = state->VesaHeight, pitch = state->VesaStride;
    UINT32 bytesPerPixel = VideoVesaBytesPerPixel(state->VesaBpp), origin = state->VesaOriginLive;  /* as displayed (VideoLatch) */
    if (!width || !height || !pitch) return;
    if (width > VIDEO_VESA_MAX_WIDTH || height > VIDEO_VESA_MAX_HEIGHT) return;   /* cannot happen: VideoVesaFind caps it */
    for (line = 0; line < height; ++line) {
        const BYTE *source = state->VesaVram + origin + line * pitch;  /* from the 4F07 start */
        UINT32 *destination = state->VesaArgb + line * width;
        if (origin + line * pitch + width * bytesPerPixel > VIDEO_VESA_VRAM) break;
        for (column = 0; column < width; ++column) {
            UINT32 red, green, blue;
            if (state->VesaBpp == VIDEO_BPP_15) {
                UINT32 value = (UINT32)source[column*VIDEO_BYTES_16BPP] | ((UINT32)source[column*VIDEO_BYTES_16BPP+1] << BYTE_SHIFT);
                red = (value >> VIDEO_RGB555_RED_SHIFT) & VIDEO_RGB_5BIT_MASK; green = (value >> VIDEO_RGB5_GREEN_SHIFT) & VIDEO_RGB_5BIT_MASK; blue = value & VIDEO_RGB_5BIT_MASK;
                red = (red << VIDEO_EXPAND5_SHIFT) | (red >> VIDEO_EXPAND5_REFILL); green = (green << VIDEO_EXPAND5_SHIFT) | (green >> VIDEO_EXPAND5_REFILL); blue = (blue << VIDEO_EXPAND5_SHIFT) | (blue >> VIDEO_EXPAND5_REFILL);
            } else if (state->VesaBpp == VIDEO_BPP_16) {
                UINT32 value = (UINT32)source[column*VIDEO_BYTES_16BPP] | ((UINT32)source[column*VIDEO_BYTES_16BPP+1] << BYTE_SHIFT);
                red = (value >> VIDEO_RGB565_RED_SHIFT) & VIDEO_RGB_5BIT_MASK; green = (value >> VIDEO_RGB5_GREEN_SHIFT) & VIDEO_RGB_6BIT_MASK; blue = value & VIDEO_RGB_5BIT_MASK;
                red = (red << VIDEO_EXPAND5_SHIFT) | (red >> VIDEO_EXPAND5_REFILL); green = (green << VIDEO_EXPAND6_SHIFT) | (green >> VIDEO_EXPAND6_REFILL); blue = (blue << VIDEO_EXPAND5_SHIFT) | (blue >> VIDEO_EXPAND5_REFILL);
            } else {                                   /* 24bpp, B G R in memory */
                blue = source[column*VIDEO_BYTES_24BPP]; green = source[column*VIDEO_BYTES_24BPP+VIDEO_BGR_GREEN]; red = source[column*VIDEO_BYTES_24BPP+VIDEO_BGR_RED];
            }
            destination[column] = VIDEO_ARGB_OPAQUE | (red << VIDEO_RED_SHIFT) | (green << VIDEO_GREEN_SHIFT) | blue;
        }
    }
}

/* ── SAVE / RESTORE STATE: one block for INT 10h AH=1Ch and VBE 4F04 (§4.7). (s74b)
     AH=1Ch used to report 3 blocks (192 bytes) and then write 768 bytes of DAC into the
     caller's buffer -- the Heretic MCB overrun again, in a different function -- and
     4F04 did not exist. A guest treats the buffer as opaque, so the layout is ours:
        +0   'NTVS'  +4 mask  +6 version
        +8   mode, IsVesa, VesaMode(2), VesaBpp, IsVesaLfb, dacwidth, bank(2),
             stride(4) @+20, start_x(2) @+24, start_y(2) @+26
        +32  DAC, 256 x (R,G,B) at 8 bits each -- the 8-bit width would lose precision
             at 6 bits, and AH=1Ch's own 6-bit format is produced from this on the way out
        +800 the attribute controller (vpal[17], AR10, AR14, overscan), text cursor
             (row, col, shape), page, CRTC start/offset
     896 bytes = 14 blocks, reported for every mask; the spec permits over-reporting
     and a guest allocates from the answer, which is the one thing that must hold.
     Restore re-enters the saved mode with the DON'T-CLEAR bit -- frame buffer memory
     is explicitly not part of the state -- then puts the registers back over it. */
#define VIDEO_STATE_BLOCK_BYTES 64u
#define VIDEO_STATE_BYTES  896u
#define VIDEO_STATE_BLOCKS (VIDEO_STATE_BYTES / VIDEO_STATE_BLOCK_BYTES)
#define VIDEO_SAVE_MASK               4
#define VIDEO_SAVE_VERSION            6
#define VIDEO_SAVE_FORMAT_VERSION     1
#define VIDEO_SAVE_MODE               8
#define VIDEO_SAVE_IS_VESA            9
#define VIDEO_SAVE_VESA_MODE          10
#define VIDEO_SAVE_VESA_BPP           12
#define VIDEO_SAVE_IS_LFB             13
#define VIDEO_SAVE_DAC_WIDTH          14
#define VIDEO_SAVE_BANK               16
#define VIDEO_SAVE_STRIDE             20
#define VIDEO_SAVE_START_X            24
#define VIDEO_SAVE_START_Y            26
#define VIDEO_SAVE_DAC                32
#define VIDEO_SAVE_PALETTE            800
#define VIDEO_PALETTE_REGISTERS_AND_BORDER  (VIDEO_EGA_PALETTE_REGISTERS + 1)   /* + the border */
#define VIDEO_SAVE_ATTRIBUTE_MODE     817
#define VIDEO_SAVE_COLOR_SELECT       818
#define VIDEO_SAVE_OVERSCAN           819
#define VIDEO_SAVE_CURSOR_ROW         820
#define VIDEO_SAVE_CURSOR_COLUMN      821
#define VIDEO_SAVE_CURSOR_SHAPE       822
#define VIDEO_SAVE_PAGE               824
#define VIDEO_SAVE_CRTC_START         826
#define VIDEO_SAVE_CRTC_OFFSET        828
static VOID VideoInt10(PVOID context, PNTVDD_REGISTERS registers);
static VOID VideoVesa(PVIDEO_STATE state, PNTVDD_REGISTERS registers);
static VOID VideoStateSave(PVIDEO_STATE state, BYTE *buffer, WORD mask)
{
    UINT index;
    for (index = 0; index < VIDEO_STATE_BYTES; ++index) buffer[index] = 0;
    buffer[0] = 'N'; buffer[1] = 'T'; buffer[2] = 'V'; buffer[3] = 'S'; VideoWrite16(buffer + VIDEO_SAVE_MASK, mask); VideoWrite16(buffer + VIDEO_SAVE_VERSION, VIDEO_SAVE_FORMAT_VERSION);
    buffer[VIDEO_SAVE_MODE] = state->Mode; buffer[VIDEO_SAVE_IS_VESA] = state->IsVesa; VideoWrite16(buffer + VIDEO_SAVE_VESA_MODE, state->VesaMode);
    buffer[VIDEO_SAVE_VESA_BPP] = state->VesaBpp; buffer[VIDEO_SAVE_IS_LFB] = state->IsVesaLfb; buffer[VIDEO_SAVE_DAC_WIDTH] = state->VesaDacWidth;
    VideoWrite16(buffer + VIDEO_SAVE_BANK, state->VesaBank); VideoWrite32(buffer + VIDEO_SAVE_STRIDE, state->VesaStride);
    VideoWrite16(buffer + VIDEO_SAVE_START_X, state->VesaStartX); VideoWrite16(buffer + VIDEO_SAVE_START_Y, state->VesaStartY);
    for (index = 0; index < NTVDD_PALETTE_ENTRIES; ++index) {
        buffer[VIDEO_SAVE_DAC + index*VIDEO_RGB_BYTES]     = (BYTE)(state->Dac[index] >> VIDEO_RED_SHIFT);
        buffer[VIDEO_SAVE_DAC + index*VIDEO_RGB_BYTES + 1] = (BYTE)(state->Dac[index] >> VIDEO_GREEN_SHIFT);
        buffer[VIDEO_SAVE_DAC + index*VIDEO_RGB_BYTES + VIDEO_RGB_BLUE] = (BYTE)(state->Dac[index]);
    }
    for (index = 0; index < VIDEO_PALETTE_REGISTERS_AND_BORDER; ++index) buffer[VIDEO_SAVE_PALETTE + index] = state->PaletteRegisters[index];
    buffer[VIDEO_SAVE_ATTRIBUTE_MODE] = state->AttributeMode; buffer[VIDEO_SAVE_COLOR_SELECT] = state->AttributeColorSelect; buffer[VIDEO_SAVE_OVERSCAN] = state->Overscan;
    buffer[VIDEO_SAVE_CURSOR_ROW] = state->CursorRow; buffer[VIDEO_SAVE_CURSOR_COLUMN] = state->CursorColumn; VideoWrite16(buffer + VIDEO_SAVE_CURSOR_SHAPE, state->CursorShape);
    buffer[VIDEO_SAVE_PAGE] = state->Page; VideoWrite16(buffer + VIDEO_SAVE_CRTC_START, state->CrtcStart); buffer[VIDEO_SAVE_CRTC_OFFSET] = state->CrtcOffset;
}
/* 1 = restored, 0 = not a buffer we wrote (the caller answers AH=02). */
static INT VideoStateLoad(PVIDEO_STATE state, const BYTE *buffer)
{
    UINT index; NTVDD_REGISTERS modeRegisters;
    if (buffer[0] != 'N' || buffer[1] != 'T' || buffer[2] != 'V' || buffer[3] != 'S') return 0;
    for (index = 0; index < sizeof modeRegisters; ++index) ((BYTE *)&modeRegisters)[index] = 0;
    if (buffer[VIDEO_SAVE_IS_VESA]) {                                   /* back into the VESA mode, no clear */
        WORD bx = (WORD)(VIDEO_VBE_MODE_NO_CLEAR | (buffer[VIDEO_SAVE_IS_LFB] ? VIDEO_VBE_MODE_LFB : 0u) | (buffer[VIDEO_SAVE_VESA_MODE] | (buffer[VIDEO_SAVE_VESA_MODE + 1] << BYTE_SHIFT)));
        VddSetAh(&modeRegisters, VIDEO_FUNCTION_VESA); VddSetAl(&modeRegisters, VIDEO_VBE_SET_MODE); VddSetBx(&modeRegisters, bx); VideoVesa(state, &modeRegisters);
        state->VesaDacWidth = buffer[VIDEO_SAVE_DAC_WIDTH]; state->VesaBank = (WORD)(buffer[VIDEO_SAVE_BANK] | (buffer[VIDEO_SAVE_BANK + 1] << BYTE_SHIFT));
        state->VesaStride   = (UINT32)buffer[VIDEO_SAVE_STRIDE] | ((UINT32)buffer[VIDEO_SAVE_STRIDE + 1] << BYTE_SHIFT) | ((UINT32)buffer[VIDEO_SAVE_STRIDE + 2] << WORD_SHIFT) | ((UINT32)buffer[VIDEO_SAVE_STRIDE + 3] << TOP_BYTE_SHIFT);
        state->VesaStartX  = (WORD)(buffer[VIDEO_SAVE_START_X] | (buffer[VIDEO_SAVE_START_X + 1] << BYTE_SHIFT));
        state->VesaStartY  = (WORD)(buffer[VIDEO_SAVE_START_Y] | (buffer[VIDEO_SAVE_START_Y + 1] << BYTE_SHIFT));
        state->VesaOrigin = state->VesaOriginVs = state->VesaOriginLive =   /* shown at once (#226) */
            VideoVesaOriginFits(state, VideoVesaXyOrigin(state, state->VesaStartX, state->VesaStartY))
                ? VideoVesaXyOrigin(state, state->VesaStartX, state->VesaStartY) : 0u;
    } else {                                      /* a standard mode, bit 7 = no clear */
        VddSetAh(&modeRegisters, VIDEO_FUNCTION_SET_MODE); VddSetAl(&modeRegisters, (BYTE)(buffer[VIDEO_SAVE_MODE] | VIDEO_MODE_NO_CLEAR)); VideoInt10(state, &modeRegisters);
    }
    for (index = 0; index < NTVDD_PALETTE_ENTRIES; ++index)
        state->Dac[index] = VIDEO_ARGB_OPAQUE | ((UINT32)buffer[VIDEO_SAVE_DAC + index*VIDEO_RGB_BYTES] << VIDEO_RED_SHIFT)
                   | ((UINT32)buffer[VIDEO_SAVE_DAC + index*VIDEO_RGB_BYTES + 1] << VIDEO_GREEN_SHIFT) | (UINT32)buffer[VIDEO_SAVE_DAC + index*VIDEO_RGB_BYTES + VIDEO_RGB_BLUE];
    for (index = 0; index < VIDEO_PALETTE_REGISTERS_AND_BORDER; ++index) state->PaletteRegisters[index] = buffer[VIDEO_SAVE_PALETTE + index];
    state->AttributeMode = buffer[VIDEO_SAVE_ATTRIBUTE_MODE]; state->AttributeColorSelect = buffer[VIDEO_SAVE_COLOR_SELECT]; state->Overscan = buffer[VIDEO_SAVE_OVERSCAN];
    state->CursorRow = buffer[VIDEO_SAVE_CURSOR_ROW]; state->CursorColumn = buffer[VIDEO_SAVE_CURSOR_COLUMN]; state->CursorShape = (WORD)(buffer[VIDEO_SAVE_CURSOR_SHAPE] | (buffer[VIDEO_SAVE_CURSOR_SHAPE + 1] << BYTE_SHIFT));
    state->Page = buffer[VIDEO_SAVE_PAGE]; state->CrtcStart = (WORD)(buffer[VIDEO_SAVE_CRTC_START] | (buffer[VIDEO_SAVE_CRTC_START + 1] << BYTE_SHIFT)); state->CrtcOffset = buffer[VIDEO_SAVE_CRTC_OFFSET];
    VideoPaletteRefresh(state); state->IsDirty = 1;
    return 1;
}

/* INT 10h AX=4Fxx. Always returns AX=0x004F (supported+ok) for what we handle. */
static VOID VideoVesa(PVIDEO_STATE state, PNTVDD_REGISTERS registers)
{
    BYTE al = VddGetAl(registers); UINT index;
    if (al < VIDEO_VBE_FUNCTION_COUNT) {                              /* inventory: see VesaCalls[]   */
        BYTE bl = (BYTE)(VddGetBx(registers) & BYTE_MASK);
        state->VesaCalls[al]++;
        state->VesaBl[al] |= (WORD)(bl == VIDEO_VESA_TALLY_BL_80 ? VIDEO_VESA_TALLY_BIT_80 : bl < VIDEO_VESA_TALLY_BL_LIMIT ? (1u << bl) : 0u);
    }
    switch (al) {
    case VIDEO_VBE_CONTROLLER_INFO: {                                  /* return controller info       */
        /* ⚠ THE CALLER'S BLOCK IS 256 BYTES UNLESS IT PRESET "VBE2". (s74) VBE 2.0
             §4.3: a VbeInfoBlock is 256 bytes for a VBE 1.x caller and 512 only when
             the caller wrote "VBE2" into the signature first; the OEM string and mode
             list go in the reserved area at +34 (that is what it is for). We used to
             put the OEM string at +0x100 and the mode list at +0x120 REGARDLESS.
             Heretic allocates exactly 16 paragraphs for this call, so +0x100 was the
             MCB of the next block: its signature became 'N' (of "NTVDMEX VESA"), the
             chain walk stopped there, ~480 KB above went invisible, and its next DOS
             allocation died with "I_AllocLow: DOS alloc of 1024 failed, 256 free".
             Doom never probes VESA, which is why only Heretic paid. */
        BYTE *buffer = (BYTE *)VddMapFlat(state->Bus, registers->Es, (WORD)(WORD)registers->Edi);
        INT isVbe2 = (buffer[0]=='V' && buffer[1]=='B' && buffer[2]=='E' && buffer[3]=='2');
        const UINT OEM = 0x22, MODES = 0x40;  /* both inside the reserved area */
        for (index = 0; index < (isVbe2 ? VIDEO_VBE_INFO_BYTES_V2 : VIDEO_VBE_INFO_BYTES); ++index) buffer[index] = 0;
        buffer[0]='V'; buffer[1]='E'; buffer[2]='S'; buffer[3]='A';
        VideoWrite16(buffer + VIDEO_VBE_INFO_VERSION, VIDEO_VBE_VERSION_2);                      /* VBE 2.0                      */
        VideoWrite32(buffer + VIDEO_VBE_INFO_OEM_STRING, ((UINT32)registers->Es << WORD_SHIFT) | (((WORD)registers->Edi + OEM) & WORD_MASK_U));    /* OEM string */
        /* Capabilities D0 = "DAC width is switchable to 8 bits per primary" (§4.3).
           We answer 4F08 BH=8 with 8 -- and advertised 0 here, so a guest that
           follows the spec's own advice ("query capabilities before 4F08") never
           asked. Found by p_vesa against QEMU's VBE (s74b), the first oracle row. */
        VideoWrite32(buffer + VIDEO_VBE_INFO_CAPABILITIES, VIDEO_VBE_CAPABILITY_DAC_SWITCHABLE);                          /* capabilities: D0 DAC switchable */
        VideoWrite32(buffer + VIDEO_VBE_INFO_MODE_LIST, ((UINT32)registers->Es << WORD_SHIFT) | (((WORD)registers->Edi + MODES) & WORD_MASK_U));  /* mode list  */
        VideoWrite16(buffer + VIDEO_VBE_INFO_TOTAL_MEMORY, VIDEO_VESA_VRAM / VIDEO_VBE_MEMORY_UNIT);    /* total memory in 64KB units   */
        { const char *oemString = "NTVDMEX VESA"; for (index = 0; oemString[index]; ++index) buffer[OEM + index] = (BYTE)oemString[index]; buffer[OEM+index]=0; }
        if (isVbe2) {                               /* VBE 2.0 fields, only for a 2.0 caller */
            /* ── THE FOUR STRINGS GO IN OemData (+100h), EACH ITS OWN (#226). §4.3: "VBE
                 2.0 BIOS implementations must place this string [OemString] in the
                 OemData area within the VbeInfoBlock if 'VBE2' is preset", and "The
                 OemVendorName string, OemProductName string and OemProductRev string
                 are copied into this area by the VBE implementation" -- so a protected-
                 mode client can turn each far pointer into an offset in ITS copy of the
                 block. All four pointed at the OEM string at +22h: inside the block,
                 but in the Reserved area §4.3 keeps for the mode list, and three of
                 them named the wrong thing. A 1.x caller (no 'VBE2', 256 bytes) keeps
                 the +22h string: it has no OemData, and +100h is not its memory. */
            static const char *const strings[VIDEO_VBE_OEM_STRINGS] = { "NTVDMEX VESA", "NTVDMEX", "NTVDMEX VBE", "1.00" };
            static const UINT pointerOffsets[VIDEO_VBE_OEM_STRINGS] = { VIDEO_VBE_INFO_OEM_STRING, VIDEO_VBE_INFO_OEM_VENDOR, VIDEO_VBE_INFO_OEM_PRODUCT, VIDEO_VBE_INFO_OEM_PRODUCT_REVISION };   /* OemString, Vendor, Product, Rev */
            UINT stringOffset = VIDEO_VBE_INFO_OEM_DATA, byteIndex;
            VideoWrite16(buffer + VIDEO_VBE_INFO_OEM_SOFTWARE_REVISION, VIDEO_VBE_OEM_REVISION_1_00);                 /* OEM software rev 1.00        */
            for (byteIndex = 0; byteIndex < VIDEO_VBE_OEM_STRINGS; ++byteIndex) {
                VideoWrite32(buffer + pointerOffsets[byteIndex], ((UINT32)registers->Es << WORD_SHIFT) | (((WORD)registers->Edi + stringOffset) & WORD_MASK_U));
                for (index = 0; strings[byteIndex][index]; ++index) buffer[stringOffset++] = (BYTE)strings[byteIndex][index];
                buffer[stringOffset++] = 0;
            }
        }
        for (index = 0; index < sizeof(g_VideoVesaModes)/sizeof(g_VideoVesaModes[0]); ++index)
            VideoWrite16(buffer + MODES + index*VIDEO_VBE_MODE_NUMBER_BYTES, g_VideoVesaModes[index].Number);
        { UINT textMode;
          for (textMode = 0; textMode < sizeof(g_VideoVesaTextModes)/sizeof(g_VideoVesaTextModes[0]); ++textMode, ++index)
              VideoWrite16(buffer + MODES + index*VIDEO_VBE_MODE_NUMBER_BYTES, g_VideoVesaTextModes[textMode].Number); }
        VideoWrite16(buffer + MODES + index*VIDEO_VBE_MODE_NUMBER_BYTES, VIDEO_VBE_MODE_LIST_END);            /* mode-list terminator         */
        VddSetAx(registers, VIDEO_VBE_OK);
        break; }
    case VIDEO_VBE_MODE_INFO: {                                  /* return mode info             */
        WORD width, height; BYTE modeBitsPerPixel = VIDEO_BPP_INDEXED;
        { BYTE textColumns, textRows, textCellHeight;
          if (VideoVesaFindText(VddGetCx(registers), &textColumns, &textRows, &textCellHeight)) {   /* a TEXT mode: answer in characters */
              BYTE *buffer = (BYTE *)VddMapFlat(state->Bus, registers->Es, (WORD)registers->Edi);
              UINT textPageBytes = (UINT)textColumns * textRows * VIDEO_CELL_BYTES;
              VideoVesaNote(state, VIDEO_VBE_MODE_INFO, VddGetCx(registers), 1);
              for (index = 0; index < VIDEO_MODE_INFO_BYTES; ++index) buffer[index] = 0;
              VideoWrite16(buffer + VIDEO_MODE_INFO_ATTRIBUTES, VIDEO_MODE_ATTRIBUTES_TEXT);              /* supported|opt info|BIOS output|colour; bit 4 clear = TEXT */
              buffer[VIDEO_MODE_INFO_WINDOW_A_ATTRIBUTES] = VIDEO_WINDOW_READ_WRITE; buffer[VIDEO_MODE_INFO_WINDOW_B_ATTRIBUTES] = 0x00;         /* WinA r/w/exists; WinB none    */
              VideoWrite16(buffer + VIDEO_MODE_INFO_GRANULARITY, VIDEO_TEXT_WINDOW_KB); VideoWrite16(buffer + VIDEO_MODE_INFO_WINDOW_SIZE, VIDEO_TEXT_WINDOW_KB); /* the 32 KB colour-text window  */
              VideoWrite16(buffer + VIDEO_MODE_INFO_WINDOW_A_SEGMENT, VIDEO_SEGMENT_COLOUR_TEXT); VideoWrite16(buffer + VIDEO_MODE_INFO_WINDOW_B_SEGMENT, 0);
              VideoWrite32(buffer + VIDEO_MODE_INFO_WINDOW_FUNCTION, 0);
              VideoWrite16(buffer + VIDEO_MODE_INFO_BYTES_PER_LINE, (WORD)(textColumns * VIDEO_CELL_BYTES)); /* bytes per character row       */
              VideoWrite16(buffer + VIDEO_MODE_INFO_X_RESOLUTION, textColumns); VideoWrite16(buffer + VIDEO_MODE_INFO_Y_RESOLUTION, textRows); /* X/Y resolution IN CHARACTERS  */
              buffer[VIDEO_MODE_INFO_X_CHARACTER] = VIDEO_GLYPH_WIDTH; buffer[VIDEO_MODE_INFO_Y_CHARACTER] = textCellHeight;            /* char cell                     */
              buffer[VIDEO_MODE_INFO_PLANES] = 1;                        /* planes                        */
              buffer[VIDEO_MODE_INFO_BITS_PER_PIXEL] = VIDEO_TEXT_ATTRIBUTE_BITS;                        /* bits per pixel (attribute)    */
              buffer[VIDEO_MODE_INFO_BANKS] = 1;                        /* NumberOfBanks                 */
              buffer[VIDEO_MODE_INFO_MEMORY_MODEL] = VIDEO_MEMORY_MODEL_TEXT;                        /* MemoryModel 0 = text          */
              buffer[VIDEO_MODE_INFO_BANK_SIZE] = 0;                        /* BankSize                      */
              buffer[VIDEO_MODE_INFO_IMAGE_PAGES] = (BYTE)(textPageBytes ? (VIDEO_TEXT_WINDOW_BYTES / textPageBytes) - 1u : 0u);   /* image pages */
              buffer[VIDEO_MODE_INFO_RESERVED] = 1;                        /* Reserved = 1                  */
              VddSetAx(registers, VIDEO_VBE_OK);
              break;
          } }
        VideoVesaNote(state, VIDEO_VBE_MODE_INFO, VddGetCx(registers), VideoVesaFind(VddGetCx(registers), &width, &height, &modeBitsPerPixel));
        if (VideoVesaFind(VddGetCx(registers), &width, &height, &modeBitsPerPixel)) {
            BYTE *buffer = (BYTE *)VddMapFlat(state->Bus, registers->Es, (WORD)(WORD)registers->Edi);
            UINT32 bytesPerPixel = VideoVesaBytesPerPixel(modeBitsPerPixel), pitch = (UINT32)width * bytesPerPixel;
            for (index = 0; index < VIDEO_MODE_INFO_BYTES; ++index) buffer[index] = 0;
            /* ⛔ D5 STAYS CLEAR (s84, a user-found regression). #226 set it (0xBB, "not
                 VGA compatible", as QEMU's SeaVGABIOS does) and ZARMMX stopped seeing VESA
                 at all: its VESA 1 renderer draws through the banked window at A0000, which
                 D5 set says may not exist, so it filtered out every mode and fell back to
                 320x200 (headless A/B, runs/s84/zarvesa/: 0xBB -> VGA, 0x9B -> 640x480).
                 Our window DOES work -- 4F05 banking drove ZAR's VESA 1 in s74c -- so for
                 the window D5 clear is the true answer, and it is what the period card
                 says too (Tseng ET4000 under PCem: 1Fh/1Bh, D5 clear). */
            VideoWrite16(buffer + VIDEO_MODE_INFO_ATTRIBUTES, VIDEO_MODE_ATTRIBUTES_GRAPHICS);                  /* attrs: supported|color|graphics */
            buffer[VIDEO_MODE_INFO_WINDOW_A_ATTRIBUTES] = VIDEO_WINDOW_READ_WRITE; buffer[VIDEO_MODE_INFO_WINDOW_B_ATTRIBUTES] = 0x00;             /* WinA r/w/exists; WinB none    */
            VideoWrite16(buffer + VIDEO_MODE_INFO_GRANULARITY, VIDEO_GRAPHICS_WINDOW_KB); VideoWrite16(buffer + VIDEO_MODE_INFO_WINDOW_SIZE, VIDEO_GRAPHICS_WINDOW_KB);     /* granularity / size (KB)       */
            VideoWrite16(buffer + VIDEO_MODE_INFO_WINDOW_A_SEGMENT, VIDEO_SEGMENT_GRAPHICS); VideoWrite16(buffer + VIDEO_MODE_INFO_WINDOW_B_SEGMENT, 0); /* WinA seg / WinB seg           */
            /* #273: WinFuncPtr -> the real-mode stub beside the 4F0Ah block (vbe_rm.asm).
                 It was NULL ("use 4F05h"), legal, but a VBE 1.x program that far-calls it
                 without checking ran 0000:0000. Re-planted on every 4F01h, like 4F0Ah's. */
            VideoVbePmInstall(state);
            VideoWrite32(buffer + VIDEO_MODE_INFO_WINDOW_FUNCTION, ((UINT32)VDD_VBEPM_SEG << WORD_SHIFT) | VDD_VBERM_OFF);
            VideoWrite16(buffer + VIDEO_MODE_INFO_BYTES_PER_LINE, (WORD)pitch);        /* bytes per scan line           */
            VideoWrite16(buffer + VIDEO_MODE_INFO_X_RESOLUTION, width); VideoWrite16(buffer + VIDEO_MODE_INFO_Y_RESOLUTION, height);     /* X / Y resolution              */
            /* char cell: the BIOS font the mode's line count implies -- 8x8 at 200
               lines, 8x14 at 350, 8x16 otherwise. The ET4000/W32p ROM says YCharSize=8
               for 320x200 (p_vesa vs pcem-vesa, s74b); we said 16 for everything. */
            buffer[VIDEO_MODE_INFO_X_CHARACTER] = VIDEO_GLYPH_WIDTH; buffer[VIDEO_MODE_INFO_Y_CHARACTER] = (BYTE)(height <= VIDEO_LINES_200 ? VGA_FONT8_HEIGHT : height <= VIDEO_LINES_350 ? VGA_FONT14_HEIGHT : VGA_FONT16_HEIGHT);
            buffer[VIDEO_MODE_INFO_PLANES] = 1; buffer[VIDEO_MODE_INFO_BITS_PER_PIXEL] = modeBitsPerPixel;              /* planes / bits per pixel       */
            /* ⚠ MEMORY MODEL IS NOT A CONSTANT. It was 4 ("packed pixel", i.e. a
                 palette index) for every mode, which is a lie for direct colour --
                 a guest reads this byte to decide whether the bytes it writes are
                 indices or channels. VBE 2.0 §4.4: 04h packed pixel, 06h direct colour. */
            buffer[VIDEO_MODE_INFO_MEMORY_MODEL] = (BYTE)(modeBitsPerPixel > VIDEO_BPP_INDEXED ? VIDEO_MEMORY_MODEL_DIRECT : VIDEO_MEMORY_MODEL_PACKED);
            /* ⚠⚠ NumberOfBanks IS **NOT** "how many 64KB windows the mode needs".
                 I wrote that from memory and it is the wrong CONCEPT, not just a wrong
                 number -- it computed 22 for 640x480x24. VBE 2.0 §4.4: banks are the
                 groups the SCAN LINES are divided into (the CGA/Hercules interleave),
                 and "for modes that don't have scanline banks (such as VGA modes
                 0Dh-13h), this field should be set to 1". BankSize likewise 0.
                 Caught by reading the spec, not by any guest -- heaven7 never looks. */
            buffer[VIDEO_MODE_INFO_BANKS] = 1;                            /* +26 NumberOfBanks: no scanline banks */
            /* ⚠⚠⚠ AND THE OFFSETS FROM HERE WERE OFF BY ONE, ALSO FROM MEMORY. The
                 VBE 2.0 ModeInfoBlock runs +26 NumberOfBanks, +27 MemoryModel,
                 +28 BankSize, +29 NumberOfImagePages, +30 Reserved(=1). I had written
                 NumberOfImagePages at +28 and "reserved must be 1" at +29 -- so the
                 image-page count was landing in BankSize, a 1 was landing in
                 NumberOfImagePages (which is a count MINUS ONE, i.e. claiming two
                 pages of a mode we have one page of VRAM for), and Reserved at +30 was
                 left 0 when the spec says it is always 1 in this version.
                 Three fields wrong, none of which any guest we have would have caught. */
            buffer[VIDEO_MODE_INFO_BANK_SIZE] = 0;                            /* +28 BankSize: no scanline banks   */
            /* +29 NumberOfImagePages = pages VRAM holds MINUS ONE. The s74 audit put
               this field at the right offset and left a 0 in it, which told every
               page-flipping guest there was a single page. QEMU's VBE says 50 for
               640x480x8 in 16 MB; with 4 MB we say 12. Found by p_vesa (s74b). */
            { UINT32 pageBytes = (UINT32)pitch * height;
              UINT32 pageCount = pageBytes ? VIDEO_VESA_VRAM / pageBytes : 1u;
              buffer[VIDEO_MODE_INFO_IMAGE_PAGES] = (BYTE)(pageCount ? (pageCount > VIDEO_VBE_MAX_IMAGE_PAGES ? VIDEO_VBE_MAX_IMAGE_PAGES - 1u : pageCount - 1u) : 0u); }
            buffer[VIDEO_MODE_INFO_RESERVED] = 1;                            /* +30 Reserved: always 1 in VBE 2.0 */
            /* ── DIRECT-COLOUR FIELD LAYOUT (offsets 31..38). A guest cannot pack a
                 pixel without these, and it will not trust a mode that leaves them
                 zero. 15bpp is 5:5:5 with one byte unused, 16bpp is 5:6:5, 24bpp is
                 8:8:8 -- all little-endian, blue in the low bits, which is what every
                 PC VBE implementation does. */
            if (modeBitsPerPixel == VIDEO_BPP_15) {
                buffer[VIDEO_MODE_INFO_RED_SIZE]=5; buffer[VIDEO_MODE_INFO_RED_POSITION]=10;
                buffer[VIDEO_MODE_INFO_GREEN_SIZE]=5; buffer[VIDEO_MODE_INFO_GREEN_POSITION]=5;
                buffer[VIDEO_MODE_INFO_BLUE_SIZE]=5; buffer[VIDEO_MODE_INFO_BLUE_POSITION]=0;
                buffer[VIDEO_MODE_INFO_RESERVED_SIZE]=1; buffer[VIDEO_MODE_INFO_RESERVED_POSITION]=15; }
            else if (modeBitsPerPixel == VIDEO_BPP_16) {
                buffer[VIDEO_MODE_INFO_RED_SIZE]=5; buffer[VIDEO_MODE_INFO_RED_POSITION]=11;
                buffer[VIDEO_MODE_INFO_GREEN_SIZE]=6; buffer[VIDEO_MODE_INFO_GREEN_POSITION]=5;
                buffer[VIDEO_MODE_INFO_BLUE_SIZE]=5; buffer[VIDEO_MODE_INFO_BLUE_POSITION]=0;
                buffer[VIDEO_MODE_INFO_RESERVED_SIZE]=0; buffer[VIDEO_MODE_INFO_RESERVED_POSITION]=0; }
            else if (modeBitsPerPixel == VIDEO_BPP_24) {
                buffer[VIDEO_MODE_INFO_RED_SIZE]=8; buffer[VIDEO_MODE_INFO_RED_POSITION]=16;
                buffer[VIDEO_MODE_INFO_GREEN_SIZE]=8; buffer[VIDEO_MODE_INFO_GREEN_POSITION]=8;
                buffer[VIDEO_MODE_INFO_BLUE_SIZE]=8; buffer[VIDEO_MODE_INFO_BLUE_POSITION]=0;
                buffer[VIDEO_MODE_INFO_RESERVED_SIZE]=0; buffer[VIDEO_MODE_INFO_RESERVED_POSITION]=0; }
            /* DirectColorModeInfo: D0 colour ramp programmable (no), D1 "bits in the
               Rsvd field are usable by the application" -- yes for 5:5:5, whose spare
               bit nothing reads (the ET4000/W32p ROM says 02 there; p_vesa, s74b). */
            buffer[VIDEO_MODE_INFO_DIRECT_COLOUR_INFO] = (BYTE)(modeBitsPerPixel == VIDEO_BPP_15 ? VIDEO_DIRECT_RESERVED_USABLE : 0x00);
            /* ── ★★★ LINEAR FRAMEBUFFER. (s74) Attribute bit 7 says a mode HAS one and
                 PhysBasePtr says where; with them 0 the whole mode list reads as
                 "banked only". heaven7 enumerated all twelve modes we published,
                 including 320x200x16 and 640x480x16, and refused every one without
                 ever calling 4F02 -- measured twice, before and after the list grew.
                 A 2000-era demo will not paginate a 64KB window to raytrace. */
            /* ⚠ 0x9B ALREADY CARRIES D7 (LFB available) -- D0|D1|D3|D4|D7. An earlier
                 note here claimed bit 7 was 0 and OR'd 0x80 in; that was a no-op and
                 the claim was wrong. What was actually missing was PhysBasePtr, which
                 D7 is worthless without. VBE 2.0 §4.4 D7/D6 table: D7=1,D6=0 means
                 "both windowed and linear", which is exactly what we now provide. */
            VideoWrite32(buffer + VIDEO_MODE_INFO_PHYSICAL_BASE, VIDEO_VESA_LFB_PHYSICAL);            /* PhysBasePtr              */
            /* VBE 2.0 adds the linear-mode geometry at +50; a guest that drives the
               LFB reads these rather than the banked ones. Same numbers here because
               our pitch does not change between the two. */
            VideoWrite16(buffer + VIDEO_MODE_INFO_LINEAR_BYTES_PER_LINE, (WORD)pitch);              /* LinBytesPerScanLine      */
            buffer[VIDEO_MODE_INFO_BANKED_IMAGE_PAGES] = buffer[VIDEO_MODE_INFO_IMAGE_PAGES]; buffer[VIDEO_MODE_INFO_LINEAR_IMAGE_PAGES] = buffer[VIDEO_MODE_INFO_IMAGE_PAGES];               /* Lin/Bnk NumberOfImagePages = +29 */
            if (modeBitsPerPixel > VIDEO_BPP_INDEXED) {
                buffer[VIDEO_MODE_INFO_LINEAR_RED_SIZE]=buffer[VIDEO_MODE_INFO_RED_SIZE]; buffer[VIDEO_MODE_INFO_LINEAR_RED_POSITION]=buffer[VIDEO_MODE_INFO_RED_POSITION];
                buffer[VIDEO_MODE_INFO_LINEAR_GREEN_SIZE]=buffer[VIDEO_MODE_INFO_GREEN_SIZE]; buffer[VIDEO_MODE_INFO_LINEAR_GREEN_POSITION]=buffer[VIDEO_MODE_INFO_GREEN_POSITION];
                buffer[VIDEO_MODE_INFO_LINEAR_BLUE_SIZE]=buffer[VIDEO_MODE_INFO_BLUE_SIZE]; buffer[VIDEO_MODE_INFO_LINEAR_BLUE_POSITION]=buffer[VIDEO_MODE_INFO_BLUE_POSITION];
                buffer[VIDEO_MODE_INFO_LINEAR_RESERVED_SIZE]=buffer[VIDEO_MODE_INFO_RESERVED_SIZE]; buffer[VIDEO_MODE_INFO_LINEAR_RESERVED_POSITION]=buffer[VIDEO_MODE_INFO_RESERVED_POSITION]; }
            VddSetAx(registers, VIDEO_VBE_OK);
        } else VddSetAx(registers, VIDEO_VBE_FAILED);
        break; }
    case VIDEO_VBE_SET_MODE: {                                  /* set VBE mode                 */
        WORD width, height; BYTE modeBitsPerPixel = VIDEO_BPP_INDEXED;
        { BYTE textColumns, textRows, textCellHeight;
          if (VideoVesaFindText(VddGetBx(registers), &textColumns, &textRows, &textCellHeight)) {
              /* A VESA text mode = mode 3 with a different geometry. Go through the
                 standard mode set so everything a text mode resets is reset (font,
                 palette, CRTC, cursor, blink), honouring D15 as AL bit 7, then apply
                 the geometry. VesaTextMode is what 4F03 reports until a standard
                 mode set clears it (that path zeroes it below). */
              NTVDD_REGISTERS modeRegisters; UINT byteIndex;
              for (byteIndex = 0; byteIndex < sizeof modeRegisters; ++byteIndex) ((BYTE *)&modeRegisters)[byteIndex] = 0;
              /* §4.5: "If D14 is set, and a linear frame buffer model is not available
                 then the call will fail." A text mode has none (its ModeInfoBlock says
                 D7 = 0), so fail before anything changes (#226; we used to accept it). */
              if (VddGetBx(registers) & VIDEO_VBE_MODE_LFB) {
                  VideoVesaNote(state, VIDEO_VBE_SET_MODE, VddGetBx(registers), 0);
                  state->VesaSetBx = VddGetBx(registers); state->IsVesaSetSeen = 1; state->IsVesaSetOk = 0;
                  VddSetAx(registers, VIDEO_VBE_FAILED);
                  break;
              }
              VideoVesaNote(state, VIDEO_VBE_SET_MODE, VddGetBx(registers), 1);
              state->VesaSetBx = VddGetBx(registers); state->IsVesaSetSeen = 1; state->IsVesaSetOk = 1;
              VddSetAh(&modeRegisters, VIDEO_FUNCTION_SET_MODE); VddSetAl(&modeRegisters, (BYTE)(VIDEO_MODE_TEXT_80 | ((VddGetBx(registers) & VIDEO_VBE_MODE_NO_CLEAR) ? VIDEO_MODE_NO_CLEAR : 0x00)));
              VideoInt10(state, &modeRegisters);
              state->Columns = textColumns; state->Rows = textRows; state->CellHeight = textCellHeight;
              state->GraphicsWidth = (WORD)(textColumns * VIDEO_CELL_WIDTH); state->GraphicsHeight = (WORD)(textRows * textCellHeight);
              if (!(VddGetBx(registers) & VIDEO_VBE_MODE_NO_CLEAR)) VideoClearText(state, VIDEO_ATTRIBUTE_NORMAL);
              state->VesaTextMode = (WORD)(VddGetBx(registers) & VIDEO_VBE_MODE_NUMBER_MASK);
              state->VesaModeFlags = (WORD)(VddGetBx(registers) & VIDEO_VBE_MODE_NO_CLEAR);   /* 4F03 D15 (#226) */
              state->IsDirty = 1; VddSetAx(registers, VIDEO_VBE_OK);
              break;
          } }
        VideoVesaNote(state, VIDEO_VBE_SET_MODE, VddGetBx(registers), VideoVesaFind(VddGetBx(registers), &width, &height, &modeBitsPerPixel));
        state->VesaSetBx = VddGetBx(registers); state->IsVesaSetSeen = 1;
        state->IsVesaSetOk = (BYTE)(VideoVesaFind(VddGetBx(registers), &width, &height, &modeBitsPerPixel) ? 1 : 0);
        if (VideoVesaFind(VddGetBx(registers), &width, &height, &modeBitsPerPixel)) {
            UINT32 count;
            state->IsVesa = 1; state->VesaMode = VddGetBx(registers) & VIDEO_VBE_MODE_NUMBER_MASK; state->VesaWidth = width; state->VesaHeight = height;
            /* Bit 14 of the mode = "use the linear framebuffer". It matters beyond
               bookkeeping: an LFB guest never touches A0000, so VideoVesaSync must stop
               copying that window over the picture (see VideoVesaSync). */
            state->IsVesaLfb = (BYTE)((VddGetBx(registers) & VIDEO_VBE_MODE_LFB) ? 1 : 0);
            state->VesaBpp = modeBitsPerPixel;
            state->VesaStride = (UINT32)width * VideoVesaBytesPerPixel(modeBitsPerPixel);
            state->VesaStartX = state->VesaStartY = 0;   /* a mode set shows page 1 */
            state->VesaOrigin = state->VesaOriginVs = state->VesaOriginLive = 0;   /* ...on every stage */
            state->Vesa07Vbl = 0; state->Int10WaitUntil = 0;
            state->VesaDacWidth = VIDEO_DAC_WIDTH_6;                     /* §4.11: any mode set -> 6 bits */
            state->VesaBank = 0;
            /* ── ★★★ D15 = "DON'T CLEAR DISPLAY MEMORY". (VBE 2.0 §4.5) ──────────────
                 We cleared unconditionally. This is the SAME defect as the standard
                 BIOS one already on the books -- "AL bit 7 on a mode set = DO NOT
                 CLEAR VRAM" -- just on the VESA function instead, and it was found the
                 same way it should have been the first time: by reading the spec.
                 A guest that sets a mode to change geometry while keeping its picture
                 gets a black screen from us otherwise. */
            if (!(VddGetBx(registers) & VIDEO_VBE_MODE_NO_CLEAR)) {
                for (count = 0; count < VIDEO_VESA_VRAM; ++count) state->VesaVram[count] = 0;
                for (count = 0; count < VIDEO_VESA_WINDOW; ++count) state->VideoMemory[count] = 0;
            }
            /* ── 4F03h REPORTS D14/D15 AS THIS CALL SET THEM, AND 40:87h BIT 7 RECORDS
                 D15 (#226). §4.6: BX D14 = linear, D15 = "memory not cleared at last mode
                 set", and the Version 2.x note: "Unlike version 1.x VBE implementations,
                 the memory clear flag will be returned". §4.5: "VBE BIOS 2.0
                 implementations should also update the BIOS Data Area 40:87 memory clear
                 bit so that VBE Function 03h can return this flag." We stored the mode
                 `& 3FFFh`, so a guest that saves 4F03 and re-sets it with 4F02 came back
                 BANKED -- and VideoVesaSync then painted the stale A0000 window over the LFB
                 it was still drawing into, every frame. */
            state->VesaModeFlags = (WORD)(VddGetBx(registers) & (VIDEO_VBE_MODE_LFB | VIDEO_VBE_MODE_NO_CLEAR));
            state->IsModeSetNoClear = (BYTE)((VddGetBx(registers) & VIDEO_VBE_MODE_NO_CLEAR) ? 1 : 0);
            /* ── ★★ THE VGA LAYER UNDER A VESA MODE (#226). ─────────────────────────────
                 4F02h used to set the VESA fields and nothing else: `mkind`, the
                 sequencer/GC shadows, gw/gh, the text geometry and 40:49h all kept
                 the PREVIOUS standard mode's values. The hazard that left, read from the
                 code (not yet seen on a guest): after mode 12h, mkind stayed
                 VIDEO_KIND_PLANAR and chain4 0, so
                   * VddVideoIsPlanarActive() said 1 and the host (video_trap_sync)
                     kept running the guest in its INTERPRETER, whose A0000 stores go
                     through VddVideoPlanarWrite() into the four planes -- not into the
                     A0000 window VideoVesaSync copies into VesaVram. A banked VESA guest
                     would have drawn into planes nothing displays, at interpreter speed;
                   * the host's A0000 mapping stayed on a plane section (YMapSelect was
                     last told the planar map mask), so even native stores missed vmem;
                   * write mode / read mode / bit mask stayed mode 12h's.
                 And ModeAttributes told the guest the mode was VGA-compatible.
               ► WHAT A VBE BIOS DOES, so what we do: it programs the VGA for a CHAINED
                 256-colour pixel pipe and extends it -- measured in QEMU's SeaVGABIOS
                 binary (vgabios-stdvga.bin, qemu 10.2.1: SR4 |= 08h chain-4, GR5 = 40h
                 256-colour shift, AR index 20h video on) -- then fills the BDA from the
                 mode. So: mode 13h's register file (VideoLoadModeDefinition), kind LINEAR8 with
                 chain-4 on and the host window back on the linear section (the same arm
                 as the INT 10h mode-13h set), and the mode's geometry in gw/gh.
               ► THE BDA, FROM THE SAME BINARY'S vga_set_mode(): 40:49h = the mode if it
                 fits a byte, else FFh -- a VBE mode number does not, so FFh (Bochs's
                 older VGABIOS leaves 40:49h alone; SeaVGABIOS is the one we can read,
                 and "the last standard mode" is the lie that let a TSR believe mode 12h
                 was still set); 40:4Ah = XRes / 8; 40:84h = YRes / char height - 1;
                 40:85h = the char height (the YCharSize 4F01h reports); cursors and page
                 zeroed; 40:87h = 60h | (D15 ? 80h : 0). VddVideoBdaSync derives all
                 of those from the fields set here. */
            /* ── #325: AND THE 256-COLOUR DEFAULT PALETTE, as a mode 13h set loads it.
                 A 4F02 left the DAC as the previous mode had it, so after a 16-colour
                 mode (0Dh) colour 15 drew grey -- seen on the rig as a grey VESA
                 checkerboard. AH=12h BL=31h (palette loading off) is still honoured
                 inside VideoLoadDefaultPalette. */
            state->Mode   = VIDEO_MODE_VGA_256;
            VideoLoadDefaultPalette(state);
            state->Mode   = VIDEO_MODE_NOT_STANDARD;
            VideoLoadModeDefinition(state, VIDEO_MODE_VGA_256);               /* the chained 256-colour register file */
            state->ModeKind  = VIDEO_KIND_LINEAR8;
            state->MapMask = VIDEO_ALL_PLANES; state->YMask = VIDEO_ALL_PLANES;
            if (!state->IsChain4) { state->IsChain4 = 1; state->Chain4Selects++;
                               if (state->YMapSelect) state->YMapSelect(state->YMapContext, -1); }
            state->GraphicsWidth = width; state->GraphicsHeight = height;
            state->CellHeight = (BYTE)(height <= VIDEO_LINES_200 ? VGA_FONT8_HEIGHT : height <= VIDEO_LINES_350 ? VGA_FONT14_HEIGHT : VGA_FONT16_HEIGHT);
            state->Columns   = (BYTE)(width / VIDEO_GLYPH_WIDTH);
            state->Rows   = (BYTE)(height / state->CellHeight);
            state->CursorRow = state->CursorColumn = 0; state->Page = 0;
            state->VesaTextMode = 0;
            VideoPaletteRefresh(state);                           /* identity DAC path, see VideoPaletteRefresh */
            state->IsDirty = 1; VddSetAx(registers, VIDEO_VBE_OK);
        } else VddSetAx(registers, VIDEO_VBE_FAILED);
        break; }
    case VIDEO_VBE_STATE: {                                  /* save/restore state (§4.7)     */
        BYTE dl = (BYTE)(VddGetDx(registers) & BYTE_MASK);
        if (dl == VIDEO_STATE_GET_SIZE) { VddSetBx(registers, VIDEO_STATE_BLOCKS); VddSetAx(registers, VIDEO_VBE_OK); break; }
        if (dl == VIDEO_STATE_SAVE || dl == VIDEO_STATE_RESTORE) {
            BYTE *stateBuffer = (BYTE *)VddMapFlat(state->Bus, registers->Es, (WORD)registers->Ebx);
            if (dl == VIDEO_STATE_SAVE) { VideoStateSave(state, stateBuffer, VddGetCx(registers)); VddSetAx(registers, VIDEO_VBE_OK); }
            else VddSetAx(registers, VideoStateLoad(state, stateBuffer) ? VIDEO_VBE_OK : VIDEO_VBE_NOT_SUPPORTED);
            break;
        }
        VddSetAx(registers, VIDEO_VBE_FAILED);
        break; }
    case VIDEO_VBE_GET_MODE:                                    /* get the current VBE mode      */
        /* D0-D13 the mode, D14 linear, D15 not cleared -- as the last mode set left
           them (#226, §4.6; see 4F02). A standard mode reports its number and D15 from
           AL bit 7, which is what SeaVGABIOS's 4F03h does (it returns the word its
           vga_set_mode() stored for EVERY mode set, flags included); §4.6 itself only
           promises an accurate answer after a 4F02h. */
        if (state->IsVesa)             VddSetBx(registers, (WORD)(state->VesaMode | state->VesaModeFlags));
        else if (state->VesaTextMode) VddSetBx(registers, (WORD)(state->VesaTextMode | (state->VesaModeFlags & VIDEO_VBE_MODE_NO_CLEAR)));
        else                         VddSetBx(registers, (WORD)(state->Mode | (state->IsModeSetNoClear ? VIDEO_VBE_MODE_NO_CLEAR : 0u)));
        VddSetAx(registers, VIDEO_VBE_OK);
        break;
    /* ── 4F06 / 4F07: THE LOGICAL SCREEN, AND WHICH PART OF IT IS SHOWN. (s74b) ──
         VBE 2.0 §4.9/§4.10. Both of these used to be accepted and IGNORED: 4F06 kept a
         private `vesa_scanline` the presenter never read (and assumed bytes == pixels,
         true only at 8bpp), and 4F07 stored a start the presenter never applied. So a
         guest that draws page 2 and calls 4F07 to show it -- the standard VESA
         double-buffer -- got 004F back and saw page 1 forever. `VesaStride` is the
         single truth for bytes-per-line and `VideoVesaOrigin()` for the displayed start;
         the presenter reads both.
       ► Failure codes are the spec's: AH=02 for a length or start that does not fit,
         AH=03 outside a VESA mode ("invalid in current video mode"). "Fail and make no
         changes" -- §4.10 -- so nothing is written until the request has been checked. */
    case VIDEO_VBE_SCAN_LENGTH: {                                  /* get/set logical scan length   */
        BYTE  bl   = (BYTE)(VddGetBx(registers) & BYTE_MASK);
        UINT32 bytesPerPixel = VideoVesaBytesPerPixel(state->VesaBpp);
        UINT32 minBank = (UINT32)state->VesaWidth * bytesPerPixel;                 /* the mode's own pitch */
        UINT32 maxBank = state->VesaHeight ? VIDEO_VESA_VRAM / state->VesaHeight : 0; /* longest line that still holds h rows */
        maxBank -= maxBank % bytesPerPixel;                                         /* whole pixels          */
        if (!state->IsVesa || !minBank || !maxBank) { VddSetAx(registers, VIDEO_VBE_INVALID_IN_MODE); break; }
        if (bl == VIDEO_SCAN_SET_PIXELS || bl == VIDEO_SCAN_SET_BYTES) {
            UINT32 wanted = VddGetCx(registers);
            if (bl == VIDEO_SCAN_SET_PIXELS) wanted *= bytesPerPixel;                            /* pixels -> bytes       */
            wanted = (wanted + bytesPerPixel - 1) / bytesPerPixel * bytesPerPixel;                  /* "next larger value"   */
            if (wanted < minBank || wanted > maxBank) { VddSetAx(registers, VIDEO_VBE_NOT_SUPPORTED); break; }
            state->VesaStride = wanted;
            /* a start that no longer leaves a full page at the new pitch is reset,
               which is what a BIOS that re-latches its CRTC offset does in effect.
               The (x, y) start is kept and re-derived at the new pitch, at once (the
               behaviour this call has always had; #226 keeps it). */
            if (!VideoVesaOriginFits(state, VideoVesaXyOrigin(state, state->VesaStartX, state->VesaStartY)))
                state->VesaStartX = state->VesaStartY = 0;
            state->VesaOrigin = state->VesaOriginVs = state->VesaOriginLive =
                VideoVesaXyOrigin(state, state->VesaStartX, state->VesaStartY);
            state->IsDirty = 1;
        } else if (bl == VIDEO_SCAN_GET_MAXIMUM) {                  /* get maximum                   */
            VddSetBx(registers, (WORD)maxBank); VddSetCx(registers, (WORD)(maxBank / bytesPerPixel));
            VddSetDx(registers, (WORD)(VIDEO_VESA_VRAM / maxBank)); VddSetAx(registers, VIDEO_VBE_OK);
            break;
        } else if (bl != VIDEO_SCAN_GET) { VddSetAx(registers, VIDEO_VBE_FAILED); break; }
        VddSetBx(registers, (WORD)state->VesaStride);
        VddSetCx(registers, (WORD)(state->VesaStride / bytesPerPixel));
        VddSetDx(registers, (WORD)(VIDEO_VESA_VRAM / state->VesaStride));
        VddSetAx(registers, VIDEO_VBE_OK);
        break; }
    case VIDEO_VBE_DISPLAY_START: {                                  /* get/set display start         */
        BYTE bl = (BYTE)(VddGetBx(registers) & BYTE_MASK);
        if (!state->IsVesa) { VddSetAx(registers, VIDEO_VBE_INVALID_IN_MODE); break; }
        /* ── #226: THE START ON THE RETRACE'S SCHEDULE, AND 80h WAITS FOR IT. ────────
             BL=00h  set now: register, retrace load and display all take it at once --
                     the behaviour this call has always had, kept deliberately (a guest
                     that pans with 00h and draws straight after sees what it expects);
             BL=80h  set "during vertical retrace": the register takes it, the retrace
                     loads it (VideoLatch) and the call does not complete before that
                     retrace -- Int10WaitUntil, which the HOST honours outside its
                     lock (VddVideoInt10WaitUs). heaven7's 15,900 (0,0) calls a run
                     are this idiom and were paced by nothing;
             BL=02h  (3.0) schedule a start given as a BYTE address; return at once;
             BL=82h  (3.0) the same, and wait as 80h does;
             BL=04h  (3.0) has the scheduled flip happened? CX = 0 not yet, 1 done --
                     "done" meaning the retrace has loaded the register (vs == reg);
             BL=03h/83h/05h/06h  stereo: no such hardware (ModeAttributes D11/D12 = 0,
                     Capabilities D3 = 0), so 014Fh -- which 3.0's implementation note
                     prescribes for a card without it. */
        if (bl == VIDEO_START_GET) {                         /* get                           */
            VddSetCx(registers, state->VesaStartX); VddSetDx(registers, state->VesaStartY); VddSetBx(registers, 0);
            VddSetAx(registers, VIDEO_VBE_OK);
        } else if (bl == VIDEO_START_FLIP_STATUS) {                  /* 3.0: scheduled flip status    */
            VideoLatch(state, 0);                     /* bring the schedule up to now  */
            VddSetCx(registers, (WORD)(state->VesaOriginVs == state->VesaOrigin ? 1 : 0));
            VddSetAx(registers, VIDEO_VBE_OK);
        } else if (bl == VIDEO_START_SET || bl == (VIDEO_START_SET | VIDEO_START_WAIT_RETRACE) || bl == VIDEO_START_SCHEDULE || bl == (VIDEO_START_SCHEDULE | VIDEO_START_WAIT_RETRACE)) {
            UINT32 bytesPerPixel = VideoVesaBytesPerPixel(state->VesaBpp), origin, column, line;
            if (bl == VIDEO_START_SCHEDULE || bl == (VIDEO_START_SCHEDULE | VIDEO_START_WAIT_RETRACE)) {       /* ECX = byte address (3.0)      */
                origin = registers->Ecx;
                line = state->VesaStride ? origin / state->VesaStride : 0;
                column = state->VesaStride ? (origin % state->VesaStride) / bytesPerPixel : 0;
            } else {
                column = VddGetCx(registers); line = VddGetDx(registers);
                origin = VideoVesaXyOrigin(state, column, line);
            }
            if (column > state->Vesa07MaxX) state->Vesa07MaxX = (WORD)(column > WORD_MASK_U ? WORD_MASK_U : column);   /* inventory */
            if (line > state->Vesa07MaxY) state->Vesa07MaxY = (WORD)(line > WORD_MASK_U ? WORD_MASK_U : line);
            /* the whole displayed page must exist: "if the requested Display Start
               coordinates do not allow for a full page of video memory ... fail" */
            if (!VideoVesaOriginFits(state, origin)) { state->Vesa07Rejected++; VddSetAx(registers, VIDEO_VBE_NOT_SUPPORTED); break; }
            VideoLatch(state, 0);                     /* boundaries already passed keep the old start */
            state->VesaStartX = (WORD)column; state->VesaStartY = (WORD)line;
            state->VesaOrigin = origin;
            if (bl == VIDEO_START_SET) {
                state->VesaOriginVs = state->VesaOriginLive = origin;          /* at once, as always */
            } else if (bl & VIDEO_START_WAIT_RETRACE) {
                INT isInVbl = 0;
                UINT64 until = VideoVesaVblRelease(state, &isInVbl);
                if (isInVbl) state->VesaOriginVs = origin;  /* this retrace loads it: next picture */
                state->Int10WaitUntil = until;
                if (until && state->TimeUs) {
                    UINT64 now = state->TimeUs();
                    state->Vesa07Waits++;
                    if (until > now) state->Vesa07WaitUs += until - now;
                }
            }                                     /* 02h: the latch takes it at the retrace */
            state->IsDirty = 1;
            VddSetAx(registers, VIDEO_VBE_OK);
        } else VddSetAx(registers, VIDEO_VBE_FAILED);                   /* 03h/83h/05h/06h stereo, unknown BL */
        break; }
    case VIDEO_VBE_DAC_WIDTH: {                                  /* get/set DAC palette width     */
        /* VBE 2.0 §4.11: BL=00 set (BH = wanted bits), BL=01 get; BH out = current.
           "If the hardware cannot select the requested width, the NEXT LOWER value it
           can is selected" -- we have 6 and 8, so 10 -> 8 and 7 -> 6 (this used to send
           anything but exactly 8 to 6). AH=03 in a direct-colour mode; the width is
           reset to 6 by any mode set (done in 4F02 and the standard AH=00).
         ► THE WIDTH NOW REACHES THE PORTS (#226) -- 3C9h and the INT 10h AH=10h DAC
           calls convert through VideoDacTo8/VideoDacFrom8 -- so it is a real switch, not a
           4F09-only flag.
         ► AND IT IS NOT CONFINED TO VESA MODES. §4.11 refuses the call in "a direct
           color or YUV mode" and nowhere else; the DAC is one device, and mode 13h (or
           a text mode, whose colours are DAC entries through the attribute controller)
           drives the same RAMDAC. We answered 034Fh outside a VESA mode, which a
           mode-13h game that wants 8-bit primaries reads as "not in this mode". */
        BYTE bl8 = (BYTE)(VddGetBx(registers) & BYTE_MASK);
        if (state->IsVesa && state->VesaBpp > VIDEO_BPP_INDEXED) { VddSetAx(registers, VIDEO_VBE_INVALID_IN_MODE); break; }
        if (bl8 == VIDEO_DAC_SET) {
            BYTE dacWidth = (BYTE)((VddGetBx(registers) >> BYTE_SHIFT) & BYTE_MASK);
            state->VesaDacWidth = (BYTE)(dacWidth >= VIDEO_DAC_WIDTH_8 ? VIDEO_DAC_WIDTH_8 : VIDEO_DAC_WIDTH_6);
        } else if (bl8 != VIDEO_DAC_GET) { VddSetAx(registers, VIDEO_VBE_FAILED); break; }
        if (!state->VesaDacWidth) state->VesaDacWidth = VIDEO_DAC_WIDTH_6;
        VddSetBx(registers, (WORD)((VddGetBx(registers) & BYTE_MASK) | ((WORD)state->VesaDacWidth << BYTE_SHIFT)));
        VddSetAx(registers, VIDEO_VBE_OK);
        break; }
    case VIDEO_VBE_PALETTE: {                                  /* get/set palette data          */
        /* VBE 2.0 §4.12: BL=00 set, 01 get, 02/03 secondary palette (none here ->
           AH=02), 80h set during retrace; DX=first, CX=count, ES:DI = quads laid out
           B,G,R,align in memory. The DAC width set by 4F08 decides how far to shift.
           ⚠ (s74b) A set never called VideoPaletteRefresh(), so the presenter kept showing the
             OLD palette until some other DAC path happened to refresh it. */
        BYTE bl9 = (BYTE)(VddGetBx(registers) & BYTE_MASK);
        WORD first = VddGetDx(registers), count = VddGetCx(registers), index;
        BYTE *table = (BYTE *)VddMapFlat(state->Bus, registers->Es, (WORD)registers->Edi);
        if (bl9 == VIDEO_PALETTE_SET_SECONDARY || bl9 == VIDEO_PALETTE_GET_SECONDARY) { VddSetAx(registers, VIDEO_VBE_NOT_SUPPORTED); break; }
        if (bl9 != VIDEO_PALETTE_SET && bl9 != VIDEO_PALETTE_GET && bl9 != VIDEO_PALETTE_SET_IN_RETRACE) { VddSetAx(registers, VIDEO_VBE_FAILED); break; }
        if ((UINT32)first + count > NTVDD_PALETTE_ENTRIES) { VddSetAx(registers, VIDEO_VBE_NOT_SUPPORTED); break; }   /* fail, change nothing */
        /* The same conversion the ports use (VideoDacTo8/VideoDacFrom8). ⚠ (#226) The 6-bit set
           used to shift WITHOUT masking, so bits 6-7 of green and blue spilled into the
           low bits of red and green (`t << 2` of a byte, OR'd into its neighbour's
           field). §4.12: "When in 6 bit mode, the format of the 6 bits is LSB" -- the
           top two bits are not part of the value, as on the port. */
        for (index = 0; index < count && (first + index) < NTVDD_PALETTE_ENTRIES; ++index) {
            if (bl9 == VIDEO_PALETTE_SET || bl9 == VIDEO_PALETTE_SET_IN_RETRACE)
                state->Dac[first + index] = VideoDacPackWidth(state, table[index*VIDEO_PALETTE_ENTRY_BYTES+VIDEO_PALETTE_RED], table[index*VIDEO_PALETTE_ENTRY_BYTES+VIDEO_PALETTE_GREEN], table[index*VIDEO_PALETTE_ENTRY_BYTES+VIDEO_PALETTE_BLUE]);
            else {
                UINT32 value = state->Dac[first + index];
                table[index*VIDEO_PALETTE_ENTRY_BYTES+VIDEO_PALETTE_BLUE] = VideoDacFrom8(state, (BYTE)value);
                table[index*VIDEO_PALETTE_ENTRY_BYTES+VIDEO_PALETTE_GREEN] = VideoDacFrom8(state, (BYTE)(value >> VIDEO_GREEN_SHIFT));
                table[index*VIDEO_PALETTE_ENTRY_BYTES+VIDEO_PALETTE_RED] = VideoDacFrom8(state, (BYTE)(value >> VIDEO_RED_SHIFT));
                table[index*VIDEO_PALETTE_ENTRY_BYTES+VIDEO_PALETTE_ALIGN] = 0;
            }
        }
        if (bl9 != VIDEO_PALETTE_GET) VideoPaletteRefresh(state);         /* the presenter reads st->Palette */
        state->IsDirty = 1;
        VddSetAx(registers, VIDEO_VBE_OK);
        break; }
    case VIDEO_VBE_PM_INTERFACE:                                    /* protected-mode interface      */
        /* ── #53: BL=00h -> ES:DI = the block, CX = its length (VBE 2.0 §4.13). See
             VideoVbePmInstall. It is rewritten on every call, so the copy a client takes
             is always the real one. Any other BL: 014Fh (the function exists; that
             subfunction does not).
           ⚠ (s74b) The answer was AX=0100h, which BOTH measured real BIOSes give (the
             Tseng ET4000/W32p ROM and Bochs's -- p_vesapm), and which was right while
             there was nothing to hand out. Neither is an oracle for the block now:
             the spec is (VBE 2.0 §4.13), and video_test.c runs the code it returns. */
        if ((VddGetBx(registers) & BYTE_MASK) != VIDEO_PM_INTERFACE_GET) { VddSetAx(registers, VIDEO_VBE_FAILED); break; }
        VideoVbePmInstall(state);
        registers->Es = VDD_VBEPM_SEG;
        registers->Edi = (registers->Edi & HIGH_WORD_MASK_U);
        VddSetCx(registers, (WORD)VBE_PM_LEN);
        VddSetAx(registers, VIDEO_VBE_OK);
        break;
    case VIDEO_VBE_POWER: {                                  /* VBE/PM: display power (DPMS)  */
        /* VBE/PM 1.0. BL=00 report: BL=version 10h (BCD), BH=states supported
           (bit0 standby, bit1 suspend, bit2 off, bit3 reduced-on); BL=01 set state
           BH; BL=02 get state -> BH. ⚠ From the published interface (what Bochs and
           DOSBox answer), not from a VESA PDF in docs/ref -- there is no oracle for
           it. We claim all four states and remember the one set; nothing blanks,
           which is what a monitor with no power management would show too. */
        BYTE bl = (BYTE)(VddGetBx(registers) & BYTE_MASK), bh = (BYTE)((VddGetBx(registers) >> BYTE_SHIFT) & BYTE_MASK);
        if (bl == VIDEO_POWER_REPORT)      VddSetBx(registers, (WORD)((VIDEO_POWER_STATES << BYTE_SHIFT) | VIDEO_POWER_VERSION));
        else if (bl == VIDEO_POWER_SET) { if (bh & ~VIDEO_POWER_STATES) { VddSetAx(registers, VIDEO_VBE_NOT_SUPPORTED); break; } state->VesaPmState = bh; }
        else if (bl == VIDEO_POWER_GET) VddSetBx(registers, (WORD)(((WORD)state->VesaPmState << BYTE_SHIFT) | bl));
        else { VddSetAx(registers, VIDEO_VBE_FAILED); break; }
        VddSetAx(registers, VIDEO_VBE_OK);
        break; }
    case VIDEO_VBE_DDC: {                                  /* VBE/DDC: display identification */
        /* BL=00 report capabilities: BH = seconds per EDID block, BL = bit0 DDC1,
           bit1 DDC2, bit2 "screen blanked during transfer". BL=01 read EDID block
           DX into ES:DI (128 bytes). ⚠ From the published VBE/DDC interface (what
           Bochs/DOSBox answer), not a PDF in docs/ref; no oracle. The block is a
           SYNTHESISED EDID 1.3 for a generic analogue monitor that does every mode
           we publish -- "no monitor" was the previous answer, and a guest that asks
           deserves a plausible one rather than a refusal. One block, no extensions. */
        BYTE bl = (BYTE)(VddGetBx(registers) & BYTE_MASK);
        if (bl == VIDEO_DDC_REPORT) { VddSetBx(registers, VIDEO_DDC_CAPABILITIES); VddSetAx(registers, VIDEO_VBE_OK); break; }   /* 1 s, DDC1+DDC2 */
        if (bl == VIDEO_DDC_READ_EDID) {
            BYTE *extra; UINT byteIndex, sum = 0;
            if (VddGetDx(registers) != 0) { VddSetAx(registers, VIDEO_VBE_FAILED); break; }              /* block 0 only   */
            extra = (BYTE *)VddMapFlat(state->Bus, registers->Es, (WORD)registers->Edi);
            for (byteIndex = 0; byteIndex < VIDEO_EDID_BYTES; ++byteIndex) extra[byteIndex] = 0;
            extra[0] = 0x00; for (byteIndex = 1; byteIndex < 7; ++byteIndex) extra[byteIndex] = 0xFF; extra[7] = 0x00;   /* header   */
            extra[8] = 0x3A; extra[9] = 0x96;                 /* manufacturer "NTV" (5-bit packed) */
            extra[10] = 0x01; extra[11] = 0x00;               /* product code 1                */
            extra[16] = 1; extra[17] = 10;                    /* week 1, 2000                  */
            extra[18] = 1; extra[19] = 3;                     /* EDID 1.3                      */
            extra[20] = 0x0E;                             /* analogue, 0.7/0.3 V, sync on H/V + composite */
            extra[21] = 34; extra[22] = 27;                   /* 34 x 27 cm (17")               */
            extra[23] = 120;                              /* gamma 2.2                     */
            extra[24] = 0xEE;                             /* DPMS standby/suspend/off, RGB, preferred timing */
            /* chromaticity: sRGB primaries */
            extra[25] = 0xEE; extra[26] = 0x91; extra[27] = 0xA3; extra[28] = 0x54; extra[29] = 0x4C;
            extra[30] = 0x99; extra[31] = 0x26; extra[32] = 0x0F; extra[33] = 0x50; extra[34] = 0x54;
            extra[35] = 0x2D; extra[36] = 0xEF; extra[37] = 0x00; /* established: 720x400@70 640x480@60/72/75 800x600@56/60/72/75 1024x768@60/70/75 */
            extra[38] = 0x81; extra[39] = 0x80;               /* standard timing 1280x1024@60  */
            extra[40] = 0x81; extra[41] = 0x40;               /* 1280x960@60                   */
            for (byteIndex = 42; byteIndex < 54; ++byteIndex) extra[byteIndex] = 0x01;    /* unused standard timings       */
            /* detailed timing 1: 1280x1024@60 (108 MHz) */
            extra[54] = 0x30; extra[55] = 0x2A; extra[56] = 0x00; extra[57] = 0x98; extra[58] = 0x51; extra[59] = 0x00;
            extra[60] = 0x2A; extra[61] = 0x40; extra[62] = 0x30; extra[63] = 0x70; extra[64] = 0x13; extra[65] = 0x00;
            extra[66] = 0x54; extra[67] = 0x0E; extra[68] = 0x11; extra[69] = 0x00; extra[70] = 0x00; extra[71] = 0x1E;
            /* descriptor 2: range limits 50-75 Hz, 30-80 kHz, 110 MHz */
            extra[72] = 0; extra[73] = 0; extra[74] = 0; extra[75] = 0xFD; extra[76] = 0;
            extra[77] = 50; extra[78] = 75; extra[79] = 30; extra[80] = 80; extra[81] = 11; extra[82] = 0x00; extra[83] = 0x0A;
            for (byteIndex = 84; byteIndex < 90; ++byteIndex) extra[byteIndex] = 0x20;
            /* descriptor 3: monitor name */
            extra[90] = 0; extra[91] = 0; extra[92] = 0; extra[93] = 0xFC; extra[94] = 0;
            { const char *productName = "NTVDMEX VESA\n"; for (byteIndex = 0; byteIndex < 13; ++byteIndex) extra[95 + byteIndex] = (BYTE)(productName[byteIndex] ? productName[byteIndex] : ' '); }
            /* descriptor 4: serial */
            extra[108] = 0; extra[109] = 0; extra[110] = 0; extra[111] = 0xFF; extra[112] = 0;
            { const char *serialString = "0000001\n"; for (byteIndex = 0; byteIndex < 13; ++byteIndex) extra[113 + byteIndex] = (BYTE)(serialString[byteIndex] ? serialString[byteIndex] : ' '); }
            extra[126] = 0;                               /* no extension blocks           */
            for (byteIndex = 0; byteIndex < VIDEO_EDID_CHECKSUM; ++byteIndex) sum += extra[byteIndex];
            extra[VIDEO_EDID_CHECKSUM] = (BYTE)(VIDEO_EDID_CHECKSUM_MODULUS - (sum & VIDEO_EDID_CHECKSUM_MASK));
            VddSetAx(registers, VIDEO_VBE_OK); break;
        }
        VddSetAx(registers, VIDEO_VBE_FAILED);
        break; }
    case VIDEO_VBE_WINDOW: {                                  /* window control (bank switch) */
        /* VBE 2.0 §4.8: BH = 00h set / 01h get, BL = WINDOW (00h A, 01h B), DX = window
           position in granularity units (64 KB here, as 4F01 says).
           ⚠ (s74b) This read BL as the set/get selector. Window A's number is 0, so a
             SET of window A (BH=00,BL=00) happened to work -- and a GET of window A
             (BH=01,BL=00) was treated as a set to whatever DX held. A guest that reads
             the bank back to restore it later flipped its own window to junk. Caught
             by the spec audit, not by a guest; there is no oracle for VESA yet.
           Window B is advertised as absent (WinBAttributes=0) so BL=01 fails; in an LFB
           mode the spec requires AH=03. */
        BYTE bh = (BYTE)((VddGetBx(registers) >> BYTE_SHIFT) & BYTE_MASK), bl = (BYTE)(VddGetBx(registers) & BYTE_MASK);
        if (!state->IsVesa || state->IsVesaLfb) { VddSetAx(registers, VIDEO_VBE_INVALID_IN_MODE); break; }
        if (bl != VIDEO_WINDOW_A) { VddSetAx(registers, VIDEO_VBE_FAILED); break; }
        if (bh == VIDEO_WINDOW_SET) {                         /* set window A                  */
            if (!VideoVesaSetBank(state, VddGetDx(registers))) { VddSetAx(registers, VIDEO_VBE_NOT_SUPPORTED); break; }   /* past VRAM */
        } else if (bh == VIDEO_WINDOW_GET) {                  /* get window A                  */
            VddSetDx(registers, state->VesaBank);
        } else { VddSetAx(registers, VIDEO_VBE_FAILED); break; }
        VddSetAx(registers, VIDEO_VBE_OK);
        break; }
    default: VddSetAx(registers, VIDEO_VBE_NO_SUCH_FUNCTION); break;              /* no such sub-function: AL != 4Fh (measured on two BIOSes) */
    }
}

/* (glyph_12h -- the 8x16-at-640-stride planar glyph, with a background taken from
   BL bits 4-7 -- was replaced by VideoGraphicsGlyph for #252. QuickBASIC's SCREEN 12 text,
   the case it was written for, is the 8x16 row of VideoGraphicsGlyph's planar arm.) */

/* INT 10h text + mode + palette services. */
static VOID VideoInt10(PVOID context, PNTVDD_REGISTERS registers)
{
    PVIDEO_STATE state = (PVIDEO_STATE)context;
    BYTE ah = VddGetAh(registers), al = VddGetAl(registers);
    state->IsDirty = 1;
    state->Int10WaitUntil = 0;          /* every call completes at once unless it says otherwise (#226) */
    switch (ah) {
    case VIDEO_FUNCTION_SET_MODE:                                        /* set video mode          */
        /* ── ▶ AL BIT 7 = "DO NOT CLEAR VIDEO MEMORY", AND IT IS NOT DECORATION. ────
             Standard since the EGA: a mode set with bit 7 reprograms the card but
             LEAVES DISPLAY MEMORY ALONE. We masked the bit off to get the mode number
             and then threw it away, so every mode set wiped all four 64KB planes.
           ⚠ THAT IS THE MISSING LEMMINGS TOOLBAR. The game composes its skill-button
             panel into OFF-SCREEN VRAM at 0xF91F..0xFFFE, then sets its mode with
             `mov ax,0x008D` (0x0FB3) -- and the menu with `mov ax,0x0090` (0x4BEF) --
             exactly so that cache survives. We cleared it, and the panel blit at
             0x7626 then copied 1760 bytes of zeroes onto the screen, faithfully.
             MEASURED: the blit writes with wm=1 and lat=00000000 while the cache's
             own last write left 0x00/0x24/0x7F/0x00 there -- the copy was never wrong,
             its source had been destroyed underneath it.
           ▶ Everything else a mode set does still happens; only the erase is skipped,
             which is the whole difference the bit describes. */
        {   INT noclear = (al & VIDEO_MODE_NO_CLEAR) != 0;
        state->Mode = al & VIDEO_MODE_NUMBER_MASK; state->IsVesa = 0;        /* a standard mode leaves VESA */
        state->VesaTextMode = 0;                       /* ...including a VESA text mode */
        /* 40:87h bit 7 is "the last mode set did not clear memory" -- IBM's EGA/VGA
           BIOS copies AL bit 7 there, and VBE 2.0 §4.5/§4.6 builds 4F02h's D15 on the
           same bit (#226). It read 60h always. */
        state->IsModeSetNoClear = (BYTE)(noclear ? 1 : 0);
        state->VesaModeFlags = 0;
        state->VesaDacWidth = VIDEO_DAC_WIDTH_6;                        /* §4.11: any mode set -> 6 bits */
        state->CursorRow = state->CursorColumn = 0; state->Page = 0;
        {   INT page; for (page = 0; page < VIDEO_PAGES; ++page) state->PageRow[page] = state->PageColumn[page] = 0; }   /* #252 */
        /* These three are the FALLBACK for a mode g_VgaModeDefinitions does not cover:
           VideoLoadModeDefinition below overrides all of them from the measured table for
           every mode it knows, which is where 0x0D0E rather than this 8-line
           0x0607 comes from. A mode nobody measured still gets a sane cursor and
           blink enabled rather than zeros. */
        state->CursorShape = VIDEO_CURSOR_SHAPE_DEFAULT;                       /* the BIOS resets the shape too */
        /* #188: 0040:0065/0066, the CGA mode-select and palette registers, are the
           standard CGA table for modes 00h-07h and are LEFT ALONE by the EGA/VGA
           modes -- measured (p_video2) on PCem's genuine IBM VGA ROM and DOSBox-X:
           0Dh-11h read back 05h's 2E after it, 12h/13h read 06h's 1E/3F. 0066 is 30h,
           3Fh in mode 6. We wrote 29h/30h once at start-up and never again. */
        if (state->Mode <= VIDEO_MODE_MDA) {
            static const BYTE cgaModeSelect[VIDEO_MODE_MDA + 1] = { 0x2C, 0x28, 0x2D, 0x29, 0x2A, 0x2E, 0x1E, 0x29 };
            state->CgaSelect = (BYTE)(state->Mode == VIDEO_MODE_CGA_640 ? VIDEO_CGA_SELECT_MODE_6 : VIDEO_CGA_SELECT_DEFAULT);   /* AH=0Bh's shadow (#266) */
            if (state->BiosData) { state->BiosData[BIOS_BDA_CGA_MODE_SELECT] = cgaModeSelect[state->Mode]; state->BiosData[BIOS_BDA_CGA_PALETTE] = state->CgaSelect; }
        }
        state->IsBlink = 1;                                /* ...and re-enables blink (AR10 bit 3) */
        state->AttributeMode = (BYTE)(state->AttributeMode | VIDEO_AR_MODE_BLINK);   /* the register agrees    */
        state->IsUserFontOn = 0;                         /* the ROM font comes back with the mode */
        /* The cell height the mode's BIOS font gives: 8x16 in the VGA text modes and
           the 480-line graphics modes, 8x14 at 350 lines, 8x8 at 200. */
        state->CellHeight = VideoModeCellHeight(state->Mode);
        VideoLoadDefaultPalette(state);                    /* HW reloads the DAC on mode set */
        state->IsGeometryRegistersOk = 0;                         /* #325: until a measured set loads */
        VideoLoadModeDefinition(state, state->Mode);               /* ...and programs the register file */
        VideoLoadDefaultCrtc(state);                        /* ...and reprograms the CRTC     */
        {   UINT modeIndex; PCVOID found = 0;
            for (modeIndex = 0; modeIndex < sizeof(g_VideoModes)/sizeof(g_VideoModes[0]); ++modeIndex)
                if (g_VideoModes[modeIndex].Mode == state->Mode) { found = &g_VideoModes[modeIndex]; break; }
            if (!found) {                             /* a mode nobody defines   */
                VIDEO_UNIMPLEMENTED_SET(state->UnimplementedModes, state->Mode);
                state->ModeKind = VIDEO_KIND_TEXT;
                state->Columns = VIDEO_COLUMNS; state->Rows = VIDEO_ROWS;
                state->GraphicsWidth = VIDEO_FRAME_WIDTH; state->GraphicsHeight = VIDEO_FRAME_HEIGHT;
                if (!noclear) VideoClearText(state, VIDEO_ATTRIBUTE_NORMAL);
            } else {
                state->ModeKind = g_VideoModes[modeIndex].Kind;
                state->Columns  = g_VideoModes[modeIndex].Columns;
                state->Rows  = g_VideoModes[modeIndex].Rows;
                state->GraphicsWidth    = g_VideoModes[modeIndex].Width;
                state->GraphicsHeight    = g_VideoModes[modeIndex].Height;
                if (state->ModeKind == VIDEO_KIND_UNSUPPORTED) {
                    /* Say so; do not paint a text screen and let the program
                       draw into a layout that is not there. */
                    VIDEO_UNIMPLEMENTED_SET(state->UnimplementedModes, state->Mode);
                    state->ModeKind = VIDEO_KIND_TEXT;
                    state->Columns = VIDEO_COLUMNS; state->Rows = VIDEO_ROWS;
                    state->GraphicsWidth = VIDEO_FRAME_WIDTH;  state->GraphicsHeight = VIDEO_FRAME_HEIGHT;
                    if (!noclear) VideoClearText(state, VIDEO_ATTRIBUTE_NORMAL);
                } else if (state->ModeKind == VIDEO_KIND_LINEAR8) {
                    INT index;
                    /* the BIOS sets SR4 = 0Eh for mode 13h: chain-4 ON (see the planar arm) */
                    state->MapMask = VIDEO_ALL_PLANES; state->YMask = VIDEO_ALL_PLANES;
                    if (!state->IsChain4) { state->IsChain4 = 1; state->Chain4Selects++;
                                       if (state->YMapSelect) state->YMapSelect(state->YMapContext, -1); }
                    if (!noclear) for (index = 0; index < VIDEO_MODE13_WIDTH * VIDEO_MODE13_HEIGHT; ++index) state->VideoMemory[index] = 0;
                } else if (state->ModeKind == VIDEO_KIND_PLANAR) {
                    INT plane; UINT32 index;
                    /* ── ★★★ THE BIOS PROGRAMS THE SEQUENCER TOO. (s74b) A mode set writes
                         SR4 = 06h for the planar modes (chain-4 OFF, odd/even off) and SR2 =
                         0Fh; we modelled neither, so chain4 kept whatever the guest last
                         wrote -- 1 from reset -- and the map-mask handler took its
                         `MaskSkipChain4` branch for EVERY mask write in mode 12h. The
                         host window never left the linear section, and a guest that fills
                         the four planes with mask 1/2/4/8 (Hexen's and Heretic's 640x480
                         loaders, from protected mode where no interpreter routes the store)
                         wrote all four on top of each other into memory nothing renders.
                         `planar hi_water=0`, black screen. Doom escaped because it programs
                         SR4 itself for mode Y. Real-mode guests escaped because their pixel
                         loops run in the interpreter, which reaches the engine regardless. */
                    state->MapMask = VIDEO_ALL_PLANES; state->YMask = VIDEO_ALL_PLANES;
                    if (state->IsChain4) { state->IsChain4 = 0; state->Chain4Selects++; }
                    if (state->YMapSelect) state->YMapSelect(state->YMapContext, (INT)state->MapMask);
                    if (!noclear)
                        for (plane = 0; plane < VIDEO_PLANES; ++plane)
                            for (index = 0; index < VIDEO_PLANE_SIZE; ++index) VideoPlaneBytes(state,plane)[index] = 0;
                } else if (state->ModeKind == VIDEO_KIND_CGA) {
                    INT index;
                    state->CgaBpp = (BYTE)(state->Mode == VIDEO_MODE_CGA_640 ? 1 : VIDEO_CGA_BITS_PER_PIXEL);
                    state->CgaPalette = 0;
                    if (!noclear) for (index = 0; index < VIDEO_CGA_BUFFER_BYTES; ++index) state->VideoMemory[VIDEO_TEXT_OFFSET + index] = 0;
                } else {
                    if (!noclear) VideoClearText(state, VIDEO_ATTRIBUTE_NORMAL);
                }
            }
            /* ── ★ THE COLOURS ARE REBUILT AFTER THE MODE'S KIND IS KNOWN. (s81, user:
                 "white shows faint blue" after Doom; ENDOOM's bright words pale blue.)
                 VideoLoadDefaultPalette() above runs VideoPaletteRefresh() while mkind is still
                 the OLD mode's -- so leaving mode 13h built the text colours with the
                 8bpp rule (colour n = DAC n, no attribute palette): bright white 0Fh
                 drew as DAC 0Fh = AAAAFF, yellow as AAAA55, light cyan as 00AAFF. The
                 machine state was right (a report after the exit read vpal[15]=3F,
                 DAC 3F=FFFFFF); only the renderer's table was stale, and grey 07h looks
                 the same either way, which is why a shell prompt hid it. And the BIOS
                 resets the DAC pixel mask on a mode set as well. */
            /* ── AH=12h BL=30h CHOSE THE SCAN LINES FOR THIS (#252). The colour text modes
                 come up in 200, 350 or 400 lines with the font that fills 25 rows of
                 them -- 8x8, 8x14, 8x16. It answered "supported" and every text mode
                 came up at 400 regardless. */
            if (state->ModeKind == VIDEO_KIND_TEXT && state->Mode <= VIDEO_MODE_TEXT_80 && state->ScanSelect < VIDEO_SCAN_SELECT_400) {
                state->CellHeight = (BYTE)(state->ScanSelect == VIDEO_SCAN_SELECT_200 ? VGA_FONT8_HEIGHT : VGA_FONT14_HEIGHT);
                state->GraphicsHeight = (WORD)(state->Rows * state->CellHeight);
            }
            /* ── #266: INT 43h FOLLOWS THE MODE. A VGA BIOS points the graphics-font
                 vector at the table of the new mode's cell (SeaVGABIOS's vga_set_mode:
                 8 / 14 / 16 from the mode's character height), so a program that reads
                 INT 43h after a mode set finds the font the BIOS will draw with. Nothing
                 wrote it before. ⚠ Whether IBM's ROM also does this in the TEXT modes is
                 unmeasured (p_vid266 `i43.mode03` asks). INT 1Fh is a power-on vector and
                 is left alone (VideoInt1FRom, at install). A caller font (AL=21h) ends here. */
            VideoInt43Rom(state, state->CellHeight);
            state->DacMask = VIDEO_DAC_MASK_ALL;
            VideoPaletteRefresh(state);
            /* ── AH=00h RETURNS A "VIDEO MODE FLAG" IN AL, NOT THE MODE. (s74b) Measured
                 on the AMI 486 ROM under PCem and on SeaBIOS alike (p_plan12: AX=0020
                 after mode 12h); RBIL documents it for Phoenix/AMI: 20h for modes > 7,
                 30h for modes 0-5 and 7, 3Fh for mode 6. We returned AL = the mode,
                 which is what a caller that saved AX would see as "mode 12h set" --
                 harmless for most, wrong for anything that keys on the flag. */
            VddSetAl(registers, (BYTE)(state->Mode > VIDEO_MODE_MDA ? VIDEO_SET_MODE_FLAG_EGA : state->Mode == VIDEO_MODE_CGA_640 ? VIDEO_SET_MODE_FLAG_MODE_6 : VIDEO_SET_MODE_FLAG_CGA));
            if (state->ModeQueryCount < VIDEO_MODE_QUERIES) {
                state->ModeQueries[state->ModeQueryCount].Mode = state->Mode;
                state->ModeQueries[state->ModeQueryCount].Kind = state->ModeKind;
                state->ModeQueries[state->ModeQueryCount].Columns = state->Columns;
                state->ModeQueries[state->ModeQueryCount].Rows = state->Rows;
                state->ModeQueries[state->ModeQueryCount].Width    = state->GraphicsWidth;
                state->ModeQueries[state->ModeQueryCount].Height    = state->GraphicsHeight;
                state->ModeQueryCount++;
            }
        } }
        break;
    case VIDEO_FUNCTION_SET_CURSOR_SHAPE: state->CursorShape = VddGetCx(registers); break;
    /* ── 02h/03h: THE CURSOR OF PAGE BH (#252). Eight cursors, one per page -- the
         active page's is CursorRow/CursorColumn, the rest PageRow/PageColumn (0040:0050). BH was
         ignored, so positioning page 1's cursor moved the one on screen. */
    case VIDEO_FUNCTION_SET_CURSOR: {
        BYTE page = (BYTE)((VddGetBx(registers) >> BYTE_SHIFT) & VIDEO_PAGE_MASK);
        if (page == (state->Page & VIDEO_PAGE_MASK)) { state->CursorRow = (BYTE)(VddGetDx(registers) >> BYTE_SHIFT); state->CursorColumn = (BYTE)(VddGetDx(registers) & BYTE_MASK); }
        else { state->PageRow[page] = (BYTE)(VddGetDx(registers) >> BYTE_SHIFT); state->PageColumn[page] = (BYTE)(VddGetDx(registers) & BYTE_MASK); }
        break; }
    case VIDEO_FUNCTION_GET_CURSOR: {
        /* THERE IS NO TEXT CURSOR IN A GRAPHICS MODE, and the BIOS says so: CX comes
           back 0000 in 06h/12h/13h, where we were still handing out the text
           underline shape 0607. Measured, p_video.asm VideoInt10.03.<mode>. The stored
           shape is left alone so returning to a text mode restores it. */
        BYTE page = (BYTE)((VddGetBx(registers) >> BYTE_SHIFT) & VIDEO_PAGE_MASK);
        INT isActive = (page == (state->Page & VIDEO_PAGE_MASK));
        VddSetDx(registers, (WORD)(((isActive ? state->CursorRow : state->PageRow[page]) << BYTE_SHIFT) | (isActive ? state->CursorColumn : state->PageColumn[page])));
        VddSetCx(registers, (WORD)(state->ModeKind == VIDEO_KIND_TEXT ? state->CursorShape : 0));
        break; }
    /* ── 05h: SELECT THE ACTIVE PAGE, AND SHOW IT (#252). It stored the number and the
         BDA followed (oracle-verified, #188) -- but the CRTC start address never moved,
         so the screen stayed on page 0. The BIOS loads CR0C/CR0D with the page's offset:
         in WORDS in the text modes (page 1 of 80x25 = 0800h), in BYTES in the planar
         modes (0Dh page 1 = 2000h) -- p_vidtxt t03/t0D.05.crtc on PCem's IBM ROM. The
         renderers read the latched start (CrtcStartLive), so it is loaded at once, as
         VideoLoadDefaultCrtc does. The cursor swaps with the page. Modes with one page
         (CGA, 11h-13h) keep the number only. */
    case VIDEO_FUNCTION_SELECT_PAGE: {
        BYTE newPage = (BYTE)(al & VIDEO_PAGE_MASK), oldPage = (BYTE)(state->Page & VIDEO_PAGE_MASK);
        state->PageRow[oldPage] = state->CursorRow; state->PageColumn[oldPage] = state->CursorColumn;
        state->Page = al;
        state->CursorRow = state->PageRow[newPage]; state->CursorColumn = state->PageColumn[newPage];
        if (state->ModeKind == VIDEO_KIND_TEXT || (state->ModeKind == VIDEO_KIND_PLANAR && state->Mode <= VIDEO_MODE_EGA_350)) {
            UINT32 offset = (UINT32)newPage * VideoPageSize(state);
            if (state->ModeKind == VIDEO_KIND_TEXT) offset >>= 1;
            state->CrtcStart = (WORD)offset;
            state->CrtcStartLive = state->StartVs = (WORD)offset;
            state->IsCrtcStartPending = 0;
            if (state->ModeKind == VIDEO_KIND_PLANAR) state->IsCrtcSeen = 1;   /* VideoRenderPlanar reads it */
        }
        break; }
    case VIDEO_FUNCTION_SCROLL_UP:
        VideoScrollUp(state, al, (BYTE)(VddGetCx(registers) >> BYTE_SHIFT), (BYTE)(VddGetCx(registers) & BYTE_MASK),
                  (BYTE)(VddGetDx(registers) >> BYTE_SHIFT), (BYTE)(VddGetDx(registers) & BYTE_MASK), (BYTE)(VddGetBx(registers) >> BYTE_SHIFT));
        break;
    /* ── 08h: READ THE CHARACTER AT PAGE BH's CURSOR -- from the cell in text, from
         the PIXELS in a graphics mode (VideoGraphicsReadChar), AH = 0 there. */
    case VIDEO_FUNCTION_READ_CHARACTER: {
        BYTE page = (BYTE)((VddGetBx(registers) >> BYTE_SHIFT) & VIDEO_PAGE_MASK);
        INT isActive = (page == (state->Page & VIDEO_PAGE_MASK));
        INT row = isActive ? state->CursorRow : state->PageRow[page], column = isActive ? state->CursorColumn : state->PageColumn[page];
        if (state->ModeKind == VIDEO_KIND_TEXT) {
            BYTE *cellPointer = VideoPageCell(state, page, row, column);
            VddSetAx(registers, (WORD)((cellPointer[1] << BYTE_SHIFT) | cellPointer[0]));
        } else VddSetAx(registers, VideoGraphicsReadChar(state, page, column, row));
        break; }
    /* ── 09h/0Ah: CX copies at page BH's cursor, cursor not moved. Text: the (char,
         attr) pairs on THAT page. Graphics: the glyph, BL = colour (0Ah too -- RBIL:
         "BL = colour in graphics modes"), bit 7 XOR; see VideoGraphicsGlyph. */
    case VIDEO_FUNCTION_WRITE_CHARACTER_ATTRIBUTE:
    case VIDEO_FUNCTION_WRITE_CHARACTER: {
        WORD count = VddGetCx(registers); BYTE attribute = (BYTE)(VddGetBx(registers) & BYTE_MASK);
        BYTE page = (BYTE)((VddGetBx(registers) >> BYTE_SHIFT) & VIDEO_PAGE_MASK);
        INT isActive = (page == (state->Page & VIDEO_PAGE_MASK));
        INT columnIndex = isActive ? state->CursorColumn : state->PageColumn[page], row = isActive ? state->CursorRow : state->PageRow[page];
        if (!count) count = 1;
        while (count-- && row < state->Rows) {
            if (state->ModeKind == VIDEO_KIND_TEXT) {
                BYTE *cellPointer = VideoPageCell(state, page, row, columnIndex); cellPointer[0] = al; if (ah == VIDEO_FUNCTION_WRITE_CHARACTER_ATTRIBUTE) cellPointer[1] = attribute;
            } else VideoGraphicsGlyph(state, page, columnIndex, row, al, attribute);
            if (++columnIndex >= state->Columns) { columnIndex = 0; if (++row >= state->Rows) break; }
        }
        break; }
    /* ── 0Ch/0Dh: ONE PIXEL, IN THE MODE'S OWN GEOMETRY (#252). The planar arm used a
         640-pixel stride in every planar mode and ignored the page; the CGA modes were
         not written at all (and read 0). AL bit 7 = XOR, except in 13h (one byte a
         pixel: the colour is all eight bits). BH = page in the planar modes. */
    case VIDEO_FUNCTION_WRITE_PIXEL: {
        UINT32 pixelColumn = VddGetCx(registers), line = VddGetDx(registers);
        INT isXor = (al & VIDEO_PIXEL_XOR) != 0;
        if (state->IsVesa) break;
        if (state->ModeKind == VIDEO_KIND_LINEAR8) {
            if (pixelColumn < state->GraphicsWidth && line < state->GraphicsHeight && line * state->GraphicsWidth + pixelColumn < VIDEO_APERTURE_SIZE) state->VideoMemory[line * state->GraphicsWidth + pixelColumn] = al;
        } else if (state->ModeKind == VIDEO_KIND_PLANAR) {
            if (pixelColumn < state->GraphicsWidth && line < state->GraphicsHeight) {
                UINT32 byteOffset = (UINT32)((VddGetBx(registers) >> BYTE_SHIFT) & VIDEO_PAGE_MASK) * VideoPageSize(state) + line * (state->GraphicsWidth / VIDEO_PIXELS_PER_PLANE_BYTE) + (pixelColumn >> VIDEO_PLANE_BYTE_SHIFT);
                BYTE  bitMask = (BYTE)(VIDEO_PIXEL_LEFT_BIT >> (pixelColumn & VIDEO_PIXEL_IN_BYTE_MASK)), planeIndex;
                if (byteOffset < VIDEO_PLANE_SIZE)
                    for (planeIndex = 0; planeIndex < VIDEO_PLANES; ++planeIndex) {
                        if (isXor) { if (al & (1 << planeIndex)) VideoPlaneBytes(state,planeIndex)[byteOffset] ^= bitMask; }
                        else if (al & (1 << planeIndex)) VideoPlaneBytes(state,planeIndex)[byteOffset] |= bitMask;
                        else                    VideoPlaneBytes(state,planeIndex)[byteOffset] &= (BYTE)~bitMask;
                    }
            }
        } else if (state->ModeKind == VIDEO_KIND_CGA) {
            if (pixelColumn < state->GraphicsWidth && line < state->GraphicsHeight) {
                BYTE *memory = state->VideoMemory + VIDEO_TEXT_OFFSET + ((line & 1) ? VIDEO_CGA_ODD_BANK : 0u) + (line >> 1) * VIDEO_CGA_BYTES_PER_LINE;
                if (state->CgaBpp == 1) {
                    BYTE bitMask = (BYTE)(VIDEO_PIXEL_LEFT_BIT >> (pixelColumn & VIDEO_PIXEL_IN_BYTE_MASK));
                    if (isXor) { if (al & 1) memory[pixelColumn >> VIDEO_PLANE_BYTE_SHIFT] ^= bitMask; }
                    else memory[pixelColumn >> VIDEO_PLANE_BYTE_SHIFT] = (BYTE)((memory[pixelColumn >> VIDEO_PLANE_BYTE_SHIFT] & ~bitMask) | ((al & 1) ? bitMask : 0));
                } else {
                    UINT shift = VIDEO_CGA_PIXEL_TOP_SHIFT - VIDEO_CGA_BITS_PER_PIXEL * (pixelColumn & VIDEO_CGA_PIXEL_MASK);
                    BYTE value = (BYTE)((al & VIDEO_CGA_PIXEL_MASK) << shift);
                    if (isXor) memory[pixelColumn >> VIDEO_CGA_BYTE_SHIFT] ^= value;
                    else memory[pixelColumn >> VIDEO_CGA_BYTE_SHIFT] = (BYTE)((memory[pixelColumn >> VIDEO_CGA_BYTE_SHIFT] & ~((UINT)VIDEO_CGA_PIXEL_MASK << shift)) | value);
                }
            }
        }
        break; }
    /* ── 0Eh: TELETYPE ON THE ACTIVE PAGE; BL = foreground in graphics (#252). BH is
         not consulted -- p_vidtxt t03.0E.which: with page 1 active a BH=0 teletype
         lands on page 1 (PCem's IBM ROM). */
    case VIDEO_FUNCTION_TELETYPE:
        VideoTeletypeChar(state, al, (BYTE)(VddGetBx(registers) & BYTE_MASK));
        break;
    case VIDEO_FUNCTION_GET_MODE:
        /* BH is the active page; BL IS NOT DEFINED BY THIS CALL and the real BIOS
           leaves it alone -- we were zeroing the whole of BX and taking the caller's
           BL with it. Measured: the oracle returns the probe's poison in BL. */
        VddSetAx(registers, (WORD)((state->Columns << BYTE_SHIFT) | state->Mode));
        VddSetBx(registers, (WORD)((state->Page << BYTE_SHIFT) | (VddGetBx(registers) & BYTE_MASK)));
        break;
    case VIDEO_FUNCTION_PALETTE:                                        /* palette / DAC            */
        /* ⚠ THE DAC CALLS BELOW GO THROUGH VideoDacPackWidth / VideoDacFrom8, i.e. AT THE DAC
             WIDTH (#226). A VGA BIOS implements them as plain OUTs to 3C8h/3C9h and INs
             from 3C7h/3C9h -- it does not know the RAMDAC was switched -- so after 4F08h
             BH=8 a real card takes these values as 8-bit too. Same device, same rule. */
        if (al == VIDEO_DAC_SET_REGISTER) {                             /* set one DAC register     */
            WORD paletteIndex = VddGetBx(registers);
            state->DacBlock[((paletteIndex & BYTE_MASK) >> VIDEO_DAC_BLOCK_SHIFT) & VIDEO_DAC_BLOCK_MASK]++;
            state->Dac[paletteIndex & BYTE_MASK] = VideoDacPackWidth(state, (BYTE)(VddGetDx(registers) >> BYTE_SHIFT),
                                             (BYTE)(VddGetCx(registers) >> BYTE_SHIFT), (BYTE)VddGetCx(registers));
            if (state->IsGreySum) VideoDacGrey(state, paletteIndex & BYTE_MASK, 1);   /* 12h BL=33h (#252) */
            VideoPaletteRefresh(state);
        } else if (al == VIDEO_DAC_SET_BLOCK) {                      /* set block of DAC regs    */
            WORD first = VddGetBx(registers), count = VddGetCx(registers), index;
            BYTE *table = (BYTE *)VddMapFlat(state->Bus, registers->Es, (WORD)VddGetDx(registers));
            for (index = 0; index < count && (first + index) < NTVDD_PALETTE_ENTRIES; ++index)
                { state->Dac[first + index] = VideoDacPackWidth(state, table[index*VIDEO_RGB_BYTES], table[index*VIDEO_RGB_BYTES+1], table[index*VIDEO_RGB_BYTES+VIDEO_RGB_BLUE]);
                  state->DacBlock[((first + index) >> VIDEO_DAC_BLOCK_SHIFT) & VIDEO_DAC_BLOCK_MASK]++;
                  if (((first + index) & VIDEO_DAC_BLOCK_BASE_MASK) == VIDEO_DAC_TRACKED_BLOCK) state->DacHighSinceReset++; }
            state->DacWrites += count;
            if (state->IsGreySum) VideoDacGrey(state, first, count);   /* 12h BL=33h (#252) */
            VideoPaletteRefresh(state);
        } else if (al == VIDEO_AC_SET_REGISTER) {                      /* set one palette register */
            BYTE registerIndex = (BYTE)((VddGetBx(registers) >> BYTE_SHIFT) & BYTE_MASK);
            if (registerIndex < VIDEO_PALETTE_REGISTERS_AND_BORDER) { state->PaletteRegisters[registerIndex] = (BYTE)(VddGetBx(registers) & VIDEO_AR_PALETTE_MASK); state->AcBiosWrites++; }
            VideoPaletteRefresh(state);   /* AH=10h stored vpal and rendered from ega16: inert until now */
        } else if (al == VIDEO_AC_SET_BORDER) {                      /* set the border           */
            state->PaletteRegisters[VIDEO_OVERSCAN_REGISTER] = state->Overscan = (BYTE)((VddGetBx(registers) >> BYTE_SHIFT) & VIDEO_AR_PALETTE_MASK);
        } else if (al == VIDEO_AC_SET_ALL) {                      /* set all 16 + border      */
            BYTE *table = (BYTE *)VddMapFlat(state->Bus, registers->Es, (WORD)VddGetDx(registers));
            INT index; for (index = 0; index < VIDEO_PALETTE_REGISTERS_AND_BORDER; ++index) state->PaletteRegisters[index] = (BYTE)(table[index] & VIDEO_AR_PALETTE_MASK);
            state->Overscan = state->PaletteRegisters[VIDEO_OVERSCAN_REGISTER];
            state->AcBiosWrites += VIDEO_PALETTE_REGISTERS_AND_BORDER;
            VideoPaletteRefresh(state);
        } else if (al == VIDEO_AC_BLINK) {                      /* blink vs bright background */
            /* The BIOS's job here is to write AR10 bit 3; keep both in step so a
               guest that sets it through the BIOS and then READS the register back
               sees what it asked for. Oracle-measured (p_video.asm ar10.blink.*):
               BL=0 leaves AR10 bit 3 clear, BL=1 sets it. */
            state->IsBlink = (BYTE)(VddGetBx(registers) & 1);
            state->AttributeMode = (BYTE)((state->AttributeMode & ~VIDEO_AR_MODE_BLINK) | (state->IsBlink ? VIDEO_AR_MODE_BLINK : 0u));
        } else if (al == VIDEO_AC_GET_REGISTER) {                      /* get one palette register */
            BYTE registerIndex = (BYTE)((VddGetBx(registers) >> BYTE_SHIFT) & BYTE_MASK);
            VddSetBx(registers, (WORD)((VddGetBx(registers) & HIGH_BYTE_MASK) | (registerIndex < VIDEO_PALETTE_REGISTERS_AND_BORDER ? state->PaletteRegisters[registerIndex] : 0)));
        } else if (al == VIDEO_AC_GET_BORDER) {                      /* get the border           */
            VddSetBx(registers, (WORD)((VddGetBx(registers) & BYTE_MASK) | ((WORD)state->PaletteRegisters[VIDEO_OVERSCAN_REGISTER] << BYTE_SHIFT)));
        } else if (al == VIDEO_AC_GET_ALL) {                      /* get all 16 + border      */
            BYTE *table = (BYTE *)VddMapFlat(state->Bus, registers->Es, (WORD)VddGetDx(registers));
            INT index; for (index = 0; index < VIDEO_PALETTE_REGISTERS_AND_BORDER; ++index) table[index] = state->PaletteRegisters[index];
        } else if (al == VIDEO_DAC_SELECT_PAGE) {                      /* select DAC page / mode   */
            state->DacPage = (BYTE)(VddGetBx(registers) >> BYTE_SHIFT);
        } else if (al == VIDEO_DAC_GET_REGISTER) {                      /* get one DAC register     */
            UINT32 value = state->Dac[VddGetBx(registers) & BYTE_MASK];
            VddSetDx(registers, (WORD)((WORD)VideoDacFrom8(state, (BYTE)(value >> VIDEO_RED_SHIFT)) << BYTE_SHIFT));
            VddSetCx(registers, (WORD)(((WORD)VideoDacFrom8(state, (BYTE)(value >> VIDEO_GREEN_SHIFT)) << BYTE_SHIFT)
                              | VideoDacFrom8(state, (BYTE)value)));
        } else if (al == VIDEO_DAC_GET_BLOCK) {                      /* get block of DAC regs    */
            WORD first = VddGetBx(registers), count = VddGetCx(registers), index;
            BYTE *table = (BYTE *)VddMapFlat(state->Bus, registers->Es, (WORD)VddGetDx(registers));
            for (index = 0; index < count && (first + index) < NTVDD_PALETTE_ENTRIES; ++index) {
                UINT32 value = state->Dac[first + index];
                table[index*VIDEO_RGB_BYTES]   = VideoDacFrom8(state, (BYTE)(value >> VIDEO_RED_SHIFT));
                table[index*VIDEO_RGB_BYTES+1] = VideoDacFrom8(state, (BYTE)(value >> VIDEO_GREEN_SHIFT));
                table[index*VIDEO_RGB_BYTES+VIDEO_RGB_BLUE] = VideoDacFrom8(state, (BYTE)value);
            }
        } else if (al == VIDEO_DAC_GET_PAGE) {                      /* get DAC page state       */
            VddSetBx(registers, (WORD)((state->DacPage << BYTE_SHIFT) | 0));
        } else if (al == VIDEO_DAC_GREY) {                      /* convert to grey scale    */
            VideoDacGrey(state, VddGetBx(registers), VddGetCx(registers));
            VideoPaletteRefresh(state);
        } else {
            VIDEO_UNIMPLEMENTED_SET(state->UnimplementedFunctions, VIDEO_FUNCTION_PALETTE);      /* name it, do not ignore it */
        }
        break;
    case VIDEO_FUNCTION_SCROLL_DOWN: {                                       /* scroll window DOWN      */
        /* 06h scrolled up and 07h fell through to the unimplemented default, so
           any program scrolling downwards silently did nothing. */
        BYTE count = al, top = (BYTE)(VddGetCx(registers) >> BYTE_SHIFT), left = (BYTE)(VddGetCx(registers) & BYTE_MASK);
        BYTE bottom = (BYTE)(VddGetDx(registers) >> BYTE_SHIFT), right = (BYTE)(VddGetDx(registers) & BYTE_MASK);
        BYTE attribute = (BYTE)(VddGetBx(registers) >> BYTE_SHIFT);
        INT row, column, step;
        if (state->ModeKind != VIDEO_KIND_TEXT) {              /* graphics: pixels, BH = fill colour (#252) */
            VideoGraphicsScroll(state, count, top, left, bottom, right, attribute, 0);
            break;
        }
        if (!count || count > (bottom - top + 1)) {               /* 0 or oversized = clear  */
            for (row = top; row <= bottom; ++row)
                for (column = left; column <= right; ++column)
                    { BYTE *cellPointer2 = VideoCell(state, row, column); cellPointer2[0] = ' '; cellPointer2[1] = attribute; }
        } else {
            for (step = 0; step < count; ++step) {
                for (row = bottom; row > top; --row)
                    for (column = left; column <= right; ++column) {
                        BYTE *destination = VideoCell(state, row, column), *source = VideoCell(state, row - 1, column);
                        destination[0] = source[0]; destination[1] = source[1];
                    }
                for (column = left; column <= right; ++column)
                    { BYTE *cellPointer2 = VideoCell(state, top, column); cellPointer2[0] = ' '; cellPointer2[1] = attribute; }
            }
        }
        break; }
    /* ── 0Bh: SET BACKGROUND / BORDER / CGA PALETTE, THROUGH THE ATTRIBUTE CONTROLLER (#266).
         It wrote BL into `overscan` (not AR11, which read back unchanged) whatever the
         mode, and BH=1 flipped a private `CgaPalette` that VideoRenderCga indexed a hard-coded
         table with -- so the AC registers a guest reads back, and AH=10h's view of them,
         never moved, and a background colour never appeared in any graphics mode.
         Now the CGA colour-select byte 0040:0066 is maintained (VddCgaColourSelect)
         and turned into AC values the way the VGA BIOS does (DOSBox's arithmetic; see
         VddCgaBackgroundAr / VddCgaPaletteAr):
           BH=0  AR11 (border) = BL as an AC colour, in every mode. In a graphics mode
                 AR00 (the background) too -- except 06h, where CGA's colour select is the
                 FOREGROUND and AR01 takes it. In 04h/05h AR01-03 follow 0066 bit 4
                 (intensity, BL bit 4).
           BH=1  0066 bit 5 = BL bit 0; in 04h/05h AR01-03 = palette 0 (2/4/6) or 1
                 (3/5/7), keeping the intensity. Other modes: the byte only.
         ⚠ UNMEASURED, and p_vid266 asks each one on PCem's IBM ROM: (a) AR00 in the
           EGA/VGA graphics modes (DOSBox sets it AND AR01-03 for every mode above 3;
           the LGPL/SeaVGABIOS line sets AR00 in text modes too and never AR11); (b) the
           06h foreground (taken from the CGA, the issue's reading); (c) 0066 in text. */
    case VIDEO_FUNCTION_CGA_PALETTE: {
        BYTE bh = (BYTE)(VddGetBx(registers) >> BYTE_SHIFT), bl = (BYTE)(VddGetBx(registers) & BYTE_MASK);
        BYTE colourSelect = state->BiosData ? state->BiosData[BIOS_BDA_CGA_PALETTE] : state->CgaSelect;
        INT isGraphics = (state->ModeKind != VIDEO_KIND_TEXT) && !state->IsVesa;
        INT isCga4 = isGraphics && (state->Mode == VIDEO_MODE_CGA_320 || state->Mode == VIDEO_MODE_CGA_320_GREY);
        BYTE attributes[VIDEO_CGA_PALETTE_COLOURS];
        if (bh > 1) break;                             /* not a defined BH: nothing    */
        colourSelect = VddCgaColourSelect(colourSelect, bh, bl);
        state->CgaSelect = colourSelect;
        if (state->BiosData) state->BiosData[BIOS_BDA_CGA_PALETTE] = colourSelect;
        if (bh == 0) {
            BYTE value = VddCgaBackgroundAr(bl);
            VideoAttributeBiosSet(state, VIDEO_AR_OVERSCAN, value);
            if (isGraphics) VideoAttributeBiosSet(state, (BYTE)(state->Mode == VIDEO_MODE_CGA_640 ? VIDEO_AR_PALETTE_1 : VIDEO_AR_PALETTE_0), value);
        } else {
            state->CgaPalette = (BYTE)(bl & 1);
        }
        if (isCga4) {
            VddCgaPaletteAr(colourSelect, attributes);
            VideoAttributeBiosSet(state, VIDEO_AR_PALETTE_1, attributes[0]); VideoAttributeBiosSet(state, VIDEO_AR_PALETTE_2, attributes[1]); VideoAttributeBiosSet(state, VIDEO_AR_PALETTE_3, attributes[2]);
        }
        VideoPaletteRefresh(state);
        break; }
    /* ── 04h: READ LIGHT PEN. A VGA has no light-pen input; its BIOS answers AH=00h,
         "not triggered" (#266). We left AH=04h -- which a caller reads as "triggered",
         with BX/CX/DX as a position. The other registers are untouched.
       ★ s92, MEASURED (dosdiff p_vid266 t03.04.pen): AX=0000 on MS-DOS 6.22, DOSBox-X and
         PCem's IBM VGA BIOS alike -- AL is cleared too, not only AH. */
    case VIDEO_FUNCTION_LIGHT_PEN:
        VddSetAx(registers, VIDEO_LIGHT_PEN_NOT_TRIGGERED);
        break;
    case VIDEO_FUNCTION_READ_PIXEL: {                                       /* READ a pixel            */
        WORD pixelColumn = VddGetCx(registers), line = VddGetDx(registers);
        BYTE value = 0;
        if (state->ModeKind == VIDEO_KIND_LINEAR8) {
            if (pixelColumn < state->GraphicsWidth && line < state->GraphicsHeight) value = state->VideoMemory[line * state->GraphicsWidth + pixelColumn];
        } else if (state->ModeKind == VIDEO_KIND_PLANAR) {     /* page BH (#252) */
            UINT32 byteIndex = (UINT32)((VddGetBx(registers) >> BYTE_SHIFT) & VIDEO_PAGE_MASK) * VideoPageSize(state) + line * (state->GraphicsWidth / VIDEO_PIXELS_PER_PLANE_BYTE) + (pixelColumn >> VIDEO_PLANE_BYTE_SHIFT);
            BYTE  pixelMask = (BYTE)(VIDEO_PIXEL_LEFT_BIT >> (pixelColumn & VIDEO_PIXEL_IN_BYTE_MASK));
            if (byteIndex < VIDEO_PLANE_SIZE)
                value = (BYTE)(((VideoPlaneBytes(state,0)[byteIndex] & pixelMask) ? 1 : 0)
                            | ((VideoPlaneBytes(state,1)[byteIndex] & pixelMask) ? VIDEO_PLANE1_BIT : 0)
                            | ((VideoPlaneBytes(state,2)[byteIndex] & pixelMask) ? VIDEO_PLANE2_BIT : 0)
                            | ((VideoPlaneBytes(state,3)[byteIndex] & pixelMask) ? VIDEO_PLANE3_BIT : 0));
        } else if (state->ModeKind == VIDEO_KIND_CGA && pixelColumn < state->GraphicsWidth && line < state->GraphicsHeight) {   /* #252: read 0 before */
            const BYTE *memory = state->VideoMemory + VIDEO_TEXT_OFFSET + ((line & 1) ? VIDEO_CGA_ODD_BANK : 0u) + (UINT32)(line >> 1) * VIDEO_CGA_BYTES_PER_LINE;
            value = state->CgaBpp == 1 ? (BYTE)((memory[pixelColumn >> VIDEO_PLANE_BYTE_SHIFT] >> (VIDEO_PIXEL_IN_BYTE_MASK - (pixelColumn & VIDEO_PIXEL_IN_BYTE_MASK))) & 1)
                                 : (BYTE)((memory[pixelColumn >> VIDEO_CGA_BYTE_SHIFT] >> (VIDEO_CGA_PIXEL_TOP_SHIFT - VIDEO_CGA_BITS_PER_PIXEL * (pixelColumn & VIDEO_CGA_PIXEL_MASK))) & VIDEO_CGA_PIXEL_MASK);
        }
        VddSetAx(registers, (WORD)((VddGetAx(registers) & HIGH_BYTE_MASK) | value));
        break; }
    case VIDEO_FUNCTION_SAVE_STATE: {                                       /* save / restore state    */
        /* CX is a bitmask of what to save; ES:BX is the buffer. Same state block as
           VBE 4F04 -- see VideoStateSave(). ⚠ This used to report 3 blocks and then
           write 768 bytes: a guest that allocated what it was told got its next MCB
           overwritten. The size reported is now the size written. */
        BYTE al1C = al;
        if (al1C == VIDEO_STATE_GET_SIZE)      { VddSetBx(registers, VIDEO_STATE_BLOCKS); VddSetAx(registers, (WORD)((VddGetAx(registers) & HIGH_BYTE_MASK) | VIDEO_FUNCTION_SAVE_STATE)); }
        else if (al1C == VIDEO_STATE_SAVE || al1C == VIDEO_STATE_RESTORE) {
            BYTE *saveBuffer = (BYTE *)VddMapFlat(state->Bus, registers->Es, (WORD)registers->Ebx);
            if (al1C == VIDEO_STATE_SAVE) VideoStateSave(state, saveBuffer, VddGetCx(registers));
            else              (VOID)VideoStateLoad(state, saveBuffer);   /* a foreign buffer: no-op */
            VddSetAx(registers, (WORD)((VddGetAx(registers) & HIGH_BYTE_MASK) | VIDEO_FUNCTION_SAVE_STATE));
        }
        break; }
    /* ── 13h WRITE STRING (#252). AL bit 0 = leave the cursor after the string (clear:
         put it back), bit 1 = the string is (char, attr) pairs (else BL for all); BH =
         page; DH/DL = where. BEL, BS, CR and LF are EXECUTED, as the teletype does --
         they were stored as glyphs, the cursor was always moved, the page ignored, and
         a graphics mode got (char, attr) pairs at B800:0. Scrolling past the bottom
         happens on the active page only. */
    case VIDEO_FUNCTION_WRITE_STRING: {
        WORD count = VddGetCx(registers), index; BYTE mode = al, attribute = (BYTE)(VddGetBx(registers) & BYTE_MASK);
        BYTE page = (BYTE)((VddGetBx(registers) >> BYTE_SHIFT) & VIDEO_PAGE_MASK);
        INT isActive = (page == (state->Page & VIDEO_PAGE_MASK));
        INT row = (BYTE)(VddGetDx(registers) >> BYTE_SHIFT), column = (BYTE)(VddGetDx(registers) & BYTE_MASK);
        BYTE *string = (BYTE *)VddMapFlat(state->Bus, registers->Es, (WORD)registers->Ebp);
        if (!string) break;
        for (index = 0; index < count; ++index) {
            BYTE character = *string++; if (mode & VIDEO_STRING_HAS_ATTRIBUTES) attribute = *string++;
            if (character == VIDEO_CHAR_CARRIAGE_RETURN) { column = 0; continue; }
            if (character == VIDEO_CHAR_BACKSPACE) { if (column) --column; continue; }
            if (character == VIDEO_CHAR_BELL) continue;
            if (character != VIDEO_CHAR_LINE_FEED) {
                if (row < state->Rows && column < state->Columns) {
                    if (state->ModeKind == VIDEO_KIND_TEXT) { BYTE *cellPointer = VideoPageCell(state, page, row, column); cellPointer[0] = character; cellPointer[1] = attribute; }
                    else VideoGraphicsGlyph(state, page, column, row, character, attribute);
                }
                if (++column < state->Columns) continue;
                column = 0;
            }
            if (++row >= state->Rows) {
                if (isActive) VideoScrollUp(state, 1, 0, 0, state->Rows - 1, state->Columns - 1, VideoTeletypeFill(state));
                row = state->Rows - 1;
            }
        }
        if (mode & VIDEO_STRING_MOVE_CURSOR) {
            if (isActive) { state->CursorRow = (BYTE)row; state->CursorColumn = (BYTE)column; }
            else     { state->PageRow[page] = (BYTE)row; state->PageColumn[page] = (BYTE)column; }
        }
        break; }
    case VIDEO_FUNCTION_FONT:                                         /* character generator     */
        state->Int10Ah11Calls++;                          /* did the guest ASK at all? */
        if (al == VIDEO_FONT_GET_INFO) {                              /* get font info -> ES:BP  */
            /* THE POINTER IS THE POINT. BH selects which table the caller wants, and the
               answer is returned in ES:BP with CX = bytes per character. Returning only
               CX/DL (as we used to) leaves the caller drawing from whatever ES:BP already
               held -- which is why Skyroads' "ROAD COMPLETED" came out as glyph-shaped
               noise. BH: 0/1 = the INT 1Fh / INT 43h vectors, 2 = 8x14, 3 = 8x8 lower,
               4 = 8x8 upper (chars 128-255), 5 = 9x14 alt, 6 = 8x16, 7 = 9x16 alt. We hold
               two real tables and answer every code from the nearer of the two. */
            BYTE bh = (BYTE)((VddGetBx(registers) >> BYTE_SHIFT) & BYTE_MASK);
            WORD segment = VDD_FONT8X16_SEG, offset = 0, bytesPerCharacter = VGA_FONT16_HEIGHT;
            switch (bh) {
            case VIDEO_FONT_INFO_ROM_8X8: segment = VDD_FONT8X8_SEG;  offset = 0;       bytesPerCharacter = VGA_FONT8_HEIGHT;  break;
            case VIDEO_FONT_INFO_INT1F:                                        /* INT 1Fh: 8x8 upper half */
                /* #266: what INT 1Fh holds -- ours (8x8 upper half) unless AL=20h
                   installed the caller's. */
                if (state->IsInt1FUser) { segment = state->Int1FSegment; offset = state->Int1FOffset; bytesPerCharacter = VGA_FONT8_HEIGHT; break; }
                /* fall through */
            case VIDEO_FONT_INFO_ROM_8X8_HIGH: segment = VDD_FONT8X8_SEG;  offset = VIDEO_FONT_HIGH_FIRST * VGA_FONT8_HEIGHT; bytesPerCharacter = VGA_FONT8_HEIGHT;  break;
            case VIDEO_FONT_INFO_ROM_8X14:                                        /* ROM 8x14 / 9x14 alt     */
            case VIDEO_FONT_INFO_ROM_9X14: segment = VDD_FONT8X14_SEG; offset = 0;       bytesPerCharacter = VGA_FONT14_HEIGHT; break;
            case VIDEO_FONT_INFO_INT43:                                        /* INT 43h: the CURRENT font */
                /* Whatever the active mode actually draws with -- 8x8 in the 200-line
                   graphics modes, 8x16 in text. We used to answer this (and 8x14) with the
                   8x16 table while reporting CX=14, so a caller striding by 14 through
                   16-byte glyphs drifted 2 bytes per character and drew shredded text.
                   CellHeight is that answer, and it follows a 1112h/1111h font change too. */
                /* #266: and a caller's graphics font (AL=21h) IS the current font --
                   INT 43h points at it, so this answers with it. */
                if (state->IsGraphicsFontUser) { segment = state->Int43Segment; offset = state->Int43Offset; bytesPerCharacter = state->CellHeight; }
                else if (state->CellHeight == VGA_FONT8_HEIGHT)  { segment = VDD_FONT8X8_SEG;  offset = 0; bytesPerCharacter = VGA_FONT8_HEIGHT;  }
                else if (state->CellHeight == VGA_FONT14_HEIGHT) { segment = VDD_FONT8X14_SEG; offset = 0; bytesPerCharacter = VGA_FONT14_HEIGHT; }
                else                       { segment = VDD_FONT8X16_SEG; offset = 0; bytesPerCharacter = VGA_FONT16_HEIGHT; }
                break;
            default:   segment = VDD_FONT8X16_SEG; offset = 0;       bytesPerCharacter = VGA_FONT16_HEIGHT; break;
            }
            registers->Es = segment; registers->Ebp = offset;
            /* ── ★ CX IS THE ON-SCREEN FONT'S HEIGHT, NOT THE REQUESTED TABLE'S. ──
                 The classic gotcha in this call, and we had it backwards: BH selects
                 which TABLE ES:BP points at, but CX reports the height of the font
                 the screen is CURRENTLY drawing with. Measured on 6.22: BH=0 asks for
                 the 8x8 upper half and CX still comes back 16 in mode 3. We answered
                 8, contradicting our OWN BDA byte at 0040:0085 two lines of probe
                 output earlier -- and a 43/50-line editor sizes the screen from CX. */
            VddSetCx(registers, state->CellHeight ? state->CellHeight : bytesPerCharacter);    /* on-screen bytes/character */
            VddSetDx(registers, (WORD)(state->Rows ? state->Rows - 1 : VIDEO_TEXT_DEFAULT_ROWS - 1));  /* DL = rows-1     */
            if (state->FontQueryCount < VIDEO_FONT_QUERIES) {                     /* record the request + answer */
                state->FontQueries[state->FontQueryCount].Al  = al;
                state->FontQueries[state->FontQueryCount].Bh  = bh;
                state->FontQueries[state->FontQueryCount].Segment = segment;
                state->FontQueries[state->FontQueryCount].Offset = offset;
                state->FontQueries[state->FontQueryCount].Cx  = bytesPerCharacter;
                state->FontQueryCount++;
            }
        } else if (al == VIDEO_FONT_SET_BLOCK) {
            /* ── 03h: SET BLOCK SPECIFIER = the Sequencer's Character Map Select (#266).
                 BL goes to SR3 as-is (SeaVGABIOS: stdvga_set_text_block_specifier) --
                 which font block attribute bit 3 = 0 / = 1 cells are drawn from. It was
                 caught by the ROM-font arm below as "AL & 0Fh <= 4" and reloaded the
                 8x16 ROM font, DROPPING a font the program had just loaded to select it.
               ⚠ The renderer still draws one font (the loaded one, whatever block it
                 was loaded to), so SR3 reads back right and selects nothing yet: the
                 512-character case is a docs/inventory/vga.md gap. */
            state->SequencerRegisters[VIDEO_SR_CHARACTER_MAP] = (BYTE)(VddGetBx(registers) & BYTE_MASK);
        } else if ((al & VIDEO_FONT_LOAD_MASK) <= VIDEO_FONT_LOAD_8X16 && al != (VIDEO_FONT_RECALCULATE | VIDEO_FONT_SET_BLOCK) && (al <= VIDEO_FONT_LOAD_8X16 || (al >= VIDEO_FONT_RECALCULATE && al <= (VIDEO_FONT_RECALCULATE | VIDEO_FONT_LOAD_8X16)))) {
            /* ── AL=x0 LOADS THE CALLER'S OWN GLYPHS, AND NOW THEY GET DRAWN. ──
                 This used to accept the call, mark it unimplemented and keep
                 drawing from the ROM table -- so a program that loaded a custom
                 character set saw the stock font and no error at all. That is
                 the silent-wrong-output class: the call succeeded, the screen
                 was wrong, and nothing said so.
                 ES:BP = the table, CX = how many characters, DX = the first
                 character, BH = bytes per character, BL = the font block.
                 AL=x1/x2/x3/x4 select ROM fonts, which is a request to go BACK
                 to our own tables -- so they clear the override rather than
                 leaving a stale user font in place. (GH #52) */
            if ((al & VIDEO_FONT_LOAD_MASK) == VIDEO_FONT_LOAD_USER) {
                WORD fontSegment = registers->Es, fontOffset = (WORD)(registers->Ebp & WORD_MASK_U);
                WORD characterCount = (WORD)VddGetCx(registers), first = (WORD)VddGetDx(registers);
                BYTE  bytesPerCharacter = (BYTE)((VddGetBx(registers) >> BYTE_SHIFT) & BYTE_MASK);
                const BYTE *fontSource = (const BYTE *)VddMapFlat(state->Bus, fontSegment, fontOffset);
                if (!fontSource || bytesPerCharacter == 0 || bytesPerCharacter > VIDEO_CELL_HEIGHT) {
                    /* Cannot represent it -- a cell is VIDEO_CELL_HEIGHT tall. Say so
                       rather than store something the renderer would misread. */
                    VIDEO_UNIMPLEMENTED_SET(state->UnimplementedFunctions, VIDEO_FUNCTION_FONT);
                } else {
                    UINT index, line;
                    if (!state->IsUserFontOn) {       /* seed from ROM so characters
                                                      the caller does NOT supply
                                                      still draw as themselves */
                        UINT candidate;
                        for (candidate = 0; candidate < VGA_FONT_CHARACTERS; ++candidate)
                            for (line = 0; line < VIDEO_CELL_HEIGHT; ++line)
                                state->UserFont[candidate * VIDEO_CELL_HEIGHT + line] = g_VgaFont8x16[candidate][line];
                    }
                    for (index = 0; index < characterCount && (first + index) < VGA_FONT_CHARACTERS; ++index) {
                        UINT character2 = first + index;
                        for (line = 0; line < VIDEO_CELL_HEIGHT; ++line)
                            state->UserFont[character2 * VIDEO_CELL_HEIGHT + line] =
                                (line < bytesPerCharacter) ? fontSource[index * bytesPerCharacter + line] : 0;
                    }
                    state->UserFontRows = bytesPerCharacter;
                    state->IsUserFontOn = 1;
                    state->IsDirty = 1;
                    /* AL=10h (not 00h) also reprograms the CRTC for the new height:
                       the cell becomes the font's height and the row count follows. */
                    if (al == (VIDEO_FONT_RECALCULATE | VIDEO_FONT_LOAD_USER) && state->ModeKind == VIDEO_KIND_TEXT) {
                        state->CellHeight = bytesPerCharacter;
                        state->Rows = VideoTextRowsFor(bytesPerCharacter);
                        VideoInt43Rom(state, bytesPerCharacter);  /* 1130h BH=1 and INT 43h agree (#266) */
                    }
                }
            } else {
                /* ── ★ AL=x1/x2/x4 SELECT A ROM FONT, AND WITH 1x THAT IS A ROW COUNT. ──
                     1112h is THE 50-line call: load the 8x8 ROM font and, because AL
                     has bit 4 set, recompute the CRTC -- 400 lines / 8 = 50 rows.
                     1111h is 8x14 (28 rows) and 1114h 8x16 (25). The 0x variants only
                     change the glyphs, which is what the BIOS documents and what a
                     caller that follows 1102h with its own CRTC programming expects. */
                BYTE height = (BYTE)(((al & VIDEO_FONT_LOAD_MASK) == VIDEO_FONT_LOAD_8X8) ? VGA_FONT8_HEIGHT : ((al & VIDEO_FONT_LOAD_MASK) == VIDEO_FONT_LOAD_8X14) ? VGA_FONT14_HEIGHT : VGA_FONT16_HEIGHT);
                state->IsUserFontOn = 0;              /* back to the ROM tables */
                if ((al & VIDEO_FONT_RECALCULATE) && state->ModeKind == VIDEO_KIND_TEXT) {
                    state->CellHeight = height;
                    state->Rows = VideoTextRowsFor(height);
                    if (state->CursorRow >= state->Rows) state->CursorRow = (BYTE)(state->Rows - 1);
                    /* #266: INT 43h follows the cell, so 1130h BH=1 (which answers with
                       the cell's table, a measured fix) and the vector stay one fact.
                       ⚠ SeaVGABIOS leaves INT 43h alone on a text-mode font load; IBM's
                       ROM is unmeasured. Kept consistent with our own 1130h instead. */
                    VideoInt43Rom(state, height);
                }
                state->IsDirty = 1;
            }
            VddSetDx(registers, (WORD)(state->Rows ? state->Rows - 1 : VIDEO_TEXT_DEFAULT_ROWS - 1));
        } else if (al >= VIDEO_FONT_GRAPHICS_INT1F && al <= VIDEO_FONT_GRAPHICS_8X16) {
            /* ── 20h-24h: THE GRAPHICS-MODE FONT CALLS (#266). They answered DL and
                 stored nothing -- no vector, no row count -- so a program that loaded
                 its own graphics font (or asked for 43 rows of 8x8 in mode 10h) kept
                 drawing with the ROM table at the mode's own height.
                   20h  INT 1Fh = ES:BP (the 8x8 characters 80h-FFh)
                   21h  INT 43h = ES:BP, CX bytes a character
                   22h  INT 43h = the ROM 8x14     23h  the ROM 8x8     24h  the ROM 8x16
                 21h-24h also set the screen's rows from BL (VddGraphicsFontRows: 0 = DL,
                 1 = 14, 2 = 25, 3 = 43) and the character height -- 0040:0084/0085 --
                 which here are `rows` and `CellHeight`, so the glyph services draw at that
                 height from that table (VideoGraphicsFont) and the BDA follows.
               ⚠ Registers are left as they came (SeaVGABIOS returns nothing; this used
                 to write DX). In a TEXT mode only the vector moves: rows/CellHeight are the
                 text screen's geometry, and the calls are documented for graphics modes
                 (IBM's text-mode behaviour unmeasured). A height outside 1..16 sets the
                 vector and is reported, since our glyph paths draw at most 16 lines. */
            if (al == VIDEO_FONT_GRAPHICS_INT1F) {
                state->Int1FSegment = registers->Es; state->Int1FOffset = (WORD)(registers->Ebp & WORD_MASK_U); state->IsInt1FUser = 1;
                VideoSetVector(state, VIDEO_VECTOR_FONT_HIGH, state->Int1FSegment, state->Int1FOffset);
            } else {
                BYTE height = al == VIDEO_FONT_GRAPHICS_USER ? (BYTE)(VddGetCx(registers) & BYTE_MASK) : al == VIDEO_FONT_GRAPHICS_8X14 ? VGA_FONT14_HEIGHT : al == VIDEO_FONT_GRAPHICS_8X8 ? VGA_FONT8_HEIGHT : VGA_FONT16_HEIGHT;
                BYTE rows = VddGraphicsFontRows((BYTE)(VddGetBx(registers) & BYTE_MASK), (BYTE)(VddGetDx(registers) & BYTE_MASK));
                if (al == VIDEO_FONT_GRAPHICS_USER) {
                    state->Int43Segment = registers->Es; state->Int43Offset = (WORD)(registers->Ebp & WORD_MASK_U); state->IsGraphicsFontUser = 1;
                    VideoSetVector(state, VIDEO_VECTOR_GRAPHICS_FONT, state->Int43Segment, state->Int43Offset);
                } else VideoInt43Rom(state, height);
                if (state->ModeKind != VIDEO_KIND_TEXT && !state->IsVesa) {
                    if (height >= 1 && height <= VGA_FONT16_HEIGHT && rows) { state->CellHeight = height; state->Rows = rows; }
                    else VIDEO_UNIMPLEMENTED_SET(state->UnimplementedFunctions, VIDEO_FUNCTION_FONT);
                }
            }
        } else {
            VIDEO_UNIMPLEMENTED_SET(state->UnimplementedFunctions, VIDEO_FUNCTION_FONT);
        }
        break;
    case VIDEO_FUNCTION_ALTERNATE_SELECT: {                                       /* alternate function sel  */
        BYTE bl = (BYTE)(VddGetBx(registers) & BYTE_MASK);
        if (bl == VIDEO_ALTERNATE_GET_INFO) { VddSetBx(registers, VIDEO_ALTERNATE_INFO_COLOUR_256K); VddSetCx(registers, VIDEO_ALTERNATE_INFO_SWITCHES); }  /* color / 256K, switches */
        /* ── ★★ BL=31h: DEFAULT PALETTE LOADING. NOT A NO-OP. ────────────────────
             A mode set normally reloads the DAC and the attribute palette, which
             destroys any colours the program has already installed. A game that
             wants to keep them says so with this call BEFORE setting the mode --
             and that is exactly what Lemmings does.
           ⚠ WE USED TO ACCEPT IT AND IGNORE IT. The `else` arm below answers
             AL=12h ("supported") to every unhandled BL, so this looked handled and
             did nothing: measured, Lemmings wrote 752 entries into DAC 0x30..0x3F,
             a mode set wiped them (palresets=5, hi_since_reset=0), and it never
             wrote them again -- because on real hardware it did not have to. Its
             terrain then rendered in the leftover default EGA brights, which is the
             cyan-and-green "garbling" this whole thread has been chasing.
             The same "unimplemented call still answers" shape as ever: a sentinel
             that reads as success is worse than a refusal. */
        else if (bl == VIDEO_ALTERNATE_PALETTE_LOADING) {
            state->IsDefaultPaletteOff = (BYTE)((VddGetAx(registers) & BYTE_MASK) ? 1 : 0);
            VddSetAx(registers, (WORD)((VddGetAx(registers) & HIGH_BYTE_MASK) | VIDEO_FUNCTION_ALTERNATE_SELECT));
        }
        /* ── #252: THE REST OF THE VGA's BL TABLE, EACH DOING ITS JOB -- and anything
             else REFUSED. The `else` used to answer AL=12h for every BL, so 30h/32h/33h/
             34h/36h each said "done" and did nothing. AL in = 0 enable / 1 disable (30h:
             0/1/2 = 200/350/400 lines); AL out = 12h. An AL out of range is refused too.
               30h  the next colour TEXT mode set comes up in that many lines (VideoInt10 AH=00h);
               32h  CPU access to video memory: MiscOut bit 1 is the switch on the card.
                    Recorded in the register (3CCh reads it back); our aperture is host
                    memory and is not cut off -- a docs/inventory/vga.md gap, not this one;
               33h  grey-scale summing of later DAC loads (mode set, AH=10h AL=10h/12h);
               34h  CGA cursor emulation -- off, AH=01h's CX is drawn literally;
               36h  video refresh: Clocking Mode (SR1) bit 5 "screen off", recorded the
                    same way as 32h (the renderer does not blank on SR1 -- vga.md).
             Unknown BL: PCem's IBM VGA ROM (and DOSBox-X) return AL=00h -- p_vidtxt
             t12.55.unknown -- so does this. (SeaVGABIOS leaves AL: provisional.) */
        else if (bl == VIDEO_ALTERNATE_SCAN_LINES || bl == VIDEO_ALTERNATE_VIDEO_ACCESS || bl == VIDEO_ALTERNATE_GREY_SUMMING || bl == VIDEO_ALTERNATE_CURSOR_EMULATION || bl == VIDEO_ALTERNATE_VIDEO_REFRESH) {
            BYTE requestValue = (BYTE)(VddGetAx(registers) & BYTE_MASK);
            if (requestValue > (bl == VIDEO_ALTERNATE_SCAN_LINES ? VIDEO_SCAN_SELECT_400 : 1)) { VddSetAx(registers, (WORD)(VddGetAx(registers) & HIGH_BYTE_MASK)); break; }
            if (bl == VIDEO_ALTERNATE_SCAN_LINES)      state->ScanSelect = requestValue;
            else if (bl == VIDEO_ALTERNATE_VIDEO_ACCESS) { state->IsVideoOff = requestValue; state->MiscOutput = (BYTE)(requestValue ? (state->MiscOutput & ~VIDEO_MISC_RAM_ENABLE) : (state->MiscOutput | VIDEO_MISC_RAM_ENABLE)); }
            else if (bl == VIDEO_ALTERNATE_GREY_SUMMING) state->IsGreySum = (BYTE)(requestValue == 0);
            else if (bl == VIDEO_ALTERNATE_CURSOR_EMULATION) state->IsCursorEmulationOff = requestValue;
            else                 state->SequencerRegisters[VIDEO_SR_CLOCKING_MODE] = (BYTE)(requestValue ? (state->SequencerRegisters[VIDEO_SR_CLOCKING_MODE] | VIDEO_SR1_SCREEN_OFF) : (state->SequencerRegisters[VIDEO_SR_CLOCKING_MODE] & ~VIDEO_SR1_SCREEN_OFF));
            VddSetAx(registers, (WORD)((VddGetAx(registers) & HIGH_BYTE_MASK) | VIDEO_FUNCTION_ALTERNATE_SELECT));
        }
        else            { VddSetAx(registers, (WORD)(VddGetAx(registers) & HIGH_BYTE_MASK)); }   /* not supported: AL=00h */
        break; }
    case VIDEO_FUNCTION_DISPLAY_COMBINATION:                                         /* get display combination */
        VddSetAx(registers, (WORD)((VddGetAx(registers) & HIGH_BYTE_MASK) | VIDEO_FUNCTION_DISPLAY_COMBINATION));/* AL=1A: function present */
        VddSetBx(registers, VIDEO_DCC_VGA_COLOUR);                               /* BL=08 active=VGA colour */
        break;
    case VIDEO_FUNCTION_FUNCTIONALITY: {                                       /* functionality/state info */
        BYTE *buffer = (BYTE *)VddMapFlat(state->Bus, registers->Es, (WORD)registers->Edi);
        UINT index;
        for (index = 0; index < VIDEO_STATE_INFO_BYTES; ++index) buffer[index] = 0;
        /* static functionality table kept inside the 64-byte block (reserved tail
           at 0x2E) so we never write past the caller's buffer. */
        VideoWrite32(buffer + VIDEO_STATE_INFO_STATIC_POINTER, ((UINT32)registers->Es << WORD_SHIFT) | (((WORD)registers->Edi + VIDEO_STATE_INFO_STATIC_TABLE) & WORD_MASK_U));
        buffer[VIDEO_STATE_INFO_MODE] = (BYTE)state->Mode;                      /* current mode            */
        VideoWrite16(buffer + VIDEO_STATE_INFO_COLUMNS, state->Columns);                         /* columns on screen       */
        buffer[VIDEO_STATE_INFO_ROWS] = (BYTE)(state->Rows ? state->Rows : VIDEO_TEXT_DEFAULT_ROWS); /* character rows          */
        VideoWrite16(buffer + VIDEO_STATE_INFO_CHARACTER_HEIGHT, state->CellHeight ? state->CellHeight : VGA_FONT16_HEIGHT);  /* bytes per character     */
        buffer[VIDEO_STATE_INFO_ACTIVE_DCC] = VIDEO_DCC_VGA_COLOUR;                                /* active DCC = VGA colour */
        VideoWrite16(buffer + VIDEO_STATE_INFO_COLOURS, NTVDD_PALETTE_ENTRIES);                           /* number of colours       */
        buffer[VIDEO_STATE_INFO_PAGES] = VIDEO_PAGES;                                   /* number of pages         */
        buffer[VIDEO_STATE_INFO_SCAN_LINES] = 0;                                   /* scan lines (0 = 200)    */
        buffer[VIDEO_STATE_INFO_PRIMARY_BLOCK] = 0; buffer[VIDEO_STATE_INFO_SECONDARY_BLOCK] = 0; buffer[VIDEO_STATE_INFO_MISC] = VIDEO_STATE_INFO_MISC_FLAGS;      /* char blocks / misc      */
        /* static functionality table (16 bytes) -- modes 0..0x1F all supported   */
        buffer[VIDEO_STATE_INFO_STATIC_TABLE + 0] = VIDEO_STATIC_ALL_MODES; buffer[VIDEO_STATE_INFO_STATIC_TABLE + 1] = VIDEO_STATIC_ALL_MODES; buffer[VIDEO_STATE_INFO_STATIC_TABLE + 2] = VIDEO_STATIC_ALL_MODES; buffer[VIDEO_STATE_INFO_STATIC_TABLE + 3] = VIDEO_STATIC_ALL_MODES;
        buffer[VIDEO_STATE_INFO_STATIC_TABLE + VIDEO_STATIC_SCAN_LINES] = VIDEO_STATIC_SCAN_LINES_200_350_400;                            /* scan lines 200/350/400  */
        buffer[VIDEO_STATE_INFO_STATIC_TABLE + VIDEO_STATIC_CHARACTER_BLOCKS] = VIDEO_STATIC_FONT_BLOCKS; buffer[VIDEO_STATE_INFO_STATIC_TABLE + VIDEO_STATIC_ACTIVE_BLOCKS] = VIDEO_STATIC_FONT_BLOCKS;              /* char blocks             */
        buffer[VIDEO_STATE_INFO_STATIC_TABLE + VIDEO_STATIC_CAPABILITIES] = VIDEO_STATIC_CAPABILITY_BITS; buffer[VIDEO_STATE_INFO_STATIC_TABLE + VIDEO_STATIC_CAPABILITIES_2] = VIDEO_STATIC_CAPABILITY_BITS_2;  /* capability bits         */
        VddSetAx(registers, (WORD)((VddGetAx(registers) & HIGH_BYTE_MASK) | VIDEO_FUNCTION_FUNCTIONALITY));/* AL=1B: supported        */
        break; }
    case VIDEO_FUNCTION_VESA: VideoVesa(state, registers); break;                     /* VESA VBE 2.0            */
    default:                                           /* unimplemented function  */
        VIDEO_UNIMPLEMENTED_SET(state->UnimplementedFunctions, ah);
        state->IsDirty = 0;
        break;
    }
    VddVideoBdaSync(state);                            /* the BDA follows every call */
}

/* DAC palette ports 3C7 (read index) / 3C8 (write index) / 3C9 (data). */
static VOID VideoDacOut(PVOID context, WORD port, BYTE width, UINT32 value)
{
    PVIDEO_STATE state = (PVIDEO_STATE)context; BYTE byteValue = (BYTE)value; (VOID)width;
    if (port == VIDEO_PORT_DAC_WRITE_INDEX) { state->DacWriteIndex = byteValue; state->DacComponent = 0; }
    else if (port == VIDEO_PORT_DAC_READ_INDEX) { state->DacReadIndex = byteValue; state->DacComponent = 0; }
    else if (port == VIDEO_PORT_DAC_DATA) {
        /* The byte as written; the width decides at the third primary what it means
           (6 bits: the low six, bits 6-7 ignored as the hardware ignores them; 8 bits:
           all of it). See VideoDacTo8 -- this used to mask to 6 bits whatever 4F08 said. */
        state->DacLatch[state->DacComponent++] = byteValue;
        if (state->DacComponent >= VIDEO_RGB_BYTES) {
            state->Dac[state->DacWriteIndex] = VideoDacPackWidth(state, state->DacLatch[0], state->DacLatch[1], state->DacLatch[VIDEO_RGB_BLUE]);
            state->DacBlock[(state->DacWriteIndex >> VIDEO_DAC_BLOCK_SHIFT) & VIDEO_DAC_BLOCK_MASK]++;
            if ((state->DacWriteIndex & VIDEO_DAC_BLOCK_BASE_MASK) == VIDEO_DAC_TRACKED_BLOCK) state->DacHighSinceReset++;
            state->DacWriteIndex++; state->DacComponent = 0; state->DacWrites++;
            {   UINT64 now; UINT32 frameUs, verticalTotal, verticalDisplay, verticalBlank, frameNumber, line; WORD row = VIDEO_ROW_UNKNOWN;
                if (VideoBeam(state, &now, &frameUs, &verticalTotal, &verticalDisplay, &verticalBlank, &frameNumber, &line) && state->GraphicsHeight && verticalDisplay)
                    row = (line >= verticalBlank) ? VIDEO_ROW_NONE : (WORD)(((UINT64)line * state->GraphicsHeight) / verticalDisplay);
                state->DacLastRow = row;
                state->DacRowHistogram[row == VIDEO_ROW_NONE ? VIDEO_DAC_ROW_BIN_BLANK : row == VIDEO_ROW_UNKNOWN ? VIDEO_DAC_ROW_BIN_TOP : row < VIDEO_DAC_ROW_TOP_ROWS ? VIDEO_DAC_ROW_BIN_TOP : row < VIDEO_DAC_ROW_SPLIT ? VIDEO_DAC_ROW_BIN_UPPER : VIDEO_DAC_ROW_BIN_LOWER]++;
            }
            /* pal[] is DERIVED from dac[] -- see VideoPaletteRefresh. Without this a guest
               could reprogram the DAC and see nothing change, which is precisely the
               half of the Lemmings bug that survived the first fix. */
            VideoPaletteRefresh(state);
        }
    }
}
static VOID VideoDacIn(PVOID context, WORD port, BYTE width, UINT32 *value)
{
    PVIDEO_STATE state = (PVIDEO_STATE)context; UINT32 position; (VOID)width;
    if (port == VIDEO_PORT_DAC_WRITE_INDEX) { *value = state->DacWriteIndex; return; }
    if (port != VIDEO_PORT_DAC_DATA) { *value = VIDEO_FLOATING_BUS; return; }
    position = state->Dac[state->DacReadIndex];
    switch (state->DacComponent) {
    case 0: *value = VideoDacFrom8(state, (BYTE)(position >> VIDEO_RED_SHIFT)); break;   /* R, at the DAC width */
    case 1: *value = VideoDacFrom8(state, (BYTE)(position >> VIDEO_GREEN_SHIFT));  break;   /* G                   */
    default:*value = VideoDacFrom8(state, (BYTE)position); state->DacReadIndex++; break;   /* B, then advance */
    }
    if (++state->DacComponent >= VIDEO_RGB_BYTES) state->DacComponent = 0;
}

/* --- VGA planar write engine (mode 12h: Sequencer 3C4/5 + GC 3CE/F) ------- */
static BYTE VideoRotateRight(BYTE value, BYTE count)
{ count &= VIDEO_ROTATE_MASK; return count ? (BYTE)((value >> count) | (value << (BITS_PER_BYTE - count))) : value; }
static BYTE VideoAlu(BYTE operation, BYTE value, BYTE latch)
{ switch (operation & VIDEO_GC_ALU_MASK) { case VIDEO_GC_ALU_AND: return (BYTE)(value & latch); case VIDEO_GC_ALU_OR: return (BYTE)(value | latch);
                    case VIDEO_GC_ALU_XOR: return (BYTE)(value ^ latch); default: return value; } }

static VOID VideoPlanarWrite1(PVIDEO_STATE state, UINT32 offset, BYTE cpu)
{
    BYTE alu = (BYTE)((state->FunctionRotate >> VIDEO_GC_ALU_SHIFT) & VIDEO_GC_ALU_MASK), bitMask = state->BitMask; INT plane;
    if (offset >= VIDEO_PLANE_SIZE) return;
    state->IsDirty = 1;
    switch (state->WriteMode & VIDEO_GC_WRITE_MODE_MASK) {
    case VIDEO_WRITE_MODE_1:                                       /* copy latches -> planes        */
        for (plane = 0; plane < VIDEO_PLANES; ++plane) if (state->MapMask & (1<<plane)) VideoPlaneBytes(state,plane)[offset] = state->Latch[plane];
        return;
    case VIDEO_WRITE_MODE_2:                                       /* CPU bit p -> plane p           */
        for (plane = 0; plane < VIDEO_PLANES; ++plane) {
            BYTE byteValue = (BYTE)((cpu & (1<<plane)) ? VIDEO_FILL_SET : 0x00);
            BYTE registers = VideoAlu(alu, byteValue, state->Latch[plane]);
            registers = (BYTE)((registers & bitMask) | (state->Latch[plane] & (BYTE)~bitMask));
            if (state->MapMask & (1<<plane)) VideoPlaneBytes(state,plane)[offset] = registers;
        }
        return;
    case VIDEO_WRITE_MODE_3: {                                     /* set/reset masked by rot(cpu)&bm */
        BYTE data = VideoRotateRight(cpu, state->FunctionRotate), mask = (BYTE)(data & bitMask);
        for (plane = 0; plane < VIDEO_PLANES; ++plane) {
            BYTE byteValue = (BYTE)((state->SetReset & (1<<plane)) ? VIDEO_FILL_SET : 0x00);
            BYTE registers = (BYTE)((byteValue & mask) | (state->Latch[plane] & (BYTE)~mask));
            if (state->MapMask & (1<<plane)) VideoPlaneBytes(state,plane)[offset] = registers;
        }
        return; }
    default: {                                    /* write mode 0                   */
        BYTE data = VideoRotateRight(cpu, state->FunctionRotate);
        state->WriteEnableSetResetHistogram[state->EnableSetReset & VIDEO_ALL_PLANES]++;
        state->WriteAluHistogram[alu & VIDEO_GC_ALU_MASK]++;
        for (plane = 0; plane < VIDEO_PLANES; ++plane) {
            BYTE byteValue = (state->EnableSetReset & (1<<plane)) ? (BYTE)((state->SetReset & (1<<plane)) ? VIDEO_FILL_SET : 0x00) : data;
            BYTE registers = VideoAlu(alu, byteValue, state->Latch[plane]);
            registers = (BYTE)((registers & bitMask) | (state->Latch[plane] & (BYTE)~bitMask));
            if (plane == VIDEO_PLANE_3 && (state->MapMask & VIDEO_PLANE3_BIT)) {
                if (state->EnableSetReset & VIDEO_PLANE3_BIT) { state->WritePlane3SetReset++; if (registers) state->WritePlane3NonZero++; }
                else                     state->WritePlane3Data++;
            }
            if (state->MapMask & (1<<plane)) VideoPlaneBytes(state,plane)[offset] = registers;
        }
        return; }
    }
}

/* ── THE CACHE WITNESS. A linear scan over ten slots, entered only for accesses
     above VIDEO_CACHE_LOW, so its cost falls on nothing that draws the screen. It is
     deliberately NOT a hash: the whole point is that "this pc never appeared" must
     mean the pc never ran, and a hashed table cannot say that. See vdd_video.h. */
static VOID VideoCacheSiteNote(PVIDEO_STATE state, UINT32 offset, INT isWrite)
{
    UINT32 guestPc, index;
    if (offset < VIDEO_CACHE_LOW || !state->GuestPc) return;
    guestPc = state->GuestPc();
    state->CacheSequence++;
    for (index = 0; index < VIDEO_CACHE_SITES; ++index) {
        PVIDEO_CACHE_SITE site = &state->CacheSites[index];
        if (site->Count) {
            if (site->Pc != guestPc || site->IsWrite != (BYTE)isWrite) continue;
            if (offset < site->Low) site->Low = offset;
            if (offset > site->High) site->High = offset;
        } else {
            site->Pc = guestPc; site->IsWrite = (BYTE)isWrite; site->Low = site->High = offset;
            site->First = state->CacheSequence;
        }
        site->Count++; site->Last = state->CacheSequence;
        return;
    }
    state->CacheSitesLost++;
}

/* The watchpoint wrapper. The engine above is left exactly as it was so that the
   instrument cannot change what it measures; this only records around it. */
VOID VddVideoPlanarWrite(PVIDEO_STATE state, UINT32 offset, BYTE cpu)
{
    VIDEO_WATCH_RECORD registers; INT plane;
    if (offset > state->PlanarHighWater) state->PlanarHighWater = offset;
    if (state->GuestPc) {
        UINT32 guestPc = state->GuestPc();
        PVIDEO_SITE site = &state->WriteSites[VIDEO_SITE_HASH(guestPc)];
        if (!site->Count)            { site->Pc = guestPc; site->Low = site->High = offset; site->Count = 1; }
        else if (site->Pc == guestPc) { if (offset < site->Low) site->Low = offset;
                                if (offset > site->High) site->High = offset; site->Count++; }
        else                  state->WriteSitesLost++;
    }
    VideoCacheSiteNote(state, offset, 1);
    if (offset != state->WatchOffset) { VideoPlanarWrite1(state, offset, cpu); return; }
    registers.Pc = state->GuestPc ? state->GuestPc() : 0;
    registers.WriteMode = (BYTE)(state->WriteMode & VIDEO_GC_WRITE_MODE_MASK); registers.MapMask = state->MapMask;
    registers.EnableSetReset = state->EnableSetReset; registers.SetReset = state->SetReset;
    registers.FunctionRotate = state->FunctionRotate; registers.BitMask = state->BitMask; registers.Cpu = cpu;
    for (plane = 0; plane < VIDEO_PLANES; ++plane) registers.Latch[plane] = state->Latch[plane];
    VideoPlanarWrite1(state, offset, cpu);
    for (plane = 0; plane < VIDEO_PLANES; ++plane)
        registers.After[plane] = (offset < VIDEO_PLANE_SIZE) ? VideoPlaneBytes(state,plane)[offset] : 0;
    if (state->WatchCount < VIDEO_WATCH_MAX) state->Watch[state->WatchCount] = registers;
    state->WatchLast = registers;
    state->WatchCount++;
}

BYTE VddVideoPlanarRead(PVIDEO_STATE state, UINT32 offset)
{
    INT plane;
    if (offset > state->PlanarHighWater) state->PlanarHighWater = offset;
    if (state->GuestPc) {
        UINT32 guestPc = state->GuestPc();
        PVIDEO_SITE site = &state->ReadSites[VIDEO_SITE_HASH(guestPc)];
        if (!site->Count)            { site->Pc = guestPc; site->Low = site->High = offset; site->Count = 1; }
        else if (site->Pc == guestPc) { if (offset < site->Low) site->Low = offset;
                                if (offset > site->High) site->High = offset; site->Count++; }
        else                  state->ReadSitesLost++;
        if (state->ReadMode & 1) {
            PVIDEO_SITE site = &state->CompareSites[VIDEO_SITE_HASH(guestPc)];
            if (!site->Count)            { site->Pc = guestPc; site->Low = site->High = offset; site->Count = 1; }
            else if (site->Pc == guestPc) { if (offset < site->Low) site->Low = offset;
                                    if (offset > site->High) site->High = offset; site->Count++; }
            else                  state->CompareSitesLost++;
        }
    }
    VideoCacheSiteNote(state, offset, 0);
    if (offset >= VIDEO_PLANE_SIZE) return VIDEO_FLOATING_BUS;
    for (plane = 0; plane < VIDEO_PLANES; ++plane) state->Latch[plane] = VideoPlaneBytes(state,plane)[offset];   /* load latches    */
    state->ReadModeHistogram[state->ReadMode & 1]++;
    if (!(state->ReadMode & 1))
        return VideoPlaneBytes(state,state->ReadMap & VIDEO_GC_READ_MAP_MASK)[offset];                /* read mode 0     */
    /* ── READ MODE 1: COLOUR COMPARE. One bit per pixel, set where that pixel's
         colour matches GR2 in every plane GR7 selects. GR7 is "Color DON'T Care" and
         reads backwards: a SET bit means the plane DOES take part. With GR7 = 0 no
         plane is compared, so every pixel matches and the read is 0xFF -- which is the
         hardware's answer, not a failure, and worth not "fixing". */
    {   BYTE registers = 0; INT bit;
        for (bit = 0; bit < BITS_PER_BYTE; ++bit) {
            INT match = 1;
            for (plane = 0; plane < VIDEO_PLANES; ++plane) {
                if (!((state->ColorDontCare >> plane) & 1)) continue;
                if (((state->Latch[plane] >> bit) & 1) != ((state->ColorCompare >> plane) & 1)) { match = 0; break; }
            }
            if (match) registers = (BYTE)(registers | (1 << bit));
        }
        /* Record what we ANSWERED, not just that we were asked -- see vdd_video.h. */
        if (state->GuestPc) {
            UINT32 comparePc = state->GuestPc();
            UINT height = VIDEO_SITE_HASH(comparePc);
            if (state->CompareSites[height].Pc == comparePc) {
                if (!registers)            state->CompareSitesZero[height]++;
                else if (registers == VIDEO_COMPARE_ALL) state->CompareSitesOnes[height]++;
            }
        }
        return registers; }
}

INT VddVideoIsPlanarActive(PCVIDEO_STATE state) { return state->ModeKind == VIDEO_KIND_PLANAR; }

/* CRT timings, shared by the 0x3DA status read and the present scheduler. */
#define VIDEO_VBL_HZ_HIGH     60        /* 640x480 modes                                */
#define VIDEO_VBL_HZ_LOW     70        /* 320x200 / 720x400 modes                      */
#define VIDEO_VTOTAL_HIGH    525        /* scanlines per frame incl. blanking, 480-line */
#define VIDEO_VTOTAL_LOW    449        /*                                    400-line  */
#define VIDEO_VACTIVE_HIGH   480
#define VIDEO_VACTIVE_LOW   400
#define VIDEO_HACTIVE_PERCENT   80        /* % of a scanline that is active (rest = hblank) */

/* See the header. Phase within the frame, in permille, against the point where the
   active picture ends (480/525 = 914, 400/449 = 891). The window is the last ~12%
   of the active period: late enough that a guest released by the PREVIOUS retrace
   has finished its drawing, early enough to be a distinct instant every frame. */
#define VIDEO_PRESENT_WINDOW_PER_MILLE 120
/* A poll this long after the previous one means the guest went away to draw. Its own
   draw is 1-4 ms (BOUNCEBX 2.4); a poll loop iterates in well under 50 us. */
#define VIDEO_PRESENT_GAP_US    400
static INT VideoVerticalTiming(PCVIDEO_STATE state, UINT32 *total, UINT32 *active,
                       UINT32 *blankStart);
/* The mode's frame period in microseconds -- the same 60/70 Hz choice the retrace
   model makes -- for the host's Auto fallback floor. 1/60 s when there is no clock. */
UINT32 VddVideoFrameUs(PCVIDEO_STATE state)
{
    UINT32 verticalTotal, verticalDisplay, verticalBlank, hz; INT isTall = (state->GraphicsHeight > VIDEO_VACTIVE_LOW);
    if (VideoVesaGeometry(state, &verticalTotal, &verticalDisplay, &verticalBlank, &hz)) return MICROSECONDS_PER_SECOND_U / hz;   /* #226: the VESA mode's */
    if (VideoVerticalTiming(state, &verticalTotal, &verticalDisplay, &verticalBlank)) isTall = (verticalTotal >= VIDEO_TALL_FRAME_LINES);
    return MICROSECONDS_PER_SECOND_U / (UINT32)(isTall ? VIDEO_VBL_HZ_HIGH : VIDEO_VBL_HZ_LOW);
}
INT VddVideoIsPresentReady(PVIDEO_STATE state)
{
    UINT32 frameUs, perMille, verticalTotal, verticalDisplay, verticalBlank, hz;
    INT presentPerMille, isTall;
    if (!state->TimeUs) return 1;                 /* no clock: present every tick   */
    if (VideoVesaGeometry(state, &verticalTotal, &verticalDisplay, &verticalBlank, &hz)) {      /* #226: the VESA mode's own frame */
        frameUs = MICROSECONDS_PER_SECOND_U / hz; presentPerMille = (INT)(verticalDisplay * VIDEO_PER_MILLE / verticalTotal);
        perMille  = (UINT32)((state->TimeUs() % frameUs) * VIDEO_PER_MILLE / frameUs);
        return (INT)perMille >= presentPerMille - VIDEO_PRESENT_WINDOW_PER_MILLE && (INT)perMille < presentPerMille;
    }
    /* Same geometry the 0x3DA read uses, and for the same reason: 914/891 permille
       are the two BIOS cases, and 640x350 is neither -- its picture ends at 350 of
       449 lines, 780 permille. Presenting at 891 there meant building the frame 1.6ms
       into the blanking interval rather than at the end of the picture. */
    isTall = (state->GraphicsHeight > VIDEO_VACTIVE_LOW);
    presentPerMille  = isTall ? VIDEO_PRESENT_PER_MILLE_TALL : VIDEO_PRESENT_PER_MILLE_SHORT;
    if (VideoVerticalTiming(state, &verticalTotal, &verticalDisplay, &verticalBlank)) { isTall = (verticalTotal >= VIDEO_TALL_FRAME_LINES); presentPerMille = (INT)(verticalDisplay * VIDEO_PER_MILLE / verticalTotal); }
    frameUs = MICROSECONDS_PER_SECOND_U / (UINT32)(isTall ? VIDEO_VBL_HZ_HIGH : VIDEO_VBL_HZ_LOW);
    if (!frameUs) return 1;
    perMille  = (UINT32)((state->TimeUs() % frameUs) * VIDEO_PER_MILLE / frameUs);
    return (INT)perMille >= presentPerMille - VIDEO_PRESENT_WINDOW_PER_MILLE && (INT)perMille < presentPerMille;
}

/* Sequencer ports 3C4 (index) / 3C5 (data) -- Map Mask (SR2). */
/* ── ATTRIBUTE ONLY THE BYTES THAT CHANGED, NOT THE WHOLE APERTURE. ─────────────────
     The A0000 aperture is one flat buffer -- the page trap is deliberately not armed,
     because arming it makes the interpreter the CPU and collapses the run -- so a guest
     write lands there with no record of which plane the map mask had selected. This
     used to copy the ENTIRE aperture into the outgoing plane on every mask change, on
     the assumption that an unchained program fills one whole plane before moving to the
     next.

     Doom does not. It updates in dirty boxes: measured, the map mask changes about 516
     times per frame, four planes x ~129 boxes, each write touching a few columns. So
     every "snapshot" copied 16000 bytes of which only a handful belonged to that plane,
     and the other 15,900-odd were whatever the PREVIOUS plane had left behind. All four
     planes therefore converged on the same picture -- measured, they ended a run
     reporting an identical 48,031 non-zero bytes -- and the frame came out with every
     even column equal to the one after it. On screen that reads as Doom at half
     horizontal resolution, which is not a thing Doom can do: its own low-detail mode
     leaves the status bar alone, and the doubling was in the status bar too.

     The aperture does carry the information, just not in its addresses: a byte that
     CHANGED since the last flush was written under the mask that is now going out.
     So keep a shadow of the aperture and attribute the differences. That is exact
     without a page trap, and it is not more expensive than the copy it replaces --
     comparing dwords, an untouched region costs a quarter of the reads a copy did. */
/* ── DE-INTERLEAVE MODE Y BY ATTRIBUTING EACH CHANGED RUN TO THE SELECTED PLANE. ────
     A0000 is one flat buffer -- the page trap is deliberately not armed, because arming
     it makes the interpreter the CPU and collapses the run -- so a guest write lands
     there carrying no record of which plane the map mask had selected. It has to be
     recovered afterwards, from a shadow of the aperture.

   ► WHAT THE USER'S PLAY SESSION HANDED US, AND IT IS THE WHOLE DESIGN. "The intro
     screen is 320x200 until the menu shows, then it degrades... I played through the
     first level and got to the score screen, which went back to correct 320x200, and
     then degraded again on the next level."
     Title and intermission are FULL-SCREEN blits: one mask change per plane, the whole
     plane written under it. Menu, demo and gameplay are DIRTY-BOX updates: ~516 mask
     changes a frame, a few columns each. So the rule that copies the WHOLE aperture
     into the outgoing plane is exactly right for the first and exactly wrong for the
     second -- which is precisely the split seen on the screen, and confirms the model.

   ► THE RULE: copy the aperture over the CHANGED EXTENT WITHIN EACH GRANULE. A whole-
     plane write changes every granule end to end, so the copy is the whole plane and
     the full-screen case stays exact. A box changes only the granules its columns fall
     in, and only the span within them, so the rest of each plane keeps its own data.
     Sizing the granule is the entire trick, and each wrong answer was measured on
     captured frames (even-column match: 0.08 is a real 320-wide picture, 1.000 is
     every even column equal to the next, i.e. half horizontal resolution):

       whole aperture      1.000  correct only for full-screen writers
       changed bytes       0.08   full resolution but STREAKED -- a byte rewritten with
                                  the value it already held is still a write and the
                                  shadow cannot see it, and Doom's textures are full of
                                  equal neighbours
       changed span (all)  0.90   one global min..max spans nearly the whole page
       whole 16B granules  0.78   copying the WHOLE granule over-attributes: a plane
                                  byte is FOUR screen pixels wide, so 16 bytes span 64

     Per-granule min..max is the one that is tight in both directions: it never copies
     beyond the outermost change in a granule, and it carries the same-valued bytes
     between two changes, which is what kills the streaks.
   ⚠ Do not change the rule or the granule without measuring even-column match on a
     captured frame. Four plausible variants have already made it worse. */
/* A guest store to the aperture is a CONTIGUOUS RUN of plane bytes -- a row segment of
   whatever box is being updated. Find those runs in the diff and copy each one whole.
   Bytes inside a run that happen to be unchanged (the same value written again, which
   the shadow cannot see) come along with it; bytes outside stay with their own plane.
   MODEY_GAP is how many unchanged dwords may sit inside one run before it is treated as
   two: it is the only tuning constant here, and it trades streaks (too small) against
   over-attribution (too large). */
/* ► IT IS A KNOB, AND THE HUMAN IS THE INSTRUMENT. There is no good point on this
     curve -- six rules have been measured and every one trades resolution against
     stale streaks -- so the value is read from `modey.txt` on the share rather than
     compiled in, and a play session can walk it without a rebuild. Measured
     even-column match on Doom's 3D view (0.08 = a real 320-wide picture, 1.000 = every
     even column equal to the next):
         gap 0     tightest, most detail, most streaking
         gap 2     ~0.55, the shipped default
         gap huge  1.000, the old whole-aperture behaviour: coherent, half resolution
     A run of guest stores is contiguous, so this is how many unchanged dwords may sit
     inside one before it is treated as two. */
#define VIDEO_MODEY_GAP_DEFAULT 2u

static VOID VideoModeYCopy(PVIDEO_STATE state, const INT *selected, INT selectedCount, UINT32 low, UINT32 high)
{
    UINT32 index;
    for (index = low; index < high; ++index) {
        BYTE byteValue = state->VideoMemory[index];
        INT selectedIndex;
        state->YShadow[index] = byteValue;
        for (selectedIndex = 0; selectedIndex < selectedCount; ++selectedIndex) state->YPlanes[selected[selectedIndex]][index] = byteValue;
    }
    state->YNonZero[0] += high - low;                              /* bytes attributed, for STAGE2 */
}

static VOID VideoModeYFlush(PVIDEO_STATE state)
{
    UINT32 index, runLow = 0, runHigh = 0, gap = 0;
    const UINT32 *memoryWords, *shadowWords;
    INT plane, selected[VIDEO_PLANES], selectedCount = 0, isInRun = 0;
    if (state->IsChain4 || state->ModeKind != VIDEO_KIND_LINEAR8 || !state->VideoMemory) return;
    for (plane = 0; plane < VIDEO_PLANES; ++plane) if (state->YMask & (1u << plane)) selected[selectedCount++] = plane;
    if (!selectedCount) return;
    memoryWords = (const UINT32 *)state->VideoMemory;
    shadowWords = (const UINT32 *)state->YShadow;

    /* Dword-at-a-time scan. Most of the aperture is untouched between two adjacent
       mask changes -- and in Doom there are ~3,800 of those a second -- so the reject
       path is the one that has to be cheap. */
    for (index = 0; index < VIDEO_Y_PLANE_SIZE / VIDEO_DWORD_BYTES; ++index) {
        if (memoryWords[index] != shadowWords[index]) {
            if (!isInRun) { isInRun = 1; runLow = index; }
            runHigh = index + 1; gap = 0;
        } else if (isInRun && ++gap > state->ModeYGap) {
            VideoModeYCopy(state, selected, selectedCount, runLow * VIDEO_DWORD_BYTES, runHigh * VIDEO_DWORD_BYTES);
            isInRun = 0;
        }
    }
    if (isInRun) VideoModeYCopy(state, selected, selectedCount, runLow * VIDEO_DWORD_BYTES, runHigh * VIDEO_DWORD_BYTES);
    state->IsDirty = 1;
}

/* CRTC: only the registers unchained page-flipping needs. 0x0C/0x0D are the
   display START address (how a mode-Y program flips pages) and 0x13 the logical
   line width. Everything else is accepted and ignored -- this VDD does not model
   CRTC timing and pretending to would be worse than not. */
static VOID VideoIndexData(BYTE *index, BYTE width, UINT32 value,
                         VOID (*setData)(PVOID, UINT32), PVOID context)
{
    *index = (BYTE)value;
    if (width == VIDEO_PORT_WIDTH_WORD) setData(context, (value >> BYTE_SHIFT) & BYTE_MASK);
}

/* ── ★★★ A MODE SET PROGRAMS THE REGISTER FILE, AS A REAL BIOS DOES. ────────────────
     (docs/inventory/vga.md step 3.) Measured on genuine MS-DOS 6.22: a BIOS mode set
     leaves about SIXTY meaningful values across the Sequencer, CRTC, Graphics
     Controller, Attribute Controller and Miscellaneous Output. Ours left about five,
     so a guest that reads a register back to learn the geometry -- or saves and
     restores the card around its own mode switch, which plenty of DOS programs do --
     was told zero.
   ⚠ THIS FILLS THE FILE; IT DOES NOT YET DRIVE THE PICTURE. Every derived field
     (mkind, gw/gh, chain4, CrtcOffset, MapMask ...) is still computed exactly as
     before and remains the authority for rendering. Moving the authority here is the
     NEXT step and is a separate commit, because it touches every rendering path.
   ⚠ ONLY MEASURED MODES. A mode absent from g_VgaModeDefinitions is left exactly as it was
     rather than filled with a guess -- extend p_vgareg.asm and regenerate.
   ⚠ THE WRITE COUNTS ARE NOT TOUCHED. `*_w[i]` means "the GUEST wrote this index",
     and it is the evidence the inventory is built from; a BIOS load must not forge it.
     After this, `VGAREG` values are the BIOS's and the written-by-guest list is still
     only the guest's. */
static VOID VideoLoadModeDefinition(PVIDEO_STATE state, BYTE mode)
{
    INT index, definitionIndex;
    for (definitionIndex = 0; definitionIndex < VGA_MODEDEF_COUNT; ++definitionIndex) {
        PCVGA_MODEDEF definition = &g_VgaModeDefinitions[definitionIndex];
        if (definition->Mode != mode) continue;
        state->IsGeometryRegistersOk = 1;                      /* #325: the file IS this mode */
        state->MiscOutput = definition->MiscOutput;
        for (index = 0; index < VGA_MODEDEF_SEQUENCER;  ++index) state->SequencerRegisters[index]  = definition->Sequencer[index];
        for (index = 0; index < VGA_MODEDEF_CRTC; ++index) state->CrtcRegisters[index] = definition->Crtc[index];
        for (index = 0; index < VGA_MODEDEF_GC;  ++index) state->GcRegisters[index]   = definition->Graphics[index];
        for (index = 0; index < VGA_MODEDEF_ATTRIBUTE; ++index) state->AttributeRegisters[index] = definition->Attribute[index];
        /* ── ★★★★ AND THE LIVE SHADOWS, OR THE FILE AND THE AUTHORITY DISAGREE. ─────
             Loading the register FILE above is only half a mode set. Six of these
             registers are not read back from `*_reg[]` at all -- the read paths
             answer from a live shadow, because that shadow is what the rendering
             engine actually uses -- so the table set the file and the guest still
             saw the old value. MEASURED, on the rig and reproduced off-VM: after
             INT 10h mode 3 we answered SR2=0F (want 03), CR0A/0B=06/07 (want
             0D/0E), GR5=00 (want 10), AR10=00 (want 0C). Five registers, 55 of the
             VGA parity gap's 55 bytes, ONE defect.
           ⚠ ORDER MATTERS AND IT IS DELIBERATE. This runs BEFORE the per-kind arms
             below, so a mode that really does need its own value -- LINEAR8 and
             PLANAR both force MapMask 0x0F and drive YMapSelect -- still wins.
             What changes is only the modes those arms say nothing about.
           ⚠ GR7 IS THE ONE THE ORACLES SPLIT ON, and the table already carries the
             answer: both say 0x0F in the graphics modes, and in the text/CGA modes
             QEMU says 0x0F where PCem's real IBM VGA says 0x00. Taking the table
             rather than a constant is what keeps that distinction. */
        state->MapMask     = (BYTE)(definition->Sequencer[VIDEO_SR_MAP_MASK] & VIDEO_ALL_PLANES);
        state->YMask       = state->MapMask;
        /* CR0A/CR0B ARE the cursor shape; `CursorShape` is that pair, not a copy of
           it. The BIOS leaves 0x0D0E for an 8x16 cell, and the 0x0607 set above is
           the 8-line CGA shape -- which VddCursorLines then RESCALES to lines
           14-15. So this moves the drawn cursor by one scan line, and stops
           AH=03h reporting a shape the card does not hold. */
        state->CursorShape    = (WORD)(((WORD)definition->Crtc[VIDEO_CR_CURSOR_START] << BYTE_SHIFT) | definition->Crtc[VIDEO_CR_CURSOR_END]);
        state->WriteMode   = (BYTE)(definition->Graphics[VIDEO_GR_MODE] & VIDEO_GC_WRITE_MODE_MASK);
        state->ReadMode    = (BYTE)((definition->Graphics[VIDEO_GR_MODE] >> VIDEO_GR5_READ_MODE_SHIFT) & 1);
        state->ColorDontCare = (BYTE)(definition->Graphics[VIDEO_GR_COLOR_DONT_CARE] & VIDEO_ALL_PLANES);
        state->AttributeMode    = definition->Attribute[VIDEO_AR_MODE];
        state->IsBlink        = (BYTE)((definition->Attribute[VIDEO_AR_MODE] >> VIDEO_AR_BLINK_SHIFT) & 1);
        return;
    }
}

/* ── #266: THE VIDEO PARAMETER TABLE, FROM THE SAME MEASURED REGISTER SETS THE MODE SET
     LOADS. A VGA BIOS programs a mode FROM this table, and publishes it through
     0040:00A8 -> Save Pointer table -> first far pointer; text utilities and mode
     switchers read the register values out of it rather than out of the card. So
     each entry here is built from g_VgaModeDefinitions -- the bytes VideoLoadModeDefinition puts in
     the register file -- and can never disagree with what the mode set leaves.
     The 29 slots are IBM's (RBIL "Video Parameter Table"): 00h-03h modes 0-3 at 200
     lines, 04h-07h, 08h-0Ch PCjr/reserved, 0Dh/0Eh, 0Fh/10h modes 0Fh/10h on a 64 KB
     card, 11h/12h the same on 256 KB, 13h-16h modes 0-3 at 350 lines, 17h modes 0+/1+,
     18h 2+/3+, 19h 7+ (the 400-line 9-dot text modes), 1Ah-1Ch modes 11h-13h.
   ⚠ WHICH SLOTS ARE FILLED is SeaVGABIOS's choice (stdvga_build_video_param: 04h-07h,
     0Dh, 0Eh, 11h, 12h, 17h-1Ch), less 11h (mode 0Fh: we hold no measured set for it).
     The 200- and 350-line text slots and the 64 KB slots are ZERO, as there: we have no
     measurement of those register sets, and inventing them is the thing not to do.
     IBM's ROM fills them -- p_vid266 dumps slots 03h and 18h so the lead can see what
     a naive `mode * 64` lookup finds there on the real ROM. */
static const BYTE VideoParameterMode[VDD_VPARAM_N] = {
    0, 0, 0, 0, 0x04, 0x05, 0x06, 0x07,  0, 0, 0, 0, 0, 0x0D, 0x0E, 0,
    0, 0, 0x10, 0, 0, 0, 0, 0x01,  0x03, 0x07, 0x11, 0x12, 0x13
};
INT VddVideoParameterEntry(BYTE tableIndex, BYTE entry[VIDEO_PARAMETER_ENTRY_BYTES])
{
    INT index, definitionIndex;
    UINT modeIndex;
    BYTE mode;
    for (index = 0; index < VIDEO_PARAMETER_ENTRY_BYTES; ++index) entry[index] = 0;
    if (tableIndex >= VDD_VPARAM_N || !(mode = VideoParameterMode[tableIndex])) return 0;
    for (definitionIndex = 0; definitionIndex < VGA_MODEDEF_COUNT; ++definitionIndex) {
        PCVGA_MODEDEF definition = &g_VgaModeDefinitions[definitionIndex];
        if (definition->Mode != mode) continue;
        for (modeIndex = 0; modeIndex < sizeof(g_VideoModes)/sizeof(g_VideoModes[0]); ++modeIndex)
            if (g_VideoModes[modeIndex].Mode == mode) break;
        if (modeIndex == sizeof(g_VideoModes)/sizeof(g_VideoModes[0])) return 0;
        {   UINT pageSize = VideoModePageSize(mode, g_VideoModes[modeIndex].Kind,
                                         g_VideoModes[modeIndex].Columns, g_VideoModes[modeIndex].Rows);
            entry[VIDEO_PARAMETER_COLUMNS] = g_VideoModes[modeIndex].Columns;
            entry[VIDEO_PARAMETER_ROWS] = (BYTE)(g_VideoModes[modeIndex].Rows - 1);
            entry[VIDEO_PARAMETER_CHARACTER_HEIGHT] = VideoModeCellHeight(mode);
            entry[VIDEO_PARAMETER_PAGE_SIZE] = (BYTE)pageSize; entry[VIDEO_PARAMETER_PAGE_SIZE + 1] = (BYTE)(pageSize >> BYTE_SHIFT); }
        for (index = 0; index < VIDEO_PARAMETER_SEQUENCER_COUNT;  ++index) entry[VIDEO_PARAMETER_SEQUENCER + index] = definition->Sequencer[1 + index];     /* SR1-SR4 */
        entry[VIDEO_PARAMETER_MISC] = definition->MiscOutput;
        for (index = 0; index < VGA_MODEDEF_CRTC; ++index) entry[VIDEO_PARAMETER_CRTC + index] = definition->Crtc[index];
        for (index = 0; index < VIDEO_PARAMETER_ATTRIBUTE_COUNT; ++index) entry[VIDEO_PARAMETER_ATTRIBUTE + index] = definition->Attribute[index];        /* AR00-AR13 */
        for (index = 0; index < VGA_MODEDEF_GC;  ++index) entry[VIDEO_PARAMETER_GC + index] = definition->Graphics[index];
        return 1;
    }
    return 0;
}

static VOID VideoCrtcSetData(PVOID context, UINT32 value);

/* The cursor's CRTC address (0x0E/0x0F) as the BIOS path implies it: cells from the
   start of video memory, so the display start is added back in. */
static WORD VideoCrtcCursorOf(PCVIDEO_STATE state)
{ return (WORD)(state->CrtcStart + (UINT)state->CursorRow * state->Columns + state->CursorColumn); }
/* ...and the reverse: a guest wrote 0x0E/0x0F, so derive row/col from it. A value
   off the visible page (some guests park the cursor at 0x7FFF to hide it) is left
   where it is -- hidden by being out of range, as it is on the card. */
static VOID VideoCrtcCursorApply(PVIDEO_STATE state)
{
    UINT position = state->CrtcCursor;
    UINT base = state->CrtcStart;
    state->IsDirty = 1;
    if (!state->Columns || !state->Rows) return;
    if (position < base) { state->CursorRow = state->Rows; return; }          /* off-page: hidden  */
    position -= base;
    if (position >= (UINT)state->Columns * state->Rows) { state->CursorRow = state->Rows; return; }
    state->CursorRow = (BYTE)(position / state->Columns);
    state->CursorColumn = (BYTE)(position % state->Columns);
    VddVideoBdaSync(state);
}

/* Reassemble the ten-bit Line Compare from its three registers. */
static VOID VideoCrtcLineCompareUpdate(PVIDEO_STATE state)
{
    state->CrtcLineCompare = (WORD)(state->CrtcLineCompareLow
                            | (((WORD)(state->CrtcOverflow >> VIDEO_CR07_LINE_COMPARE_8) & 1u) << VIDEO_BIT8_SHIFT)
                            | (((WORD)(state->CrtcMaxScan  >> VIDEO_CR09_LINE_COMPARE_9) & 1u) << VIDEO_BIT9_SHIFT));
}

/* A mode set writes 0x06, 0x12 and 0x15 among the rest; only once all three have
   arrived is the vertical timing a complete statement rather than one register of
   the old mode's geometry beside two of the new one's. */
/* ── ▶ DERIVED ONCE PER CRTC WRITE, NOT ONCE PER 0x3DA READ. ──────────────────────
     The vertical geometry is a function of five registers. Recomputing it inside
     VideoStatusIn meant reassembling three 10-bit values out of scattered bits, running
     three validity tests and a division ON EVERY POLL -- and a guest polls this port
     harder than it does anything else: Lemmings reads 0x3DA 73.8 MILLION times in a
     45-second run, 1.6M/s, and in its menu phase it does essentially nothing else.
     MEASURED: that put the per-poll cost up from 16.0ns to 20.0ns off-VM, and on the
     rig the guest got through 77.0M polls per run before and 73.6M after -- 4.4% fewer
     in the same wall time, paid by every guest that waits on retrace.
     The inputs change only when the guest writes the CRTC, so the answer is cached
     there and the hot path just reads it. Same numbers, none of the arithmetic. */
static VOID VideoCrtcVerticalTimingRecompute(PVIDEO_STATE state)
{
    UINT32 overflow, maxScan, verticalTotal, verticalDisplayEnd, verticalBlankStart;
    state->IsVerticalTimingValid = 0;
    if (!state->IsCrtcVerticalTimingSeen) return;
    overflow = state->CrtcOverflow; maxScan = state->CrtcMaxScan;
    verticalTotal  = (UINT32)state->CrtcVerticalTotalLow | ((overflow >> VIDEO_CR07_VERTICAL_TOTAL_8 & 1u) << VIDEO_BIT8_SHIFT) | ((overflow >> VIDEO_CR07_VERTICAL_TOTAL_9 & 1u) << VIDEO_BIT9_SHIFT);
    verticalDisplayEnd = (UINT32)state->CrtcVerticalDisplayEndLow    | ((overflow >> VIDEO_CR07_DISPLAY_END_8 & 1u) << VIDEO_BIT8_SHIFT) | ((overflow >> VIDEO_CR07_DISPLAY_END_9 & 1u) << VIDEO_BIT9_SHIFT);
    verticalBlankStart = (UINT32)state->CrtcVerticalBlankStartLow    | ((overflow >> VIDEO_CR07_BLANK_START_8 & 1u) << VIDEO_BIT8_SHIFT) | ((maxScan >> VIDEO_CR09_BLANK_START_9 & 1u) << VIDEO_BIT9_SHIFT);
    verticalTotal += VIDEO_CR_VERTICAL_TOTAL_BIAS; verticalDisplayEnd += 1;
    if (verticalTotal < VIDEO_VERTICAL_TOTAL_MIN || verticalTotal > VIDEO_VERTICAL_TOTAL_MAX) return;
    if (verticalDisplayEnd == 0u || verticalDisplayEnd > verticalTotal)   return;
    if (verticalBlankStart < verticalDisplayEnd || verticalBlankStart >= verticalTotal)  return;
    state->VerticalTotal = (WORD)verticalTotal;
    state->VerticalActive = (WORD)verticalDisplayEnd;
    state->VerticalBlank  = (WORD)verticalBlankStart;
    state->IsVerticalTimingValid  = 1;
}

static VOID VideoCrtcVerticalTimingUpdate(PVIDEO_STATE state)
{
    if (state->CrtcVerticalTotalLow && state->CrtcVerticalDisplayEndLow && state->CrtcVerticalBlankStartLow) state->IsCrtcVerticalTimingSeen = 1;
    VideoCrtcVerticalTimingRecompute(state);
}

/* ── THE CRT'S VERTICAL GEOMETRY, FROM THE REGISTERS THE GUEST WROTE. ──────────────
     Each of the three quantities is a 10-bit value split across three registers: the
     low byte of its own, plus one bit in Overflow (0x07) and -- for Blank Start only
     -- one in Maximum Scan Line (0x09). That scattering is why it is worth composing
     in one place instead of at each use.

       Vertical Total        0x06 + ov bit0 + ov bit5   (+2 = scanlines per frame)
       Vertical Display End  0x12 + ov bit1 + ov bit6   (+1 = active scanlines)
       Vertical Blank Start  0x15 + ov bit3 + maxscan bit5

   ▶ WHY NOT KEEP THE TWO CONSTANTS. They were right for every mode the BIOS sets
     except 0Fh/10h, and wrong there by 45 lines -- checked against the per-mode CRTC
     tables in vga_defaults.h, which were read back off a real card. A guest that
     programs its own timing (Mode X and friends) was never covered at all.
   ▶ Returns 0 and touches nothing when the guest has not programmed the CRTC, or
     when what it programmed is not a plausible screen: the caller then keeps the old
     constants. An off-VM test that sets gh directly takes this path, which is why
     the existing battery is unaffected. */
/* Read back what VideoCrtcVerticalTimingRecompute() worked out. The validity rules -- a plausible
   screen: blanking after the picture and inside the frame -- live there, because a
   half-written mode set must be rejected ONCE, not re-rejected 73 million times. */
static INT VideoVerticalTiming(PCVIDEO_STATE state, UINT32 *total, UINT32 *active,
                       UINT32 *blankStart)
{
    if (!state->IsVerticalTimingValid) return 0;
    *total = state->VerticalTotal; *active = state->VerticalActive; *blankStart = state->VerticalBlank;
    return 1;
}

static VOID VideoCrtcOut(PVOID context, WORD port, BYTE width, UINT32 value)
{
    PVIDEO_STATE state = (PVIDEO_STATE)context;
    /* 3B4 is the SAME index port as 3D4 (Misc Output bit 0 picks which one decodes);
       both are claimed, so both must be recognised as the index half or a mono guest's
       index write would be taken as data. */
    if (port == VIDEO_PORT_CRTC_COLOUR || port == VIDEO_PORT_CRTC_MONO) { VideoIndexData(&state->CrtcIndex, width, value, VideoCrtcSetData, state); return; }
    VideoCrtcSetData(state, value);
}
/* ── #187: THE VERTICAL-RETRACE INTERRUPT LATCH, Input Status 0 bit 7. ──────────────
     IBM VGA: CR11 bit 5 = 0 ENABLES the vertical interrupt (active low), bit 4 = 0
     CLEARS it and holds it clear; with the interrupt enabled and not held, the start of
     vertical retrace sets the latch, which reads back as 3C2 bit 7. DOSBox-X does this;
     PCem's IBM VGA never sets the bit (p_vgaext is0.vsync) -- the spec outranks the
     oracle that leaves it out, so it is built.
   ⚠ THE LATCH ONLY. The card would also raise IRQ 2 (cascaded to 9), and we do NOT:
     that line is a jumper left open on most VGA cards, and every BIOS mode set writes
     CR11 with bit 5 = 0 -- the interrupt nominally ENABLED -- so raising it would fire an
     unexpected IRQ 9 into every graphics program. The BIOS modes also write bit 4 = 0,
     so an ordinary program still reads 0x10, as both oracles do.
   Evaluated lazily at the read, from the same beam clock as the 3DA status bits. */
static VOID VideoVintCr11(PVIDEO_STATE state, BYTE value)
{
    INT isEnabled = !(value & VIDEO_CR11_VINT_DISABLE), isHoldClear = !(value & VIDEO_CR11_VINT_CLEAR);
    if (isHoldClear) { state->IsVintPending = 0; state->IsVintArmed = 0; return; }
    if (!isEnabled)        { state->IsVintArmed = 0; return; }
    if (!state->IsVintArmed) {
        state->IsVintArmed = 1;
        state->VintArmTime = state->TimeUs ? state->TimeUs() : 0;
    }
}
static BYTE VideoVintStatus(PVIDEO_STATE state)
{
    UINT64 now, next, blankOffset;
    UINT32 frameUs, verticalTotal, verticalDisplay, verticalBlank, frameNumber, line;
    if (state->IsVintArmed && !state->IsVintPending) {
        if (!VideoBeam(state, &now, &frameUs, &verticalTotal, &verticalDisplay, &verticalBlank, &frameNumber, &line) || !frameUs || !verticalTotal) {
            state->IsVintPending = 1;                       /* no clock: a retrace has passed */
        } else {
            (VOID)verticalDisplay; (VOID)frameNumber; (VOID)line;
            blankOffset  = (UINT64)verticalBlank * frameUs / verticalTotal;             /* retrace start within a frame */
            next = (state->VintArmTime / frameUs) * frameUs + blankOffset;    /* this frame's retrace start   */
            if (next <= state->VintArmTime) next += frameUs;    /* ...already gone: the next one */
            if (now >= next) state->IsVintPending = 1;
        }
    }
    return state->IsVintPending ? VIDEO_STATUS0_VINT : 0x00;
}

static VOID VideoCrtcSetData(PVOID context, UINT32 value)
{
    PVIDEO_STATE state = (PVIDEO_STATE)context;
    /* ── ⛔ CR11 BIT 7 WRITE-PROTECTS CR00..CR07, AND WE USED TO IGNORE IT. ─────────
         Measured against 6.22 by p_vgareg (`vga.cr11wp.protected.cr00`): with the bit
         set, hardware REFUSES a write to CR00 and the register still reads back 0x5F;
         we accepted it and read back 0x55. A guest that protects the timing registers
         and then writes them -- which is what the protect is FOR -- got a card that
         silently reprogrammed itself.
       ⚠ Refused means refused: not stored, not counted as a guest write, and not
         applied below. `CrtcWriteProtectRefused` counts them so the report can say it
         happened rather than leaving an absence to be interpreted. */
    if ((state->CrtcIndex & VIDEO_CR_INDEX_MASK) <= VIDEO_CR_OVERFLOW && (state->CrtcRegisters[VIDEO_CR_VERTICAL_RETRACE_END] & VIDEO_CR11_PROTECT)) {
        state->CrtcWriteProtectRefused++;
        return;
    }
    /* CAPTURE FIRST, always, whatever the switch below does with it -- an index that
       falls into `default:` is exactly the one the inventory needs to hear about. */
    state->CrtcRegisters[state->CrtcIndex & VIDEO_CR_INDEX_MASK] = (BYTE)value;
    state->CrtcWrites  [state->CrtcIndex & VIDEO_CR_INDEX_MASK]++;
    switch (state->CrtcIndex & VIDEO_CR_INDEX_MASK) {                 /* #325: a geometry register */
    case VIDEO_CR_HORIZONTAL_DISPLAY_END: case VIDEO_CR_OVERFLOW: case VIDEO_CR_MAX_SCAN: case VIDEO_CR_VERTICAL_DISPLAY_END: case VIDEO_CR_MODE_CONTROL: state->IsGeometryRegistersOk = 1; break;
    default: break;
    }
    if ((state->CrtcIndex & VIDEO_CR_INDEX_MASK) == VIDEO_CR_VERTICAL_RETRACE_END) VideoVintCr11(state, (BYTE)value);   /* #187 */
    switch (state->CrtcIndex) {
    /* ── THE START ADDRESS IS SIXTEEN BITS WRITTEN AS TWO REGISTERS, so between the
         two writes it holds a value the guest never asked for -- half of the old
         address and half of the new. Real hardware survives that because the address
         counter LOADS FROM THESE REGISTERS AT THE VERTICAL RETRACE, not continuously,
         and a guest that flips pages during retrace is therefore never seen torn.
         We rendered from them directly, so a frame built between the two writes
         showed a garbage address -- a whole-screen flicker on any guest that scrolls
         or page-flips, which is every scrolling game. CrtcStartHalf counts how
         often a frame was built mid-pair; CrtcStartLive is what the renderer uses. */
    case VIDEO_CR_START_HIGH: VideoLatch(state, 0);          /* boundaries passed BEFORE this write see the old value */
               state->CrtcStart = (WORD)((state->CrtcStart & BYTE_MASK) | ((WORD)(value & BYTE_MASK) << BYTE_SHIFT));
               state->IsCrtcSeen = 1; state->IsCrtcStartPending ^= 1; state->IsDirty = 1; break;
    case VIDEO_CR_START_LOW: VideoLatch(state, 0);
               state->CrtcStart = (WORD)((state->CrtcStart & HIGH_BYTE_MASK) | (value & BYTE_MASK));
               state->IsCrtcSeen = 1; state->IsCrtcStartPending ^= 1;
               if (!state->IsCrtcStartPending) {
                   state->CrtcStartWrites++;
                   if (state->TimeUs) {           /* pacing: frames since the last pair */
                       UINT64 now; UINT32 frameUs, verticalTotal, verticalDisplay, verticalBlank, frameNumber, line;
                       if (VideoBeam(state, &now, &frameUs, &verticalTotal, &verticalDisplay, &verticalBlank, &frameNumber, &line)) {
                           UINT32 gap = state->StartPreviousFrame ? frameNumber - state->StartPreviousFrame : 1u;
                           state->StartGapHistogram[gap < VIDEO_START_GAP_LAST ? gap : VIDEO_START_GAP_LAST]++;
                           state->StartPreviousFrame = frameNumber;
                       }
                   }
               }
               state->IsDirty = 1; break;
    case VIDEO_CR_OFFSET: state->CrtcOffset = (BYTE)value; state->IsCrtcOffsetSeen = 1;                                   state->IsDirty = 1; break;
    /* Line Compare, and the two registers that carry its top two bits. */
    case VIDEO_CR_OVERFLOW: state->CrtcOverflow = (BYTE)value; VideoCrtcLineCompareUpdate(state); VideoCrtcVerticalTimingUpdate(state); state->IsDirty = 1; break;
    case VIDEO_CR_MAX_SCAN: state->CrtcMaxScan  = (BYTE)value; VideoCrtcLineCompareUpdate(state); VideoCrtcVerticalTimingUpdate(state); state->IsDirty = 1; break;
    case VIDEO_CR_LINE_COMPARE: state->CrtcLineCompareLow   = (BYTE)value; VideoCrtcLineCompareUpdate(state); state->IsDirty = 1; break;
    /* Vertical timing -- see VideoVerticalTiming(). Low bytes only; 0x07/0x09 carry the
       high bits and are latched above for Line Compare already. */
    case VIDEO_CR_VERTICAL_TOTAL: state->CrtcVerticalTotalLow = (BYTE)value; VideoCrtcVerticalTimingUpdate(state); state->IsDirty = 1; break;
    case VIDEO_CR_VERTICAL_DISPLAY_END: state->CrtcVerticalDisplayEndLow    = (BYTE)value; VideoCrtcVerticalTimingUpdate(state); state->IsDirty = 1; break;
    case VIDEO_CR_VERTICAL_BLANK_START: state->CrtcVerticalBlankStartLow    = (BYTE)value; VideoCrtcVerticalTimingUpdate(state); state->IsDirty = 1; break;
    /* ── THE TEXT CURSOR, PROGRAMMED DIRECTLY. ────────────────────────────────────
         0x0A/0x0B are Cursor Start/End (the same CH/CL INT 10h AH=01h takes, bit 5
         of Start = off) and 0x0E/0x0F the cursor's address in character cells from
         the start of video memory. Full-screen editors and every CRT unit (Turbo
         Pascal's, QB's runtime) position the cursor this way instead of through the
         BIOS, and these registers fell into `default:` -- so the cursor sat wherever
         the last INT 10h left it, which in an editor is nowhere near the text. */
    case VIDEO_CR_CURSOR_START: state->CursorShape = (WORD)((state->CursorShape & BYTE_MASK) | ((WORD)(value & VIDEO_CR0A_CURSOR_START_MASK) << BYTE_SHIFT));
               state->IsDirty = 1; VddVideoBdaSync(state); break;
    case VIDEO_CR_CURSOR_END: state->CursorShape = (WORD)((state->CursorShape & HIGH_BYTE_MASK) | (value & VIDEO_CR0B_CURSOR_END_MASK));
               state->IsDirty = 1; VddVideoBdaSync(state); break;
    case VIDEO_CR_CURSOR_HIGH: state->CrtcCursor = (WORD)((state->CrtcCursor & BYTE_MASK) | ((WORD)(value & BYTE_MASK) << BYTE_SHIFT));
               VideoCrtcCursorApply(state); break;
    case VIDEO_CR_CURSOR_LOW: state->CrtcCursor = (WORD)((state->CrtcCursor & HIGH_BYTE_MASK) | (value & BYTE_MASK));
               VideoCrtcCursorApply(state); break;
    default: break;
    }
}
static VOID VideoCrtcIn(PVOID context, WORD port, BYTE width, UINT32 *value)
{
    PVIDEO_STATE state = (PVIDEO_STATE)context; (VOID)width;
    if (port == VIDEO_PORT_CRTC_COLOUR || port == VIDEO_PORT_CRTC_MONO) { *value = state->CrtcIndex; return; }
    switch (state->CrtcIndex) {
    case VIDEO_CR_START_HIGH: *value = (BYTE)(state->CrtcStart >> BYTE_SHIFT); break;
    case VIDEO_CR_START_LOW: *value = (BYTE)(state->CrtcStart & BYTE_MASK); break;
    case VIDEO_CR_OFFSET: *value = state->CrtcOffset; break;
    case VIDEO_CR_CURSOR_START: *value = (BYTE)((state->CursorShape >> BYTE_SHIFT) & VIDEO_CR0A_CURSOR_START_MASK); break;
    case VIDEO_CR_CURSOR_END: *value = (BYTE)(state->CursorShape & VIDEO_CR0B_CURSOR_END_MASK); break;
    /* Read back what the BIOS path set, in the hardware's own units -- in TEXT modes.
       ► In a graphics mode the BIOS never writes CR0E/CR0F (there is no hardware
         cursor to place), so they hold what the mode set loaded, or what the guest
         wrote: plain storage. Deriving them there leaked the text cursor into Mode X
         (`modeX.320x240` CR0F: hardware 0x00, ours 0xA0 -- the last 1/690 VGA parity
         byte; s81, #186). The text-mode cursor paths are untouched. */
    case VIDEO_CR_CURSOR_HIGH: *value = state->ModeKind == VIDEO_KIND_TEXT ? (BYTE)(VideoCrtcCursorOf(state) >> BYTE_SHIFT)
                                               : state->CrtcRegisters[VIDEO_CR_CURSOR_HIGH]; break;
    case VIDEO_CR_CURSOR_LOW: *value = state->ModeKind == VIDEO_KIND_TEXT ? (BYTE)(VideoCrtcCursorOf(state) & BYTE_MASK)
                                               : state->CrtcRegisters[VIDEO_CR_CURSOR_LOW]; break;
    default:   *value = state->CrtcRegisters[state->CrtcIndex & VIDEO_CR_INDEX_MASK]; break;  /* CR00-05/11/17 read back */
    }
}

/* ── A 16-BIT `OUT` TO A VGA INDEX PORT WRITES INDEX **AND** DATA. ──────────────────
     The index and data registers of the sequencer, the graphics controller and the CRTC
     are adjacent by design precisely so that one word OUT can set both -- `outpw(0x3C4,
     index | value<<8)` is the idiom every DOS graphics programmer uses, and Watcom
     compiles it to `mov eax,0x102 / out dx,ax`.
     These handlers ignored `w` and treated the whole word as an index, THROWING THE
     DATA BYTE AWAY. Doom's mode-Y frame blit selects each plane with exactly that
     instruction:
         19f8f:  mov edx,0x3c4 / mov eax,0x102 / out dx,ax    ; map mask := plane 0
     so the map mask never changed, the de-interleave saw one plane's bytes where four
     should have been, and every even screen column came out identical to the one after
     it -- measured at 1.000 across whole frames, status bar included. It reads as "Doom
     at half resolution", which is not a thing Doom can do: its low-detail mode leaves
     the status bar alone.
   ► THIS IS ALSO WHY CLAIMING THE CRTC REGRESSED DOOM THREE TIMES (sessions 19-20,
     "mechanism UNKNOWN"). Doom page-flips with `mov edx,0x3d4 / out dx,ax` -- the same
     idiom. Claiming 0x3D4 while dropping the data byte breaks the flip outright, which
     is strictly worse than not claiming it and inferring the page from the data. */
static VOID VideoSequencerSetData(PVOID context, UINT32 value);

static VOID VideoSequencerOut(PVOID context, WORD port, BYTE width, UINT32 value)
{
    PVIDEO_STATE state = (PVIDEO_STATE)context;
    if (port == VIDEO_PORT_SEQUENCER_INDEX) { VideoIndexData(&state->SequencerIndex, width, value, VideoSequencerSetData, state); return; }
    VideoSequencerSetData(state, value);
}
static VOID VideoSequencerSetData(PVOID context, UINT32 value)
{
    PVIDEO_STATE state = (PVIDEO_STATE)context;
    if ((state->SequencerIndex & VIDEO_SR_INDEX_MASK) == VIDEO_SR_CLOCKING_MODE && ((state->SequencerRegisters[VIDEO_SR_CLOCKING_MODE] ^ (BYTE)value) & VIDEO_SR1_SCREEN_OFF))
        state->IsDirty = 1;                             /* SR1.5 screen off/on: re-present (#266) */
    state->SequencerRegisters[state->SequencerIndex & VIDEO_SR_INDEX_MASK] = (BYTE)value;
    state->SequencerWrites  [state->SequencerIndex & VIDEO_SR_INDEX_MASK]++;
    if (state->SequencerIndex == VIDEO_SR_MAP_MASK) {
        /* Which map-mask values does this program actually use, and how often? The
           de-interleave is built entirely on the assumption that an unchained program
           selects ONE plane at a time and changes the mask between planes; nothing has
           ever checked that against a real one. A 16-entry histogram costs nothing and
           turns "the frame comes out doubled" into "plane 1 was never selected". */
        state->MaskHistogram[value & VIDEO_ALL_PLANES]++;
        /* ► THE PAIR, NOT THE TWO HISTOGRAMS SEPARATELY. "write mode 1 happens 120
             times" and "mask 0x0F happens 44 times" cannot be combined by the reader:
             a latch copy through a SINGLE-plane mask is served correctly by per-plane
             backing, one through an ALL-plane mask is not, and only the pairing says
             which Doom actually does. */
        state->ModeMaskHistogram[(state->WriteMode & VIDEO_GC_WRITE_MODE_MASK) * VIDEO_MAP_MASK_VALUES + (value & VIDEO_ALL_PLANES)]++;
        /* A mask change is the moment the outgoing plane's data is complete. */
        /* Flush BEFORE the mask moves: everything written since the last flush
           belongs to the mask that is now going out. With host-supplied per-plane
           backing there is nothing to flush -- the write already went to the right
           plane -- and all that is needed is to point the window at the new one. */
        /* ► ACCOUNT FOR EVERY WRITE THAT DOES NOT REACH YMapSelect. 8.6% of a run's
             map-mask writes did not move the window and no counter said why. These two
             are the only ways a write can be dropped here, and a dropped mask change
             strands the window on the plane the PREVIOUS mask chose -- so the next
             store lands in the wrong plane, which is what a four-way collapse is made
             of. `chain4` in particular is a live suspect: the guest may change the mask
             while chained and expect the change to hold once it unchains. */
        if (state->IsChain4)                                   state->MaskSkipChain4++;
        else if ((BYTE)(value & VIDEO_ALL_PLANES) == state->YMask)       state->MaskSkipSame++;
        if (!state->IsChain4) {
            /* ⚠ WITH HOST BACKING, CALL ON EVERY WRITE -- NOT ONLY ON A CHANGE. Once the
                 host follows GR4 (the read plane) the window can have MOVED since the
                 last map-mask write, so "the mask value is unchanged" no longer implies
                 "the window is where the writes need it". The host early-returns when it
                 already is, so the extra calls cost a compare; the alternative is a
                 store landing in the plane the last READ selected.
                 The fallback de-interleave path has no such window and keeps the skip. */
            if (state->YMapSelect)                        state->YMapSelect(state->YMapContext, (INT)(value & VIDEO_ALL_PLANES));
            else if ((BYTE)(value & VIDEO_ALL_PLANES) != state->YMask) VideoModeYFlush(state);
            if ((BYTE)(value & VIDEO_ALL_PLANES) != state->YMask)      state->IsDirty = 1;
        }
        state->MapMask = (BYTE)(value & VIDEO_ALL_PLANES);
        state->YMask   = state->MapMask;
    }
    else if (state->SequencerIndex == VIDEO_SR_MEMORY_MODE) {                 /* Memory Mode: bit 3 = Chain-4 */
        BYTE chain4 = (BYTE)((value >> VIDEO_SR4_CHAIN4_SHIFT) & 1);
        if (chain4 != state->IsChain4) {
            /* ── #184: THE SAME BYTES, TWO ADDRESSINGS. A chain-4 store at CPU address A
                 lands in plane A&3 at plane offset A&~3 (docs/ref/vga.md §8) -- so a program
                 that draws chained and then unchains (Doom, and p_vgamem's chain4.abcd)
                 must find those bytes spread across the planes. Here the chained view is the
                 linear aperture and the unchained one is the four plane sections, so the
                 switch has to MOVE them: scatter on the way out of chain-4, gather on the way
                 back in. Snapshot first -- the aperture changes meaning at the remap, and a
                 host may back the linear view with plane 0 itself. */
            static BYTE transfer[VIDEO_Y_PLANE_SIZE];
            UINT32 address;
            INT linear = (state->ModeKind == VIDEO_KIND_LINEAR8 && state->VideoMemory);
            if (linear && !chain4) for (address = 0; address < VIDEO_Y_PLANE_SIZE; ++address) transfer[address] = state->VideoMemory[address];
            if (linear && chain4)
                for (address = 0; address < VIDEO_Y_PLANE_SIZE; ++address) transfer[address] = VideoPlaneBytes(state, (INT)(address & VIDEO_PLANE_INDEX_MASK))[address & ~VIDEO_PLANE_INDEX_MASK];
            state->IsChain4 = chain4; state->YMask = state->MapMask;
            state->Chain4Selects++;
            if (state->YMapSelect) state->YMapSelect(state->YMapContext, chain4 ? -1 : (INT)state->MapMask);
            if (linear && !chain4) {
                for (address = 0; address < VIDEO_Y_PLANE_SIZE; ++address) VideoPlaneBytes(state, (INT)(address & VIDEO_PLANE_INDEX_MASK))[address & ~VIDEO_PLANE_INDEX_MASK] = transfer[address];
                if (!state->YMapPlane)                      /* the no-host fallback's copy */
                    for (address = 0; address < VIDEO_Y_PLANE_SIZE; ++address) state->YPlanes[address & VIDEO_PLANE_INDEX_MASK][address & ~VIDEO_PLANE_INDEX_MASK] = transfer[address];
                ++state->Chain4Transfers;
            }
            if (linear && chain4) {
                for (address = 0; address < VIDEO_Y_PLANE_SIZE; ++address) state->VideoMemory[address] = transfer[address];
                ++state->Chain4Transfers;
            }
            state->IsDirty = 1;
        }
    }
}
static VOID VideoSequencerIn(PVOID context, WORD port, BYTE width, UINT32 *value)
{
    PVIDEO_STATE state = (PVIDEO_STATE)context; (VOID)width;
    /* ⚠ USED TO RETURN 0 FOR EVERY INDEX BUT 2. A VGA reads every Sequencer register
       back, and a guest that probes the card by writing and re-reading one got a zero
       that says "no card here". The register file answers properly now; index 2 still
       comes from MapMask, which is the live authority for it. */
    if (port == VIDEO_PORT_SEQUENCER_INDEX) { *value = state->SequencerIndex; return; }
    *value = (state->SequencerIndex == VIDEO_SR_MAP_MASK) ? state->MapMask : state->SequencerRegisters[state->SequencerIndex & VIDEO_SR_INDEX_MASK];
}
/* Graphics Controller ports 3CE (index) / 3CF (data). */
static VOID VideoGcSetData(PVOID context, UINT32 value);

static VOID VideoGcOut(PVOID context, WORD port, BYTE width, UINT32 value)
{
    PVIDEO_STATE state = (PVIDEO_STATE)context;
    if (port == VIDEO_PORT_GC_INDEX) { VideoIndexData(&state->GcIndex, width, value, VideoGcSetData, state); return; }
    VideoGcSetData(state, value);
}
static VOID VideoGcSetData(PVOID context, UINT32 value)
{
    PVIDEO_STATE state = (PVIDEO_STATE)context;
    state->GcRegisters[state->GcIndex & VIDEO_GR_INDEX_MASK] = (BYTE)value;
    state->GcWrites  [state->GcIndex & VIDEO_GR_INDEX_MASK]++;
    switch (state->GcIndex) {
    case VIDEO_GR_SET_RESET: state->SetReset   = (BYTE)(value & VIDEO_ALL_PLANES); break;
    case VIDEO_GR_ENABLE_SET_RESET: state->EnableSetReset   = (BYTE)(value & VIDEO_ALL_PLANES); break;
    /* GR2 and GR7 are the two halves of read mode 1 and used to fall into default:,
       i.e. be dropped. See ReadMode in the header. */
    case VIDEO_GR_COLOR_COMPARE: state->ColorCompare  = (BYTE)(value & VIDEO_ALL_PLANES); break;
    case VIDEO_GR_COLOR_DONT_CARE: state->ColorDontCare = (BYTE)(value & VIDEO_ALL_PLANES); break;
    case VIDEO_GR_DATA_ROTATE: state->FunctionRotate = (BYTE)(value & VIDEO_GR3_MASK); break;
    /* ── GR4 IS THE READ PLANE, AND THE REMAP PATH CANNOT SEE READS AT ALL. ──────────
         In the `st->Planes[]` interpreter path a guest read is served by us and honours
         this register (see the read-mode-0 return). With host-supplied per-plane backing
         A0000 is a REAL mapped section, so a guest read never reaches this file and
         returns whatever plane the WRITE MASK last selected. Read plane and write plane
         are independent on the hardware, so any guest that sets them apart -- Doom's
         `I_ReadScreen` cycles GR4 with the write mask irrelevant -- gets the wrong bytes,
         SILENTLY. Every exclusion so far in the status-bar hunt has been about writes.
       ► Count the pairing, not the register. `Gr4Histogram` alone cannot say whether GR4
         ever DISAGREED with the mapped plane, and disagreement is the entire defect;
         the host compares against `g_ycur` in the hook. */
    case VIDEO_GR_READ_MAP:
        state->ReadMap = (BYTE)(value & VIDEO_GC_READ_MAP_MASK);
        state->Gr4Histogram[value & VIDEO_GC_READ_MAP_MASK]++;
        if (state->YMapReadMap) state->YMapReadMap(state->YMapContext, (INT)(value & VIDEO_GC_READ_MAP_MASK));
        break;
    case VIDEO_GR_MODE:
        /* ► COUNT THE WRITE MODES. Per-plane backing can only serve write mode 0, where
             a guest store is a plain byte into the selected plane. WRITE MODE 1 is a
             LATCH COPY: reading an address loads all four planes into the VGA's latches
             and the next store writes all four back at once. That is the standard mode-Y
             trick for moving a region inside video memory without touching the CPU bus
             four times -- and with A0000 pointing at ONE plane it collapses, because the
             guest can only read and write the plane that happens to be mapped.
             If a program uses it, the mapping approach cannot serve it and the fact has
             to be visible rather than inferred. */
        state->WriteModeHistogram[value & VIDEO_GC_WRITE_MODE_MASK]++;
        state->WriteMode  = (BYTE)(value & VIDEO_GC_WRITE_MODE_MASK);
        state->ReadMode   = (BYTE)((value >> VIDEO_GR5_READ_MODE_SHIFT) & 1);   /* ⚠ bit 3 used to be masked off */
        if (state->YMapWriteMode) state->YMapWriteMode(state->YMapContext, (INT)(value & VIDEO_GC_WRITE_MODE_MASK));
        break;
    case VIDEO_GR_BIT_MASK: state->BitMask    = (BYTE)value;          break;
    default: break;
    }
}
static VOID VideoGcIn(PVOID context, WORD port, BYTE width, UINT32 *value)
{
    PVIDEO_STATE state = (PVIDEO_STATE)context; (VOID)width;
    if (port == VIDEO_PORT_GC_INDEX) { *value = state->GcIndex; return; }
    switch (state->GcIndex) {
    case VIDEO_GR_SET_RESET: *value = state->SetReset; break;  case VIDEO_GR_ENABLE_SET_RESET: *value = state->EnableSetReset; break;
    case VIDEO_GR_COLOR_COMPARE: *value = state->ColorCompare; break; case VIDEO_GR_COLOR_DONT_CARE: *value = state->ColorDontCare; break;
    case VIDEO_GR_DATA_ROTATE: *value = state->FunctionRotate; break; case VIDEO_GR_READ_MAP: *value = state->ReadMap; break;
    /* ⚠ GR5 IS NOT ONLY THE TWO MODE FIELDS. Bits 0:1 are the write mode and bit 3
         the read mode, and those are shadowed because the engine uses them -- but
         bit 2 (test), bit 4 (odd/even), bit 5 (shift register) and bit 6 (256-colour
         shift) are not modelled, and returning only the shadows reported 0x00 where
         a real BIOS leaves 0x10 in mode 3 and 0x40 in 13h. Merge: the shadows for
         what we model, the stored byte for what we do not. */
    case VIDEO_GR_MODE: *value = (BYTE)((state->GcRegisters[VIDEO_GR_MODE] & VIDEO_GR5_UNMODELLED)
                           | state->WriteMode | (state->ReadMode << VIDEO_GR5_READ_MODE_SHIFT)); break;
    case VIDEO_GR_BIT_MASK: *value = state->BitMask; break;
    default: *value = state->GcRegisters[state->GcIndex & VIDEO_GR_INDEX_MASK]; break;   /* GR6 and the rest read back */
    }
}

/* Input Status Register 1 (3DA/3BA) -- bit 3 = vertical retrace, bit 0 = display
   disabled (set during EITHER horizontal or vertical blanking). A read also resets
   the attribute-controller flip-flop.

   ▶ THIS USED TO TOGGLE BOTH BITS ON EVERY READ. That guaranteed a "wait until set,
     then wait until clear" loop finished within two reads, so no guest could ever
     spin here forever -- the right call when the alternative was a hang, and the
     comment that lived here said plainly "we have no real CRT timing".
     But it also meant the bits had NO RELATIONSHIP TO TIME. `WAIT &H3DA,8` -- which
     is the entire frame clock of a great deal of DOS graphics code -- returned
     immediately, so those programs ran as fast as we could execute them instead of
     at ~60-70 Hz. Measured live on the physical box: BOUNCEBX tore instead of
     animating, MATRIX_2 outran MATRIX_1 (stock ntvdm has that pair the other way
     round), and CAVE ran "way too fast" in SCREEN 13.
   ▶ We DO have a timebase now -- `host_pit_sync()` has derived guest clocks from
     QueryPerformanceCounter since session 11 -- so derive the bits from it.
     `st->TimeUs` is NULL off-VM by default, which keeps the old toggle for tests
     that do not care about timing.
   ▶ The retrace bit is asserted for the whole VERTICAL BLANKING interval rather
     than just the 2-line sync pulse. A 2-line window is 0.4% of a frame, and a
     guest that does any work between polls would miss it and wait an extra frame;
     the blanking interval (~9%) is the forgiving reading and is what emulators
     conventionally report. */
/* ── WHERE IS THE BEAM? One answer for everyone who asks. ──────────────────────
     VideoStatusIn derived the frame timing inline; the raster-split bookkeeping needs
     the same numbers at every DAC write, so it lives here. Returns 0 with no clock
     (the off-VM default). `now` is the model's microseconds; `frame_us` the frame
     period; `vtotal`/`vdisp`/`vblank` the line counts (total, display end, blank
     start); `frame_no` = now / frame_us; `line` = the scanline the beam is on. */
static INT VideoBeam(PCVIDEO_STATE state, UINT64 *now, UINT32 *frameUs,
                    UINT32 *verticalTotal, UINT32 *verticalDisplay, UINT32 *verticalBlank,
                    UINT32 *frameNumber, UINT32 *line)
{
    INT isTall; UINT32 total, active, blank, hz; UINT64 inFrame;
    if (!state->TimeUs) return 0;
    /* A VESA graphics mode runs on its own timing, not the last VGA mode's (#226). */
    if (VideoVesaGeometry(state, verticalTotal, verticalDisplay, verticalBlank, &hz)) {
        *frameUs = MICROSECONDS_PER_SECOND_U / hz;
        *now      = state->TimeUs();
        *frameNumber = (UINT32)(*now / *frameUs);
        inFrame  = *now % (UINT64)*frameUs;
        *line     = (UINT32)((inFrame * (UINT64)*verticalTotal) / *frameUs);
        return 1;
    }
    /* 480-line modes run at 60 Hz, the 200/400-line ones at 70 Hz. Mode 13h is
       320x200 displayed as 400 scanlines, so it belongs with the 70 Hz group -- key
       the choice off the DISPLAYED height, not the mode number. */
    isTall    = (state->GraphicsHeight > VIDEO_VACTIVE_LOW);
    *verticalTotal = isTall ? VIDEO_VTOTAL_HIGH  : VIDEO_VTOTAL_LOW;
    *verticalBlank = isTall ? VIDEO_VACTIVE_HIGH : VIDEO_VACTIVE_LOW;
    *verticalDisplay  = *verticalBlank;
    /* Prefer the geometry the guest programmed. `vblank` is BLANK START, not display
       end -- they differ by 6 lines in the BIOS modes and by 45 in 640x350, and it is
       blanking, not the end of the picture, that raises the status bit. */
    if (VideoVerticalTiming(state, &total, &active, &blank)) {
        *verticalTotal = total; *verticalDisplay = active; *verticalBlank = blank;
        /* 449-line modes are the 70 Hz family, 525-line the 60 Hz one. Keyed off the
           measured total rather than the displayed height, so a 350-line mode is no
           longer forced to pick a side of a 400-line fence. */
        isTall = (total >= VIDEO_TALL_FRAME_LINES);
    }
    *frameUs = MICROSECONDS_PER_SECOND_U / (UINT32)(isTall ? VIDEO_VBL_HZ_HIGH : VIDEO_VBL_HZ_LOW);
    *now      = state->TimeUs();
    *frameNumber = (UINT32)(*now / *frameUs);
    inFrame  = *now % (UINT64)*frameUs;
    /* SCALE FIRST, DIVIDE ONCE -- see VideoStatusIn for why line_us must not be an integer. */
    *line     = (UINT32)((inFrame * (UINT64)*verticalTotal) / *frameUs);
    return 1;
}

/* ── ★ THE DISPLAYED FRAME'S START ADDRESS AND PEL PANNING, ON THE HARDWARE'S SCHEDULE. ──
     (s83, Mario vs Windows 98 and stock NTVDM: "butter smooth" there, "jagged, jarring"
     here.) Mario's scroll routine, from its binary:
         cli / wait 3DA bit 3 CLEAR           ; in the picture
         out 3D4: 0Ch,0Dh = y*90 + x/4        ; coarse position, whole bytes
         wait 3DA bit 3 SET                   ; retrace
         out 3C0: 33h, (x*2) & 7              ; AR13 pel panning: the 0-3 pixel remainder
     On a VGA the address counter LOADS the start address at the start of vertical
     retrace, and the panning written inside that retrace applies as the next picture
     begins -- so the pair lands together on the next frame. We took the start address
     whenever the host happened to draw and ignored AR13 altogether: the view moved in
     4-pixel jumps and, drawn at the wrong moment, a frame out of step with its pan.
   ► THE RULE, computed from the same beam clock the 3DA status read uses (so a guest
     that saw bit 3 set is past the latch point, and one that saw it clear is before it):
       * at each retrace start (the blanking line VideoBeam reports) StartVs := register;
       * at each frame start (line 0) the display takes StartVs and the current AR13.
     Called BEFORE every write to 0Ch/0Dh/AR13 and when a frame is built, so the register
     values between two calls are constant and "the value at that boundary" is simply
     the value now. No clock (off-VM) or a long gap: take everything as it stands. */
static VOID VideoLatch(PVIDEO_STATE state, INT atFrame)
{
    UINT64 now, frameStart, displayStart, blankOffset;
    UINT32 frameUs, verticalTotal, verticalDisplay, verticalBlank, frameNumber, line;
    if (!VideoBeam(state, &now, &frameUs, &verticalTotal, &verticalDisplay, &verticalBlank, &frameNumber, &line) || !frameUs || !verticalTotal) {
        /* No beam clock: the frame build IS the retrace, as before s83. */
        if (!atFrame) return;
        if (state->IsCrtcStartPending) state->CrtcStartHalf++;
        state->StartVs = state->CrtcStartLive = (WORD)state->CrtcStart;
        state->DisplayPan = state->AttributeRegisters[VIDEO_AR_PAN];
        state->VesaOriginVs = state->VesaOriginLive = state->VesaOrigin;
        return;
    }
    (VOID)verticalDisplay; (VOID)frameNumber; (VOID)line;
    frameStart = state->LatchTime; state->LatchTime = now;
    if (!frameStart || now < frameStart || now - frameStart > VIDEO_LATCH_STALE_FRAMES * (UINT64)frameUs) {
        state->StartVs = state->CrtcStartLive = (WORD)state->CrtcStart;
        state->DisplayPan = state->AttributeRegisters[VIDEO_AR_PAN];
        state->VesaOriginVs = state->VesaOriginLive = state->VesaOrigin;
        return;
    }
    /* ► THE VESA DISPLAY START RIDES THE SAME SCHEDULE (#226): a VBE BIOS implements
         4F07h by writing the CRTC start (plus its extension bits), so it loads at the
         retrace start and shows from the next picture exactly as 0Ch/0Dh do. */
    blankOffset = (UINT64)verticalBlank * frameUs / verticalTotal;                    /* retrace start, within the frame */
    displayStart  = (now / frameUs) * frameUs;                            /* the last frame start <= now     */
    if (displayStart > frameStart) {                                  /* a new picture began since t0    */
        if (displayStart >= frameUs && displayStart - frameUs + blankOffset > frameStart) {         /* ...and its retrace was after t0 */
            if (state->IsCrtcStartPending) state->CrtcStartHalf++;   /* loaded mid-pair: torn */
            state->StartVs = (WORD)state->CrtcStart;
            state->VesaOriginVs = state->VesaOrigin;
        }
        state->CrtcStartLive = state->StartVs;
        state->DisplayPan        = state->AttributeRegisters[VIDEO_AR_PAN];
        state->VesaOriginLive   = state->VesaOriginVs;
    }
    if (displayStart + blankOffset <= now && displayStart + blankOffset > frameStart) {         /* this frame's retrace began      */
        if (state->IsCrtcStartPending) state->CrtcStartHalf++;
        state->StartVs = (WORD)state->CrtcStart;
        state->VesaOriginVs = state->VesaOrigin;
    }
}

/* ── #226: WHEN MAY A 4F07h BL=80h/82h CALL RETURN? ────────────────────────────────────
     "Set Display Start during Vertical Retrace" (VBE 2.0 §4.10; 3.0 adds 82h, which
     "schedule[s] the display start address change to occur, and then wait[s] until the
     address has changed before returning"). The rule, on the beam clock 3DAh uses:
       * the beam is IN a retrace that has not yet released such a call: return now, and
         the start is the one this retrace loads (it shows from the next picture);
       * otherwise wait for the NEXT retrace start -- which also means a guest that calls
         twice inside one retrace is paced to one flip per frame, as the Bochs/SeaVGABIOS
         style `wait while in retrace; wait until in retrace` loop would pace it.
     Returns the model time the call completes at (0 = now) and says whether this call's
     start should be taken by the CURRENT retrace (1) or left to the latch (0). */
static UINT64 VideoVesaVblRelease(PVIDEO_STATE state, INT *isNowInVbl)
{
    UINT64 now, blankOffset, displayStart, verticalStart;
    UINT32 frameUs, verticalTotal, verticalDisplay, verticalBlank, frameNumber, line;
    *isNowInVbl = 0;
    if (!VideoBeam(state, &now, &frameUs, &verticalTotal, &verticalDisplay, &verticalBlank, &frameNumber, &line) || !frameUs || !verticalTotal) return 0;
    (VOID)verticalDisplay; (VOID)line;
    blankOffset    = (UINT64)verticalBlank * frameUs / verticalTotal;
    displayStart     = (now / frameUs) * frameUs;
    verticalStart = displayStart + blankOffset;                              /* this frame's retrace start   */
    if (now >= verticalStart) {                            /* in retrace now               */
        if (state->Vesa07Vbl != frameNumber + 1u) { state->Vesa07Vbl = frameNumber + 1u; *isNowInVbl = 1; return 0; }
        state->Vesa07Vbl = frameNumber + VIDEO_FRAMES_TO_AFTER_NEXT;                 /* taken: the next one          */
        return verticalStart + frameUs;
    }
    state->Vesa07Vbl = frameNumber + 1u;
    return verticalStart;
}

UINT32 VddVideoInt10WaitUs(PVIDEO_STATE state)
{
    UINT64 now, until = state->Int10WaitUntil;
    if (!until) return 0;
    if (!state->TimeUs) { state->Int10WaitUntil = 0; return 0; }
    now = state->TimeUs();
    /* Done, or a stamp more than a second out (a clock that went backwards, a stale
       value): never park the guest on it. */
    if (now >= until || until - now > MICROSECONDS_PER_SECOND_U) { state->Int10WaitUntil = 0; return 0; }
    return (UINT32)(until - now);
}

/* ── ★ THE EXTERNAL REGISTERS -- CLAIMED AT LAST. (docs/inventory/vga.md, step 1) ──
     3C2, 3C3, 3C6, 3CA and 3CC were claimed by NOBODY: a guest's write vanished and a
     read came back 0xFF from the bus's absent-device default. The inventory calls
     Miscellaneous Output the worst of them, because bits 6-7 are the sync polarities
     -- which is how a VGA encodes 400- vs 350- vs 480-line vertical size -- and bits
     2-3 the dot clock. A guest setting Mode X writes 0xE3 here, and we never saw it.
   ⚠ CAPTURE ONLY: these are stored and reported, and NOTHING derives geometry from
     them yet. In particular Misc Output bit 0 (CRTC at 3Bx vs 3Dx) is recorded and not
     acted on -- acting on it is step 3, with the whole shelf as the regression set.
   ⚠ READS CHANGE, AND THAT IS THE POINT. Every one of these ports used to answer 0xFF
     (not a value any VGA returns); now they answer what was written, or the documented
     power-on value. The one exception is deliberate: the DAC Pixel Mask resets to 0xFF,
     which is BOTH the hardware default and what the port used to return by accident. */
static VOID VideoExternalOut(PVOID context, WORD port, BYTE width, UINT32 value)
{
    PVIDEO_STATE state = (PVIDEO_STATE)context; (VOID)width;
    if (port == VIDEO_PORT_MISC_WRITE)      { state->MiscOutput   = (BYTE)value; state->MiscWrites++;  }
    else if (port == VIDEO_PORT_VGA_ENABLE) { state->VgaEnable = (BYTE)(value & 1); state->VgaEnableWrites++; }
    else if (port == VIDEO_PORT_DAC_MASK) { state->DacMask   = (BYTE)value; state->DacMaskWrites++; }
    /* 3CA and 3CC are READ-ONLY aliases (Feature Control / Misc Output); a write
       there is a guest bug on real hardware too, so it is dropped, not stored. */
}

static VOID VideoExternalIn(PVOID context, WORD port, BYTE width, UINT32 *value)
{
    PVIDEO_STATE state = (PVIDEO_STATE)context; (VOID)width;
    switch (port) {
    /* ── ★★★ INPUT STATUS 0 = 0x10. MEASURED, after three sessions of 0x00. ────
         Bit 4 is Switch Sense, the DAC comparator a real card drives; bit 7 is
         "vertical retrace interrupt pending", which we never raise. This used to
         answer 0x00 and say so -- flagged UNVERIFIED here and in the STAGE2 dump,
         deliberately not guessed at, pending an oracle with a real video BIOS.
       ★ PCem WITH A GENUINE IBM VGA ROM SAYS 0x10, IN ALL TWELVE MODES p_vgareg
         sets -- text, planar, 13h, Mode X and mono alike. dosbox-x also drives bit
         4 (0x70 in mode 3, 0x60 in 13h, so it varies there); only QEMU answers
         0x00, and QEMU is the host that has been wrong on every external-register
         row this project has checked. Two of three set bit 4, and the one that
         matters is the one running period-correct firmware.
       ⚠ A CONSTANT IS THE RIGHT SHAPE FOR US even though the real bit is a
         comparator: the sense line reports the monitor, and ours is fixed. What
         was wrong was the value, not the constancy.
       ✅ #187 (s84): BIT 7 IS NOW BUILT -- see VideoVintCr11. What follows is why it was
         once left out, kept because the reasoning about the oracles still holds.
       ⛔ BIT 7 STAYED 0, AND THAT WAS A RECORDED GAP, NOT AN OVERSIGHT. p_vgaext's
         is0.vsync case enables the vertical-retrace interrupt in CR11 and looks:
         dosbox-x sets bit 7 and clears it through CR11 bit 4; PCem's IBM VGA never
         sets it at all. So the oracles split 1-2 AGAINST the feature, the IBM VGA
         spec is for it, and no DOS guest this project has met uses it -- the VGA
         vertical interrupt is famously unreliable and IBM's own documentation
         steers software away from it. Implementing it would be a guest-visible
         change with no guest to check it against. See docs/inventory/vga.md. */
    case VIDEO_PORT_INPUT_STATUS_0: *value = VIDEO_STATUS0_SWITCH_SENSE | VideoVintStatus(state); break;   /* bit 7: #187, see VideoVintCr11 */
    case VIDEO_PORT_VGA_ENABLE: *value = state->VgaEnable; break;
    case VIDEO_PORT_DAC_MASK: *value = state->DacMask;   break;
    case VIDEO_PORT_FEATURE_READ: *value = state->FeatureControl;  break;   /* Feature Control read  */
    case VIDEO_PORT_MISC_READ: *value = state->MiscOutput;   break;   /* Misc Output read      */
    default:    *value = VIDEO_FLOATING_BUS; break;             /* 3CB: nothing decodes there */
    }
}

/* Feature Control WRITE lives at 3BA/3DA -- the same port whose READ is Input Status 1.
   It was thrown away here; now it is captured, for the same reason as the rest. */
static VOID VideoStatusOut(PVOID context, WORD port, BYTE width, UINT32 value)
{
    PVIDEO_STATE state = (PVIDEO_STATE)context; (VOID)port; (VOID)width;
    state->FeatureControl = (BYTE)value; state->FeatureWrites++;
}
#define VIDEO_HBL_DEBT_MAX 16u   /* lines: further apart than this, a poll is not counting lines */
/* #183: how long until 3DAh bit 3 (vertical retrace: blank start to frame end) READS
   `want_set`? 0 = it already does. UINT32_MAX = no beam clock to say. The host uses this
   to sleep through a retrace-wait loop instead of trapping on every iteration. */
UINT32 VddVideoUsToRetrace(PVIDEO_STATE state, INT wantSet)
{
    UINT64 now, inFrame, blankOffset;
    UINT32 frameUs, verticalTotal, verticalDisplay, verticalBlank, frameNumber, line;
    if (!VideoBeam(state, &now, &frameUs, &verticalTotal, &verticalDisplay, &verticalBlank, &frameNumber, &line) || !frameUs || !verticalTotal) return VIDEO_UINT32_MAX;
    (VOID)verticalDisplay; (VOID)frameNumber; (VOID)line;
    inFrame = now % (UINT64)frameUs;
    blankOffset = (UINT64)verticalBlank * frameUs / verticalTotal;                      /* retrace (bit 3) starts here */
    if (wantSet) return inFrame >= blankOffset ? 0u : (UINT32)(blankOffset - inFrame);
    return inFrame < blankOffset ? 0u : (UINT32)((UINT64)frameUs - inFrame);
}

static VOID VideoStatusIn(PVOID context, WORD port, BYTE width, UINT32 *value)
{
    PVIDEO_STATE state = (PVIDEO_STATE)context; (VOID)port; (VOID)width;
    UINT64 now, inFrame;
    /* ⚠ LOAD-BEARING, AND NOT A VBLANK CONCERN. Reading this port resets the
         Attribute Controller's index/data flip-flop, and every guest relies on it
         before touching 0x3C0. See VideoAttributeOut. */
    state->AttributeFlipFlop = 0;
    UINT32 frameUs, line, vesaVerticalTotal, vesaVerticalDisplay, vesaVerticalBlank, frameNumber;
    INT verticalTotal, verticalActive, isInVbl, isInHbl;

    if (!VideoBeam(state, &now, &frameUs, &vesaVerticalTotal, &vesaVerticalDisplay, &vesaVerticalBlank, &frameNumber, &line)) {
        state->Retrace ^= (VIDEO_STATUS1_VERTICAL_RETRACE | VIDEO_STATUS1_DISPLAY_DISABLED);                        /* no clock injected: old behaviour */
        *value = state->Retrace;
        return;
    }
    verticalTotal = (INT)vesaVerticalTotal; verticalActive = (INT)vesaVerticalBlank; (VOID)vesaVerticalDisplay;
    /* Bracket the polling in the model's own microseconds -- see the header. */
    if (!state->Time3DaFirst) state->Time3DaFirst = now;
    if (state->Time3DaLast) {
        UINT64 delta = now - state->Time3DaLast;
        state->PresentGapUs = (delta > VIDEO_UINT32_MAX) ? VIDEO_UINT32_MAX : (UINT32)delta;
        if (!delta) state->Dt3DaZero++;
        else {
            UINT status = 0;
            UINT64 deltaBucket = delta;
            while (status < VIDEO_DT3DA_LAST_BUCKET && deltaBucket >= VIDEO_DT3DA_BUCKET_BASE) { deltaBucket >>= VIDEO_DT3DA_BUCKET_SHIFT; status++; }
            state->Dt3DaHistogram[status]++;
            if (delta > (UINT64)state->Dt3DaMax)
                state->Dt3DaMax = (delta > VIDEO_UINT32_MAX) ? VIDEO_UINT32_MAX : (UINT32)delta;
        }
    }
    state->Time3DaLast = now;
    /* ── SCALE FIRST, DIVIDE ONCE. A scanline is not a whole number of microseconds:
         at 70 Hz it is 14285/449 = 31.8us, and taking `line_us = frame_us / vtotal`
         truncated that to 31. Lines then ran 2.6% fast -- a frame's worth of them
         reached 460 on a 449-line screen, which is what the clamp below was quietly
         absorbing, and it put every line boundary up to 12 lines out by the bottom of
         the picture. Multiplying into the frame before dividing keeps the fraction and
         needs no clamp: `line` cannot exceed vtotal-1 because in_frame < frame_us.
         The remainder is the position WITHIN the line, on the same scale. */
    inFrame = now % (UINT64)frameUs;
    {   UINT64 position = inFrame * (UINT64)verticalTotal;
        line   = (UINT32)(position / frameUs);
        isInHbl = ((UINT64)(position % frameUs) * PERCENT >= (UINT64)frameUs * VIDEO_HACTIVE_PERCENT);
    }
    isInVbl = (line >= (UINT32)verticalActive);
    /* ── #225: A RETRACE THAT STARTED AND ENDED BETWEEN TWO POLLS HAPPENED TOO -- but
         only while the CPU throttle holds the guest (IsVblOweOn). The previous poll
         saw the active picture of an EARLIER frame, so that frame's retrace passed
         unseen during a hold; report it ONCE, as the bit-0 rule below does for a line,
         and let the next poll read the true phase. One per poll, however many frames
         were skipped: a slow machine loses real time, it does not get extra frames. */
    if (state->IsVblOweOn && !isInVbl && state->IsPort3DaHaveLast && !state->IsPort3DaLastVbl
        && frameNumber != state->Port3DaLastFrame) {
        isInVbl = 1; state->Port3DaVblOwed++;
    }
    state->Port3DaLastFrame = frameNumber;
    /* bit 3 = vertical retrace; bit 0 = display disabled (h- OR v-blank). Bit 0 is a
       DIFFERENT signal on a real card -- it changes per scanline, not per frame -- so
       toggling the two together, as we used to, was doubly wrong.
       The owed-blank rule below is what makes a scanline counter exact. */
    /* ── ★★★★ A BLANK THAT PASSED BETWEEN TWO POLLS HAPPENED, WHETHER OR NOT A SAMPLE
         LANDED IN IT. Bit 0 is what a scanline COUNTER reads: Lemmings' "High
         Performance PC" calibration does `wait while set; wait while clear` 320 times
         against the 8254 and uses the elapsed clocks as its game tick (guest
         CS:1602..1643), and the tick is then what places its raster palette split on
         the screen -- row 160 when the count is exactly 320 lines. On real silicon an
         `in` is a microsecond and every 6.4us hblank is sampled. Here any host stall
         during the ~10 ms window (a lock, a present, a capture) skips whole lines:
         measured reloads 0x2F00..0x38CD across one evening -- 320 to 383 lines -- and
         at 383 the toolbar's top 31 rows were drawn in the level's palette.
       ► So if the guest's previous poll was in an EARLIER line and saw the display
         active, the blanking of that earlier line went unobserved; report it ONCE and
         let the next poll read the true phase. The rule cannot fire within a line (two
         reads at one instant still agree, the property the timed model was built for)
         and does not fire when the guest saw the blank itself, so a fast poller sees
         exactly what it saw before. A poller slower than a whole line cannot count
         lines on real hardware either and is not helped. (s69 shipped this and s69
         reverted it with the PIT change it was bundled with; on its own it was right.) */
    {   UINT64 absoluteLine = (now / (UINT64)frameUs) * (UINT64)verticalTotal + line;
        INT bit0 = (isInVbl || isInHbl);
        /* ► ONE blank owed per straddle, and only for a poll that is plausibly counting
             lines (s69's rule, bounded, s70). Two generalisations were tried and
             measured wrong in video_test: repaying every line crossed as a debt of
             synthetic pulses either swallowed real blanks in the guest's `wait while
             set` phase (a 330us stall came back wholly unrepaid) or, drained the other
             way round, broke the plain case. A two-phase poll loop cannot be fed more
             than one blank per line boundary it crosses. So the residual stands: a
             host stall of N lines inside a count is repaid ONE line; the rest is
             real time the guest lost. With the interpreter port path now syncing the
             PIT (main.c iio_out) the rig measured 320.7..322.7 lines on four runs.
           ► A poll more than VIDEO_HBL_DEBT_MAX lines after the previous one is not a
             line counter (the attribute flip-flop reset before a palette write,
             once a frame) and owes nothing. */
        if (!bit0 && state->IsPort3DaHaveLast && !state->Port3DaLastBit0 && !isInVbl && !state->IsPort3DaLastVbl
            && absoluteLine > state->Port3DaLastLine
            && absoluteLine - state->Port3DaLastLine <= VIDEO_HBL_DEBT_MAX) {
            bit0 = 1; state->Port3DaHblOwed++;
        }
        state->Port3DaLastLine = absoluteLine; state->Port3DaLastBit0 = (BYTE)bit0;
        state->IsPort3DaLastVbl = (BYTE)isInVbl;
        state->IsPort3DaHaveLast = 1;
        *value = (UINT32)((isInVbl ? VIDEO_STATUS1_VERTICAL_RETRACE : 0u) | (bit0 ? VIDEO_STATUS1_DISPLAY_DISABLED : 0u));
        if (state->IsPort3DaRingOn) {          /* debug only -- see the note in the header */
            state->Port3DaRingUs[state->Port3DaRingCount & (VIDEO_PORT_3DA_RING - 1)] = (UINT32)now;
            state->Port3DaRingValue [state->Port3DaRingCount & (VIDEO_PORT_3DA_RING - 1)] = (BYTE)*value;
            state->Port3DaRingCount++;
        }
    }
    state->Retrace = (BYTE)*value;                      /* keep it observable in dumps    */
    /* Count the edge the GUEST sees, not the one the model produces: a clear->set
       transition between two of its own reads is exactly one completed
       `WAIT &H3DA,8`, so edges/second IS the guest's frame rate. */
    state->Port3DaReads++;
    if (isInVbl && !state->VblPrevious) state->VblEdges++;
    state->VblPrevious = (BYTE)isInVbl;
    /* ── RAISE THE PRESENT FROM THE GUEST'S FRAME (see PresentHook in the header).
         Same window as VddVideoIsPresentReady -- the last VIDEO_PRESENT_WINDOW_PER_MILLE of
         the frame before blanking, when a retrace-paced guest has finished drawing
         and is parked here polling -- expressed in lines so no second clock read is
         paid on a path that runs millions of times a second. A poller that skips
         the window (one read per frame, at the edge) gets the edge instead: the
         frame it drew is complete by then too. Once per frame_no either way. */
    if (state->PresentHook && state->PresentFrame != frameNumber) {
        UINT32 windowLines = (UINT32)verticalTotal * VIDEO_PRESENT_WINDOW_PER_MILLE / VIDEO_PER_MILLE;
        /* ► THE FIRST POLL AFTER A GAP IS THE BEST MOMENT OF ALL. A retrace-paced
             guest leaves this port to DRAW and comes back to wait: the first read
             after it was away (>= VIDEO_PRESENT_GAP_US) means the frame is complete
             and the guest is parked -- with most of the frame still ahead for the
             render, which blocks its polls while it runs. Firing in the window
             instead (first cut) put that render right before the retrace the guest
             was waiting for, and it missed one frame in twenty (BOUNCEBX 1798 ->
             1697 edges, dtmax 2.4 -> 13.5 ms). The window and the edge remain for
             a guest that never leaves the port. */
        INT isGap = state->PresentGapUs >= VIDEO_PRESENT_GAP_US;
        if (isGap || isInVbl || (line + windowLines >= (UINT32)verticalActive)) {
            state->PresentFrame = frameNumber;
            state->PresentHookFires++;
            if (isGap) state->PresentHookGap++;
            state->PresentHook(state->PresentContext);
        }
    }
    state->PresentGapUs = 0;
}

/* Copy the three character generators into guest-visible memory so the pointer handed
   out by INT 10h AH=11h AL=30h resolves to real glyph data. The tables are filled at
   start-up from the system's fonts (#322, src/host/sysfont.h) before this runs. */
/* #321: copy the three tables into guest memory again after the font has changed, and
   redraw. Only the glyphs: the save-pointer table and the vectors install_fonts writes
   are a POST job a program may since have re-pointed. A font a program loaded itself
   (AH=11h, IsUserFontOn) is its own and stays. */
INT VddVideoRefreshFonts(PVIDEO_STATE state)
{
    BYTE *font16, *font8, *font14;
    UINT character, line;
    if (!state || !state->Bus) return 0;
    font16 = (BYTE *)VddMapFlat(state->Bus, VDD_FONT8X16_SEG, 0);
    font8  = (BYTE *)VddMapFlat(state->Bus, VDD_FONT8X8_SEG, 0);
    font14 = (BYTE *)VddMapFlat(state->Bus, VDD_FONT8X14_SEG, 0);
    if (!font16 || !font8 || !font14) return 0;
    for (character = 0; character < VGA_FONT_CHARACTERS; ++character) {
        for (line = 0; line < VGA_FONT16_HEIGHT; ++line) font16[character * VGA_FONT16_HEIGHT + line] = g_VgaFont8x16[character][line];
        for (line = 0; line < VGA_FONT8_HEIGHT;  ++line) font8 [character * VGA_FONT8_HEIGHT  + line] = g_VgaFont8x8 [character][line];
        for (line = 0; line < VGA_FONT14_HEIGHT; ++line) font14[character * VGA_FONT14_HEIGHT + line] = g_VgaFont8x14[character][line];
    }
    state->IsDirty = 1;
    return 1;
}

VOID VddVideoInstallFonts(PVIDEO_STATE state)
{
    if (!state || !state->Bus) return;           /* called before the VDD joined the bus */
    if (!VddVideoRefreshFonts(state)) return;
    /* All three are REAL designs at their own size (#322: from the system's fonts, see
       src/host/sysfont.h -- the 8x8 is Terminal's own 8x8). The 8x8 used to be
       manufactured here by OR-ing adjacent row pairs of the 8x16 -- which squashes a
       16-row glyph into 6 and fills in every counter, so 'A' came out solid and 'E' came
       out as noise. Skyroads asks for this exact table (BH=3 and BH=4, measured) and
       draws its own text from the pointer we return, so that hack WAS the game's
       garbled text. Never derive a font. */
    VideoVbePmInstall(state);                    /* #53: the 4F0Ah block, beside them */
    /* ── #266: THE SAVE POINTER TABLE, AND 0040:00A8 POINTING AT IT. ──────────────────
         0040:00A8 was never written: it held whatever the VDM started with -- on the
         rig the real machine's BIOS tables, which describe a different card than the
         one our INT 10h programs. Layout per IBM / RBIL "Video Save Pointer Table":
           +00 video parameter table         +04 dynamic save area (0: none)
           +08 alpha font override (0)       +0C graphics font override (0)
           +10 secondary save pointer table  +14/+18 reserved (0)
         and the secondary one: +00 its length (1Ah), +02 the display combination code
         table, +06 secondary alpha font (0), +0A user palette profile (0), 12 reserved.
         The DCC table is the IBM/SeaVGABIOS one (16 entries, version 1, max code 8).
         Written once here (a POST job): a program may legitimately re-point 0040:00A8
         at its own table, and later syncs must not undo that. */
    {
        BYTE *table = (BYTE *)VddMapFlat(state->Bus, VDD_VIDTAB_SEG, 0);
        static const WORD displayCombination[VIDEO_DCC_ENTRIES] = {
            0x0000, 0x0100, 0x0200, 0x0102, 0x0400, 0x0104, 0x0500, 0x0502,
            0x0600, 0x0601, 0x0605, 0x0800, 0x0801, 0x0700, 0x0702, 0x0706 };
        UINT index;
        if (table) {
            for (index = 0; index < VDD_VPARAM_OFF + VDD_VPARAM_N * VIDEO_PARAMETER_ENTRY_BYTES; ++index) table[index] = 0;
            VideoWrite16(table + VDD_SAVEPTR_OFF + VIDEO_SAVE_POINTER_PARAMETERS, VDD_VPARAM_OFF);   VideoWrite16(table + VDD_SAVEPTR_OFF + VIDEO_SAVE_POINTER_PARAMETERS + VIDEO_FAR_SEGMENT, VDD_VIDTAB_SEG);
            VideoWrite16(table + VDD_SAVEPTR_OFF + VIDEO_SAVE_POINTER_SECONDARY, VDD_SAVEPTR2_OFF); VideoWrite16(table + VDD_SAVEPTR_OFF + VIDEO_SAVE_POINTER_SECONDARY + VIDEO_FAR_SEGMENT, VDD_VIDTAB_SEG);
            VideoWrite16(table + VDD_SAVEPTR2_OFF + VIDEO_SECONDARY_LENGTH, VIDEO_SECONDARY_TABLE_BYTES);
            VideoWrite16(table + VDD_SAVEPTR2_OFF + VIDEO_SECONDARY_DCC, VDD_DCC_OFF);     VideoWrite16(table + VDD_SAVEPTR2_OFF + VIDEO_SECONDARY_DCC + VIDEO_FAR_SEGMENT, VDD_VIDTAB_SEG);
            table[VDD_DCC_OFF + 0] = VIDEO_DCC_ENTRIES; table[VDD_DCC_OFF + 1] = VIDEO_DCC_VERSION; table[VDD_DCC_OFF + 2] = VIDEO_DCC_MAX_CODE; table[VDD_DCC_OFF + 3] = 0;
            for (index = 0; index < VIDEO_DCC_ENTRIES; ++index) VideoWrite16(table + VDD_DCC_OFF + VIDEO_DCC_HEADER_BYTES + index * VIDEO_DCC_ENTRY_BYTES, displayCombination[index]);
            for (index = 0; index < VDD_VPARAM_N; ++index)
                (VOID)VddVideoParameterEntry((BYTE)index, table + VDD_VPARAM_OFF + index * VIDEO_PARAMETER_ENTRY_BYTES);
            if (state->BiosData) { VideoWrite16(state->BiosData + BIOS_BDA_VIDEO_SAVE_POINTER, VDD_SAVEPTR_OFF); VideoWrite16(state->BiosData + BIOS_BDA_VIDEO_SAVE_POINTER + VIDEO_FAR_SEGMENT, VDD_VIDTAB_SEG); }
        }
    }
    /* ...and the font vectors, which VddVideoReset set before the host's IVT was
       final: written again now that it is (the host plants its own vectors first). */
    VideoSetVector(state, VIDEO_VECTOR_GRAPHICS_FONT, state->Int43Segment, state->Int43Offset);
    VideoSetVector(state, VIDEO_VECTOR_FONT_HIGH, state->Int1FSegment, state->Int1FOffset);
}

/* B8000 window hook (for the off-VM test; the live host maps the aperture RAM
   so direct writes never trap -- the renderer just reads vmem each frame). */
static BYTE VideoRead(PVOID context, UINT32 offset)
{ PVIDEO_STATE state = (PVIDEO_STATE)context; return state->VideoMemory[VIDEO_TEXT_OFFSET + offset]; }
static VOID VideoWrite(PVOID context, UINT32 offset, BYTE value)
{ PVIDEO_STATE state = (PVIDEO_STATE)context; state->VideoMemory[VIDEO_TEXT_OFFSET + offset] = value; state->IsDirty = 1; }

/* One character's glyph rows: the loaded user font wins (GH #52), otherwise the ROM
   table for the cell height in force -- 8x8 after a 1112h, 8x14 after 1111h, 8x16
   otherwise. One predictable branch in the ordinary case. */
static const BYTE *VideoGlyphRows(PCVIDEO_STATE state, BYTE character)
{
    if (state->IsUserFontOn)  return &state->UserFont[character * VIDEO_CELL_HEIGHT];
    if (state->CellHeight == VGA_FONT8_HEIGHT)   return g_VgaFont8x8[character];
    if (state->CellHeight == VGA_FONT14_HEIGHT)  return g_VgaFont8x14[character];
    return g_VgaFont8x16[character];
}

/* ── ONE CELL. The attribute byte's top bit is BLINK OR BRIGHT BACKGROUND, and the
     Attribute Controller (AR10 bit 3, INT 10h AX=1003h) decides which. Blink is the
     power-on default. We always masked the bit off -- `(attr >> 4) & 7` -- so a
     program that turned blink OFF to get sixteen background colours (every text-mode
     UI with a light-grey dialog on a bright panel) got the dark eight, and one that
     left blink ON and used it never blinked. `st->IsBlinkOffPhase` is the phase for this
     render, set once per frame by VddVideoRender from the injected clock. */
/* ── #324: A VGA TEXT CELL IS NINE DOTS WIDE. Sequencer Clocking Mode bit 0 picks 8 or 9
     (every standard VGA text mode sets 9: 80x25 is 720x400, 40x25 is 360x400). The ninth
     column is background, except that with Line Graphics Enable (AR10 bit 2) the box-
     drawing range C0h-DFh repeats the eighth column into it, which is what makes
     horizontal lines join from cell to cell. A VESA 132-column mode is 8 dots: the
     wider character clock would not fit the line. */
static INT VideoTextCellWidthOf(PCVIDEO_STATE state)
{
    if (state->VesaTextMode) return VIDEO_CHARACTER_CLOCK_8;
    return (state->SequencerRegisters[VIDEO_SR_CLOCKING_MODE] & VIDEO_SR1_8_DOT) ? VIDEO_CHARACTER_CLOCK_8 : VIDEO_CHARACTER_CLOCK_9;
}
INT VddVideoTextCellWidth(PCVIDEO_STATE state) { return VideoTextCellWidthOf(state); }

static VOID VideoRenderCell(PVIDEO_STATE state, INT row, INT column, BYTE character, BYTE attribute)
{
    INT glyphY, glyphX;
    INT cellHeight = state->CellHeight ? state->CellHeight : VIDEO_CELL_HEIGHT;
    INT cellWidth = VideoTextCellWidthOf(state);
    INT stride = state->Columns * cellWidth;                        /* 360 in a 40-column mode  */
    INT isLineGraphics = cellWidth == VIDEO_CHARACTER_CLOCK_9 && (state->AttributeMode & VIDEO_AR_MODE_LINE_GRAPHICS) && character >= VIDEO_LINE_GRAPHICS_FIRST && character <= VIDEO_LINE_GRAPHICS_LAST;
    BYTE foreground = attribute & VIDEO_ATTRIBUTE_COLOUR_MASK, background;
    const BYTE *glyph = VideoGlyphRows(state, character);
    if (state->IsBlink) {
        background = (BYTE)((attribute >> VIDEO_ATTRIBUTE_BACKGROUND_SHIFT) & VIDEO_ATTRIBUTE_BACKGROUND_DIM);
        if ((attribute & VIDEO_ATTRIBUTE_BLINK) && state->IsBlinkOffPhase) foreground = background;  /* off phase: the glyph hides */
    } else {
        background = (BYTE)((attribute >> VIDEO_ATTRIBUTE_BACKGROUND_SHIFT) & VIDEO_ATTRIBUTE_COLOUR_MASK);           /* sixteen backgrounds      */
    }
    for (glyphY = 0; glyphY < cellHeight; ++glyphY) {
        BYTE bits = glyph[glyphY];
        BYTE *pixelRow = &state->FrameBuffer[(row*cellHeight + glyphY) * stride + column*cellWidth];
        for (glyphX = 0; glyphX < VIDEO_CHARACTER_CLOCK_8; ++glyphX) pixelRow[glyphX] = (bits & (VIDEO_GLYPH_LEFT_BIT >> glyphX)) ? foreground : background;
        if (cellWidth == VIDEO_CHARACTER_CLOCK_9) pixelRow[VIDEO_CHARACTER_CLOCK_8] = (isLineGraphics && (bits & VIDEO_GLYPH_RIGHT_BIT)) ? foreground : background;
    }
}

static VOID VideoDrawHardwareCursor(PVIDEO_STATE state);

/* ── THE TEXT SCREEN AS THE GUEST WROTE IT, not as we drew it. ───────────────────
     An instrument for exactly one question, and it is a question screenshots cannot
     answer: when something is missing from the display, did the guest never PUT it
     there, or did we never DRAW it? Chasing QBasic's empty file list from captured
     images cost three wrong guesses -- blink, the search API, the DTA names -- two
     of which were about the picture rather than the data.
     Rows of characters, then the attribute of each cell in hex, straight out of the
     text VRAM the renderer itself reads. */
INT VddVideoTextSnapshot(PVIDEO_STATE state, char *output, INT capacity)
{
    static const char hexDigits[] = "0123456789abcdef";
    INT row, column, count = 0;
    if (!output || capacity < VIDEO_SNAPSHOT_MIN_CAPACITY) return 0;
    for (row = 0; row < state->Rows; ++row) {
        for (column = 0; column < state->Columns && count < capacity - VIDEO_SNAPSHOT_LINE_ROOM; ++column) {
            BYTE character = VideoDisplayCell(state, row, column)[0];
            output[count++] = (character >= VIDEO_CHAR_PRINTABLE_FIRST && character < VIDEO_CHAR_PRINTABLE_END) ? (char)character : '.';
        }
        if (count < capacity - VIDEO_SNAPSHOT_LINE_ROOM) output[count++] = '\n';
    }
    if (count < capacity - VIDEO_SNAPSHOT_HEADER_ROOM) { const char *height = "--attr--\n"; while (*height && count < capacity - VIDEO_SNAPSHOT_LINE_ROOM) output[count++] = *height++; }
    for (row = 0; row < state->Rows; ++row) {
        for (column = 0; column < state->Columns && count < capacity - VIDEO_SNAPSHOT_HEX_ROOM; ++column) {
            BYTE attributeByte = VideoDisplayCell(state, row, column)[1];
            output[count++] = hexDigits[(attributeByte >> NIBBLE_SHIFT) & VIDEO_NIBBLE_MASK]; output[count++] = hexDigits[attributeByte & VIDEO_NIBBLE_MASK];
        }
        if (count < capacity - VIDEO_SNAPSHOT_LINE_ROOM) output[count++] = '\n';
    }
    output[count] = 0;
    return count;
}

VOID VddVideoRender(PVIDEO_STATE state)                 /* text glyph render        */
{
    INT row, column;
    /* Text blinks at half the cursor rate: 32 frames on, 32 off at 60 Hz. */
    state->IsBlinkOffPhase = (BYTE)((state->IsBlink && state->TimeUs)
                              ? ((state->TimeUs() % VIDEO_BLINK_PERIOD_US) >= VIDEO_BLINK_HALF_US) : 0);
    for (row = 0; row < state->Rows; ++row)
        for (column = 0; column < state->Columns; ++column) {
            BYTE *cell = VideoDisplayCell(state, row, column);
            VideoRenderCell(state, row, column, cell[0], cell[1]);
        }
    VideoDrawHardwareCursor(state);
}

VOID VddVideoTextCursor(PVIDEO_STATE state, INT column, INT row,
                           WORD andMask, WORD xorMask)
{
    BYTE *cell, character, attribute;
    if (state->ModeKind != VIDEO_KIND_TEXT) return;
    if (column < 0 || row < 0 || column >= state->Columns || row >= state->Rows) return;
    cell    = VideoDisplayCell(state, row, column);
    character   = (BYTE)((cell[0] & (andMask & BYTE_MASK)) ^ (xorMask & BYTE_MASK));
    attribute = (BYTE)((cell[1] & (andMask >> BYTE_SHIFT)) ^ (xorMask >> BYTE_SHIFT));
    VideoRenderCell(state, row, column, character, attribute);
    /* The hardware cursor is drawn by the CRTC over whatever the cell holds, so it
       stays on top of the pointer when the two share a cell. */
    if (row == state->CursorRow && column == state->CursorColumn) VideoDrawHardwareCursor(state);
}

static VOID VideoDrawHardwareCursor(PVIDEO_STATE state)
{
    INT glyphY, glyphX;
    INT cellHeight = state->CellHeight ? state->CellHeight : VIDEO_CELL_HEIGHT;
    INT cellWidth = VideoTextCellWidthOf(state);
    INT stride = state->Columns * cellWidth;
    /* ── THE TEXT CURSOR: SHAPE FROM THE GUEST, SCALED, BLINK FROM THE CLOCK. ────
         This used to be two hard-coded scan lines, always lit. Two things were wrong
         with that and only one of them is cosmetic:
           * A REAL CURSOR BLINKS. On VGA the CRTC blinks it at the vertical rate
             divided by 32 -- 16 frames lit, 16 dark, about 1.9 Hz. A steady block is
             the one thing every DOS user would notice instantly.
           * THE GUEST CHOOSES THE SHAPE, and says so in INT 10h AH=01h CX: CH is the
             first scan line, CL the last. It is also how a program HIDES the cursor
             -- bit 5 of CH, or a start line past the end -- so ignoring CX means a
             full-screen editor that turned the cursor off gets one anyway, now
             blinking at it. Honouring the shape and honouring the hide are the same
             piece of code, which is why they arrive together.
         The phase comes from st->TimeUs, the injected clock the CRT timebase already
         uses, so this stays pure C and off-VM testable: with no clock injected the
         cursor is simply steady, which is what the existing battery expects. */
    if (state->CursorRow < state->Rows && state->CursorColumn < state->Columns) {
        UINT start, end;
        INT hidden, lit = 1;
        VddCursorLines(state->CursorShape, (UINT)cellHeight, &start, &end, &hidden);
        if (state->IsCursorEmulationOff) {                  /* AH=12h BL=34h: CX as written (#252) */
            start = (state->CursorShape >> BYTE_SHIFT) & VIDEO_CURSOR_LINE_MASK; end = state->CursorShape & VIDEO_CURSOR_LINE_MASK;
            hidden = ((state->CursorShape >> BYTE_SHIFT) & VIDEO_CURSOR_HIDDEN) != 0 || start > end || start >= (UINT)cellHeight;
            if (end >= (UINT)cellHeight) end = (UINT)cellHeight - 1u;
        }
        if (state->IsCursorBlink && state->TimeUs) {
            /* 16 frames on / 16 off at 60 Hz = a 533 ms period, lit for the first
               half. Integer maths only; no floating point in a VDD. */
            UINT64 phase = state->TimeUs() % VIDEO_CURSOR_BLINK_PERIOD_US;
            lit = (phase < VIDEO_CURSOR_BLINK_HALF_US);
        }
        if (!hidden && lit) {
            BYTE foreground = VideoDisplayCell(state, state->CursorRow, state->CursorColumn)[1] & VIDEO_ATTRIBUTE_COLOUR_MASK;
            for (glyphY = (INT)start; glyphY <= (INT)end; ++glyphY)
                for (glyphX = 0; glyphX < cellWidth; ++glyphX)              /* all nine: the CRTC does */
                    state->FrameBuffer[(state->CursorRow*cellHeight + glyphY) * stride
                           + state->CursorColumn*cellWidth + glyphX] = foreground;
        }
    }
}

/* ── ★ CURSOR EMULATION. See the note in vdd_video.h for WHY. ────────────────────
     The rule is the VGA BIOS's own (IBM's, as carried by Bochs/SeaBIOS), reproduced
     rather than approximated -- an approximation here shows up as a cursor a pixel
     or two out of place on every DOS prompt in existence:

         CH &= 0x3f;  CL &= 0x1f;
         if (cell > 8 && CL < 8 && CH < 0x20) {
             CH = (CL == CH + 1) ? ((CL + 1) * cell / 8) - 2
                                 : ((CH + 1) * cell / 8) - 1;
             CL = ((CL + 1) * cell / 8) - 1;
         }

     ⚠ THE `CL == CH + 1` BRANCH IS THE ONE THAT MATTERS and it looks like a special
       case for nothing. It is not: a two-line cursor (6-7) is DOS's UNDERLINE, and
       scaling both ends the ordinary way would give 13-15, a three-line smear. The
       branch keeps it two lines tall at the bottom of the cell.
     ⚠ `CL < 8` is what stops a shape that ALREADY knows about 16-line cells from
       being scaled twice; `CH < 0x20` leaves the hide bit alone. */
VOID VddCursorLines(WORD shape, UINT cellHeight,
                      UINT *start, UINT *end, INT *isHidden)
{
    UINT character = (shape >> BYTE_SHIFT) & VIDEO_CURSOR_START_FIELD;
    UINT endLine =  shape       & VIDEO_CURSOR_LINE_MASK;
    *isHidden = (character & VIDEO_CURSOR_HIDDEN) != 0;                 /* CH bit 5: cursor off        */
    character &= VIDEO_CURSOR_LINE_MASK;
    if (cellHeight > VIDEO_CGA_CELL_HEIGHT && endLine < VIDEO_CGA_CELL_HEIGHT && !*isHidden) {
        character = (endLine == character + 1u) ? ((endLine + 1u) * cellHeight / VIDEO_CGA_CELL_HEIGHT) - VIDEO_CURSOR_UNDERLINE_LINES
                             : ((character + 1u) * cellHeight / VIDEO_CGA_CELL_HEIGHT) - 1u;
        endLine = ((endLine + 1u) * cellHeight / VIDEO_CGA_CELL_HEIGHT) - 1u;
    }
    if (character >= cellHeight) character = cellHeight - 1u;
    if (endLine >= cellHeight) endLine = cellHeight - 1u;
    if (character > endLine) *isHidden = 1;                    /* the other "off" idiom       */
    *start = character; *end = endLine;
}

/* combine the 4 bit-planes into fb (16-colour indices) -- mode 12h. */
/* CGA modes 4/5/6 at B800.  The layout is the reason these were left out before:
   rows INTERLEAVE between two 8 KB banks -- even rows from offset 0, odd rows
   from 0x2000 -- and pixels are 2 bits (modes 4/5) or 1 bit (mode 6), packed
   high-bit-first.  Nothing about that is shared with the planar path, which is
   why approximating it with a text screen was never going to work. */
/* Mode 5's palette is the grey/brown variant; 4's default is cyan/magenta. Exported
   (vdd_video.h) because the INT 33h graphics cursor has to undo it: its masks act on the
   2-bit value in video memory, and the frame holds the colour this table made of it. */
/* s92: since #266 VideoRenderCga stores the 2-bit value ITSELF (an attribute-controller
   index; pal[] maps it through AR0n), so the frame holds what video memory holds and
   there is nothing to undo -- the identity. Kept as the cursor's one question to ask. */
const BYTE *VddVideoCga4Map(PCVIDEO_STATE state)
{
    static const BYTE ident[VIDEO_CGA_COLOURS] = { 0, 1, 2, 3 };
    (VOID)state;
    return ident;
}

static VOID VideoRenderCga(PVIDEO_STATE state)
{
    const BYTE *source = state->VideoMemory + VIDEO_TEXT_OFFSET;
    INT graphicsWidth = state->GraphicsWidth, graphicsHeight = state->GraphicsHeight, line, column;
    INT pixelsPerByte = state->CgaBpp == 1 ? VIDEO_PIXELS_PER_PLANE_BYTE : VIDEO_CGA_PIXELS_PER_BYTE;             /* pixels per byte          */
    /* ── #266: THE PIXEL VALUE IS AN ATTRIBUTE-CONTROLLER INDEX, as on the card: 0-3 in
         04h/05h (AR12 = 03h), 0-1 in 06h (AR12 = 01h), and pal[] carries it through
         AR0n -> DAC. This drew from a private table -- pixel 1/2/3 as index 11/13/15
         (or 10/12/14 after AH=0Bh BH=1) -- which came out right only while AR0B/0D/0F
         happened to equal what AR01-03 hold after a mode set (13h/15h/17h; they do,
         by the measured table), so a guest's own AR01-03, or the BIOS's background,
         never showed. Unchanged picture for an unmodified mode 04h/05h/06h. */
    for (line = 0; line < graphicsHeight; ++line) {
        const BYTE *sourceRow = source + ((line & 1) ? VIDEO_CGA_ODD_BANK : 0) + (line >> 1) * (graphicsWidth / pixelsPerByte);
        BYTE *output = &state->FrameBuffer[line * graphicsWidth];
        for (column = 0; column < graphicsWidth; ++column) {
            BYTE byteValue = sourceRow[column / pixelsPerByte];
            if (state->CgaBpp == 1)
                output[column] = (BYTE)((byteValue >> (VIDEO_PIXEL_IN_BYTE_MASK - (column & VIDEO_PIXEL_IN_BYTE_MASK))) & 1);
            else
                output[column] = (BYTE)((byteValue >> (VIDEO_CGA_PIXEL_TOP_SHIFT - VIDEO_CGA_BITS_PER_PIXEL * (column & VIDEO_CGA_PIXEL_MASK))) & VIDEO_CGA_PIXEL_MASK);
        }
    }
}

/* ── ★★★ THE PLANAR RENDERER HAS TO READ THE CRTC. (s64) ─────────────────────────
     This drew every frame from address 0 with a stride of gw/8, which is correct for
     exactly one thing: a stationary full-screen picture. That is what the test card
     draws, what the mode-12h demos draw, and what QBasic's BUBBLES draws -- so the
     whole planar suite passed while Lemmings' GAMEPLAY was garbled, because Lemmings
     SCROLLS. Measured on the rig, mid-level: `IsCrtcSeen=01 CrtcStart=0x000002c2`.
     The game had panned 706 bytes into the buffer and we were still rendering from
     the top of it.
   ⚠ ANIMATION IS NOT SCROLLING, and that distinction is why every other guest missed
     this. BUBBLES animates by REDRAWING PIXELS -- its start address never leaves 0.
     A scrolling game leaves the pixels alone and moves the WINDOW over them. Nothing
     but Lemmings, in everything tested, does the second.
   ★ VideoRenderModeY() has honoured both registers for ages -- Doom needed the start
     address to page-flip. The capability existed in one renderer and not its sibling,
     purely because nothing had yet asked the sibling for it.
   ⚠ THE OFFSET REGISTER COUNTS IN 2-BYTE UNITS, and its reset value of 40 is right
     for 12h and wrong for 0Dh -- hence IsCrtcOffsetSeen: use it only when the guest has
     actually written it, and fall back to the mode's natural stride otherwise.
   ⚠ Both values come from a guest register, so every plane index is wrapped into the
     plane rather than trusted: a mid-scroll write must not read off the end. */
/* ── #325: THE PICTURE'S SIZE AS THE CRTC IS PROGRAMMED, NOT AS THE MODE NUMBER SAYS.
     A game sets mode 13h and reprograms the CRTC -- Mode X is 320x240, others 360x480 --
     and the monitor shows what the registers say. Width: Horizontal Display End (CR01)
     + 1 character clocks of 8 dots, halved when the attribute controller pairs dots
     into 256-colour pixels (AR10 bit 6). Height: Vertical Display End (CR12 + overflow
     bits, + 1) scanlines, halved by double-scan (CR09 bit 7) and divided by the max
     scan line + 1 -- except where CGA addressing (CR17 bit 0 clear) uses those scan
     lines to interleave banks rather than to repeat lines (modes 4-6).
     Graphics modes only; trusted only when IsGeometryRegistersOk, otherwise the mode's table
     size. Checked against g_VideoModes[] for every measured mode in video_test. */
static VOID VideoGeometryOf(PCVIDEO_STATE state, INT *width, INT *height)
{
    UINT32 horizontalDisplayEnd, verticalDisplayEnd, overflow, maxScan;
    *width = state->GraphicsWidth; *height = state->GraphicsHeight;
    if (state->IsVesa || !state->IsGeometryRegistersOk) return;
    if (state->ModeKind != VIDEO_KIND_PLANAR && state->ModeKind != VIDEO_KIND_LINEAR8) return;
    horizontalDisplayEnd = ((UINT32)state->CrtcRegisters[VIDEO_CR_HORIZONTAL_DISPLAY_END] + 1u) * VIDEO_CHARACTER_CLOCK_8;
    if (state->AttributeMode & VIDEO_AR_MODE_PIXEL_DOUBLE) horizontalDisplayEnd /= VIDEO_PIXEL_DOUBLE;
    overflow  = state->CrtcRegisters[VIDEO_CR_OVERFLOW]; maxScan = state->CrtcRegisters[VIDEO_CR_MAX_SCAN];
    verticalDisplayEnd = ((UINT32)state->CrtcRegisters[VIDEO_CR_VERTICAL_DISPLAY_END] | ((overflow >> VIDEO_CR07_DISPLAY_END_8 & 1u) << VIDEO_BIT8_SHIFT) | ((overflow >> VIDEO_CR07_DISPLAY_END_9 & 1u) << VIDEO_BIT9_SHIFT)) + 1u;
    if (maxScan & VIDEO_CR09_DOUBLE_SCAN) verticalDisplayEnd /= VIDEO_DOUBLE_SCAN;
    if (state->CrtcRegisters[VIDEO_CR_MODE_CONTROL] & VIDEO_CR17_COMPATIBILITY) verticalDisplayEnd /= (maxScan & VIDEO_CR09_MAX_SCAN_MASK) + 1u;
    if (horizontalDisplayEnd < VIDEO_GEOMETRY_MIN_WIDTH || horizontalDisplayEnd > NTVDD_FRAME_MAX_WIDTH || verticalDisplayEnd < VIDEO_GEOMETRY_MIN_HEIGHT || verticalDisplayEnd > NTVDD_FRAME_MAX_HEIGHT) return;
    *width = (INT)horizontalDisplayEnd; *height = (INT)verticalDisplayEnd;
}
VOID VddVideoGeometry(PCVIDEO_STATE state, INT *width, INT *height) { VideoGeometryOf(state, width, height); }

static VOID VideoRenderPlanar(PVIDEO_STATE state)
{
    INT line, byteColumn, bit, graphicsWidth, graphicsHeight;
    VideoGeometryOf(state, &graphicsWidth, &graphicsHeight);                            /* #325: the CRTC's size */
    if (!graphicsWidth) graphicsWidth = VIDEO_MODE12_WIDTH;
    if (!graphicsHeight) graphicsHeight = VIDEO_MODE12_HEIGHT;
    UINT32 bytes = (UINT32)(graphicsWidth / VIDEO_PIXELS_PER_PLANE_BYTE);
    UINT32 pitch = (state->IsCrtcOffsetSeen && state->CrtcOffset)
                     ? (UINT32)state->CrtcOffset * VIDEO_CRTC_OFFSET_UNIT : bytes;
    /* The LATCHED start address, not the register pair -- see VideoCrtcOut case 0x0C. */
    UINT32 base  = state->IsCrtcSeen ? (UINT32)state->CrtcStartLive : 0u;
    /* ── SPLIT SCREEN. Below Line Compare the address generator restarts at 0, which
         is how a scrolling game pins a status panel to the bottom of the screen while
         the level pans behind it. Lemmings does exactly this, and without it the panel
         is drawn from the scrolled address -- the striped band under an otherwise
         correct level.
       ⚠ LINE COMPARE COUNTS SCANLINES, NOT OUR ROWS. A 200-line mode is displayed as
         400 scanlines (double-scanned), so a value that looks past the bottom of the
         picture is really in the doubled space -- halve it rather than ignoring it.
         A value that is still past the end after that means "no split", which is the
         power-on state (all ones) and must stay inert. */
    UINT32 split = (UINT32)graphicsHeight;                 /* gh = no split */
    if (state->CrtcLineCompare) {
        UINT32 lineCompare = state->CrtcLineCompare;
        if (lineCompare >= (UINT32)graphicsHeight && (lineCompare / VIDEO_DOUBLE_SCAN) < (UINT32)graphicsHeight) lineCompare /= VIDEO_DOUBLE_SCAN;
        if (lineCompare < (UINT32)graphicsHeight) split = lineCompare;
    }
    /* ► PEL PANNING (AR13): shift the picture left 0-7 pixels, the fine half of a smooth
         scroll (s83). Below the split line the pan is dropped when AR10 bit 5 (PPM) is
         set -- which is how a panel stays still under a panning playfield. */
    UINT32 pan  = (state->DisplayPan & VIDEO_AR_PAN_MASK) < VIDEO_PAN_PIXELS ? (UINT32)(state->DisplayPan & VIDEO_PAN_SHIFT_MASK) : 0u;
    INT      isPelPanMode  = (state->AttributeMode >> VIDEO_AR_MODE_PPM_SHIFT) & 1;
    if (!pitch) pitch = bytes;
    for (line = 0; line < graphicsHeight; ++line) {
        UINT32 rowAddress = (line < (INT)split) ? base + (UINT32)line * pitch
                                        : (UINT32)(line - (INT)split) * pitch;
        UINT32 shift  = (line >= (INT)split && isPelPanMode) ? 0u : pan;
        BYTE *output = &state->FrameBuffer[line * graphicsWidth];
        if (!shift) {
            for (byteColumn = 0; byteColumn < (INT)bytes; ++byteColumn) {
                UINT32 planeOffset = (rowAddress + (UINT32)byteColumn) % (UINT32)VIDEO_PLANE_SIZE;
                BYTE plane0 = VideoPlaneBytes(state,0)[planeOffset], plane1 = VideoPlaneBytes(state,1)[planeOffset];
                BYTE plane2 = VideoPlaneBytes(state,2)[planeOffset], plane3 = VideoPlaneBytes(state,3)[planeOffset];
                for (bit = 0; bit < BITS_PER_BYTE; ++bit) {
                    BYTE mask = (BYTE)(VIDEO_PIXEL_LEFT_BIT >> bit);
                    output[byteColumn*VIDEO_PIXELS_PER_PLANE_BYTE + bit] = (BYTE)(((plane0&mask)?1:0) | ((plane1&mask)?VIDEO_PLANE1_BIT:0) | ((plane2&mask)?VIDEO_PLANE2_BIT:0) | ((plane3&mask)?VIDEO_PLANE3_BIT:0));
                }
            }
        } else {
            INT column;
            for (column = 0; column < graphicsWidth; ++column) {
                UINT32 shiftedX = (UINT32)column + shift;
                UINT32 planeOffset  = (rowAddress + (shiftedX >> VIDEO_PLANE_BYTE_SHIFT)) % (UINT32)VIDEO_PLANE_SIZE;
                BYTE  mask  = (BYTE)(VIDEO_PIXEL_LEFT_BIT >> (shiftedX & VIDEO_PIXEL_IN_BYTE_MASK));
                output[column] = (BYTE)(((VideoPlaneBytes(state,0)[planeOffset]&mask)?1:0) | ((VideoPlaneBytes(state,1)[planeOffset]&mask)?VIDEO_PLANE1_BIT:0)
                                 | ((VideoPlaneBytes(state,2)[planeOffset]&mask)?VIDEO_PLANE2_BIT:0) | ((VideoPlaneBytes(state,3)[planeOffset]&mask)?VIDEO_PLANE3_BIT:0));
            }
        }
    }
}

/* Render the current mode into st->Frame each tick (always, so direct A0000
   writes show and the client stays refreshed). Does NOT blit -- the host presents
   st->Frame outside the bus lock so the slow blit never starves the V86 thread. */
/* Combine the snapshotted planes. pitch/start come from the CRTC in 2-byte units,
   which is how a mode-Y program page-flips. Masked so a mid-flip value cannot
   index outside the plane. */
/* WHICH PAGE IS ON SCREEN, without the CRTC. A mode-Y program page-flips by
   pointing the CRTC start at one of the 16000-byte pages, and we cannot watch that
   register (above). But the pages are in our snapshot, so pick the one that
   actually holds a picture: count non-zero bytes per page in plane 0 and take the
   busiest. Exact for a title/menu screen, which is what this is for; a game
   double-buffering two equally-busy pages may pick either, and that is a known
   limit rather than a surprise. */
static UINT32 VideoModeYPage(PCVIDEO_STATE state)
{
    /* ► PAGES ARE 0x4000 APART, NOT 16000. A 320x200 mode-Y page OCCUPIES 16000
         bytes per plane, but programs align the pages to 0x4000 so the page
         address is a shift rather than a multiply -- Doom's pagestart[] is
         0, 0x4000, 0x8000. Detecting on a 16000 stride put the start 384 bytes
         (16384-16000) below the real page base, which is 4.8 rows: the frame came
         out VERTICALLY ROTATED by ~5 rows, with the bottom of the picture stitched
         onto the top. Measured -- the largest row-to-row discontinuity in the
         captured frame sits at y=5. */
    UINT32 page = 0, bestCount = 0, candidatePage;
    for (candidatePage = 0; candidatePage < VIDEO_MODE_Y_PAGES; ++candidatePage) {
        UINT32 base = candidatePage * VIDEO_MODE_Y_PAGE_BYTES, index, count = 0;
        if (base + VIDEO_MODE_Y_PAGE_USED > VIDEO_Y_PLANE_SIZE) break;
        for (index = 0; index < VIDEO_MODE_Y_PAGE_USED; index += VIDEO_MODE_Y_SAMPLE_STRIDE) if (state->YPlanes[0][base + index]) ++count;
        if (count > bestCount) { bestCount = count; page = candidatePage; }
    }
    return page * VIDEO_MODE_Y_PAGE_BYTES;
}

static VOID VideoRenderModeY(PVIDEO_STATE state)
{
    UINT32 pitch = (UINT32)(state->CrtcOffset ? state->CrtcOffset : VIDEO_MODE_Y_DEFAULT_OFFSET) * VIDEO_CRTC_OFFSET_UNIT;
    /* ► READ THE PAGE FLIP; DO NOT GUESS IT. VideoModeYPage() picks the busiest page out
         of the snapshot, and its own commentary admits the limit: "a game
         double-buffering two equally-busy pages may pick either". Doom double-buffers
         every frame, so the guess alternated and the picture came out streaked with
         bands of the other buffer. It only ever existed because we could not watch the
         register -- claiming 0x3D4 regressed Doom three times for reasons recorded as
         UNKNOWN. The reason was that Doom flips with ONE 16-BIT WRITE,
             19fd4: mov edx,0x3d4 / add eax,0xc / out dx,ax
         and the handler took the whole word as an index and dropped the data, so
         claiming the port broke the flip outright -- strictly worse than not claiming
         it. With index+data writes honoured, the register is the answer. */
    UINT32 start = state->IsCrtcSeen ? state->CrtcStartLive : VideoModeYPage(state);
    /* ► PEL PANNING (AR13), IN 256-COLOUR UNITS: the value counts half-pixels here, so
         0/2/4/6 shift the picture left by 0-3 pixels -- the part of a smooth scroll the
         whole-byte start address cannot express (4 pixels per byte in this mode). The
         shifted row simply reads on into the next bytes, as the address counter does. */
    UINT32 pan = (UINT32)((state->DisplayPan & VIDEO_PAN_SHIFT_MASK) >> 1);
    /* ► THE SELECTED PLANE IS READ LIVE. Its most recent bytes are in the aperture
         and nowhere else -- once a static screen stops changing the mask, no further
         flush ever comes, and that plane's columns would render as whatever was last
         snapshotted. Only a SINGLE-plane mask can be attributed this way; with more
         bits set the aperture belongs to no one plane, so fall back to the snapshots. */
    /* Flush first, then render from the planes ONLY. There is no live-aperture read
       any more: under box updates the aperture is a mixture of whichever planes were
       written most recently, so reading it for the selected plane pulls in another
       plane's pixels -- which is the same error as the whole-aperture copy, wearing a
       different hat. VideoModeYFlush() has already moved everything that was written. */
    if (!state->YMapPlane) VideoModeYFlush(state);
    { INT line, column;
      const BYTE *plane[VIDEO_PLANES];
      for (column = 0; column < VIDEO_PLANES; ++column)
          plane[column] = state->YMapPlane ? state->YMapPlane(state->YMapContext, column) : state->YPlanes[column];
      INT modeWidth, modeHeight;
      VideoGeometryOf(state, &modeWidth, &modeHeight);                          /* #325: Mode X is 320x240 */
      for (line = 0; line < modeHeight; ++line) {
          UINT32 row = start + (UINT32)line * pitch;
          BYTE *destination = state->FrameBuffer + (UINT32)line * (UINT32)modeWidth;
          for (column = 0; column < modeWidth; ++column)
          { UINT32 shiftedX = (UINT32)column + pan;
            destination[column] = plane[shiftedX & VIDEO_PLANE_INDEX_MASK][(row + (shiftedX >> VIDEO_MODE_Y_PLANE_SHIFT)) & (VIDEO_Y_PLANE_SIZE - 1u)]; }
      } }
}

static VOID VideoOnFrame(PVOID context)
{
    PVIDEO_STATE state = (PVIDEO_STATE)context;
    if (!state->VideoMemory) return;
    /* ── THE START ADDRESS IS LATCHED ONCE PER FRAME, as the hardware's address
         counter loads it at the vertical retrace. Rendering straight from the
         register pair means a frame built between the two byte writes of a page flip
         shows half the old address and half the new. CrtcStartHalf counts the
         frames latched mid-pair -- real hardware tears there too, so this is a
         diagnostic and not a guarantee; a guest avoids it by writing both halves
         inside the retrace.
       ⚠ MEASURED ON LEMMINGS: 941 completed pairs, CrtcStartHalf = 0. So this is
         NOT the cause of the flicker it was written to explain. Kept because it is
         what the hardware does and the hazard is real for other guests. */
    VideoLatch(state, 1);                /* start address + pel panning as displayed (s83) */
    if (state->IsVesa) {                                 /* VESA: sync window -> vram */
        VideoVesaSync(state);
        state->Frame.Width = state->VesaWidth; state->Frame.Height = state->VesaHeight;
        if (state->VesaBpp > VIDEO_BPP_INDEXED) {                        /* direct colour -> ARGB */
            VideoVesaScanWritten(state);
            VideoVesaToArgb(state);
            state->Frame.BitsPerPixel = VIDEO_BPP_32;
            state->Frame.Stride = (UINT32)state->VesaWidth * VIDEO_BYTES_32BPP;
            state->Frame.Pixels = (const BYTE *)state->VesaArgb;
            state->Frame.Palette = 0;                     /* contract: NULL unless bpp==8 */
        } else {
            state->Frame.BitsPerPixel = VIDEO_BPP_INDEXED;
            /* ⚠ THE STRIDE IS THE MODE'S PITCH, NOT ITS WIDTH. Equal for 8bpp, and
                 that equality is why this read `= st->VesaWidth` and nobody noticed.
                 And the pitch is the 4F06 LOGICAL one, the origin the 4F07 start:
                 a page flip is nothing more than this pointer moving. */
            state->Frame.Stride = state->VesaStride ? state->VesaStride : state->VesaWidth;
            state->Frame.Pixels = state->VesaVram + state->VesaOriginLive; state->Frame.Palette = state->Palette;
        }
    } else if (state->ModeKind == VIDEO_KIND_LINEAR8 && !state->IsChain4) {  /* mode Y */
        /* ⚠ NO SNAPSHOT HERE. It used to capture the "live" plane at present time,
             but present time is an ARBITRARY moment: it can land mid-write, and
             which planes get overwritten then depends purely on timing. That made
             the picture NON-DETERMINISTIC -- the same binary produced a perfect
             title screen on one run and a coarse, blocky one on the next (observed
             directly on the physical screen; my own analysis missed it because I
             only ever inspected the richest captured frame, which hid the bad runs).
             The mask-change snapshot is well defined -- the outgoing plane is
             complete by then -- so rely on that alone. */
        INT modeWidth, modeHeight;
        VideoRenderModeY(state);
        VideoGeometryOf(state, &modeWidth, &modeHeight);                        /* #325 */
        state->Frame.Width = (WORD)modeWidth; state->Frame.Height = (WORD)modeHeight; state->Frame.BitsPerPixel = VIDEO_BPP_INDEXED;
        state->Frame.Stride = (UINT32)modeWidth; state->Frame.Pixels = state->FrameBuffer; state->Frame.Palette = state->Palette;
    } else if (state->ModeKind == VIDEO_KIND_LINEAR8) {        /* graphics: vmem is the FB */
        state->Frame.Width = state->GraphicsWidth; state->Frame.Height = state->GraphicsHeight; state->Frame.BitsPerPixel = VIDEO_BPP_INDEXED;
        state->Frame.Stride = state->GraphicsWidth; state->Frame.Pixels = state->VideoMemory; state->Frame.Palette = state->Palette;
    } else if (state->ModeKind == VIDEO_KIND_CGA) {            /* CGA: de-interleave -> fb */
        VideoRenderCga(state);
        state->Frame.Width = state->GraphicsWidth; state->Frame.Height = state->GraphicsHeight; state->Frame.BitsPerPixel = VIDEO_BPP_INDEXED;
        state->Frame.Stride = state->GraphicsWidth; state->Frame.Pixels = state->FrameBuffer; state->Frame.Palette = state->Palette;
    } else if (state->ModeKind == VIDEO_KIND_PLANAR) {         /* planar: combine -> fb    */
        INT pictureWidth, pictureHeight;
        VideoRenderPlanar(state);
        VideoGeometryOf(state, &pictureWidth, &pictureHeight);                        /* #325 */
        if (!pictureWidth) pictureWidth = VIDEO_MODE12_WIDTH;
        if (!pictureHeight) pictureHeight = VIDEO_MODE12_HEIGHT;
        state->Frame.Width = (WORD)pictureWidth; state->Frame.Height = (WORD)pictureHeight; state->Frame.BitsPerPixel = VIDEO_BPP_INDEXED;
        state->Frame.Stride = (UINT32)pictureWidth; state->Frame.Pixels = state->FrameBuffer; state->Frame.Palette = state->Palette;
    } else {                                           /* text: render glyphs      */
        /* Geometry now follows the MODE, not a fixed 80x25 -- a 40-column mode
           renders 320 pixels wide instead of pretending to be 640. */
        VddVideoRender(state);
        state->Frame.Width = (WORD)(state->Columns * VideoTextCellWidthOf(state));   /* #324: 9-dot cells */
        state->Frame.Height = (WORD)(state->Rows * (state->CellHeight ? state->CellHeight : VIDEO_CELL_HEIGHT));
        VddVideoBdaSync(state);                        /* cursor moved by teletype etc. */
        state->Frame.BitsPerPixel = VIDEO_BPP_INDEXED;
        state->Frame.Stride = state->Frame.Width;
        state->Frame.Pixels = state->FrameBuffer; state->Frame.Palette = state->Palette;
    }
    /* ── #266: SR1 BIT 5, "SCREEN OFF", BLANKS THE PICTURE. The sequencer stops feeding
         the attribute controller, so the monitor sees black for as long as the bit is
         set -- and the picture comes back untouched when it clears, because nothing
         in video memory moved. Programs set it to hide a redraw or a mode change, and
         AH=12h BL=36h (video refresh off) is the BIOS's door to the same bit. We stored
         it (VideoSequencerSetData; 12h BL=36h since #252) and drew the frame anyway.
       ► The frame keeps its geometry (the presenter's aspect does not jump) and goes out
         as 8bpp with a zero stride -- every row is fb's first row -- through an all-black
         palette, so the result is black whatever fb holds and nothing is rendered into
         or cleared. The render above still ran: state the frame derives (the BDA sync,
         VESA's window sync) must not stall while the screen is dark. frame_touch drops
         the raster-split arrays for a blanked frame. A mode set clears the bit (the
         measured SR1 of every mode has bit 5 = 0). */
    state->IsBlanked = (BYTE)((state->SequencerRegisters[VIDEO_SR_CLOCKING_MODE] & VIDEO_SR1_SCREEN_OFF) != 0);
    if (state->IsBlanked) {
        static UINT32 black[NTVDD_PALETTE_ENTRIES];
        if (!black[0]) { INT index; for (index = 0; index < NTVDD_PALETTE_ENTRIES; ++index) black[index] = VIDEO_ARGB_OPAQUE; }
        state->Frame.BitsPerPixel = VIDEO_BPP_INDEXED; state->Frame.Stride = 0;
        state->Frame.Pixels = state->FrameBuffer; state->Frame.Palette = black;
    }
    state->IsDirty = 0;
}

VOID VddVideoPutChar(PVIDEO_STATE state, BYTE character) { VideoTeletype(state, character); state->IsDirty = 1; }

/* ── THE REGISTER FILE AS TEXT. (docs/inventory/vga.md) ──────────────────────────────
     Two things, and the second is the one that matters: every index with its value,
     and then the indices the GUEST wrote. A value whose write count is zero is OUR
     reset default -- a statement about this host, not about the guest -- so printing
     the values alone would invite exactly the wrong reading. No CRT here (the VDD is
     built off-VM too), so the formatting is by hand. */
static char *VideoReadHex2(char *output, UINT value)
{
    static const char H[] = "0123456789ABCDEF";
    *output++ = H[(value >> NIBBLE_SHIFT) & VIDEO_NIBBLE_MASK]; *output++ = H[value & VIDEO_NIBBLE_MASK]; return output;
}
static char *VideoReadString(char *output, const char *text) { while (*text) *output++ = *text++; return output; }
static char *VideoReadDecimal(char *output, UINT value)
{
    char digits[VIDEO_DECIMAL_DIGITS]; INT count = 0;
    if (!value) { *output++ = '0'; return output; }
    while (value) { digits[count++] = (char)('0' + value % VIDEO_DECIMAL_BASE); value /= VIDEO_DECIMAL_BASE; }
    while (count) *output++ = digits[--count];
    return output;
}
/* "SEQ 00:03 01:01 ..." for one group, then "  written:" and the indices with w>0. */
static char *VideoReadGroup(char *output, const char *name, const BYTE *registers,
                      const UINT32 *width, INT count)
{
    INT index, isAny = 0;
    output = VideoReadString(output, "STAGE2: VGAREG "); output = VideoReadString(output, name);
    for (index = 0; index < count; ++index) {
        *output++ = ' '; output = VideoReadHex2(output, (UINT)index); *output++ = ':';
        output = VideoReadHex2(output, registers[index]);
    }
    output = VideoReadString(output, "\r\n");
    output = VideoReadString(output, "STAGE2: VGAREG "); output = VideoReadString(output, name);
    output = VideoReadString(output, " written-by-guest:");
    for (index = 0; index < count; ++index) {
        if (!width[index]) continue;
        isAny = 1; *output++ = ' '; output = VideoReadHex2(output, (UINT)index);
        *output++ = 'x'; output = VideoReadDecimal(output, width[index]);
    }
    if (!isAny) output = VideoReadString(output, " none");
    output = VideoReadString(output, "\r\n");
    return output;
}

INT VddVideoRegistersDump(PCVIDEO_STATE state, char *output, INT capacity)
{
    char *cursor = output;
    /* Worst case is the four groups at ~7 bytes an entry plus the externals; 1600 is
       comfortably over it. Refuse rather than overrun -- this runs inside the exit
       report, where a smashed buffer would take the whole report with it. */
    if (!state || !output || capacity < VIDEO_REGISTER_DUMP_MIN_CAPACITY) return 0;
    cursor = VideoReadString(cursor, "STAGE2: VGAREG ext misc=0x");   cursor = VideoReadHex2(cursor, state->MiscOutput);
    cursor = VideoReadString(cursor, "(w=");                          cursor = VideoReadDecimal(cursor, state->MiscWrites);
    cursor = VideoReadString(cursor, ") feat=0x");                    cursor = VideoReadHex2(cursor, state->FeatureControl);
    cursor = VideoReadString(cursor, "(w=");                          cursor = VideoReadDecimal(cursor, state->FeatureWrites);
    cursor = VideoReadString(cursor, ") dacmask=0x");                 cursor = VideoReadHex2(cursor, state->DacMask);
    cursor = VideoReadString(cursor, "(w=");                          cursor = VideoReadDecimal(cursor, state->DacMaskWrites);
    cursor = VideoReadString(cursor, ") vgaen=0x");                   cursor = VideoReadHex2(cursor, state->VgaEnable);
    cursor = VideoReadString(cursor, "(w=");                          cursor = VideoReadDecimal(cursor, state->VgaEnableWrites);
    /* Say so in the artefact, not only in the source: an unwritten external register
       is OUR spec-derived default and has never been checked against a real card. */
    cursor = VideoReadString(cursor, ") cr11wp_refused=");            cursor = VideoReadDecimal(cursor, state->CrtcWriteProtectRefused);
    cursor = VideoReadString(cursor, "  [unwritten externals are spec defaults;"
                  " InputStatus0 reads 0x10 -- MEASURED on PCem's IBM VGA ROM,"
                  " all 12 modes]\r\n");
    cursor = VideoReadGroup(cursor, "SEQ ", state->SequencerRegisters,  state->SequencerWrites,   VIDEO_SR_DECODED);
    cursor = VideoReadGroup(cursor, "CRTC", state->CrtcRegisters, state->CrtcWrites, VIDEO_CR_DECODED);
    cursor = VideoReadGroup(cursor, "GC  ", state->GcRegisters,   state->GcWrites,   VIDEO_GR_DECODED);
    cursor = VideoReadGroup(cursor, "AC  ", state->AttributeRegisters, state->AttributeWrites, VIDEO_AR_DECODED);
    return (INT)(cursor - output);
}

VOID VddVideoReset(PVOID context)
{
    PVIDEO_STATE state = (PVIDEO_STATE)context;
    /* ── THE EXTERNAL REGISTERS' POWER-ON VALUES. (inventory step 1) ────────────────
         Only reached once, from VddVideoInitialize -- a mode set does NOT come through
         here -- so the write counters below accumulate for the whole run and a
         `*_w == 0` really does mean "this guest never wrote that index".
       ⚠ MiscOutput = 0x67 is what a VGA BIOS leaves after a mode 3 set (colour at 3Dx,
         RAM enabled, 28 MHz clock, 400 lines: -hsync +vsync). It is a SPEC value, not
         a measured one -- vga_defaults.h is generated from a probe against genuine
         6.22 but covers only the AC palette and the DAC, so there is no measured
         table for the externals yet. Step 2's probe against the PCem ET4000 is what
         turns this from spec-correct into verified. Until then the dump marks it. */
    state->FeatureControl = 0x00; state->VgaEnable = VIDEO_VGA_ENABLED; state->DacMask = VIDEO_DAC_MASK_ALL;
    /* ⚠ AND THE POWER-ON STATE IS MODE 3'S, NOT ZEROES. A real machine reaches DOS
         with its BIOS having set mode 3, so the register file holds mode 3's values
         before a guest runs at all -- and most guests never call AH=00h for text,
         they just start drawing. Loading it here rather than only from the INT 10h
         path is what makes `VGAREG` describe a plausible card from the first line.
         VideoLoadModeDefinition sets MiscOutput too, so it is not set separately. */
    VideoLoadModeDefinition(state, VIDEO_MODE_TEXT_80);
    state->Mode = VIDEO_MODE_TEXT_80; state->Columns = VIDEO_COLUMNS; state->Rows = VIDEO_ROWS;
    state->ModeKind = VIDEO_KIND_TEXT; state->GraphicsWidth = VIDEO_FRAME_WIDTH; state->GraphicsHeight = VIDEO_FRAME_HEIGHT;
    state->CellHeight = VIDEO_CELL_HEIGHT; state->IsBlink = 1; state->IsUserFontOn = 0;
    state->AttributeMode = (BYTE)(state->AttributeMode | VIDEO_AR_MODE_BLINK);
    state->CrtcCursor = 0;
    state->ModeQueryCount = 0;
    state->CursorRow = state->CursorColumn = 0; state->CursorShape = VIDEO_CURSOR_SHAPE_DEFAULT; state->Page = 0;
    {   INT page; for (page = 0; page < VIDEO_PAGES; ++page) state->PageRow[page] = state->PageColumn[page] = 0; }
    state->ScanSelect = VIDEO_SCAN_SELECT_400; state->IsGreySum = 0; state->IsCursorEmulationOff = 0; state->IsVideoOff = 0;   /* #252 */
    state->DacWriteIndex = state->DacReadIndex = state->DacComponent = 0;
    state->SequencerIndex = state->GcIndex = 0;
    state->MapMask = VIDEO_ALL_PLANES; state->BitMask = VIDEO_BIT_MASK_ALL; state->WriteMode = 0;
    state->IsChain4 = 1; state->YMask = VIDEO_ALL_PLANES;
    VideoLoadDefaultCrtc(state);                      /* mode 3's CRTC, measured not assumed */
    state->SetReset = state->EnableSetReset = state->FunctionRotate = state->ReadMap = 0;
    state->ReadMode = state->ColorCompare = state->ColorDontCare = 0;
    state->Latch[0] = state->Latch[1] = state->Latch[2] = state->Latch[3] = 0;
    state->IsVesa = 0; state->VesaMode = 0; state->VesaBank = 0;
    state->VesaModeFlags = 0; state->IsModeSetNoClear = 0;   /* 40:87h = 60h at power-on */
    state->VesaDacWidth = VIDEO_DAC_WIDTH_6;                      /* the power-on RAMDAC is a VGA's: 6 bits */
    VideoLoadDefaultPalette(state);
    if (state->VideoMemory) VideoClearText(state, VIDEO_ATTRIBUTE_NORMAL);
    /* #266: the power-on font vectors and CGA colour select (mode 3's 30h). */
    state->CgaSelect = VIDEO_CGA_SELECT_DEFAULT; state->IsBlanked = 0;
    VideoInt43Rom(state, state->CellHeight);
    VideoInt1FRom(state);
    VddVideoBdaSync(state);
    state->IsDirty = 1;
}

INT VddVideoInitialize(PVDD_BUS bus, PVOID context)
{
    PVIDEO_STATE state = (PVIDEO_STATE)context;
    state->Bus = bus;
    /* Disarmed before the reset, so no offset a guest can produce matches. */
    state->WatchOffset = VIDEO_OFFSET_NONE;
    VddVideoReset(state);
    state->ModeYGap = VIDEO_MODEY_GAP_DEFAULT;
    if (VddClaimMemory(bus, VIDEO_TEXT_BASE, VIDEO_TEXT_WINDOW_BYTES, VideoRead, VideoWrite, state)) return -1;
    if (VddClaimInterrupt(bus, VIDEO_INT10_VECTOR, VideoInt10, state)) return -1;
    if (VddClaimPorts(bus, VIDEO_PORT_SEQUENCER_INDEX, VIDEO_PORT_SEQUENCER_DATA, VideoSequencerIn, VideoSequencerOut, state)) return -1;  /* Sequencer */
    /* ⚠ THE OLD WARNING HERE ("DO NOT CLAIM CRTC 0x3D4/0x3D5", three regressions,
         mechanism UNKNOWN) IS RESOLVED, not ignored. The mechanism was that Doom
         page-flips with ONE 16-BIT WRITE and these handlers dropped the data byte, so
         claiming the port broke the flip outright -- worse than not claiming it. See
         VideoSequencerOut()/VideoIndexData(). */
    if (VddClaimPorts(bus, VIDEO_PORT_AC_WRITE, VIDEO_PORT_AC_READ, VideoAttributeIn, VideoAttributeOut, state)) return -1; /* Attribute */
    if (VddClaimPorts(bus, VIDEO_PORT_DAC_READ_INDEX, VIDEO_PORT_DAC_DATA, VideoDacIn, VideoDacOut, state)) return -1;  /* DAC       */
    if (VddClaimPorts(bus, VIDEO_PORT_GC_INDEX, VIDEO_PORT_GC_DATA, VideoGcIn, VideoGcOut, state)) return -1;    /* Graphics  */
    /* CRTC. Claimed at last -- see VideoRenderModeY() for why three earlier attempts
       regressed Doom and why that cause is gone. */
    if (VddClaimPorts(bus, VIDEO_PORT_CRTC_COLOUR, VIDEO_PORT_CRTC_COLOUR_DATA, VideoCrtcIn, VideoCrtcOut, state)) return -1; /* CRTC     */
    if (VddClaimPorts(bus, VIDEO_PORT_STATUS1_COLOUR, VIDEO_PORT_STATUS1_COLOUR, VideoStatusIn, VideoStatusOut, state)) return -1; /* InpStatus1 */
    /* ── THE EXTERNAL REGISTERS AND THE MONOCHROME ALIASES. (inventory step 1) ──────
         3C2 Misc Output / Input Status 0, 3C3 VGA Enable, 3C6 DAC Pixel Mask,
         3CA Feature Control read, 3CC Misc Output read -- none of which any device
         claimed, so every write was lost and every read answered 0xFF.
       ⚠ 3CB decodes nothing on a VGA and is inside the 3CA-3CC range; VideoExternalIn answers
         it 0xFF, which is what an unclaimed port answered before, so nothing changes
         for a guest that touches it.
       ⚠ THE MONO ALIASES ARE REAL. vdd_video.c already COMPUTES 0x3B4 for mode 7
         (see the BDA sync) while no handler was registered for it, so a mode-7 guest
         programming its CRTC wrote into nothing. 3B4/3B5 are the same CRTC and 3BA the
         same Input Status 1 / Feature Control as their 3Dx twins -- the same handlers,
         because they are the same registers. */
    if (VddClaimPorts(bus, VIDEO_PORT_MISC_WRITE, VIDEO_PORT_VGA_ENABLE, VideoExternalIn, VideoExternalOut, state)) return -1;  /* MiscOut/Enable */
    if (VddClaimPorts(bus, VIDEO_PORT_DAC_MASK, VIDEO_PORT_DAC_MASK, VideoExternalIn, VideoExternalOut, state)) return -1;  /* DAC pixel mask */
    if (VddClaimPorts(bus, VIDEO_PORT_FEATURE_READ, VIDEO_PORT_MISC_READ, VideoExternalIn, VideoExternalOut, state)) return -1;  /* FeatCtl/MiscOut */
    if (VddClaimPorts(bus, VIDEO_PORT_CRTC_MONO, VIDEO_PORT_CRTC_MONO_DATA, VideoCrtcIn, VideoCrtcOut, state)) return -1; /* mono CRTC */
    if (VddClaimPorts(bus, VIDEO_PORT_STATUS1_MONO, VIDEO_PORT_STATUS1_MONO, VideoStatusIn, VideoStatusOut, state)) return -1; /* mono status */
    if (VddClaimPorts(bus, VIDEO_PORT_VBE_INDEX, VIDEO_PORT_VBE_DATA, VideoVbePortIn, VideoVbePortOut, state)) return -1; /* #53: 4F0Ah's ports */
    if (VddOnFrame(bus, VideoOnFrame, state)) return -1;
    return 0;
}

/* NTVDMEX -- An NTVDM replacement for Microsoft Windows
 *
 * The Windows BMP file format the host writes (screenshots, captures) (#333).
 *
 * Defines only.
 *
 *
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Matthew Layton
 */

#ifndef NTVDMEX_VDD_BMP_FORMAT_H
#define NTVDMEX_VDD_BMP_FORMAT_H

#define BMP_FILE_HEADER_BYTES           14
#define BMP_INFO_HEADER_BYTES           40
#define BMP_QUAD_BYTES                  4       /* RGBQUAD */
#define BMP_ROW_SLACK                   4
#define BMP_ROW_PAD                     3       /* Rows are padded to 4 bytes */
#define BMP_ROW_ALIGN_MASK              3u
#define BMP_PIXELS_PER_METRE            2835    /* ~72 dpi */
#define BMP_FILE_SIZE_OFFSET            2
#define BMP_RESERVED1_OFFSET            6
#define BMP_RESERVED2_OFFSET            8
#define BMP_DATA_OFFSET_OFFSET          10
#define BMP_INFO_SIZE_OFFSET            0
#define BMP_WIDTH_OFFSET                4
#define BMP_HEIGHT_OFFSET               8
#define BMP_PLANES_OFFSET               12
#define BMP_BPP_OFFSET                  14
#define BMP_COMPRESSION_OFFSET          16
#define BMP_IMAGE_SIZE_OFFSET           20
#define BMP_X_PPM_OFFSET                24
#define BMP_Y_PPM_OFFSET                28
#define BMP_COLOURS_USED_OFFSET         32
#define BMP_COLOURS_IMPORTANT_OFFSET    36
#define BMP_PALETTED_BPP                8       /* An 8-bit paletted DIB */
#define BMP_PALETTE_ENTRIES             256
#define BMP_XRGB_BPP                    32      /* A 32-bit XRGB DIB */
#define BMP_XRGB_PIXEL_BYTES            4u
#define BMP_XRGB_ALPHA_OFFSET           3       /* The X byte of each XRGB pixel */

#endif /* NTVDMEX_VDD_BMP_FORMAT_H */

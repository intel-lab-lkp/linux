// SPDX-License-Identifier: GPL-2.0
/******************************************************************************
 *
 * Copyright(c) 2007 - 2011 Realtek Corporation. All rights reserved.
 *
 ******************************************************************************/

#include "odm_precomp.h"

/*  Global var */

u32 OFDMSwingTable[OFDM_TABLE_SIZE] = {
	0x7f8001fe, /*  0, +6.0dB */
	0x788001e2, /*  1, +5.5dB */
	0x71c001c7, /*  2, +5.0dB */
	0x6b8001ae, /*  3, +4.5dB */
	0x65400195, /*  4, +4.0dB */
	0x5fc0017f, /*  5, +3.5dB */
	0x5a400169, /*  6, +3.0dB */
	0x55400155, /*  7, +2.5dB */
	0x50800142, /*  8, +2.0dB */
	0x4c000130, /*  9, +1.5dB */
	0x47c0011f, /*  10, +1.0dB */
	0x43c0010f, /*  11, +0.5dB */
	0x40000100, /*  12, +0dB */
	0x3c8000f2, /*  13, -0.5dB */
	0x390000e4, /*  14, -1.0dB */
	0x35c000d7, /*  15, -1.5dB */
	0x32c000cb, /*  16, -2.0dB */
	0x300000c0, /*  17, -2.5dB */
	0x2d4000b5, /*  18, -3.0dB */
	0x2ac000ab, /*  19, -3.5dB */
	0x288000a2, /*  20, -4.0dB */
	0x26000098, /*  21, -4.5dB */
	0x24000090, /*  22, -5.0dB */
	0x22000088, /*  23, -5.5dB */
	0x20000080, /*  24, -6.0dB */
	0x1e400079, /*  25, -6.5dB */
	0x1c800072, /*  26, -7.0dB */
	0x1b00006c, /*  27. -7.5dB */
	0x19800066, /*  28, -8.0dB */
	0x18000060, /*  29, -8.5dB */
	0x16c0005b, /*  30, -9.0dB */
	0x15800056, /*  31, -9.5dB */
	0x14400051, /*  32, -10.0dB */
	0x1300004c, /*  33, -10.5dB */
	0x12000048, /*  34, -11.0dB */
	0x11000044, /*  35, -11.5dB */
	0x10000040, /*  36, -12.0dB */
};

u8 CCKSwingTable_Ch1_Ch13[CCK_TABLE_SIZE][8] = {
	{0x36, 0x35, 0x2e, 0x25, 0x1c, 0x12, 0x09, 0x04}, /*  0, +0dB */
	{0x33, 0x32, 0x2b, 0x23, 0x1a, 0x11, 0x08, 0x04}, /*  1, -0.5dB */
	{0x30, 0x2f, 0x29, 0x21, 0x19, 0x10, 0x08, 0x03}, /*  2, -1.0dB */
	{0x2d, 0x2d, 0x27, 0x1f, 0x18, 0x0f, 0x08, 0x03}, /*  3, -1.5dB */
	{0x2b, 0x2a, 0x25, 0x1e, 0x16, 0x0e, 0x07, 0x03}, /*  4, -2.0dB */
	{0x28, 0x28, 0x22, 0x1c, 0x15, 0x0d, 0x07, 0x03}, /*  5, -2.5dB */
	{0x26, 0x25, 0x21, 0x1b, 0x14, 0x0d, 0x06, 0x03}, /*  6, -3.0dB */
	{0x24, 0x23, 0x1f, 0x19, 0x13, 0x0c, 0x06, 0x03}, /*  7, -3.5dB */
	{0x22, 0x21, 0x1d, 0x18, 0x11, 0x0b, 0x06, 0x02}, /*  8, -4.0dB */
	{0x20, 0x20, 0x1b, 0x16, 0x11, 0x08, 0x05, 0x02}, /*  9, -4.5dB */
	{0x1f, 0x1e, 0x1a, 0x15, 0x10, 0x0a, 0x05, 0x02}, /*  10, -5.0dB */
	{0x1d, 0x1c, 0x18, 0x14, 0x0f, 0x0a, 0x05, 0x02}, /*  11, -5.5dB */
	{0x1b, 0x1a, 0x17, 0x13, 0x0e, 0x09, 0x04, 0x02}, /*  12, -6.0dB <== default */
	{0x1a, 0x19, 0x16, 0x12, 0x0d, 0x09, 0x04, 0x02}, /*  13, -6.5dB */
	{0x18, 0x17, 0x15, 0x11, 0x0c, 0x08, 0x04, 0x02}, /*  14, -7.0dB */
	{0x17, 0x16, 0x13, 0x10, 0x0c, 0x08, 0x04, 0x02}, /*  15, -7.5dB */
	{0x16, 0x15, 0x12, 0x0f, 0x0b, 0x07, 0x04, 0x01}, /*  16, -8.0dB */
	{0x14, 0x14, 0x11, 0x0e, 0x0b, 0x07, 0x03, 0x02}, /*  17, -8.5dB */
	{0x13, 0x13, 0x10, 0x0d, 0x0a, 0x06, 0x03, 0x01}, /*  18, -9.0dB */
	{0x12, 0x12, 0x0f, 0x0c, 0x09, 0x06, 0x03, 0x01}, /*  19, -9.5dB */
	{0x11, 0x11, 0x0f, 0x0c, 0x09, 0x06, 0x03, 0x01}, /*  20, -10.0dB */
	{0x10, 0x10, 0x0e, 0x0b, 0x08, 0x05, 0x03, 0x01}, /*  21, -10.5dB */
	{0x0f, 0x0f, 0x0d, 0x0b, 0x08, 0x05, 0x03, 0x01}, /*  22, -11.0dB */
	{0x0e, 0x0e, 0x0c, 0x0a, 0x08, 0x05, 0x02, 0x01}, /*  23, -11.5dB */
	{0x0d, 0x0d, 0x0c, 0x0a, 0x07, 0x05, 0x02, 0x01}, /*  24, -12.0dB */
	{0x0d, 0x0c, 0x0b, 0x09, 0x07, 0x04, 0x02, 0x01}, /*  25, -12.5dB */
	{0x0c, 0x0c, 0x0a, 0x09, 0x06, 0x04, 0x02, 0x01}, /*  26, -13.0dB */
	{0x0b, 0x0b, 0x0a, 0x08, 0x06, 0x04, 0x02, 0x01}, /*  27, -13.5dB */
	{0x0b, 0x0a, 0x09, 0x08, 0x06, 0x04, 0x02, 0x01}, /*  28, -14.0dB */
	{0x0a, 0x0a, 0x09, 0x07, 0x05, 0x03, 0x02, 0x01}, /*  29, -14.5dB */
	{0x0a, 0x09, 0x08, 0x07, 0x05, 0x03, 0x02, 0x01}, /*  30, -15.0dB */
	{0x09, 0x09, 0x08, 0x06, 0x05, 0x03, 0x01, 0x01}, /*  31, -15.5dB */
	{0x09, 0x08, 0x07, 0x06, 0x04, 0x03, 0x01, 0x01}	/*  32, -16.0dB */
};

u8 CCKSwingTable_Ch14[CCK_TABLE_SIZE][8] = {
	{0x36, 0x35, 0x2e, 0x1b, 0x00, 0x00, 0x00, 0x00}, /*  0, +0dB */
	{0x33, 0x32, 0x2b, 0x19, 0x00, 0x00, 0x00, 0x00}, /*  1, -0.5dB */
	{0x30, 0x2f, 0x29, 0x18, 0x00, 0x00, 0x00, 0x00}, /*  2, -1.0dB */
	{0x2d, 0x2d, 0x17, 0x17, 0x00, 0x00, 0x00, 0x00}, /*  3, -1.5dB */
	{0x2b, 0x2a, 0x25, 0x15, 0x00, 0x00, 0x00, 0x00}, /*  4, -2.0dB */
	{0x28, 0x28, 0x24, 0x14, 0x00, 0x00, 0x00, 0x00}, /*  5, -2.5dB */
	{0x26, 0x25, 0x21, 0x13, 0x00, 0x00, 0x00, 0x00}, /*  6, -3.0dB */
	{0x24, 0x23, 0x1f, 0x12, 0x00, 0x00, 0x00, 0x00}, /*  7, -3.5dB */
	{0x22, 0x21, 0x1d, 0x11, 0x00, 0x00, 0x00, 0x00}, /*  8, -4.0dB */
	{0x20, 0x20, 0x1b, 0x10, 0x00, 0x00, 0x00, 0x00}, /*  9, -4.5dB */
	{0x1f, 0x1e, 0x1a, 0x0f, 0x00, 0x00, 0x00, 0x00}, /*  10, -5.0dB */
	{0x1d, 0x1c, 0x18, 0x0e, 0x00, 0x00, 0x00, 0x00}, /*  11, -5.5dB */
	{0x1b, 0x1a, 0x17, 0x0e, 0x00, 0x00, 0x00, 0x00}, /*  12, -6.0dB  <== default */
	{0x1a, 0x19, 0x16, 0x0d, 0x00, 0x00, 0x00, 0x00}, /*  13, -6.5dB */
	{0x18, 0x17, 0x15, 0x0c, 0x00, 0x00, 0x00, 0x00}, /*  14, -7.0dB */
	{0x17, 0x16, 0x13, 0x0b, 0x00, 0x00, 0x00, 0x00}, /*  15, -7.5dB */
	{0x16, 0x15, 0x12, 0x0b, 0x00, 0x00, 0x00, 0x00}, /*  16, -8.0dB */
	{0x14, 0x14, 0x11, 0x0a, 0x00, 0x00, 0x00, 0x00}, /*  17, -8.5dB */
	{0x13, 0x13, 0x10, 0x0a, 0x00, 0x00, 0x00, 0x00}, /*  18, -9.0dB */
	{0x12, 0x12, 0x0f, 0x09, 0x00, 0x00, 0x00, 0x00}, /*  19, -9.5dB */
	{0x11, 0x11, 0x0f, 0x09, 0x00, 0x00, 0x00, 0x00}, /*  20, -10.0dB */
	{0x10, 0x10, 0x0e, 0x08, 0x00, 0x00, 0x00, 0x00}, /*  21, -10.5dB */
	{0x0f, 0x0f, 0x0d, 0x08, 0x00, 0x00, 0x00, 0x00}, /*  22, -11.0dB */
	{0x0e, 0x0e, 0x0c, 0x07, 0x00, 0x00, 0x00, 0x00}, /*  23, -11.5dB */
	{0x0d, 0x0d, 0x0c, 0x07, 0x00, 0x00, 0x00, 0x00}, /*  24, -12.0dB */
	{0x0d, 0x0c, 0x0b, 0x06, 0x00, 0x00, 0x00, 0x00}, /*  25, -12.5dB */
	{0x0c, 0x0c, 0x0a, 0x06, 0x00, 0x00, 0x00, 0x00}, /*  26, -13.0dB */
	{0x0b, 0x0b, 0x0a, 0x06, 0x00, 0x00, 0x00, 0x00}, /*  27, -13.5dB */
	{0x0b, 0x0a, 0x09, 0x05, 0x00, 0x00, 0x00, 0x00}, /*  28, -14.0dB */
	{0x0a, 0x0a, 0x09, 0x05, 0x00, 0x00, 0x00, 0x00}, /*  29, -14.5dB */
	{0x0a, 0x09, 0x08, 0x05, 0x00, 0x00, 0x00, 0x00}, /*  30, -15.0dB */
	{0x09, 0x09, 0x08, 0x05, 0x00, 0x00, 0x00, 0x00}, /*  31, -15.5dB */
	{0x09, 0x08, 0x07, 0x04, 0x00, 0x00, 0x00, 0x00}	/*  32, -16.0dB */
};

u32 OFDMSwingTable_New[OFDM_TABLE_SIZE] = {
	0x0b40002d, /*  0,  -15.0dB */
	0x0c000030, /*  1,  -14.5dB */
	0x0cc00033, /*  2,  -14.0dB */
	0x0d800036, /*  3,  -13.5dB */
	0x0e400039, /*  4,  -13.0dB */
	0x0f00003c, /*  5,  -12.5dB */
	0x10000040, /*  6,  -12.0dB */
	0x11000044, /*  7,  -11.5dB */
	0x12000048, /*  8,  -11.0dB */
	0x1300004c, /*  9,  -10.5dB */
	0x14400051, /*  10, -10.0dB */
	0x15800056, /*  11, -9.5dB */
	0x16c0005b, /*  12, -9.0dB */
	0x18000060, /*  13, -8.5dB */
	0x19800066, /*  14, -8.0dB */
	0x1b00006c, /*  15, -7.5dB */
	0x1c800072, /*  16, -7.0dB */
	0x1e400079, /*  17, -6.5dB */
	0x20000080, /*  18, -6.0dB */
	0x22000088, /*  19, -5.5dB */
	0x24000090, /*  20, -5.0dB */
	0x26000098, /*  21, -4.5dB */
	0x288000a2, /*  22, -4.0dB */
	0x2ac000ab, /*  23, -3.5dB */
	0x2d4000b5, /*  24, -3.0dB */
	0x300000c0, /*  25, -2.5dB */
	0x32c000cb, /*  26, -2.0dB */
	0x35c000d7, /*  27, -1.5dB */
	0x390000e4, /*  28, -1.0dB */
	0x3c8000f2, /*  29, -0.5dB */
	0x40000100, /*  30, +0dB */
	0x43c0010f, /*  31, +0.5dB */
	0x47c0011f, /*  32, +1.0dB */
	0x4c000130, /*  33, +1.5dB */
	0x50800142, /*  34, +2.0dB */
	0x55400155, /*  35, +2.5dB */
	0x5a400169, /*  36, +3.0dB */
	0x5fc0017f, /*  37, +3.5dB */
	0x65400195, /*  38, +4.0dB */
	0x6b8001ae, /*  39, +4.5dB */
	0x71c001c7, /*  40, +5.0dB */
	0x788001e2, /*  41, +5.5dB */
	0x7f8001fe  /*  42, +6.0dB */
};

u8 CCKSwingTable_Ch1_Ch13_New[CCK_TABLE_SIZE][8] = {
	{0x09, 0x08, 0x07, 0x06, 0x04, 0x03, 0x01, 0x01}, /*   0, -16.0dB */
	{0x09, 0x09, 0x08, 0x06, 0x05, 0x03, 0x01, 0x01}, /*   1, -15.5dB */
	{0x0a, 0x09, 0x08, 0x07, 0x05, 0x03, 0x02, 0x01}, /*   2, -15.0dB */
	{0x0a, 0x0a, 0x09, 0x07, 0x05, 0x03, 0x02, 0x01}, /*   3, -14.5dB */
	{0x0b, 0x0a, 0x09, 0x08, 0x06, 0x04, 0x02, 0x01}, /*   4, -14.0dB */
	{0x0b, 0x0b, 0x0a, 0x08, 0x06, 0x04, 0x02, 0x01}, /*   5, -13.5dB */
	{0x0c, 0x0c, 0x0a, 0x09, 0x06, 0x04, 0x02, 0x01}, /*   6, -13.0dB */
	{0x0d, 0x0c, 0x0b, 0x09, 0x07, 0x04, 0x02, 0x01}, /*   7, -12.5dB */
	{0x0d, 0x0d, 0x0c, 0x0a, 0x07, 0x05, 0x02, 0x01}, /*   8, -12.0dB */
	{0x0e, 0x0e, 0x0c, 0x0a, 0x08, 0x05, 0x02, 0x01}, /*   9, -11.5dB */
	{0x0f, 0x0f, 0x0d, 0x0b, 0x08, 0x05, 0x03, 0x01}, /*  10, -11.0dB */
	{0x10, 0x10, 0x0e, 0x0b, 0x08, 0x05, 0x03, 0x01}, /*  11, -10.5dB */
	{0x11, 0x11, 0x0f, 0x0c, 0x09, 0x06, 0x03, 0x01}, /*  12, -10.0dB */
	{0x12, 0x12, 0x0f, 0x0c, 0x09, 0x06, 0x03, 0x01}, /*  13, -9.5dB */
	{0x13, 0x13, 0x10, 0x0d, 0x0a, 0x06, 0x03, 0x01}, /*  14, -9.0dB */
	{0x14, 0x14, 0x11, 0x0e, 0x0b, 0x07, 0x03, 0x02}, /*  15, -8.5dB */
	{0x16, 0x15, 0x12, 0x0f, 0x0b, 0x07, 0x04, 0x01}, /*  16, -8.0dB */
	{0x17, 0x16, 0x13, 0x10, 0x0c, 0x08, 0x04, 0x02}, /*  17, -7.5dB */
	{0x18, 0x17, 0x15, 0x11, 0x0c, 0x08, 0x04, 0x02}, /*  18, -7.0dB */
	{0x1a, 0x19, 0x16, 0x12, 0x0d, 0x09, 0x04, 0x02}, /*  19, -6.5dB */
	{0x1b, 0x1a, 0x17, 0x13, 0x0e, 0x09, 0x04, 0x02}, /*  20, -6.0dB */
	{0x1d, 0x1c, 0x18, 0x14, 0x0f, 0x0a, 0x05, 0x02}, /*  21, -5.5dB */
	{0x1f, 0x1e, 0x1a, 0x15, 0x10, 0x0a, 0x05, 0x02}, /*  22, -5.0dB */
	{0x20, 0x20, 0x1b, 0x16, 0x11, 0x08, 0x05, 0x02}, /*  23, -4.5dB */
	{0x22, 0x21, 0x1d, 0x18, 0x11, 0x0b, 0x06, 0x02}, /*  24, -4.0dB */
	{0x24, 0x23, 0x1f, 0x19, 0x13, 0x0c, 0x06, 0x03}, /*  25, -3.5dB */
	{0x26, 0x25, 0x21, 0x1b, 0x14, 0x0d, 0x06, 0x03}, /*  26, -3.0dB */
	{0x28, 0x28, 0x22, 0x1c, 0x15, 0x0d, 0x07, 0x03}, /*  27, -2.5dB */
	{0x2b, 0x2a, 0x25, 0x1e, 0x16, 0x0e, 0x07, 0x03}, /*  28, -2.0dB */
	{0x2d, 0x2d, 0x27, 0x1f, 0x18, 0x0f, 0x08, 0x03}, /*  29, -1.5dB */
	{0x30, 0x2f, 0x29, 0x21, 0x19, 0x10, 0x08, 0x03}, /*  30, -1.0dB */
	{0x33, 0x32, 0x2b, 0x23, 0x1a, 0x11, 0x08, 0x04}, /*  31, -0.5dB */
	{0x36, 0x35, 0x2e, 0x25, 0x1c, 0x12, 0x09, 0x04}	/*  32, +0dB */
};

u8 CCKSwingTable_Ch14_New[CCK_TABLE_SIZE][8] = {
	{0x09, 0x08, 0x07, 0x04, 0x00, 0x00, 0x00, 0x00}, /*   0, -16.0dB */
	{0x09, 0x09, 0x08, 0x05, 0x00, 0x00, 0x00, 0x00}, /*   1, -15.5dB */
	{0x0a, 0x09, 0x08, 0x05, 0x00, 0x00, 0x00, 0x00}, /*   2, -15.0dB */
	{0x0a, 0x0a, 0x09, 0x05, 0x00, 0x00, 0x00, 0x00}, /*   3, -14.5dB */
	{0x0b, 0x0a, 0x09, 0x05, 0x00, 0x00, 0x00, 0x00}, /*   4, -14.0dB */
	{0x0b, 0x0b, 0x0a, 0x06, 0x00, 0x00, 0x00, 0x00}, /*   5, -13.5dB */
	{0x0c, 0x0c, 0x0a, 0x06, 0x00, 0x00, 0x00, 0x00}, /*   6, -13.0dB */
	{0x0d, 0x0c, 0x0b, 0x06, 0x00, 0x00, 0x00, 0x00}, /*   7, -12.5dB */
	{0x0d, 0x0d, 0x0c, 0x07, 0x00, 0x00, 0x00, 0x00}, /*   8, -12.0dB */
	{0x0e, 0x0e, 0x0c, 0x07, 0x00, 0x00, 0x00, 0x00}, /*   9, -11.5dB */
	{0x0f, 0x0f, 0x0d, 0x08, 0x00, 0x00, 0x00, 0x00}, /*  10, -11.0dB */
	{0x10, 0x10, 0x0e, 0x08, 0x00, 0x00, 0x00, 0x00}, /*  11, -10.5dB */
	{0x11, 0x11, 0x0f, 0x09, 0x00, 0x00, 0x00, 0x00}, /*  12, -10.0dB */
	{0x12, 0x12, 0x0f, 0x09, 0x00, 0x00, 0x00, 0x00}, /*  13, -9.5dB */
	{0x13, 0x13, 0x10, 0x0a, 0x00, 0x00, 0x00, 0x00}, /*  14, -9.0dB */
	{0x14, 0x14, 0x11, 0x0a, 0x00, 0x00, 0x00, 0x00}, /*  15, -8.5dB */
	{0x16, 0x15, 0x12, 0x0b, 0x00, 0x00, 0x00, 0x00}, /*  16, -8.0dB */
	{0x17, 0x16, 0x13, 0x0b, 0x00, 0x00, 0x00, 0x00}, /*  17, -7.5dB */
	{0x18, 0x17, 0x15, 0x0c, 0x00, 0x00, 0x00, 0x00}, /*  18, -7.0dB */
	{0x1a, 0x19, 0x16, 0x0d, 0x00, 0x00, 0x00, 0x00}, /*  19, -6.5dB */
	{0x1b, 0x1a, 0x17, 0x0e, 0x00, 0x00, 0x00, 0x00}, /*  20, -6.0dB */
	{0x1d, 0x1c, 0x18, 0x0e, 0x00, 0x00, 0x00, 0x00}, /*  21, -5.5dB */
	{0x1f, 0x1e, 0x1a, 0x0f, 0x00, 0x00, 0x00, 0x00}, /*  22, -5.0dB */
	{0x20, 0x20, 0x1b, 0x10, 0x00, 0x00, 0x00, 0x00}, /*  23, -4.5dB */
	{0x22, 0x21, 0x1d, 0x11, 0x00, 0x00, 0x00, 0x00}, /*  24, -4.0dB */
	{0x24, 0x23, 0x1f, 0x12, 0x00, 0x00, 0x00, 0x00}, /*  25, -3.5dB */
	{0x26, 0x25, 0x21, 0x13, 0x00, 0x00, 0x00, 0x00}, /*  26, -3.0dB */
	{0x28, 0x28, 0x24, 0x14, 0x00, 0x00, 0x00, 0x00}, /*  27, -2.5dB */
	{0x2b, 0x2a, 0x25, 0x15, 0x00, 0x00, 0x00, 0x00}, /*  28, -2.0dB */
	{0x2d, 0x2d, 0x17, 0x17, 0x00, 0x00, 0x00, 0x00}, /*  29, -1.5dB */
	{0x30, 0x2f, 0x29, 0x18, 0x00, 0x00, 0x00, 0x00}, /*  30, -1.0dB */
	{0x33, 0x32, 0x2b, 0x19, 0x00, 0x00, 0x00, 0x00}, /*  31, -0.5dB */
	{0x36, 0x35, 0x2e, 0x1b, 0x00, 0x00, 0x00, 0x00}	/*  32, +0dB */
};

u32 TxScalingTable_Jaguar[TXSCALE_TABLE_SIZE] = {
	0x081, /*  0,  -12.0dB */
	0x088, /*  1,  -11.5dB */
	0x090, /*  2,  -11.0dB */
	0x099, /*  3,  -10.5dB */
	0x0A2, /*  4,  -10.0dB */
	0x0AC, /*  5,  -9.5dB */
	0x0B6, /*  6,  -9.0dB */
	0x0C0, /*  7,  -8.5dB */
	0x0CC, /*  8,  -8.0dB */
	0x0D8, /*  9,  -7.5dB */
	0x0E5, /*  10, -7.0dB */
	0x0F2, /*  11, -6.5dB */
	0x101, /*  12, -6.0dB */
	0x110, /*  13, -5.5dB */
	0x120, /*  14, -5.0dB */
	0x131, /*  15, -4.5dB */
	0x143, /*  16, -4.0dB */
	0x156, /*  17, -3.5dB */
	0x16A, /*  18, -3.0dB */
	0x180, /*  19, -2.5dB */
	0x197, /*  20, -2.0dB */
	0x1AF, /*  21, -1.5dB */
	0x1C8, /*  22, -1.0dB */
	0x1E3, /*  23, -0.5dB */
	0x200, /*  24, +0  dB */
	0x21E, /*  25, +0.5dB */
	0x23E, /*  26, +1.0dB */
	0x261, /*  27, +1.5dB */
	0x285, /*  28, +2.0dB */
	0x2AB, /*  29, +2.5dB */
	0x2D3, /*  30, +3.0dB */
	0x2FE, /*  31, +3.5dB */
	0x32B, /*  32, +4.0dB */
	0x35C, /*  33, +4.5dB */
	0x38E, /*  34, +5.0dB */
	0x3C4, /*  35, +5.5dB */
	0x3FE  /*  36, +6.0dB */
};

/* Remove Edca by Yu Chen */

static void odm_CommonInfoSelfInit(struct dm_odm_t *dm_odm)
{
	dm_odm->bCckHighPower = (bool) PHY_QueryBBReg(dm_odm->Adapter, ODM_REG(CCK_RPT_FORMAT, dm_odm), ODM_BIT(CCK_RPT_FORMAT, dm_odm));
	dm_odm->RFPathRxEnable = (u8) PHY_QueryBBReg(dm_odm->Adapter, ODM_REG(BB_RX_PATH, dm_odm), ODM_BIT(BB_RX_PATH, dm_odm));

	dm_odm->TxRate = 0xFF;
}

static void odm_CommonInfoSelfUpdate(struct dm_odm_t *dm_odm)
{
	u8 EntryCnt = 0;
	u8 i;
	PSTA_INFO_T	entry;

	if (*(dm_odm->pBandWidth) == ODM_BW40M) {
		if (*(dm_odm->pSecChOffset) == 1)
			dm_odm->ControlChannel = *(dm_odm->pChannel)-2;
		else if (*(dm_odm->pSecChOffset) == 2)
			dm_odm->ControlChannel = *(dm_odm->pChannel)+2;
	} else
		dm_odm->ControlChannel = *(dm_odm->pChannel);

	for (i = 0; i < ODM_ASSOCIATE_ENTRY_NUM; i++) {
		entry = dm_odm->pODM_StaInfo[i];
		if (IS_STA_VALID(entry))
			EntryCnt++;
	}

	if (EntryCnt == 1)
		dm_odm->bOneEntryOnly = true;
	else
		dm_odm->bOneEntryOnly = false;
}

static void odm_CmnInfoInit_Debug(struct dm_odm_t *dm_odm)
{
}

static void odm_BasicDbgMessage(struct dm_odm_t *dm_odm)
{
}

/* 3 ============================================================ */
/* 3 RATR MASK */
/* 3 ============================================================ */
/* 3 ============================================================ */
/* 3 Rate Adaptive */
/* 3 ============================================================ */

static void odm_RateAdaptiveMaskInit(struct dm_odm_t *dm_odm)
{
	struct odm_rate_adaptive *odm_ra = &dm_odm->RateAdaptive;

	odm_ra->Type = DM_Type_ByDriver;
	if (odm_ra->Type == DM_Type_ByDriver)
		dm_odm->bUseRAMask = true;
	else
		dm_odm->bUseRAMask = false;

	odm_ra->RATRState = DM_RATR_STA_INIT;
	odm_ra->LdpcThres = 35;
	odm_ra->bUseLdpc = false;
	odm_ra->HighRSSIThresh = 50;
	odm_ra->LowRSSIThresh = 20;
}

u32 ODM_Get_Rate_Bitmap(
	struct dm_odm_t *dm_odm,
	u32 macid,
	u32 ra_mask,
	u8 rssi_level
)
{
	PSTA_INFO_T	entry;
	u32 rate_bitmap = 0;
	u8 WirelessMode;

	entry = dm_odm->pODM_StaInfo[macid];
	if (!IS_STA_VALID(entry))
		return ra_mask;

	WirelessMode = entry->wireless_mode;

	switch (WirelessMode) {
	case ODM_WM_B:
		if (ra_mask & 0x0000000c)		/* 11M or 5.5M enable */
			rate_bitmap = 0x0000000d;
		else
			rate_bitmap = 0x0000000f;
		break;

	case (ODM_WM_G):
		if (rssi_level == DM_RATR_STA_HIGH)
			rate_bitmap = 0x00000f00;
		else
			rate_bitmap = 0x00000ff0;
		break;

	case (ODM_WM_B|ODM_WM_G):
		if (rssi_level == DM_RATR_STA_HIGH)
			rate_bitmap = 0x00000f00;
		else if (rssi_level == DM_RATR_STA_MIDDLE)
			rate_bitmap = 0x00000ff0;
		else
			rate_bitmap = 0x00000ff5;
		break;

	case (ODM_WM_B|ODM_WM_G|ODM_WM_N24G):
	case (ODM_WM_B|ODM_WM_N24G):
	case (ODM_WM_G|ODM_WM_N24G):
		if (rssi_level == DM_RATR_STA_HIGH)
			rate_bitmap = 0x000f0000;
		else if (rssi_level == DM_RATR_STA_MIDDLE)
			rate_bitmap = 0x000ff000;
		else {
			if (*(dm_odm->pBandWidth) == ODM_BW40M)
				rate_bitmap = 0x000ff015;
			else
				rate_bitmap = 0x000ff005;
		}
		break;

	default:
		rate_bitmap = 0x0fffffff;
		break;
	}

	return ra_mask & rate_bitmap;

}

static void odm_RefreshRateAdaptiveMaskCE(struct dm_odm_t *dm_odm)
{
	u8 i;
	struct adapter *padapter =  dm_odm->Adapter;

	if (padapter->bDriverStopped)
		return;

	if (!dm_odm->bUseRAMask)
		return;

	for (i = 0; i < ODM_ASSOCIATE_ENTRY_NUM; i++) {
		PSTA_INFO_T pstat = dm_odm->pODM_StaInfo[i];

		if (IS_STA_VALID(pstat)) {
			if (is_multicast_ether_addr(pstat->hwaddr))  /* if (psta->mac_id == 1) */
				continue;

			if (true == ODM_RAStateCheck(dm_odm, pstat->rssi_stat.UndecoratedSmoothedPWDB, false, &pstat->rssi_level)) {
				rtw_hal_update_ra_mask(pstat, pstat->rssi_level);
			}

		}
	}
}

/*-----------------------------------------------------------------------------
* Function:	odm_RefreshRateAdaptiveMask()
*
* Overview:	Update rate table mask according to rssi
*
* Input:		NONE
*
* Output:		NONE
*
* Return:		NONE
*
* Revised History:
*When		Who		Remark
*05/27/2009	hpfan	Create Version 0.
*
* --------------------------------------------------------------------------
*/
static void odm_RefreshRateAdaptiveMask(struct dm_odm_t *dm_odm)
{

	if (!(dm_odm->SupportAbility & ODM_BB_RA_MASK))
		return;

	odm_RefreshRateAdaptiveMaskCE(dm_odm);
}

/*  Return Value: bool */
/*  - true: RATRState is changed. */
bool ODM_RAStateCheck(
	struct dm_odm_t *dm_odm,
	s32 RSSI,
	bool force_update,
	u8 *ratrstate
)
{
	struct odm_rate_adaptive *ra = &dm_odm->RateAdaptive;
	const u8 GoUpGap = 5;
	u8 HighRSSIThreshForRA = ra->HighRSSIThresh;
	u8 LowRSSIThreshForRA = ra->LowRSSIThresh;
	u8 RATRState;

	/*  Threshold Adjustment: */
	/*  when RSSI state trends to go up one or two levels, make sure RSSI is high enough. */
	/*  Here GoUpGap is added to solve the boundary's level alternation issue. */
	switch (*ratrstate) {
	case DM_RATR_STA_INIT:
	case DM_RATR_STA_HIGH:
		break;

	case DM_RATR_STA_MIDDLE:
		HighRSSIThreshForRA += GoUpGap;
		break;

	case DM_RATR_STA_LOW:
		HighRSSIThreshForRA += GoUpGap;
		LowRSSIThreshForRA += GoUpGap;
		break;

	default:
		netdev_dbg(dm_odm->Adapter->pnetdev,
			   "wrong rssi level setting %d !", *ratrstate);
		break;
	}

	/*  Decide RATRState by RSSI. */
	if (RSSI > HighRSSIThreshForRA)
		RATRState = DM_RATR_STA_HIGH;
	else if (RSSI > LowRSSIThreshForRA)
		RATRState = DM_RATR_STA_MIDDLE;
	else
		RATRState = DM_RATR_STA_LOW;

	if (*ratrstate != RATRState || force_update) {
		*ratrstate = RATRState;
		return true;
	}

	return false;
}

/*  */

/* 3 ============================================================ */
/* 3 RSSI Monitor */
/* 3 ============================================================ */

static void odm_RSSIMonitorInit(struct dm_odm_t *dm_odm)
{
	struct ra_t *ra_Table = &dm_odm->DM_RA_Table;

	ra_Table->firstconnect = false;

}

static void FindMinimumRSSI(struct adapter *padapter)
{
	struct hal_com_data	*hal_data = GET_HAL_DATA(padapter);
	struct dm_priv *pdmpriv = &hal_data->dmpriv;
	struct dm_odm_t *dm_odm = &hal_data->odmpriv;

	/* 1 1.Determine the minimum RSSI */

	if (
		(dm_odm->bLinked != true) &&
		(pdmpriv->EntryMinUndecoratedSmoothedPWDB == 0)
	) {
		pdmpriv->MinUndecoratedPWDBForDM = 0;
	} else
		pdmpriv->MinUndecoratedPWDBForDM = pdmpriv->EntryMinUndecoratedSmoothedPWDB;
}

static void odm_RSSIMonitorCheckCE(struct dm_odm_t *dm_odm)
{
	struct adapter *Adapter = dm_odm->Adapter;
	struct hal_com_data	*hal_data = GET_HAL_DATA(Adapter);
	struct dm_priv *pdmpriv = &hal_data->dmpriv;
	int i;
	int tmentryMaxPWDB = 0, tmentryMinPWDB = 0xff;
	u8 sta_cnt = 0;
	u32 PWDB_rssi[NUM_STA] = {0};/* 0~15]:MACID, [16~31]:PWDB_rssi */
	struct ra_t *ra_Table = &dm_odm->DM_RA_Table;

	if (dm_odm->bLinked != true)
		return;

	ra_Table->firstconnect = dm_odm->bLinked;

	/* if (check_fwstate(&Adapter->mlmepriv, WIFI_AP_STATE|WIFI_ADHOC_STATE|WIFI_ADHOC_MASTER_STATE) == true) */
	{
		struct sta_info *psta;

		for (i = 0; i < ODM_ASSOCIATE_ENTRY_NUM; i++) {
			psta = dm_odm->pODM_StaInfo[i];
			if (IS_STA_VALID(psta)) {
				if (is_multicast_ether_addr(psta->hwaddr))  /* if (psta->mac_id == 1) */
					continue;

				if (psta->rssi_stat.UndecoratedSmoothedPWDB == (-1))
					continue;

				if (psta->rssi_stat.UndecoratedSmoothedPWDB < tmentryMinPWDB)
					tmentryMinPWDB = psta->rssi_stat.UndecoratedSmoothedPWDB;

				if (psta->rssi_stat.UndecoratedSmoothedPWDB > tmentryMaxPWDB)
					tmentryMaxPWDB = psta->rssi_stat.UndecoratedSmoothedPWDB;

				if (psta->rssi_stat.UndecoratedSmoothedPWDB != (-1))
					PWDB_rssi[sta_cnt++] = (psta->mac_id | (psta->rssi_stat.UndecoratedSmoothedPWDB<<16));
			}
		}

		for (i = 0; i < sta_cnt; i++) {
			if (PWDB_rssi[i] != (0)) {
				if (hal_data->fw_ractrl == true)/*  Report every sta's RSSI to FW */
					rtl8723b_set_rssi_cmd(Adapter, (u8 *)(&PWDB_rssi[i]));
			}
		}
	}



	if (tmentryMaxPWDB != 0)	/*  If associated entry is found */
		pdmpriv->EntryMaxUndecoratedSmoothedPWDB = tmentryMaxPWDB;
	else
		pdmpriv->EntryMaxUndecoratedSmoothedPWDB = 0;

	if (tmentryMinPWDB != 0xff) /*  If associated entry is found */
		pdmpriv->EntryMinUndecoratedSmoothedPWDB = tmentryMinPWDB;
	else
		pdmpriv->EntryMinUndecoratedSmoothedPWDB = 0;

	FindMinimumRSSI(Adapter);/* get pdmpriv->MinUndecoratedPWDBForDM */

	dm_odm->RSSI_Min = pdmpriv->MinUndecoratedPWDBForDM;
	/* ODM_CmnInfoUpdate(&hal_data->odmpriv , ODM_CMNINFO_RSSI_MIN, pdmpriv->MinUndecoratedPWDBForDM); */
}

static void odm_RSSIMonitorCheck(struct dm_odm_t *dm_odm)
{
	if (!(dm_odm->SupportAbility & ODM_BB_RSSI_MONITOR))
		return;

	odm_RSSIMonitorCheckCE(dm_odm);

}	/*  odm_RSSIMonitorCheck */

/* 3 ============================================================ */
/* 3 SW Antenna Diversity */
/* 3 ============================================================ */
static void odm_SwAntDetectInit(struct dm_odm_t *dm_odm)
{
	struct swat_t *dm_swat_table = &dm_odm->DM_SWAT_Table;

	dm_swat_table->SWAS_NoLink_BK_Reg92c = rtw_read32(dm_odm->Adapter, rDPDT_control);
	dm_swat_table->PreAntenna = MAIN_ANT;
	dm_swat_table->CurAntenna = MAIN_ANT;
	dm_swat_table->SWAS_NoLink_State = 0;
}

/* 3 ============================================================ */
/* 3 Tx Power Tracking */
/* 3 ============================================================ */

static u8 getSwingIndex(struct dm_odm_t *dm_odm)
{
	struct adapter *Adapter = dm_odm->Adapter;
	u8 i = 0;
	u32 bbSwing;
	u32 swingTableSize;
	u32 *swing_table;

	bbSwing = PHY_QueryBBReg(Adapter, rOFDM0_XATxIQImbalance, 0xFFC00000);

	swing_table = OFDMSwingTable_New;
	swingTableSize = OFDM_TABLE_SIZE;

	for (i = 0; i < swingTableSize; ++i) {
		u32 tableValue = swing_table[i];

		if (tableValue >= 0x100000)
			tableValue >>= 22;
		if (bbSwing == tableValue)
			break;
	}
	return i;
}

void odm_TXPowerTrackingInit(struct dm_odm_t *dm_odm)
{
	u8 defaultSwingIndex = getSwingIndex(dm_odm);
	u8 p = 0;
	struct adapter *Adapter = dm_odm->Adapter;
	struct hal_com_data *hal_data = GET_HAL_DATA(Adapter);


	struct dm_priv *pdmpriv = &hal_data->dmpriv;

	pdmpriv->bTXPowerTracking = true;
	pdmpriv->TXPowercount = 0;
	pdmpriv->bTXPowerTrackingInit = false;

	if (*(dm_odm->mp_mode) != 1)
		pdmpriv->TxPowerTrackControl = true;
	else
		pdmpriv->TxPowerTrackControl = false;

	/* dm_odm->RFCalibrateInfo.TxPowerTrackControl = true; */
	dm_odm->RFCalibrateInfo.ThermalValue = hal_data->EEPROMThermalMeter;
	dm_odm->RFCalibrateInfo.ThermalValue_IQK = hal_data->EEPROMThermalMeter;
	dm_odm->RFCalibrateInfo.ThermalValue_LCK = hal_data->EEPROMThermalMeter;

	/*  The index of "0 dB" in SwingTable. */
	dm_odm->DefaultOfdmIndex = (defaultSwingIndex >= OFDM_TABLE_SIZE) ? 30 : defaultSwingIndex;
	dm_odm->DefaultCckIndex = 20;

	dm_odm->BbSwingIdxCckBase = dm_odm->DefaultCckIndex;
	dm_odm->RFCalibrateInfo.CCK_index = dm_odm->DefaultCckIndex;

	for (p = RF_PATH_A; p < MAX_RF_PATH; ++p) {
		dm_odm->BbSwingIdxOfdmBase[p] = dm_odm->DefaultOfdmIndex;
		dm_odm->RFCalibrateInfo.OFDM_index[p] = dm_odm->DefaultOfdmIndex;
		dm_odm->RFCalibrateInfo.DeltaPowerIndex[p] = 0;
		dm_odm->RFCalibrateInfo.DeltaPowerIndexLast[p] = 0;
		dm_odm->RFCalibrateInfo.PowerIndexOffset[p] = 0;
	}

}

void ODM_TXPowerTrackingCheck(struct dm_odm_t *dm_odm)
{
	struct adapter *Adapter = dm_odm->Adapter;

	if (!(dm_odm->SupportAbility & ODM_RF_TX_PWR_TRACK))
		return;

	if (!dm_odm->RFCalibrateInfo.TM_Trigger) { /* at least delay 1 sec */
		PHY_SetRFReg(dm_odm->Adapter, RF_PATH_A, RF_T_METER_NEW, (BIT17 | BIT16), 0x03);

		dm_odm->RFCalibrateInfo.TM_Trigger = 1;
		return;
	} else {
		ODM_TXPowerTrackingCallback_ThermalMeter(Adapter);
		dm_odm->RFCalibrateInfo.TM_Trigger = 0;
	}
}

/*  */
/* 3 Export Interface */
/*  */

/*  */
/*  2011/09/21 MH Add to describe different team necessary resource allocate?? */
/*  */
void ODM_DMInit(struct dm_odm_t *dm_odm)
{

	odm_CommonInfoSelfInit(dm_odm);
	odm_CmnInfoInit_Debug(dm_odm);
	odm_DIGInit(dm_odm);
	odm_NHMCounterStatisticsInit(dm_odm);
	odm_AdaptivityInit(dm_odm);
	odm_RateAdaptiveMaskInit(dm_odm);
	ODM_CfoTrackingInit(dm_odm);
	ODM_EdcaTurboInit(dm_odm);
	odm_RSSIMonitorInit(dm_odm);
	odm_TXPowerTrackingInit(dm_odm);

	ODM_ClearTxPowerTrackingState(dm_odm);

	odm_DynamicBBPowerSavingInit(dm_odm);
	odm_DynamicTxPowerInit(dm_odm);

	odm_SwAntDetectInit(dm_odm);
}

/*  */
/*  2011/09/20 MH This is the entry pointer for all team to execute HW out source DM. */
/*  You can not add any dummy function here, be care, you can only use DM structure */
/*  to perform any new ODM_DM. */
/*  */
void ODM_DMWatchdog(struct dm_odm_t *dm_odm)
{
	odm_CommonInfoSelfUpdate(dm_odm);
	odm_BasicDbgMessage(dm_odm);
	odm_FalseAlarmCounterStatistics(dm_odm);
	odm_NHMCounterStatistics(dm_odm);

	odm_RSSIMonitorCheck(dm_odm);

	/* For CE Platform(SPRD or Tablet) */
	/* 8723A or 8189ES platform */
	/* NeilChen--2012--08--24-- */
	/* Fix Leave LPS issue */
	if ((adapter_to_pwrctl(dm_odm->Adapter)->pwr_mode != PS_MODE_ACTIVE) /*  in LPS mode */
		/*  */
		/* (dm_odm->SupportICType & (ODM_RTL8723A))|| */
		/* (dm_odm->SupportICType & (ODM_RTL8188E) &&(&&(((dm_odm->SupportInterface  == ODM_ITRF_SDIO))) */
		/*  */
	) {
			odm_DIGbyRSSI_LPS(dm_odm);
	} else
		odm_DIG(dm_odm);

	{
		struct dig_t *dm_dig_table = &dm_odm->DM_DigTable;

		odm_Adaptivity(dm_odm, dm_dig_table->CurIGValue);
	}
	odm_CCKPacketDetectionThresh(dm_odm);

	if (*(dm_odm->pbPowerSaving) == true)
		return;


	odm_RefreshRateAdaptiveMask(dm_odm);
	odm_EdcaTurboCheck(dm_odm);
	ODM_CfoTracking(dm_odm);

	ODM_TXPowerTrackingCheck(dm_odm);

	/* odm_EdcaTurboCheck(dm_odm); */

	/* 2010.05.30 LukeLee: For CE platform, files in IC subfolders may not be included to be compiled, */
	/*  so compile flags must be left here to prevent from compile errors */
	dm_odm->PhyDbgInfo.NumQryBeaconPkt = 0;
}


/*  */
/*  Init /.. Fixed HW value. Only init time. */
/*  */
void ODM_CmnInfoInit(struct dm_odm_t *dm_odm, enum odm_cmninfo_e CmnInfo, u32 Value)
{
	/*  */
	/*  This section is used for init value */
	/*  */
	switch (CmnInfo) {
	/*  */
	/*  Fixed ODM value. */
	/*  */
	case ODM_CMNINFO_ABILITY:
		dm_odm->SupportAbility = (u32)Value;
		break;

	case ODM_CMNINFO_PLATFORM:
		dm_odm->SupportPlatform = (u8)Value;
		break;

	case ODM_CMNINFO_INTERFACE:
		dm_odm->SupportInterface = (u8)Value;
		break;

	case ODM_CMNINFO_IC_TYPE:
		dm_odm->SupportICType = Value;
		break;

	case ODM_CMNINFO_CUT_VER:
		dm_odm->CutVersion = (u8)Value;
		break;

	case ODM_CMNINFO_FAB_VER:
		dm_odm->FabVersion = (u8)Value;
		break;

	case ODM_CMNINFO_RFE_TYPE:
		dm_odm->RFEType = (u8)Value;
		break;

	case    ODM_CMNINFO_RF_ANTENNA_TYPE:
		dm_odm->AntDivType = (u8)Value;
		break;

	case ODM_CMNINFO_PACKAGE_TYPE:
		dm_odm->PackageType = (u8)Value;
		break;

	case ODM_CMNINFO_EXT_LNA:
		dm_odm->ExtLNA = (u8)Value;
		break;

	case ODM_CMNINFO_EXT_PA:
		dm_odm->ExtPA = (u8)Value;
		break;

	case ODM_CMNINFO_GPA:
		dm_odm->TypeGPA = (enum odm_type_gpa_e)Value;
		break;
	case ODM_CMNINFO_APA:
		dm_odm->TypeAPA = (enum odm_type_apa_e)Value;
		break;
	case ODM_CMNINFO_GLNA:
		dm_odm->TypeGLNA = (enum odm_type_glna_e)Value;
		break;
	case ODM_CMNINFO_ALNA:
		dm_odm->TypeALNA = (enum odm_type_alna_e)Value;
		break;

	case ODM_CMNINFO_EXT_TRSW:
		dm_odm->ExtTRSW = (u8)Value;
		break;
	case ODM_CMNINFO_PATCH_ID:
		dm_odm->PatchID = (u8)Value;
		break;
	case ODM_CMNINFO_BINHCT_TEST:
		dm_odm->bInHctTest = (bool)Value;
		break;
	case ODM_CMNINFO_BWIFI_TEST:
		dm_odm->bWIFITest = (bool)Value;
		break;

	case ODM_CMNINFO_SMART_CONCURRENT:
		dm_odm->bDualMacSmartConcurrent = (bool)Value;
		break;

	/* To remove the compiler warning, must add an empty default statement to handle the other values. */
	default:
		/* do nothing */
		break;
	}

}


void ODM_CmnInfoHook(struct dm_odm_t *dm_odm, enum odm_cmninfo_e CmnInfo, void *value)
{
	/*  */
	/*  Hook call by reference pointer. */
	/*  */
	switch (CmnInfo) {
	/*  */
	/*  Dynamic call by reference pointer. */
	/*  */
	case ODM_CMNINFO_MAC_PHY_MODE:
		dm_odm->pMacPhyMode = value;
		break;

	case ODM_CMNINFO_TX_UNI:
		dm_odm->pNumTxBytesUnicast = value;
		break;

	case ODM_CMNINFO_RX_UNI:
		dm_odm->pNumRxBytesUnicast = value;
		break;

	case ODM_CMNINFO_WM_MODE:
		dm_odm->pwirelessmode = value;
		break;

	case ODM_CMNINFO_SEC_CHNL_OFFSET:
		dm_odm->pSecChOffset = value;
		break;

	case ODM_CMNINFO_SEC_MODE:
		dm_odm->pSecurity = value;
		break;

	case ODM_CMNINFO_BW:
		dm_odm->pBandWidth = value;
		break;

	case ODM_CMNINFO_CHNL:
		dm_odm->pChannel = value;
		break;

	case ODM_CMNINFO_DMSP_GET_VALUE:
		dm_odm->pbGetValueFromOtherMac = value;
		break;

	case ODM_CMNINFO_BUDDY_ADAPTOR:
		dm_odm->pBuddyAdapter = value;
		break;

	case ODM_CMNINFO_DMSP_IS_MASTER:
		dm_odm->pbMasterOfDMSP = value;
		break;

	case ODM_CMNINFO_SCAN:
		dm_odm->pbScanInProcess = value;
		break;

	case ODM_CMNINFO_POWER_SAVING:
		dm_odm->pbPowerSaving = value;
		break;

	case ODM_CMNINFO_ONE_PATH_CCA:
		dm_odm->pOnePathCCA = value;
		break;

	case ODM_CMNINFO_DRV_STOP:
		dm_odm->pbDriverStopped =  value;
		break;

	case ODM_CMNINFO_PNP_IN:
		dm_odm->pbDriverIsGoingToPnpSetPowerSleep =  value;
		break;

	case ODM_CMNINFO_INIT_ON:
		dm_odm->pinit_adpt_in_progress =  value;
		break;

	case ODM_CMNINFO_ANT_TEST:
		dm_odm->pAntennaTest =  value;
		break;

	case ODM_CMNINFO_NET_CLOSED:
		dm_odm->pbNet_closed = value;
		break;

	case ODM_CMNINFO_FORCED_RATE:
		dm_odm->pForcedDataRate = value;
		break;

	case ODM_CMNINFO_FORCED_IGI_LB:
		dm_odm->pu1ForcedIgiLb = value;
		break;

	case ODM_CMNINFO_MP_MODE:
		dm_odm->mp_mode = value;
		break;

	/* case ODM_CMNINFO_RTSTA_AID: */
	/* dm_odm->pAidMap =  (u8 *)value; */
	/* break; */

	/* case ODM_CMNINFO_BT_COEXIST: */
	/* dm_odm->BTCoexist = (bool *)value; */

	/* case ODM_CMNINFO_STA_STATUS: */
	/* dm_odm->pODM_StaInfo[] = (PSTA_INFO_T)value; */
	/* break; */

	/* case ODM_CMNINFO_PHY_STATUS: */
	/* dm_odm->pPhyInfo = (ODM_PHY_INFO *)value; */
	/* break; */

	/* case ODM_CMNINFO_MAC_STATUS: */
	/* dm_odm->pMacInfo = (struct odm_mac_status_info *)value; */
	/* break; */
	/* To remove the compiler warning, must add an empty default statement to handle the other values. */
	default:
		/* do nothing */
		break;
	}

}


void ODM_CmnInfoPtrArrayHook(
	struct dm_odm_t *dm_odm,
	enum odm_cmninfo_e CmnInfo,
	u16 Index,
	void *value
)
{
	/*  */
	/*  Hook call by reference pointer. */
	/*  */
	switch (CmnInfo) {
	/*  */
	/*  Dynamic call by reference pointer. */
	/*  */
	case ODM_CMNINFO_STA_STATUS:
		dm_odm->pODM_StaInfo[Index] = (PSTA_INFO_T)value;
		break;
	/* To remove the compiler warning, must add an empty default statement to handle the other values. */
	default:
		/* do nothing */
		break;
	}

}


/*  */
/*  Update Band/CHannel/.. The values are dynamic but non-per-packet. */
/*  */
void ODM_CmnInfoUpdate(struct dm_odm_t *dm_odm, u32 CmnInfo, u64 Value)
{
	/*  */
	/*  This init variable may be changed in run time. */
	/*  */
	switch (CmnInfo) {
	case ODM_CMNINFO_LINK_IN_PROGRESS:
		dm_odm->bLinkInProcess = (bool)Value;
		break;

	case ODM_CMNINFO_ABILITY:
		dm_odm->SupportAbility = (u32)Value;
		break;

	case ODM_CMNINFO_WIFI_DIRECT:
		dm_odm->bWIFI_Direct = (bool)Value;
		break;

	case ODM_CMNINFO_WIFI_DISPLAY:
		dm_odm->bWIFI_Display = (bool)Value;
		break;

	case ODM_CMNINFO_LINK:
		dm_odm->bLinked = (bool)Value;
		break;

	case ODM_CMNINFO_STATION_STATE:
		dm_odm->bsta_state = (bool)Value;
		break;

	case ODM_CMNINFO_RSSI_MIN:
		dm_odm->RSSI_Min = (u8)Value;
		break;

	case ODM_CMNINFO_RA_THRESHOLD_HIGH:
		dm_odm->RateAdaptive.HighRSSIThresh = (u8)Value;
		break;

	case ODM_CMNINFO_RA_THRESHOLD_LOW:
		dm_odm->RateAdaptive.LowRSSIThresh = (u8)Value;
		break;
	/*  The following is for BT HS mode and BT coexist mechanism. */
	case ODM_CMNINFO_BT_ENABLED:
		dm_odm->bBtEnabled = (bool)Value;
		break;

	case ODM_CMNINFO_BT_HS_CONNECT_PROCESS:
		dm_odm->bBtConnectProcess = (bool)Value;
		break;

	case ODM_CMNINFO_BT_HS_RSSI:
		dm_odm->btHsRssi = (u8)Value;
		break;

	case ODM_CMNINFO_BT_OPERATION:
		dm_odm->bBtHsOperation = (bool)Value;
		break;

	case ODM_CMNINFO_BT_LIMITED_DIG:
		dm_odm->bBtLimitedDig = (bool)Value;
		break;

	case ODM_CMNINFO_BT_DISABLE_EDCA:
		dm_odm->bBtDisableEdcaTurbo = (bool)Value;
		break;

/*
	case	ODM_CMNINFO_OP_MODE:
		dm_odm->OPMode = (u8)Value;
		break;

	case	ODM_CMNINFO_WM_MODE:
		dm_odm->WirelessMode = (u8)Value;
		break;

	case	ODM_CMNINFO_SEC_CHNL_OFFSET:
		dm_odm->SecChOffset = (u8)Value;
		break;

	case	ODM_CMNINFO_SEC_MODE:
		dm_odm->Security = (u8)Value;
		break;

	case	ODM_CMNINFO_BW:
		dm_odm->BandWidth = (u8)Value;
		break;

	case	ODM_CMNINFO_CHNL:
		dm_odm->Channel = (u8)Value;
		break;
*/
	default:
		/* do nothing */
		break;
	}


}

/* 3 ============================================================ */
/* 3 DIG */
/* 3 ============================================================ */
/*-----------------------------------------------------------------------------
 * Function:	odm_DIGInit()
 *
 * Overview:	Set DIG scheme init value.
 *
 * Input:		NONE
 *
 * Output:		NONE
 *
 * Return:		NONE
 *
 * Revised History:
 *When		Who		Remark
 *
 *---------------------------------------------------------------------------
 */

/* Remove DIG by yuchen */

/* Remove DIG and FA check by Yu Chen */

/* 3 ============================================================ */
/* 3 BB Power Save */
/* 3 ============================================================ */

/* Remove BB power saving by Yuchen */

/* 3 ============================================================ */
/* 3 Dynamic Tx Power */
/* 3 ============================================================ */

/* Remove BY YuChen */


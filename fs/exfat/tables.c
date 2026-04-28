// SPDX-License-Identifier: GPL-2.0-or-later

#include <linux/buffer_head.h>

#include "exfat_raw.h"
#include "exfat_fs.h"

/*
 * The recommended upcase table(7.2.5.1 Recommended Up-case Table in exfat
 * specification available at
 * https://docs.microsoft.com/en-us/windows/win32/fileio/exfat-specification),
 * expressed in Linux's own format.
 *
 * It is found that the table contains following errors or subtle caveats.
 *
 *   - Uppercase converted to lowercase
 *     - U+023A -> U+2C65
 *     - U+023E -> U+2C66
 *     - U+1FCC -> U+1FC3
 *     - U+1FFC -> U+1FF3
 *   - Letters that have multiple corresponding lower or upper case letters(Greek letter sigma)
 *     - U+03C2 -> U+03C3
 *     - U+03C3 -> U+03C2
 *
 * To maintain interoperability, these errors are not corrected.
 */
const struct exfat_upcase_range_info exfat_def_utbl_ri[EXFAT_DEF_UTBL_RI_COUNT] = {
	/* ASCII */
	{
		/* (index = 0, len = 26) */
		.start = 0x0061,
		.end   = 0x007B,
		.value = 0x0041,
		.inc   = 0x0001,
	},
	/* Latin-1 Supplement */
	{
		/* (index = 1, len = 23) */
		.start = 0x00E0,
		.end   = 0x00F7,
		.value = 0x00C0,
		.inc   = 0x0001,
	},
	{
		/* (index = 2, len = 7) */
		.start = 0x00F8,
		.end   = 0x00FF,
		.value = 0x00D8,
		.inc   = 0x0001,
	},
	{
		/* (index = 3, len = 1) */
		.start = 0x00FF,
		.end   = 0x0100,
		.value = 0x0178,
		.inc   = 0x0001,
	},
	/* Latin Extended-A */
	{
		/* (index = 4, len = 47) */
		.start = 0x0101,
		.end   = 0x0130,
		.value = 0x0100,
		.inc   = 0x0002,
	},
	{
		/* (index = 5, len = 5) */
		.start = 0x0133,
		.end   = 0x0138,
		.value = 0x0132,
		.inc   = 0x0002,
	},
	{
		/* (index = 6, len = 15) */
		.start = 0x013A,
		.end   = 0x0149,
		.value = 0x0139,
		.inc   = 0x0002,
	},
	{
		/* (index = 7, len = 45) */
		.start = 0x014B,
		.end   = 0x0178,
		.value = 0x014A,
		.inc   = 0x0002,
	},
	{
		/* (index = 8, len = 6) */
		.start = 0x017A,
		.end   = 0x0180,
		.value = 0x0179,
		.inc   = 0x0002,
	},
	/* Latin Extended-B */
	{
		/* (index = 9, len = 1) */
		.start = 0x0180,
		.end   = 0x0181,
		.value = 0x0243,
		.inc   = 0x0001,
	},
	{
		/* (index = 10, len = 3) */
		.start = 0x0183,
		.end   = 0x0186,
		.value = 0x0182,
		.inc   = 0x0002,
	},
	{
		/* (index = 11, len = 5) */
		.start = 0x0188,
		.end   = 0x018D,
		.value = 0x0187,
		.inc   = 0x0004,
	},
	{
		/* (index = 12, len = 1) */
		.start = 0x0192,
		.end   = 0x0193,
		.value = 0x0191,
		.inc   = 0x0001,
	},
	{
		/* (index = 13, len = 1) */
		.start = 0x0195,
		.end   = 0x0196,
		.value = 0x01F6,
		.inc   = 0x0001,
	},
	{
		/* (index = 14, len = 1) */
		.start = 0x0199,
		.end   = 0x019A,
		.value = 0x0198,
		.inc   = 0x0001,
	},
	{
		/* (index = 15, len = 1) */
		.start = 0x019A,
		.end   = 0x019B,
		.value = 0x023D,
		.inc   = 0x0001,
	},
	{
		/* (index = 16, len = 1) */
		.start = 0x019E,
		.end   = 0x019F,
		.value = 0x0220,
		.inc   = 0x0001,
	},
	{
		/* (index = 17, len = 5) */
		.start = 0x01A1,
		.end   = 0x01A6,
		.value = 0x01A0,
		.inc   = 0x0002,
	},
	{
		/* (index = 18, len = 8) */
		.start = 0x01A8,
		.end   = 0x01B0,
		.value = 0x01A7,
		.inc   = 0x0005,
	},
	{
		/* (index = 19, len = 6) */
		.start = 0x01B0,
		.end   = 0x01B6,
		.value = 0x01AF,
		.inc   = 0x0004,
	},
	{
		/* (index = 20, len = 4) */
		.start = 0x01B6,
		.end   = 0x01BA,
		.value = 0x01B5,
		.inc   = 0x0003,
	},
	{
		/* (index = 21, len = 1) */
		.start = 0x01BD,
		.end   = 0x01BE,
		.value = 0x01BC,
		.inc   = 0x0001,
	},
	{
		/* (index = 22, len = 1) */
		.start = 0x01BF,
		.end   = 0x01C0,
		.value = 0x01F7,
		.inc   = 0x0001,
	},
	{
		/* (index = 23, len = 8) */
		.start = 0x01C6,
		.end   = 0x01CE,
		.value = 0x01C4,
		.inc   = 0x0003,
	},
	{
		/* (index = 24, len = 15) */
		.start = 0x01CE,
		.end   = 0x01DD,
		.value = 0x01CD,
		.inc   = 0x0002,
	},
	{
		/* (index = 25, len = 1) */
		.start = 0x01DD,
		.end   = 0x01DE,
		.value = 0x018E,
		.inc   = 0x0001,
	},
	{
		/* (index = 26, len = 17) */
		.start = 0x01DF,
		.end   = 0x01F0,
		.value = 0x01DE,
		.inc   = 0x0002,
	},
	{
		/* (index = 27, len = 1) */
		.start = 0x01F3,
		.end   = 0x01F4,
		.value = 0x01F1,
		.inc   = 0x0001,
	},
	{
		/* (index = 28, len = 6) */
		.start = 0x01F5,
		.end   = 0x01FB,
		.value = 0x01F4,
		.inc   = 0x0004,
	},
	{
		/* (index = 29, len = 37) */
		.start = 0x01FB,
		.end   = 0x0220,
		.value = 0x01FA,
		.inc   = 0x0002,
	},
	{
		/* (index = 30, len = 17) */
		.start = 0x0223,
		.end   = 0x0234,
		.value = 0x0222,
		.inc   = 0x0002,
	},
	{
		/* (index = 31, len = 1) */
		.start = 0x023A,
		.end   = 0x023B,
		.value = 0x2C65,
		.inc   = 0x0001,
	},
	{
		/* (index = 32, len = 1) */
		.start = 0x023C,
		.end   = 0x023D,
		.value = 0x023B,
		.inc   = 0x0001,
	},
	{
		/* (index = 33, len = 1) */
		.start = 0x023E,
		.end   = 0x023F,
		.value = 0x2C66,
		.inc   = 0x0001,
	},
	{
		/* (index = 34, len = 7) */
		.start = 0x0242,
		.end   = 0x0249,
		.value = 0x0241,
		.inc   = 0x0005,
	},
	{
		/* (index = 35, len = 7) */
		.start = 0x0249,
		.end   = 0x0250,
		.value = 0x0248,
		.inc   = 0x0002,
	},
	/* IPA Extensions */
	{
		/* (index = 36, len = 1) */
		.start = 0x0253,
		.end   = 0x0254,
		.value = 0x0181,
		.inc   = 0x0001,
	},
	{
		/* (index = 37, len = 1) */
		.start = 0x0254,
		.end   = 0x0255,
		.value = 0x0186,
		.inc   = 0x0001,
	},
	{
		/* (index = 38, len = 2) */
		.start = 0x0256,
		.end   = 0x0258,
		.value = 0x0189,
		.inc   = 0x0001,
	},
	{
		/* (index = 39, len = 1) */
		.start = 0x0259,
		.end   = 0x025A,
		.value = 0x018F,
		.inc   = 0x0001,
	},
	{
		/* (index = 40, len = 1) */
		.start = 0x025B,
		.end   = 0x025C,
		.value = 0x0190,
		.inc   = 0x0001,
	},
	{
		/* (index = 41, len = 1) */
		.start = 0x0260,
		.end   = 0x0261,
		.value = 0x0193,
		.inc   = 0x0001,
	},
	{
		/* (index = 42, len = 1) */
		.start = 0x0263,
		.end   = 0x0264,
		.value = 0x0194,
		.inc   = 0x0001,
	},
	{
		/* (index = 43, len = 1) */
		.start = 0x0268,
		.end   = 0x0269,
		.value = 0x0197,
		.inc   = 0x0001,
	},
	{
		/* (index = 44, len = 1) */
		.start = 0x0269,
		.end   = 0x026A,
		.value = 0x0196,
		.inc   = 0x0001,
	},
	{
		/* (index = 45, len = 1) */
		.start = 0x026B,
		.end   = 0x026C,
		.value = 0x2C62,
		.inc   = 0x0001,
	},
	{
		/* (index = 46, len = 1) */
		.start = 0x026F,
		.end   = 0x0270,
		.value = 0x019C,
		.inc   = 0x0001,
	},
	{
		/* (index = 47, len = 1) */
		.start = 0x0272,
		.end   = 0x0273,
		.value = 0x019D,
		.inc   = 0x0001,
	},
	{
		/* (index = 48, len = 1) */
		.start = 0x0275,
		.end   = 0x0276,
		.value = 0x019F,
		.inc   = 0x0001,
	},
	{
		/* (index = 49, len = 1) */
		.start = 0x027D,
		.end   = 0x027E,
		.value = 0x2C64,
		.inc   = 0x0001,
	},
	{
		/* (index = 50, len = 4) */
		.start = 0x0280,
		.end   = 0x0284,
		.value = 0x01A6,
		.inc   = 0x0003,
	},
	{
		/* (index = 51, len = 1) */
		.start = 0x0288,
		.end   = 0x0289,
		.value = 0x01AE,
		.inc   = 0x0001,
	},
	{
		/* (index = 52, len = 1) */
		.start = 0x0289,
		.end   = 0x028A,
		.value = 0x0244,
		.inc   = 0x0001,
	},
	{
		/* (index = 53, len = 2) */
		.start = 0x028A,
		.end   = 0x028C,
		.value = 0x01B1,
		.inc   = 0x0001,
	},
	{
		/* (index = 54, len = 1) */
		.start = 0x028C,
		.end   = 0x028D,
		.value = 0x0245,
		.inc   = 0x0001,
	},
	{
		/* (index = 55, len = 1) */
		.start = 0x0292,
		.end   = 0x0293,
		.value = 0x01B7,
		.inc   = 0x0001,
	},
	/* Greek and Coptic */
	{
		/* (index = 56, len = 3) */
		.start = 0x037B,
		.end   = 0x037E,
		.value = 0x03FD,
		.inc   = 0x0001,
	},
	{
		/* (index = 57, len = 1) */
		.start = 0x03AC,
		.end   = 0x03AD,
		.value = 0x0386,
		.inc   = 0x0001,
	},
	{
		/* (index = 58, len = 3) */
		.start = 0x03AD,
		.end   = 0x03B0,
		.value = 0x0388,
		.inc   = 0x0001,
	},
	{
		/* (index = 59, len = 17) */
		.start = 0x03B1,
		.end   = 0x03C2,
		.value = 0x0391,
		.inc   = 0x0001,
	},
	{
		/* (index = 60, len = 1) */
		.start = 0x03C2,
		.end   = 0x03C3,
		.value = 0x03A3,
		.inc   = 0x0001,
	},
	{
		/* (index = 61, len = 9) */
		.start = 0x03C3,
		.end   = 0x03CC,
		.value = 0x03A3,
		.inc   = 0x0001,
	},
	{
		/* (index = 62, len = 1) */
		.start = 0x03CC,
		.end   = 0x03CD,
		.value = 0x038C,
		.inc   = 0x0001,
	},
	{
		/* (index = 63, len = 2) */
		.start = 0x03CD,
		.end   = 0x03CF,
		.value = 0x038E,
		.inc   = 0x0001,
	},
	{
		/* (index = 64, len = 23) */
		.start = 0x03D9,
		.end   = 0x03F0,
		.value = 0x03D8,
		.inc   = 0x0002,
	},
	{
		/* (index = 65, len = 1) */
		.start = 0x03F2,
		.end   = 0x03F3,
		.value = 0x03F9,
		.inc   = 0x0001,
	},
	{
		/* (index = 66, len = 4) */
		.start = 0x03F8,
		.end   = 0x03FC,
		.value = 0x03F7,
		.inc   = 0x0003,
	},
	/* Cyrillic */
	{
		/* (index = 67, len = 32) */
		.start = 0x0430,
		.end   = 0x0450,
		.value = 0x0410,
		.inc   = 0x0001,
	},
	{
		/* (index = 68, len = 16) */
		.start = 0x0450,
		.end   = 0x0460,
		.value = 0x0400,
		.inc   = 0x0001,
	},
	{
		/* (index = 69, len = 33) */
		.start = 0x0461,
		.end   = 0x0482,
		.value = 0x0460,
		.inc   = 0x0002,
	},
	{
		/* (index = 70, len = 53) */
		.start = 0x048B,
		.end   = 0x04C0,
		.value = 0x048A,
		.inc   = 0x0002,
	},
	{
		/* (index = 71, len = 13) */
		.start = 0x04C2,
		.end   = 0x04CF,
		.value = 0x04C1,
		.inc   = 0x0002,
	},
	{
		/* (index = 72, len = 1) */
		.start = 0x04CF,
		.end   = 0x04D0,
		.value = 0x04C0,
		.inc   = 0x0001,
	},
	{
		/* (index = 73, len = 67) */
		.start = 0x04D1,
		.end   = 0x0514,
		.value = 0x04D0,
		.inc   = 0x0002,
	},
	/* Armenian */
	{
		/* (index = 74, len = 38) */
		.start = 0x0561,
		.end   = 0x0587,
		.value = 0x0531,
		.inc   = 0x0001,
	},
	/* Phonetic Extensions (LATIN SMALL LETTER P WITH STROKE) */
	{
		/* (index = 75, len = 1) */
		.start = 0x1D7D,
		.end   = 0x1D7E,
		.value = 0x2C63,
		.inc   = 0x0001,
	},
	/* Latin Extended Additional */
	{
		/* (index = 76, len = 149) */
		.start = 0x1E01,
		.end   = 0x1E96,
		.value = 0x1E00,
		.inc   = 0x0002,
	},
	{
		/* (index = 77, len = 89) */
		.start = 0x1EA1,
		.end   = 0x1EFA,
		.value = 0x1EA0,
		.inc   = 0x0002,
	},
	/* Greek Extended */
	{
		/* (index = 78, len = 8) */
		.start = 0x1F00,
		.end   = 0x1F08,
		.value = 0x1F08,
		.inc   = 0x0001,
	},
	{
		/* (index = 79, len = 6) */
		.start = 0x1F10,
		.end   = 0x1F16,
		.value = 0x1F18,
		.inc   = 0x0001,
	},
	{
		/* (index = 80, len = 8) */
		.start = 0x1F20,
		.end   = 0x1F28,
		.value = 0x1F28,
		.inc   = 0x0001,
	},
	{
		/* (index = 81, len = 8) */
		.start = 0x1F30,
		.end   = 0x1F38,
		.value = 0x1F38,
		.inc   = 0x0001,
	},
	{
		/* (index = 82, len = 6) */
		.start = 0x1F40,
		.end   = 0x1F46,
		.value = 0x1F48,
		.inc   = 0x0001,
	},
	{
		/* (index = 83, len = 7) */
		.start = 0x1F51,
		.end   = 0x1F58,
		.value = 0x1F59,
		.inc   = 0x0002,
	},
	{
		/* (index = 84, len = 8) */
		.start = 0x1F60,
		.end   = 0x1F68,
		.value = 0x1F68,
		.inc   = 0x0001,
	},
	{
		/* (index = 85, len = 2) */
		.start = 0x1F70,
		.end   = 0x1F72,
		.value = 0x1FBA,
		.inc   = 0x0001,
	},
	{
		/* (index = 86, len = 4) */
		.start = 0x1F72,
		.end   = 0x1F76,
		.value = 0x1FC8,
		.inc   = 0x0001,
	},
	{
		/* (index = 87, len = 2) */
		.start = 0x1F76,
		.end   = 0x1F78,
		.value = 0x1FDA,
		.inc   = 0x0001,
	},
	{
		/* (index = 88, len = 2) */
		.start = 0x1F78,
		.end   = 0x1F7A,
		.value = 0x1FF8,
		.inc   = 0x0001,
	},
	{
		/* (index = 89, len = 2) */
		.start = 0x1F7A,
		.end   = 0x1F7C,
		.value = 0x1FEA,
		.inc   = 0x0001,
	},
	{
		/* (index = 90, len = 2) */
		.start = 0x1F7C,
		.end   = 0x1F7E,
		.value = 0x1FFA,
		.inc   = 0x0001,
	},
	{
		/* (index = 91, len = 8) */
		.start = 0x1F80,
		.end   = 0x1F88,
		.value = 0x1F88,
		.inc   = 0x0001,
	},
	{
		/* (index = 92, len = 8) */
		.start = 0x1F90,
		.end   = 0x1F98,
		.value = 0x1F98,
		.inc   = 0x0001,
	},
	{
		/* (index = 93, len = 8) */
		.start = 0x1FA0,
		.end   = 0x1FA8,
		.value = 0x1FA8,
		.inc   = 0x0001,
	},
	{
		/* (index = 94, len = 2) */
		.start = 0x1FB0,
		.end   = 0x1FB2,
		.value = 0x1FB8,
		.inc   = 0x0001,
	},
	{
		/* (index = 95, len = 1) */
		.start = 0x1FB3,
		.end   = 0x1FB4,
		.value = 0x1FBC,
		.inc   = 0x0001,
	},
	{
		/* (index = 96, len = 1) */
		.start = 0x1FCC,
		.end   = 0x1FCD,
		.value = 0x1FC3,
		.inc   = 0x0001,
	},
	{
		/* (index = 97, len = 2) */
		.start = 0x1FD0,
		.end   = 0x1FD2,
		.value = 0x1FD8,
		.inc   = 0x0001,
	},
	{
		/* (index = 98, len = 2) */
		.start = 0x1FE0,
		.end   = 0x1FE2,
		.value = 0x1FE8,
		.inc   = 0x0001,
	},
	{
		/* (index = 99, len = 1) */
		.start = 0x1FE5,
		.end   = 0x1FE6,
		.value = 0x1FEC,
		.inc   = 0x0001,
	},
	{
		/* (index = 100, len = 1) */
		.start = 0x1FFC,
		.end   = 0x1FFD,
		.value = 0x1FF3,
		.inc   = 0x0001,
	},
	/* Letterlike Symbols (turned letter F) */
	{
		/* (index = 101, len = 1) */
		.start = 0x214E,
		.end   = 0x214F,
		.value = 0x2132,
		.inc   = 0x0001,
	},
	/* Number Forms */
	{
		/* (index = 102, len = 16) */
		.start = 0x2170,
		.end   = 0x2180,
		.value = 0x2160,
		.inc   = 0x0001,
	},
	{
		/* (index = 103, len = 1) */
		.start = 0x2184,
		.end   = 0x2185,
		.value = 0x2183,
		.inc   = 0x0001,
	},
	/* Enclosed Alphanumerics */
	{
		/* (index = 104, len = 26) */
		.start = 0x24D0,
		.end   = 0x24EA,
		.value = 0x24B6,
		.inc   = 0x0001,
	},
	/* Glagolitic */
	{
		/* (index = 105, len = 47) */
		.start = 0x2C30,
		.end   = 0x2C5F,
		.value = 0x2C00,
		.inc   = 0x0001,
	},
	/* Latin Extended-C */
	{
		/* (index = 106, len = 9) */
		.start = 0x2C61,
		.end   = 0x2C6A,
		.value = 0x2C60,
		.inc   = 0x0007,
	},
	{
		/* (index = 107, len = 3) */
		.start = 0x2C6A,
		.end   = 0x2C6D,
		.value = 0x2C69,
		.inc   = 0x0002,
	},
	{
		/* (index = 108, len = 13) */
		.start = 0x2C76,
		.end   = 0x2C83,
		.value = 0x2C75,
		.inc   = 0x000B,
	},
	/* Coptic */
	{
		/* (index = 109, len = 97) */
		.start = 0x2C83,
		.end   = 0x2CE4,
		.value = 0x2C82,
		.inc   = 0x0002,
	},
	/* Georgian Supplement */
	{
		/* (index = 110, len = 38) */
		.start = 0x2D00,
		.end   = 0x2D26,
		.value = 0x10A0,
		.inc   = 0x0001,
	},
	/* Halfwidth and Fullwidth Forms */
	{
		/* (index = 111, len = 26) */
		.start = 0xFF41,
		.end   = 0xFF5B,
		.value = 0xFF21,
		.inc   = 0x0001,
	},
};

/*
 * Allow full-width illegal characters :
 * "MS windows 7" supports full-width-invalid-name-characters.
 * So we should check half-width-invalid-name-characters(ASCII) only
 * for compatibility.
 *
 * " * / : < > ? \ |
 */
const unsigned short exfat_bad_uni_chars[] = {
	0x0022,         0x002A, 0x002F, 0x003A,
	0x003C, 0x003E, 0x003F, 0x005C, 0x007C,
	0
};

// SPDX-License-Identifier: GPL-2.0
/*
 * base64.c - Base64 with support for multiple variants
 *
 * Copyright (c) 2020 Hannes Reinecke, SUSE
 *
 * Based on the base64url routines from fs/crypto/fname.c
 * (which are using the URL-safe Base64 encoding),
 * modified to support multiple Base64 variants.
 */

#include <linux/kernel.h>
#include <linux/types.h>
#include <linux/export.h>
#include <linux/string.h>
#include <linux/base64.h>

static const char base64_tables[][65] = {
	[BASE64_STD] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/",
	[BASE64_URLSAFE] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_",
	[BASE64_IMAP] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+,",
};

static const s8 base64_rev_tables[][256] = {
	[BASE64_STD] = {
	 -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,
	 -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,
	 -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  62,  -1,  -1,  -1,  63,
	 52,  53,  54,  55,  56,  57,  58,  59,  60,  61,  -1,  -1,  -1,  -1,  -1,  -1,
	 -1,   0,   1,   2,   3,   4,   5,   6,   7,   8,   9,  10,  11,  12,  13,  14,
	 15,  16,  17,  18,  19,  20,  21,  22,  23,  24,  25,  -1,  -1,  -1,  -1,  -1,
	 -1,  26,  27,  28,  29,  30,  31,  32,  33,  34,  35,  36,  37,  38,  39,  40,
	 41,  42,  43,  44,  45,  46,  47,  48,  49,  50,  51,  -1,  -1,  -1,  -1,  -1,
	 -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,
	 -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,
	 -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,
	 -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,
	 -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,
	 -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,
	 -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,
	 -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,
	},
	[BASE64_URLSAFE] = {
	 -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,
	 -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,
	 -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  62,  -1,  -1,
	 52,  53,  54,  55,  56,  57,  58,  59,  60,  61,  -1,  -1,  -1,  -1,  -1,  -1,
	 -1,   0,   1,   2,   3,   4,   5,   6,   7,   8,   9,  10,  11,  12,  13,  14,
	 15,  16,  17,  18,  19,  20,  21,  22,  23,  24,  25,  -1,  -1,  -1,  -1,  63,
	 -1,  26,  27,  28,  29,  30,  31,  32,  33,  34,  35,  36,  37,  38,  39,  40,
	 41,  42,  43,  44,  45,  46,  47,  48,  49,  50,  51,  -1,  -1,  -1,  -1,  -1,
	 -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,
	 -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,
	 -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,
	 -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,
	 -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,
	 -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,
	 -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,
	 -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,
	},
	[BASE64_IMAP] = {
	 -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,
	 -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,
	 -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  62,  63,  -1,  -1,  -1,
	 52,  53,  54,  55,  56,  57,  58,  59,  60,  61,  -1,  -1,  -1,  -1,  -1,  -1,
	 -1,   0,   1,   2,   3,   4,   5,   6,   7,   8,   9,  10,  11,  12,  13,  14,
	 15,  16,  17,  18,  19,  20,  21,  22,  23,  24,  25,  -1,  -1,  -1,  -1,  -1,
	 -1,  26,  27,  28,  29,  30,  31,  32,  33,  34,  35,  36,  37,  38,  39,  40,
	 41,  42,  43,  44,  45,  46,  47,  48,  49,  50,  51,  -1,  -1,  -1,  -1,  -1,
	 -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,
	 -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,
	 -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,
	 -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,
	 -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,
	 -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,
	 -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,
	 -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,
	},
};

/**
 * base64_encode() - Base64-encode some binary data
 * @src: the binary data to encode
 * @srclen: the length of @src in bytes
 * @dst: (output) the Base64-encoded string.  Not NUL-terminated.
 * @padding: whether to append '=' padding characters
 * @variant: which base64 variant to use
 *
 * Encodes data using the selected Base64 variant.
 *
 * Return: the length of the resulting Base64-encoded string in bytes.
 */
int base64_encode(const u8 *src, int srclen, char *dst, bool padding, enum base64_variant variant)
{
	u32 ac = 0;
	char *cp = dst;
	const char *base64_table = base64_tables[variant];

	while (srclen >= 3) {
		ac = ((u32)src[0] << 16) |
			 ((u32)src[1] << 8) |
			 (u32)src[2];

		*cp++ = base64_table[ac >> 18];
		*cp++ = base64_table[(ac >> 12) & 0x3f];
		*cp++ = base64_table[(ac >> 6) & 0x3f];
		*cp++ = base64_table[ac & 0x3f];

		src += 3;
		srclen -= 3;
	}

	switch (srclen) {
	case 2:
		ac = ((u32)src[0] << 16) |
		     ((u32)src[1] << 8);

		*cp++ = base64_table[ac >> 18];
		*cp++ = base64_table[(ac >> 12) & 0x3f];
		*cp++ = base64_table[(ac >> 6) & 0x3f];
		if (padding)
			*cp++ = '=';
		break;
	case 1:
		ac = ((u32)src[0] << 16);
		*cp++ = base64_table[ac >> 18];
		*cp++ = base64_table[(ac >> 12) & 0x3f];
		if (padding) {
			*cp++ = '=';
			*cp++ = '=';
		}
		break;
	}
	return cp - dst;
}
EXPORT_SYMBOL_GPL(base64_encode);

/**
 * base64_decode() - Base64-decode a string
 * @src: the string to decode.  Doesn't need to be NUL-terminated.
 * @srclen: the length of @src in bytes
 * @dst: (output) the decoded binary data
 * @padding: whether to append '=' padding characters
 * @variant: which base64 variant to use
 *
 * Decodes a string using the selected Base64 variant.
 *
 * Return: the length of the resulting decoded binary data in bytes,
 *	   or -1 if the string isn't a valid Base64 string.
 */
int base64_decode(const char *src, int srclen, u8 *dst, bool padding, enum base64_variant variant)
{
	u8 *bp = dst;
	s8 input1, input2, input3, input4;
	u32 val;

	if (srclen == 0)
		return 0;

	/* Validate the input length for padding */
	if (unlikely(padding && (srclen & 0x03) != 0))
		return -1;

	while (srclen >= 4) {
		/* Decode the next 4 characters */
		input1 = base64_rev_tables[variant][(u8)src[0]];
		input2 = base64_rev_tables[variant][(u8)src[1]];
		input3 = base64_rev_tables[variant][(u8)src[2]];
		input4 = base64_rev_tables[variant][(u8)src[3]];

		/* Return error if any Base64 character is invalid */
		if (unlikely(input1 < 0 || input2 < 0 || (!padding && (input3 < 0 || input4 < 0))))
			return -1;

		/* Handle padding */
		if (unlikely(padding && ((input3 < 0 && input4 >= 0) ||
					 (input3 < 0 && src[2] != '=') ||
					 (input4 < 0 && src[3] != '=') ||
					 (srclen > 4 && (input3 < 0 || input4 < 0)))))
			return -1;
		val = ((u32)input1 << 18) |
		      ((u32)input2 << 12) |
		      ((u32)((input3 < 0) ? 0 : input3) << 6) |
		      (u32)((input4 < 0) ? 0 : input4);

		*bp++ = (u8)(val >> 16);

		if (input3 >= 0)
			*bp++ = (u8)(val >> 8);
		if (input4 >= 0)
			*bp++ = (u8)val;

		src += 4;
		srclen -= 4;
	}

	/* Handle leftover characters when padding is not used */
	if (!padding && srclen > 0) {
		switch (srclen) {
		case 2:
			input1 = base64_rev_tables[variant][(u8)src[0]];
			input2 = base64_rev_tables[variant][(u8)src[1]];
			if (unlikely(input1 < 0 || input2 < 0))
				return -1;

			val = ((u32)input1 << 6) | (u32)input2; /* 12 bits */
			if (unlikely(val & 0x0F))
				return -1; /* low 4 bits must be zero */

			*bp++ = (u8)(val >> 4);
			break;
		case 3:
			input1 = base64_rev_tables[variant][(u8)src[0]];
			input2 = base64_rev_tables[variant][(u8)src[1]];
			input3 = base64_rev_tables[variant][(u8)src[2]];
			if (unlikely(input1 < 0 || input2 < 0 || input3 < 0))
				return -1;

			val = ((u32)input1 << 12) |
			      ((u32)input2 << 6) |
			      (u32)input3; /* 18 bits */

			if (unlikely(val & 0x03))
				return -1; /* low 2 bits must be zero */

			*bp++ = (u8)(val >> 10);
			*bp++ = (u8)((val >> 2) & 0xFF);
			break;
		default:
			return -1;
		}
	}

	return bp - dst;
}
EXPORT_SYMBOL_GPL(base64_decode);

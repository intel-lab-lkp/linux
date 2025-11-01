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

#define BASE64_REV_INIT(ch_62, ch_63) { \
	[0 ... 255] = -1, \
	['A'] =  0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, \
		13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, \
	['a'] = 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, \
		39, 40, 41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, \
	['0'] = 52, 53, 54, 55, 56, 57, 58, 59, 60, 61, \
	[ch_62] = 62, [ch_63] = 63, \
}

static const s8 base64_rev_maps[][256] = {
	[BASE64_STD] = BASE64_REV_INIT('+', '/'),
	[BASE64_URLSAFE] = BASE64_REV_INIT('-', '_'),
	[BASE64_IMAP] = BASE64_REV_INIT('+', ',')
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
		ac = (u32)src[0] << 16 | (u32)src[1] << 8 | (u32)src[2];
		*cp++ = base64_table[ac >> 18];
		*cp++ = base64_table[(ac >> 12) & 0x3f];
		*cp++ = base64_table[(ac >> 6) & 0x3f];
		*cp++ = base64_table[ac & 0x3f];

		src += 3;
		srclen -= 3;
	}

	switch (srclen) {
	case 2:
		ac = (u32)src[0] << 16 | (u32)src[1] << 8;
		*cp++ = base64_table[ac >> 18];
		*cp++ = base64_table[(ac >> 12) & 0x3f];
		*cp++ = base64_table[(ac >> 6) & 0x3f];
		if (padding)
			*cp++ = '=';
		break;
	case 1:
		ac = (u32)src[0] << 16;
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
	s8 input[4];
	s32 val;
	const u8 *s = (const u8 *)src;
	const s8 *base64_rev_tables = base64_rev_maps[variant];

	while (srclen >= 4) {
		input[0] = base64_rev_tables[s[0]];
		input[1] = base64_rev_tables[s[1]];
		input[2] = base64_rev_tables[s[2]];
		input[3] = base64_rev_tables[s[3]];

		val = input[0] << 18 | input[1] << 12 | input[2] << 6 | input[3];

		if (unlikely(val < 0)) {
			if (!padding || srclen != 4 || s[3] != '=')
				return -1;
			padding = 0;
			srclen = s[2] == '=' ? 2 : 3;
			break;
		}

		*bp++ = val >> 16;
		*bp++ = val >> 8;
		*bp++ = val;

		s += 4;
		srclen -= 4;
	}

	if (likely(!srclen))
		return bp - dst;
	if (padding || srclen == 1)
		return -1;

	val = (base64_rev_tables[s[0]] << 12) | (base64_rev_tables[s[1]] << 6);
	*bp++ = val >> 10;

	if (srclen == 2) {
		if (val & 0x800003ff)
			return -1;
	} else {
		val |= base64_rev_tables[s[2]];
		if (val & 0x80000003)
			return -1;
		*bp++ = val >> 2;
	}
	return bp - dst;
}
EXPORT_SYMBOL_GPL(base64_decode);


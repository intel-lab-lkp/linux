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
	int bits = 0;
	int i;
	char *cp = dst;
	const char *base64_table = base64_tables[variant];

	for (i = 0; i < srclen; i++) {
		ac = (ac << 8) | src[i];
		bits += 8;
		do {
			bits -= 6;
			*cp++ = base64_table[(ac >> bits) & 0x3f];
		} while (bits >= 6);
	}
	if (bits) {
		*cp++ = base64_table[(ac << (6 - bits)) & 0x3f];
		bits -= 6;
	}
	if (padding) {
		while (bits < 0) {
			*cp++ = '=';
			bits += 2;
		}
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
 * This implementation hasn't been optimized for performance.
 *
 * Return: the length of the resulting decoded binary data in bytes,
 *	   or -1 if the string isn't a valid Base64 string.
 */
int base64_decode(const char *src, int srclen, u8 *dst, bool padding, enum base64_variant variant)
{
	u32 ac = 0;
	int bits = 0;
	int i;
	u8 *bp = dst;
	s8 ch;

	for (i = 0; i < srclen; i++) {
		if (padding) {
			if (src[i] == '=') {
				ac = (ac << 6);
				bits += 6;
				if (bits >= 8)
					bits -= 8;
				continue;
			}
		}
		ch = base64_rev_maps[variant][(u8)src[i]];
		if (ch == -1)
			return -1;
		ac = (ac << 6) | ch;
		bits += 6;
		if (bits >= 8) {
			bits -= 8;
			*bp++ = (u8)(ac >> bits);
		}
	}
	if (ac & ((1 << bits) - 1))
		return -1;
	return bp - dst;
}
EXPORT_SYMBOL_GPL(base64_decode);

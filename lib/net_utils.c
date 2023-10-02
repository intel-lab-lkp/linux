// SPDX-License-Identifier: GPL-2.0
#include <linux/string.h>
#include <linux/if_ether.h>
#include <linux/ctype.h>
#include <linux/export.h>
#include <linux/hex.h>

bool mac_pton(const char *s, u8 *mac)
{
	size_t minlen = 2 * ETH_ALEN;
	size_t maxlen = 3 * ETH_ALEN - 1;
	int i;

	/* AABBCCDDEEFF */
	if (strnlen(s, maxlen) == minlen)
		goto no_delim;

	/* XX:XX:XX:XX:XX:XX */
	if (strnlen(s, maxlen) < maxlen)
		return false;

	/* Don't dirty result unless string is valid MAC. */
	for (i = 0; i < ETH_ALEN; i++) {
		if (!isxdigit(s[i * 3]) || !isxdigit(s[i * 3 + 1]))
			return false;
		if (i != ETH_ALEN - 1 && !ispunct(s[i * 3 + 2]))
			return false;
	}
	for (i = 0; i < ETH_ALEN; i++) {
		mac[i] = (hex_to_bin(s[i * 3]) << 4) | hex_to_bin(s[i * 3 + 1]);
	}
	return true;

no_delim:
	for (i = 0; i < minlen; i++) {
		if (!isxdigit(s[i]))
			return false;
	}
	for (i = 0; i < ETH_ALEN; i++) {
		mac[i] = (hex_to_bin(s[i * 2]) << 4) | hex_to_bin(s[i * 2 + 1]);
	}
	return true;
}
EXPORT_SYMBOL(mac_pton);

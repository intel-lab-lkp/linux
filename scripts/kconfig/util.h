/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef UTIL_H
#define UTIL_H

static unsigned int strhash(const char *s)
{
	/* fnv32 hash */
	unsigned int hash = 2166136261U;

	for (; *s; s++)
		hash = (hash ^ *s) * 0x01000193;
	return hash;
}

#endif /* UTIL_H */

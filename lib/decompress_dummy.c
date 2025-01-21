// SPDX-License-Identifier: GPL-2.0
STATIC int INIT __decompress(unsigned char *buf, long len,
			   long (*fill)(void*, unsigned long),
			   long (*flush)(void*, unsigned long),
			   unsigned char *out_buf, long out_len,
			   long *pos,
			   void (*error)(char *x))
{
	if (out_len < len-4) {
		error("output buffer too small");
		return -1;
	}
	memcpy(out_buf, buf, len-4);
	return 0;
}

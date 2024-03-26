// SPDX-License-Identifier: GPL-2.0
#ifndef _DECOMPRESSOR_H
#define _DECOMPRESSOR_H

/* The linker tells us where the image is. */
extern unsigned char __image_begin[], __image_end[];

/* debug interfaces  */
#ifdef CONFIG_DEBUG_ZBOOT
extern void printascii(const char *s);
extern void printhexl(unsigned long val);
#define puts(s) printascii(s)
#define puthex(val) printhexl(val)
#else
#define puts(s) do {} while (0)
#define puthex(val) do {} while (0)
#endif

extern char __appended_dtb[];

void error(char *x);
void decompress_kernel(unsigned long boot_heap_start);

#endif

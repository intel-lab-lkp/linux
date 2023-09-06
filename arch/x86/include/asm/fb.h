/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_X86_FB_H
#define _ASM_X86_FB_H

#include <asm/page.h>

struct fb_info;

pgprot_t fb_pgprot_device(pgprot_t prot,
			  unsigned long vm_start, unsigned long vm_end,
			  unsigned long offset);
#define fb_pgprot_device fb_pgprot_device

int fb_is_primary_device(struct fb_info *info);
#define fb_is_primary_device fb_is_primary_device

#include <asm-generic/fb.h>

#endif /* _ASM_X86_FB_H */

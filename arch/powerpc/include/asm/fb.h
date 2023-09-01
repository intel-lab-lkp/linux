/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_FB_H_
#define _ASM_FB_H_

#include <asm/page.h>

static inline pgprot_t fb_pgprot_device(pgprot_t prot,
					unsigned long vm_start, unsigned long vm_end,
					unsigned long offset)
{
	return __phys_mem_access_prot(PHYS_PFN(offset), vm_end - vm_start, prot);
}
#define fb_pgprot_device fb_pgprot_device

#include <asm-generic/fb.h>

#endif /* _ASM_FB_H_ */

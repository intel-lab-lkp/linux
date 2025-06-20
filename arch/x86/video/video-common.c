/*
 * Copyright (C) 2007 Antonino Daplas <adaplas@gmail.com>
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License.  See the file COPYING in the main directory of this archive
 * for more details.
 *
 */

#include <linux/module.h>
#include <linux/pci.h>
#include <linux/screen_info.h>
#include <linux/vgaarb.h>

#include <asm/video.h>

pgprot_t pgprot_framebuffer(pgprot_t prot,
			    unsigned long vm_start, unsigned long vm_end,
			    unsigned long offset)
{
	pgprot_val(prot) &= ~_PAGE_CACHE_MASK;
	if (boot_cpu_data.x86 > 3)
		pgprot_val(prot) |= cachemode2protval(_PAGE_CACHE_MODE_UC_MINUS);

	return prot;
}
EXPORT_SYMBOL(pgprot_framebuffer);

bool video_is_primary_device(struct device *dev)
{
	u64 base = screen_info.lfb_base;
	u64 size = screen_info.lfb_size;
	struct pci_dev *pdev;
	struct resource *r;
	u64 limit;

	if (!dev_is_pci(dev))
		return false;

	pdev = to_pci_dev(dev);

	if (!pci_is_display(pdev))
		return false;

	/* Select the device owning the boot framebuffer if there is one */
	if (screen_info.capabilities & VIDEO_CAPABILITY_64BIT_BASE)
		base |= (u64)screen_info.ext_lfb_base << 32;

	limit = base + size;

	/* Does firmware framebuffer belong to us? */
	pci_dev_for_each_resource(pdev, r) {
		if (resource_type(r) != IORESOURCE_MEM)
			continue;

		if (!r->start || !r->end)
			continue;

		if (base < r->start || limit >= r->end)
			continue;

		return true;
	}

	return (pdev == vga_default_device());
}
EXPORT_SYMBOL(video_is_primary_device);

MODULE_LICENSE("GPL");

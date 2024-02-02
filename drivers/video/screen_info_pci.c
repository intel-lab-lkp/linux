// SPDX-License-Identifier: GPL-2.0

#include <linux/pci.h>
#include <linux/screen_info.h>

static struct pci_dev *__screen_info_pci_dev(struct resource *res)
{
	struct pci_dev *pdev;

	if (!(res->flags & IORESOURCE_MEM))
		return NULL;

	for_each_pci_dev(pdev) {
		const struct resource *r;

		if ((pdev->class >> 16) != PCI_BASE_CLASS_DISPLAY)
			continue;

		r = pci_find_resource(pdev, res);
		if (r)
			return pdev;
	}

	return NULL;
}

/**
 * screen_info_pci_dev() - Return PCI parent device that contains screen_info's framebuffer
 * @si: the screen_info
 *
 * Returns:
 * The screen_info's parent device on success, or NULL otherwise.
 */
struct pci_dev *screen_info_pci_dev(const struct screen_info *si)
{
	struct resource res[SCREEN_INFO_MAX_RESOURCES];
	ssize_t i, numres;

	numres = screen_info_resources(si, res, ARRAY_SIZE(res));
	if (numres < 0)
		return ERR_PTR(numres);

	for (i = 0; i < numres; ++i) {
		struct pci_dev *pdev = __screen_info_pci_dev(&res[i]);

		if (pdev)
			return pdev;
	}

	return NULL;
}
EXPORT_SYMBOL(screen_info_pci_dev);

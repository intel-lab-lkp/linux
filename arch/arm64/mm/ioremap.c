// SPDX-License-Identifier: GPL-2.0-only

#include <linux/mm.h>
#include <linux/io.h>

#ifdef CONFIG_ALTRA_ERRATUM_82288

#define CREATE_TRACE_POINTS
#include <trace/events/altra_fixup.h>

bool have_altra_erratum_82288 __read_mostly;
EXPORT_SYMBOL(have_altra_erratum_82288);

void do_trace_altra_mkspecial(pte_t pte)
{
	trace_altra_mkspecial(pte);
}
EXPORT_SYMBOL(do_trace_altra_mkspecial);
EXPORT_TRACEPOINT_SYMBOL(altra_mkspecial);

static bool is_altra_pci(phys_addr_t phys_addr, size_t size)
{
	phys_addr_t end = phys_addr + size;

	return (phys_addr < 0x80000000 ||
		(end > 0x200000000000 && phys_addr < 0x400000000000) ||
		(end > 0x600000000000 && phys_addr < 0x800000000000));
}
#endif

pgprot_t ioremap_map_prot(phys_addr_t phys_addr, size_t size,
                          unsigned long prot_val)
{
	pgprot_t prot = __pgprot(prot_val);
#ifdef CONFIG_ALTRA_ERRATUM_82288
	if (unlikely(have_altra_erratum_82288 && is_altra_pci(phys_addr, size))) {
		prot = pgprot_device(prot);
		trace_altra_ioremap_prot(prot);
	}
#endif
	return prot;
}

void __iomem *ioremap_prot(phys_addr_t phys_addr, size_t size,
			   unsigned long prot)
{
	unsigned long last_addr = phys_addr + size - 1;

	/* Don't allow outside PHYS_MASK */
	if (last_addr & ~PHYS_MASK)
		return NULL;

	/* Don't allow RAM to be mapped. */
	if (WARN_ON(pfn_is_map_memory(__phys_to_pfn(phys_addr))))
		return NULL;

	return generic_ioremap_prot(phys_addr, size, __pgprot(prot));
}
EXPORT_SYMBOL(ioremap_prot);

/*
 * Must be called after early_fixmap_init
 */
void __init early_ioremap_init(void)
{
	early_ioremap_setup();
}

bool arch_memremap_can_ram_remap(resource_size_t offset, size_t size,
				 unsigned long flags)
{
	unsigned long pfn = PHYS_PFN(offset);

	return pfn_is_map_memory(pfn);
}

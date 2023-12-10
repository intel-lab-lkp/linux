// SPDX-License-Identifier: GPL-2.0
#include <linux/acpi.h>
#include <linux/export.h>
#include <linux/pci.h>

#include <xen/hvc-console.h>

#include <asm/io_apic.h>
#include <asm/hypervisor.h>
#include <asm/e820/api.h>

#include <xen/xen.h>
#include <asm/xen/interface.h>
#include <asm/xen/hypercall.h>

#include <xen/interface/memory.h>

#include "xen-ops.h"

/*
 * PVH variables.
 *
 * The variable xen_pvh needs to live in a data segment since it is used
 * after startup_{32|64} is invoked, which will clear the .bss segment.
 */
bool __ro_after_init xen_pvh;
EXPORT_SYMBOL_GPL(xen_pvh);

typedef struct gsi_info {
	u32 gsi;
	int trigger;
	int polarity;
	int pirq;
} gsi_info_t;

struct acpi_prt_entry {
	struct acpi_pci_id	id;
	u8			pin;
	acpi_handle		link;
	u32			index;		/* GSI, or link _CRS index */
};

static int xen_pvh_get_gsi_info(struct pci_dev *dev,
								gsi_info_t *gsi_info)
{
	int gsi;
	u8 pin = 0;
	struct acpi_prt_entry *entry;
	int trigger = ACPI_LEVEL_SENSITIVE;
	int polarity = acpi_irq_model == ACPI_IRQ_MODEL_GIC ?
				      ACPI_ACTIVE_HIGH : ACPI_ACTIVE_LOW;

	if (dev)
		pin = dev->pin;
	if (!dev || !pin || !gsi_info)
		return -EINVAL;

	entry = acpi_pci_irq_lookup(dev, pin);
	if (entry) {
		if (entry->link)
			gsi = acpi_pci_link_allocate_irq(entry->link,
							 entry->index,
							 &trigger, &polarity,
							 NULL);
		else
			gsi = entry->index;
	} else
		return -EINVAL;

	if (gsi < 0)
		return -EINVAL;

	gsi_info->gsi = gsi;
	gsi_info->trigger = trigger;
	gsi_info->polarity = polarity;

	return 0;
}

static int xen_pvh_setup_gsi(gsi_info_t *gsi_info)
{
	struct physdev_setup_gsi setup_gsi;

	if (!gsi_info)
		return -EINVAL;

	setup_gsi.gsi = gsi_info->gsi;
	setup_gsi.triggering = (gsi_info->trigger == ACPI_EDGE_SENSITIVE ? 0 : 1);
	setup_gsi.polarity = (gsi_info->polarity == ACPI_ACTIVE_HIGH ? 0 : 1);

	return HYPERVISOR_physdev_op(PHYSDEVOP_setup_gsi, &setup_gsi);
}

static int xen_pvh_map_pirq(gsi_info_t *gsi_info)
{
	struct physdev_map_pirq map_irq;
	int ret;

	if (!gsi_info)
		return -EINVAL;

	map_irq.domid = DOMID_SELF;
	map_irq.type = MAP_PIRQ_TYPE_GSI;
	map_irq.index = gsi_info->gsi;
	map_irq.pirq = gsi_info->gsi;

	ret = HYPERVISOR_physdev_op(PHYSDEVOP_map_pirq, &map_irq);
	gsi_info->pirq = map_irq.pirq;

	return ret;
}

int xen_pvh_passthrough_gsi(struct pci_dev *dev)
{
	int ret;
	gsi_info_t gsi_info;

	if (!dev)
		return -EINVAL;

	ret = xen_pvh_get_gsi_info(dev, &gsi_info);
	if (ret) {
		xen_raw_printk("Fail to get gsi info!\n");
		return ret;
	}

	ret = xen_pvh_setup_gsi(&gsi_info);
	if (ret == -EEXIST) {
		ret = 0;
		xen_raw_printk("Already setup the GSI :%u\n", gsi_info.gsi);
	} else if (ret) {
		xen_raw_printk("Fail to setup gsi (%d)!\n", gsi_info.gsi);
		return ret;
	}

	ret = xen_pvh_map_pirq(&gsi_info);
	if (ret)
		xen_raw_printk("Fail to map pirq for gsi (%d)!\n", gsi_info.gsi);

	return ret;
}
EXPORT_SYMBOL_GPL(xen_pvh_passthrough_gsi);

void __init xen_pvh_init(struct boot_params *boot_params)
{
	u32 msr;
	u64 pfn;

	xen_pvh = 1;
	xen_domain_type = XEN_HVM_DOMAIN;
	xen_start_flags = pvh_start_info.flags;

	msr = cpuid_ebx(xen_cpuid_base() + 2);
	pfn = __pa(hypercall_page);
	wrmsr_safe(msr, (u32)pfn, (u32)(pfn >> 32));

	if (xen_initial_domain())
		x86_init.oem.arch_setup = xen_add_preferred_consoles;
	x86_init.oem.banner = xen_banner;

	xen_efi_init(boot_params);

	if (xen_initial_domain()) {
		struct xen_platform_op op = {
			.cmd = XENPF_get_dom0_console,
		};
		int ret = HYPERVISOR_platform_op(&op);

		if (ret > 0)
			xen_init_vga(&op.u.dom0_console,
				     min(ret * sizeof(char),
					 sizeof(op.u.dom0_console)),
				     &boot_params->screen_info);
	}
}

void __init mem_map_via_hcall(struct boot_params *boot_params_p)
{
	struct xen_memory_map memmap;
	int rc;

	memmap.nr_entries = ARRAY_SIZE(boot_params_p->e820_table);
	set_xen_guest_handle(memmap.buffer, boot_params_p->e820_table);
	rc = HYPERVISOR_memory_op(XENMEM_memory_map, &memmap);
	if (rc) {
		xen_raw_printk("XENMEM_memory_map failed (%d)\n", rc);
		BUG();
	}
	boot_params_p->e820_entries = memmap.nr_entries;
}

// SPDX-License-Identifier: GPL-2.0
/*
 * Author:  Xiang Gao <gaoxiang@loongson.cn>
 *          Huacai Chen <chenhuacai@loongson.cn>
 *
 * Copyright (C) 2020-2022 Loongson Technology Corporation Limited
 */
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/mmzone.h>
#include <linux/export.h>
#include <linux/nodemask.h>
#include <linux/numa_memblks.h>
#include <linux/swap.h>
#include <linux/memblock.h>
#include <linux/pfn.h>
#include <linux/acpi.h>
#include <linux/efi.h>
#include <linux/irq.h>
#include <linux/pci.h>
#include <asm/bootinfo.h>
#include <asm/loongson.h>
#include <asm/numa.h>
#include <asm/page.h>
#include <asm/pgalloc.h>
#include <asm/sections.h>
#include <asm/time.h>

int numa_off;
cpumask_t cpus_on_node[MAX_NUMNODES];
cpumask_t phys_cpus_on_node[MAX_NUMNODES];
EXPORT_SYMBOL(cpus_on_node);

/*
 * apicid, cpu, node mappings
 */
s16 __cpuid_to_node[CONFIG_NR_CPUS] = {
	[0 ... CONFIG_NR_CPUS - 1] = NUMA_NO_NODE
};
EXPORT_SYMBOL(__cpuid_to_node);

#ifdef CONFIG_HAVE_SETUP_PER_CPU_AREA
unsigned long __per_cpu_offset[NR_CPUS] __read_mostly;
EXPORT_SYMBOL(__per_cpu_offset);

static int __init pcpu_cpu_to_node(int cpu)
{
	return early_cpu_to_node(cpu);
}

static int __init pcpu_cpu_distance(unsigned int from, unsigned int to)
{
	if (early_cpu_to_node(from) == early_cpu_to_node(to))
		return LOCAL_DISTANCE;
	else
		return REMOTE_DISTANCE;
}

void __init pcpu_populate_pte(unsigned long addr)
{
	populate_kernel_pte(addr);
}

void __init setup_per_cpu_areas(void)
{
	unsigned long delta;
	unsigned int cpu;
	int rc = -EINVAL;

	if (pcpu_chosen_fc == PCPU_FC_AUTO) {
		if (nr_node_ids >= 8)
			pcpu_chosen_fc = PCPU_FC_PAGE;
		else
			pcpu_chosen_fc = PCPU_FC_EMBED;
	}

	/*
	 * Always reserve area for module percpu variables.  That's
	 * what the legacy allocator did.
	 */
	if (pcpu_chosen_fc != PCPU_FC_PAGE) {
		rc = pcpu_embed_first_chunk(PERCPU_MODULE_RESERVE,
					    PERCPU_DYNAMIC_RESERVE, PMD_SIZE,
					    pcpu_cpu_distance, pcpu_cpu_to_node);
		if (rc < 0)
			pr_warn("%s allocator failed (%d), falling back to page size\n",
				pcpu_fc_names[pcpu_chosen_fc], rc);
	}
	if (rc < 0)
		rc = pcpu_page_first_chunk(PERCPU_MODULE_RESERVE, pcpu_cpu_to_node);
	if (rc < 0)
		panic("cannot initialize percpu area (err=%d)", rc);

	delta = (unsigned long)pcpu_base_addr - (unsigned long)__per_cpu_start;
	for_each_possible_cpu(cpu)
		__per_cpu_offset[cpu] = delta + pcpu_unit_offsets[cpu];
}
#endif

/*
 * Get nodeid by logical cpu number.
 * __cpuid_to_node maps phyical cpu id to node, so we
 * should use cpu_logical_map(cpu) to index it.
 *
 * This routine is only used in early phase during
 * booting, after setup_per_cpu_areas calling and numa_node
 * initialization, cpu_to_node will be used instead.
 */
int early_cpu_to_node(int cpu)
{
	int physid = cpu_logical_map(cpu);

	if (physid < 0)
		return NUMA_NO_NODE;

	return __cpuid_to_node[physid];
}

void __init early_numa_add_cpu(int cpuid, s16 node)
{
	int cpu = __cpu_number_map[cpuid];

	if (cpu < 0)
		return;

	cpumask_set_cpu(cpu, &cpus_on_node[node]);
	cpumask_set_cpu(cpuid, &phys_cpus_on_node[node]);
}

void numa_add_cpu(unsigned int cpu)
{
	int nid = cpu_to_node(cpu);
	cpumask_set_cpu(cpu, &cpus_on_node[nid]);
}

void numa_remove_cpu(unsigned int cpu)
{
	int nid = cpu_to_node(cpu);
	cpumask_clear_cpu(cpu, &cpus_on_node[nid]);
}

static void __init node_mem_init(unsigned int node)
{
	unsigned long start_pfn, end_pfn;
	unsigned long node_addrspace_offset;

	node_addrspace_offset = nid_to_addrbase(node);
	pr_info("Node%d's addrspace_offset is 0x%lx\n",
			node, node_addrspace_offset);

	get_pfn_range_for_nid(node, &start_pfn, &end_pfn);
	pr_info("Node%d: start_pfn=0x%lx, end_pfn=0x%lx\n",
		node, start_pfn, end_pfn);

	alloc_node_data(node);
}

#ifdef CONFIG_ACPI_NUMA
/*
 * fake_numa_init() - For Non-ACPI systems
 * Return: 0 on success, -errno on failure.
 */
static int __init fake_numa_init(void)
{
	phys_addr_t start = memblock_start_of_DRAM();
	phys_addr_t end = memblock_end_of_DRAM() - 1;

	node_set(0, numa_nodes_parsed);
	pr_info("Faking a node at [mem %pap-%pap]\n", &start, &end);

	return numa_add_memblk(0, start, end + 1);
}

int __init init_numa_memory(void)
{
	int i;
	int ret;
	int node;

	for (i = 0; i < NR_CPUS; i++)
		set_cpuid_to_node(i, NUMA_NO_NODE);

	/* Parse SRAT and SLIT if provided by firmware. */
	ret = acpi_disabled ? numa_memblks_init(fake_numa_init, false) :
			numa_memblks_init(acpi_numa_init, false);
	if (ret < 0)
		return ret;

	if (!memblock_validate_numa_coverage(SZ_1M))
		return -EINVAL;

	for_each_node_mask(node, node_possible_map) {
		node_mem_init(node);
		node_set_online(node);
	}
	max_low_pfn = PHYS_PFN(memblock_end_of_DRAM());

	setup_nr_node_ids();
	loongson_sysconf.nr_nodes = nr_node_ids;
	loongson_sysconf.cores_per_node = cpumask_weight(&phys_cpus_on_node[0]);

	return 0;
}

#endif

void __init paging_init(void)
{
	unsigned int node;
	unsigned long zones_size[MAX_NR_ZONES] = {0, };

	for_each_online_node(node) {
		unsigned long start_pfn, end_pfn;

		get_pfn_range_for_nid(node, &start_pfn, &end_pfn);

		if (end_pfn > max_low_pfn)
			max_low_pfn = end_pfn;
	}
#ifdef CONFIG_ZONE_DMA32
	zones_size[ZONE_DMA32] = MAX_DMA32_PFN;
#endif
	zones_size[ZONE_NORMAL] = max_low_pfn;
	free_area_init(zones_size);
}

int pcibus_to_node(struct pci_bus *bus)
{
	return dev_to_node(&bus->dev);
}
EXPORT_SYMBOL(pcibus_to_node);

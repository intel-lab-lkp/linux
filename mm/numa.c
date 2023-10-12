// SPDX-License-Identifier: GPL-2.0-only
#include <linux/acpi.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/string.h>
#include <linux/init.h>
#include <linux/memblock.h>
#include <linux/mmzone.h>
#include <linux/ctype.h>
#include <linux/nodemask.h>
#include <linux/sched.h>
#include <linux/topology.h>

#include <asm/dma.h>

struct numa_meminfo numa_meminfo __initdata_or_meminfo;
struct numa_meminfo numa_reserved_meminfo __initdata_or_meminfo;

#ifdef CONFIG_NUMA_EMU
char *emu_cmdline __initdata;
#endif

/*
 * Set nodes, which have memory in @mi, in *@nodemask.
 */
void __init numa_nodemask_from_meminfo(nodemask_t *nodemask,
					      const struct numa_meminfo *mi)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(mi->blk); i++)
		if (mi->blk[i].start != mi->blk[i].end &&
		    mi->blk[i].nid != NUMA_NO_NODE)
			node_set(mi->blk[i].nid, *nodemask);
}

/**
 * numa_remove_memblk_from - Remove one numa_memblk from a numa_meminfo
 * @idx: Index of memblk to remove
 * @mi: numa_meminfo to remove memblk from
 *
 * Remove @idx'th numa_memblk from @mi by shifting @mi->blk[] and
 * decrementing @mi->nr_blks.
 */
static void __init numa_remove_memblk_from(int idx, struct numa_meminfo *mi)
{
	mi->nr_blks--;
	memmove(&mi->blk[idx], &mi->blk[idx + 1],
		(mi->nr_blks - idx) * sizeof(mi->blk[0]));
}

/**
 * numa_move_tail_memblk - Move a numa_memblk from one numa_meminfo to another
 * @dst: numa_meminfo to append block to
 * @idx: Index of memblk to remove
 * @src: numa_meminfo to remove memblk from
 */
static void __init numa_move_tail_memblk(struct numa_meminfo *dst, int idx,
					 struct numa_meminfo *src)
{
	dst->blk[dst->nr_blks++] = src->blk[idx];
	numa_remove_memblk_from(idx, src);
}

int __init numa_add_memblk_to(int nid, u64 start, u64 end,
				     struct numa_meminfo *mi)
{
	/* ignore zero length blks */
	if (start == end)
		return 0;

	/* whine about and ignore invalid blks */
	if (start > end || nid < 0 || nid >= MAX_NUMNODES) {
		pr_warn("Warning: invalid memblk node %d [mem %#010Lx-%#010Lx]\n",
			nid, start, end - 1);
		return 0;
	}

	if (mi->nr_blks >= NR_NODE_MEMBLKS) {
		pr_err("too many memblk ranges\n");
		return -EINVAL;
	}

	mi->blk[mi->nr_blks].start = start;
	mi->blk[mi->nr_blks].end = end;
	mi->blk[mi->nr_blks].nid = nid;
	mi->nr_blks++;
	return 0;
}

/**
 * numa_cleanup_meminfo - Cleanup a numa_meminfo
 * @mi: numa_meminfo to clean up
 *
 * Sanitize @mi by merging and removing unnecessary memblks.  Also check for
 * conflicts and clear unused memblks.
 *
 * RETURNS:
 * 0 on success, -errno on failure.
 */
int __init numa_cleanup_meminfo(struct numa_meminfo *mi)
{
	const u64 low = 0;
	const u64 high = PFN_PHYS(max_pfn);
	int i, j, k;

	/* first, trim all entries */
	for (i = 0; i < mi->nr_blks; i++) {
		struct numa_memblk *bi = &mi->blk[i];

		/* move / save reserved memory ranges */
		if (!memblock_overlaps_region(&memblock.memory,
					bi->start, bi->end - bi->start)) {
			numa_move_tail_memblk(&numa_reserved_meminfo, i--, mi);
			continue;
		}

		/* make sure all non-reserved blocks are inside the limits */
		bi->start = max(bi->start, low);

		/* preserve info for non-RAM areas above 'max_pfn': */
		if (bi->end > high) {
			numa_add_memblk_to(bi->nid, high, bi->end,
					   &numa_reserved_meminfo);
			bi->end = high;
		}

		/* and there's no empty block */
		if (bi->start >= bi->end)
			numa_remove_memblk_from(i--, mi);
	}

	/* merge neighboring / overlapping entries */
	for (i = 0; i < mi->nr_blks; i++) {
		struct numa_memblk *bi = &mi->blk[i];

		for (j = i + 1; j < mi->nr_blks; j++) {
			struct numa_memblk *bj = &mi->blk[j];
			u64 start, end;

			/*
			 * See whether there are overlapping blocks.  Whine
			 * about but allow overlaps of the same nid.  They
			 * will be merged below.
			 */
			if (bi->end > bj->start && bi->start < bj->end) {
				if (bi->nid != bj->nid) {
					pr_err("node %d [mem %#010Lx-%#010Lx] overlaps with node %d [mem %#010Lx-%#010Lx]\n",
					       bi->nid, bi->start, bi->end - 1,
					       bj->nid, bj->start, bj->end - 1);
					return -EINVAL;
				}
				pr_warn("Warning: node %d [mem %#010Lx-%#010Lx] overlaps with itself [mem %#010Lx-%#010Lx]\n",
					bi->nid, bi->start, bi->end - 1,
					bj->start, bj->end - 1);
			}

			/*
			 * Join together blocks on the same node, holes
			 * between which don't overlap with memory on other
			 * nodes.
			 */
			if (bi->nid != bj->nid)
				continue;
			start = min(bi->start, bj->start);
			end = max(bi->end, bj->end);
			for (k = 0; k < mi->nr_blks; k++) {
				struct numa_memblk *bk = &mi->blk[k];

				if (bi->nid == bk->nid)
					continue;
				if (start < bk->end && end > bk->start)
					break;
			}
			if (k < mi->nr_blks)
				continue;
			printk(KERN_INFO "NUMA: Node %d [mem %#010Lx-%#010Lx] + [mem %#010Lx-%#010Lx] -> [mem %#010Lx-%#010Lx]\n",
			       bi->nid, bi->start, bi->end - 1, bj->start,
			       bj->end - 1, start, end - 1);
			bi->start = start;
			bi->end = end;
			numa_remove_memblk_from(j--, mi);
		}
	}

	/* clear unused ones */
	for (i = mi->nr_blks; i < ARRAY_SIZE(mi->blk); i++) {
		mi->blk[i].start = mi->blk[i].end = 0;
		mi->blk[i].nid = NUMA_NO_NODE;
	}

	return 0;
}

/*
 * Mark all currently memblock-reserved physical memory (which covers the
 * kernel's own memory ranges) as hot-unswappable.
 */
static void __init numa_clear_kernel_node_hotplug(void)
{
	nodemask_t reserved_nodemask = NODE_MASK_NONE;
	struct memblock_region *mb_region;
	int i;

	/*
	 * We have to do some preprocessing of memblock regions, to
	 * make them suitable for reservation.
	 *
	 * At this time, all memory regions reserved by memblock are
	 * used by the kernel, but those regions are not split up
	 * along node boundaries yet, and don't necessarily have their
	 * node ID set yet either.
	 *
	 * So iterate over all memory known to the x86 architecture,
	 * and use those ranges to set the nid in memblock.reserved.
	 * This will split up the memblock regions along node
	 * boundaries and will set the node IDs as well.
	 */
	for (i = 0; i < numa_meminfo.nr_blks; i++) {
		struct numa_memblk *mb = numa_meminfo.blk + i;
		int ret;

		ret = memblock_set_node(mb->start, mb->end - mb->start, &memblock.reserved, mb->nid);
		WARN_ON_ONCE(ret);
	}

	/*
	 * Now go over all reserved memblock regions, to construct a
	 * node mask of all kernel reserved memory areas.
	 *
	 * [ Note, when booting with mem=nn[kMG] or in a kdump kernel,
	 *   numa_meminfo might not include all memblock.reserved
	 *   memory ranges, because quirks such as trim_snb_memory()
	 *   reserve specific pages for Sandy Bridge graphics. ]
	 */
	for_each_reserved_mem_region(mb_region) {
		int nid = memblock_get_region_node(mb_region);

		if (nid != MAX_NUMNODES)
			node_set(nid, reserved_nodemask);
	}

	/*
	 * Finally, clear the MEMBLOCK_HOTPLUG flag for all memory
	 * belonging to the reserved node mask.
	 *
	 * Note that this will include memory regions that reside
	 * on nodes that contain kernel memory - entire nodes
	 * become hot-unpluggable:
	 */
	for (i = 0; i < numa_meminfo.nr_blks; i++) {
		struct numa_memblk *mb = numa_meminfo.blk + i;

		if (!node_isset(mb->nid, reserved_nodemask))
			continue;

		memblock_clear_hotplug(mb->start, mb->end - mb->start);
	}
}

int __weak __init numa_register_memblks(struct numa_meminfo *mi)
{
	int i;

	/* Account for nodes with cpus and no memory */
	node_possible_map = numa_nodes_parsed;
	numa_nodemask_from_meminfo(&node_possible_map, mi);
	if (WARN_ON(nodes_empty(node_possible_map)))
		return -EINVAL;

	for (i = 0; i < mi->nr_blks; i++) {
		struct numa_memblk *mb = &mi->blk[i];
		memblock_set_node(mb->start, mb->end - mb->start,
				  &memblock.memory, mb->nid);
	}

	/*
	 * At very early time, the kernel have to use some memory such as
	 * loading the kernel image. We cannot prevent this anyway. So any
	 * node the kernel resides in should be un-hotpluggable.
	 *
	 * And when we come here, alloc node data won't fail.
	 */
	numa_clear_kernel_node_hotplug();

	/*
	 * If sections array is gonna be used for pfn -> nid mapping, check
	 * whether its granularity is fine enough.
	 */
	if (IS_ENABLED(NODE_NOT_IN_PAGE_FLAGS)) {
		unsigned long pfn_align = node_map_pfn_alignment();

		if (pfn_align && pfn_align < PAGES_PER_SECTION) {
			pr_warn("Node alignment %LuMB < min %LuMB, rejecting NUMA config\n",
				PFN_PHYS(pfn_align) >> 20,
				PFN_PHYS(PAGES_PER_SECTION) >> 20);
			return -EINVAL;
		}
	}

	return 0;
}

#ifdef CONFIG_NUMA_EMU
int __init numa_emu_cmdline(char *str)
{
	emu_cmdline = str;
	return 0;
}
#endif

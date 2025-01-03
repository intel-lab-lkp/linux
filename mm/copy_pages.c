// SPDX-License-Identifier: GPL-2.0
/*
 * Parallel page copy routine.
 */

#include <linux/sysctl.h>
#include <linux/highmem.h>
#include <linux/workqueue.h>
#include <linux/slab.h>
#include <linux/migrate.h>


unsigned int sysctl_limit_mt_num = 4;
/* push by default */
unsigned int sysctl_push_0_pull_1;

struct copy_item {
	char *to;
	char *from;
	unsigned long chunk_size;
};

struct copy_page_info {
	struct work_struct copy_page_work;
	unsigned long num_items;
	struct copy_item item_list[];
};

static void copy_page_routine(char *vto, char *vfrom,
	unsigned long chunk_size)
{
	memcpy(vto, vfrom, chunk_size);
}

static void copy_page_work_queue_thread(struct work_struct *work)
{
	struct copy_page_info *my_work = (struct copy_page_info *)work;
	int i;

	for (i = 0; i < my_work->num_items; ++i)
		copy_page_routine(my_work->item_list[i].to,
						  my_work->item_list[i].from,
						  my_work->item_list[i].chunk_size);
}

int copy_page_lists_mt(struct list_head *dst_folios,
		struct list_head *src_folios, int nr_items)
{
	int err = 0;
	unsigned int total_mt_num = sysctl_limit_mt_num;
	int to_node = folio_nid(list_first_entry(dst_folios, struct folio, lru));
	int from_node = folio_nid(list_first_entry(src_folios, struct folio, lru));
	int i;
	struct copy_page_info *work_items[32] = {0};
	const struct cpumask *per_node_cpumask =
		cpumask_of_node(sysctl_push_0_pull_1 ? to_node : from_node);
	int cpu_id_list[32] = {0};
	int cpu;
	int max_items_per_thread;
	int item_idx;
	struct folio *src, *src2, *dst, *dst2;

	total_mt_num = min_t(unsigned int, total_mt_num,
			cpumask_weight(per_node_cpumask));

	if (total_mt_num > 32)
		total_mt_num = 32;

	/* Each threads get part of each page, if nr_items < totla_mt_num */
	if (nr_items < total_mt_num)
		max_items_per_thread = nr_items;
	else
		max_items_per_thread = (nr_items / total_mt_num) +
				((nr_items % total_mt_num) ? 1 : 0);


	for (cpu = 0; cpu < total_mt_num; ++cpu) {
		work_items[cpu] = kzalloc(sizeof(struct copy_page_info) +
					sizeof(struct copy_item) * max_items_per_thread,
					GFP_NOWAIT);
		if (!work_items[cpu]) {
			err = -ENOMEM;
			goto free_work_items;
		}
	}

	i = 0;
	/* TODO: need a better cpu selection method */
	for_each_cpu(cpu, per_node_cpumask) {
		if (i >= total_mt_num)
			break;
		cpu_id_list[i] = cpu;
		++i;
	}

	if (nr_items < total_mt_num) {
		for (cpu = 0; cpu < total_mt_num; ++cpu) {
			INIT_WORK((struct work_struct *)work_items[cpu],
					  copy_page_work_queue_thread);
			work_items[cpu]->num_items = max_items_per_thread;
		}

		item_idx = 0;
		dst = list_first_entry(dst_folios, struct folio, lru);
		dst2 = list_next_entry(dst, lru);
		list_for_each_entry_safe(src, src2, src_folios, lru) {
			unsigned long chunk_size = PAGE_SIZE * folio_nr_pages(src) / total_mt_num;
			/* XXX: not working in HIGHMEM */
			char *vfrom = page_address(&src->page);
			char *vto = page_address(&dst->page);

			VM_WARN_ON(PAGE_SIZE * folio_nr_pages(src) % total_mt_num);
			VM_WARN_ON(folio_nr_pages(dst) != folio_nr_pages(src));

			for (cpu = 0; cpu < total_mt_num; ++cpu) {
				work_items[cpu]->item_list[item_idx].to =
					vto + chunk_size * cpu;
				work_items[cpu]->item_list[item_idx].from =
					vfrom + chunk_size * cpu;
				work_items[cpu]->item_list[item_idx].chunk_size =
					chunk_size;
			}

			item_idx++;
			dst = dst2;
			dst2 = list_next_entry(dst, lru);
		}

		for (cpu = 0; cpu < total_mt_num; ++cpu)
			queue_work_on(cpu_id_list[cpu],
						  system_unbound_wq,
						  (struct work_struct *)work_items[cpu]);
	} else {
		int num_xfer_per_thread = nr_items / total_mt_num;
		int per_cpu_item_idx;


		for (cpu = 0; cpu < total_mt_num; ++cpu) {
			INIT_WORK((struct work_struct *)work_items[cpu],
					  copy_page_work_queue_thread);

			work_items[cpu]->num_items = num_xfer_per_thread +
					(cpu < (nr_items % total_mt_num));
		}

		cpu = 0;
		per_cpu_item_idx = 0;
		item_idx = 0;
		dst = list_first_entry(dst_folios, struct folio, lru);
		dst2 = list_next_entry(dst, lru);
		list_for_each_entry_safe(src, src2, src_folios, lru) {
			/* XXX: not working in HIGHMEM */
			work_items[cpu]->item_list[per_cpu_item_idx].to =
				page_address(&dst->page);
			work_items[cpu]->item_list[per_cpu_item_idx].from =
				page_address(&src->page);
			work_items[cpu]->item_list[per_cpu_item_idx].chunk_size =
				PAGE_SIZE * folio_nr_pages(src);

			VM_WARN_ON(folio_nr_pages(dst) !=
				   folio_nr_pages(src));

			per_cpu_item_idx++;
			item_idx++;
			dst = dst2;
			dst2 = list_next_entry(dst, lru);

			if (per_cpu_item_idx == work_items[cpu]->num_items) {
				queue_work_on(cpu_id_list[cpu],
					system_unbound_wq,
					(struct work_struct *)work_items[cpu]);
				per_cpu_item_idx = 0;
				cpu++;
			}
		}
		if (item_idx != nr_items)
			pr_warn("%s: only %d out of %d pages are transferred\n",
				__func__, item_idx - 1, nr_items);
	}

	/* Wait until it finishes  */
	for (i = 0; i < total_mt_num; ++i)
		flush_work((struct work_struct *)work_items[i]);

free_work_items:
	for (cpu = 0; cpu < total_mt_num; ++cpu)
		kfree(work_items[cpu]);

	return err;
}

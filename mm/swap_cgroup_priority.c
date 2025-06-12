// SPDX-License-Identifier: GPL-2.0-or-later

/* per mem_cgroup */
struct swap_cgroup_priority {
	struct list_head link;
	/* XXX: to flatten memory is hard. variable array is our enemy */
	struct swap_cgroup_priority_pnode *pnode[MAX_SWAPFILES];
	struct plist_head plist[];
};

/* per mem_cgroup & per swap device node */
struct swap_cgroup_priority_pnode {
	struct swap_info_struct *swap;
	int prio;
	struct plist_node avail_lists[];
};

/* per swap device unique id counter */
static atomic_t swap_unique_id_counter;

/* active swap_cgroup_priority list */
static LIST_HEAD(swap_cgroup_priority_list);

/* XXX: Not want memcontrol to know swap_cgroup_priority internal. */
void show_swap_device_unique_id(struct seq_file *m)
{
	struct mem_cgroup *memcg = mem_cgroup_from_seq(m);

	spin_lock(&swap_lock);
	/* XXX: what is beautiful visibility? */
	seq_printf(m, "%s\n", memcg->swap_priority ? "Active" : "Inactive");
	for (int i = 0; i < nr_swapfiles; i++) {
		struct swap_info_struct *si = swap_info[i];

		if (!(si->flags & SWP_USED))
			continue;

		seq_file_path(m, si->swap_file, "\t\n\\");
		seq_printf(m,  "\tunique:%d\t", si->unique_id);

		if (!memcg->swap_priority) {
			seq_printf(m, " prio:%d\n", si->prio);
			continue;
		}

		seq_printf(m,  "prio:%d\n",
			memcg->swap_priority->pnode[i]->prio);
	}
	spin_unlock(&swap_lock);
}

static void get_swap_unique_id(struct swap_info_struct *si)
{
	si->unique_id = atomic_add_return(1, &swap_unique_id_counter);
}

int create_swap_cgroup_priority(struct mem_cgroup *memcg,
		int unique[], int prio[], int nr)
{
	bool b_found = false;
	struct swap_cgroup_priority *swap_priority, *old_swap_priority = NULL;
	int nid;

	/* Fast check */
	if (nr != nr_swapfiles)
		return -EINVAL;

	/*
	* XXX: always make newly object and exchange it.
	* possible to give object reusability if it is simple and better.
	*/
	swap_priority = kvmalloc(struct_size(swap_priority, plist, nr_node_ids),
			GFP_KERNEL);

	if (!swap_priority)
		return -ENOMEM;

	/* XXX: use pre allocate. think swapon time allocate is better? */
	for (int i = 0; i < MAX_SWAPFILES; i++) {
		swap_priority->pnode[i] =
			kvmalloc(struct_size(swap_priority->pnode[0],
				avail_lists, nr_node_ids),
				GFP_KERNEL);

		if (!swap_priority->pnode[i]) {
			for (int j = 0; j < i; j++)
				kvfree(swap_priority->pnode[i]);

			kvfree(swap_priority);
			return -ENOMEM;
		}
	}

	INIT_LIST_HEAD(&swap_priority->link);
	for_each_node(nid)
		plist_head_init(&swap_priority->plist[nid]);

	spin_lock(&swap_lock);
	spin_lock(&swap_avail_lock);

	/* swap on/off under us. */
	if (nr != nr_swapfiles)
		goto error;

	/* TODO: naive search. make it fast.*/
	for (int i = 0; i < nr; i++) {
		b_found = false;
		for (int j = 0; j < nr_swapfiles; j++) {
			struct swap_info_struct *si = swap_info[j];
			struct swap_cgroup_priority_pnode *pnode
					= swap_priority->pnode[j];

			if (si->unique_id != unique[i])
				continue;

			/* swap off under us */
			if (!(si->flags & SWP_USED))
				goto error;

			int k;
			for_each_node(k) {
				if (prio[i] >= 0) {
					pnode->prio = prio[i];
					plist_node_init(&pnode->avail_lists[k],
						-pnode->prio);
				} else {
					pnode->prio = si->prio;
					if (swap_node(si) == k)
						plist_node_init(
							&pnode->avail_lists[k],
							1);
					else
						plist_node_init(
							&pnode->avail_lists[k],
							-pnode->prio);
				}

				plist_add(&pnode->avail_lists[k],
					&swap_priority->plist[k]);
			}

			pnode->swap = si;
			b_found = true;
			break;
		}

		/* cannot find unique id pair */
		if (!b_found)
			goto error;
	}

	if (memcg->swap_priority) {
		old_swap_priority = memcg->swap_priority;
		list_del(&old_swap_priority->link);
	}

	list_add(&swap_priority->link, &swap_cgroup_priority_list);

	memcg->swap_priority = swap_priority;
	spin_unlock(&swap_avail_lock);
	spin_unlock(&swap_lock);

	if (old_swap_priority) {
		for (int i = 0; i < MAX_SWAPFILES; i++)
			kvfree(old_swap_priority->pnode[i]);
		kvfree(old_swap_priority);
	}

	return 0;

error:
	spin_unlock(&swap_avail_lock);
	spin_unlock(&swap_lock);

	for (int i = 0; i < MAX_SWAPFILES; i++)
		kvfree(swap_priority->pnode[i]);
	kvfree(swap_priority);

	return -EINVAL;
}

void delete_swap_cgroup_priority(struct mem_cgroup *memcg)
{
	struct swap_cgroup_priority *swap_priority;

	spin_lock(&swap_avail_lock);
	swap_priority = memcg->swap_priority;
	if (!swap_priority) {
		spin_unlock(&swap_avail_lock);
		return;
	}
	memcg->swap_priority = NULL;
	list_del(&swap_priority->link);
	spin_unlock(&swap_avail_lock);

	/* wait show_swap_device_unique_id */
	synchronize_rcu();

	for (int i = 0; i < MAX_SWAPFILES; i++)
		kvfree(swap_priority->pnode[i]);
	kvfree(swap_priority);
}

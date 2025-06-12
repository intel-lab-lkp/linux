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

static bool swap_alloc_cgroup_priority(struct mem_cgroup *memcg,
				swp_entry_t *entry, int order)
{
	struct swap_cgroup_priority *swap_priority;
	struct swap_cgroup_priority_pnode *pnode, *next;
	unsigned long offset;
	int node;

	if (!memcg)
		return false;

	spin_lock(&swap_avail_lock);
priority_check:
	swap_priority = memcg->swap_priority;
	if (!swap_priority) {
		spin_unlock(&swap_avail_lock);
		return false;
	}

	node = numa_node_id();
start_over:
	plist_for_each_entry_safe(pnode, next, &swap_priority->plist[node],
					avail_lists[node]) {
		struct swap_info_struct *si = pnode->swap;
		plist_requeue(&pnode->avail_lists[node],
			&swap_priority->plist[node]);
		spin_unlock(&swap_avail_lock);

		if (get_swap_device_info(si)) {
			offset = cluster_alloc_swap_entry(si,
					order, SWAP_HAS_CACHE, true);
			put_swap_device(si);
			if (offset) {
				*entry = swp_entry(si->type, offset);
				return true;
			}
			if (order)
				return false;
		}

		spin_lock(&swap_avail_lock);

		/* swap_priority is remove or changed under us. */
		if (swap_priority != memcg->swap_priority)
			goto priority_check;

		if (plist_node_empty(&next->avail_lists[node]))
			goto start_over;
	}
	spin_unlock(&swap_avail_lock);

	return false;
}

/* add_to_avail_list (swapon / swapusage > 0) */
static void activate_swap_cgroup_priority_pnode(struct swap_info_struct *swp,
			bool swapon)
{
	struct swap_cgroup_priority *swap_priority;
	int i;

	list_for_each_entry(swap_priority, &swap_cgroup_priority_list, link) {
		struct swap_cgroup_priority_pnode *pnode
			= swap_priority->pnode[swp->type];

		if (swapon) {
			pnode->swap = swp;
			pnode->prio = swp->prio;
		}

		/* NUMA priority handling */
		for_each_node(i) {
			if (swapon) {
				if (swap_node(swp) == i) {
					plist_node_init(
						&pnode->avail_lists[i],
						1);
				} else {
					plist_node_init(
						&pnode->avail_lists[i],
						-pnode->prio);
				}
			}

			plist_add(&pnode->avail_lists[i],
				&swap_priority->plist[i]);
		}
	}
}

/* del_from_avail_list (swapoff / swap usage <= 0) */
static void deactivate_swap_cgroup_priority_pnode(struct swap_info_struct *swp,
		bool swapoff)
{
	struct swap_cgroup_priority *swap_priority;
	int nid, i;

	list_for_each_entry(swap_priority, &swap_cgroup_priority_list, link) {
		struct swap_cgroup_priority_pnode *pnode;

		if (swapoff && swp->prio < 0) {
			/*
			* NUMA priority handling
			* mimic swapoff prio adjustment without plist
			*/
			for (int i = 0; i < MAX_SWAPFILES; i++) {
				pnode = swap_priority->pnode[i];
				if (pnode->prio > swp->prio ||
					pnode->swap == swp)
					continue;

				pnode->prio++;
				for_each_node(nid) {
					if (pnode->avail_lists[nid].prio != 1)
						pnode->avail_lists[nid].prio--;
				}
			}
		}

		pnode = swap_priority->pnode[swp->type];
		for_each_node(i)
			plist_del(&pnode->avail_lists[i],
				&swap_priority->plist[i]);
	}
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

	/*
	* XXX: Possible RCU wait? No. Cannot protect priority list addition.
	* swap_avail_lock gives protection.
	* Think about other object protection mechanism
	* might be solve it and better. (e.g object reference)
	*/
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

// SPDX-License-Identifier: GPL-2.0-only
#include <linux/module.h>
#include <linux/moduleloader.h>
#include <linux/mutex.h>
#include <linux/orc.h>
#include <linux/sort.h>

struct orc_entry *__orc_find(int *ip_table, struct orc_entry *u_table,
			     unsigned int num_entries, unsigned long ip)
{
	int *first = ip_table;
	int *last = ip_table + num_entries - 1;
	int *mid, *found = first;

	if (!num_entries)
		return NULL;

	/*
	 * Do a binary range search to find the rightmost duplicate of a given
	 * starting address.  Some entries are section terminators which are
	 * "weak" entries for ensuring there are no gaps.  They should be
	 * ignored when they conflict with a real entry.
	 */
	while (first <= last) {
		mid = first + ((last - first) / 2);

		if (orc_ip(mid) <= ip) {
			found = mid;
			first = mid + 1;
		} else {
			last = mid - 1;
		}
	}

	return u_table + (found - ip_table);
}

#ifdef CONFIG_MODULES
struct orc_entry *orc_module_find(unsigned long ip)
{
	struct module *mod;

	mod = __module_address(ip);
	if (!mod || !mod->orc_unwind || !mod->orc_unwind_ip)
		return NULL;
	return __orc_find(mod->orc_unwind_ip, mod->orc_unwind,
			  mod->num_orcs, ip);
}

static DEFINE_MUTEX(sort_mutex);
static int *cur_orc_ip_table;
static struct orc_entry *cur_orc_table;

static void orc_sort_swap(void *_a, void *_b, int size)
{
	struct orc_entry *orc_a, *orc_b;
	int *a = _a, *b = _b, tmp;
	int delta = _b - _a;

	/* Swap the .orc_unwind_ip entries: */
	tmp = *a;
	*a = *b + delta;
	*b = tmp - delta;

	/* Swap the corresponding .orc_unwind entries: */
	orc_a = cur_orc_table + (a - cur_orc_ip_table);
	orc_b = cur_orc_table + (b - cur_orc_ip_table);
	swap(*orc_a, *orc_b);
}

static int orc_sort_cmp(const void *_a, const void *_b)
{
	struct orc_entry *orc_a;
	const int *a = _a, *b = _b;
	unsigned long a_val = orc_ip(a);
	unsigned long b_val = orc_ip(b);

	if (a_val > b_val)
		return 1;
	if (a_val < b_val)
		return -1;

	/*
	 * The "weak" section terminator entries need to always be first
	 * to ensure the lookup code skips them in favor of real entries.
	 * These terminator entries exist to handle any gaps created by
	 * whitelisted .o files which didn't get objtool generation.
	 */
	orc_a = cur_orc_table + (a - cur_orc_ip_table);
	return orc_a->type == ORC_TYPE_UNDEFINED ? -1 : 1;
}

void unwind_module_init(struct module *mod, void *_orc_ip, size_t orc_ip_size,
			void *_orc, size_t orc_size)
{
	int *orc_ip = _orc_ip;
	struct orc_entry *orc = _orc;
	unsigned int num_entries = orc_ip_size / sizeof(int);

	WARN_ON_ONCE(orc_ip_size % sizeof(int) != 0 ||
		     orc_size % sizeof(*orc) != 0 ||
		     num_entries != orc_size / sizeof(*orc));

	/*
	 * The 'cur_orc_*' globals allow the orc_sort_swap() callback to
	 * associate an .orc_unwind_ip table entry with its corresponding
	 * .orc_unwind entry so they can both be swapped.
	 */
	mutex_lock(&sort_mutex);
	cur_orc_ip_table = orc_ip;
	cur_orc_table = orc;
	sort(orc_ip, num_entries, sizeof(int), orc_sort_cmp, orc_sort_swap);
	mutex_unlock(&sort_mutex);

	mod->orc_unwind_ip = orc_ip;
	mod->orc_unwind = orc;
	mod->num_orcs = num_entries;
}
#endif

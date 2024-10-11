// SPDX-License-Identifier: GPL-2.0
#ifdef __KERNEL__
# include <linux/slab.h>
# include <linux/crush/crush.h>
#else
# include "crush_compat.h"
# include "crush.h"
#endif

void crush_destroy_bucket_uniform(struct crush_bucket_uniform *b)
{
	kfree(b->h.items);
	kfree(b);
}

void crush_destroy_bucket_list(struct crush_bucket_list *b)
{
	kfree(b->item_weights);
	kfree(b->sum_weights);
	kfree(b->h.items);
	kfree(b);
}

void crush_destroy_bucket_tree(struct crush_bucket_tree *b)
{
	kfree(b->h.items);
	kfree(b->node_weights);
	kfree(b);
}

void crush_destroy_bucket_straw(struct crush_bucket_straw *b)
{
	kfree(b->straws);
	kfree(b->item_weights);
	kfree(b->h.items);
	kfree(b);
}

void crush_destroy_bucket_straw2(struct crush_bucket_straw2 *b)
{
	kfree(b->item_weights);
	kfree(b->h.items);
	kfree(b);
}

void crush_destroy_bucket(struct crush_bucket *b)
{
	switch (b->alg) {
	case CRUSH_BUCKET_UNIFORM:
		crush_destroy_bucket_uniform((struct crush_bucket_uniform *)b);
		break;
	case CRUSH_BUCKET_LIST:
		crush_destroy_bucket_list((struct crush_bucket_list *)b);
		break;
	case CRUSH_BUCKET_TREE:
		crush_destroy_bucket_tree((struct crush_bucket_tree *)b);
		break;
	case CRUSH_BUCKET_STRAW:
		crush_destroy_bucket_straw((struct crush_bucket_straw *)b);
		break;
	case CRUSH_BUCKET_STRAW2:
		crush_destroy_bucket_straw2((struct crush_bucket_straw2 *)b);
		break;
	}
}

/**
 * crush_destroy - Destroy a crush_map
 * @map: crush_map pointer
 */
void crush_destroy(struct crush_map *map)
{
	/* buckets */
	if (map->buckets) {
		__s32 b;
		for (b = 0; b < map->max_buckets; b++) {
			if (map->buckets[b] == NULL)
				continue;
			crush_destroy_bucket(map->buckets[b]);
		}
		kfree(map->buckets);
	}

	/* rules */
	if (map->rules) {
		__u32 b;
		for (b = 0; b < map->max_rules; b++)
			crush_destroy_rule(map->rules[b]);
		kfree(map->rules);
	}

#ifndef __KERNEL__
	kfree(map->choose_tries);
#else
	clear_crush_names(&map->type_names);
	clear_crush_names(&map->names);
	clear_choose_args(map);
#endif
	kfree(map);
}

void crush_destroy_rule(struct crush_rule *rule)
{
	kfree(rule);
}

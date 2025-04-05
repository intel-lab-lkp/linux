// SPDX-License-Identifier: GPL-2.0

#include <linux/maple_tree.h>


void rust_helper_mt_init_flags(struct maple_tree *mt, unsigned int flags)
{
	mt_init_flags(mt, flags);
}

void rust_helper_mtree_lock(struct maple_tree *mt)
{
	mtree_lock(mt);
}

void rust_helper_mtree_unlock(struct maple_tree *mt)
{
	mtree_unlock(mt);
}

struct ma_state rust_helper_MA_STATE(struct maple_tree *mt, unsigned long start, unsigned long end)
{
	MA_STATE(mas, mt, start, end);
	return mas;
}

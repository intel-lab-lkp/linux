// SPDX-License-Identifier: GPL-2.0
#include <linux/debugfs.h>
#include <linux/seq_file.h>

#include <asm/ptdump.h>
#include <asm/pgtable.h>
#include <asm/kasan.h>

static int ptdump_show(struct seq_file *m, void *v)
{
	struct ptd_mm_info *info = m->private;

	ptdump_walk(m, info);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(ptdump);

void __init ptdump_debugfs_register(struct ptd_mm_info *info, const char *name)
{
	debugfs_create_file(name, 0400, NULL, info, &ptdump_fops);
}


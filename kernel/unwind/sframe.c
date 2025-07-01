// SPDX-License-Identifier: GPL-2.0
/*
 * Userspace sframe access functions
 */

#define pr_fmt(fmt)	"sframe: " fmt

#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/srcu.h>
#include <linux/uaccess.h>
#include <linux/mm.h>
#include <linux/string_helpers.h>
#include <linux/sframe.h>
#include <linux/unwind_user_types.h>

#include "sframe.h"

#define dbg(fmt, ...)							\
	pr_debug("%s (%d): " fmt, current->comm, current->pid, ##__VA_ARGS__)

static void free_section(struct sframe_section *sec)
{
	kfree(sec);
}

static int sframe_read_header(struct sframe_section *sec)
{
	unsigned long header_end, fdes_start, fdes_end, fres_start, fres_end;
	struct sframe_header shdr;
	unsigned int num_fdes;

	if (copy_from_user(&shdr, (void __user *)sec->sframe_start, sizeof(shdr))) {
		dbg("header usercopy failed\n");
		return -EFAULT;
	}

	if (shdr.preamble.magic != SFRAME_MAGIC ||
	    shdr.preamble.version != SFRAME_VERSION_2 ||
	    !(shdr.preamble.flags & SFRAME_F_FDE_SORTED) ||
	    shdr.auxhdr_len) {
		dbg("bad/unsupported sframe header\n");
		return -EINVAL;
	}

	if (!shdr.num_fdes || !shdr.num_fres) {
		dbg("no fde/fre entries\n");
		return -EINVAL;
	}

	header_end = sec->sframe_start + SFRAME_HEADER_SIZE(shdr);
	if (header_end >= sec->sframe_end) {
		dbg("header doesn't fit in section\n");
		return -EINVAL;
	}

	num_fdes   = shdr.num_fdes;
	fdes_start = header_end + shdr.fdes_off;
	fdes_end   = fdes_start + (num_fdes * sizeof(struct sframe_fde));

	fres_start = header_end + shdr.fres_off;
	fres_end   = fres_start + shdr.fre_len;

	if (fres_start < fdes_end || fres_end > sec->sframe_end) {
		dbg("inconsistent fde/fre offsets\n");
		return -EINVAL;
	}

	sec->num_fdes		= num_fdes;
	sec->fdes_start		= fdes_start;
	sec->fres_start		= fres_start;
	sec->fres_end		= fres_end;

	sec->ra_off		= shdr.cfa_fixed_ra_offset;
	sec->fp_off		= shdr.cfa_fixed_fp_offset;

	return 0;
}

int sframe_add_section(unsigned long sframe_start, unsigned long sframe_end,
		       unsigned long text_start, unsigned long text_end)
{
	struct maple_tree *sframe_mt = &current->mm->sframe_mt;
	struct vm_area_struct *sframe_vma, *text_vma;
	struct mm_struct *mm = current->mm;
	struct sframe_section *sec;
	int ret;

	if (!sframe_start || !sframe_end || !text_start || !text_end) {
		dbg("zero-length sframe/text address\n");
		return -EINVAL;
	}

	scoped_guard(mmap_read_lock, mm) {
		sframe_vma = vma_lookup(mm, sframe_start);
		if (!sframe_vma || sframe_end > sframe_vma->vm_end) {
			dbg("bad sframe address (0x%lx - 0x%lx)\n",
			    sframe_start, sframe_end);
			return -EINVAL;
		}

		text_vma = vma_lookup(mm, text_start);
		if (!text_vma ||
		    !(text_vma->vm_flags & VM_EXEC) ||
		    text_end > text_vma->vm_end) {
			dbg("bad text address (0x%lx - 0x%lx)\n",
			    text_start, text_end);
			return -EINVAL;
		}
	}

	sec = kzalloc(sizeof(*sec), GFP_KERNEL);
	if (!sec)
		return -ENOMEM;

	sec->sframe_start	= sframe_start;
	sec->sframe_end		= sframe_end;
	sec->text_start		= text_start;
	sec->text_end		= text_end;

	ret = sframe_read_header(sec);
	if (ret)
		goto err_free;

	/* TODO nowhere to store it yet - just free it and return an error */
	ret = -ENOSYS;

err_free:
	free_section(sec);
	return ret;
}

int sframe_remove_section(unsigned long sframe_start)
{
	return -ENOSYS;
}

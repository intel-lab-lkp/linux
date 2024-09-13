// SPDX-License-Identifier: GPL-2.0

#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/srcu.h>
#include <linux/uaccess.h>
#include <linux/mm.h>
#include <linux/sframe.h>
#include <linux/user_unwind.h>

#include "sframe.h"

#define SFRAME_FILENAME_LEN 32

struct sframe_section {
	struct rcu_head rcu;

	unsigned long sframe_addr;
	unsigned long text_addr;

	unsigned long fdes_addr;
	unsigned long fres_addr;
	unsigned int  fdes_nr;
	signed char ra_off, fp_off;
};

DEFINE_STATIC_SRCU(sframe_srcu);

#define __SFRAME_GET_USER(out, user_ptr, type)				\
({									\
	type __tmp;							\
	if (get_user(__tmp, (type *)user_ptr))				\
		return -EFAULT;						\
	user_ptr += sizeof(__tmp);					\
	out = __tmp;							\
})

#define SFRAME_GET_USER_SIGNED(out, user_ptr, size)			\
({									\
	switch (size) {							\
	case 1:								\
		__SFRAME_GET_USER(out, user_ptr, s8);			\
		break;							\
	case 2:								\
		__SFRAME_GET_USER(out, user_ptr, s16);			\
		break;							\
	case 4:								\
		__SFRAME_GET_USER(out, user_ptr, s32);			\
		break;							\
	default:							\
		return -EINVAL;						\
	}								\
})

#define SFRAME_GET_USER_UNSIGNED(out, user_ptr, size)			\
({									\
	switch (size) {							\
	case 1:								\
		__SFRAME_GET_USER(out, user_ptr, u8);			\
		break;							\
	case 2:								\
		__SFRAME_GET_USER(out, user_ptr, u16);			\
		break;							\
	case 4:								\
		__SFRAME_GET_USER(out, user_ptr, u32);			\
		break;							\
	default:							\
		return -EINVAL;						\
	}								\
})

static unsigned char fre_type_to_size(unsigned char fre_type)
{
	if (fre_type > 2)
		return 0;
	return 1 << fre_type;
}

static unsigned char offset_size_enum_to_size(unsigned char off_size)
{
	if (off_size > 2)
		return 0;
	return 1 << off_size;
}

static int find_fde(struct sframe_section *sec, unsigned long ip,
		    struct sframe_fde *fde)
{
	s32 func_off, ip_off;
	struct sframe_fde __user *first, *last, *mid, *found;

	ip_off = ip - sec->sframe_addr;

	first = (void *)sec->fdes_addr;
	last = first + sec->fdes_nr;
	while (first <= last) {
		mid = first + ((last - first) / 2);
		if (get_user(func_off, (s32 *)mid))
			return -EFAULT;
		if (ip_off >= func_off) {
			found = mid;
			first = mid + 1;
		} else
			last = mid - 1;
	}

	if (!found)
		return -EINVAL;

	if (copy_from_user(fde, found, sizeof(*fde)))
		return -EFAULT;

	return 0;
}

static int find_fre(struct sframe_section *sec, struct sframe_fde *fde,
		    unsigned long ip, struct user_unwind_frame *frame)
{
	unsigned char fde_type = SFRAME_FUNC_FDE_TYPE(fde->info);
	unsigned char fre_type = SFRAME_FUNC_FRE_TYPE(fde->info);
	s32 fre_ip_off, cfa_off, ra_off, fp_off, ip_off;
	unsigned char offset_count, offset_size;
	unsigned char addr_size;
	void __user *f, *last_f;
	u8 fre_info;
	int i;

	addr_size = fre_type_to_size(fre_type);
	if (!addr_size)
		return -EINVAL;

	ip_off = ip - sec->sframe_addr - fde->start_addr;

	f = (void *)sec->fres_addr + fde->fres_off;

	for (i = 0; i < fde->fres_num; i++) {

		SFRAME_GET_USER_UNSIGNED(fre_ip_off, f, addr_size);

		if (fde_type == SFRAME_FDE_TYPE_PCINC) {
			if (fre_ip_off > ip_off)
				break;
		} else {
			/* SFRAME_FDE_TYPE_PCMASK */
			if (ip_off % fde->rep_size < fre_ip_off)
				break;
		}

		SFRAME_GET_USER_UNSIGNED(fre_info, f, 1);

		offset_count = SFRAME_FRE_OFFSET_COUNT(fre_info);
		offset_size  = offset_size_enum_to_size(SFRAME_FRE_OFFSET_SIZE(fre_info));

		if (!offset_count || !offset_size)
			return -EINVAL;

		last_f = f;
		f += offset_count * offset_size;
	}

	if (!last_f)
		return -EINVAL;

	f = last_f;

	SFRAME_GET_USER_UNSIGNED(cfa_off, f, offset_size);
	offset_count--;

	ra_off = sec->ra_off;
	if (!ra_off) {
		if (!offset_count--)
			return -EINVAL;
		SFRAME_GET_USER_SIGNED(ra_off, f, offset_size);
	}

	fp_off = sec->fp_off;
	if (!fp_off && offset_count) {
		offset_count--;
		SFRAME_GET_USER_SIGNED(fp_off, f, offset_size);
	}

	if (offset_count)
		return -EINVAL;

	frame->cfa_off = cfa_off;
	frame->ra_off = ra_off;
	frame->fp_off = fp_off;
	frame->use_fp = SFRAME_FRE_CFA_BASE_REG_ID(fre_info) == SFRAME_BASE_REG_FP;

	return 0;
}

int sframe_find(unsigned long ip, struct user_unwind_frame *frame)
{
	struct mm_struct *mm = current->mm;
	struct sframe_section *sec;
	struct sframe_fde fde;
	int srcu_idx;
	int ret = -EINVAL;

	srcu_idx = srcu_read_lock(&sframe_srcu);

	sec = mtree_load(&mm->sframe_mt, ip);
	if (!sec) {
		srcu_read_unlock(&sframe_srcu, srcu_idx);
		return -EINVAL;
	}


	ret = find_fde(sec, ip, &fde);
	if (ret)
		goto err_unlock;

	ret = find_fre(sec, &fde, ip, frame);
	if (ret)
		goto err_unlock;

	srcu_read_unlock(&sframe_srcu, srcu_idx);
	return 0;

err_unlock:
	srcu_read_unlock(&sframe_srcu, srcu_idx);
	return ret;
}

static int get_sframe_file(unsigned long sframe_addr, struct sframe_file *file)
{
	struct mm_struct *mm = current->mm;
	struct vm_area_struct *sframe_vma, *text_vma, *vma;
	VMA_ITERATOR(vmi, mm, 0);

	mmap_read_lock(mm);

	sframe_vma = vma_lookup(mm, sframe_addr);
	if (!sframe_vma || !sframe_vma->vm_file)
		goto err_unlock;

	text_vma = NULL;

	for_each_vma(vmi, vma) {
		if (vma->vm_file != sframe_vma->vm_file)
			continue;
		if (vma->vm_flags & VM_EXEC) {
			if (text_vma) {
				/*
				 * Multiple EXEC segments in a single file
				 * aren't currently supported, is that a thing?
				 */
				mmap_read_unlock(mm);
				pr_warn_once("unsupported multiple EXEC segments in task %s[%d]\n",
					     current->comm, current->pid);
				return -EINVAL;
			}
			text_vma = vma;
		}
	}

	file->sframe_addr	= sframe_addr;
	file->text_start	= text_vma->vm_start;
	file->text_end		= text_vma->vm_end;

	mmap_read_unlock(mm);
	return 0;

err_unlock:
	mmap_read_unlock(mm);
	return -EINVAL;
}

static int validate_sframe_addrs(struct sframe_file *file)
{
	struct mm_struct *mm = current->mm;
	struct vm_area_struct *text_vma;

	mmap_read_lock(mm);

	if (!vma_lookup(mm, file->sframe_addr))
		goto err_unlock;

	text_vma = vma_lookup(mm, file->text_start);
	if (!(text_vma->vm_flags & VM_EXEC))
		goto err_unlock;

	if (vma_lookup(mm, file->text_end-1) != text_vma)
		goto err_unlock;

	mmap_read_unlock(mm);
	return 0;

err_unlock:
	mmap_read_unlock(mm);
	return -EINVAL;
}

int __sframe_add_section(struct sframe_file *file)
{
	struct maple_tree *sframe_mt = &current->mm->sframe_mt;
	struct sframe_section *sec;
	struct sframe_header shdr;
	unsigned long header_end;
	int ret;

	if (copy_from_user(&shdr, (void *)file->sframe_addr, sizeof(shdr)))
		return -EFAULT;

	if (shdr.preamble.magic != SFRAME_MAGIC ||
	    shdr.preamble.version != SFRAME_VERSION_2 ||
	    !(shdr.preamble.flags & SFRAME_F_FDE_SORTED) ||
	    shdr.auxhdr_len || !shdr.num_fdes || !shdr.num_fres ||
	    shdr.fdes_off > shdr.fres_off) {
		/*
		 * Either binutils < 2.41, corrupt sframe header, or
		 * unsupported feature.
		 * */
		pr_warn_once("bad sframe header in task %s[%d]\n",
			     current->comm, current->pid);
		return -EINVAL;
	}

	header_end = file->sframe_addr + SFRAME_HDR_SIZE(shdr);

	sec = kmalloc(sizeof(*sec), GFP_KERNEL);
	if (!sec)
		return -ENOMEM;

	sec->sframe_addr	= file->sframe_addr;
	sec->text_addr		= file->text_start;
	sec->fdes_addr		= header_end + shdr.fdes_off;
	sec->fres_addr		= header_end + shdr.fres_off;
	sec->fdes_nr		= shdr.num_fdes;
	sec->ra_off		= shdr.cfa_fixed_ra_offset;
	sec->fp_off		= shdr.cfa_fixed_fp_offset;

	ret = mtree_insert_range(sframe_mt, file->text_start, file->text_end,
				 sec, GFP_KERNEL);
	if (ret) {
		kfree(sec);
		return ret;
	}

	return 0;
}

int sframe_add_section(unsigned long sframe_addr, unsigned long text_start, unsigned long text_end)
{
	struct sframe_file file;
	int ret;

	if (!text_start || !text_end) {
		ret = get_sframe_file(sframe_addr, &file);
		if (ret)
			return ret;
	} else {
		/*
		 * This is mainly for generated code, for which the text isn't
		 * file-backed so the user has to give the text bounds.
		 */
		file.sframe_addr	= sframe_addr;
		file.text_start		= text_start;
		file.text_end		= text_end;
		ret = validate_sframe_addrs(&file);
		if (ret)
			return ret;
	}

	return __sframe_add_section(&file);
}

static void sframe_free_rcu(struct rcu_head *rcu)
{
	struct sframe_section *sec = container_of(rcu, struct sframe_section, rcu);

	kfree(sec);
}

static int __sframe_remove_section(struct mm_struct *mm,
				   struct sframe_section *sec)
{
	struct sframe_section *s;

	s = mtree_erase(&mm->sframe_mt, sec->text_addr);
	if (!s || WARN_ON_ONCE(s != sec))
		return -EINVAL;

	call_srcu(&sframe_srcu, &sec->rcu, sframe_free_rcu);

	return 0;
}

int sframe_remove_section(unsigned long sframe_addr)
{
	struct mm_struct *mm = current->mm;
	struct sframe_section *sec;
	unsigned long index = 0;

	sec = mtree_load(&mm->sframe_mt, sframe_addr);
	if (!sec)
		return -EINVAL;

	mt_for_each(&mm->sframe_mt, sec, index, ULONG_MAX) {
		if (sec->sframe_addr == sframe_addr)
			return __sframe_remove_section(mm, sec);
	}

	return -EINVAL;
}

void sframe_free_mm(struct mm_struct *mm)
{
	struct sframe_section *sec;
	unsigned long index = 0;

	if (!mm)
		return;

	mt_for_each(&mm->sframe_mt, sec, index, ULONG_MAX)
		kfree(sec);

	mtree_destroy(&mm->sframe_mt);
}

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
#include "sframe_debug.h"

struct sframe_fre {
	unsigned int	size;
	u32		ip_off;
	s32		cfa_off;
	s32		ra_off;
	s32		fp_off;
	u8		info;
};

DEFINE_STATIC_SRCU(sframe_srcu);

static __always_inline unsigned char fre_type_to_size(unsigned char fre_type)
{
	if (fre_type > 2)
		return 0;
	return 1 << fre_type;
}

static __always_inline unsigned char offset_size_enum_to_size(unsigned char off_size)
{
	if (off_size > 2)
		return 0;
	return 1 << off_size;
}

static __always_inline int __read_fde(struct sframe_section *sec,
				      unsigned int fde_num,
				      struct sframe_fde *fde)
{
	unsigned long fde_addr, ip;

	fde_addr = sec->fdes_start + (fde_num * sizeof(struct sframe_fde));
	unsafe_copy_from_user(fde, (void __user *)fde_addr,
			      sizeof(struct sframe_fde), Efault);

	ip = sec->sframe_start + fde->start_addr;
	if (ip < sec->text_start || ip > sec->text_end) {
		dbg_sec_uaccess("bad fde num %d\n", fde_num);
		return -EINVAL;
	}

	return 0;

Efault:
	dbg_sec_uaccess("fde %d usercopy failed\n", fde_num);
	return -EFAULT;
}

static __always_inline int __find_fde(struct sframe_section *sec,
				      unsigned long ip,
				      struct sframe_fde *fde)
{
	s32 ip_off, func_off_low = S32_MIN, func_off_high = S32_MAX;
	struct sframe_fde __user *first, *low, *high, *found = NULL;
	int ret;

	ip_off = ip - sec->sframe_start;

	first = (void __user *)sec->fdes_start;
	low = first;
	high = first + sec->num_fdes - 1;

	while (low <= high) {
		struct sframe_fde __user *mid;
		s32 func_off;

		mid = low + ((high - low) / 2);

		unsafe_get_user(func_off, (s32 __user *)mid, Efault);

		if (ip_off >= func_off) {
			if (func_off < func_off_low) {
				dbg_sec_uaccess("fde %u not sorted\n",
						(unsigned int)(mid - first));
				return -EFAULT;
			}

			func_off_low = func_off;

			found = mid;
			low = mid + 1;
		} else {
			if (func_off > func_off_high) {
				dbg_sec_uaccess("fde %u not sorted\n",
						(unsigned int)(mid - first));
				return -EFAULT;
			}

			func_off_high = func_off;

			high = mid - 1;
		}
	}

	if (!found)
		return -EINVAL;

	ret = __read_fde(sec, found - first, fde);
	if (ret)
		return ret;

	/* make sure it's not in a gap */
	if (ip_off < fde->start_addr || ip_off >= fde->start_addr + fde->func_size)
		return -EINVAL;

	return 0;

Efault:
	dbg_sec_uaccess("fde usercopy failed\n");
	return -EFAULT;
}

#define ____UNSAFE_GET_USER_INC(to, from, type, label)			\
({									\
	type __to;							\
	unsafe_get_user(__to, (type __user *)from, label);		\
	from += sizeof(__to);						\
	to = __to;							\
})

#define __UNSAFE_GET_USER_INC(to, from, size, label, u_or_s)		\
({									\
	switch (size) {							\
	case 1:								\
		____UNSAFE_GET_USER_INC(to, from, u_or_s##8, label);	\
		break;							\
	case 2:								\
		____UNSAFE_GET_USER_INC(to, from, u_or_s##16, label);	\
		break;							\
	case 4:								\
		____UNSAFE_GET_USER_INC(to, from, u_or_s##32, label);	\
		break;							\
	default:							\
		dbg_sec_uaccess("%d: bad UNSAFE_GET_USER_INC size %u\n",\
				__LINE__, size);			\
		return -EFAULT;						\
	}								\
})

#define UNSAFE_GET_USER_UNSIGNED_INC(to, from, size, label)		\
	__UNSAFE_GET_USER_INC(to, from, size, label, u)

#define UNSAFE_GET_USER_SIGNED_INC(to, from, size, label)		\
	__UNSAFE_GET_USER_INC(to, from, size, label, s)

#define UNSAFE_GET_USER_INC(to, from, size, label)				\
	_Generic(to,								\
		 u8:	UNSAFE_GET_USER_UNSIGNED_INC(to, from, size, label),	\
		 u16:	UNSAFE_GET_USER_UNSIGNED_INC(to, from, size, label),	\
		 u32:	UNSAFE_GET_USER_UNSIGNED_INC(to, from, size, label),	\
		 s8:	UNSAFE_GET_USER_SIGNED_INC(to, from, size, label),	\
		 s16:	UNSAFE_GET_USER_SIGNED_INC(to, from, size, label),	\
		 s32:	UNSAFE_GET_USER_SIGNED_INC(to, from, size, label))

static __always_inline int __read_fre(struct sframe_section *sec,
				      struct sframe_fde *fde,
				      unsigned long fre_addr,
				      struct sframe_fre *fre)
{
	unsigned char fde_type = SFRAME_FUNC_FDE_TYPE(fde->info);
	unsigned char fre_type = SFRAME_FUNC_FRE_TYPE(fde->info);
	unsigned char offset_count, offset_size;
	s32 cfa_off, ra_off, fp_off;
	unsigned long cur = fre_addr;
	unsigned char addr_size;
	u32 ip_off;
	u8 info;

	addr_size = fre_type_to_size(fre_type);
	if (!addr_size) {
		dbg_sec_uaccess("bad addr_size in fde info %u\n", fde->info);
		return -EFAULT;
	}

	if (fre_addr + addr_size + 1 > sec->fres_end) {
		dbg_sec_uaccess("fre addr+info goes past end of subsection\n");
		return -EFAULT;
	}

	UNSAFE_GET_USER_INC(ip_off, cur, addr_size, Efault);
	if (fde_type == SFRAME_FDE_TYPE_PCINC && ip_off > fde->func_size) {
		dbg_sec_uaccess("fre starts past end of function: ip_off=0x%x, func_size=0x%x\n",
				ip_off, fde->func_size);
		return -EFAULT;
	}

	UNSAFE_GET_USER_INC(info, cur, 1, Efault);
	offset_count = SFRAME_FRE_OFFSET_COUNT(info);
	offset_size  = offset_size_enum_to_size(SFRAME_FRE_OFFSET_SIZE(info));
	if (!offset_count || !offset_size) {
		dbg_sec_uaccess("zero offset_count or size in fre info %u\n",info);
		return -EFAULT;
	}
	if (cur + (offset_count * offset_size) > sec->fres_end) {
		dbg_sec_uaccess("fre goes past end of subsection\n");
		return -EFAULT;
	}

	fre->size = addr_size + 1 + (offset_count * offset_size);

	UNSAFE_GET_USER_INC(cfa_off, cur, offset_size, Efault);
	offset_count--;

	ra_off = sec->ra_off;
	if (!ra_off) {
		if (!offset_count--) {
			dbg_sec_uaccess("zero offset_count, can't find ra_off\n");
			return -EFAULT;
		}

		UNSAFE_GET_USER_INC(ra_off, cur, offset_size, Efault);
	}

	fp_off = sec->fp_off;
	if (!fp_off && offset_count) {
		offset_count--;
		UNSAFE_GET_USER_INC(fp_off, cur, offset_size, Efault);
	}

	if (offset_count) {
		dbg_sec_uaccess("non-zero offset_count after reading fre\n");
		return -EFAULT;
	}

	fre->ip_off		= ip_off;
	fre->cfa_off		= cfa_off;
	fre->ra_off		= ra_off;
	fre->fp_off		= fp_off;
	fre->info		= info;

	return 0;

Efault:
	dbg_sec_uaccess("fre usercopy failed\n");
	return -EFAULT;
}

static __always_inline int __find_fre(struct sframe_section *sec,
				      struct sframe_fde *fde, unsigned long ip,
				      struct unwind_user_frame *frame)
{
	unsigned char fde_type = SFRAME_FUNC_FDE_TYPE(fde->info);
	struct sframe_fre *fre, *prev_fre = NULL;
	struct sframe_fre fres[2];
	unsigned long fre_addr;
	bool which = false;
	unsigned int i;
	u32 ip_off;

	ip_off = ip - (sec->sframe_start + fde->start_addr);

	if (fde_type == SFRAME_FDE_TYPE_PCMASK)
		ip_off %= fde->rep_size;

	fre_addr = sec->fres_start + fde->fres_off;

	for (i = 0; i < fde->fres_num; i++) {
		int ret;

		/*
		 * Alternate between the two fre_addr[] entries for 'fre' and
		 * 'prev_fre'.
		 */
		fre = which ? fres : fres + 1;
		which = !which;

		ret = __read_fre(sec, fde, fre_addr, fre);
		if (ret) {
			dbg_sec_uaccess("fde addr 0x%x: __read_fre(%u) failed\n",
					fde->start_addr, i);
			dbg_print_fde_uaccess(sec, fde);
			return ret;
		}

		fre_addr += fre->size;

		if (prev_fre && fre->ip_off <= prev_fre->ip_off) {
			dbg_sec_uaccess("fde addr 0x%x: fre %u not sorted\n",
					fde->start_addr, i);
			return -EFAULT;
		}

		if (fre->ip_off > ip_off)
			break;

		prev_fre = fre;
	}

	if (!prev_fre)
		return -EINVAL;
	fre = prev_fre;

	frame->cfa_off = fre->cfa_off;
	frame->ra_off  = fre->ra_off;
	frame->fp_off  = fre->fp_off;
	frame->use_fp  = SFRAME_FRE_CFA_BASE_REG_ID(fre->info) == SFRAME_BASE_REG_FP;

	return 0;
}

int sframe_find(unsigned long ip, struct unwind_user_frame *frame)
{
	struct mm_struct *mm = current->mm;
	struct sframe_section *sec;
	struct sframe_fde fde;
	int ret;

	if (!mm)
		return -EINVAL;

	guard(srcu)(&sframe_srcu);

	sec = mtree_load(&mm->sframe_mt, ip);
	if (!sec)
		return -EINVAL;

	if (!user_read_access_begin((void __user *)sec->sframe_start,
				    sec->sframe_end - sec->sframe_start))
		return -EFAULT;

	ret = __find_fde(sec, ip, &fde);
	if (ret)
		goto end;

	ret = __find_fre(sec, &fde, ip, frame);
end:
	user_read_access_end();

	if (ret == -EFAULT) {
		dbg_sec("removing bad .sframe section\n");
		WARN_ON_ONCE(sframe_remove_section(sec->sframe_start));
	}

	return ret;
}

static void free_section(struct sframe_section *sec)
{
	dbg_free(sec);
	kfree(sec);
}

static int sframe_read_header(struct sframe_section *sec)
{
	unsigned long header_end, fdes_start, fdes_end, fres_start, fres_end;
	struct sframe_header shdr;
	unsigned int num_fdes;

	if (copy_from_user(&shdr, (void __user *)sec->sframe_start, sizeof(shdr))) {
		dbg_sec("header usercopy failed\n");
		return -EFAULT;
	}

	if (shdr.preamble.magic != SFRAME_MAGIC ||
	    shdr.preamble.version != SFRAME_VERSION_2 ||
	    !(shdr.preamble.flags & SFRAME_F_FDE_SORTED) ||
	    shdr.auxhdr_len) {
		dbg_sec("bad/unsupported sframe header\n");
		return -EINVAL;
	}

	if (!shdr.num_fdes || !shdr.num_fres) {
		dbg_sec("no fde/fre entries\n");
		return -EINVAL;
	}

	header_end = sec->sframe_start + SFRAME_HEADER_SIZE(shdr);
	if (header_end >= sec->sframe_end) {
		dbg_sec("header doesn't fit in section\n");
		return -EINVAL;
	}

	num_fdes   = shdr.num_fdes;
	fdes_start = header_end + shdr.fdes_off;
	fdes_end   = fdes_start + (num_fdes * sizeof(struct sframe_fde));

	fres_start = header_end + shdr.fres_off;
	fres_end   = fres_start + shdr.fre_len;

	if (fres_start < fdes_end || fres_end > sec->sframe_end) {
		dbg_sec("inconsistent fde/fre offsets\n");
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

	dbg_init(sec);

	ret = sframe_read_header(sec);
	if (ret) {
		dbg_print_header(sec);
		goto err_free;
	}

	ret = mtree_insert_range(sframe_mt, sec->text_start, sec->text_end, sec, GFP_KERNEL);
	if (ret) {
		dbg_sec("mtree_insert_range failed: text=%lx-%lx\n",
			sec->text_start, sec->text_end);
		goto err_free;
	}

	return 0;

err_free:
	free_section(sec);
	return ret;
}

static void sframe_free_srcu(struct rcu_head *rcu)
{
	struct sframe_section *sec = container_of(rcu, struct sframe_section, rcu);

	free_section(sec);
}

static int __sframe_remove_section(struct mm_struct *mm,
				   struct sframe_section *sec)
{
	if (!mtree_erase(&mm->sframe_mt, sec->text_start)) {
		dbg_sec("mtree_erase failed: text=%lx\n", sec->text_start);
		return -EINVAL;
	}

	call_srcu(&sframe_srcu, &sec->rcu, sframe_free_srcu);

	return 0;
}

int sframe_remove_section(unsigned long sframe_start)
{
	struct mm_struct *mm = current->mm;
	struct sframe_section *sec;
	unsigned long index = 0;
	bool found = false;
	int ret = 0;

	mt_for_each(&mm->sframe_mt, sec, index, ULONG_MAX) {
		if (sec->sframe_start == sframe_start) {
			found = true;
			ret |= __sframe_remove_section(mm, sec);
		}
	}

	if (!found || ret)
		return -EINVAL;

	return 0;
}

void sframe_free_mm(struct mm_struct *mm)
{
	struct sframe_section *sec;
	unsigned long index = 0;

	if (!mm)
		return;

	mt_for_each(&mm->sframe_mt, sec, index, ULONG_MAX)
		free_section(sec);

	mtree_destroy(&mm->sframe_mt);
}

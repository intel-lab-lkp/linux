// SPDX-License-Identifier: GPL-2.0
/*
 * Userspace eh_frame access functions
 */

#define pr_fmt(fmt)	"eh_frame: " fmt

#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/srcu.h>
#include <linux/uaccess.h>
#include <linux/mm.h>
#include <linux/string_helpers.h>
#include <linux/eh_frame.h>
#include <linux/unwind_user_types.h>

#include "eh_frame.h"

#define dbg(fmt, ...)							\
	pr_debug("%s (%d): " fmt, current->comm, current->pid, ##__VA_ARGS__)

#define UNSAFE_GET_USER_INC(to, from, end, label)			\
({									\
	typeof(to) __to;						\
	if (sizeof(__to) > end - from)					\
		return -EINVAL;						\
	unsafe_get_user(__to, (typeof(to) __user *)from, label);	\
	from += sizeof(__to);						\
	to = __to;							\
})

static __always_inline int read_uleb128(unsigned long *addr, unsigned long end,
					unsigned long *value)
{
	unsigned long cur = *addr;
	unsigned long result = 0;
	int shift = 0;
	u8 byte;

	do {
		if (shift >= BITS_PER_LONG)
			return -EINVAL;

		UNSAFE_GET_USER_INC(byte, cur, end, Efault);
		result |= (unsigned long)(byte & 0x7f) << shift;
		shift += 7;
	} while (byte & 0x80);

	*value = result;
	*addr = cur;
	return 0;

Efault:
	return -EFAULT;
}

static __always_inline int read_sleb128(unsigned long *addr, unsigned long end,
					long *value)
{
	unsigned long cur = *addr;
	long result = 0;
	int shift = 0;
	u8 byte;

	do {
		if (shift >= BITS_PER_LONG)
			return -EINVAL;

		UNSAFE_GET_USER_INC(byte, cur, end, Efault);
		result |= (long)(byte & 0x7f) << shift;
		shift += 7;
	} while (byte & 0x80);

	/* Sign extend if necessary */
	if (shift < BITS_PER_LONG && (byte & 0x40))
		result |= -(1L << shift);

	*value = result;
	*addr = cur;
	return 0;

Efault:
	return -EFAULT;
}

static __always_inline int encoded_pointer_size(u8 encoding)
{
	u8 format = DW_EH_PE_format(encoding);

	switch (format) {
	case DW_EH_PE_absptr:
		return sizeof(unsigned long);
	case DW_EH_PE_udata2:
	case DW_EH_PE_sdata2:
		return 2;
	case DW_EH_PE_udata4:
	case DW_EH_PE_sdata4:
		return 4;
	case DW_EH_PE_udata8:
	case DW_EH_PE_sdata8:
		return 8;
	case DW_EH_PE_uleb128:
	case DW_EH_PE_sleb128:
		/* Variable length */
		return 0;
	default:
		return 0;
	}
}

static __always_inline int read_encoded_pointer(struct eh_frame_section *sec,
						unsigned long *addr,
						unsigned long end,
						u8 encoding,
						unsigned long *value)
{
	unsigned long cur = *addr;
	u8 format = DW_EH_PE_format(encoding);
	u8 application = DW_EH_PE_application(encoding);
	unsigned long result;
	int ret;

	if (encoding == DW_EH_PE_omit)
		return -EINVAL;

	/* Determine base address based on application */
	switch (application) {
	case 0:
		/* Absolute */
		result = 0;
		break;
	case DW_EH_PE_pcrel:
		result = *addr;
		break;
	case DW_EH_PE_datarel:
		result = sec->eh_frame_hdr_start;
		break;
	case DW_EH_PE_textrel:
		result = sec->text_start;
		break;
	case DW_EH_PE_funcrel:
	case DW_EH_PE_aligned:
		return -EOPNOTSUPP;
	default:
		return -EINVAL;
	}

	/* Read value based on format */
	switch (format) {
	case DW_EH_PE_absptr: {
		unsigned long tmp;
		UNSAFE_GET_USER_INC(tmp, cur, end, Efault);
		result += tmp;
		break;
	}
	case DW_EH_PE_uleb128: {
		unsigned long tmp;
		ret = read_uleb128(&cur, end, &tmp);
		if (ret)
			return ret;
		result += tmp;
		break;
	}
	case DW_EH_PE_udata2: {
		u16 tmp16;
		UNSAFE_GET_USER_INC(tmp16, cur, end, Efault);
		result += tmp16;
		break;
	}
	case DW_EH_PE_udata4: {
		u32 tmp32;
		UNSAFE_GET_USER_INC(tmp32, cur, end, Efault);
		result += tmp32;
		break;
	}
	case DW_EH_PE_udata8: {
		u64 tmp64;
		UNSAFE_GET_USER_INC(tmp64, cur, end, Efault);
		result += tmp64;
		break;
	}
	case DW_EH_PE_sleb128: {
		long stmp;
		ret = read_sleb128(&cur, end, &stmp);
		if (ret)
			return ret;
		result += stmp;
		break;
	}
	case DW_EH_PE_sdata2: {
		s16 stmp16;
		UNSAFE_GET_USER_INC(stmp16, cur, end, Efault);
		result += stmp16;
		break;
	}
	case DW_EH_PE_sdata4: {
		s32 stmp32;
		UNSAFE_GET_USER_INC(stmp32, cur, end, Efault);
		result += stmp32;
		break;
	}
	case DW_EH_PE_sdata8: {
		s64 stmp64;
		UNSAFE_GET_USER_INC(stmp64, cur, end, Efault);
		result += stmp64;
		break;
	}
	default:
		return -EINVAL;
	}

	/* Indirect (dereference) - should not occur */
	if (encoding & DW_EH_PE_indirect)
		return -EOPNOTSUPP;

	*value = result;
	*addr = cur;
	return 0;

Efault:
	return -EFAULT;
}

static void free_section(struct eh_frame_section *sec)
{
	kfree(sec);
}

static int eh_frame_read_header(struct eh_frame_section *sec)
{
	struct mm_struct *mm = current->mm;
	void __user *eh_frame_hdr = (void __user *)sec->eh_frame_hdr_start;
	unsigned long cur = sec->eh_frame_hdr_start, end = sec->eh_frame_hdr_end;
	unsigned long eh_frame_start, eh_frame_vma_end, table_start, table_end;
	u8 version, eh_frame_ptr_enc, fde_count_enc, table_enc;
	unsigned long fde_count;
	int entry_size;
	int ret;

	/*
	 * Unaligned access to .eh_frame[_hdr] fields using
	 * unsafe_get_user() via UNSAFE_GET_USER_INC()
	 */
	BUILD_BUG_ON(!IS_ENABLED(CONFIG_HAVE_EFFICIENT_UNALIGNED_ACCESS));

	scoped_user_read_access_size(eh_frame_hdr, end - sec->eh_frame_hdr_start,
				     Efault) {
		/* Read version */
		UNSAFE_GET_USER_INC(version, cur, end, Efault);
		if (version != 1)
			return -EINVAL;

		/* Read encoding information */
		UNSAFE_GET_USER_INC(eh_frame_ptr_enc, cur, end, Efault);
		UNSAFE_GET_USER_INC(fde_count_enc, cur, end, Efault);
		UNSAFE_GET_USER_INC(table_enc, cur, end, Efault);

		/* .eh_frame_hdr without binary search table is not supported */
		if (fde_count_enc == DW_EH_PE_omit || table_enc == DW_EH_PE_omit)
			return -EINVAL;

		/* Read pointer to .eh_frame */
		ret = read_encoded_pointer(sec, &cur, end,
					   eh_frame_ptr_enc, &eh_frame_start);
		if (ret)
			return ret;

		/* Read FDE count */
		ret = read_encoded_pointer(sec, &cur, end,
					   fde_count_enc, &fde_count);
		if (ret)
			return ret;

		/* Determine binary search table start and end */
		table_start = cur;
		entry_size = 2 * encoded_pointer_size(table_enc);
		if (!entry_size)
			return -EINVAL;
		if (fde_count > (end - table_start) / entry_size)
			return -EINVAL;
		table_end = table_start + fde_count * entry_size;
	}

	scoped_guard(mmap_read_lock, mm) {
		struct vm_area_struct *eh_frame_vma;

		eh_frame_vma = vma_lookup(mm, eh_frame_start);
		if (!eh_frame_vma) {
			dbg("bad eh_frame address (0x%lx)\n", eh_frame_start);
			return -EINVAL;
		}
		eh_frame_vma_end = eh_frame_vma->vm_end;
	}

	sec->eh_frame_start		= eh_frame_start;
	sec->eh_frame_vma_end		= eh_frame_vma_end;
	sec->binary_search_table_start	= table_start;
	sec->binary_search_table_end	= table_end;
	sec->binary_search_table_enc	= table_enc;
	sec->fde_count			= fde_count;

	return 0;

Efault:
	return -EFAULT;
}

int eh_frame_add_section(unsigned long eh_frame_hdr_start,
			 unsigned long eh_frame_hdr_end,
			 unsigned long text_start,
			 unsigned long text_end)
{
	struct mm_struct *mm = current->mm;
	struct eh_frame_section *sec;
	int ret;

	if (eh_frame_hdr_start >= eh_frame_hdr_end || text_start >= text_end) {
		dbg("invalid eh_frame/text address\n");
		return -EINVAL;
	}

	scoped_guard(mmap_read_lock, mm) {
		struct vm_area_struct *eh_frame_hdr_vma, *text_vma;

		eh_frame_hdr_vma = vma_lookup(mm, eh_frame_hdr_start);
		if (!eh_frame_hdr_vma || eh_frame_hdr_end > eh_frame_hdr_vma->vm_end) {
			dbg("bad eh_frame_hdr address (0x%lx - 0x%lx)\n",
			    eh_frame_hdr_start, eh_frame_hdr_end);
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

	sec = kzalloc(sizeof(*sec), GFP_KERNEL_ACCOUNT);
	if (!sec)
		return -ENOMEM;

	sec->eh_frame_hdr_start	= eh_frame_hdr_start;
	sec->eh_frame_hdr_end	= eh_frame_hdr_end;
	sec->text_start		= text_start;
	sec->text_end		= text_end;

	ret = eh_frame_read_header(sec);
	if (ret)
		goto err_free;

	/* TODO nowhere to store it yet - just free it and return an error */
	ret = -ENOSYS;

err_free:
	free_section(sec);
	return ret;
}

int eh_frame_remove_section(unsigned long eh_frame_hdr_start)
{
	return -ENOSYS;
}

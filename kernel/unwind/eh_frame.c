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
#include <asm/unwind_user_eh_frame.h>

#include "eh_frame.h"
#include "eh_frame_debug.h"

/* Register state for CFI interpreter */
enum eh_frame_cfa_rule {
	CFA_UNDEFINED,		/* unrecoverable */
	CFA_REG_OFFSET,		/* CFA = reg + offset */
};

enum eh_frame_reg_rule {
	REG_UNDEFINED_IMPLICIT,	/* reg = reg */
	REG_UNDEFINED_EXPLICIT,	/* unrecoverable; RA: outermost frame */
	REG_SAME_VALUE,		/* reg = reg; TODO: reset to CIE initial CFI */
	REG_OFFSET,		/* reg = *(CFA + offset) */
	REG_VAL_OFFSET,		/* reg = CFA + offset */
	REG_REGISTER,		/* reg = other_reg */
};

enum eh_frame_reg_index {
	FP_IDX,			/* frame pointer (FP) */
	RA_IDX,			/* return address (RA) */
	NR_REGS
};

struct eh_frame_reg_state {
	/* CFA recovery rule */
	enum eh_frame_cfa_rule cfa_rule;
	unsigned long cfa_regnum;
	long cfa_offset;

	/* FP and RA recovery rules (SP uses implicit recovery) */
	enum eh_frame_reg_rule reg_rule[NR_REGS];
	unsigned long reg_regnum[NR_REGS];
	long reg_offset[NR_REGS];
};

struct eh_frame_cfi_context {
	struct eh_frame_reg_state state;
	struct eh_frame_reg_state stack[EH_FRAME_MAX_STATE_STACK];
	struct eh_frame_reg_state cie_state;
	int stack_depth;
	bool cie;
};

struct eh_frame_cie {
	unsigned long cfi_insn_start;
	unsigned long cfi_insn_end;
	int data_align;
	unsigned int code_align;
	u8 fde_addr_enc;		/* from CIE 'R' augmentation */
	bool aug_data_present;		/* from CIE 'z' augmentation */
	bool signal_frame;		/* from CIE 'S' augmentation */
};

struct eh_frame_fde {
	unsigned long func_addr;
	unsigned long func_size;
	unsigned long cfi_insn_start;
	unsigned long cfi_insn_end;

	struct eh_frame_cie cie;	/* referenced CIE*/
};

DEFINE_STATIC_SRCU(eh_frame_srcu);

#define GET_USER_INC(to, from, end)					\
({									\
	typeof(to) __to;						\
	int ret;							\
	if (from + sizeof(__to) > end)					\
		return -EINVAL;						\
	ret = get_user(__to, (typeof(to) __user *)from);		\
	from += sizeof(__to);						\
	to = __to;							\
	ret;								\
})

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

static __always_inline int __read_cie(struct eh_frame_section *sec,
				      unsigned long cie_addr,
				      struct eh_frame_cie *cie)
{
	void __user *cie_ptr = (void __user *)cie_addr;
	unsigned long cur = cie_addr, end;
	u32 length, cie_id;
	u8 version;
	char aug_str[EH_FRAME_MAX_AUGSTR_LENGTH];
	int aug_idx;
	unsigned long code_align;
	long data_align;
	u8 ra_reg;
	bool aug_data_present = false;
	unsigned long aug_data_len, aug_data_end;
	u8 fde_addr_enc = DW_EH_PE_absptr;
	bool signal_frame = false;
	int ret;

	/* Read CIE length */
	ret = GET_USER_INC(length, cur, sec->eh_frame_vma_end);
	if (ret)
		return ret;
	if (!length || length == EH_FRAME_DWARF64_LENGTH || length > EH_FRAME_MAX_CIE_LENGTH)
		return -EINVAL;
	end = cie_addr + 4 + length;
	if (end < cie_addr || end > sec->eh_frame_vma_end)
		return -EFAULT;

	scoped_user_read_access_size(cie_ptr, 4 + length, Efault) {
		/* Read CIE_ID (must be 0 for CIE; FDE otherwise) */
		UNSAFE_GET_USER_INC(cie_id, cur, end, Efault);
		if (cie_id != EH_FRAME_CIE_ID)
			return -EINVAL;

		/* Read version */
		UNSAFE_GET_USER_INC(version, cur, end, Efault);
		if (version != 1)
			return -EOPNOTSUPP;

		/* Read augmentation string */
		for (aug_idx = 0; aug_idx < sizeof(aug_str); aug_idx++) {
			UNSAFE_GET_USER_INC(aug_str[aug_idx], cur, end, Efault);
			if (aug_str[aug_idx] == '\0')
				break;
		}
		if (aug_idx >= sizeof(aug_str))
			return -EINVAL;

		/* Read code alignment factor */
		ret = read_uleb128(&cur, end, &code_align);
		if (ret)
			return ret;
		if (!code_align || code_align > EH_FRAME_MAX_CODE_ALIGN)
			return -EINVAL;

		/* Read data alignment factor */
		ret = read_sleb128(&cur, end, &data_align);
		if (ret)
			return ret;
		if (!data_align || (data_align < EH_FRAME_MIN_DATA_ALIGN ||
				    data_align > EH_FRAME_MAX_DATA_ALIGN))
			return -EINVAL;

		/* Read return address register number */
		UNSAFE_GET_USER_INC(ra_reg, cur, end, Efault);
		if (ra_reg != EH_FRAME_REG_RA)
			return -EOPNOTSUPP;

		/* Parse augmentation string and read augmentation data if present */
		aug_data_end = cur;
		for (aug_idx = 0; aug_str[aug_idx]; aug_idx++) {
			switch (aug_str[aug_idx]) {
			case 'z':
				/* Augmentation data present - must be first character */
				if (aug_idx != 0)
					return -EINVAL;
				aug_data_present = true;
				ret = read_uleb128(&cur, end, &aug_data_len);
				if (ret)
					return ret;
				aug_data_end = cur + aug_data_len;
				if (aug_data_end < cur || aug_data_end > end)
					return -EINVAL;
				break;
			case 'L': {
				/* LSDA encoding - skip */
				u8 lsda_enc;
				if (!aug_data_present)
					return -EINVAL;
				UNSAFE_GET_USER_INC(lsda_enc, cur, aug_data_end, Efault);
				break;
			}
			case 'P': {
				/* Personality encoding and routine - skip */
				u8 personality_enc;
				unsigned long personality_rtn;
				if (!aug_data_present)
					return -EINVAL;
				UNSAFE_GET_USER_INC(personality_enc, cur, aug_data_end, Efault);
				/*
				 * Clear indirect flag to avoid user read from
				 * arbitrary address; still skip field.
				 */
				personality_enc &= ~DW_EH_PE_indirect;
				ret = read_encoded_pointer(sec, &cur, aug_data_end,
							   personality_enc, &personality_rtn);
				if (ret)
					return ret;
				break;
			}
			case 'R':
				/* FDE encoding */
				if (!aug_data_present)
					return -EINVAL;
				UNSAFE_GET_USER_INC(fde_addr_enc, cur, aug_data_end, Efault);
				break;
			case 'S':
				/* Signal frame */
				signal_frame = true;
				break;
			default:
				/* Unknown augmentation */
				return -EOPNOTSUPP;
			}
		}
		if (cur != aug_data_end)
			return -EINVAL;
	}

	cie->code_align		= code_align;
	cie->data_align		= data_align;
	cie->fde_addr_enc	= fde_addr_enc;
	cie->aug_data_present	= aug_data_present;
	cie->signal_frame	= signal_frame;
	cie->cfi_insn_start	= cur;
	cie->cfi_insn_end	= end;

	return 0;

Efault:
	return -EFAULT;
}

static __always_inline int __read_fde(struct eh_frame_section *sec,
				      unsigned long fde_addr,
				      struct eh_frame_fde *fde)
{
	void __user *fde_ptr = (void __user *)fde_addr;
	unsigned long cur = fde_addr, end;
	u32 length, cie_offset;
	unsigned long cie_addr, func_addr, func_size;
	u8 range_enc;
	int ret;

	/* Read FDE length */
	ret = GET_USER_INC(length, cur, sec->eh_frame_vma_end);
	if (ret)
		return ret;
	if (!length || length == EH_FRAME_DWARF64_LENGTH || length > EH_FRAME_MAX_FDE_LENGTH)
		return -EINVAL;
	end = fde_addr + 4 + length;
	if (end < fde_addr || end > sec->eh_frame_vma_end)
		return -EFAULT;

	scoped_user_read_access_size(fde_ptr, 4 + length, Efault) {
		/* Read CIE pointer (offset from current position) */
		UNSAFE_GET_USER_INC(cie_offset, cur, end, Efault);
		cie_addr = cur - 4 - cie_offset;
		if (cie_addr + EH_FRAME_CIE_MIN_LENGTH > fde_addr)
			return -EINVAL;
		if (cie_addr < sec->eh_frame_start)
			return -EINVAL;
	}

	/* Read the CIE to populate alignment factors, RA register, and FDE encoding */
	ret = __read_cie(sec, cie_addr, &fde->cie);
	if (ret)
		return ret;

	scoped_user_read_access_size(fde_ptr, 4 + length, Efault) {
		/* Read PC begin (function start address) */
		ret = read_encoded_pointer(sec, &cur, end, fde->cie.fde_addr_enc, &func_addr);
		if (ret)
			return ret;
		if (func_addr < sec->text_start || func_addr >= sec->text_end)
			return -EINVAL;


		/* Read PC range (function size) using PE format only */
		range_enc = DW_EH_PE_format(fde->cie.fde_addr_enc);
		ret = read_encoded_pointer(sec, &cur, end, range_enc, &func_size);
		if (ret)
			return ret;
		if (func_addr + func_size < func_addr || func_addr + func_size > sec->text_end)
			return -EINVAL;

		/* Skip augmentation data if present */
		if (fde->cie.aug_data_present) {
			unsigned long aug_data_len;
			unsigned long aug_data_end;

			ret = read_uleb128(&cur, end, &aug_data_len);
			if (ret)
				return ret;
			aug_data_end = cur + aug_data_len;
			if (aug_data_end < cur || aug_data_end > end)
				return -EINVAL;
			cur = aug_data_end;
		}
	}

	fde->func_addr		= func_addr;
	fde->func_size		= func_size;
	fde->cfi_insn_start	= cur;
	fde->cfi_insn_end	= end;

	return 0;

Efault:
	return -EFAULT;
}


static __always_inline int __find_fde(struct eh_frame_section *sec,
				      unsigned long ip,
				      struct eh_frame_fde *fde)
{
	void __user *table_start_ptr;
	unsigned long table_size;
	u8 table_enc;
	int entry_size;
	unsigned long low, high;
	unsigned long found_cur = 0, found_func_addr;
	unsigned long fde_addr;
	int ret;

	if (!sec->fde_count)
		return -ENOENT;

	table_enc = sec->binary_search_table_enc;
	entry_size = 2 * encoded_pointer_size(table_enc);
	if (!entry_size)
		return -EINVAL;

	table_start_ptr = (void __user *)sec->binary_search_table_start;
	table_size = sec->binary_search_table_end - sec->binary_search_table_start;
	scoped_user_read_access_size(table_start_ptr, table_size, Efault) {
		/*
		 * Binary search in .eh_frame_hdr table using half-open
		 * interval [low, high) to avoid underflow of high if
		 * target IP is lower than first entry.
		 */
		low = 0;
		high = sec->fde_count;
		while (low < high) {
			unsigned long mid, cur, func_addr;

			mid = low + ((high - low) / 2);
			cur = sec->binary_search_table_start + mid * entry_size;

			/* Read function start address from table */
			ret = read_encoded_pointer(sec, &cur, sec->binary_search_table_end,
						   table_enc, &func_addr);
			if (ret)
				return ret;

			if (ip >= func_addr) {
				found_cur = cur;
				found_func_addr = func_addr;
				low = mid + 1;
			} else {
				high = mid;
			}
		}

		if (!found_cur)
			return -ENOENT;

		/* Read FDE address from table */
		ret = read_encoded_pointer(sec, &found_cur, sec->binary_search_table_end,
					   table_enc, &fde_addr);
		if (ret)
			return ret;
		if (fde_addr < sec->eh_frame_start)
			return -EINVAL;
	}

	ret = __read_fde(sec, fde_addr, fde);
	if (ret)
		return ret;
	if (found_func_addr != fde->func_addr)
		return -EINVAL;

	/* Make sure it is not a gap */
	if (ip < fde->func_addr || ip >= fde->func_addr + fde->func_size)
		return -ENOENT;

	return 0;

Efault:
	return -EFAULT;
}

/* Helper to convert DWARF register number to index (FP=0, RA=1) */
static inline int reg_to_index(unsigned int reg)
{
	if (reg == EH_FRAME_REG_FP)
		return 0;
	if (reg == EH_FRAME_REG_RA)
		return 1;
	return -1;
}

static __always_inline int __do_cfi_insn(struct eh_frame_section *sec,
					 struct eh_frame_fde *fde,
					 unsigned long *cur_ptr,
					 unsigned long end,
					 unsigned long *ip_ptr,
					 unsigned long target_ip,
					 struct eh_frame_cfi_context *ctx)
{
	unsigned long cur = *cur_ptr;
	unsigned long ip = *ip_ptr;
	u8 opcode;
	int ret;

	UNSAFE_GET_USER_INC(opcode, cur, end, Efault);

	switch (DW_CFA_opcode(opcode)) {
	case DW_CFA_advance_loc: {
		unsigned long offset = DW_CFA_operand(opcode) * fde->cie.code_align;

		ip += offset;
		break;
	}

	case DW_CFA_offset: {
		u8 reg = DW_CFA_operand(opcode);
		unsigned long _offset;
		long offset;
		int idx;

		ret = read_uleb128(&cur, end, &_offset);
		if (ret)
			return ret;
		if (check_mul_overflow(_offset, fde->cie.data_align, &offset))
			return -EINVAL;

		if (reg == EH_FRAME_REG_SP && eh_frame_reject_sp_rule())
			return -EOPNOTSUPP;

		idx = reg_to_index(reg);
		if (idx >= 0) {
			ctx->state.reg_rule[idx] = REG_OFFSET;
			ctx->state.reg_offset[idx] = offset;
		}
		break;
	}

	case DW_CFA_restore: {
		u8 reg = DW_CFA_operand(opcode);
		int idx;

		if (ctx->cie)
			return -EINVAL;

		idx = reg_to_index(reg);
		if (idx >= 0)
			ctx->state.reg_rule[idx] = ctx->cie_state.reg_rule[idx];
		break;
	}

	case 0: /* Extended opcodes */
		switch (opcode) {
		case DW_CFA_nop:
			break;

		case DW_CFA_advance_loc1: {
			unsigned long offset;
			u8 delta;

			UNSAFE_GET_USER_INC(delta, cur, end, Efault);
			offset = delta * fde->cie.code_align;
			ip += offset;
			break;
		}

		case DW_CFA_advance_loc2: {
			unsigned long offset;
			u16 delta;

			UNSAFE_GET_USER_INC(delta, cur, end, Efault);
			offset = delta * fde->cie.code_align;
			ip += offset;
			break;
		}

		case DW_CFA_advance_loc4: {
			unsigned long offset;
			u32 delta;

			UNSAFE_GET_USER_INC(delta, cur, end, Efault);
			offset = delta * fde->cie.code_align;
			ip += offset;
			break;
		}

		case DW_CFA_def_cfa: {
			unsigned long reg, offset;

			ret = read_uleb128(&cur, end, &reg);
			if (ret)
				return ret;
			ret = read_uleb128(&cur, end, &offset);
			if (ret)
				return ret;

			if (offset > LONG_MAX)
				return -EOPNOTSUPP;

			ctx->state.cfa_rule = CFA_REG_OFFSET;
			ctx->state.cfa_regnum = reg;
			ctx->state.cfa_offset = offset;
			break;
		}

		case DW_CFA_def_cfa_sf: {
			unsigned long reg;
			long offset;

			ret = read_uleb128(&cur, end, &reg);
			if (ret)
				return ret;
			ret = read_sleb128(&cur, end, &offset);
			if (ret)
				return ret;
			if (check_mul_overflow(offset, fde->cie.data_align, &offset))
				return -EINVAL;

			ctx->state.cfa_rule = CFA_REG_OFFSET;
			ctx->state.cfa_regnum = reg;
			ctx->state.cfa_offset = offset;
			break;
		}

		case DW_CFA_def_cfa_register: {
			unsigned long reg;

			if (ctx->state.cfa_rule != CFA_REG_OFFSET)
				return -EINVAL;

			ret = read_uleb128(&cur, end, &reg);
			if (ret)
				return ret;

			ctx->state.cfa_regnum = reg;
			break;
		}

		case DW_CFA_def_cfa_offset: {
			unsigned long offset;

			if (ctx->state.cfa_rule != CFA_REG_OFFSET)
				return -EINVAL;

			ret = read_uleb128(&cur, end, &offset);
			if (ret)
				return ret;

			if (offset > LONG_MAX)
				return -EOPNOTSUPP;

			ctx->state.cfa_offset = offset;
			break;
		}

		case DW_CFA_def_cfa_offset_sf: {
			long offset;

			if (ctx->state.cfa_rule != CFA_REG_OFFSET)
				return -EINVAL;
			ret = read_sleb128(&cur, end, &offset);
			if (ret)
				return ret;
			if (check_mul_overflow(offset, fde->cie.data_align, &offset))
				return -EINVAL;

			ctx->state.cfa_offset = offset;
			break;
		}

		case DW_CFA_restore_extended: {
			unsigned long reg;
			int idx;

			if (ctx->cie)
				return -EINVAL;

			ret = read_uleb128(&cur, end, &reg);
			if (ret)
				return ret;

			idx = reg_to_index(reg);
			if (idx >= 0)
				ctx->state.reg_rule[idx] = ctx->cie_state.reg_rule[idx];
			break;
		}

		case DW_CFA_undefined: {
			unsigned long reg;
			int idx;

			ret = read_uleb128(&cur, end, &reg);
			if (ret)
				return ret;

			if (reg == EH_FRAME_REG_SP)
				return -EOPNOTSUPP;

			idx = reg_to_index(reg);
			if (idx >= 0)
				ctx->state.reg_rule[idx] = REG_UNDEFINED_EXPLICIT;
			break;
		}

		case DW_CFA_same_value: {
			unsigned long reg;
			int idx;

			ret = read_uleb128(&cur, end, &reg);
			if (ret)
				return ret;

			if (reg == EH_FRAME_REG_SP && eh_frame_reject_sp_rule())
				return -EOPNOTSUPP;

			idx = reg_to_index(reg);
			if (idx >= 0)
				ctx->state.reg_rule[idx] = REG_SAME_VALUE;
			break;
		}

		case DW_CFA_offset_extended: {
			unsigned long reg, _offset;
			long offset;
			int idx;

			ret = read_uleb128(&cur, end, &reg);
			if (ret)
				return ret;
			ret = read_uleb128(&cur, end, &_offset);
			if (ret)
				return ret;
			if (check_mul_overflow(_offset, fde->cie.data_align, &offset))
				return -EINVAL;

			if (reg == EH_FRAME_REG_SP && eh_frame_reject_sp_rule())
				return -EOPNOTSUPP;

			idx = reg_to_index(reg);
			if (idx >= 0) {
				ctx->state.reg_rule[idx] = REG_OFFSET;
				ctx->state.reg_offset[idx] = offset;
			}
			break;
		}

		case DW_CFA_offset_extended_sf: {
			unsigned long reg;
			long offset;
			int idx;

			ret = read_uleb128(&cur, end, &reg);
			if (ret)
				return ret;
			ret = read_sleb128(&cur, end, &offset);
			if (ret)
				return ret;
			if (check_mul_overflow(offset, fde->cie.data_align, &offset))
				return -EINVAL;

			if (reg == EH_FRAME_REG_SP && eh_frame_reject_sp_rule())
				return -EOPNOTSUPP;

			idx = reg_to_index(reg);
			if (idx >= 0) {
				ctx->state.reg_rule[idx] = REG_OFFSET;
				ctx->state.reg_offset[idx] = offset;
			}
			break;
		}

		case DW_CFA_val_offset: {
			unsigned long reg, _offset;
			long offset;
			int idx;

			ret = read_uleb128(&cur, end, &reg);
			if (ret)
				return ret;
			ret = read_uleb128(&cur, end, &_offset);
			if (ret)
				return ret;
			if (check_mul_overflow(_offset, fde->cie.data_align, &offset))
				return -EINVAL;

			if (reg == EH_FRAME_REG_SP && offset != EH_FRAME_SP_VAL_OFFSET)
				return -EOPNOTSUPP;

			idx = reg_to_index(reg);
			if (idx >= 0) {
				ctx->state.reg_rule[idx] = REG_VAL_OFFSET;
				ctx->state.reg_offset[idx] = offset;
			}
			break;
		}

		case DW_CFA_val_offset_sf: {
			unsigned long reg;
			long offset;
			int idx;

			ret = read_uleb128(&cur, end, &reg);
			if (ret)
				return ret;
			ret = read_sleb128(&cur, end, &offset);
			if (ret)
				return ret;
			if (check_mul_overflow(offset, fde->cie.data_align, &offset))
				return -EINVAL;

			if (reg == EH_FRAME_REG_SP && offset != EH_FRAME_SP_VAL_OFFSET)
				return -EOPNOTSUPP;

			idx = reg_to_index(reg);
			if (idx >= 0) {
				ctx->state.reg_rule[idx] = REG_VAL_OFFSET;
				ctx->state.reg_offset[idx] = offset;
			}
			break;
		}

		case DW_CFA_register: {
			unsigned long reg1, reg2;
			int idx;

			ret = read_uleb128(&cur, end, &reg1);
			if (ret)
				return ret;
			ret = read_uleb128(&cur, end, &reg2);
			if (ret)
				return ret;

			if (reg1 == EH_FRAME_REG_SP && eh_frame_reject_sp_rule())
				return -EOPNOTSUPP;

			idx = reg_to_index(reg1);
			if (idx >= 0) {
				ctx->state.reg_rule[idx] = REG_REGISTER;
				ctx->state.reg_regnum[idx] = reg2;
			}
			break;
		}

		case DW_CFA_expression:
		case DW_CFA_val_expression: {
			unsigned long reg, expr_len;

			ret = read_uleb128(&cur, end, &reg);
			if (ret)
				return ret;
			ret = read_uleb128(&cur, end, &expr_len);
			if (ret)
				return ret;

			if (cur + expr_len > end)
				return -EINVAL;

			if (reg == EH_FRAME_REG_SP || reg == EH_FRAME_REG_FP || reg == EH_FRAME_REG_RA)
				return -EOPNOTSUPP;

			cur += expr_len;
			break;
		}

		case DW_CFA_GNU_args_size: {
			unsigned long args_size;

			ret = read_uleb128(&cur, end, &args_size);
			if (ret)
				return ret;

			/* Ignore DW_CFA_GNU_args_size */
			break;
		}

		case DW_CFA_remember_state:
			if (ctx->stack_depth >= EH_FRAME_MAX_STATE_STACK)
				return -EINVAL;
			ctx->stack[ctx->stack_depth++] = ctx->state;
			break;

		case DW_CFA_restore_state:
			if (ctx->stack_depth <= 0)
				return -EINVAL;
			ctx->state = ctx->stack[--ctx->stack_depth];
			break;

		default:
			return -EOPNOTSUPP;
		}
		break;

	default:
		return -EOPNOTSUPP;
	}

	*cur_ptr = cur;
	*ip_ptr = ip;
	return 0;

Efault:
	return -EFAULT;
}

static __always_inline int __do_cfi_program(struct eh_frame_section *sec,
					    struct eh_frame_fde *fde,
					    unsigned long target_ip,
					    struct eh_frame_cfi_context *ctx)
{
	void __user *cfi_ptr;
	unsigned long cfi_size;
	unsigned long ip = fde->func_addr;
	unsigned long cur;
	unsigned int insn_count = 0;
	int ret;

	/* Initialize state */
	ctx->state.cfa_rule = CFA_UNDEFINED;
	ctx->state.reg_rule[FP_IDX] = REG_UNDEFINED_IMPLICIT;
	ctx->state.reg_rule[RA_IDX] = REG_UNDEFINED_IMPLICIT;
	ctx->stack_depth = 0;

	/* Process CIE initial CFI instructions (if any) */
	ctx->cie = true;
	cfi_ptr = (void __user *)fde->cie.cfi_insn_start;
	cfi_size = fde->cie.cfi_insn_end - fde->cie.cfi_insn_start;
	scoped_user_read_access_size(cfi_ptr, cfi_size, Efault) {
		cur = fde->cie.cfi_insn_start;
		while (cur < fde->cie.cfi_insn_end) {
			if (insn_count++ >= EH_FRAME_CFI_INSN_LIMIT)
				return -EINVAL;
			ret = __do_cfi_insn(sec, fde, &cur, fde->cie.cfi_insn_end, &ip, target_ip, ctx);
			if (ret)
				return ret;
		}
	}

	/* Save CIE state for DW_CFA_restore[_extended] */
	ctx->cie_state = ctx->state;

	/* Do not allow remember/restore between CIE and FDE */
	ctx->stack_depth = 0;

	/* Process FDE CFI instructions up to target IP */
	ctx->cie = false;
	cfi_ptr = (void __user *)fde->cfi_insn_start;
	cfi_size = fde->cfi_insn_end - fde->cfi_insn_start;
	scoped_user_read_access_size(cfi_ptr, cfi_size, Efault) {
		cur = fde->cfi_insn_start;
		while (cur < fde->cfi_insn_end && ip <= target_ip) {
			if (insn_count++ >= EH_FRAME_CFI_INSN_LIMIT)
				return -EINVAL;
			ret = __do_cfi_insn(sec, fde, &cur, fde->cfi_insn_end, &ip, target_ip, ctx);
			if (ret)
				return ret;
		}
	}

	return 0;

Efault:
	return -EFAULT;
}

static __always_inline int __find_frame_row(struct eh_frame_section *sec,
					    struct eh_frame_fde *fde,
					    unsigned long ip,
					    struct unwind_user_frame *frame)
{
	struct eh_frame_cfi_context ctx;
	int ret;

	/* TODO: Signal frame - not supported yet */
	if (fde->cie.signal_frame)
		return -EOPNOTSUPP;

	ret = __do_cfi_program(sec, fde, ip, &ctx);
	if (ret)
		return ret;

	/* Convert CFA rule */
	if (ctx.state.cfa_rule != CFA_REG_OFFSET)
		return -EINVAL;

	if (ctx.state.cfa_regnum == EH_FRAME_REG_SP)
		frame->cfa.rule = UNWIND_USER_CFA_RULE_SP_OFFSET;
	else if (ctx.state.cfa_regnum == EH_FRAME_REG_FP)
		frame->cfa.rule = UNWIND_USER_CFA_RULE_FP_OFFSET;
	else {
		if (ctx.state.cfa_regnum > UINT_MAX)
			return -EINVAL;
		frame->cfa.rule = UNWIND_USER_CFA_RULE_REG_OFFSET;
		frame->cfa.regnum = ctx.state.cfa_regnum;
	}

	if (ctx.state.cfa_offset < INT_MIN ||
	    ctx.state.cfa_offset > INT_MAX)
		return -EOPNOTSUPP;
	frame->cfa.offset = ctx.state.cfa_offset;

	/* Convert RA rule */
	frame->outermost = false;
	switch (ctx.state.reg_rule[RA_IDX]) {
	case REG_UNDEFINED_IMPLICIT:
		frame->ra.rule = UNWIND_USER_RULE_RETAIN;
		break;
	case REG_UNDEFINED_EXPLICIT:
		frame->outermost = true;
		break;
	case REG_SAME_VALUE:
		frame->ra.rule = UNWIND_USER_RULE_RETAIN;
		break;
	case REG_OFFSET:
		if (ctx.state.reg_offset[RA_IDX] < INT_MIN ||
		    ctx.state.reg_offset[RA_IDX] > INT_MAX)
			return -EOPNOTSUPP;
		frame->ra.rule = UNWIND_USER_RULE_CFA_OFFSET_DEREF;
		frame->ra.offset = ctx.state.reg_offset[RA_IDX];
		break;
	case REG_VAL_OFFSET:
		if (ctx.state.reg_offset[RA_IDX] < INT_MIN ||
		    ctx.state.reg_offset[RA_IDX] > INT_MAX)
			return -EOPNOTSUPP;
		frame->ra.rule = UNWIND_USER_RULE_CFA_OFFSET;
		frame->ra.offset = ctx.state.reg_offset[RA_IDX];
		break;
	case REG_REGISTER:
		if (ctx.state.reg_regnum[RA_IDX] > UINT_MAX)
			return -EINVAL;
		frame->ra.rule = UNWIND_USER_RULE_REG_OFFSET;
		frame->ra.regnum = ctx.state.reg_regnum[RA_IDX];
		frame->ra.offset = 0;
		break;
	default:
		return -EINVAL;
	}

	/* Convert FP rule */
	switch (ctx.state.reg_rule[FP_IDX]) {
	case REG_UNDEFINED_IMPLICIT:
	case REG_UNDEFINED_EXPLICIT:
		frame->fp.rule = UNWIND_USER_RULE_RETAIN;
		break;
	case REG_SAME_VALUE:
		frame->fp.rule = UNWIND_USER_RULE_RETAIN;
		break;
	case REG_OFFSET:
		if (ctx.state.reg_offset[FP_IDX] < INT_MIN ||
		    ctx.state.reg_offset[FP_IDX] > INT_MAX)
			return -EOPNOTSUPP;
		frame->fp.rule = UNWIND_USER_RULE_CFA_OFFSET_DEREF;
		frame->fp.offset = ctx.state.reg_offset[FP_IDX];
		break;
	case REG_VAL_OFFSET:
		if (ctx.state.reg_offset[FP_IDX] < INT_MIN ||
		    ctx.state.reg_offset[FP_IDX] > INT_MAX)
			return -EOPNOTSUPP;
		frame->fp.rule = UNWIND_USER_RULE_CFA_OFFSET;
		frame->fp.offset = ctx.state.reg_offset[FP_IDX];
		break;
	case REG_REGISTER:
		if (ctx.state.reg_regnum[FP_IDX] > UINT_MAX)
			return -EINVAL;
		frame->fp.rule = UNWIND_USER_RULE_REG_OFFSET;
		frame->fp.regnum = ctx.state.reg_regnum[FP_IDX];
		frame->fp.offset = 0;
		break;
	default:
		return -EINVAL;
	}

	/* SP offset from CFA used in implicit CFA rule */
	frame->sp_off = EH_FRAME_SP_VAL_OFFSET;

	return 0;
}

int eh_frame_find(unsigned long ip, struct unwind_user_frame *frame)
{
	struct mm_struct *mm = current->mm;
	struct eh_frame_section *sec;
	struct eh_frame_fde fde;
	int ret;

	if (!mm)
		return -EINVAL;

	guard(srcu)(&eh_frame_srcu);

	sec = mtree_load(&mm->eh_frame_mt, ip);
	if (!sec)
		return -ENOENT;

	ret = __find_fde(sec, ip, &fde);
	if (!ret)
		ret = __find_frame_row(sec, &fde, ip, frame);

	/*
	 * Unregister .eh_frame[_hdr] in case of an error,
	 * e.g. EINVAL (corrupted) or EFAULT (inaccessible).
	 * Keep if ENOENT (not found) or EOPNOTSUPP (unsupported CFI).
	 */
	if (ret && (ret != -ENOENT && ret != -EOPNOTSUPP)) {
		dbg_sec("removing bad .eh_frame[_hdr] section\n");
		if (eh_frame_remove_section(sec->eh_frame_hdr_start))
			dbg("eh_frame_remove_section() failed\n");
	}

	return ret;
}

#ifdef CONFIG_EH_FRAME_VALIDATION

static int eh_frame_validate_section(struct eh_frame_section *sec)
{
	void __user *table_start_ptr;
	unsigned long table_size;
	u8 table_enc;
	int entry_size;
	unsigned long prev_func_addr;
	unsigned long i;

	if (!sec->fde_count) {
		dbg_sec(".eh_frame_hdr: invalid FDE count\n");
		return -EINVAL;
	}

	table_enc = sec->binary_search_table_enc;
	entry_size = 2 * encoded_pointer_size(table_enc);
	if (!entry_size) {
		dbg_sec(".eh_frame_hdr: invalid binary search table entry size\n");
		return -EINVAL;
	}
	table_start_ptr = (void __user *)sec->binary_search_table_start;
	table_size = sec->binary_search_table_end - sec->binary_search_table_start;

	for (i = 0; i < sec->fde_count; i++) {
		struct eh_frame_fde fde;
		unsigned long cur;
		unsigned long func_addr, fde_addr;
		int ret;

		cur = sec->binary_search_table_start + i * entry_size;

		scoped_user_read_access_size(table_start_ptr, table_size, Efault) {
			/* Read function start address from table */
			ret = read_encoded_pointer(sec, &cur,
						   sec->binary_search_table_end,
						   table_enc, &func_addr);
			if (ret) {
				dbg_sec_ehfh(cur, "table[%lu]: failed to read function start address\n", i);
				return ret;
			}
			if (i && func_addr <= prev_func_addr) {
				dbg_sec(".eh_frame_hdr: table[%lu]: not sorted\n", i);
				return -EINVAL;
			}
			prev_func_addr = func_addr;

			/* Read FDE address from table */
			ret = read_encoded_pointer(sec, &cur,
						   sec->binary_search_table_end,
						   table_enc, &fde_addr);
			if (ret) {
				dbg_sec_ehfh(cur, "table[%lu]: failed to read FDE pointer\n", i);
				return ret;
			}
			if (fde_addr < sec->eh_frame_start) {
				dbg_sec(".eh_frame_hdr: table[%lu]: invalid FDE address\n", i);
				return -EINVAL;
			}
		}

		ret = __read_fde(sec, fde_addr, &fde);
		if (ret) {
			dbg_sec(".eh_frame_hdr: table[%lu]: failed to read FDE at .eh_frame+%#lx\n",
				i, fde_addr - sec->eh_frame_start);
			return ret;
		}
		if (func_addr != fde.func_addr) {
			dbg_sec(".eh_frame_hdr: table[%lu]: function start address mismatch\n", i);
			return -EINVAL;
		}
	}

	return 0;

Efault:
	return -EFAULT;
}

#else /* !CONFIG_EH_FRAME_VALIDATION */

static int eh_frame_validate_section(struct eh_frame_section *sec) { return 0; }

#endif /* !CONFIG_EH_FRAME_VALIDATION */

static void free_section(struct eh_frame_section *sec)
{
	dbg_free(sec);
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
	struct maple_tree *eh_frame_mt = &current->mm->eh_frame_mt;
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

	dbg_init(sec);

	ret = eh_frame_read_header(sec);
	if (ret)
		goto err_free;

	ret = eh_frame_validate_section(sec);
	if (ret)
		goto err_free;

	ret = mtree_insert_range(eh_frame_mt, sec->text_start, sec->text_end - 1,
				 sec, GFP_KERNEL_ACCOUNT);
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

static void eh_frame_free_srcu(struct rcu_head *rcu)
{
	struct eh_frame_section *sec = container_of(rcu, struct eh_frame_section, rcu);

	free_section(sec);
}

static int __eh_frame_remove_section(struct ma_state *mas,
				     struct eh_frame_section *sec)
{
	if (mas_erase(mas) != sec) {
		dbg_sec("mas_erase failed: text=%lx\n", sec->text_start);
		return -EINVAL;
	}

	call_srcu(&eh_frame_srcu, &sec->rcu, eh_frame_free_srcu);

	return 0;
}

int eh_frame_remove_section(unsigned long eh_frame_hdr_start)
{
	struct mm_struct *mm = current->mm;
	struct eh_frame_section *sec;
	MA_STATE(mas, &mm->eh_frame_mt, 0, 0);
	bool found = false;
	int ret = 0;

	guard(srcu)(&eh_frame_srcu);

	mtree_lock(&mm->eh_frame_mt);
	mas_for_each(&mas, sec, ULONG_MAX) {
		if (sec->eh_frame_hdr_start == eh_frame_hdr_start) {
			found = true;
			ret |= __eh_frame_remove_section(&mas, sec);
		}
	}
	mtree_unlock(&mm->eh_frame_mt);

	if (!found || ret)
		return -EINVAL;

	return 0;
}

static void __eh_frame_dup_section(struct eh_frame_section *sec,
				   struct eh_frame_section *oldsec)
{
	sec->eh_frame_hdr_start	= oldsec->eh_frame_hdr_start;
	sec->eh_frame_hdr_end	= oldsec->eh_frame_hdr_end;
	sec->text_start		= oldsec->text_start;
	sec->text_end		= oldsec->text_end;

	sec->eh_frame_start		= oldsec->eh_frame_start;
	sec->eh_frame_vma_end		= oldsec->eh_frame_vma_end;
	sec->binary_search_table_start	= oldsec->binary_search_table_start;
	sec->binary_search_table_end	= oldsec->binary_search_table_end;
	sec->fde_count			= oldsec->fde_count;
	sec->binary_search_table_enc	= oldsec->binary_search_table_enc;

	dbg_dup(sec, oldsec);
}

int eh_frame_dup_mm(struct mm_struct *mm, struct mm_struct *oldmm)
{
	struct eh_frame_section *sec, *oldsec;
	unsigned long index = 0;
	int ret;

	guard(srcu)(&eh_frame_srcu);

	mt_for_each(&oldmm->eh_frame_mt, oldsec, index, ULONG_MAX) {
		sec = kzalloc(sizeof(*sec), GFP_KERNEL_ACCOUNT);
		if (!sec)
			return -ENOMEM;

		__eh_frame_dup_section(sec, oldsec);

		ret = mtree_insert_range(&mm->eh_frame_mt,
					 sec->text_start,
					 sec->text_end - 1,
					 sec, GFP_KERNEL_ACCOUNT);
		if (ret)
			goto err_free;
	}

	return 0;

err_free:
	free_section(sec);
	return ret;
}

void eh_frame_free_mm(struct mm_struct *mm)
{
	struct eh_frame_section *sec;
	unsigned long index = 0;

	if (!mm)
		return;

	mt_for_each(&mm->eh_frame_mt, sec, index, ULONG_MAX)
		free_section(sec);

	mtree_destroy(&mm->eh_frame_mt);
}

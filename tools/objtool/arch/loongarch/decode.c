// SPDX-License-Identifier: GPL-2.0-or-later
#include <string.h>
#include <objtool/check.h>
#include <objtool/disas.h>
#include <objtool/warn.h>
#include <asm/inst.h>
#include <asm/orc_types.h>
#include <linux/objtool_types.h>
#include <arch/elf.h>

const char *arch_reg_name[CFI_NUM_REGS] = {
	"zero", "ra", "tp", "sp",
	"a0", "a1", "a2", "a3",
	"a4", "a5", "a6", "a7",
	"t0", "t1", "t2", "t3",
	"t4", "t5", "t6", "t7",
	"t8", "u0", "fp", "s0",
	"s1", "s2", "s3", "s4",
	"s5", "s6", "s7", "s8"
};

int arch_ftrace_match(const char *name)
{
	return !strcmp(name, "_mcount");
}

unsigned long arch_jump_destination(struct instruction *insn)
{
	return insn->offset + (insn->immediate << 2);
}

s64 arch_insn_adjusted_addend(struct instruction *insn, struct reloc *reloc)
{
	return reloc_addend(reloc);
}

u64 arch_adjusted_addend(struct reloc *reloc)
{
	return reloc_addend(reloc);
}

/*
 * A cross-section difference like the "key - ." field of a __jump_table
 * entry may come out as a paired R_LARCH_ADD plus R_LARCH_SUB relocation
 * at the same offset: clang's integrated assembler does this when the
 * symbol is not defined in the same translation unit (GAS, and clang for
 * locally-defined symbols, emit a single PCREL instead).  objtool allows
 * only one relocation per offset, so the pair breaks cloning.
 *
 * When the SUB half points at the reloc's own position ("sym - ."), the
 * pair means the same as a single PC-relative relocation, which the
 * module loader also supports: rewrite the ADD half to R_LARCH_*_PCREL
 * and tell the caller to skip the SUB half.
 *
 * Return 1 to skip the reloc, 0 to proceed, -1 on error.
 */
int arch_normalize_paired_reloc(struct elf *elf, struct reloc *reloc)
{
	struct section *rsec = reloc->sec;
	unsigned int sub_type, pcrel_type;
	struct reloc *sub, *add;

	switch (reloc_type(reloc)) {
	case R_LARCH_ADD32:
		sub_type = R_LARCH_SUB32;
		pcrel_type = R_LARCH_32_PCREL;
		break;
	case R_LARCH_ADD64:
		sub_type = R_LARCH_SUB64;
		pcrel_type = R_LARCH_64_PCREL;
		break;
	case R_LARCH_SUB32:
	case R_LARCH_SUB64:
		/*
		 * Skip only if the paired ADD (the preceding reloc) has
		 * been rewritten to PCREL; otherwise leave the pair
		 * intact so a failed conversion stays loud.
		 */
		add = reloc_idx(reloc) ? reloc - 1 : NULL;
		if (add && reloc_offset(add) == reloc_offset(reloc) &&
		    (reloc_type(add) == R_LARCH_32_PCREL ||
		     reloc_type(add) == R_LARCH_64_PCREL))
			return 1;
		return 0;
	default:
		return 0;
	}

	/* The paired SUB reloc immediately follows the ADD */
	sub = rsec_next_reloc(rsec, reloc);
	if (!sub || reloc_offset(sub) != reloc_offset(reloc) ||
	    reloc_type(sub) != sub_type)
		return 0;

	/* Only a "sym - ." difference is PC-relative */
	if (sub->sym->sec != rsec->base ||
	    sub->sym->offset + reloc_addend(sub) != reloc_offset(sub))
		return 0;

	set_reloc_type(elf, reloc, pcrel_type);
	return 0;
}

static bool klp_read_insn(struct section *sec, unsigned long offset,
			  union loongarch_instruction *insn)
{
	if (offset + sizeof(*insn) > sec_size(sec))
		return false;

	memcpy(insn, sec->data->d_buf + offset, sizeof(*insn));
	return true;
}

/*
 * A livepatch module is mapped far more than the +-2GB reachable by
 * pcalau12i/addi.d, so a klp reloc which resolves to a symbol in vmlinux cannot
 * be reached PC-relatively: the relocation overflows and the patched function
 * computes a wild pointer.  Far calls are already handled by the module loader,
 * which redirects them through a PLT stub, but there is no such indirection for
 * data: the address is materialized inline.
 *
 * Compilers emit the direct PC-relative form for any symbol they consider local
 * (in particular file-local 'static' data, which is never interposable, so
 * -fPIC does not route it through the GOT).  This is correct for vmlinux and
 * for ordinary modules; it only breaks once klp-diff extracts the function into
 * a far away livepatch module while its data stays behind.
 *
 * Convert the reference to its GOT-indirect equivalent, which loads the full
 * 64-bit address from a GOT slot the module loader fills in and so works at any
 * distance:
 *
 *	pcalau12i rd, %pc_hi20(sym)	->  pcalau12i rd, %got_pc_hi20(sym)
 *	addi.d    rd, rj, %pc_lo12(sym)	->  ld.d      rd, rj, %got_pc_lo12(sym)
 *
 * Only an adjacent pcalau12i/addi.d pair which materializes an address can be
 * converted, and only that exact shape is accepted:
 *
 *  - A PCALA_LO12 on anything but addi.d is a load or store reading the symbol
 *    directly.  Going through the GOT would need an extra instruction to
 *    dereference the slot, and the function cannot grow without shifting every
 *    offset, relocation and ORC entry.
 *
 *  - The pair must be adjacent and register-consistent.  If one pcalau12i is
 *    shared by several addi.d with different addends, each needs its own GOT
 *    slot, but the single GOT_PC_HI20 only spans one page: the result would be
 *    correct only if those slots happened to share a page.  Requiring
 *    adjacency rejects that (the second addi.d is not preceded by a pcalau12i),
 *    as well as a pair the compiler scheduled apart.
 *
 * Bail out on anything else instead of emitting a livepatch which is quietly
 * broken.
 */
int arch_klp_convert_reloc_to_got(struct elf *elf, struct section *sec,
				  unsigned long offset, unsigned int *type)
{
	union loongarch_instruction hi, lo;
	unsigned long hi_offset, lo_offset;

	switch (*type) {
	case R_LARCH_PCALA_HI20:
		hi_offset = offset;
		lo_offset = offset + LOONGARCH_INSN_SIZE;
		break;

	case R_LARCH_PCALA_LO12:
		if (offset < LOONGARCH_INSN_SIZE) {
			ERROR("%s+0x%lx: PCALA_LO12 with no preceding instruction",
			      sec->name, offset);
			return -1;
		}
		hi_offset = offset - LOONGARCH_INSN_SIZE;
		lo_offset = offset;
		break;

	default:
		return 0;
	}

	if (!klp_read_insn(sec, hi_offset, &hi) ||
	    !klp_read_insn(sec, lo_offset, &lo)) {
		ERROR("%s+0x%lx: PCALA pair runs past end of section",
		      sec->name, offset);
		return -1;
	}

	if (hi.reg1i20_format.opcode != pcalau12i_op ||
	    lo.reg2i12_format.opcode != addid_op ||
	    lo.reg2i12_format.rj != hi.reg1i20_format.rd) {
		ERROR("%s+0x%lx: cannot make klp reference GOT-indirect: expected an adjacent pcalau12i/addi.d pair, found 0x%08x/0x%08x",
		      sec->name, offset, hi.word, lo.word);
		return -1;
	}

	if (*type == R_LARCH_PCALA_HI20) {
		*type = R_LARCH_GOT_PC_HI20;
		return 0;
	}

	/* addi.d rd, rj, %pc_lo12(sym) -> ld.d rd, rj, %got_pc_lo12(sym) */
	lo.reg2i12_format.opcode = ldd_op;
	if (elf_write_insn(elf, sec, lo_offset, sizeof(lo), (const char *)&lo))
		return -1;

	*type = R_LARCH_GOT_PC_LO12;
	return 0;
}

bool arch_pc_relative_reloc(struct reloc *reloc)
{
	return false;
}

bool arch_callee_saved_reg(unsigned char reg)
{
	switch (reg) {
	case CFI_RA:
	case CFI_FP:
	case CFI_S0 ... CFI_S8:
		return true;
	default:
		return false;
	}
}

int arch_decode_hint_reg(u8 sp_reg, int *base)
{
	switch (sp_reg) {
	case ORC_REG_UNDEFINED:
		*base = CFI_UNDEFINED;
		break;
	case ORC_REG_SP:
		*base = CFI_SP;
		break;
	case ORC_REG_FP:
		*base = CFI_FP;
		break;
	default:
		return -1;
	}

	return 0;
}

static bool is_loongarch(const struct elf *elf)
{
	if (elf->ehdr.e_machine == EM_LOONGARCH)
		return true;

	ERROR("unexpected ELF machine type %d", elf->ehdr.e_machine);
	return false;
}

#define ADD_OP(op) \
	if (!(op = calloc(1, sizeof(*op)))) \
		return -1; \
	else for (*ops_list = op, ops_list = &op->next; op; op = NULL)

static bool decode_insn_reg0i26_fomat(union loongarch_instruction inst,
				      struct instruction *insn)
{
	switch (inst.reg0i26_format.opcode) {
	case b_op:
		insn->type = INSN_JUMP_UNCONDITIONAL;
		insn->immediate = sign_extend64(inst.reg0i26_format.immediate_h << 16 |
						inst.reg0i26_format.immediate_l, 25);
		break;
	case bl_op:
		insn->type = INSN_CALL;
		insn->immediate = sign_extend64(inst.reg0i26_format.immediate_h << 16 |
						inst.reg0i26_format.immediate_l, 25);
		break;
	default:
		return false;
	}

	return true;
}

static bool decode_insn_reg1i21_fomat(union loongarch_instruction inst,
				      struct instruction *insn)
{
	switch (inst.reg1i21_format.opcode) {
	case beqz_op:
	case bnez_op:
	case bceqz_op:
		insn->type = INSN_JUMP_CONDITIONAL;
		insn->immediate = sign_extend64(inst.reg1i21_format.immediate_h << 16 |
						inst.reg1i21_format.immediate_l, 20);
		break;
	default:
		return false;
	}

	return true;
}

static bool decode_insn_reg2i12_fomat(union loongarch_instruction inst,
				      struct instruction *insn,
				      struct stack_op **ops_list,
				      struct stack_op *op)
{
	switch (inst.reg2i12_format.opcode) {
	case addid_op:
		if ((inst.reg2i12_format.rd == CFI_SP) || (inst.reg2i12_format.rj == CFI_SP)) {
			/* addi.d sp,sp,si12 or addi.d fp,sp,si12 or addi.d sp,fp,si12 */
			insn->immediate = sign_extend64(inst.reg2i12_format.immediate, 11);
			ADD_OP(op) {
				op->src.type = OP_SRC_ADD;
				op->src.reg = inst.reg2i12_format.rj;
				op->src.offset = insn->immediate;
				op->dest.type = OP_DEST_REG;
				op->dest.reg = inst.reg2i12_format.rd;
			}
		}
		if ((inst.reg2i12_format.rd == CFI_SP) && (inst.reg2i12_format.rj == CFI_FP)) {
			/* addi.d sp,fp,si12 */
			struct symbol *func = find_func_containing(insn->sec, insn->offset);

			if (!func)
				return false;

			func->frame_pointer = true;
		}
		break;
	case ldd_op:
		if (inst.reg2i12_format.rj == CFI_SP) {
			/* ld.d rd,sp,si12 */
			insn->immediate = sign_extend64(inst.reg2i12_format.immediate, 11);
			ADD_OP(op) {
				op->src.type = OP_SRC_REG_INDIRECT;
				op->src.reg = CFI_SP;
				op->src.offset = insn->immediate;
				op->dest.type = OP_DEST_REG;
				op->dest.reg = inst.reg2i12_format.rd;
			}
		}
		break;
	case std_op:
		if (inst.reg2i12_format.rj == CFI_SP) {
			/* st.d rd,sp,si12 */
			insn->immediate = sign_extend64(inst.reg2i12_format.immediate, 11);
			ADD_OP(op) {
				op->src.type = OP_SRC_REG;
				op->src.reg = inst.reg2i12_format.rd;
				op->dest.type = OP_DEST_REG_INDIRECT;
				op->dest.reg = CFI_SP;
				op->dest.offset = insn->immediate;
			}
		}
		break;
	case andi_op:
		if (inst.reg2i12_format.rd == 0 &&
		    inst.reg2i12_format.rj == 0 &&
		    inst.reg2i12_format.immediate == 0)
			/* andi r0,r0,0 */
			insn->type = INSN_NOP;
		break;
	default:
		return false;
	}

	return true;
}

static bool decode_insn_reg2i14_fomat(union loongarch_instruction inst,
				      struct instruction *insn,
				      struct stack_op **ops_list,
				      struct stack_op *op)
{
	switch (inst.reg2i14_format.opcode) {
	case ldptrd_op:
		if (inst.reg2i14_format.rj == CFI_SP) {
			/* ldptr.d rd,sp,si14 */
			insn->immediate = sign_extend64(inst.reg2i14_format.immediate, 13);
			ADD_OP(op) {
				op->src.type = OP_SRC_REG_INDIRECT;
				op->src.reg = CFI_SP;
				op->src.offset = insn->immediate;
				op->dest.type = OP_DEST_REG;
				op->dest.reg = inst.reg2i14_format.rd;
			}
		}
		break;
	case stptrd_op:
		if (inst.reg2i14_format.rj == CFI_SP) {
			/* stptr.d ra,sp,0 */
			if (inst.reg2i14_format.rd == LOONGARCH_GPR_RA &&
			    inst.reg2i14_format.immediate == 0)
				break;

			/* stptr.d rd,sp,si14 */
			insn->immediate = sign_extend64(inst.reg2i14_format.immediate, 13);
			ADD_OP(op) {
				op->src.type = OP_SRC_REG;
				op->src.reg = inst.reg2i14_format.rd;
				op->dest.type = OP_DEST_REG_INDIRECT;
				op->dest.reg = CFI_SP;
				op->dest.offset = insn->immediate;
			}
		}
		break;
	default:
		return false;
	}

	return true;
}

static bool decode_insn_reg2i16_fomat(union loongarch_instruction inst,
				      struct instruction *insn)
{
	switch (inst.reg2i16_format.opcode) {
	case jirl_op:
		if (inst.reg2i16_format.rd == 0 &&
		    inst.reg2i16_format.rj == CFI_RA &&
		    inst.reg2i16_format.immediate == 0) {
			/* jirl r0,ra,0 */
			insn->type = INSN_RETURN;
		} else if (inst.reg2i16_format.rd == CFI_RA) {
			/* jirl ra,rj,offs16 */
			insn->type = INSN_CALL_DYNAMIC;
		} else if (inst.reg2i16_format.rd == CFI_A0 &&
			   inst.reg2i16_format.immediate == 0) {
			/*
			 * jirl a0,t0,0
			 * this is a special case in loongarch_suspend_enter,
			 * just treat it as a call instruction.
			 */
			insn->type = INSN_CALL_DYNAMIC;
		} else if (inst.reg2i16_format.rd == 0 &&
			   inst.reg2i16_format.immediate == 0) {
			/* jirl r0,rj,0 */
			insn->type = INSN_JUMP_DYNAMIC;
		} else if (inst.reg2i16_format.rd == 0 &&
			   inst.reg2i16_format.immediate != 0) {
			/*
			 * jirl r0,t0,12
			 * this is a rare case in JUMP_VIRT_ADDR,
			 * just ignore it due to it is harmless for tracing.
			 */
			break;
		} else {
			/* jirl rd,rj,offs16 */
			insn->type = INSN_JUMP_UNCONDITIONAL;
			insn->immediate = sign_extend64(inst.reg2i16_format.immediate, 15);
		}
		break;
	case beq_op:
	case bne_op:
	case blt_op:
	case bge_op:
	case bltu_op:
	case bgeu_op:
		insn->type = INSN_JUMP_CONDITIONAL;
		insn->immediate = sign_extend64(inst.reg2i16_format.immediate, 15);
		break;
	default:
		return false;
	}

	return true;
}

static bool decode_insn_reg3_fomat(union loongarch_instruction inst,
				   struct instruction *insn)
{
	switch (inst.reg3_format.opcode) {
	case amswapw_op:
		if (inst.reg3_format.rd == LOONGARCH_GPR_ZERO &&
		    inst.reg3_format.rk == LOONGARCH_GPR_RA &&
		    inst.reg3_format.rj == LOONGARCH_GPR_ZERO) {
			/* amswap.w $zero, $ra, $zero */
			insn->type = INSN_BUG;
		}
		break;
	default:
		return false;
	}

	return true;
}

int arch_decode_instruction(struct objtool_file *file, const struct section *sec,
			    unsigned long offset, unsigned int maxlen,
			    struct instruction *insn)
{
	struct stack_op **ops_list = &insn->stack_ops;
	const struct elf *elf = file->elf;
	struct stack_op *op = NULL;
	union loongarch_instruction inst;

	if (!is_loongarch(elf))
		return -1;

	if (maxlen < LOONGARCH_INSN_SIZE)
		return 0;

	insn->len = LOONGARCH_INSN_SIZE;
	insn->type = INSN_OTHER;
	insn->immediate = 0;

	inst = *(union loongarch_instruction *)(sec->data->d_buf + offset);

	if (decode_insn_reg0i26_fomat(inst, insn))
		return 0;
	if (decode_insn_reg1i21_fomat(inst, insn))
		return 0;
	if (decode_insn_reg2i12_fomat(inst, insn, ops_list, op))
		return 0;
	if (decode_insn_reg2i14_fomat(inst, insn, ops_list, op))
		return 0;
	if (decode_insn_reg2i16_fomat(inst, insn))
		return 0;
	if (decode_insn_reg3_fomat(inst, insn))
		return 0;

	if (inst.word == 0) {
		/* andi $zero, $zero, 0x0 */
		insn->type = INSN_NOP;
	} else if (inst.reg0i15_format.opcode == break_op &&
		   inst.reg0i15_format.immediate == 0x0) {
		/* break 0x0 */
		insn->type = INSN_TRAP;
	} else if (inst.reg0i15_format.opcode == break_op &&
		   inst.reg0i15_format.immediate == 0x1) {
		/* break 0x1 */
		insn->type = INSN_BUG;
	} else if (inst.reg2_format.opcode == ertn_op) {
		/* ertn */
		insn->type = INSN_RETURN;
	}

	return 0;
}

const char *arch_nop_insn(int len)
{
	static u32 nop;

	if (len != LOONGARCH_INSN_SIZE) {
		ERROR("invalid NOP size: %d\n", len);
		return NULL;
	}

	nop = LOONGARCH_INSN_NOP;

	return (const char *)&nop;
}

const char *arch_ret_insn(int len)
{
	static u32 ret;

	if (len != LOONGARCH_INSN_SIZE) {
		ERROR("invalid RET size: %d\n", len);
		return NULL;
	}

	emit_jirl((union loongarch_instruction *)&ret, LOONGARCH_GPR_RA, LOONGARCH_GPR_ZERO, 0);

	return (const char *)&ret;
}

void arch_initial_func_cfi_state(struct cfi_init_state *state)
{
	int i;

	for (i = 0; i < CFI_NUM_REGS; i++) {
		state->regs[i].base = CFI_UNDEFINED;
		state->regs[i].offset = 0;
	}

	/* initial CFA (call frame address) */
	state->cfa.base = CFI_SP;
	state->cfa.offset = 0;
}

unsigned int arch_reloc_size(struct reloc *reloc)
{
	switch (reloc_type(reloc)) {
	case R_LARCH_32:
	case R_LARCH_32_PCREL:
		return 4;
	default:
		return 8;
	}
}

unsigned long arch_jump_table_sym_offset(struct reloc *reloc, struct reloc *table)
{
	switch (reloc_type(reloc)) {
	case R_LARCH_32_PCREL:
	case R_LARCH_64_PCREL:
		return reloc->sym->offset + reloc_addend(reloc) -
		       (reloc_offset(reloc) - reloc_offset(table));
	default:
		return reloc->sym->offset + reloc_addend(reloc);
	}
}

size_t arch_jump_opcode_bytes(struct objtool_file *file, struct instruction *insn,
			      unsigned char *buf)
{
	union loongarch_instruction *code;
	u32 insn_word;

	insn_word = le32toh(*(u32 *)(insn->sec->data->d_buf + insn->offset));
	code = (union loongarch_instruction *)&insn_word;

	switch (code->reg0i26_format.opcode) {
	case b_op:
	case bl_op:
		/* reg0i26: 26-bit offset, no register operands */
		insn_word &= 0xfc000000;
		break;
	case beqz_op:
	case bnez_op:
	case bceqz_op:		/* == bcnez_op */
		/* reg1i21: keep opcode + rj/cj at bits[9:5] */
		insn_word &= 0xfc0003e0;
		break;
	case jirl_op:
	case beq_op:
	case bne_op:
	case blt_op:
	case bge_op:
	case bltu_op:
	case bgeu_op:
		/* reg2i16: keep opcode + rj/rd at bits[9:0] */
		insn_word &= 0xfc0003ff;
		break;
	default:
		break;
	}

	insn_word = htole32(insn_word);
	memcpy(buf, &insn_word, sizeof(insn_word));

	return LOONGARCH_INSN_SIZE;
}

#ifdef DISAS

int arch_disas_info_init(struct disassemble_info *dinfo)
{
	return disas_info_init(dinfo, bfd_arch_loongarch,
			       bfd_mach_loongarch32, bfd_mach_loongarch64,
			       NULL);
}

#endif /* DISAS */

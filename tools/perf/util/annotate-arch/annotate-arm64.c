// SPDX-License-Identifier: GPL-2.0
#include <linux/bitops.h>
#include <linux/compiler.h>
#include <linux/kernel.h>
#include <errno.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <linux/zalloc.h>
#include <regex.h>
#include "../annotate.h"
#include "../debug.h"
#include "../disasm.h"
#ifdef HAVE_LIBDW_SUPPORT
#include "../annotate-data.h"
#include "../map.h"
#include "../symbol.h"
#endif

struct arch_arm64 {
	struct arch arch;
	regex_t call_insn;
	regex_t jump_insn;
};

/*
 * ARM64 instruction encoding masks and values.
 * Derived from ARM Architecture Reference Manual, C4.1 A64 encoding index.
 *
 * These mirror the definitions in arch/arm64/include/asm/insn.h but are
 * duplicated here because that header depends on kernel-only macros
 * (BUILD_BUG_ON, __always_inline).
 */

/* GP Load/Store: bit[27]=1, bit[26]=0 (GP, not SIMD/FP), bit[25]=0 */
#define A64_INSN_GP_LS_MASK	0x0e000000
#define A64_INSN_GP_LS_VAL	0x08000000

/* LDR/LDRSW (literal): bits[29:27]=011, bit[26]=0 -- must be excluded from GP LS */
#define A64_INSN_LDR_LIT_MASK	0x3b000000
#define A64_INSN_LDR_LIT_VAL	0x18000000

/*
 * Load/Store register (register offset):
 * bits[29:27]=111, bits[25:24]=00, bit[21]=1, bits[11:10]=10
 */
#define A64_INSN_LS_REG_OFF_MASK	0x3b200c00
#define A64_INSN_LS_REG_OFF_VAL	0x38200800

/* ADRP: mask=0x9F000000, val=0x90000000 */
#define A64_INSN_ADRP_MASK	0x9f000000
#define A64_INSN_ADRP_VAL	0x90000000

/* ADD (immediate): mask=0x7F800000, val=0x11000000 (excludes ADDG/SUBG) */
#define A64_INSN_ADD_IMM_MASK	0x7f800000
#define A64_INSN_ADD_IMM_VAL	0x11000000

/* MOV (register) = ORR Xd/Wd, XZR/WZR, Xm/Wm: Rn=11111, imm6=000000 */
#define A64_INSN_MOV_REG_MASK	0x7fe0ffe0
#define A64_INSN_MOV_REG_VAL	0x2a0003e0

/* Instruction field extraction */
#define A64_RT(insn)	((insn) & 0x1f)
#define A64_RN(insn)	(((insn) >> 5) & 0x1f)
#define A64_RM(insn)	(((insn) >> 16) & 0x1f)

static int arm64_mov__parse(const struct arch *arch __maybe_unused,
			    struct ins_operands *ops,
			    struct map_symbol *ms __maybe_unused,
			    struct disasm_line *dl __maybe_unused)
{
	char *s = strchr(ops->raw, ','), *target, *endptr;

	if (s == NULL)
		return -1;

	*s = '\0';
	ops->source.raw = strdup(ops->raw);
	*s = ',';

	if (ops->source.raw == NULL)
		return -1;

	target = ++s;
	ops->target.raw = strdup(target);
	if (ops->target.raw == NULL)
		goto out_free_source;

	ops->target.addr = strtoull(target, &endptr, 16);
	if (endptr == target)
		goto out_free_target;

	s = strchr(endptr, '<');
	if (s == NULL)
		goto out_free_target;
	endptr = strchr(s + 1, '>');
	if (endptr == NULL)
		goto out_free_target;

	*endptr = '\0';
	*s = ' ';
	ops->target.name = strdup(s);
	*s = '<';
	*endptr = '>';
	if (ops->target.name == NULL)
		goto out_free_target;

	return 0;

out_free_target:
	zfree(&ops->target.raw);
out_free_source:
	zfree(&ops->source.raw);
	return -1;
}

static const struct ins_ops arm64_mov_ops = {
	.parse	   = arm64_mov__parse,
	.scnprintf = mov__scnprintf,
};

/*
 * ARM64 load/store instruction parser.
 * Sets mem_ref and multi_regs based on raw instruction encoding.
 */
static int arm64_load_store__parse(const struct arch *arch __maybe_unused,
				   struct ins_operands *ops,
				   struct map_symbol *ms __maybe_unused,
				   struct disasm_line *dl)
{
	u32 insn = dl->raw.raw_insn;

	ops->source.mem_ref = true;
	ops->source.multi_regs = false;

	/* Load/Store register (register offset) uses Rm as second source */
	if ((insn & A64_INSN_LS_REG_OFF_MASK) == A64_INSN_LS_REG_OFF_VAL)
		ops->source.multi_regs = true;

	ops->target.mem_ref = false;
	ops->target.multi_regs = false;

	return 0;
}

static int arm64_load_store__scnprintf(const struct ins *ins, char *bf,
				       size_t size,
				       struct ins_operands *ops,
				       int max_ins_name)
{
	return scnprintf(bf, size, "%-*s %s", max_ins_name, ins->name,
			 ops->raw);
}

static const struct ins_ops arm64_load_store_ops = {
	.parse     = arm64_load_store__parse,
	.scnprintf = arm64_load_store__scnprintf,
};

static int arm64_arithmetic__parse(const struct arch *arch __maybe_unused,
				   struct ins_operands *ops,
				   struct map_symbol *ms __maybe_unused,
				   struct disasm_line *dl __maybe_unused)
{
	ops->source.mem_ref = false;
	ops->source.multi_regs = false;
	ops->target.mem_ref = false;
	ops->target.multi_regs = false;

	return 0;
}

static int arm64_arithmetic__scnprintf(const struct ins *ins, char *bf,
				       size_t size,
				       struct ins_operands *ops,
				       int max_ins_name)
{
	return scnprintf(bf, size, "%-*s %s", max_ins_name, ins->name,
			 ops->raw);
}

static const struct ins_ops arm64_arithmetic_ops = {
	.parse     = arm64_arithmetic__parse,
	.scnprintf = arm64_arithmetic__scnprintf,
};

/*
 * Classify ARM64 instructions by raw encoding for data type profiling.
 */
const struct ins_ops *check_arm64_insn(struct disasm_line *dl)
{
	u32 insn = dl->raw.raw_insn;

	/* Exclude LDR/LDRSW (literal) before matching GP Load/Store */
	if ((insn & A64_INSN_LDR_LIT_MASK) == A64_INSN_LDR_LIT_VAL)
		return NULL;

	if ((insn & A64_INSN_GP_LS_MASK) == A64_INSN_GP_LS_VAL)
		return &arm64_load_store_ops;

	if ((insn & A64_INSN_MOV_REG_MASK) == A64_INSN_MOV_REG_VAL)
		return &arm64_arithmetic_ops;

	if ((insn & A64_INSN_ADRP_MASK) == A64_INSN_ADRP_VAL)
		return &arm64_arithmetic_ops;

	if ((insn & A64_INSN_ADD_IMM_MASK) == A64_INSN_ADD_IMM_VAL)
		return &arm64_arithmetic_ops;

	return NULL;
}

#ifdef HAVE_LIBDW_SUPPORT

static inline bool arm64_is_adrp(u32 insn)
{
	return (insn & A64_INSN_ADRP_MASK) == A64_INSN_ADRP_VAL;
}

static inline bool arm64_is_add_imm(u32 insn)
{
	return (insn & A64_INSN_ADD_IMM_MASK) == A64_INSN_ADD_IMM_VAL;
}

static inline bool arm64_is_mov_reg(u32 insn)
{
	return (insn & A64_INSN_MOV_REG_MASK) == A64_INSN_MOV_REG_VAL;
}

/*
 * Compute the page address from an ADRP instruction.
 * ADRP Xd, #imm: Xd = (PC & ~0xFFF) + (imm << 12)
 * immhi = bits[23:5] (19 bits), immlo = bits[30:29] (2 bits)
 * imm = sign_extend(immhi:immlo, 21)
 */
static u64 arm64_adrp_target(u64 pc, u32 insn)
{
	u64 immhi = (insn >> 5) & 0x7ffff;
	u64 immlo = (insn >> 29) & 0x3;
	u64 imm = (immhi << 2) | immlo;

	return (pc & ~0xfffULL) + (sign_extend64(imm, 20) << 12);
}

/*
 * Track register state for ARM64 instructions.
 *
 * Handles three instruction patterns:
 *
 * 1. ADRP Xd, #page - computes a PC-relative page address.
 *    Track the computed address so a subsequent LDR can resolve
 *    the global variable.
 *
 * 2. ADD Xd, Xn, #imm - if Xn holds a tracked address (from ADRP),
 *    propagate the adjusted address to Xd.
 *
 * 3. MOV Xd, Xm - propagate type state from Xm to Xd.
 */
static void update_insn_state_arm64(struct type_state *state,
				    struct data_loc_info *dloc,
				    Dwarf_Die *cu_die,
				    struct disasm_line *dl)
{
	u32 insn = dl->raw.raw_insn;
	int rd, rn;
	struct type_state_reg *tsr;

	if (arm64_is_adrp(insn)) {
		u64 pc, page_addr;
		int offset;
		Dwarf_Die type_die;

		rd = A64_RT(insn);
		if (!has_reg_type(state, rd))
			return;

		tsr = &state->regs[rd];

		pc = map__rip_2objdump(dloc->ms->map,
				       dloc->ms->sym->start + dl->al.offset);
		page_addr = arm64_adrp_target(pc, insn);

		/*
		 * Try to resolve the global variable at this page address.
		 * If not found, store it as a constant for later ADD resolution.
		 */
		if (get_global_var_type(cu_die, dloc,
					dloc->ms->sym->start + dl->al.offset,
					page_addr, &offset, &type_die)) {
			tsr->type = type_die;
			tsr->kind = TSR_KIND_POINTER;
			tsr->offset = offset;
			tsr->ok = true;

			pr_debug_dtp("adrp [%x] global addr=%#"PRIx64" -> reg%d",
				     (u32)dl->al.offset, page_addr, rd);
			pr_debug_type_name(&tsr->type, tsr->kind);
		} else {
			tsr->kind = TSR_KIND_CONST;
			tsr->imm_value = page_addr;
			tsr->ok = true;

			pr_debug_dtp("adrp [%x] page=%#"PRIx64" -> reg%d\n",
				     (u32)dl->al.offset, page_addr, rd);
		}
		return;
	}

	if (arm64_is_add_imm(insn)) {
		int imm12, shift;
		u64 var_addr;
		int offset;
		Dwarf_Die type_die;

		rd = A64_RT(insn);
		rn = A64_RN(insn);

		if (!has_reg_type(state, rd) || !has_reg_type(state, rn))
			return;

		tsr = &state->regs[rd];

		if (!state->regs[rn].ok) {
			tsr->ok = false;
			return;
		}

		imm12 = (insn >> 10) & 0xfff;
		shift = ((insn >> 22) & 0x1) ? 12 : 0;

		/*
		 * If Rn holds an ADRP result (TSR_KIND_CONST), compute
		 * the full address and try to resolve the global variable.
		 */
		if (state->regs[rn].kind == TSR_KIND_CONST) {
			var_addr = state->regs[rn].imm_value +
				   ((u64)imm12 << shift);

			if (get_global_var_type(cu_die, dloc,
						dloc->ms->sym->start + dl->al.offset,
						var_addr, &offset, &type_die)) {
				tsr->type = type_die;
				tsr->kind = TSR_KIND_POINTER;
				tsr->offset = offset;
				tsr->ok = true;

				pr_debug_dtp("add [%x] global addr=%#"PRIx64" -> reg%d",
					     (u32)dl->al.offset, var_addr, rd);
				pr_debug_type_name(&tsr->type, tsr->kind);
				return;
			}
		}

		/* Otherwise propagate existing type with adjusted offset */
		if (state->regs[rn].kind == TSR_KIND_TYPE ||
		    state->regs[rn].kind == TSR_KIND_POINTER) {
			tsr->type = state->regs[rn].type;
			tsr->kind = state->regs[rn].kind;
			tsr->offset = state->regs[rn].offset + (imm12 << shift);
			tsr->ok = true;

			pr_debug_dtp("add [%x] imm=%#x reg%d -> reg%d",
				     (u32)dl->al.offset, imm12 << shift, rn, rd);
			pr_debug_type_name(&tsr->type, tsr->kind);
		} else {
			tsr->ok = false;
		}
		return;
	}

	if (arm64_is_mov_reg(insn)) {
		int rm;

		rd = A64_RT(insn);
		rm = A64_RM(insn);

		if (!has_reg_type(state, rd))
			return;

		tsr = &state->regs[rd];

		if (!has_reg_type(state, rm) || !state->regs[rm].ok) {
			tsr->ok = false;
			return;
		}

		tsr->type = state->regs[rm].type;
		tsr->kind = state->regs[rm].kind;
		tsr->offset = state->regs[rm].offset;
		tsr->imm_value = state->regs[rm].imm_value;
		tsr->ok = true;

		pr_debug_dtp("mov [%x] reg%d -> reg%d",
			     (u32)dl->al.offset, rm, rd);
		pr_debug_type_name(&tsr->type, tsr->kind);
		return;
	}
}
#endif /* HAVE_LIBDW_SUPPORT */

static const struct ins_ops *arm64__associate_instruction_ops(struct arch *arch, const char *name)
{
	struct arch_arm64 *arm = container_of(arch, struct arch_arm64, arch);
	const struct ins_ops *ops;
	regmatch_t match[2];

	if (!regexec(&arm->jump_insn, name, 2, match, 0))
		ops = &jump_ops;
	else if (!regexec(&arm->call_insn, name, 2, match, 0))
		ops = &call_ops;
	else if (!strcmp(name, "ret"))
		ops = &ret_ops;
	else
		ops = &arm64_mov_ops;

	arch__associate_ins_ops(arch, name, ops);
	return ops;
}

const struct arch *arch__new_arm64(const struct e_machine_and_e_flags *id,
				   const char *cpuid __maybe_unused)
{
	int err;
	struct arch_arm64 *arm = zalloc(sizeof(*arm));
	struct arch *arch;

	if (!arm)
		return NULL;

	arch = &arm->arch;
	arch->name = "arm64";
	arch->id = *id;
	arch->objdump.comment_char	  = '/';
	arch->objdump.skip_functions_char = '+';
	arch->associate_instruction_ops   = arm64__associate_instruction_ops;
	annotate_opts.show_asm_raw = true;
#ifdef HAVE_LIBDW_SUPPORT
	arch->update_insn_state = update_insn_state_arm64;
#endif

	/* bl, blr */
	err = regcomp(&arm->call_insn, "^blr?$", REG_EXTENDED);
	if (err)
		goto out_free_arm;

	/* b, b.cond, br, cbz/cbnz, tbz/tbnz */
	err = regcomp(&arm->jump_insn, "^[ct]?br?\\.?(cc|cs|eq|ge|gt|hi|hs|le|lo|ls|lt|mi|ne|pl|vc|vs)?n?z?$",
		      REG_EXTENDED);
	if (err)
		goto out_free_call;

	return arch;

out_free_call:
	regfree(&arm->call_insn);
out_free_arm:
	free(arm);
	errno = SYMBOL_ANNOTATE_ERRNO__ARCH_INIT_REGEXP;
	return NULL;
}

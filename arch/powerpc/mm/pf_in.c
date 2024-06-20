// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Derived from arch/x86/mm/pf_in.c:
 *   Copyright (C) 2024 Jialong Yang (jialong.yang@shingroup.cn)
 */

#include <linux/ptrace.h> /* struct pt_regs */
#include "pf_in.h"
#include <linux/printk.h>
#include <linux/mmiotrace.h>

/* D 32 0x80000000 B lwz Load Word and Zero */
/* D 33 0x84000000 B lwz Load Word and Zero with Update */
/* D 34 0x88000000 B lbz Load Byte and Zero */
/* D 33 0x8C000000 B lbzu Load Word and Zero with Update */
/* D 35 0x90000000 B stw Store Word */
/* D 36 0x94000000 B stwu Store Word with Update */
/* D 37 0x98000000 B stb Store Byte */
/* D 38 0x9C000000 B stbu Store Byte with Update */
/* D 40 0xA0000000 B lhz Load Halfword and Zero with Update */
/* D 41 0xA4000000 B lhzu Load Halfword and Zero with Update */
/* D 42 0xA8000000 B lha Load Halfword Algebraic */
/* D 43 0xAC000000 B lhau Load Halfword Algebraic with Update */
/* D 44 0xB0000000 B sth Store Halfword */
/* D 45 0xB4000000 B sthu Store Halfword with Update */
/* D 46 0xB8000000 B lmw Load Multiple Word */
/* D 47 0xBC000000 B stmw Store Multiple Word */
/* D 48 0xC0000000 FP lfs Load Floating-Point Single */
/* D 49 0xC4000000 FP lfsu Load Floating-Point Single with Update */
/* D 50 0xC8000000 FP lfd Load Floating-Point Double */
/* D 51 0xCC000000 FP lfdu Load Floating-Point Double with Update */
/* D 52 0xD0000000 FP stfs Store Floating-Point Single */
/* D 53 0xD4000000 FP stfsu Store Floating-Point Single with Update */
/* D 54 0xD8000000 FP stfd Store Floating-Point Double */
/* D 55 0xDC000000 FP stfdu Store Floating-Point Double with Update */
/* DQ 56 0xE0000000 P 58 LSQ lq Load Quadword */
/* DS 57 0xE4000000 140 FP.out Lfdp Load Floating-Point Double Pair */
/* DS 58 0xE8000000 53 64 Ldu Load Doubleword with Update */
/* DS 58 0xE8000001 53 64 Ld Load Doubleword */
/* DS 58 0xE8000002 52 64 Lwa Load Word Algebraic */
/* DS 62 0xF8000000 57 64 std Store Doubleword */
/* DS 62 0xF8000001 57 64 stdu Store Doubleword with Update */
/* DS 62 0xF8000002 59 LSQ stq Store Quadword */

// D-form:
// 0-5    6-10    11-15   16-31
// opcode RT      RA      Offset

// DQ-form:
// 0-5    6-10  11-15  16-27
// opcode RT    RA     Offset

// DS-form:
// 0-5    6-10  11-15  16-29  30-31
// opcode RT    RA     Offset opcode

#define D_OPCODE_MASK GENMASK(31, 26)
#define DQ_OPCODE_MASK D_OPCODE_MASK
#define DS_OPCODE_MASK (D_OPCODE_MASK | GENMASK(0, 1))
#define RS_RT_OFFSET 21UL
#define RS_RT_MASK GENMASK(25, 21)
#define RA_MASK GENMASK(20, 16)
#define D_OFFSET GENMASK(15, 0)
#define DQ_OFFSET GENMASK(15, 4)
#define DS_OFFSET GENMASK(15, 2)

struct opcode_t opcodes[] = {
	{0x80000000, D_FORMAT, "lwz", },
	{0x84000000, D_FORMAT, "lwzu", },
	{0x88000000, D_FORMAT, "lbz", },
	{0x8C000000, D_FORMAT, "lbzu", },
	{0x90000000, D_FORMAT, "stw", },
	{0x94000000, D_FORMAT, "stwu", },
	{0x98000000, D_FORMAT, "stb", },
	{0x9C000000, D_FORMAT, "stbu", },
	{0xA0000000, D_FORMAT, "lhz", },
	{0xA4000000, D_FORMAT, "lhzu", },
	{0xA8000000, D_FORMAT, "lha", },
	{0xAC000000, D_FORMAT, "lhau", },
	{0xB0000000, D_FORMAT, "sth", },
	{0xB4000000, D_FORMAT, "sthu", },
	{0xB8000000, D_FORMAT, "lmw", },
	{0xBC000000, D_FORMAT, "stmw", },
	{0xC0000000, D_FORMAT, "lfs", },
	{0xC4000000, D_FORMAT, "lfsu", },
	{0xC8000000, D_FORMAT, "lfd", },
	{0xCC000000, D_FORMAT, "lfdu", },
	{0xD0000000, D_FORMAT, "stfs", },
	{0xD4000000, D_FORMAT, "stfsu", },
	{0xD8000000, D_FORMAT, "stfd", },
	{0xDC000000, D_FORMAT, "stfdu", },
	{0xE0000000, DQ_FORMAT, "lq", },
	{0xE4000000, DS_FORMAT, "lfdp", },
	{0xE8000000, DS_FORMAT, "ldu", },
	{0xE8000001, DS_FORMAT, "ld", },
	{0xE8000002, DS_FORMAT, "lwa", },
	{0xF8000000, DS_FORMAT, "std", },
	{0xF8000001, DS_FORMAT, "stdu", },
	{0xF8000002, DS_FORMAT, "stq", }
};

struct opcode_t *get_opcode(unsigned int *addr)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(opcodes); i++) {
		switch (opcodes[i].form) {
		case D_FORMAT:
			if (opcodes[i].opcode == (*addr & D_OPCODE_MASK))
				return &opcodes[i];
			break;
		case DQ_FORMAT:
			if (opcodes[i].opcode == (*addr & DQ_OPCODE_MASK))
				return &opcodes[i];
			break;
		case DS_FORMAT:
			if (opcodes[i].opcode == (*addr & DQ_OPCODE_MASK))
				return &opcodes[i];
			break;
		}
	}

	return NULL;
}

inline enum mm_io_opcode get_ins_type(struct opcode_t *opcode)
{
	if (!opcode)
		return MMIO_UNKNOWN_OP;

	if (opcode->name[0] == 'l')
		return MMIO_READ;

	if (opcode->name[0] == 's')
		return MMIO_WRITE;

	return MMIO_UNKNOWN_OP;
}

unsigned int get_ins_width(struct opcode_t *opcode)
{
	char width_ch;

	if (!opcode)
		return 0;

	if (opcode->name[0] == 'l')
		width_ch = opcode->name[1];

	if (opcode->name[0] == 's')
		width_ch = opcode->name[2];

	switch (width_ch) {
	case 'b': /* byte */
		return 1;
	case 'h': /* half word */
		return sizeof(long) / 2;
	case 'w': /* word */
		/* return sizeof(long); */
	case 'm': /* multi words(can be calculated out by (32-RT) * sizeof(long)) */
	case 'f': /* float(not too much. So ignore word number) */
	case 'd': /* double words */
		/* return 2 * sizeof(long); */
	case 'q': /* quad words */
		/* return 4 * sizeof(long); */
	default:
		return sizeof(long);
	}
}

unsigned long get_ins_val(struct trap_reason *reason, struct pt_regs *regs)
{
	struct opcode_t *opcode = reason->opcode;
	unsigned int ins = *(unsigned int *)(reason->ip);
	unsigned int reg_no;
	unsigned long mask = ~0UL;

	if (!opcode)
		return 0;

	mask >>= 8 * (sizeof(long) - get_ins_width(opcode));
	reg_no = (ins & RS_RT_MASK) >> RS_RT_OFFSET;

	return regs->gpr[reg_no] & mask;
}

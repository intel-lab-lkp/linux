/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef _EH_FRAME_H
#define _EH_FRAME_H

/* DWARF CFI opcodes */
#define DW_CFA_advance_loc		0x40
#define DW_CFA_offset			0x80
#define DW_CFA_restore			0xc0
#define DW_CFA_nop			0x00
#define DW_CFA_set_loc			0x01
#define DW_CFA_advance_loc1		0x02
#define DW_CFA_advance_loc2		0x03
#define DW_CFA_advance_loc4		0x04
#define DW_CFA_offset_extended		0x05
#define DW_CFA_restore_extended		0x06
#define DW_CFA_undefined		0x07
#define DW_CFA_same_value		0x08
#define DW_CFA_register			0x09
#define DW_CFA_remember_state		0x0a
#define DW_CFA_restore_state		0x0b
#define DW_CFA_def_cfa			0x0c
#define DW_CFA_def_cfa_register		0x0d
#define DW_CFA_def_cfa_offset		0x0e
#define DW_CFA_def_cfa_expression	0x0f
#define DW_CFA_expression		0x10
#define DW_CFA_offset_extended_sf	0x11
#define DW_CFA_def_cfa_sf		0x12
#define DW_CFA_def_cfa_offset_sf	0x13
#define DW_CFA_val_offset		0x14
#define DW_CFA_val_offset_sf		0x15
#define DW_CFA_val_expression		0x16
#define DW_CFA_GNU_args_size		0x2e

/* Helpers for CFI opcodes */
#define DW_CFA_opcode(insn)		((insn) & 0xc0)
#define DW_CFA_operand(insn)		((insn) & 0x3f)

/* DWARF exception header pointer encodings */
#define DW_EH_PE_omit			0xff
/* Formats */
#define DW_EH_PE_absptr			0x00
#define DW_EH_PE_uleb128		0x01
#define DW_EH_PE_udata2			0x02
#define DW_EH_PE_udata4			0x03
#define DW_EH_PE_udata8			0x04
#define DW_EH_PE_sleb128		0x09
#define DW_EH_PE_sdata2			0x0a
#define DW_EH_PE_sdata4			0x0b
#define DW_EH_PE_sdata8			0x0c
/* Applications */
#define DW_EH_PE_pcrel			0x10
#define DW_EH_PE_textrel		0x20
#define DW_EH_PE_datarel		0x30
#define DW_EH_PE_funcrel		0x40
#define DW_EH_PE_aligned		0x50
/* Flags */
#define DW_EH_PE_indirect		0x80

/* Helpers for DWARF exception header pointer encodings */
#define DW_EH_PE_format(encoding)	((encoding) & 0x0f)
#define DW_EH_PE_application(encoding)	((encoding) & 0x70)

/* DWARF expression operations */
#define DW_OP_lit0	0x30
/* ... */
#define DW_OP_lit31	0x4f
#define DW_OP_breg0	0x70
/* ... */
#define DW_OP_breg31	0x8f

/* Helpers for DWARF expression operations */
#define DW_OP_is_lit(op)		((op) >= DW_OP_lit0 && (op) <= DW_OP_lit31)
#define DW_OP_is_breg(op)		((op) >= DW_OP_breg0 && (op) <= DW_OP_breg31)
#define DW_OP_lit_value(op)		((op) - DW_OP_lit0)
#define DW_OP_breg_register(op)		((op) - DW_OP_breg0)

/* CIE/FDE constants */
#define EH_FRAME_CIE_ID			0
#define EH_FRAME_DWARF64_LENGTH		0xffffffff
#define EH_FRAME_CIE_MIN_LENGTH		13
#define EH_FRAME_FDE_MIN_LENGTH		10

#endif /* _EH_FRAME_H */

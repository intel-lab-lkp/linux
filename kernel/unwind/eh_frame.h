/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef _EH_FRAME_H
#define _EH_FRAME_H

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

#endif /* _EH_FRAME_H */

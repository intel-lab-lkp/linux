/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef _UAPI_LINUX_ELF_R_H
#define _UAPI_LINUX_ELF_R_H

/* i386 relocation types */
#define R_386_NONE	0
#define R_386_32	1
#define R_386_PC32	2
#define R_386_GOT32	3
#define R_386_PLT32	4
#define R_386_COPY	5
#define R_386_GLOB_DAT	6
#define R_386_JMP_SLOT	7
#define R_386_RELATIVE	8
#define R_386_GOTOFF	9
#define R_386_GOTPC	10
#define R_386_NUM	11

/* x86-64 relocation types */
#define R_X86_64_NONE		0	/* No reloc */
#define R_X86_64_64		1	/* Direct 64 bit  */
#define R_X86_64_PC32		2	/* PC relative 32 bit signed */
#define R_X86_64_GOT32		3	/* 32 bit GOT entry */
#define R_X86_64_PLT32		4	/* 32 bit PLT address */
#define R_X86_64_COPY		5	/* Copy symbol at runtime */
#define R_X86_64_GLOB_DAT	6	/* Create GOT entry */
#define R_X86_64_JUMP_SLOT	7	/* Create PLT entry */
#define R_X86_64_RELATIVE	8	/* Adjust by program base */
#define R_X86_64_GOTPCREL	9	/* 32 bit signed pc relative
					   offset to GOT */
#define R_X86_64_32		10	/* Direct 32 bit zero extended */
#define R_X86_64_32S		11	/* Direct 32 bit sign extended */
#define R_X86_64_16		12	/* Direct 16 bit zero extended */
#define R_X86_64_PC16		13	/* 16 bit sign extended pc relative */
#define R_X86_64_8		14	/* Direct 8 bit sign extended  */
#define R_X86_64_PC8		15	/* 8 bit sign extended pc relative */
#define R_X86_64_PC64		24	/* Place relative 64-bit signed */

/* arm relocation types */
#define R_ARM_NONE		0
#define R_ARM_PC24		1
#define R_ARM_ABS32		2
#define R_ARM_REL32		3
#define R_ARM_CALL		28
#define R_ARM_JUMP24		29
#define R_ARM_TARGET1		38
#define R_ARM_V4BX		40
#define R_ARM_PREL31		42
#define R_ARM_MOVW_ABS_NC	43
#define R_ARM_MOVT_ABS		44
#define R_ARM_MOVW_PREL_NC	45
#define R_ARM_MOVT_PREL		46
#define R_ARM_ALU_PC_G0_NC	57
#define R_ARM_ALU_PC_G1_NC	59
#define R_ARM_LDR_PC_G2		63

#define R_ARM_THM_CALL		10
#define R_ARM_THM_JUMP24	30
#define R_ARM_THM_MOVW_ABS_NC	47
#define R_ARM_THM_MOVT_ABS	48
#define R_ARM_THM_MOVW_PREL_NC	49
#define R_ARM_THM_MOVT_PREL	50

/* AArch64 static relocation types */

/* Miscellaneous. */
#define R_AARCH64_NONE			256

/* Data. */
#define R_AARCH64_ABS64			257
#define R_AARCH64_ABS32			258
#define R_AARCH64_ABS16			259
#define R_AARCH64_PREL64		260
#define R_AARCH64_PREL32		261
#define R_AARCH64_PREL16		262

/* Instructions. */
#define R_AARCH64_MOVW_UABS_G0		263
#define R_AARCH64_MOVW_UABS_G0_NC	264
#define R_AARCH64_MOVW_UABS_G1		265
#define R_AARCH64_MOVW_UABS_G1_NC	266
#define R_AARCH64_MOVW_UABS_G2		267
#define R_AARCH64_MOVW_UABS_G2_NC	268
#define R_AARCH64_MOVW_UABS_G3		269

#define R_AARCH64_MOVW_SABS_G0		270
#define R_AARCH64_MOVW_SABS_G1		271
#define R_AARCH64_MOVW_SABS_G2		272

#define R_AARCH64_LD_PREL_LO19		273
#define R_AARCH64_ADR_PREL_LO21		274
#define R_AARCH64_ADR_PREL_PG_HI21	275
#define R_AARCH64_ADR_PREL_PG_HI21_NC	276
#define R_AARCH64_ADD_ABS_LO12_NC	277
#define R_AARCH64_LDST8_ABS_LO12_NC	278

#define R_AARCH64_TSTBR14		279
#define R_AARCH64_CONDBR19		280
#define R_AARCH64_JUMP26		282
#define R_AARCH64_CALL26		283
#define R_AARCH64_LDST16_ABS_LO12_NC	284
#define R_AARCH64_LDST32_ABS_LO12_NC	285
#define R_AARCH64_LDST64_ABS_LO12_NC	286
#define R_AARCH64_LDST128_ABS_LO12_NC	299

#define R_AARCH64_MOVW_PREL_G0		287
#define R_AARCH64_MOVW_PREL_G0_NC	288
#define R_AARCH64_MOVW_PREL_G1		289
#define R_AARCH64_MOVW_PREL_G1_NC	290
#define R_AARCH64_MOVW_PREL_G2		291
#define R_AARCH64_MOVW_PREL_G2_NC	292
#define R_AARCH64_MOVW_PREL_G3		293

#define R_AARCH64_RELATIVE		1027

/* PowerPC relocations defined by the ABIs */
#define R_PPC_NONE		0
#define R_PPC_ADDR32		1	/* 32bit absolute address */
#define R_PPC_ADDR24		2	/* 26bit address, 2 bits ignored.  */
#define R_PPC_ADDR16		3	/* 16bit absolute address */
#define R_PPC_ADDR16_LO		4	/* lower 16bit of absolute address */
#define R_PPC_ADDR16_HI		5	/* high 16bit of absolute address */
#define R_PPC_ADDR16_HA		6	/* adjusted high 16bit */
#define R_PPC_ADDR14		7	/* 16bit address, 2 bits ignored */
#define R_PPC_ADDR14_BRTAKEN	8
#define R_PPC_ADDR14_BRNTAKEN	9
#define R_PPC_REL24		10	/* PC relative 26 bit */
#define R_PPC_REL14		11	/* PC relative 16 bit */
#define R_PPC_REL14_BRTAKEN	12
#define R_PPC_REL14_BRNTAKEN	13
#define R_PPC_GOT16		14
#define R_PPC_GOT16_LO		15
#define R_PPC_GOT16_HI		16
#define R_PPC_GOT16_HA		17
#define R_PPC_PLTREL24		18
#define R_PPC_COPY		19
#define R_PPC_GLOB_DAT		20
#define R_PPC_JMP_SLOT		21
#define R_PPC_RELATIVE		22
#define R_PPC_LOCAL24PC		23
#define R_PPC_UADDR32		24
#define R_PPC_UADDR16		25
#define R_PPC_REL32		26
#define R_PPC_PLT32		27
#define R_PPC_PLTREL32		28
#define R_PPC_PLT16_LO		29
#define R_PPC_PLT16_HI		30
#define R_PPC_PLT16_HA		31
#define R_PPC_SDAREL16		32
#define R_PPC_SECTOFF		33
#define R_PPC_SECTOFF_LO	34
#define R_PPC_SECTOFF_HI	35
#define R_PPC_SECTOFF_HA	36

/* PowerPC relocations defined for the TLS access ABI.  */
#define R_PPC_TLS		67 /* none	(sym+add)@tls */
#define R_PPC_DTPMOD32		68 /* word32	(sym+add)@dtpmod */
#define R_PPC_TPREL16		69 /* half16*	(sym+add)@tprel */
#define R_PPC_TPREL16_LO	70 /* half16	(sym+add)@tprel@l */
#define R_PPC_TPREL16_HI	71 /* half16	(sym+add)@tprel@h */
#define R_PPC_TPREL16_HA	72 /* half16	(sym+add)@tprel@ha */
#define R_PPC_TPREL32		73 /* word32	(sym+add)@tprel */
#define R_PPC_DTPREL16		74 /* half16*	(sym+add)@dtprel */
#define R_PPC_DTPREL16_LO	75 /* half16	(sym+add)@dtprel@l */
#define R_PPC_DTPREL16_HI	76 /* half16	(sym+add)@dtprel@h */
#define R_PPC_DTPREL16_HA	77 /* half16	(sym+add)@dtprel@ha */
#define R_PPC_DTPREL32		78 /* word32	(sym+add)@dtprel */
#define R_PPC_GOT_TLSGD16	79 /* half16*	(sym+add)@got@tlsgd */
#define R_PPC_GOT_TLSGD16_LO	80 /* half16	(sym+add)@got@tlsgd@l */
#define R_PPC_GOT_TLSGD16_HI	81 /* half16	(sym+add)@got@tlsgd@h */
#define R_PPC_GOT_TLSGD16_HA	82 /* half16	(sym+add)@got@tlsgd@ha */
#define R_PPC_GOT_TLSLD16	83 /* half16*	(sym+add)@got@tlsld */
#define R_PPC_GOT_TLSLD16_LO	84 /* half16	(sym+add)@got@tlsld@l */
#define R_PPC_GOT_TLSLD16_HI	85 /* half16	(sym+add)@got@tlsld@h */
#define R_PPC_GOT_TLSLD16_HA	86 /* half16	(sym+add)@got@tlsld@ha */
#define R_PPC_GOT_TPREL16	87 /* half16*	(sym+add)@got@tprel */
#define R_PPC_GOT_TPREL16_LO	88 /* half16	(sym+add)@got@tprel@l */
#define R_PPC_GOT_TPREL16_HI	89 /* half16	(sym+add)@got@tprel@h */
#define R_PPC_GOT_TPREL16_HA	90 /* half16	(sym+add)@got@tprel@ha */
#define R_PPC_GOT_DTPREL16	91 /* half16*	(sym+add)@got@dtprel */
#define R_PPC_GOT_DTPREL16_LO	92 /* half16*	(sym+add)@got@dtprel@l */
#define R_PPC_GOT_DTPREL16_HI	93 /* half16*	(sym+add)@got@dtprel@h */
#define R_PPC_GOT_DTPREL16_HA	94 /* half16*	(sym+add)@got@dtprel@ha */

/* keep this the last entry. */
#define R_PPC_NUM		95

/* PowerPC64 relocations defined by the ABIs */
#define R_PPC64_NONE    R_PPC_NONE
#define R_PPC64_ADDR32  R_PPC_ADDR32  /* 32bit absolute address.  */
#define R_PPC64_ADDR24  R_PPC_ADDR24  /* 26bit address, word aligned.  */
#define R_PPC64_ADDR16  R_PPC_ADDR16  /* 16bit absolute address. */
#define R_PPC64_ADDR16_LO R_PPC_ADDR16_LO /* lower 16bits of abs. address.  */
#define R_PPC64_ADDR16_HI R_PPC_ADDR16_HI /* high 16bits of abs. address. */
#define R_PPC64_ADDR16_HA R_PPC_ADDR16_HA /* adjusted high 16bits.  */
#define R_PPC64_ADDR14 R_PPC_ADDR14   /* 16bit address, word aligned.  */
#define R_PPC64_ADDR14_BRTAKEN  R_PPC_ADDR14_BRTAKEN
#define R_PPC64_ADDR14_BRNTAKEN R_PPC_ADDR14_BRNTAKEN
#define R_PPC64_REL24   R_PPC_REL24 /* PC relative 26 bit, word aligned.  */
#define R_PPC64_REL14   R_PPC_REL14 /* PC relative 16 bit. */
#define R_PPC64_REL14_BRTAKEN   R_PPC_REL14_BRTAKEN
#define R_PPC64_REL14_BRNTAKEN  R_PPC_REL14_BRNTAKEN
#define R_PPC64_GOT16     R_PPC_GOT16
#define R_PPC64_GOT16_LO  R_PPC_GOT16_LO
#define R_PPC64_GOT16_HI  R_PPC_GOT16_HI
#define R_PPC64_GOT16_HA  R_PPC_GOT16_HA

#define R_PPC64_COPY      R_PPC_COPY
#define R_PPC64_GLOB_DAT  R_PPC_GLOB_DAT
#define R_PPC64_JMP_SLOT  R_PPC_JMP_SLOT
#define R_PPC64_RELATIVE  R_PPC_RELATIVE

#define R_PPC64_UADDR32   R_PPC_UADDR32
#define R_PPC64_UADDR16   R_PPC_UADDR16
#define R_PPC64_REL32     R_PPC_REL32
#define R_PPC64_PLT32     R_PPC_PLT32
#define R_PPC64_PLTREL32  R_PPC_PLTREL32
#define R_PPC64_PLT16_LO  R_PPC_PLT16_LO
#define R_PPC64_PLT16_HI  R_PPC_PLT16_HI
#define R_PPC64_PLT16_HA  R_PPC_PLT16_HA

#define R_PPC64_SECTOFF     R_PPC_SECTOFF
#define R_PPC64_SECTOFF_LO  R_PPC_SECTOFF_LO
#define R_PPC64_SECTOFF_HI  R_PPC_SECTOFF_HI
#define R_PPC64_SECTOFF_HA  R_PPC_SECTOFF_HA
#define R_PPC64_ADDR30          37  /* word30 (S + A - P) >> 2.  */
#define R_PPC64_ADDR64          38  /* doubleword64 S + A.  */
#define R_PPC64_ADDR16_HIGHER   39  /* half16 #higher(S + A).  */
#define R_PPC64_ADDR16_HIGHERA  40  /* half16 #highera(S + A).  */
#define R_PPC64_ADDR16_HIGHEST  41  /* half16 #highest(S + A).  */
#define R_PPC64_ADDR16_HIGHESTA 42  /* half16 #highesta(S + A). */
#define R_PPC64_UADDR64     43  /* doubleword64 S + A.  */
#define R_PPC64_REL64       44  /* doubleword64 S + A - P.  */
#define R_PPC64_PLT64       45  /* doubleword64 L + A.  */
#define R_PPC64_PLTREL64    46  /* doubleword64 L + A - P.  */
#define R_PPC64_TOC16       47  /* half16* S + A - .TOC.  */
#define R_PPC64_TOC16_LO    48  /* half16 #lo(S + A - .TOC.).  */
#define R_PPC64_TOC16_HI    49  /* half16 #hi(S + A - .TOC.).  */
#define R_PPC64_TOC16_HA    50  /* half16 #ha(S + A - .TOC.).  */
#define R_PPC64_TOC         51  /* doubleword64 .TOC. */
#define R_PPC64_PLTGOT16    52  /* half16* M + A.  */
#define R_PPC64_PLTGOT16_LO 53  /* half16 #lo(M + A).  */
#define R_PPC64_PLTGOT16_HI 54  /* half16 #hi(M + A).  */
#define R_PPC64_PLTGOT16_HA 55  /* half16 #ha(M + A).  */

#define R_PPC64_ADDR16_DS      56 /* half16ds* (S + A) >> 2.  */
#define R_PPC64_ADDR16_LO_DS   57 /* half16ds  #lo(S + A) >> 2.  */
#define R_PPC64_GOT16_DS       58 /* half16ds* (G + A) >> 2.  */
#define R_PPC64_GOT16_LO_DS    59 /* half16ds  #lo(G + A) >> 2.  */
#define R_PPC64_PLT16_LO_DS    60 /* half16ds  #lo(L + A) >> 2.  */
#define R_PPC64_SECTOFF_DS     61 /* half16ds* (R + A) >> 2.  */
#define R_PPC64_SECTOFF_LO_DS  62 /* half16ds  #lo(R + A) >> 2.  */
#define R_PPC64_TOC16_DS       63 /* half16ds* (S + A - .TOC.) >> 2.  */
#define R_PPC64_TOC16_LO_DS    64 /* half16ds  #lo(S + A - .TOC.) >> 2.  */
#define R_PPC64_PLTGOT16_DS    65 /* half16ds* (M + A) >> 2.  */
#define R_PPC64_PLTGOT16_LO_DS 66 /* half16ds  #lo(M + A) >> 2.  */

/* PowerPC64 relocations defined for the TLS access ABI.  */
#define R_PPC64_TLS		67 /* none	(sym+add)@tls */
#define R_PPC64_DTPMOD64	68 /* doubleword64 (sym+add)@dtpmod */
#define R_PPC64_TPREL16		69 /* half16*	(sym+add)@tprel */
#define R_PPC64_TPREL16_LO	70 /* half16	(sym+add)@tprel@l */
#define R_PPC64_TPREL16_HI	71 /* half16	(sym+add)@tprel@h */
#define R_PPC64_TPREL16_HA	72 /* half16	(sym+add)@tprel@ha */
#define R_PPC64_TPREL64		73 /* doubleword64 (sym+add)@tprel */
#define R_PPC64_DTPREL16	74 /* half16*	(sym+add)@dtprel */
#define R_PPC64_DTPREL16_LO	75 /* half16	(sym+add)@dtprel@l */
#define R_PPC64_DTPREL16_HI	76 /* half16	(sym+add)@dtprel@h */
#define R_PPC64_DTPREL16_HA	77 /* half16	(sym+add)@dtprel@ha */
#define R_PPC64_DTPREL64	78 /* doubleword64 (sym+add)@dtprel */
#define R_PPC64_GOT_TLSGD16	79 /* half16*	(sym+add)@got@tlsgd */
#define R_PPC64_GOT_TLSGD16_LO	80 /* half16	(sym+add)@got@tlsgd@l */
#define R_PPC64_GOT_TLSGD16_HI	81 /* half16	(sym+add)@got@tlsgd@h */
#define R_PPC64_GOT_TLSGD16_HA	82 /* half16	(sym+add)@got@tlsgd@ha */
#define R_PPC64_GOT_TLSLD16	83 /* half16*	(sym+add)@got@tlsld */
#define R_PPC64_GOT_TLSLD16_LO	84 /* half16	(sym+add)@got@tlsld@l */
#define R_PPC64_GOT_TLSLD16_HI	85 /* half16	(sym+add)@got@tlsld@h */
#define R_PPC64_GOT_TLSLD16_HA	86 /* half16	(sym+add)@got@tlsld@ha */
#define R_PPC64_GOT_TPREL16_DS	87 /* half16ds*	(sym+add)@got@tprel */
#define R_PPC64_GOT_TPREL16_LO_DS 88 /* half16ds (sym+add)@got@tprel@l */
#define R_PPC64_GOT_TPREL16_HI	89 /* half16	(sym+add)@got@tprel@h */
#define R_PPC64_GOT_TPREL16_HA	90 /* half16	(sym+add)@got@tprel@ha */
#define R_PPC64_GOT_DTPREL16_DS	91 /* half16ds*	(sym+add)@got@dtprel */
#define R_PPC64_GOT_DTPREL16_LO_DS 92 /* half16ds (sym+add)@got@dtprel@l */
#define R_PPC64_GOT_DTPREL16_HI	93 /* half16	(sym+add)@got@dtprel@h */
#define R_PPC64_GOT_DTPREL16_HA	94 /* half16	(sym+add)@got@dtprel@ha */
#define R_PPC64_TPREL16_DS	95 /* half16ds*	(sym+add)@tprel */
#define R_PPC64_TPREL16_LO_DS	96 /* half16ds	(sym+add)@tprel@l */
#define R_PPC64_TPREL16_HIGHER	97 /* half16	(sym+add)@tprel@higher */
#define R_PPC64_TPREL16_HIGHERA	98 /* half16	(sym+add)@tprel@highera */
#define R_PPC64_TPREL16_HIGHEST	99 /* half16	(sym+add)@tprel@highest */
#define R_PPC64_TPREL16_HIGHESTA 100 /* half16	(sym+add)@tprel@highesta */
#define R_PPC64_DTPREL16_DS	101 /* half16ds* (sym+add)@dtprel */
#define R_PPC64_DTPREL16_LO_DS	102 /* half16ds	(sym+add)@dtprel@l */
#define R_PPC64_DTPREL16_HIGHER	103 /* half16	(sym+add)@dtprel@higher */
#define R_PPC64_DTPREL16_HIGHERA 104 /* half16	(sym+add)@dtprel@highera */
#define R_PPC64_DTPREL16_HIGHEST 105 /* half16	(sym+add)@dtprel@highest */
#define R_PPC64_DTPREL16_HIGHESTA 106 /* half16	(sym+add)@dtprel@highesta */
#define R_PPC64_TLSGD		107
#define R_PPC64_TLSLD		108
#define R_PPC64_TOCSAVE		109

#define R_PPC64_REL24_NOTOC	116
#define R_PPC64_ENTRY		118

#define R_PPC64_PCREL34		132
#define R_PPC64_GOT_PCREL34	133

#define R_PPC64_REL16		249
#define R_PPC64_REL16_LO	250
#define R_PPC64_REL16_HI	251
#define R_PPC64_REL16_HA	252

/* Keep this the last entry.  */
#define R_PPC64_NUM		253


/*
 * RISC-V relocation types
 */

/* Relocation types used by the dynamic linker */
#define R_RISCV_NONE		0
#define R_RISCV_32		1
#define R_RISCV_64		2
#define R_RISCV_RELATIVE	3
#define R_RISCV_COPY		4
#define R_RISCV_JUMP_SLOT	5
#define R_RISCV_TLS_DTPMOD32	6
#define R_RISCV_TLS_DTPMOD64	7
#define R_RISCV_TLS_DTPREL32	8
#define R_RISCV_TLS_DTPREL64	9
#define R_RISCV_TLS_TPREL32	10
#define R_RISCV_TLS_TPREL64	11
#define R_RISCV_IRELATIVE	58

/* Relocation types not used by the dynamic linker */
#define R_RISCV_BRANCH		16
#define R_RISCV_JAL		17
#define R_RISCV_CALL		18
#define R_RISCV_CALL_PLT	19
#define R_RISCV_GOT_HI20	20
#define R_RISCV_TLS_GOT_HI20	21
#define R_RISCV_TLS_GD_HI20	22
#define R_RISCV_PCREL_HI20	23
#define R_RISCV_PCREL_LO12_I	24
#define R_RISCV_PCREL_LO12_S	25
#define R_RISCV_HI20		26
#define R_RISCV_LO12_I		27
#define R_RISCV_LO12_S		28
#define R_RISCV_TPREL_HI20	29
#define R_RISCV_TPREL_LO12_I	30
#define R_RISCV_TPREL_LO12_S	31
#define R_RISCV_TPREL_ADD	32
#define R_RISCV_ADD8		33
#define R_RISCV_ADD16		34
#define R_RISCV_ADD32		35
#define R_RISCV_ADD64		36
#define R_RISCV_SUB8		37
#define R_RISCV_SUB16		38
#define R_RISCV_SUB32		39
#define R_RISCV_SUB64		40
#define R_RISCV_GNU_VTINHERIT	41
#define R_RISCV_GNU_VTENTRY	42
#define R_RISCV_ALIGN		43
#define R_RISCV_RVC_BRANCH	44
#define R_RISCV_RVC_JUMP	45
#define R_RISCV_GPREL_I		47
#define R_RISCV_GPREL_S		48
#define R_RISCV_TPREL_I		49
#define R_RISCV_TPREL_S		50
#define R_RISCV_RELAX		51
#define R_RISCV_SUB6		52
#define R_RISCV_SET6		53
#define R_RISCV_SET8		54
#define R_RISCV_SET16		55
#define R_RISCV_SET32		56
#define R_RISCV_32_PCREL	57
#define R_RISCV_PLT32		59
#define R_RISCV_SET_ULEB128	60
#define R_RISCV_SUB_ULEB128	61

/* LoongArch relocation types used by the dynamic linker */
#define R_LARCH_NONE				0
#define R_LARCH_32				1
#define R_LARCH_64				2
#define R_LARCH_RELATIVE			3
#define R_LARCH_COPY				4
#define R_LARCH_JUMP_SLOT			5
#define R_LARCH_TLS_DTPMOD32			6
#define R_LARCH_TLS_DTPMOD64			7
#define R_LARCH_TLS_DTPREL32			8
#define R_LARCH_TLS_DTPREL64			9
#define R_LARCH_TLS_TPREL32			10
#define R_LARCH_TLS_TPREL64			11
#define R_LARCH_IRELATIVE			12
#define R_LARCH_MARK_LA				20
#define R_LARCH_MARK_PCREL			21
#define R_LARCH_SOP_PUSH_PCREL			22
#define R_LARCH_SOP_PUSH_ABSOLUTE		23
#define R_LARCH_SOP_PUSH_DUP			24
#define R_LARCH_SOP_PUSH_GPREL			25
#define R_LARCH_SOP_PUSH_TLS_TPREL		26
#define R_LARCH_SOP_PUSH_TLS_GOT		27
#define R_LARCH_SOP_PUSH_TLS_GD			28
#define R_LARCH_SOP_PUSH_PLT_PCREL		29
#define R_LARCH_SOP_ASSERT			30
#define R_LARCH_SOP_NOT				31
#define R_LARCH_SOP_SUB				32
#define R_LARCH_SOP_SL				33
#define R_LARCH_SOP_SR				34
#define R_LARCH_SOP_ADD				35
#define R_LARCH_SOP_AND				36
#define R_LARCH_SOP_IF_ELSE			37
#define R_LARCH_SOP_POP_32_S_10_5		38
#define R_LARCH_SOP_POP_32_U_10_12		39
#define R_LARCH_SOP_POP_32_S_10_12		40
#define R_LARCH_SOP_POP_32_S_10_16		41
#define R_LARCH_SOP_POP_32_S_10_16_S2		42
#define R_LARCH_SOP_POP_32_S_5_20		43
#define R_LARCH_SOP_POP_32_S_0_5_10_16_S2	44
#define R_LARCH_SOP_POP_32_S_0_10_10_16_S2	45
#define R_LARCH_SOP_POP_32_U			46
#define R_LARCH_ADD8				47
#define R_LARCH_ADD16				48
#define R_LARCH_ADD24				49
#define R_LARCH_ADD32				50
#define R_LARCH_ADD64				51
#define R_LARCH_SUB8				52
#define R_LARCH_SUB16				53
#define R_LARCH_SUB24				54
#define R_LARCH_SUB32				55
#define R_LARCH_SUB64				56
#define R_LARCH_GNU_VTINHERIT			57
#define R_LARCH_GNU_VTENTRY			58
#define R_LARCH_B16				64
#define R_LARCH_B21				65
#define R_LARCH_B26				66
#define R_LARCH_ABS_HI20			67
#define R_LARCH_ABS_LO12			68
#define R_LARCH_ABS64_LO20			69
#define R_LARCH_ABS64_HI12			70
#define R_LARCH_PCALA_HI20			71
#define R_LARCH_PCALA_LO12			72
#define R_LARCH_PCALA64_LO20			73
#define R_LARCH_PCALA64_HI12			74
#define R_LARCH_GOT_PC_HI20			75
#define R_LARCH_GOT_PC_LO12			76
#define R_LARCH_GOT64_PC_LO20			77
#define R_LARCH_GOT64_PC_HI12			78
#define R_LARCH_GOT_HI20			79
#define R_LARCH_GOT_LO12			80
#define R_LARCH_GOT64_LO20			81
#define R_LARCH_GOT64_HI12			82
#define R_LARCH_TLS_LE_HI20			83
#define R_LARCH_TLS_LE_LO12			84
#define R_LARCH_TLS_LE64_LO20			85
#define R_LARCH_TLS_LE64_HI12			86
#define R_LARCH_TLS_IE_PC_HI20			87
#define R_LARCH_TLS_IE_PC_LO12			88
#define R_LARCH_TLS_IE64_PC_LO20		89
#define R_LARCH_TLS_IE64_PC_HI12		90
#define R_LARCH_TLS_IE_HI20			91
#define R_LARCH_TLS_IE_LO12			92
#define R_LARCH_TLS_IE64_LO20			93
#define R_LARCH_TLS_IE64_HI12			94
#define R_LARCH_TLS_LD_PC_HI20			95
#define R_LARCH_TLS_LD_HI20			96
#define R_LARCH_TLS_GD_PC_HI20			97
#define R_LARCH_TLS_GD_HI20			98
#define R_LARCH_32_PCREL			99
#define R_LARCH_RELAX				100
#define R_LARCH_DELETE				101
#define R_LARCH_ALIGN				102
#define R_LARCH_PCREL20_S2			103
#define R_LARCH_CFA				104
#define R_LARCH_ADD6				105
#define R_LARCH_SUB6				106
#define R_LARCH_ADD_ULEB128			107
#define R_LARCH_SUB_ULEB128			108
#define R_LARCH_64_PCREL			109
#define R_LARCH_CALL36				110
#define R_LARCH_TLS_DESC_PC_HI20		111
#define R_LARCH_TLS_DESC_PC_LO12		112
#define R_LARCH_TLS_DESC64_PC_LO20		113
#define R_LARCH_TLS_DESC64_PC_HI12		114
#define R_LARCH_TLS_DESC_HI20			115
#define R_LARCH_TLS_DESC_LO12			116
#define R_LARCH_TLS_DESC64_LO20			117
#define R_LARCH_TLS_DESC64_HI12			118
#define R_LARCH_TLS_DESC_LD			119
#define R_LARCH_TLS_DESC_CALL			120
#define R_LARCH_TLS_LE_HI20_R			121
#define R_LARCH_TLS_LE_ADD_R			122
#define R_LARCH_TLS_LE_LO12_R			123
#define R_LARCH_TLS_LD_PCREL20_S2		124
#define R_LARCH_TLS_GD_PCREL20_S2		125
#define R_LARCH_TLS_DESC_PCREL20_S2		126
#define R_LARCH_CALL30				127
#define R_LARCH_PCADD_HI20			128
#define R_LARCH_PCADD_LO12			129
#define R_LARCH_GOT_PCADD_HI20			130
#define R_LARCH_GOT_PCADD_LO12			131
#define R_LARCH_TLS_IE_PCADD_HI20		132
#define R_LARCH_TLS_IE_PCADD_LO12		133
#define R_LARCH_TLS_LD_PCADD_HI20		134
#define R_LARCH_TLS_LD_PCADD_LO12		135
#define R_LARCH_TLS_GD_PCADD_HI20		136
#define R_LARCH_TLS_GD_PCADD_LO12		137
#define R_LARCH_TLS_DESC_PCADD_HI20		138
#define R_LARCH_TLS_DESC_PCADD_LO12		139

/* s390 relocations defined by the ABIs */
#define R_390_NONE		0	/* No reloc.  */
#define R_390_8			1	/* Direct 8 bit.  */
#define R_390_12		2	/* Direct 12 bit.  */
#define R_390_16		3	/* Direct 16 bit.  */
#define R_390_32		4	/* Direct 32 bit.  */
#define R_390_PC32		5	/* PC relative 32 bit.	*/
#define R_390_GOT12		6	/* 12 bit GOT offset.  */
#define R_390_GOT32		7	/* 32 bit GOT offset.  */
#define R_390_PLT32		8	/* 32 bit PC relative PLT address.  */
#define R_390_COPY		9	/* Copy symbol at runtime.  */
#define R_390_GLOB_DAT		10	/* Create GOT entry.  */
#define R_390_JMP_SLOT		11	/* Create PLT entry.  */
#define R_390_RELATIVE		12	/* Adjust by program base.  */
#define R_390_GOTOFF32		13	/* 32 bit offset to GOT.	 */
#define R_390_GOTPC		14	/* 32 bit PC rel. offset to GOT.  */
#define R_390_GOT16		15	/* 16 bit GOT offset.  */
#define R_390_PC16		16	/* PC relative 16 bit.	*/
#define R_390_PC16DBL		17	/* PC relative 16 bit shifted by 1.  */
#define R_390_PLT16DBL		18	/* 16 bit PC rel. PLT shifted by 1.  */
#define R_390_PC32DBL		19	/* PC relative 32 bit shifted by 1.  */
#define R_390_PLT32DBL		20	/* 32 bit PC rel. PLT shifted by 1.  */
#define R_390_GOTPCDBL		21	/* 32 bit PC rel. GOT shifted by 1.  */
#define R_390_64		22	/* Direct 64 bit.  */
#define R_390_PC64		23	/* PC relative 64 bit.	*/
#define R_390_GOT64		24	/* 64 bit GOT offset.  */
#define R_390_PLT64		25	/* 64 bit PC relative PLT address.  */
#define R_390_GOTENT		26	/* 32 bit PC rel. to GOT entry >> 1. */
#define R_390_GOTOFF16		27	/* 16 bit offset to GOT. */
#define R_390_GOTOFF64		28	/* 64 bit offset to GOT. */
#define R_390_GOTPLT12		29	/* 12 bit offset to jump slot.	*/
#define R_390_GOTPLT16		30	/* 16 bit offset to jump slot.	*/
#define R_390_GOTPLT32		31	/* 32 bit offset to jump slot.	*/
#define R_390_GOTPLT64		32	/* 64 bit offset to jump slot.	*/
#define R_390_GOTPLTENT		33	/* 32 bit rel. offset to jump slot.  */
#define R_390_PLTOFF16		34	/* 16 bit offset from GOT to PLT. */
#define R_390_PLTOFF32		35	/* 32 bit offset from GOT to PLT. */
#define R_390_PLTOFF64		36	/* 16 bit offset from GOT to PLT. */
#define R_390_TLS_LOAD		37	/* Tag for load insn in TLS code. */
#define R_390_TLS_GDCALL	38	/* Tag for function call in general
                                           dynamic TLS code.  */
#define R_390_TLS_LDCALL	39	/* Tag for function call in local
                                           dynamic TLS code.  */
#define R_390_TLS_GD32		40	/* Direct 32 bit for general dynamic
                                           thread local data.  */
#define R_390_TLS_GD64		41	/* Direct 64 bit for general dynamic
                                           thread local data.  */
#define R_390_TLS_GOTIE12	42	/* 12 bit GOT offset for static TLS
                                           block offset.  */
#define R_390_TLS_GOTIE32	43	/* 32 bit GOT offset for static TLS
                                           block offset.  */
#define R_390_TLS_GOTIE64	44	/* 64 bit GOT offset for static TLS
                                           block offset.  */
#define R_390_TLS_LDM32		45	/* Direct 32 bit for local dynamic
                                           thread local data in LD code.  */
#define R_390_TLS_LDM64		46	/* Direct 64 bit for local dynamic
                                           thread local data in LD code.  */
#define R_390_TLS_IE32		47	/* 32 bit address of GOT entry for
                                           negated static TLS block offset.  */
#define R_390_TLS_IE64		48	/* 64 bit address of GOT entry for
                                           negated static TLS block offset.  */
#define R_390_TLS_IEENT		49	/* 32 bit rel. offset to GOT entry for
                                           negated static TLS block offset.  */
#define R_390_TLS_LE32		50	/* 32 bit negated offset relative to
                                           static TLS block.  */
#define R_390_TLS_LE64		51	/* 64 bit negated offset relative to
                                           static TLS block.  */
#define R_390_TLS_LDO32		52	/* 32 bit offset relative to TLS
                                           block.  */
#define R_390_TLS_LDO64		53	/* 64 bit offset relative to TLS
                                           block.  */
#define R_390_TLS_DTPMOD	54	/* ID of module containing symbol.  */
#define R_390_TLS_DTPOFF	55	/* Offset in TLS block.  */
#define R_390_TLS_TPOFF		56	/* Negate offset in static TLS
                                           block.  */
#define R_390_20		57	/* Direct 20 bit.  */
#define R_390_GOT20		58	/* 20 bit GOT offset.  */
#define R_390_GOTPLT20		59	/* 20 bit offset to jump slot.  */
#define R_390_TLS_GOTIE20	60	/* 20 bit GOT offset for static TLS
					   block offset.  */
/* Keep this the last entry.  */
#define R_390_NUM	61

/* MIPS relocations defined by the ABIs */
#define R_MIPS_NONE		0
#define R_MIPS_16		1
#define R_MIPS_32		2
#define R_MIPS_REL32		3
#define R_MIPS_26		4
#define R_MIPS_HI16		5
#define R_MIPS_LO16		6
#define R_MIPS_GPREL16		7
#define R_MIPS_LITERAL		8
#define R_MIPS_GOT16		9
#define R_MIPS_PC16		10
#define R_MIPS_CALL16		11
#define R_MIPS_GPREL32		12
/* The remaining relocs are defined on Irix, although they are not
   in the MIPS ELF ABI.	 */
#define R_MIPS_UNUSED1		13
#define R_MIPS_UNUSED2		14
#define R_MIPS_UNUSED3		15
#define R_MIPS_SHIFT5		16
#define R_MIPS_SHIFT6		17
#define R_MIPS_64		18
#define R_MIPS_GOT_DISP		19
#define R_MIPS_GOT_PAGE		20
#define R_MIPS_GOT_OFST		21
/*
 * The following two relocation types are specified in the MIPS ABI
 * conformance guide version 1.2 but not yet in the psABI.
 */
#define R_MIPS_GOTHI16		22
#define R_MIPS_GOTLO16		23
#define R_MIPS_SUB		24
#define R_MIPS_INSERT_A		25
#define R_MIPS_INSERT_B		26
#define R_MIPS_DELETE		27
#define R_MIPS_HIGHER		28
#define R_MIPS_HIGHEST		29
/*
 * The following two relocation types are specified in the MIPS ABI
 * conformance guide version 1.2 but not yet in the psABI.
 */
#define R_MIPS_CALLHI16		30
#define R_MIPS_CALLLO16		31
/*
 * Introduced for MIPSr6.
 */
#define R_MIPS_PC21_S2		60
#define R_MIPS_PC26_S2		61
/*
 * This range is reserved for vendor specific relocations.
 */
#define R_MIPS_LOVENDOR		100
#define R_MIPS_HIVENDOR		127

#define R_MIPS_PC32		248

#endif /* _UAPI_LINUX_ELF_R_H */

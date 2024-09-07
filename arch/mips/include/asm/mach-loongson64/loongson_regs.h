/*
 * Read/Write Loongson Extension Registers
 */

#ifndef _LOONGSON_REGS_H_
#define _LOONGSON_REGS_H_

#include <linux/bits.h>
#include <linux/types.h>

#include <asm/mipsregs.h>
#include <asm/cpu.h>

#ifndef __ASSEMBLY__
static inline bool cpu_has_cfg(void)
{
	return ((read_c0_prid() & PRID_IMP_MASK) == PRID_IMP_LOONGSON_64G);
}

static inline u32 read_cpucfg(u32 reg)
{
	u32 __res;

	__asm__ __volatile__(
		_ASM_SET_PARSE_R
		"parse_r __res,%0\n\t"
		"parse_r reg,%1\n\t"
		_ASM_UNSET_PARSE_R
		".insn \n\t"
		".word (0xc8080118 | (reg << 21) | (__res << 11))\n\t"
		:"=r"(__res)
		:"r"(reg)
		:
		);
	return __res;
}
#endif

/* Bit Domains for CFG registers */
#define LOONGSON_CFG0	0x0
#define LOONGSON_CFG0_PRID GENMASK(31, 0)

#define LOONGSON_CFG1 0x1
#define LOONGSON_CFG1_FP	BIT(0)
#define LOONGSON_CFG1_FPREV	GENMASK(3, 1)
#define LOONGSON_CFG1_MMI	BIT(4)
#define LOONGSON_CFG1_MSA1	BIT(5)
#define LOONGSON_CFG1_MSA2	BIT(6)
#define LOONGSON_CFG1_CGP	BIT(7)
#define LOONGSON_CFG1_WRP	BIT(8)
#define LOONGSON_CFG1_LSX1	BIT(9)
#define LOONGSON_CFG1_LSX2	BIT(10)
#define LOONGSON_CFG1_LASX	BIT(11)
#define LOONGSON_CFG1_R6FXP	BIT(12)
#define LOONGSON_CFG1_R6CRCP	BIT(13)
#define LOONGSON_CFG1_R6FPP	BIT(14)
#define LOONGSON_CFG1_CNT64	BIT(15)
#define LOONGSON_CFG1_LSLDR0	BIT(16)
#define LOONGSON_CFG1_LSPREF	BIT(17)
#define LOONGSON_CFG1_LSPREFX	BIT(18)
#define LOONGSON_CFG1_LSSYNCI	BIT(19)
#define LOONGSON_CFG1_LSUCA	BIT(20)
#define LOONGSON_CFG1_LLSYNC	BIT(21)
#define LOONGSON_CFG1_TGTSYNC	BIT(22)
#define LOONGSON_CFG1_LLEXC	BIT(23)
#define LOONGSON_CFG1_SCRAND	BIT(24)
#define LOONGSON_CFG1_MUALP	BIT(25)
#define LOONGSON_CFG1_KMUALEN	BIT(26)
#define LOONGSON_CFG1_ITLBT	BIT(27)
#define LOONGSON_CFG1_LSUPERF	BIT(28)
#define LOONGSON_CFG1_SFBP	BIT(29)
#define LOONGSON_CFG1_CDMAP	BIT(30)

#define LOONGSON_CFG1_FPREV_OFFSET	1

#define LOONGSON_CFG2 0x2
#define LOONGSON_CFG2_LEXT1	BIT(0)
#define LOONGSON_CFG2_LEXT2	BIT(1)
#define LOONGSON_CFG2_LEXT3	BIT(2)
#define LOONGSON_CFG2_LSPW	BIT(3)
#define LOONGSON_CFG2_LBT1	BIT(4)
#define LOONGSON_CFG2_LBT2	BIT(5)
#define LOONGSON_CFG2_LBT3	BIT(6)
#define LOONGSON_CFG2_LBTMMU	BIT(7)
#define LOONGSON_CFG2_LPMP	BIT(8)
#define LOONGSON_CFG2_LPMREV	GENMASK(11, 9)
#define LOONGSON_CFG2_LAMO	BIT(12)
#define LOONGSON_CFG2_LPIXU	BIT(13)
#define LOONGSON_CFG2_LPIXNU	BIT(14)
#define LOONGSON_CFG2_LVZP	BIT(15)
#define LOONGSON_CFG2_LVZREV	GENMASK(18, 16)
#define LOONGSON_CFG2_LGFTP	BIT(19)
#define LOONGSON_CFG2_LGFTPREV	GENMASK(22, 20)
#define LOONGSON_CFG2_LLFTP	BIT(23)
#define LOONGSON_CFG2_LLFTPREV	GENMASK(26, 24)
#define LOONGSON_CFG2_LCSRP	BIT(27)
#define LOONGSON_CFG2_LDISBLIKELY	BIT(28)

#define LOONGSON_CFG2_LPMREV_OFFSET	9
#define LOONGSON_CFG2_LPM_REV1		(1 << LOONGSON_CFG2_LPMREV_OFFSET)
#define LOONGSON_CFG2_LPM_REV2		(2 << LOONGSON_CFG2_LPMREV_OFFSET)
#define LOONGSON_CFG2_LVZREV_OFFSET	16
#define LOONGSON_CFG2_LVZ_REV1		(1 << LOONGSON_CFG2_LVZREV_OFFSET)
#define LOONGSON_CFG2_LVZ_REV2		(2 << LOONGSON_CFG2_LVZREV_OFFSET)

#define LOONGSON_CFG3 0x3
#define LOONGSON_CFG3_LCAMP	BIT(0)
#define LOONGSON_CFG3_LCAMREV	GENMASK(3, 1)
#define LOONGSON_CFG3_LCAMNUM	GENMASK(11, 4)
#define LOONGSON_CFG3_LCAMKW	GENMASK(19, 12)
#define LOONGSON_CFG3_LCAMVW	GENMASK(27, 20)

#define LOONGSON_CFG3_LCAMREV_OFFSET	1
#define LOONGSON_CFG3_LCAM_REV1		(1 << LOONGSON_CFG3_LCAMREV_OFFSET)
#define LOONGSON_CFG3_LCAM_REV2		(2 << LOONGSON_CFG3_LCAMREV_OFFSET)
#define LOONGSON_CFG3_LCAMNUM_OFFSET	4
#define LOONGSON_CFG3_LCAMNUM_REV1	(0x3f << LOONGSON_CFG3_LCAMNUM_OFFSET)
#define LOONGSON_CFG3_LCAMKW_OFFSET	12
#define LOONGSON_CFG3_LCAMKW_REV1	(0x27 << LOONGSON_CFG3_LCAMKW_OFFSET)
#define LOONGSON_CFG3_LCAMVW_OFFSET	20
#define LOONGSON_CFG3_LCAMVW_REV1	(0x3f << LOONGSON_CFG3_LCAMVW_OFFSET)

#define LOONGSON_CFG4 0x4
#define LOONGSON_CFG4_CCFREQ	GENMASK(31, 0)

#define LOONGSON_CFG5 0x5
#define LOONGSON_CFG5_CFM	GENMASK(15, 0)
#define LOONGSON_CFG5_CFD	GENMASK(31, 16)

#define LOONGSON_CFG6 0x6

#define LOONGSON_CFG7 0x7
#define LOONGSON_CFG7_GCCAEQRP	BIT(0)
#define LOONGSON_CFG7_UCAWINP	BIT(1)

#ifndef __ASSEMBLY__
static inline bool cpu_has_csr(void)
{
	if (cpu_has_cfg())
		return (read_cpucfg(LOONGSON_CFG2) & LOONGSON_CFG2_LCSRP);

	return false;
}

#define cpu_has_iocsr()	cpu_has_csr()

/* For MIPS/Loongson IOCSR and Core CSR are accessed same way */
static inline u32 csr_read32(u32 reg)
{
	u32 __res;

	/* RDCSR reg, val */
	__asm__ __volatile__(
		_ASM_SET_PARSE_R
		"parse_r __res,%0\n\t"
		"parse_r reg,%1\n\t"
		_ASM_UNSET_PARSE_R
		".insn \n\t"
		".word (0xc8000118 | (reg << 21) | (__res << 11))\n\t"
		:"=r"(__res)
		:"r"(reg)
		:
		);
	return __res;
}

static inline u64 csr_read64(u32 reg)
{
	u64 __res;

	/* DRDCSR reg, val */
	__asm__ __volatile__(
		_ASM_SET_PARSE_R
		"parse_r __res,%0\n\t"
		"parse_r reg,%1\n\t"
		_ASM_UNSET_PARSE_R
		".insn \n\t"
		".word (0xc8020118 | (reg << 21) | (__res << 11))\n\t"
		:"=r"(__res)
		:"r"(reg)
		:
		);
	return __res;
}

static inline void csr_write32(u32 val, u32 reg)
{
	/* WRCSR reg, val */
	__asm__ __volatile__(
		_ASM_SET_PARSE_R
		"parse_r reg,%0\n\t"
		"parse_r val,%1\n\t"
		_ASM_UNSET_PARSE_R
		".insn \n\t"
		".word (0xc8010118 | (reg << 21) | (val << 11))\n\t"
		:
		:"r"(reg),"r"(val)
		:
		);
}

static inline void csr_write64(u64 val, u32 reg)
{
	/* DWRCSR reg, val */
	__asm__ __volatile__(
		_ASM_SET_PARSE_R
		"parse_r reg,%0\n\t"
		"parse_r val,%1\n\t"
		_ASM_UNSET_PARSE_R
		".insn \n\t"
		".word (0xc8030118 | (reg << 21) | (val << 11))\n\t"
		:
		:"r"(reg),"r"(val)
		:
		);
}

#define iocsr_read32(reg)	csr_read32(reg)
#define iocsr_read64(reg)	csr_read64(reg)
#define iocsr_write32(val, reg)	csr_write32(val, reg)
#define iocsr_write64(val, reg)	csr_write64(val, reg)

static inline u64 drdtime(void)
{
	int rID = 0;
	u64 val = 0;

	__asm__ __volatile__(
		_ASM_SET_PARSE_R
		"parse_r rID,%0\n\t"
		"parse_r val,%1\n\t"
		_ASM_UNSET_PARSE_R
		".insn \n\t"
		".word (0xc8090118 | (rID << 21) | (val << 11))\n\t"
		:"=r"(rID),"=r"(val)
		:
		);
	return val;
}
#endif

#endif

/* SPDX-License-Identifier: GPL-2.0 */
/*
 * SiTime SiT9531x register definitions
 *
 * Copyright (C) 2026 SiTime Corp.
 * Author: Ali Rouhi <arouhi@sitime.com>
 * Author: Oleg Zadorozhnyi <Oleg.Zadorozhnyi@devoxsoftware.com>
 */

#ifndef _SIT9531X_REGS_H
#define _SIT9531X_REGS_H

/*
 * I2C register model:
 *   - Page select register at offset 0x01
 *   - Each page has 128 registers (0x00-0x7F)
 *   - Some pages are paired (e.g. 0x0A/0x1A for PLLA)
 */
#define SIT9531X_PAGE_SEL		0xFF
#define SIT9531X_PAGE_SIZE		0x100
#define SIT9531X_NUM_PAGES		32

/* Helper macros for page:offset addressing */
#define SIT9531X_REG(_page, _offset)		(((_page) << 8) | (_offset))
#define SIT9531X_REG_PAGE(_reg)		((_reg) >> 8)
#define SIT9531X_REG_OFFSET(_reg)		((_reg) & 0xFF)

/* ---- Page definitions ---- */
#define SIT9531X_PAGE_MAINSYS0		0x00
#define SIT9531X_PAGE_MAINSYS1		0x01
#define SIT9531X_PAGE_INPUTSYS		0x02
#define SIT9531X_PAGE_OUTSYS0		0x03
#define SIT9531X_PAGE_OUTSYS1		0x04
#define SIT9531X_PAGE_CLKMON0		0x06
#define SIT9531X_PAGE_CLKMON1		0x07
#define SIT9531X_PAGE_PLLA			0x0A
#define SIT9531X_PAGE_PLLA_EXT		0x1A
#define SIT9531X_PAGE_PLLB			0x0B
#define SIT9531X_PAGE_PLLB_EXT		0x1B
#define SIT9531X_PAGE_PLLC			0x0C
#define SIT9531X_PAGE_PLLC_EXT		0x1C
#define SIT9531X_PAGE_PLLD			0x0D
#define SIT9531X_PAGE_PLLD_EXT		0x1D

/* PLL index to page mapping */
#define SIT9531X_PLL_PAGE(_idx) \
	(SIT9531X_PAGE_PLLA + (_idx))

/* ---- Page 0x00 (Main System) registers ---- */
/*
 * VARIANT_ID is a single byte at page 0 reg 0x02 (95317 = 0x17, 95316 = 0x31).
 * Reg 0x03 carries an unrelated revision byte and must not be combined into
 * the variant identifier.
 */
#define SIT9531X_REG_VARIANT_ID		SIT9531X_REG(0x00, 0x02)
#define SIT9531X_REG_LOS_STATUS		SIT9531X_REG(0x00, 0x04)
#define SIT9531X_REG_OOF_STATUS		SIT9531X_REG(0x00, 0x05)
#define SIT9531X_REG_HOLDOVER_STATUS		SIT9531X_REG(0x00, 0x06)
#define SIT9531X_REG_SYNC_STATUS		SIT9531X_REG(0x00, 0x07)
#define SIT9531X_REG_STATUS_1		SIT9531X_REG(0x00, 0x06)
#define SIT9531X_REG_STATUS_2		SIT9531X_REG(0x00, 0x0A)

/* DCO trigger register (Page 0x00) */
#define SIT9531X_REG_DCO_TRIGGER		SIT9531X_REG(0x00, 0x64)
#define SIT9531X_DCO_TRIGGER_INCR		BIT(6)
#define SIT9531X_DCO_TRIGGER_DECR		BIT(4)
#define SIT9531X_DCO_TRIGGER_BASE		0xAE

/* DCO trigger pulse timing: minimum 6 ns required by hardware */
#define SIT9531X_DCO_TRIGGER_PULSE_NS		167

#define SIT9531X_REG_HOLDOVER_HISTORY	SIT9531X_REG(0x00, 0x58)

/* Page 0 -- PLL inner loop loss-of-lock */
#define SIT9531X_REG_PLL_INNER_LOL_STATUS	SIT9531X_REG(0x00, 0x92)
#define SIT9531X_REG_PLL_INNER_LOL_NOTIF	SIT9531X_REG(0x00, 0x93)

/* Page 0 -- Clock monitor PLL / XO status */
#define SIT9531X_REG_CMON_STATUS		SIT9531X_REG(0x00, 0x9D)
#define SIT9531X_REG_CMON_NOTIF		SIT9531X_REG(0x00, 0x9E)
#define SIT9531X_CMON_XO_LOSS		BIT(4)
#define SIT9531X_CMON_PLL_INNER_LOL		BIT(5)

/* Page 0 -- PLL outer-loop loss-of-lock */
#define SIT9531X_REG_OUTER_LOL_STATUS	SIT9531X_REG(0x00, 0x06)
#define SIT9531X_REG_OUTER_LOL_NOTIF		SIT9531X_REG(0x00, 0x07)

/* Page 0 -- PLL holdover freeze status */
#define SIT9531X_REG_HO_FREEZE_STATUS	SIT9531X_REG(0x00, 0x0A)
#define SIT9531X_REG_HO_FREEZE_NOTIF	SIT9531X_REG(0x00, 0x0B)

/* Page 0 -- INTSYNC (inter-PLL synchronization) global enable */
#define SIT9531X_REG_INTSYNC_GLOBAL		SIT9531X_REG(0x00, 0x40)
#define SIT9531X_INTSYNC_EN_BIT		6

/* ---- Page 0x01 (Input Priority Table) registers ---- */
/*
 * Priority table: 6 registers per PLL, each holds two priority slots
 * nibble-packed (even slot in [3:0], odd slot in [7:4]).
 *
 * Base registers for PLLA: 0x16-0x1B (slots 0-11).
 * For PLL N:  base + 6 * N  (e.g. PLLB starts at 0x1C).
 *
 * Input source encoding (4-bit value):
 *   0=IN0P, 1=IN1P, 2=IN2P, 3=IN3P, 4=IN4P,
 *   5=OCXO, 6=INTSYNC,
 *   7=IN0N, 8=IN1N, 9=IN2N, 10=IN3N, 11=IN4N
 */
#define SIT9531X_PAGE_PRIOSYS		0x01
#define SIT9531X_PRIO_BASE_REG		0x16
#define SIT9531X_PRIO_REGS_PER_PLL		6
#define SIT9531X_PRIO_SLOTS_PER_REG		2
#define SIT9531X_PRIO_MAX_SLOTS		12
#define SIT9531X_PRIO_NIBBLE_MASK		0x0F
#define SIT9531X_PRIO_HI_SHIFT		4

/* Page 0 -- Global update register (PRG_CMD / NVM / loop lock) */
#define SIT9531X_REG_GLOBAL_UPDATE		SIT9531X_REG(0x00, 0x0F)

/* PLL holdover control (PLL page offset) */
#define SIT9531X_PLL_REG_HO_CTRL		0x6F
#define SIT9531X_PLL_HO_FORCE_BIT		4

/* ---- Page 0x02 (Input System) -- input disable control ---- */
#define SIT9531X_REG_IN_DE_FORCE		SIT9531X_REG(0x02, 0xE8)
#define SIT9531X_REG_IN_DE_STATE		SIT9531X_REG(0x02, 0xE9)
#define SIT9531X_REG_IN_SEP_FORCE		SIT9531X_REG(0x02, 0xEA)
#define SIT9531X_REG_IN_SEP_STATE		SIT9531X_REG(0x02, 0xEB)
#define SIT9531X_REG_IN_SEN_FORCE		SIT9531X_REG(0x02, 0xF2)
#define SIT9531X_REG_IN_SEN_STATE		SIT9531X_REG(0x02, 0xF3)

/* ---- Page 0x03 (Output System) registers -- Hi-Z control ---- */
#define SIT9531X_REG_HIZ_DIFF_07_MASK	SIT9531X_REG(0x03, 0xF2)
#define SIT9531X_REG_HIZ_DIFF_07_STATE	SIT9531X_REG(0x03, 0xF3)
#define SIT9531X_REG_HIZ_DIFF_811_MASK	SIT9531X_REG(0x03, 0xF4)
#define SIT9531X_REG_HIZ_DIFF_811_STATE	SIT9531X_REG(0x03, 0xF5)
#define SIT9531X_REG_HIZ_SE_811_OE_MASK	SIT9531X_REG(0x03, 0xF4)
#define SIT9531X_REG_HIZ_SE_811_OE_STATE	SIT9531X_REG(0x03, 0xF5)
#define SIT9531X_REG_HIZ_SE_07_OE_MASK	SIT9531X_REG(0x03, 0xF6)
#define SIT9531X_REG_HIZ_SE_07_OE_STATE	SIT9531X_REG(0x03, 0xF7)
#define SIT9531X_REG_HIZ_SE_07_MASK		SIT9531X_REG(0x03, 0xF8)
#define SIT9531X_REG_HIZ_SE_07_STATE		SIT9531X_REG(0x03, 0xF9)
#define SIT9531X_REG_HIZ_SE_811_MASK		SIT9531X_REG(0x03, 0xFA)
#define SIT9531X_REG_HIZ_SE_811_STATE	SIT9531X_REG(0x03, 0xFB)

/* ---- Pages 0x03/0x04 -- Output divider (DIVO) registers ---- */
/*
 * Output divider registers in Pages 3/4.  Each output has a 34-bit
 * integer divider mapped to 5 bytes (LSB at base reg, MSB at base-4).
 * Outputs 0-5 are on Page 3, outputs 6-11 are on Page 4.
 *
 * The base register for slot N within a page is:
 *   clkout_odr_divn_base[slot] = { 0x14, 0x24, 0x34, 0x44, 0x54, 0x64 }
 *
 * Layout: base=LSB, base-1, base-2, base-3, base-4[1:0]=MSB.
 *
 * Per-chip clkout_map[] translates output index to slot position.
 */
#define SIT9531X_PAGE_OUTSYS0_SLOT_MAX	5   /* slots 0-5 on Page 0x03 */

/* Misc output system registers */
#define SIT9531X_REG_PRG_DIR_GEN		SIT9531X_REG(0x03, 0x0F)
#define SIT9531X_PRG_CMD_STATE		0x01
#define SIT9531X_UPDATE_NVM			0x10
#define SIT9531X_LOOP_LOCK			0x40

/* Debug register (same offset, per-page) */
#define SIT9531X_REG_OUTSYS_DEBUG		SIT9531X_REG(0x03, 0xBD)
#define SIT9531X_DEBUG_UNLOCK_VAL		0xC3

/* ---- Pages 0x03/0x04 -- Output PRG_RST_DELAY (per-output phase delay) ---- */
/*
 * Per-output programmable phase delay: 34-bit coarse (in VCO clock
 * cycles) plus a 3-bit fine field with fixed 30 ps steps.  Each output
 * has a five-byte block PROG6..PROG2:
 *
 *   base + 0  PROG6  [7:5] OPSTG_VCASC_BUMP (preserve via RMW)
 *                    [4:2] PRG_RST_FINE_DELAY[2:0]
 *                    [1:0] PRG_RST_DELAY[33:32]
 *   base + 1  PROG5  [7:0] PRG_RST_DELAY[31:24]
 *   base + 2  PROG4  [7:0] PRG_RST_DELAY[23:16]
 *   base + 3  PROG3  [7:0] PRG_RST_DELAY[15:8]
 *   base + 4  PROG2  [7:0] PRG_RST_DELAY[7:0]
 *
 * Outputs 0-5 are on Page 3, outputs 6-11 on Page 4.  The block base
 * within a page is 0x15 + 16 * (out_idx % 6).
 */
#define SIT9531X_OUT_PRG_DELAY_BASE		0x15
#define SIT9531X_OUT_PRG_SLOT_STRIDE		0x10
#define SIT9531X_OUT_PRG_OPSTG_MASK		0xE0	/* bits [7:5], preserve */
#define SIT9531X_OUT_PRG_FINE_SHIFT		2
#define SIT9531X_OUT_PRG_FINE_MASK		0x1C	/* bits [4:2] */
#define SIT9531X_OUT_PRG_COARSE_HI_MASK		0x03	/* bits [1:0] */
#define SIT9531X_OUT_PRG_FINE_STEP_PS		30
#define SIT9531X_OUT_PRG_FINE_MAX		7	/* 3-bit field */
#define SIT9531X_OUT_PRG_COARSE_BITS		34

/* ---- Pages 0x03/0x04 -- Output PROG0 (PULSE_CTRL, 8-bit) ---- */
/*
 * Per-output pulse-count control byte used in SYSREF / SYNCB modes.
 * Slot N within a page sits at 0x1B + 16 * (slot % 6).  Same page
 * mapping as PRG_RST_DELAY: slots 0-5 on Page 3, slots 6-11 on Page 4.
 */
#define SIT9531X_OUT_PROG0_BASE		0x1B

/* ---- Page 0 -- per-PLL DIVO trigger enables (NVMSPARE1_GENERIC, 0x19) ---- */
/*
 * One bit per PLL (A=0, B=1, C=2, D=3) for the small-change (SYSREF
 * trigger) path; bit n+4 enables the large-change (DIVO restart)
 * path for the same PLL.  See SiT95316 register map p.6.
 */
#define SIT9531X_REG_DIVO_TRIGGER_EN	SIT9531X_REG(0x00, 0x19)
#define SIT9531X_DIVO_SYSREF_TRIG_BIT(_pll)	(_pll)
#define SIT9531X_DIVO_LARGE_TRIG_BIT(_pll)	((_pll) + 4)

/* ---- PLL_CONFIG47_PLL (per PLL page reg 0x47) ---- */
/*
 * Mode bits 6/5/4 select the SYSREF/SYNCB/PULSER variants; bits 3:0
 * carry DIVO_SYS_REF[11:8] of the 12-bit one-hot output select that
 * continues into reg 0x48[7:0].  See register map p.84.
 */
#define SIT9531X_PLL_REG_SYSREF_MODE	0x47
#define SIT9531X_PLL_SYSREF_PULSER_BIT	BIT(6)
#define SIT9531X_PLL_SYSREF_MODE_BIT	BIT(5)
#define SIT9531X_PLL_SYSREF_SYNCB_BIT	BIT(4)
#define SIT9531X_PLL_SYSREF_MODE_MASK	(SIT9531X_PLL_SYSREF_PULSER_BIT | \
					 SIT9531X_PLL_SYSREF_MODE_BIT | \
					 SIT9531X_PLL_SYSREF_SYNCB_BIT)
#define SIT9531X_PLL_SYSREF_TARGET_HI_MASK	0x0F
#define SIT9531X_PLL_REG_SYSREF_SEL	0x48

/* ---- PLL page registers (apply to pages 0x0A-0x0D) ---- */
#define SIT9531X_PLL_REG_SMALL_UPDATE	0x0F

/*
 * Loop-filter coefficients on PLL_PAGE regs 0x10-0x15 (3 normal +
 * 3 fast-lock) are GUI/NVM-generated by the timing configurator and must not be
 * reprogrammed at runtime; the register map flags them as
 * "GUI generated configuration should not change manually".
 */

#define SIT9531X_PLL_REG_OUT_MAP_HI		0x27
#define SIT9531X_PLL_REG_OUT_MAP_LO		0x28
#define SIT9531X_PLL_REG_INPUT_SEL		0x29
/*
 * LL_REG2_PLL -- lock-detection thresholds (PDF p.80):
 *   bits [7:4] LL_SET_VALUE_PLL[3:0]  outer-loop unlock threshold
 *   bits [3:0] LL_CLR_VALUE_PLL[3:0]  outer-loop relock  threshold
 * 16-step ladder spans 0.05 PPB to 4000 PPM.
 */
#define SIT9531X_PLL_REG_LL_THRESH		0x2A
#define SIT9531X_PLL_REG_STATUS		0x31
#define SIT9531X_PLL_REG_NVM_UPDATE		0x3F

/* DIVN registers (free-run divider readback) */
#define SIT9531X_PLL_REG_DIVN_INT		0x30
#define SIT9531X_PLL_REG_DIVN_NUM		0x32  /* 4 bytes (0x32-0x35) */
#define SIT9531X_PLL_REG_DIVN_DEN		0x38  /* 4 bytes (0x38-0x3B) */

/* DIVN2 registers (sync divider readback) */
#define SIT9531X_PLL_REG_DIVN2_INT		0x3E  /* 5 bytes (0x3E-0x42) */
#define SIT9531X_PLL_REG_DIVN2_FRAC_NUM	0x43  /* 4 bytes (0x43-0x46) */
#define SIT9531X_PLL_REG_DIVN2_FRAC_DEN	0x49  /* 4 bytes (0x49-0x4C) */

/* Inner loop DCO word registers (48-bit fractional) */
#define SIT9531X_PLL_REG_DCO_FRAC1		0x51
#define SIT9531X_PLL_REG_DCO_FRAC2		0x52
#define SIT9531X_PLL_REG_DCO_FRAC3		0x53
#define SIT9531X_PLL_REG_DCO_FRAC4		0x54
#define SIT9531X_PLL_REG_DCO_FRAC5		0x55
#define SIT9531X_PLL_REG_DCO_FRAC6		0x56

/* DCO function register */
#define SIT9531X_PLL_REG_DCO_FUNC		0x57
#define SIT9531X_DCO_MASK			BIT(0)
#define SIT9531X_DCO_EN			BIT(1)
#define SIT9531X_DCO_OUTER_EN		BIT(4)
#define SIT9531X_DCO_DITHER_MODE		BIT(6)

/* Outer loop DCO integer registers (24-bit) */
#define SIT9531X_PLL_REG_DCO_INT_7		0x5E
#define SIT9531X_PLL_REG_DCO_INT_15		0x5F
#define SIT9531X_PLL_REG_DCO_INT_23		0x60

/* Outer loop DCO fractional registers (32-bit, shifted <<16) */
#define SIT9531X_PLL_REG_DCO_OFRAC_7	0x63
#define SIT9531X_PLL_REG_DCO_OFRAC_15	0x64
#define SIT9531X_PLL_REG_DCO_OFRAC_23	0x65
#define SIT9531X_PLL_REG_DCO_OFRAC_31	0x66

/* Debug register unlock */
#define SIT9531X_PLL_REG_DEBUG		0xBD
#define SIT9531X_PLL_DEBUG_UNLOCK		0xC3

/* TDC (Time-to-Digital Converter) phase measurement -- PLL page */
#define SIT9531X_PLL_REG_TDC_CFG		0xB3
#define SIT9531X_PLL_REG_TDC_MODE		0xB4
#define SIT9531X_TDC_MODE_ENABLE		0x80
#define SIT9531X_TDC_CFG_DEFAULT		69
#define SIT9531X_PLL_REG_TDC_DATA_0		0xB5  /* [7:0] */
#define SIT9531X_PLL_REG_TDC_DATA_1		0xB6  /* [15:8] */
#define SIT9531X_PLL_REG_TDC_DATA_2		0xB7  /* [23:16] */
#define SIT9531X_PLL_REG_TDC_DATA_3		0xB8  /* [31:24] */
#define SIT9531X_PLL_REG_TDC_DATA_4		0xB9  /* [39:32] + sign */
#define SIT9531X_TDC_SIGN_BIT		3
#define SIT9531X_PLL_REG_TDC_TRIGGER		0xD0  /* read to latch TDC sample */

/* PLL EXT page INTSYNC configuration registers */
#define SIT9531X_PLL_EXT_PAGE(_idx)		(SIT9531X_PAGE_PLLA_EXT + (_idx))

/* PLL STATUS register bits */
#define SIT9531X_PLL_STATUS_LOCK		BIT(0)
#define SIT9531X_PLL_STATUS_OUTER_DIS	BIT(5)

/* Small update command */
#define SIT9531X_PLL_SMALL_UPDATE_CMD	0x02

/* ---- Clock monitor registers (Page 0x06) ---- */
/*
 * Per-input clock monitor status registers.  Each register holds
 * status for two inputs (even input in bits [3:0], odd in [7:4]).
 * P-polarity and N-polarity inputs have separate register banks.
 *
 * Bit layout per input nibble:
 *   [0] freq_fine_drifted   -- fine frequency drift detected
 *   [1] freq_coarse_drifted -- coarse frequency drift detected
 *   [2] clk_loss            -- clock input loss (LOS)
 *   [3] clk_loss_fd         -- clock input loss with freq drift
 *
 * Status register is at base offset, notification at base+1.
 */

/* P-polarity status registers */
#define SIT9531X_CLKMON_P_STATUS_01		SIT9531X_REG(0x06, 0x02)  /* inputs 0,1 */
#define SIT9531X_CLKMON_P_NOTIF_01		SIT9531X_REG(0x06, 0x03)
#define SIT9531X_CLKMON_P_STATUS_23		SIT9531X_REG(0x06, 0x06)  /* inputs 2,3 */
#define SIT9531X_CLKMON_P_NOTIF_23		SIT9531X_REG(0x06, 0x07)

/* N-polarity status registers */
#define SIT9531X_CLKMON_N_STATUS_01		SIT9531X_REG(0x06, 0x92)  /* inputs 0,1 */
#define SIT9531X_CLKMON_N_NOTIF_01		SIT9531X_REG(0x06, 0x93)
#define SIT9531X_CLKMON_N_STATUS_23		SIT9531X_REG(0x06, 0x96)  /* inputs 2,3 */
#define SIT9531X_CLKMON_N_NOTIF_23		SIT9531X_REG(0x06, 0x97)

/* Per-input bit offsets within clock monitor nibble */
#define SIT9531X_CLKMON_FREQ_FINE		0  /* bit 0 / bit 4 */
#define SIT9531X_CLKMON_FREQ_COARSE		1  /* bit 1 / bit 5 */
#define SIT9531X_CLKMON_CLK_LOSS		2  /* bit 2 / bit 6 */
#define SIT9531X_CLKMON_CLK_LOSS_FD		3  /* bit 3 / bit 7 */

/* ---- Debug / NVM unlock registers ---- */
#define SIT9531X_REG_DBG_UNLOCK1		0x24
#define SIT9531X_REG_DBG_UNLOCK2		0x25

/* ---- Variant ID values (single byte read from SIT9531X_REG_VARIANT_ID) ---- */
#define SIT9531X_VARIANT_ID_95317	0x17
#define SIT9531X_VARIANT_ID_95316	0x31

#endif /* _SIT9531X_REGS_H */

// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 Stefan Dösinger
 */
#include <dt-bindings/clock/zte,zx297520v3-clk.h>
#include <linux/reset-controller.h>
#include <linux/platform_device.h>
#include <linux/clk-provider.h>
#include <linux/of_address.h>
#include <linux/reboot.h>
#include <linux/iopoll.h>
#include <linux/delay.h>
#include <linux/clk.h>
#include <linux/io.h>

#include "pll.h"

/* All LSP and some Matrix registers contain both resets and clock gates, so access to them needs
 * to be synchronized between the reset and clock callbacks.
 */
static DEFINE_SPINLOCK(reg_lock);

struct zx29_reset_reg {
	void __iomem *reg;
	u32 mask, wait_mask;
};

struct zx29_clk_controller {
	struct clk_hw_onecell_data *clocks;
	struct reset_controller_dev rcdev;
	struct zx29_reset_reg resets[];
};

static int __zx297520v3_rst_assert(struct reset_controller_dev *rcdev, unsigned long id)
{
	struct zx29_clk_controller *data = container_of(rcdev, struct zx29_clk_controller,
							    rcdev);
	u32 val;

	val = readl(data->resets[id].reg);
	val &= ~data->resets[id].mask;
	writel(val, data->resets[id].reg);

	return 0;
}

static int zx297520v3_rst_assert(struct reset_controller_dev *rcdev, unsigned long id)
{
	unsigned long flags;
	int res;

	spin_lock_irqsave(&reg_lock, flags);
	res = __zx297520v3_rst_assert(rcdev, id);
	spin_unlock_irqrestore(&reg_lock, flags);

	return res;
}

static int __zx297520v3_rst_deassert(struct reset_controller_dev *rcdev, unsigned long id)
{
	struct zx29_clk_controller *data = container_of(rcdev, struct zx29_clk_controller,
							    rcdev);
	u32 val;

	val = readl(data->resets[id].reg);
	val |= data->resets[id].mask;
	writel(val, data->resets[id].reg);

	/* This is a special case used only by USB reset */
	if (data->resets[id].wait_mask) {
		return readl_poll_timeout(data->resets[id].reg + 4, val,
					  val & data->resets[id].wait_mask, 1, 100);
	}

	return 0;
}

static int zx297520v3_rst_deassert(struct reset_controller_dev *rcdev, unsigned long id)
{
	unsigned long flags;
	int res;

	spin_lock_irqsave(&reg_lock, flags);
	res = __zx297520v3_rst_deassert(rcdev, id);
	spin_unlock_irqrestore(&reg_lock, flags);

	return res;
}

static int zx297520v3_rst_reset(struct reset_controller_dev *rcdev, unsigned long id)
{
	unsigned long flags;
	int res;

	spin_lock_irqsave(&reg_lock, flags);

	res = __zx297520v3_rst_assert(rcdev, id);
	if (res)
		goto unlock;
	udelay(100);
	res = __zx297520v3_rst_deassert(rcdev, id);

unlock:
	spin_unlock_irqrestore(&reg_lock, flags);
	return res;
}

static int zx297520v3_rst_status(struct reset_controller_dev *rcdev,
			       unsigned long id)
{
	struct zx29_clk_controller *data = container_of(rcdev, struct zx29_clk_controller,
							    rcdev);
	u32 val;

	val = readl(data->resets[id].reg);

	return val & data->resets[id].mask;
}

const struct reset_control_ops zx297520v3_rst_ops = {
	.assert		= zx297520v3_rst_assert,
	.deassert	= zx297520v3_rst_deassert,
	.reset		= zx297520v3_rst_reset,
	.status		= zx297520v3_rst_status,
};

/* Used for gates where we don't know the parent input(s). Assume general bus clock. */
static const char * const clk_unknown[] = {
	"osc26m",
};

/* Used for gates where we know it is using the 26 mhz main clock. */
static const char * const clk_main[] = {
	"osc26m",
};

/* Top and matrix clocks are chaotic - I haven't found a consistent pattern behind their register
 * and bit locations. Generally there are two gates (pclk, wclk), one mux, one reset and sometimes
 * one divider, but exceptions apply. For some devices there is only a reset and some general
 * (parent) clocks need setup. This structure plus macro handles the somewhat regular parts.
 *
 * There are some patterns that can be observed.
 * mux 0x3c, div 0x48, gate 0x54
 * mux 0x40, div 0x4c, gate 0x5c
 * mux 0x44, div 0x50, gate 0x60
 *
 * For a 0 - 0xc - 0x18 pattern. Muxes from 0x3c to 0x44, dividers from 0x48 to 0x50, gates 0x54 to
 * 0x60. The pattern is broken for timer t17 though.
 *
 * Gates have 4 bits per clock - bit 0 for wclk, bit 1 for pclk, bit 2 for something the ZTE kernel
 * calls "gate" (the bits we use here are called "en"), which I don't know what it does, and bit 3
 * seems unused. E.g. offset 0x54 accepts all bits in 0xF77F7F7F - suggesting RTC, I2C0 have an
 * extra gate bit.
 */
struct zx297520v3_composite {
	u32 reset_id, wclk_id, pclk_id;
	const char *name;
	u32 reset_reg, reset_shift;
	u32 gate_reg, wclk_gate_shift, pclk_gate_shift;
	const char *pclk_parent;
	u32 mux_reg, mux_shift, mux_size;
	const char * const *mux_sel;
	u32 mux_sel_count;
	u32 div_reg, div_shift, div_size;
	u32 flags;
};

struct zx297520v3_gate {
	u32 id;
	const char *name, *parent;
	u32 reg, shift;
};

#define _ZX_CLK(name, reset_reg, reset_shift, gate_reg, wclk_shift, pclk_shift, pclk_parent,\
		mux_reg, mux_shift, mux_size, mux_sel,\
		div_reg, div_shift, div_size, flags) \
		{ZX297520V3_##name##_RESET, ZX297520V3_##name##_WCLK, ZX297520V3_##name##_PCLK,\
		 #name, reset_reg, reset_shift, gate_reg, wclk_shift, pclk_shift, pclk_parent,\
		 mux_reg, mux_shift, mux_size, mux_sel, ARRAY_SIZE(mux_sel),\
		 div_reg, div_shift, div_size, flags}

#define ZX_CLK(name, reset_reg, reset_shift, gate_reg, wclk_shift, pclk_shift,\
	       mux_reg, mux_shift, mux_size, mux_sel,\
	       div_reg, div_shift, div_size) \
	       _ZX_CLK(name, reset_reg, reset_shift, gate_reg, wclk_shift, pclk_shift, "osc26m",\
		mux_reg, mux_shift, mux_size, mux_sel,\
		div_reg, div_shift, div_size, 0)

#define ZX_CLK_CRIT(name, reset_reg, reset_shift, gate_reg, wclk_shift, pclk_shift,\
	       mux_reg, mux_shift, mux_size, mux_sel,\
	       div_reg, div_shift, div_size) \
	       _ZX_CLK(name, reset_reg, reset_shift, gate_reg, wclk_shift, pclk_shift, "osc26m",\
		mux_reg, mux_shift, mux_size, mux_sel,\
		div_reg, div_shift, div_size, CLK_IS_CRITICAL)

/* The default mpll settings multiply the 26 MHz reference clock times 24. A mux selection of 26 MHz
 * could mean using the 26 MHz oscillator directly, or passing it through the PLL and divide by 24.
 *
 * If a UART is set to mpl_d6 (default 104 MHz), changing the mpll multipliers does affect UART
 * timing as it should. This does not happen when the UART is set to 26 MHz input or timers that
 * read 26 MHz input. This suggests 26 MHz clocks use the reference clock directly.
 */
static const char * const ahb_sel[] = {
	"osc26m",
	"mpll_d6",	/* 104 mhz */
	"mpll_d8",	/* 78 mhz */
	"mpll_d8",	/* 78 mhz */
};

static const char * const timer_top_sel[] = {
	"osc32k",
	"osc26m",
};

static const char * const uart_top_sel[] = {
	"osc26m",
	"mpll_d6",	/* 104 mhz */
};

static const char * const m0_sel[] = {
	"osc26m",
	"mpll_d6",	/* 104 mhz */
	"mpll_d8",	/* 78 mhz */
	"osc32k",	/* Yes, tested. It is SLLLLOOOOOWWW. */
};

static const struct zx297520v3_composite top_clocks[] = {
	/*    (NAME,       RESET,     GATE,         MUX,                        DIV        ), */

	/* AHB: Don't turn this one off. The clock mux works and impact can be tested e.g. with
	 * iperf speed testing of the USB network connection. Values 2 and 3 give the same speed.
	 */
	ZX_CLK_CRIT(AHB,   0x70,  0,  0x54, 12, 13, 0x3c,  4, 2, ahb_sel,       0,     0, 0),

	/* Pinmux (AON, TOP, IOCFG but not PDCFG). Critical as well until we have a driver that
	 * consumes it. I don't think we'll realistically shut this off ever.
	 *
	 * Setting either bit 0 or 1 in register 0x58 makes the device work.
	 */
	ZX_CLK_CRIT(PMM,   0x74,  0,  0x58,  0,  1, 0x00,  0, 0, clk_unknown,   0,     0, 0),

	/* Timers. We don't use any of them, just shut them off. The timers are named and sorted
	 * by the IO address of the main timer controls. Some of the controls are documented in
	 * ZTE's kernel. Some I found by trial and error.
	 *
	 * Timer T17 is used by the ZSP firmware. The rproc driver will enable them as needed.
	 */
	ZX_CLK(TIMER_T08,  0x78,   4, 0x5c,  8,  9, 0x40,  1, 1, timer_top_sel, 0x4c,   8, 4),
	ZX_CLK(TIMER_T09,  0x78,   2, 0x5c,  4,  5, 0x40,  0, 1, timer_top_sel, 0x4c,   0, 4),
	ZX_CLK(TIMER_T12,  0x74,   6, 0x54,  4,  5, 0x3c,  0, 1, timer_top_sel, 0x48,   0, 4),
	ZX_CLK(TIMER_T13,  0x7c,   0, 0x60,  0,  1, 0x44,  0, 1, timer_top_sel, 0x50,   0, 4),
	ZX_CLK(TIMER_T14,  0x7c,   2, 0x60,  4,  5, 0x44,  1, 1, timer_top_sel, 0x50,   4, 4),
	ZX_CLK(TIMER_T15,  0x74,  10, 0x54, 20, 21, 0x3c,  3, 1, timer_top_sel, 0x48,   4, 4),
	ZX_CLK(TIMER_T16,  0x7c,   4, 0x60,  8,  9, 0x44,  2, 1, timer_top_sel, 0x50,   8, 4),
	ZX_CLK(TIMER_T17,  0x12c,  0, 0x128, 0,  1, 0x120, 0, 1, timer_top_sel, 0x124,  0, 4),

	/* This watchdog is set up by the bootloader and in normal operation the m0 firmware will
	 * feed the dog. The m0 firmware in turn wants to be fed in its own way. Since we normally
	 * don't run any m0 firmware we shut it off by default and expose it to userspace via the
	 * watchdog driver.
	 */
	ZX_CLK(WDT_T18,    0x74,  12, 0x54, 24, 25, 0x3c,  6, 1, timer_top_sel, 0x48,   8, 4),

	ZX_CLK(I2C0,       0x74,   8, 0x54,  8,  9, 0x3c,  1, 1, uart_top_sel,  0,      0, 0),
	ZX_CLK(UART0,      0x78,   6, 0x5c, 12, 13, 0x40,  2, 1, uart_top_sel,  0,      0, 0),

	/* How does this RTC work? I don't know, the ZTE kernel does not talk to it. It has an
	 * external RTC connected to I2C0.
	 */
	ZX_CLK(RTC,        0x74,   4, 0x54,  0,  1, 0x00,  0, 0, timer_top_sel, 0,      0, 0),

	/* This doesn't see to be talking to the physical SIM card. I can turn it off on the ZTE
	 * firmware without breaking LTE, and the "uicc" IRQ count keeps climbing. I think this is
	 * a eSim-like chip that can be provisioned with data at runtime, but I have no idea how to
	 * do it.
	 */
	ZX_CLK(USIM1,      0x74,  14, 0x54, 28, 29, 0x00,  0, 0, clk_main,      0x48,  12, 1),

	/*    (NAME,       RESET,     GATE,         MUX,                        DIV         ), */
};

/* Stand-alone topclk gates. */
static const struct zx297520v3_gate top_gates[] = {
	{ZX297520V3_USB_24M,		"usb_24m",	"mpll_d26",	0x6c,	  3},
	{ZX297520V3_USB_AHB,		"usb_ahb",	"AHB_wclk",	0x6c,	  4},
	/* LTE: gate only as far as I can see. I looked for resets and did not find any. There may
	 * be mux/div, but without understanding the behavior of this hardware it is impossible to
	 * tell. They are sorted by physical MMIO address of the devices, which happens to be the
	 * inverse order of the bits.
	 *
	 * I don't know what "LPM", "TD" and "W" mean. I copied them from ZTE's names.
	 */
	{ZX297520V3_LPM_GSM_WCLK,	"LPM_GSM_wclk",	clk_unknown[0],	0x58,	10},
	{ZX297520V3_LPM_GSM_PCLK,	"LPM_GSM_pclk",	clk_unknown[0],	0x58,	11},
	{ZX297520V3_LPM_LTE_WCLK,	"LPM_LTE_wclk",	clk_unknown[0],	0x58,	 8},
	{ZX297520V3_LPM_LTE_PCLK,	"LPM_LTE_pclk",	clk_unknown[0],	0x58,	 9},
	{ZX297520V3_LPM_TD_WCLK,	"LPM_TD_wclk",	clk_unknown[0],	0x58,	 6},
	{ZX297520V3_LPM_TD_PCLK,	"LPM_TD_pclk",	clk_unknown[0],	0x58,	 7},
	{ZX297520V3_LPM_W_WCLK,		"LPM_W_wclk",	clk_unknown[0],	0x58,	 4},
	{ZX297520V3_LPM_W_PCLK,		"LPM_W_pclk",	clk_unknown[0],	0x58,	 5},
	/* There are PCLKs for BROM/SRAM2 in 0x54, bit 16 and SRAM1 in 0x54, bit 18. Turning them
	 * off locks up the Cortex M0 coproc. Not added to the kernel until a way is found to
	 * recover the Cortex M0 or evidence of power savings.
	 */

	/* 4 Clock signals can be exposed to GPIO 15 (OUT0), 16 (OUT1), 17 (OUT2) and 18 (OUT32K).
	 * It looks like OUT1 and OUT2 feed an optional camera. OUT0 feeds an optional sound codec.
	 * Turning them off reduces noise in GPIO dumps. WCLK only although the ZTE camera code
	 * talks about a "software control enable" bit (0x1) that needs to be set for OUT1 and OUT2.
	 *
	 * CLK_OUT1 has a MUX in top + 0x34 bits 6 and 7 and is the only clock that has upll as a
	 * possible parent. CLK_OUT2 has a mux in bit 8, where 1 (default) selects osc26m. 0 is an
	 * unknown clock.
	 */
	{ZX297520V3_OUT0_WCLK,		"OUT0_wclk",	clk_unknown[0],	0x34,	 0},
	{ZX297520V3_OUT1_WCLK,		"OUT1_wclk",	"upll_d5",	0x90,	 2},
	{ZX297520V3_OUT2_WCLK,		"OUT2_wclk",	"osc26m",	0x94,	 2},
	{ZX297520V3_OUT32K_WCLK,	"OUT32k_wclk",	"osc32k",	0x34,	 1},
};

static const char * const cpu_sel[] = {
	"osc26m",
	"mpll",		/* 624 MHz */
	"mpll_d2",	/* 312 MHz */
	"mpll_d4",	/* 156 MHz */
};

static const char * const sd0_sel[] = {
	"osc26m",
	"mpll_d4",	/* 156 MHz */
	"gpll_d2",	/* 100 MHz */
	"mpll_d8",	/* 78 MHz */
	"gpll_d4",	/* 50 MHz */
	"gpll_d8",	/* 25 MHz */
};

static const char * const sd1_sel[] = {
	"osc26m",
	"gpll_d2",	/* 100 MHz */
	"mpll_d8",	/* 78 MHz */
	"gpll_d4",	/* 50 MHz */
	"mpll_d16",	/* 39 MHz */
	"gpll_d8",	/* 25 MHz */
};

static const char * const nand_sel[] = {
	"mpll_d4",	/* 156 MHz */
	"osc26m",
};

static const char * const edcp_sel[] = {
	"osc26m",
	"mpll_d4",	/* 156 MHz */
	"mpll_d5",	/* 124.8 MHz */
	"mpll_d6",	/* 104 MHz */
};

static const char * const tdm_sel[] = {
	"osc26m",
	"dpll_d4",	/* 122.88 MHz */
	"mpll_d6",	/* 104 MHz */
};

static const struct zx297520v3_composite matrix_clocks[] = {
	/* Both 0x24 and 0x28 bits 1 and 2 stop the CPU. There is also a bit in topclk+0x138, which
	 * ZTE's uboot calls "A53 reset", which also stops the CPU. I can't really tell the
	 * difference between matrix+28 and top+138. The clock can be disabled and enabled from the
	 * Cortex M0 and it will nicely stop and restart the A53, retaining all state.
	 *
	 * 0x50, bits 0-3 have the DDR clock. A lot of DDR gates and resets are in 0x100.
	 */
	ZX_CLK_CRIT(CPU,    0x28,  1,  0x24,  1,  2, 0x20,  0, 2, cpu_sel,     0,     0, 0),
	ZX_CLK(SD0,         0x58,  1,  0x54, 12, 13, 0x50,  4, 3, sd0_sel,     0,     0, 0),
	ZX_CLK(SD1,         0x58,  0,  0x54,  4,  5, 0x50,  8, 3, sd1_sel,     0,     0, 0),
	/* This is some "denali" NAND, not the qspi connected one. */
	ZX_CLK(NAND,        0x58,  4,  0x54, 20, 21, 0x50, 12, 2, nand_sel,    0,     0, 0),
	ZX_CLK(SSC,         0x94, 24,  0x84,  1,  2, 0,     0, 0, clk_unknown, 0,     0, 0),
	ZX_CLK(EDCP,        0x68,  0,  0x64,  2,  1, 0x50, 16, 2, edcp_sel,    0,     0, 0),
	/* PDCFG. Like PMM, either clock bit will allow the device to function. */
	ZX_CLK_CRIT(PDCFG,  0x94, 20,  0x88,  0,  1, 0,     0, 0, clk_unknown, 0,     0, 0),
	/* There are a lot more VOU related controls in these registers, but turning off the main
	 * clock seems to shut off the entire VOU MMIO range.
	 */
	ZX_CLK(VOU,        0x16c,  0, 0x168,  0,  1, 0,     0, 0, clk_main,       0,     0, 0),
};

static const struct zx297520v3_gate matrix_gates[] = {
	/* ZTE's driver has a statemt to the effect of *(matrix->base+0x11C) = 5, with a comment
	 * suggesting that this sets a 50 mhz clock. The clock code itself lists the parents of
	 * these clock as 50mhz pll output, but the GMAC driver never enables the clocks.
	 *
	 * The clocks below are enabled by the boot loader though, so they are on. And it turns
	 * out that they are necessary for proper operation of the ethernet hardware. As far as
	 * I can see trough experimentation, bit 1 affects the PHY whereas 0 and 2 affect the
	 * MAC chip itself.
	 *
	 * Chain the wclk and rmii clk together for now. I haven't found a way to make either
	 * the mdio node or the phy node enable a clock. According to ethernet-phy.yaml it is
	 * supposed to be possible, but I can't find code to that effect in of_mdio.c.
	 */
	{ZX297520V3_GMAC_RMII,	"gmac_rmii",		"gpll_d4",	0x110,	  1},
	{ZX297520V3_GMAC_WCLK,	"gmac_wclk",		"gmac_rmii",	0x110,	  2},
	{ZX297520V3_GMAC_PCLK,	"gmac_pclk",		"gpll_d4",	0x110,	  0},

	/* ZSP aka LTE DSP clock. I think there is a mux at matrix+0x30, but I have no idea
	 * about the frequencies it selects. Gate is at matrix+0x3c.
	 */
	{ZX297520V3_ZSP_WCLK,	"zsp_wclk",		"osc26m",	0x3c,	  0},

	/* Mailbox. I haven't found a reset for this. It seems to have a PCLK only - turning it off
	 * makes the MMIO area read 0x0. It looks like it does not need a WCLK. It generates IRQs
	 * fine with just bit 2 set. Bits 1 and 3 are 0 by default in this register.
	 */
	{ZX297520V3_MBOX_PCLK,	"mbox_pclk",		"osc26m",	0x88,	  2},

	/* DMA Controller. It has a reset and PCLK, but no WCLK. */
	{ZX297520V3_DMA_PCLK,	"dma_pclk",		"osc26m",	0x94,	  3},

	/* There is another clock controlling some "GSM" IP at 0xF3000000 in 0x88, bit 8. It appears
	 * to be a PCLK, but I have not found a matching WCLK or reset yet.
	 */

	/* I don't know how this clock works. Card detection in the way the dwc,mmc driver uses it
	 * appears broken no matter this clock's setting.
	 */
	{ZX297520V3_SD0_CDET,	"sd0_cdet",		"osc32k",	0x54,	  14},
	{ZX297520V3_SD1_CDET,	"sd1_cdet",		"osc32k",	0x54,	  6},

	/* LSP uplink clocks. The PCLK is fairly obvious (disabling it shuts off the entire LSP
	 * register area). The WCLK speeds were deduced by setting timers and qspi muxes to a
	 * specific speed and seeing which bit in matrix+0x7c needs to be enabled for the device
	 * to work.
	 *
	 * Due to the timers I am certain about the 26mhz and 32khz clocks. I cannot directly
	 * observe the qspi mux frequency, so the clock rates depend on ZTE's qspi mux selection
	 * being correct.
	 *
	 * Two additional bits are specific to sound components - the mux for the LSP's TDM IP is
	 * in matrixclk and gets passed down. I2S has a mux in LSP, which can select the dpll_d4
	 * clock.
	 *
	 * This code is commented out until the next patch because disabling unused clocks without
	 * an LSP consumer breaks the UART.
	 */
	{ZX297520V3_LSP_MPLL_D5_WCLK,	"lsp_mpll_d5",	"mpll_d5",	0x7c,	  0},
	{ZX297520V3_LSP_MPLL_D4_WCLK,	"lsp_mpll_d4",	"mpll_d4",	0x7c,	  1},
	{ZX297520V3_LSP_MPLL_D6_WCLK,	"lsp_mpll_d6",	"mpll_d6",	0x7c,	  2},
	{ZX297520V3_LSP_MPLL_D8_WCLK,	"lsp_mpll_d8",	"mpll_d8",	0x7c,	  3},
	{ZX297520V3_LSP_MPLL_D12_WCLK,	"lsp_mpll_d12",	"mpll_d12",	0x7c,	  4},
	{ZX297520V3_LSP_OSC26M_WCLK,	"lsp_osc26m",	"osc26m",	0x7c,	  5},
	{ZX297520V3_LSP_OSC32K_WCLK,	"lsp_osc32k",	"osc32k",	0x7c,	  6},
	{ZX297520V3_LSP_PCLK,		"lsp_pclk",	"osc26m",	0x7c,	  7},
	{ZX297520V3_LSP_TDM_WCLK,	"lsp_tdm_wclk",	"tdm_mux",	0x7c,	  8},
	{ZX297520V3_LSP_DPLL_D4_WCLK,	"lsp_dpll_d4",	"dpll_d4",	0x7c,	  9},
};

static int zx297520v3_composite(struct device *dev, void __iomem *base,
				struct clk_hw_onecell_data *clocks, struct zx29_reset_reg *resets,
				const struct zx297520v3_composite *input, size_t size)
{
	char pclk_name[32], wclk_name[32], mux_name[32], div_name[32];
	const char *wclk_parent, *div_parent;
	struct clk_hw *hw;
	unsigned int i;

	for (i = 0; i < size; ++i) {
		snprintf(wclk_name, ARRAY_SIZE(wclk_name), "%s_wclk", input[i].name);
		snprintf(pclk_name, ARRAY_SIZE(pclk_name), "%s_pclk", input[i].name);
		snprintf(mux_name, ARRAY_SIZE(mux_name), "%s_mux", input[i].name);
		snprintf(div_name, ARRAY_SIZE(div_name), "%s_div", input[i].name);

		resets[input[i].reset_id].reg = base + input[i].reset_reg;
		resets[input[i].reset_id].mask = BIT(input[i].reset_shift) +
						 BIT(input[i].reset_shift + 1);

		if (input[i].div_size)
			wclk_parent = div_name;
		else if (input[i].mux_size)
			wclk_parent = mux_name;
		else
			wclk_parent = input[i].mux_sel[0];

		hw = devm_clk_hw_register_gate(dev, pclk_name, input[i].pclk_parent, input[i].flags,
					       base + input[i].gate_reg, input[i].pclk_gate_shift,
					       0, &reg_lock);
		if (IS_ERR(hw))
			return PTR_ERR(hw);
		clocks->hws[input[i].pclk_id] = hw;

		if (input[i].mux_size) {
			hw = devm_clk_hw_register_mux(dev, mux_name, input[i].mux_sel,
						      input[i].mux_sel_count, 0,
						      base + input[i].mux_reg,
						      input[i].mux_shift, input[i].mux_size, 0,
						      &reg_lock);
			if (IS_ERR(hw))
				return PTR_ERR(hw);
			div_parent = mux_name;
		} else {
			div_parent = input[i].mux_sel[0];
		}

		hw = devm_clk_hw_register_gate(dev, wclk_name, wclk_parent,
					       input[i].flags | CLK_SET_RATE_PARENT,
					       base + input[i].gate_reg, input[i].wclk_gate_shift,
					       0, &reg_lock);
		if (IS_ERR(hw))
			return PTR_ERR(hw);
		clocks->hws[input[i].wclk_id] = hw;

		if (!input[i].div_size)
			continue;

		hw = devm_clk_hw_register_divider(dev, div_name, div_parent, CLK_SET_RATE_PARENT,
						  base + input[i].div_reg, input[i].div_shift,
						  input[i].div_size, 0, &reg_lock);
		if (IS_ERR(hw))
			return PTR_ERR(hw);
	}

	return 0;
}

static int zx297520v3_gate(struct device *dev, void __iomem *base,
			   struct clk_hw_onecell_data *clocks,
			   const struct zx297520v3_gate *input, size_t size)
{
	struct clk_hw *hw;
	unsigned int i;

	for (i = 0; i < size; ++i) {
		hw = devm_clk_hw_register_gate(dev, input[i].name, input[i].parent,
					       CLK_SET_RATE_PARENT, base + input[i].reg,
					       input[i].shift, 0, &reg_lock);
		if (IS_ERR(hw))
			return PTR_ERR(hw);
		clocks->hws[input[i].id] = hw;
	}

	return 0;
}

static int zx_restart_handle(struct sys_off_data *data)
{
	void __iomem *top_base = data->cb_data;

	writel(1, top_base);
	mdelay(1000);

	pr_emerg("Unable to restart system\n");
	return NOTIFY_DONE;
}

static const char * const dpll_sel[] = {
	"unknownpll_d2",
	"osc26m",
	"osc26m",
	"osc26m",
};

static const struct zx29_pll_desc zx297520_plls[] = {
	/* Default setting: 0x48040c11. 624/312/156. Only a single possible parent. This is the
	 * PLL for pretty much everything, including CPU, RAM and USB.
	 *
	 * Changing this PLL makes it possible to overclock the CPU or do more fine grained
	 * underclocking than the CPU's mux allows. It does run into two problems though: The USB
	 * device uses this PLL's output directly and is *very* sensitive to differences. DRAM
	 * is also fed by this clock and needs to be re-trained on larger changes, which needs to
	 * be done by the stage 1 boot loader.
	 */
	{0x8,	"mpll",		clk_main, ARRAY_SIZE(clk_main)},

	/* ZTE's code calls this PLL "upll". The only possible consumer I found is clk_out1, which
	 * outputs this clock on GPIO 16. The device that consumes this is an SPI camera, which I
	 * haven't seen in any device so far. clk_out1 has a gate in top + 0x90, bits 0 and 2. A mux
	 * is in top + 0x34, bits 2 and 3.
	 *
	 * Long story short, shut it off.
	 */
	{0x10,	"upll",		clk_main, ARRAY_SIZE(clk_main)},

	/* Default value 0x4834902d. Feeds dpll. 46.08 MHz. Bit 25 can be set, so two parents are
	 * possible. It looks like both values select the 26 MHz oscillator though.
	 */
	{0x100,	"unknownpll",	clk_main, ARRAY_SIZE(clk_main)},

	/* The documentation says 491.52 MHz and measurement with the LSP TDM device supports this.
	 * The default value is 0x480C2011, but not all boot loaders set it up. To get to 491.52
	 * with these settings it needs a 23.04 MHz reference clock, which matches unknownpll_d2.
	 * If unknownpll is disabled, dpll loses its lock. We set the frequency on this PLL if we
	 * find it is not enabled by the boot loader.
	 *
	 * The proprietary LTE driver or coproc enables and disables it. TDM and I2S can use it.
	 *
	 * It accepts parent values 0, 1, 2 and 3. Parent 0 is unknownpll_d2. The others look like
	 * osc26m. With a parent != 0 dpll never loses its lock even when all other PLLs are off
	 * and the TDM counter register increases at a rate consistent with a 26.0/23.04 clock
	 * increase.
	 */
	{0x18,	"dpll",		dpll_sel, ARRAY_SIZE(dpll_sel), 491520000},

	/* "g" is either for "general" or "gigahertz". The VCO runs at 1GHz. Output clocks are 200,
	 * 100, 50, 25 MHz. It is only used by SDIO though, so not very general.
	 */
	{0x110,	"gpll",		clk_main, ARRAY_SIZE(clk_main)},
};

static int zx297520_topclk_probe(struct platform_device *pdev)
{
	void __iomem *top_base, *matrix_base;
	struct zx29_clk_controller *top;
	struct device *dev = &pdev->dev;
	struct clk_hw *hw;
	struct clk *clk;
	unsigned int i;
	int res;

	top = devm_kzalloc(dev, offsetof(struct zx29_clk_controller,
					 resets[ZX297520V3_TOPRST_END]), GFP_KERNEL);
	if (!top)
		return -ENOMEM;

	top->clocks = devm_kzalloc(dev, struct_size(top->clocks, hws,
				   ZX297520V3_TOPCLK_END), GFP_KERNEL);
	if (!top->clocks)
		return -ENOMEM;
	top->clocks->num = ZX297520V3_TOPCLK_END;

	top_base = devm_platform_ioremap_resource_byname(pdev, "top");
	if (IS_ERR(top_base))
		return PTR_ERR(top_base);
	matrix_base = devm_platform_ioremap_resource_byname(pdev, "matrix");
	if (IS_ERR(matrix_base))
		return PTR_ERR(matrix_base);


	/* Offset 0x0 is the global board reset
	 * Offset 0x4 gives some static boot information - raw NAND or SPI NAND
	 */

	clk = devm_clk_get(dev, "osc32k");
	if (IS_ERR(clk)) {
		dev_err(dev, "32 KHz input clock not found\n");
		return PTR_ERR(clk);
	}

	clk = devm_clk_get(dev, clk_main[0]);
	if (IS_ERR(clk)) {
		dev_err(dev, "26 MHz input clock not found\n");
		return PTR_ERR(clk);
	}

	res = zx29_register_plls(dev, top_base, zx297520_plls, ARRAY_SIZE(zx297520_plls));
	if (res)
		return res;

	res = zx297520v3_composite(dev, top_base, top->clocks, top->resets,
				 top_clocks, ARRAY_SIZE(top_clocks));
	if (res)
		return res;
	res = zx297520v3_composite(dev, matrix_base, top->clocks, top->resets,
				   matrix_clocks, ARRAY_SIZE(matrix_clocks));
	if (res)
		return res;

	res = zx297520v3_gate(dev, top_base, top->clocks, top_gates, ARRAY_SIZE(top_gates));
	if (res)
		return res;
	res = zx297520v3_gate(dev, matrix_base, top->clocks,
			      matrix_gates, ARRAY_SIZE(matrix_gates));
	if (res)
		return res;

	/* The Cortex M0 coprocessor. It is responsible for booting the board and runs some power
	 * management helper code on the stock firmware, but isn't critical. We can run custom code
	 * on it but currently do not.  These bits control the speed and the values are mentioned in
	 * ZTE's uboot. It isn't clear to me if this is directly responsible for the m0 clock, or
	 * if it is the input to another clock. I also haven't found a gate that shuts the m0 off
	 * and allows restarting. There don't seem to be resets either.
	 *
	 * Also note the comment about SRAM1 and SRAM2 PCLKs. They can be turned off, which will
	 * crash the M0 if it reads instructions from them.
	 */
	hw = devm_clk_hw_register_mux(dev, "m0_wclk", m0_sel, ARRAY_SIZE(m0_sel),
				      0, top_base + 0x38, 0, 2, 0, &reg_lock);
	if (IS_ERR(hw))
		return PTR_ERR(hw);
	top->clocks->hws[ZX297520V3_M0_WCLK] = hw;

	/* One stray matrix mux: The TDM mux is in matrixclk and it is passed to the LSP controller.
	 * In a way the matrix-LSP link gate (LSP_TDM_WCLK) could be considered a matching gate, but
	 * there is no reset and no pclk.
	 */
	hw = devm_clk_hw_register_mux(dev, "tdm_mux", tdm_sel, ARRAY_SIZE(tdm_sel), 0,
				      matrix_base + 0x50, 24, 2, 0, &reg_lock);
	if (IS_ERR(hw))
		return PTR_ERR(hw);

	/* This is to catch holes in the tables rather than registration errors */
	for (i = 0; i < ZX297520V3_TOPCLK_END; i++) {
		if (!top->clocks->hws[i]) {
			dev_err(dev, "Clock %u not registered\n", i);
			return -ENODEV;
		}
	}

	res = devm_of_clk_add_hw_provider(dev, of_clk_hw_onecell_get, top->clocks);
	if (res)
		return res;


	res = devm_register_restart_handler(dev, zx_restart_handle, top_base);
	if (res)
		dev_err(dev, "can't register restart handler (res=%d)\n", res);

	/* Stray reset bits follow.
	 *
	 * I haven't found any clocks for GPIO. It probably wouldn't make much
	 * sense anyway. Only one bit per controller.
	 */
	top->resets[ZX297520V3_GPIO8_RESET].reg = top_base + 0x74;
	top->resets[ZX297520V3_GPIO8_RESET].mask = BIT(2);
	top->resets[ZX297520V3_GPIO_RESET].reg = top_base + 0x74;
	top->resets[ZX297520V3_GPIO_RESET].mask = BIT(3);

	/* USB reset. This is slightly special because it needs to wait for a ready bit after
	 * deasserting.
	 */
	top->resets[ZX297520V3_USB_RESET].reg = top_base + 0x80;
	top->resets[ZX297520V3_USB_RESET].mask = BIT(3) | BIT(4) | BIT(5);
	top->resets[ZX297520V3_USB_RESET].wait_mask = BIT(1);

	/* This bit is set by ZTE's cpko.ko blob, it looks like a reset bit for the LTE DSP
	 * coprocessor. Clocks for it are in matrixclk.
	 */
	top->resets[ZX297520V3_ZSP_RESET].reg = top_base + 0x13c;
	top->resets[ZX297520V3_ZSP_RESET].mask = BIT(0);

	top->resets[ZX297520V3_DMA_RESET].reg = matrix_base + 0x70;
	top->resets[ZX297520V3_DMA_RESET].mask = BIT(0) | BIT(1);
	top->resets[ZX297520V3_GMAC_RESET].reg = matrix_base + 0x114;
	top->resets[ZX297520V3_GMAC_RESET].mask = BIT(0) | BIT(1);

	for (i = 0; i < ZX297520V3_TOPRST_END; ++i) {
		if (!(top->resets[i].reg && top->resets[i].mask)) {
			dev_err(dev, "zx297520v3 reset %u has no register or mask\n", i);
			return -ENODEV;
		}
	}

	top->rcdev.owner = THIS_MODULE;
	top->rcdev.nr_resets = ZX297520V3_TOPRST_END;
	top->rcdev.ops = &zx297520v3_rst_ops;
	top->rcdev.of_node = dev->of_node;
	return devm_reset_controller_register(dev, &top->rcdev);
}

/* LSP clock entries have a common pattern: Bit 0 for PCLK, Bit 1 for WCLK. Bit 4 (and sometimes
 * more) for WCLK mux.
 *
 * Bit 8 and 9 are reset bits. I don't know the difference between the two, but they both
 * need to be set to deassert the reset.
 *
 * Bits 12-16 can be a divisor, but not all clocks have it. Some clocks have a divisor in 16-20.
 *
 * The ID given in this table is the first register in the device's MMIO space. ZTE's drivers
 * usually call this a version register, but it looks more like a device identifier.
 *
 * It looks like the registers map to devices like this:
 *
 * Timer reg	function	div	dev offset(lsp + xxxx)	ID
 * 0x0: Read-only, probably device identifier			0x00752100
 * 0x4:		timer_l1	Y	0x1000			0x02020000
 * 0x8:		watchdog_l2	Y	0x2000			0x02020000
 * 0xc:		watchdog_l3	Y	0x3000			0x02020000
 * 0x10:	i2c1		N	0x4000			0x01020000
 * 0x14:	i2s0		Yh	0x5000			0x01030000
 * 0x18:	always 0	N	-
 * 0x1c:	i2s1		Yh	0x6000			0x01030000
 * 0x20:	always 0	N	-
 * 0x24:	qspi		N	0x7000			0x01040000
 * 0x28:	uart1		N	0x8000			0x01060000
 * 0x2c:	i2c2		N	0x9000			0x01020000
 * 0x30:	spi0		Y	0xa000			0x01040000
 * 0x34:	timer_lb	Y	0xb000			0x02020000
 * 0x38:	timer_lc	Y	0xc000			0x02020000
 * 0x3c:	uart2		N	0xd000			0x01060000
 * 0x40:	watchdog_le	Y	0xe000			0x02020000
 * 0x44:	timer_lf	Y	0xf000			0x02020000
 * 0x48:	spi1		Y	0x10000			0x01040000
 * 0x4c:	timer_l11	Y	0x11000			0x02020000
 * 0x50:	tdm		Y	0x12000			0x01040000
 *
 * Registers 0x58, 0x5c, 0x60, 0x64, 0x68 seem to contain more controls for i2s and tdm.
 */

static const char * const timer_lsp_sel[] = {
	"lsp_osc32k",
	"lsp_osc26m",
};

static const char * const uart_lsp_sel[] = {
	"lsp_osc26m",
	"lsp_mpll_d6",
};

static const char * const i2s_lsp_sel[] = {
	"lsp_osc26m",
	"lsp_dpll_d4",
	"lsp_mpll_d6",
	/* Unknown */
};

static const char * const tdm_lsp_sel[] = {
	"lsp_tdm_wclk",
};

static const char * const spi_lsp_sel[] = {
	"lsp_osc26m",
	"lsp_mpll_d4",
	"lsp_mpll_d6",
	/* Unknown */
};

static const char * const qspi_lsp_sel[] = {
	"lsp_osc26m",
	"lsp_mpll_d4",
	"lsp_mpll_d5",
	"lsp_mpll_d6",
	"lsp_mpll_d8",
	"lsp_mpll_d12",
	"lsp_osc26m",
	"lsp_osc26m",
};

#define LSP_CLOCK(offset, name, mux, div_shift, div_size) {\
		ZX297520V3_##name##_RESET, ZX297520V3_##name##_WCLK, ZX297520V3_##name##_PCLK,\
		#name, offset, 8, offset, 0, 1, "lsp_pclk", offset, 4, 4, mux, ARRAY_SIZE(mux),\
		offset, div_shift, div_size, 0}

static const struct zx297520v3_composite lsp_clocks[] =  {
	LSP_CLOCK(0x4,	TIMER_L1,	timer_lsp_sel,	0,	0),
	LSP_CLOCK(0x8,	WDT_L2,		timer_lsp_sel,	0,	0),
	LSP_CLOCK(0xc,	WDT_L3,		timer_lsp_sel,	0,	0),
	LSP_CLOCK(0x10,	I2C1,		uart_lsp_sel,	0,	0),
	LSP_CLOCK(0x14,	I2S0,		i2s_lsp_sel,	16,	4),
	LSP_CLOCK(0x1c,	I2S1,		i2s_lsp_sel,	16,	4),
	LSP_CLOCK(0x24,	QSPI,		qspi_lsp_sel,	0,	0),
	LSP_CLOCK(0x28,	UART1,		uart_lsp_sel,	0,	0),
	LSP_CLOCK(0x2C,	I2C2,		uart_lsp_sel,	0,	0),
	LSP_CLOCK(0x30,	SPI0,		spi_lsp_sel,	12,	4),
	LSP_CLOCK(0x34,	TIMER_LB,	timer_lsp_sel,	12,	4),
	LSP_CLOCK(0x38,	TIMER_LC,	timer_lsp_sel,	12,	4),
	LSP_CLOCK(0x3c,	UART2,		uart_lsp_sel,	0,	0),
	LSP_CLOCK(0x40,	WDT_LE,		timer_lsp_sel,	12,	4),
	LSP_CLOCK(0x44,	TIMER_LF,	timer_lsp_sel,	12,	4),
	LSP_CLOCK(0x48,	SPI1,		spi_lsp_sel,	12,	4),
	LSP_CLOCK(0x4c,	TIMER_L11,	timer_lsp_sel,	12,	4),
	LSP_CLOCK(0x50,	TDM,		tdm_lsp_sel,	16,	4),
};

#undef LSP_CLOCK

static int zx297520_lspclk_probe(struct platform_device *pdev)
{
	static const char * const parent_names[] = { "mpll_d5", "mpll_d4", "mpll_d6", "mpll_d8",
						     "mpll_d12", "osc26m", "osc32k", "pclk",
						     "tdm_wclk", "dpll_d4",
	};

	struct zx29_clk_controller *lsp;
	struct device *dev = &pdev->dev;
	void __iomem *base;
	struct clk *parent;
	unsigned int i;
	int res;

	lsp = devm_kzalloc(dev, offsetof(struct zx29_clk_controller,
					 resets[ZX297520V3_LSPRST_END]), GFP_KERNEL);
	if (!lsp)
		return -ENOMEM;

	lsp->clocks = devm_kzalloc(dev, struct_size(lsp->clocks, hws,
				   ZX297520V3_LSPCLK_END), GFP_KERNEL);
	if (!lsp->clocks)
		return -ENOMEM;
	lsp->clocks->num = ZX297520V3_LSPCLK_END;

	base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(base))
		return PTR_ERR(base);

	/* TODO: Technically we can disable the pclk if all LSP devices are shut down, but that
	 * needs custom clk ops to tiptoe around a disabled LSP pclk before attempting to access
	 * the actual clock. In normal operation it is unlikely that all LSP devices are shut down
	 * simultaneously though as UART and NAND are located here.
	 */
	parent = devm_clk_get_enabled(dev, "pclk");
	if (IS_ERR(parent)) {
		dev_err(dev, "failed to find lsp pclk\n");
		return PTR_ERR(parent);
	}

	for (i = 0; i < ARRAY_SIZE(parent_names); ++i) {
		parent = devm_clk_get(dev, parent_names[i]);
		if (IS_ERR(parent)) {
			dev_err(dev, "failed to find lsp %s clock\n", parent_names[i]);
			return PTR_ERR(parent);
		}
	}

	res = zx297520v3_composite(dev, base, lsp->clocks, lsp->resets,
				 lsp_clocks, ARRAY_SIZE(lsp_clocks));
	if (res)
		return res;

	/* This is to catch holes in the tables rather than registration errors */
	for (i = 0; i < ZX297520V3_LSPCLK_END; i++) {
		if (!lsp->clocks->hws[i]) {
			dev_err(dev, "Clock %u not registered\n", i);
			return -ENODEV;
		}
	}

	res = devm_of_clk_add_hw_provider(dev, of_clk_hw_onecell_get, lsp->clocks);
	if (res)
		return res;

	for (i = 0; i < ZX297520V3_LSPRST_END; ++i) {
		if (!(lsp->resets[i].reg && lsp->resets[i].mask)) {
			dev_err(dev, "Reset %u has no register or mask\n", i);
			return -ENODEV;
		}
	}

	lsp->rcdev.owner = THIS_MODULE;
	lsp->rcdev.nr_resets = ZX297520V3_LSPRST_END;
	lsp->rcdev.ops = &zx297520v3_rst_ops;
	lsp->rcdev.of_node = dev->of_node;
	return devm_reset_controller_register(dev, &lsp->rcdev);
}

static const struct of_device_id of_match_zx297520v3_topclk[] = {
	{ .compatible = "zte,zx297520v3-topclk"},
	{ }
};
MODULE_DEVICE_TABLE(of, of_match_zx297520v3_topclk);

static struct platform_driver clk_zx297520v3_topclk = {
	.probe = zx297520_topclk_probe,
	.driver = {
		.name = "clk-zx297520v3-topclk",
		.of_match_table = of_match_zx297520v3_topclk,
	},
};

static const struct of_device_id of_match_zx297520v3_lspclk[] = {
	{ .compatible = "zte,zx297520v3-lspclk"},
	{ }
};
MODULE_DEVICE_TABLE(of, of_match_zx297520v3_lspclk);

static struct platform_driver clk_zx297520v3_lspclk = {
	.probe = zx297520_lspclk_probe,
	.driver = {
		.name = "clk-zx297520v3-lspclk",
		.of_match_table = of_match_zx297520v3_lspclk,
	},
};

static int __init zx297520v3_clk_module_init(void)
{
	int res;

	res = platform_driver_register(&clk_zx297520v3_topclk);
	if (res) {
		pr_err("Failed to register driver %s: %d\n",
		       clk_zx297520v3_topclk.driver.name, res);
		return res;
	}
	res = platform_driver_register(&clk_zx297520v3_lspclk);
	if (res) {
		pr_err("Failed to register driver %s: %d\n",
		       clk_zx297520v3_lspclk.driver.name, res);
		platform_driver_unregister(&clk_zx297520v3_topclk);
		return res;
	}

	return 0;
}

static void __exit zx297520v3_clk_module_exit(void)
{
	platform_driver_unregister(&clk_zx297520v3_lspclk);
	platform_driver_unregister(&clk_zx297520v3_topclk);
}

module_init(zx297520v3_clk_module_init);
module_exit(zx297520v3_clk_module_exit);

MODULE_AUTHOR("Stefan Dösinger <stefandoesinger@gmail.com>");
MODULE_DESCRIPTION("ZTE zx297520v3 clock driver");
MODULE_LICENSE("GPL");

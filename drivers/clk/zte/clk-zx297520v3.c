// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 Stefan Dösinger
 *
 * There are no public datasheets for zx297520v3. The controls on this clock controller have been
 * extracted from ZTE's kernel and U-Boot sources and a LOT of manual testing. Some clocks can be
 * found in the power management (evb_cpurpm.img) and LTE (cpko.ko) blobs shipped with the routers.
 *
 * Because ZTE's sources are wrong or misleading in some parts and testing can be misinterpreted,
 * this file contains a LOT of comments on how particular clocks were tested and how the impact of
 * changes to them can be observed.
 */
#include <dt-bindings/clock/zte,zx297520v3-clk.h>
#include <linux/platform_device.h>
#include <linux/module.h>
#include <linux/regmap.h>
#include <linux/types.h>
#include <linux/errno.h>

#include "clk-zx.h"

MODULE_IMPORT_NS("ZTE_CLK");

/* Used for gates where we don't know the parent input(s). Assume general oscillator */
static const char clk_unknown[] = "clock-26m";

/* For clocks that use the ungated clock-26m input */
static const char * const clk_main[] = {
	"clock-26m",
};

static const char * const dpll_parents[] = {
	"unknownpll-d2",
	"clock-26m",
};

static const char * const zx297520v3_top_inputs[] = {
	"osc26m",
	"osc32k"
};

/* Top and matrix clocks are chaotic - I haven't found a consistent pattern behind their register
 * and bit locations. Generally there are two gates (pclk, wclk), one mux, two resets and sometimes
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
 * calls "gate" (the bits we use here are called "en"), which presumably enables automatic clock
 * gating. Bit 3, if settable, seems unused. E.g. offset 0x54 accepts all bits in 0xF77F7F7F -
 * suggesting RTC, I2C0 have an extra gate bit.
 *
 * The default mpll settings multiply the 26 MHz reference clock times 24. A mux selection of 26 MHz
 * could mean using the 26 MHz oscillator directly, or passing it through the PLL and divide by 24.
 *
 * If a UART is set to mpll-d6 (default 104 MHz), changing the mpll multipliers does affect UART
 * timing as it should. This does not happen when the UART is set to 26 MHz input or timers that
 * read 26 MHz input. This suggests 26 MHz clocks use the reference clock directly.
 *
 * The geneneral clock flow on this board goes from clock-26m into the PLLs. The PLL output is
 * branched into a small number of divided frequencies (mpll, mpll-d4, mpll-d5, mpll-d6, mpll-d8,
 * gpll, gpll-d2, gpll-d4) at a high level. These divided frequencies as well as clock-26m go
 * through a series of gates at offset 0x140 in topcrm. The output of these gates provide wclks for
 * everything.
 *
 * At least 3 of those gate outputs (mpll, dpll, gpll-d2) are further divided behind those gates,
 * presumably in the matrix controller. There are multiple paths how 104 and 78 MHz are derived,
 * some specific to topcrm (0x140 bits 1 and 4), some specific to matrix (0x140 bit 9). There are
 * likely 3 "copies" of the 78 MHz clock and two 50 MHz ones.
 *
 * Pclk is not gated by the 0x140 gates at all. Likewise clock-32k is fed to the timers without
 * passing through this gate, even for consumers on matrix/LSP. Topcrm clock-26m wclks are not gated
 * either, but top-consumed PLL clocks are.
 */

static const struct zx_fixed_divider_desc zx297520v3_top_fixed_divs[] = {
	/* clock-26m division for clk_outX */
	{"clock-26m-d2",		"clock-26m",		 2},

	/* This one is a parent for dpll */
	{"unknownpll-d2",		"unknownpll",		 2},

	/* Pre-0x140 divisions. They enter topcrm through different bits in 0x140 */
	{"mpll-d2",			"mpll",			 2},
	{"mpll-d5",			"mpll",			 5},
	{"mpll-d6",			"mpll",			 6},
	{"mpll-d8",			"mpll",			 8},
	{"upll-d12",			"upll",			12},
	{"gpll-d2",			"gpll",			 2},
	{"gpll-d4",			"gpll",			 4},

	/* Post-140 divisions. They enter through the same bit and can be selected by different mux
	 * values of their consumers or get piped to multiple consumers with different documented
	 * rates - in particular, USB wclk (24 MHz according to ZTE) shares bit 2 with M0's 78MHz.
	 */
	{"top-mpll-d2-d4",		"top-mpll-d2",		 4},
	{"top-mpll-d2-d13",		"top-mpll-d2",		13},
	{"top-upll-d12-d2",		"top-upll-d12",		 2},
	{"top-gpll-d4-d2",		"top-gpll-d4",		 2},
};

/* AHB: The clock mux works and impact can be tested e.g. with iperf speed testing of the USB
 * network connection. Values 2 and 3 give the same speed and depend on the same bit (3) in 0x140.
 * The mpll-d6 rate is gated off by the same bit in 0x140 as m0's mpll-d6, but the d8 is different.
 * This may indicate that one of the two uses a different clock rate than documented in ZTE's
 * kernel. The M0 speed is easy to measure, so I am confident in the M0's 78 MHz rate. AHB looks
 * like 78 rather than 52, but confidence is lower because RAM read tests are influenced by CPU
 * overhead too.
 */
static const char * const ahb_sel[] = {
	"clock-26m",
	"top-mpll-d6",		/* 104 mhz */
	"top-mpll-d8",		/* 78 mhz */
	"top-mpll-d8",		/* 78 mhz */
};

static const char * const timer_top_sel[] = {
	"clock-32k",
	"clock-26m",
};

static const char * const uart_top_sel[] = {
	"clock-26m",
	"top-mpll-d6",	/* 104 mhz, sharing M0's and ahb's 0x140 gate */
};

/* The Cortex M0 coprocessor. It is responsible for booting the board and runs some power management
 * helper code on the stock firmware. The M0 rproc itself is not critical, but most of topcrm's
 * registers become unreadable if this is gated off or become slow when clocked at 32khz, e.g. GPIO
 * becomes borderline unusable at 32khz.
 *
 * There is no dedicated M0 gate. Disabling ahb-wclk looks like the M0 stops from A53's point of
 * view, but it merely breaks the A53's ability to communicate with it, and M0's ability to
 * communicate with the standard UART on LSP. M0 keeps running with both ahb-wclk and ahb-pclk
 * gated off and can re-enable the gates.
 */
static const char * const m0_sel[] = {
	"clock-26m",
	"top-mpll-d6",		/* 104 mhz */
	"top-mpll-d2-d4",	/* 78 mhz, but different gate than the ahb counterpart */
	"clock-32k",		/* Yes, tested. It is SLLLLOOOOOWWW. */
};

/* Clk-out0/1/2/32k: These clocks are exposed on GPIOs 15, 16, 17 and 18 respectively. They are used
 * in ZTE's camera and sound code, by directly poking into the clock registers from the device
 * drivers. Until the respective devices are supported they can safely be switched off.
 *
 * For clk-out1 ZTE's camera code says the following:
 *
 * 0 -> 20 MHz
 * 1 -> 40 MHz
 * 2 -> 13 MHz
 * 3 -> 26 MHz
 *
 * 0 and 1 read from upll. I confirmed their rates (upll-d24 and upll-d12) by setting upll to a very
 * low frequency and sampling the clock by GPIO reads. The outputs of 2 and 3 are way too high to
 * test that way. Neither is reading from any PLL, they keep oscillating when all PLLs are disabled.
 * I am fairly sure that 3 is just direct clock-26m because it perfectly matches what out2 is
 * showing in its 26 MHz setting.
 *
 * Clk-out2 is similar, but it has only one control bit in top 0x34 bit 8. Neither setting selects
 * a PLL output. When setting *0x34 = 0x080, clk-out1 and clk-out2 are in lockstep, presumably
 * running at 13 MHz. 0x1c0 also runs in lockstep, presumably 26 MHz.
 *
 * clk-out0 has a mux in bit 5. Value 0 most likely selects clock-26m. I am not entirely sure about
 * value 1 (which is the default), but the m0 mux has an impact on it. It looks like a debug pin
 * that exposes some core clock.
 */

static const char * const out0_sel[] = {
	"clock-26m",
	"m0-mux",
};

static const char * const out1_sel[] = {
	"top-upll-d12",		/* 40 MHz */
	"top-upll-d12-d2",	/* 20 MHz */
	"clock-26m",
	"clock-26m-d2",
};

/* Clk-o is similar to clk-out*, providing a clock on GPIO 20, presumably for driving a (R)MII phy.
 * The 50 MHz value is documented in a stray comment in ZTE's GMAC driver. I tested it similarly to
 * the above pins. Mux setting 0 gives half the count as setting 1 and setting 1 gives gpll divided
 * by 4 - matching the 50 MHz suggested by the ZTE comment at the default gpll settings. Gating gpll
 * stops the clock for values 0 and 1.
 *
 * Settings 2 and 3 are possible, but seem to return garbage. It is generally pulsing up and down,
 * except if both gpll and upll are stopped. I suspect it just reads random electrical fluctuation
 * from other places in the board. Yes, I had a pull-down enabled when testing this.
 *
 * I am not aware of any board that uses this though. The Ethernet equipped ones I have all run the
 * phy with its own oscillator.
 */
static const char * const rmii_sel[] = {
	"top-gpll-d4-d2",	/* 25 MHz */
	"top-gpll-d4",		/* 50 MHz */
};

static const struct zx_pll_desc zx297520v3_plls[] = {
	/* Default setting: 0x48040c11. 624/312/156. Only a single possible parent. This is the
	 * PLL for pretty much everything, including CPU, RAM and USB.
	 *
	 * Changing this PLL makes it possible to overclock the CPU or do more fine grained
	 * underclocking than the CPU's mux allows. It does run into two problems though: The USB
	 * device uses this PLL's output directly and is *very* sensitive to differences. DRAM
	 * is also fed by this clock and needs to be re-trained on larger changes, which needs to
	 * be done by the stage 1 boot loader.
	 */
	{
		.name = "mpll",
		.parents = clk_main,
		.num_parents = ARRAY_SIZE(clk_main),
		.rate = 0,
		.reg = 0x8
	},

	/* ZTE's code calls this PLL "upll". The only possible consumers I found are clk-out1,
	 * which outputs this clock on GPIO 16 and HSIC. HSIC doesn't have anything connected to
	 * it on the devices I tested. The device that consumes clk-out1 seems to be an SPI
	 * camera, which I haven't seen in any device so far. ZTE manipulates this PLL directly out
	 * of the camera code, so it is unlikely to have another consumer.
	 *
	 * Long story short, shut it off.
	 */
	{
		.name = "upll",
		.parents = clk_main,
		.num_parents = ARRAY_SIZE(clk_main),
		.rate = 0,
		.reg = 0x10,
	},

	/* Default value 0x4834902d. Feeds dpll. 46.08 MHz. Bit 25 can be set, so two parents are
	 * possible. It looks like both values select the 26 MHz oscillator though.
	 *
	 * Since dpll's prepare may depend in this clock generating a usable signal, it needs to be
	 * enabled when prepared.
	 */
	{
		.name = "unknownpll",
		.parents = clk_main,
		.num_parents = ARRAY_SIZE(clk_main),
		.rate = 0,
		.reg = 0x100,
		.flags = ZX297520V3_PLL_PREPARE_IS_ENABLE,
	},

	/* The documentation says 491.52 MHz and measurement with the LSP TDM device supports this.
	 * The default value is 0x480c2011, but not all boot loaders set it up. To get to 491.52
	 * with these settings it needs a 23.04 MHz reference clock, which matches unknownpll-d2.
	 * If unknownpll is disabled, dpll loses its lock. We set the frequency on this PLL if we
	 * find it is not enabled by the boot loader.
	 *
	 * The proprietary LTE driver or rproc enables and disables it. TDM and I2S can use it.
	 *
	 * It accepts parent values 0, 1, 2 and 3. Parent 0 is unknownpll-d2. The others look like
	 * clock-26m. With a parent != 0 dpll never loses its lock even when all other PLLs are off
	 * and the TDM counter register increases at a rate consistent with a 26.0/23.04 clock
	 * increase.
	 */
	{
		.name = "dpll",
		.parents = dpll_parents,
		.num_parents = ARRAY_SIZE(dpll_parents),
		.rate = 491520000,
		.reg = 0x18,
	},

	/* "g" is either for "general" or "gigahertz". The VCO runs at 1GHz. Output clocks are 200,
	 * 100, 50, 25, ... MHz. It is used optionally by SDIO and QSPI and can drive a GPIO clock
	 * output for RMII, so it doesn't seem very general.
	 */
	{
		.name = "gpll",
		.parents = clk_main,
		.num_parents = ARRAY_SIZE(clk_main),
		.rate = 0,
		.reg = 0x110,
	},
};

#define MUX(_id, _name, _parents, _reg, _shift, _size) { \
	_id, _name, _parents, ARRAY_SIZE(_parents), _reg, _shift, _size}

#define DIV(_name, _parent, _reg, _shift, _size) { _name, _parent, _reg, _shift, _size }

#define GATE(_id, _name, _parent, _reg, _shift, _flags) { \
	.id = _id, \
	.name = _name, \
	.parent = _parent, \
	.flags = _flags, \
	.reg = _reg, \
	.shift = _shift, \
}

static const struct zx_mux_desc zx297520v3_top_muxes[] = {
	MUX(ZX297520V3_M0_WCLK,            "m0-mux",        m0_sel,            0x38,  0, 2),
	MUX(0,                             "ahb-mux",       ahb_sel,           0x3c,  4, 2),
	MUX(0,                             "timer-t08-mux", timer_top_sel,     0x40,  1, 1),
	MUX(0,                             "timer-t09-mux", timer_top_sel,     0x40,  0, 1),
	MUX(0,                             "timer-t12-mux", timer_top_sel,     0x3c,  0, 1),
	MUX(0,                             "timer-t13-mux", timer_top_sel,     0x44,  0, 1),
	MUX(0,                             "timer-t14-mux", timer_top_sel,     0x44,  1, 1),
	MUX(0,                             "timer-t15-mux", timer_top_sel,     0x3c,  3, 1),
	MUX(0,                             "timer-t16-mux", timer_top_sel,     0x44,  2, 1),
	MUX(0,                             "timer-t17-mux", timer_top_sel,    0x120,  0, 1),
	MUX(0,                             "wdt-t18-mux",   timer_top_sel,     0x3c,  6, 1),
	MUX(0,                             "i2c0-mux",      uart_top_sel,      0x3c,  1, 1),
	MUX(0,                             "uart0-mux",     uart_top_sel,      0x40,  2, 1),
	MUX(0,                             "out0-mux",      out0_sel,          0x34,  5, 1),
	MUX(0,                             "out1-mux",      out1_sel,          0x34,  6, 2),
	MUX(0,                             "rmiiphy-mux",   rmii_sel,         0x11c,  0, 2),
};

static const struct zx_div_desc zx297520v3_top_dividers[] = {
	DIV("timer-t08-div", "timer-t08-mux",   0x4c,  8, 4),
	DIV("timer-t09-div", "timer-t09-mux",   0x4c,  0, 4),
	DIV("timer-t12-div", "timer-t12-mux",   0x48,  0, 4),
	DIV("timer-t13-div", "timer-t13-mux",   0x50,  0, 4),
	DIV("timer-t14-div", "timer-t14-mux",   0x50,  4, 4),
	DIV("timer-t15-div", "timer-t15-mux",   0x48,  4, 4),
	DIV("timer-t16-div", "timer-t16-mux",   0x50,  8, 4),
	DIV("timer-t17-div", "timer-t17-mux",  0x124,  0, 4),
	DIV("wdt-t18-div",   "wdt-t18-mux",     0x48,  8, 4),
	DIV("usim1-div",     clk_unknown,       0x48, 12, 1),
};

static const struct zx_gate_desc zx297520v3_top_gates[] = {
	/* topcrm input gates. These are complicated, but shed a lot of light on the board's clock
	 * distribution.
	 *
	 * Settable mask: 0x017333FF. Generally one nibble per PLL, except for MPLL that has more.
	 * One for clock-26m work clock. pclk and clock-32k are not gated globally here or anywhere
	 * else as far as I can see.
	 *
	 * If you want to test your understanding of this board, try to manually configure it into a
	 * setup where bit 24 or bit 9 are off without crashing it.
	 */

	/* Bit 0: No known consumer */
	/* Bit 1: M0's 78 MHz selection, but somehow also involved in USB too */
	GATE(0,                            "top-mpll-d2",    "mpll-d2",       0x140,  1, 0),
	/* Bit 2: No known consumer, named mpll_48m_clk in ZTE's cpko.ko blob */
	GATE(0,                            "top-mpll-d8",    "mpll-d8",       0x140,  3, 0),
	GATE(0,                            "top-mpll-d6",    "mpll-d6",       0x140,  4, 0),
	/* Bit 5: No known consumer. mpll_156m_clk in cpko.ko */
	/* Bit 6: AXI voting candidate, might be selected by rprocs without notification */
	GATE(ZX297520V3_MPLL_D5,           "matrix-mpll-d5", "mpll-d5",       0x140,  6,
		CLK_IS_CRITICAL),
	/* Bit 7: No known consumer */
	/* Bit 8: Has a consumer, LTE depends on it. But unknown what and how */
	/* Bit 9: Big MPLL provider for Matrix. Candidate in the AXI election, thus critical */
	GATE(ZX297520V3_MPLL,              "matrix-mpll",    "mpll",          0x140,  9,
		CLK_IS_CRITICAL),
	/* Bit 10: Always 0 */
	/* Bit 11: Always 0 */
	/* Bit 12: No known consumer. Existing patterns indicate a dpll consumer in top itself */
	GATE(ZX297520V3_DPLL,              "matrix-dpll",    "dpll",          0x140, 13, 0),
	/* Bit 14: Always 0 */
	/* Bit 15: Always 0 */
	GATE(0,                            "top-upll-d12",   "upll-d12",      0x140, 16, 0),
	GATE(0,                            "top-upll",       "upll",          0x140, 17, 0),
	/* Bit 18: Always 0 */
	/* Bit 19: Always 0 */
	GATE(0,                            "top-gpll-d4",    "gpll-d4",       0x140, 20, 0),
	GATE(ZX297520V3_GPLL_D2,           "matrix-gpll-d2", "gpll-d2",       0x140, 21, 0),
	GATE(ZX297520V3_GPLL,              "matrix-gpll",    "gpll",          0x140, 22, 0),
	/* Bit 23: Always 0 */
	/* Bit 24 covers all 26 MHz on matrix, but not pclks. It is an AXI voting candidate.
	 * clock-26m consumers on topcrm to not use this, they get the ungated clock-26m.
	 */
	GATE(ZX297520V3_GATED_OSC26M,      "matrix-osc26m",  "clock-26m",     0x140, 24,
		CLK_IS_CRITICAL),

	/* AHB clock gates: Disabling these cuts off the A53 from register access into 0x130000/
	 * 0x140000, including the topcrm region, so it can't be re-enabled. Mark them critical for
	 * that reason.
	 *
	 * Gating this off does not stop the M0 rproc, nor does it stop M0 from accessing topcrm. It
	 * does however gate off M0 from the AXI interconnect, so it can't read RAM, Matrix, etc. So
	 * these bits here merely gate the bus, not the actual devices.
	 */
	GATE(ZX297520V3_AHB_PCLK,          "ahb-pclk",       "ahb-mux",        0x54, 12,
		CLK_IS_CRITICAL),
	GATE(ZX297520V3_AHB_WCLK,          "ahb-wclk",       "m0-mux",         0x54, 13,
		CLK_IS_CRITICAL),

	/* SRAM1 and 2 clocks. Leave them on for now, as turning them off carelessly hangs the M0 */
	GATE(ZX297520V3_SRAM1_PCLK,        "sram1-pclk",     clk_main[0],      0x54, 18,
		CLK_IS_CRITICAL),
	GATE(ZX297520V3_SRAM2_PCLK,        "sram2-pclk",     clk_main[0],      0x54, 16,
		CLK_IS_CRITICAL),

	/* Pinmux (AON, TOP, IOCFG but not PDCFG). Critical as well until we have a driver that
	 * consumes it. I don't think we'll realistically shut this off ever.
	 *
	 * Setting either bit 0 or 1 in register 0x58 makes the device work.
	 */
	GATE(ZX297520V3_PMM_WCLK,          "pmm-wclk",       clk_main[0],      0x58,  0,
		CLK_IS_CRITICAL),
	GATE(ZX297520V3_PMM_PCLK,          "pmm-pclk",       clk_main[0],      0x58,  1,
		CLK_IS_CRITICAL),

	/* Timers. We don't use any of them, just shut them off. The timers are named and sorted
	 * by the IO address of the main timer controls. Some of the controls are documented in
	 * ZTE's kernel, the others I found by trial and error.
	 *
	 * Timer T17 is used by the ZSP firmware. The rproc driver will enable it as needed.
	 */
	GATE(ZX297520V3_TIMER_T08_WCLK,    "timer-t08-wclk", "timer-t08-div",  0x5c,  8, 0),
	GATE(ZX297520V3_TIMER_T08_PCLK,    "timer-t08-pclk", clk_main[0],      0x5c,  9, 0),
	GATE(ZX297520V3_TIMER_T09_WCLK,    "timer-t09-wclk", "timer-t09-div",  0x5c,  4, 0),
	GATE(ZX297520V3_TIMER_T09_PCLK,    "timer-t09-pclk", clk_main[0],      0x5c,  5, 0),
	GATE(ZX297520V3_TIMER_T12_WCLK,    "timer-t12-wclk", "timer-t12-div",  0x54,  4, 0),
	GATE(ZX297520V3_TIMER_T12_PCLK,    "timer-t12-pclk", clk_main[0],      0x54,  5, 0),
	GATE(ZX297520V3_TIMER_T13_WCLK,    "timer-t13-wclk", "timer-t13-div",  0x60,  0, 0),
	GATE(ZX297520V3_TIMER_T13_PCLK,    "timer-t13-pclk", clk_main[0],      0x60,  1, 0),
	GATE(ZX297520V3_TIMER_T14_WCLK,    "timer-t14-wclk", "timer-t14-div",  0x60,  4, 0),
	GATE(ZX297520V3_TIMER_T14_PCLK,    "timer-t14-pclk", clk_main[0],      0x60,  5, 0),
	GATE(ZX297520V3_TIMER_T15_WCLK,    "timer-t15-wclk", "timer-t15-div",  0x54, 20, 0),
	GATE(ZX297520V3_TIMER_T15_PCLK,    "timer-t15-pclk", clk_main[0],      0x54, 21, 0),
	GATE(ZX297520V3_TIMER_T16_WCLK,    "timer-t16-wclk", "timer-t16-div",  0x60,  8, 0),
	GATE(ZX297520V3_TIMER_T16_PCLK,    "timer-t16-pclk", clk_main[0],      0x60,  9, 0),
	GATE(ZX297520V3_TIMER_T17_WCLK,    "timer-t17-wclk", "timer-t17-div", 0x128,  0, 0),
	GATE(ZX297520V3_TIMER_T17_PCLK,    "timer-t17-pclk", clk_main[0],     0x128,  1, 0),
	/* This watchdog is set up by the bootloader and in normal operation the m0 firmware will
	 * feed the dog. The m0 firmware in turn wants to be fed in its own way. Since we normally
	 * don't run any m0 firmware we shut it off by default and expose it to userspace via the
	 * watchdog driver.
	 */
	GATE(ZX297520V3_WDT_T18_WCLK,      "wdt-t18-wclk",   "wdt-t18-div",    0x54, 24, 0),
	GATE(ZX297520V3_WDT_T18_PCLK,      "wdt-t18-pclk",   clk_main[0],      0x54, 25, 0),

	GATE(ZX297520V3_I2C0_WCLK,         "i2c0-wclk",      "i2c0-mux",       0x54,  8, 0),
	GATE(ZX297520V3_I2C0_PCLK,         "i2c0-pclk",      clk_main[0],      0x54,  9, 0),
	GATE(ZX297520V3_UART0_WCLK,        "uart0-wclk",     "uart0-mux",      0x5c, 12, 0),
	GATE(ZX297520V3_UART0_PCLK,        "uart0-pclk",     clk_main[0],      0x5c, 13, 0),

	/* ZTE says the USB input is a 24 MHz clock based on mpll. Testing shows that Upll is not
	 * involved. The USB register space can be accessed with ahb-pclk gated, but not with
	 * ahb-wclk gated. ZTE also lists ahb-wclk as parent for the second clock.
	 *
	 * There doesn't seem to be a separate PHY clock. usb-wclk stops controller registers from
	 * updating, but doesn't cause the USB device to disconnect like asserting the PHY reset
	 * does. Bit 0 in this register can't be set, so no PHY clock is hiding there either.
	 */
	GATE(ZX297520V3_USB_WCLK,          "usb-wclk",      "top-mpll-d2-d13", 0x6c,  3, 0),
	GATE(ZX297520V3_USB_PCLK,          "usb-pclk",      "ahb-wclk",        0x6c,  4, 0),
	/* The HSIC hardware is listed in ZTE's code with a physical address of 0x01600000. Unlike
	 * the USB controller, it does consume upll. ZTE says 240 MHz, but 480 is the usual one for
	 * HSIC.
	 */
	GATE(ZX297520V3_HSIC_WCLK,         "hsic-wclk",     "top-upll",        0x6c,  1, 0),
	GATE(ZX297520V3_HSIC_PCLK,         "hsic-pclk",     "ahb-wclk",        0x6c,  2, 0),

	/* How does this RTC work? I don't know, the ZTE kernel does not talk to it. The actual RTC
	 * is on the I2C connected PMIC.
	 */
	GATE(ZX297520V3_RTC_WCLK,          "rtc-wclk",       clk_unknown,      0x54,  0, 0),
	GATE(ZX297520V3_RTC_PCLK,          "rtc-pclk",       clk_main[0],      0x54,  1, 0),

	GATE(ZX297520V3_USIM1_WCLK,        "usim1-wclk",     "usim1-div",      0x54, 28, 0),
	GATE(ZX297520V3_USIM1_PCLK,        "usim1-pclk",     clk_main[0],      0x54, 29, 0),

	/* LTE: gate only as far as I can see. I looked for resets and did not find any. There may
	 * be mux/div, but without understanding the behavior of this hardware it is impossible to
	 * tell. They are sorted by physical MMIO address of the devices, which happens to be the
	 * inverse order of the bits.
	 *
	 * I don't know what "LPM", "TD" and "W" mean. I copied them from ZTE's names.
	 */
	GATE(ZX297520V3_LPM_GSM_WCLK,      "lpm-gsm-wclk",   clk_unknown,      0x58, 10, 0),
	GATE(ZX297520V3_LPM_GSM_PCLK,      "lpm-gsm-pclk",   clk_main[0],      0x58, 11, 0),
	GATE(ZX297520V3_LPM_LTE_WCLK,      "lpm-lte-wclk",   clk_unknown,      0x58,  8, 0),
	GATE(ZX297520V3_LPM_LTE_PCLK,      "lpm-lte-pclk",   clk_main[0],      0x58,  9, 0),
	GATE(ZX297520V3_LPM_TD_WCLK,       "lpm-td-wclk",    clk_unknown,      0x58,  6, 0),
	GATE(ZX297520V3_LPM_TD_PCLK,       "lpm-td-pclk",    clk_main[0],      0x58,  7, 0),
	/* This one is called gsm_156m in cpko.ko, thus a candidate for an unknown 0x140 bit. */
	GATE(ZX297520V3_LPM_W_WCLK,        "lpm-w-wclk",     clk_unknown,      0x58,  4, 0),
	GATE(ZX297520V3_LPM_W_PCLK,        "lpm-w-pclk",     clk_main[0],      0x58,  5, 0),
	/* cpko.ko has a clock "gsm_48m" on bit 2, a candidate for an unknown 0x140 bit. */

	GATE(ZX297520V3_OUT0_WCLK,         "out0-wclk",      "out0-mux",       0x34,  0, 0),
	GATE(ZX297520V3_OUT1_WCLK,         "out1-wclk",      "out1-mux",       0x90,  2, 0),
	GATE(ZX297520V3_OUT2_WCLK,         "out2-wclk",      clk_main[0],      0x94,  2, 0),
	GATE(ZX297520V3_OUT32K_WCLK,       "out32k-wclk",    "clock-32k",      0x34,  1, 0),
	GATE(ZX297520V3_RMIIPHY_WCLK,      "rmiiphy-wclk",   "rmiiphy-mux",   0x11c,  2, 0),
};

static const struct zx_clk_data zx297520v3_topclk_data = {
	.inputs = zx297520v3_top_inputs,
	.num_inputs = ARRAY_SIZE(zx297520v3_top_inputs),
	.plls = zx297520v3_plls,
	.num_plls = ARRAY_SIZE(zx297520v3_plls),
	.fixed_divs = zx297520v3_top_fixed_divs,
	.num_fixed_divs = ARRAY_SIZE(zx297520v3_top_fixed_divs),
	.muxes = zx297520v3_top_muxes,
	.num_muxes = ARRAY_SIZE(zx297520v3_top_muxes),
	.divs = zx297520v3_top_dividers,
	.num_divs = ARRAY_SIZE(zx297520v3_top_dividers),
	.gates = zx297520v3_top_gates,
	.num_gates = ARRAY_SIZE(zx297520v3_top_gates),
};

/* For devices which have a working driver the work clock can be figured out by gating off registers
 * in top+0x140. This is used for devices where I can't interpret the register contents yet.
 */
static const char matrix_unk[] = "matrix-osc26m";

static const char * const cpu_sel[] = {
	"matrix-osc26m",
	"matrix-mpll",		/* 624 MHz */
	"matrix-mpll-d2",	/* 312 MHz */
	"matrix-mpll-d4",	/* 156 MHz */
};

/* Low confidence on the actual value, but at least I know it is bit 13 in top+0x140 */
static const char * const zsp_sel[] = {
	"matrix-osc26m",
	"matrix-dpll",		/* 491.52 MHz */
};

/* We can't realistically change DDR speed while running an OS out of DDR (it reads garbage for a
 * short while on transition), but we need to know if we have to keep gpll alive. 32 MB devices use
 * 200MHz, while 64/128 MB ones use 156.
 */
static const char * const ddr_ctrl_sel[] = {
	"matrix-mpll-d4",	/* 156 MHz */
	"matrix-gpll",		/* 200 MHz */
	"matrix-mpll-d6",	/* 104 MHz */
	"matrix-mpll-d8",	/* 78 MHz */
};

static const char * const sd0_sel[] = {
	"matrix-osc26m",
	"matrix-mpll-d4",	/* 156 MHz */
	"matrix-gpll-d2",	/* 100 MHz */
	"matrix-mpll-d8",	/* 78 MHz */
	"matrix-gpll-d2-d2",	/* 50 MHz */
	"matrix-gpll-d2-d4",	/* 25 MHz */
};

static const char * const sd1_sel[] = {
	"matrix-osc26m",
	"matrix-gpll-d2",	/* 100 MHz */
	"matrix-mpll-d8",	/* 78 MHz */
	"matrix-gpll-d2-d2",	/* 50 MHz */
	"matrix-mpll-d16",	/* 39 MHz */
	"matrix-gpll-d2-d4",	/* 25 MHz */
};

/* ZTE's kernel puts the high frequency first, but this is against the usual convention on this
 * SoC. I don't have any device that has a raw NAND controller though.
 */
static const char * const nand_sel[] = {
	"matrix-osc26m",
	"matrix-mpll-d4",	/* 156 MHz */
};

static const char * const edcp_sel[] = {
	"matrix-osc26m",
	"matrix-mpll-d4",	/* 156 MHz */
	"matrix-mpll-d5",	/* 124.8 MHz */
	"matrix-mpll-d6",	/* 104 MHz */
};

static const char * const tdm_sel[] = {
	"matrix-osc26m",
	"matrix-dpll-d4",	/* 122.88 MHz */
	"matrix-mpll-d6",	/* 104 MHz */
};

/* AXI speed is either determined by a fixed selection in 0x0, bits 0:2 or by a voting system with
 * ballots in 0x120, 0x124, 0x128. The voting system is enabled by setting 0x12c to 1. Otherwise the
 * selection in 0x0 applies.
 *
 * ZTE's firmware uses 0x128 as the PHY's vote and names 0x120 as "PS" and 0x124 as "AP" vote.
 * However, their cpufreq driver ultimately uses the "PS" define and thus writes to 0x120, which can
 * be observed in the running system.
 *
 * The fastest selected speed wins. This holds true for values 0 (26 MHz) and 7 (6.5 MHz) too, so it
 * isn't a case of lowest selector wins. I haven't tested what happens if mpll is changed to output
 * 5x the normal clock, so 7 beats 0. Presumably value 0 would still win the election.
 *
 * AXI speed 6.5 is an mpll child, not a division of [matrix-]osc26m by 4. It depends on top+0x140
 * bit 9, like other mpll (other than d5) choices.
 *
 * The system initializes with voting disabled and all preferences set to the lowest setting, 7.
 * The zx297520v3_matrix_init function transfers the global mux to our vote and enables voting.
 */
#define ZX297520V3_AXI_AP_VOTE 0x120
static const char * const axi_sel[] = {
	"matrix-osc26m",
	"matrix-mpll-d4",	/* 156 MHz */
	"matrix-mpll-d5",	/* 124.88 MHz */
	"matrix-mpll-d6",	/* 104 MHz */
	"matrix-mpll-d8",	/* 78 MHz */
	"matrix-mpll-d12",	/* 52 MHz */
	"matrix-mpll-d16",	/* 39 MHz */
	"matrix-mpll-d96",	/* 6.5 MHz */
};

static const struct zx_mux_desc zx297520v3_matrix_muxes[] = {
	MUX(0,                             "cpu-mux",        cpu_sel,          0x20,  0, 2),
	MUX(0,                             "zsp-mux",        zsp_sel,          0x30,  0, 2),
	MUX(0,                             "ddr-ctrl-mux",   ddr_ctrl_sel,     0x50,  0, 2),
	MUX(0,                             "sd0-mux",        sd0_sel,          0x50,  4, 3),
	MUX(0,                             "sd1-mux",        sd1_sel,          0x50,  8, 3),
	MUX(0,                             "nand-mux",       nand_sel,         0x50, 12, 2),
	MUX(0,                             "edcp-mux",       edcp_sel,         0x50, 16, 2),
	MUX(0,                             "tdm-mux",        tdm_sel,          0x50, 24, 2),
	MUX(0,                             "axi-mux", axi_sel, ZX297520V3_AXI_AP_VOTE, 0, 3),
};

static const struct zx_gate_desc zx297520v3_matrix_gates[] = {
	/* This bit cuts off the clock signal to the ARM architected timer, which the kernel uses
	 * as its main timer. It isn't critical per se - there are plenty of proprietary timers
	 * available that could be used - but the arm arch timer binding does not accept a clock, so
	 * this CCF driver won't know if the timer is in use.
	 *
	 * This clock is fed by the mux in topclk+0x140 - this critical clock here protects its
	 * parent.
	 */
	GATE(ZX297520V3_SYS_TIMER_WCLK,    "sys-timer-wclk", "matrix-osc26m", 0x144,  1,
		CLK_IS_CRITICAL),

	/* Both 0x24 and 0x28 bits 1 and 2 stop the CPU. There is also a bit in topclk+0x138, which
	 * ZTE's uboot calls "A53 reset", which also stops the CPU. I can't really tell the
	 * difference between matrix+28 and top+138. The clock (maxtrix+0x24) can be disabled and
	 * enabled from the Cortex M0 and it will nicely stop and restart the A53, retaining all
	 * state.
	 */
	GATE(ZX297520V3_CPU_WCLK,          "cpu-wclk",       "cpu-mux",        0x24,  1,
		CLK_IS_CRITICAL),
	GATE(ZX297520V3_CPU_PCLK,          "cpu-pclk",       clk_main[0],      0x24,  2,
		CLK_IS_CRITICAL),

	/* There are a lot more controls in matrix+0x100. 13-16 appear to be 4 different AXI
	 * channels for different priorities. Bit 19 appears to be the DDR PHY wclk.
	 *
	 * The important task is to keep gpll powered if the bootloader selected a gpll-based rate
	 * for RAM.
	 */
	GATE(ZX297520V3_DDR_CTRL_PCLK,     "ddr-ctrl-pclk",  clk_main[0],     0x100, 17,
		CLK_IS_CRITICAL),
	GATE(ZX297520V3_DDR_CTRL_WCLK,     "ddr-ctrl-wclk",  "ddr-ctrl-mux",  0x100, 18,
		CLK_IS_CRITICAL),

	GATE(ZX297520V3_ZSP_WCLK,          "zsp-wclk",       "zsp-mux",        0x3c,  0, 0),

	GATE(ZX297520V3_SD0_WCLK,          "sd0-wclk",       "sd0-mux",        0x54, 12, 0),
	GATE(ZX297520V3_SD0_PCLK,          "sd0-pclk",       clk_main[0],      0x54, 13, 0),
	GATE(ZX297520V3_SD0_CDET,          "sd0-cdet",       "clock-32k",      0x54, 14, 0),
	GATE(ZX297520V3_SD1_WCLK,          "sd1-wclk",       "sd1-mux",        0x54,  4, 0),
	GATE(ZX297520V3_SD1_PCLK,          "sd1-pclk",       clk_main[0],      0x54,  5, 0),
	/* I don't know how the cdet clock works. Card detection in the way the dwc,mmc driver uses
	 * it appears broken no matter this clock's setting.
	 */
	GATE(ZX297520V3_SD1_CDET,          "sd1-cdet",       "clock-32k",      0x54,  6, 0),

	/* This is some "denali" NAND, not the qspi connected one */
	GATE(ZX297520V3_NAND_WCLK,         "nand-wclk",      "nand-mux",       0x54, 20, 0),
	GATE(ZX297520V3_NAND_PCLK,         "nand-pclk",      clk_main[0],      0x54, 21, 0),

	/* There is a set of gates for an unknown device at matrix+0x60. This device sends 50 HZ
	 * interrupts to SPI+68. From testing it seems to be ZSP/LTE PHY related. Closing the gates
	 * stops the IRQs but breaks LTE and access to ZSP's TCM MMIO regions on ZTE's kernel.
	 * Interestingly the IRQs arrive when the ZSP clocks themselves are disabled or ZSP is in
	 * reset.
	 */

	/* Yes, WCLK bit > PCLK bit for EDCP */
	GATE(ZX297520V3_EDCP_WCLK,         "edcp-wclk",      "edcp-mux",       0x64,  2, 0),
	GATE(ZX297520V3_EDCP_PCLK,         "edcp-pclk",      clk_main[0],      0x64,  1, 0),

	/* This seems to be another SPI-Like device. ZTE's firmware operates it and from testing
	 * it looks like the matrix_osc26m gate in top+0x140 stops this device. This would be
	 * consistent with the other SPI controllers too.
	 */
	GATE(ZX297520V3_SSC_WCLK,          "ssc-wclk",       "matrix-osc26m",  0x84,  1, 0),
	GATE(ZX297520V3_SSC_PCLK,          "ssc-pclk",       clk_main[0],      0x84,  2, 0),

	/* PDCFG. Like PMM, either clock bit will allow the device to function. Probably there is
	 * no wclk line at all and the two bits are just an artifact of generally having two bits
	 * per device.
	 */
	GATE(ZX297520V3_PDCFG_WCLK,        "pdcfg-wclk",     matrix_unk,       0x88,  0,
		CLK_IS_CRITICAL),
	GATE(ZX297520V3_PDCFG_PCLK,        "pdcfg-pclk",     clk_main[0],      0x88,  1,
		CLK_IS_CRITICAL),
	GATE(ZX297520V3_MBOX_PCLK,         "mbox-pclk",      clk_main[0],      0x88,  2, 0),
	GATE(ZX297520V3_SRAM0_PCLK,        "sram0-pclk",     clk_main[0],      0x88,  4, 0),
	GATE(ZX297520V3_GSM_CFG_PCLK,      "gsm-cfg-pclk",   clk_main[0],      0x88,  8, 0),

	/* ZTE's driver has a statemt to the effect of *(top->base+0x11c) = 5, with a comment
	 * suggesting that this sets a 50 mhz clock. The clock code itself lists GMAC clocks in
	 * matrix+110 and lists the parents of these clock as 50mhz gpll output, but the downstream
	 * ZTE GMAC driver never enables the clocks. It turns out ZTE's code is highly misleading.
	 *
	 * The GMAC's work clock is definitly not any gpll output because it keeps working fine with
	 * gpll disabled. Gating off matrix_osc26m breaks GMAC, so it must be its parent.
	 *
	 * The GMAC Gates are left enabled by the boot loader and are required for the GMAC to work.
	 *
	 * As for the 50 MHz comment: See rmiiphy-wclk.
	 */
	GATE(ZX297520V3_GMAC_WCLK,         "gmac-wclk",      "matrix-osc26m", 0x110,  0, 0),
	GATE(ZX297520V3_GMAC_PCLK,         "gmac-pclk",      clk_main[0],     0x110,  1, 0),
	GATE(ZX297520V3_GMAC_AHB,          "gmac-ahb",       "ahb-wclk",      0x110,  2, 0),

	/* Is there an AXI bus gate? The symptom of cutting off the AXI mux selection in top+0x140
	 * is that matrixcrm becomes unreadable from m0 and A53 hangs. Inside matrix itself only 3
	 * bits fit that bill: 0x8c bits 5, 6, 7. It seems a bit self-defeating to have a clock
	 * gate that shuts off access to itself though. I expect a clock gate for the bus
	 * somewhere, and the mux exists, so exposing one AXI clock in the bindings is the correct
	 * thing to do. It also serves to tell the kernel to keep the mux's parent enabled.
	 *
	 * Register 0x8c has 12 settable bits (0xfff). Ultimately it doesn't matter much which
	 * do-not-remove bit we don't remove. Other bits in this register behave like gates (e.g
	 * bits 12:9 cut off USB temporarily), so I think we are looking in the right place.
	 *
	 * If an explanation for the remaining bits surfaces and they are further gates and/or
	 * resets, add them to the bindings.
	 */
	GATE(ZX297520V3_AXI_WCLK,          "axi-wclk",       "axi-mux",        0x8c,  5,
		CLK_IS_CRITICAL),

	GATE(ZX297520V3_DMA_PCLK,          "dma-pclk",       clk_main[0],      0x94,  3, 0),

	/* There are a lot more VOU related controls in these registers, but turning off the main
	 * clock seems to shut off the entire VOU MMIO range.
	 */
	GATE(ZX297520V3_VOU_WCLK,          "vou-wclk",       matrix_unk,      0x168,  0, 0),
	GATE(ZX297520V3_VOU_PCLK,          "vou-pclk",       clk_main[0],     0x168,  1, 0),

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
	 * in matrixclk and gets passed down. I2S has a mux in LSP, which can select the dpll-d4
	 * clock.
	 */
	GATE(ZX297520V3_LSP_MPLL_D5_WCLK,  "lsp-mpll-d5",   "matrix-mpll-d5",  0x7c,  0, 0),
	GATE(ZX297520V3_LSP_MPLL_D4_WCLK,  "lsp-mpll-d4",   "matrix-mpll-d4",  0x7c,  1, 0),
	GATE(ZX297520V3_LSP_MPLL_D6_WCLK,  "lsp-mpll-d6",   "matrix-mpll-d6",  0x7c,  2, 0),
	GATE(ZX297520V3_LSP_MPLL_D8_WCLK,  "lsp-mpll-d8",   "matrix-mpll-d8",  0x7c,  3, 0),
	GATE(ZX297520V3_LSP_MPLL_D12_WCLK, "lsp-mpll-d12",  "matrix-mpll-d12", 0x7c,  4, 0),
	GATE(ZX297520V3_LSP_OSC26M_WCLK,   "lsp-osc26m",    "matrix-osc26m",   0x7c,  5, 0),
	GATE(ZX297520V3_LSP_OSC32K_WCLK,   "lsp-osc32k",    "clock-32k",       0x7c,  6, 0),
	GATE(ZX297520V3_LSP_PCLK,          "lsp-pclk",      clk_main[0],       0x7c,  7, 0),
	GATE(ZX297520V3_LSP_TDM_WCLK,      "lsp-tdm-wclk",  "tdm-mux",         0x7c,  8, 0),
	GATE(ZX297520V3_LSP_DPLL_D4_WCLK,  "lsp-dpll-d4",   "matrix-dpll-d4",  0x7c,  9, 0),
};

static const char * const zx297520v3_matrix_inputs[] = {
	"osc26m", "osc32k",
	"mpll", "mpll-d5", "dpll", "gpll", "gpll-d2", "wclk-osc26m",
};

static const struct zx_fixed_divider_desc zx297520v3_matrix_fixed_divs[] = {
	{"matrix-mpll-d2",		"matrix-mpll",		 2},
	{"matrix-mpll-d4",		"matrix-mpll",		 4},
	/* div 5: provided as a separate line from topcrm */
	{"matrix-mpll-d6",		"matrix-mpll",		 6},
	{"matrix-mpll-d8",		"matrix-mpll",		 8},
	{"matrix-mpll-d12",		"matrix-mpll",		12},
	{"matrix-mpll-d16",		"matrix-mpll",		16},
	{"matrix-mpll-d96",		"matrix-mpll",		96},

	{"matrix-gpll-d2-d2",		"matrix-gpll-d2",	 2},
	{"matrix-gpll-d2-d4",		"matrix-gpll-d2",	 4},

	{"matrix-dpll-d4",		"matrix-dpll",		 4},
};

static int zx297520v3_matrix_init(struct regmap *map)
{
	u32 axi_val;
	int res;
	/* Read the global AXI speed selection, insert it into our ballot and enable voting */

	res = regmap_read(map, 0x0, &axi_val);
	if (res)
		return res;

	axi_val &= 0x7;
	res = regmap_write(map, ZX297520V3_AXI_AP_VOTE, axi_val);
	if (res)
		return res;

	/* If this fails for some reason our vote will simply be ignored */
	return regmap_write(map, 0x12c, 1);
}

static const struct zx_clk_data zx297520v3_matrixclk_data = {
	.init = zx297520v3_matrix_init,
	.inputs = zx297520v3_matrix_inputs,
	.num_inputs = ARRAY_SIZE(zx297520v3_matrix_inputs),
	.fixed_divs = zx297520v3_matrix_fixed_divs,
	.num_fixed_divs = ARRAY_SIZE(zx297520v3_matrix_fixed_divs),
	.muxes = zx297520v3_matrix_muxes,
	.num_muxes = ARRAY_SIZE(zx297520v3_matrix_muxes),
	.gates = zx297520v3_matrix_gates,
	.num_gates = ARRAY_SIZE(zx297520v3_matrix_gates),
};

static int clk_zx297520v3_probe(struct platform_device *pdev)
{
	const struct platform_device_id *id = platform_get_device_id(pdev);

	if (!id)
		return -ENODEV;

	return zx_clk_common_probe(&pdev->dev, pdev->dev.parent->of_node,
				   (const struct zx_clk_data *)id->driver_data);
}

static const struct platform_device_id clk_zx297520v3_ids[] = {
	{
		.name = "zx297520v3-topclk",
		.driver_data = (kernel_ulong_t)&zx297520v3_topclk_data,
	},
	{
		.name = "zx297520v3-matrixclk",
		.driver_data = (kernel_ulong_t)&zx297520v3_matrixclk_data,
	},
	{ }
};
MODULE_DEVICE_TABLE(platform, clk_zx297520v3_ids);

static struct platform_driver clk_zx297520v3 = {
	.probe = clk_zx297520v3_probe,
	.driver = {
		.name = "clk-zx297520v3",
	},
	.id_table = clk_zx297520v3_ids,
};
module_platform_driver(clk_zx297520v3);

MODULE_AUTHOR("Stefan Dösinger <stefandoesinger@gmail.com>");
MODULE_DESCRIPTION("ZTE zx297520v3 clock driver");
MODULE_LICENSE("GPL");

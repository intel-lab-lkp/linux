/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Renesas RZ/V2H(P) Clock Pulse Generator
 *
 * Copyright (C) 2024 Renesas Electronics Corp.
 */

#ifndef __RENESAS_RZV2H_CPG_H__
#define __RENESAS_RZV2H_CPG_H__

/**
 * Definitions of CPG Core Clocks
 *
 * These include:
 *   - Clock outputs exported to DT
 *   - External input clocks
 *   - Internal CPG clocks
 */
struct cpg_core_clk {
	const char *name;
	unsigned int id;
	unsigned int parent;
	unsigned int div;
	unsigned int mult;
	unsigned int type;
	unsigned int conf;
};

enum clk_types {
	/* Generic */
	CLK_TYPE_IN,		/* External Clock Input */
	CLK_TYPE_FF,		/* Fixed Factor Clock */
	CLK_TYPE_PLL,
};

#define DEF_TYPE(_name, _id, _type...) \
	{ .name = _name, .id = _id, .type = _type }
#define DEF_BASE(_name, _id, _type, _parent...) \
	DEF_TYPE(_name, _id, _type, .parent = _parent)
#define DEF_PLL(_name, _id, _parent, _conf) \
	DEF_TYPE(_name, _id, CLK_TYPE_PLL, .parent = _parent, .conf = _conf)
#define DEF_INPUT(_name, _id) \
	DEF_TYPE(_name, _id, CLK_TYPE_IN)
#define DEF_FIXED(_name, _id, _parent, _mult, _div) \
	DEF_BASE(_name, _id, CLK_TYPE_FF, _parent, .div = _div, .mult = _mult)

/**
 * struct rzv2h_mod_clk - Module Clocks definitions
 *
 * @name: handle between common and hardware-specific interfaces
 * @id: clock index in array containing all Core and Module Clocks
 * @parent: id of parent clock
 * @off: register offset
 * @bit: ON/MON bit
 * @monoff: monitor register offset
 * @monbit: monitor bit
 */
struct rzv2h_mod_clk {
	const char *name;
	unsigned int id;
	unsigned int parent;
	u16 off;
	u8 bit;
	u16 monoff;
	u8 monbit;
};

#define DEF_MOD_BASE(_name, _id, _parent, _off, _bit, _monoff, _monbit)	\
	{ \
		.name = _name, \
		.id = MOD_CLK_BASE + (_id), \
		.parent = (_parent), \
		.off = (_off), \
		.bit = (_bit), \
		.monoff = (_monoff), \
		.monbit = (_monbit), \
	}

#define DEF_MOD(_name, _id, _parent, _off, _bit, _monoff, _monbit)	\
	DEF_MOD_BASE(_name, _id, _parent, _off, _bit, _monoff, _monbit)

/**
 * struct rzv2h_reset - Reset definitions
 *
 * @off: reset register offset
 * @bit: reset bit
 * @monoff: monitor register offset
 * @monbit: monitor bit
 */
struct rzv2h_reset {
	u16 resoff;
	u8 resbit;
	u16 monoff;
	u8 monbit;
};

#define DEF_RST(_id, _resoff, _resbit, _monoff, _monbit)	\
	[_id] = { \
		.resoff = (_resoff), \
		.resbit = (_resbit), \
		.monoff = (_monoff), \
		.monbit = (_monbit) \
	}

/**
 * struct rzv2h_cpg_info - SoC-specific CPG Description
 *
 * @core_clks: Array of Core Clock definitions
 * @num_core_clks: Number of entries in core_clks[]
 * @num_total_core_clks: Total number of Core Clocks (exported + internal)
 *
 * @mod_clks: Array of Module Clock definitions
 * @num_mod_clks: Number of entries in mod_clks[]
 * @num_hw_mod_clks: Number of Module Clocks supported by the hardware
 *
 * @resets: Array of Module Reset definitions
 * @num_resets: Number of entries in resets[]
 *
 * @crit_mod_clks: Array with Module Clock IDs of critical clocks that
 *                 should not be disabled without a knowledgeable driver
 * @num_crit_mod_clks: Number of entries in crit_mod_clks[]
 * @pll_get_clk1_offset: Function pointer to get PLL CLK1 offset
 * @pll_get_clk2_offset: Function pointer to get PLL CLK2 offset
 */
struct rzv2h_cpg_info {
	/* Core Clocks */
	const struct cpg_core_clk *core_clks;
	unsigned int num_core_clks;
	unsigned int num_total_core_clks;

	/* Module Clocks */
	const struct rzv2h_mod_clk *mod_clks;
	unsigned int num_mod_clks;
	unsigned int num_hw_mod_clks;

	/* Resets */
	const struct rzv2h_reset *resets;
	unsigned int num_resets;

	/* Critical Module Clocks that should not be disabled */
	const unsigned int *crit_mod_clks;
	unsigned int num_crit_mod_clks;

	/* function pointers for PLL information */
	int (*pll_get_clk1_offset)(int clk);
	int (*pll_get_clk2_offset)(int clk);
};

extern const struct rzv2h_cpg_info r9a09g057_cpg_info;

#endif	/* __RENESAS_RZV2H_CPG_H__ */

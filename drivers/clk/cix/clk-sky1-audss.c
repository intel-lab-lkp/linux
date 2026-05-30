// SPDX-License-Identifier: GPL-2.0-only
// Copyright 2026 Cix Technology Group Co., Ltd.

#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/delay.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/regmap.h>
#include <linux/reset.h>

#include <dt-bindings/clock/sky1-audss.h>

#define INFO_HIFI0				0x00
#define INFO_CLK_GATE				0x10
#define INFO_CLK_DIV				0x14
#define INFO_CLK_MUX				0x18
#define INFO_MCLK				0x70

#define SKY1_AUDSS_CLK_PARENTS_CNT		6
#define AUDSS_CLK_PARENTS_MAX			SKY1_AUDSS_CLK_PARENTS_CNT

static u32 sky1_reg_save[][2] = {
	{ INFO_HIFI0,  0 },
	{ INFO_CLK_GATE,  0 },
	{ INFO_CLK_DIV, 0 },
	{ INFO_CLK_MUX, 0 },
	{ INFO_MCLK, 0 },
};

static const char *sky1_audss_clk_names[SKY1_AUDSS_CLK_PARENTS_CNT] = {
	"audio_clk0", "audio_clk1", "audio_clk2",
	"audio_clk3", "audio_clk4", "audio_clk5",
};

static const u32 sky1_clk_rate_default[SKY1_AUDSS_CLK_PARENTS_CNT] = {
	294912000,
	344064000,
	270950400,
	316108800,
	800000000,
	48000000,
};

static DEFINE_SPINLOCK(lock);
static struct clk_hw_onecell_data *clk_data;

static const char * const dsp_clk_parent[] = {
	"audio_clk4"
};

static const char * const dsp_bclk_parent[] = {
	"audio_clk4_div2"
};

static const char * const dsp_pbclk_parent[] = {
	"audio_clk4_div4"
};

static const char * const sram_axi_parent[] = {
	"audio_clk4_div2"
};

static const char * const hda_sys_parent[] = {
	"audio_clk4_div2"
};

static const char * const hda_hda_parent[] = {
	"audio_clk5"
};

static const char * const dmac_axi_parent[] = {
	"audio_clk4_div2"
};

static const char * const wdg_apb_parent[] = {
	"audio_clk5_div2"
};

static const char * const wdg_wdg_parent[] = {
	"audio_clk5_div2"
};

static const char * const timer_apb_parent[] = {
	"audio_clk4_div4"
};

static const char * const timer_timer_parent[] = {
	"audio_clk5_div2"
};

static const char * const mailbox_apb_parent[] = {
	"audio_clk4_div4"
};

static const char * const i2s_apb_parent[] = {
	"audio_clk4_div4"
};

static const char * const i2s0_parents[] = {
	"audio_clk0", "audio_clk2"
};

static const char * const i2s1_parents[] = {
	"audio_clk0", "audio_clk2"
};

static const char * const i2s2_parents[] = {
	"audio_clk0", "audio_clk2"
};

static const char * const i2s3_parents[] = {
	"audio_clk0", "audio_clk1", "audio_clk2", "audio_clk3"
};

static const char * const i2s4_parents[] = {
	"audio_clk0", "audio_clk1", "audio_clk2", "audio_clk3"
};

static const char * const i2s5_parents[] = {
	"audio_clk0", "audio_clk2"
};

static const char * const i2s6_parents[] = {
	"audio_clk0", "audio_clk2"
};

static const char * const i2s7_parents[] = {
	"audio_clk0", "audio_clk2"
};

static const char * const i2s8_parents[] = {
	"audio_clk0", "audio_clk2"
};

static const char * const i2s9_parents[] = {
	"audio_clk0", "audio_clk2"
};

static const char * const mclk_parents[] = {
	"audio_clk0", "audio_clk2"
};

/*
 * audss composite clock definition
 */
struct muxdiv_cfg {
	int offset;
	u8 shift;
	u8 width;
	u8 flags;
};

struct gate_cfg {
	int offset;
	u8 shift;
	u8 flags;
};

struct composite_clk_cfg {
	u32 id;
	const char *name;
	const char * const *parent_names;
	int num_parents;
	struct muxdiv_cfg *mux_cfg;
	struct muxdiv_cfg *div_cfg;
	struct gate_cfg *gate_cfg;
	unsigned long flags;
};

#define CFG(_id,\
	    _name,\
	    _parent_names,\
	    _mux_offset, _mux_shift, _mux_width, _mux_flags,\
	    _div_offset, _div_shift, _div_width, _div_flags,\
	    _gate_offset, _gate_shift, _gate_flags,\
	    _flags)\
{\
	.id = _id,\
	.name = _name,\
	.parent_names = _parent_names,\
	.num_parents = ARRAY_SIZE(_parent_names),\
	.mux_cfg = &(struct muxdiv_cfg) { _mux_offset, _mux_shift, _mux_width, _mux_flags },\
	.div_cfg = &(struct muxdiv_cfg) { _div_offset, _div_shift, _div_width, _div_flags },\
	.gate_cfg = &(struct gate_cfg) { _gate_offset, _gate_shift, _gate_flags },\
	.flags = _flags,\
}

static const struct composite_clk_cfg sky1_audss_clks[] = {
	/* dsp */
	CFG(CLK_DSP_CLK,
	    "audss_dsp_clk",
	    dsp_clk_parent,
	    -1, 0, 0, 0,
	    INFO_CLK_DIV, 0, 2, 0,
	    INFO_HIFI0, 0, 0,
	    CLK_GET_RATE_NOCACHE),
	CFG(CLK_DSP_BCLK,
	    "audss_dsp_bclk",
	    dsp_bclk_parent,
	    -1, 0, 0, 0,
	    INFO_CLK_DIV, 0, 2, 0,
	    -1, 0, 0,
	    CLK_GET_RATE_NOCACHE),
	CFG(CLK_DSP_PBCLK,
	    "audss_dsp_pbclk",
	    dsp_pbclk_parent,
	    -1, 0, 0, 0,
	    INFO_CLK_DIV, 0, 2, 0,
	    -1, 0, 0,
	    CLK_GET_RATE_NOCACHE),
	/* sram */
	CFG(CLK_SRAM_AXI,
	    "audss_sram_axi",
	    sram_axi_parent,
	    -1, 0, 0, 0,
	    INFO_CLK_DIV, 0, 2, 0,
	    INFO_CLK_GATE, 16, 0,
	    CLK_GET_RATE_NOCACHE),
	/* hda */
	CFG(CLK_HDA_SYS,
	    "audss_hda_sys",
	    hda_sys_parent,
	    -1, 0, 0, 0,
	    INFO_CLK_DIV, 0, 2, 0,
	    INFO_CLK_GATE, 14, 0,
	    CLK_GET_RATE_NOCACHE),
	CFG(CLK_HDA_HDA,
	    "audss_hda_hda",
	    hda_hda_parent,
	    -1, 0, 0, 0,
	    -1, 0, 0, 0,
	    INFO_CLK_GATE, 14, 0,
	    CLK_GET_RATE_NOCACHE),
	/* dmac */
	CFG(CLK_DMAC_AXI,
	    "audss_dmac_axi",
	    dmac_axi_parent,
	    -1, 0, 0, 0,
	    INFO_CLK_DIV, 0, 2, 0,
	    INFO_CLK_GATE, 15, 0,
	    CLK_GET_RATE_NOCACHE),
	/* wdg */
	CFG(CLK_WDG_APB,
	    "audss_wdg_apb",
	    wdg_apb_parent,
	    -1, 0, 0, 0,
	    -1, 0, 0, 0,
	    INFO_CLK_GATE, 10, 0,
	    CLK_GET_RATE_NOCACHE),
	CFG(CLK_WDG_WDG,
	    "audss_wdg_wdg",
	    wdg_wdg_parent,
	    -1, 0, 0, 0,
	    -1, 0, 0, 0,
	    INFO_CLK_GATE, 10, 0,
	    CLK_GET_RATE_NOCACHE),
	/* timer */
	CFG(CLK_TIMER_APB,
	    "audss_timer_apb",
	    timer_apb_parent,
	    -1, 0, 0, 0,
	    INFO_CLK_DIV, 0, 2, 0,
	    INFO_CLK_GATE, 11, 0,
	    CLK_GET_RATE_NOCACHE),
	CFG(CLK_TIMER_TIMER,
	    "audss_timer_timer",
	    timer_timer_parent,
	    -1, 0, 0, 0,
	    -1, 0, 0, 0,
	    INFO_CLK_GATE, 11, 0,
	    CLK_GET_RATE_NOCACHE),
	/* mailbox: mb0(ap->dsp), mb1(dsp->ap) */
	CFG(CLK_MB_0_APB,
	    "audss_mb_0_apb",
	    mailbox_apb_parent,
	    -1, 0, 0, 0,
	    -1, 0, 0, 0,
	    INFO_CLK_GATE, 12, 0,
	    CLK_GET_RATE_NOCACHE),
	CFG(CLK_MB_1_APB,
	    "audss_mb_1_apb",
	    mailbox_apb_parent,
	    -1, 0, 0, 0,
	    -1, 0, 0, 0,
	    INFO_CLK_GATE, 13, 0,
	    CLK_GET_RATE_NOCACHE),
	/* i2s */
	CFG(CLK_I2S0_APB,
	    "audss_i2s0_apb",
	    i2s_apb_parent,
	    -1, 0, 0, 0,
	    INFO_CLK_DIV, 0, 2, 0,
	    INFO_CLK_GATE, 0, 0,
	    CLK_GET_RATE_NOCACHE),
	CFG(CLK_I2S1_APB,
	    "audss_i2s1_apb",
	    i2s_apb_parent,
	    -1, 0, 0, 0,
	    INFO_CLK_DIV, 0, 2, 0,
	    INFO_CLK_GATE, 1, 0,
	    CLK_GET_RATE_NOCACHE),
	CFG(CLK_I2S2_APB,
	    "audss_i2s2_apb",
	    i2s_apb_parent,
	    -1, 0, 0, 0,
	    INFO_CLK_DIV, 0, 2, 0,
	    INFO_CLK_GATE, 2, 0,
	    CLK_GET_RATE_NOCACHE),
	CFG(CLK_I2S3_APB,
	    "audss_i2s3_apb",
	    i2s_apb_parent,
	    -1, 0, 0, 0,
	    INFO_CLK_DIV, 0, 2, 0,
	    INFO_CLK_GATE, 3, 0,
	    CLK_GET_RATE_NOCACHE),
	CFG(CLK_I2S4_APB,
	    "audss_i2s4_apb",
	    i2s_apb_parent,
	    -1, 0, 0, 0,
	    INFO_CLK_DIV, 0, 2, 0,
	    INFO_CLK_GATE, 4, 0,
	    CLK_GET_RATE_NOCACHE),
	CFG(CLK_I2S5_APB,
	    "audss_i2s5_apb",
	    i2s_apb_parent,
	    -1, 0, 0, 0,
	    INFO_CLK_DIV, 0, 2, 0,
	    INFO_CLK_GATE, 5, 0,
	    CLK_GET_RATE_NOCACHE),
	CFG(CLK_I2S6_APB,
	    "audss_i2s6_apb",
	    i2s_apb_parent,
	    -1, 0, 0, 0,
	    INFO_CLK_DIV, 0, 2, 0,
	    INFO_CLK_GATE, 6, 0,
	    CLK_GET_RATE_NOCACHE),
	CFG(CLK_I2S7_APB,
	    "audss_i2s7_apb",
	    i2s_apb_parent,
	    -1, 0, 0, 0,
	    INFO_CLK_DIV, 0, 2, 0,
	    INFO_CLK_GATE, 7, 0,
	    CLK_GET_RATE_NOCACHE),
	CFG(CLK_I2S8_APB,
	    "audss_i2s8_apb",
	    i2s_apb_parent,
	    -1, 0, 0, 0,
	    INFO_CLK_DIV, 0, 2, 0,
	    INFO_CLK_GATE, 8, 0,
	    CLK_GET_RATE_NOCACHE),
	CFG(CLK_I2S9_APB,
	    "audss_i2s9_apb",
	    i2s_apb_parent,
	    -1, 0, 0, 0,
	    INFO_CLK_DIV, 0, 2, 0,
	    INFO_CLK_GATE, 9, 0,
	    CLK_GET_RATE_NOCACHE),
	CFG(CLK_I2S0,
	    "audss_i2s0",
	    i2s0_parents,
	    INFO_CLK_MUX, 0, 2, 0,
	    INFO_CLK_DIV, 2, 2, 0,
	    INFO_CLK_GATE, 0, 0,
	    CLK_GET_RATE_NOCACHE),
	CFG(CLK_I2S1,
	    "audss_i2s1",
	    i2s1_parents,
	    INFO_CLK_MUX, 2, 2, 0,
	    INFO_CLK_DIV, 4, 2, 0,
	    INFO_CLK_GATE, 1, 0,
	    CLK_GET_RATE_NOCACHE),
	CFG(CLK_I2S2,
	    "audss_i2s2",
	    i2s2_parents,
	    INFO_CLK_MUX, 4, 2, 0,
	    INFO_CLK_DIV, 6, 2, 0,
	    INFO_CLK_GATE, 2, 0,
	    CLK_GET_RATE_NOCACHE),
	CFG(CLK_I2S3,
	    "audss_i2s3",
	    i2s3_parents,
	    INFO_CLK_MUX, 6, 2, 0,
	    INFO_CLK_DIV, 8, 2, 0,
	    INFO_CLK_GATE, 3, 0,
	    CLK_GET_RATE_NOCACHE),
	CFG(CLK_I2S4,
	    "audss_i2s4",
	    i2s4_parents,
	    INFO_CLK_MUX, 8, 2, 0,
	    INFO_CLK_DIV, 10, 2, 0,
	    INFO_CLK_GATE, 4, 0,
	    CLK_GET_RATE_NOCACHE),
	CFG(CLK_I2S5,
	    "audss_i2s5",
	    i2s5_parents,
	    INFO_CLK_MUX, 10, 2, 0,
	    INFO_CLK_DIV, 12, 2, 0,
	    INFO_CLK_GATE, 5, 0,
	    CLK_GET_RATE_NOCACHE),
	CFG(CLK_I2S6,
	    "audss_i2s6",
	    i2s6_parents,
	    INFO_CLK_MUX, 12, 2, 0,
	    INFO_CLK_DIV, 14, 2, 0,
	    INFO_CLK_GATE, 6, 0,
	    CLK_GET_RATE_NOCACHE),
	CFG(CLK_I2S7,
	    "audss_i2s7",
	    i2s7_parents,
	    INFO_CLK_MUX, 14, 2, 0,
	    INFO_CLK_DIV, 16, 2, 0,
	    INFO_CLK_GATE, 7, 0,
	    CLK_GET_RATE_NOCACHE),
	CFG(CLK_I2S8,
	    "audss_i2s8",
	    i2s8_parents,
	    INFO_CLK_MUX, 16, 2, 0,
	    INFO_CLK_DIV, 18, 2, 0,
	    INFO_CLK_GATE, 8, 0,
	    CLK_GET_RATE_NOCACHE),
	CFG(CLK_I2S9,
	    "audss_i2s9",
	    i2s9_parents,
	    INFO_CLK_MUX, 18, 2, 0,
	    INFO_CLK_DIV, 20, 2, 0,
	    INFO_CLK_GATE, 9, 0,
	    CLK_GET_RATE_NOCACHE),
	/* mclk */
	CFG(CLK_MCLK0,
	    "audss_mclk0",
	    mclk_parents,
	    INFO_MCLK, 5, 1, 0,
	    -1, 0, 0, 0,
	    INFO_MCLK, 0, 0,
	    CLK_GET_RATE_NOCACHE),
	CFG(CLK_MCLK1,
	    "audss_mclk1",
	    mclk_parents,
	    INFO_MCLK, 6, 1, 0,
	    -1, 0, 0, 0,
	    INFO_MCLK, 1, 0,
	    CLK_GET_RATE_NOCACHE),
	CFG(CLK_MCLK2,
	    "audss_mclk2",
	    mclk_parents,
	    INFO_MCLK, 7, 1, 0,
	    -1, 0, 0, 0,
	    INFO_MCLK, 2, 0,
	    CLK_GET_RATE_NOCACHE),
	CFG(CLK_MCLK3,
	    "audss_mclk3",
	    mclk_parents,
	    INFO_MCLK, 8, 1, 0,
	    -1, 0, 0, 0,
	    INFO_MCLK, 3, 0,
	    CLK_GET_RATE_NOCACHE),
	CFG(CLK_MCLK4,
	    "audss_mclk4",
	    mclk_parents,
	    INFO_MCLK, 9, 1, 0,
	    -1, 0, 0, 0,
	    INFO_MCLK, 4, 0,
	    CLK_GET_RATE_NOCACHE),
};

struct sky1_audss_clks_devtype_data {
	u32 (*reg_save)[2];
	size_t reg_save_size;
	const char **clk_names;
	size_t clk_num;
	const u32 *clk_rate_default;
	const struct composite_clk_cfg *clk_cfg;
	size_t clk_cfg_size;
};

struct sky1_audss_clks_priv {
	struct device *dev;
	struct regmap *regmap_cru;
	struct clk *clks[AUDSS_CLK_PARENTS_MAX];
	struct reset_control *rst_noc;
	const struct sky1_audss_clks_devtype_data *devtype_data;
};

/*
 * clk_ops for audss clock mux/divicer/gate
 */
struct sky1_clk_divider {
	struct clk_divider div;
	struct regmap *regmap;
	int offset;
};

struct sky1_clk_gate {
	struct clk_gate gate;
	struct regmap *regmap;
	int offset;
};

struct sky1_clk_mux {
	struct clk_mux mux;
	struct regmap *regmap;
	int offset;
};

static inline struct sky1_clk_mux *to_sky1_clk_mux(struct clk_mux *mux)
{
	return container_of(mux, struct sky1_clk_mux, mux);
}

static u8 sky1_audss_clk_mux_get_parent(struct clk_hw *hw)
{
	struct clk_mux *mux = to_clk_mux(hw);
	struct sky1_clk_mux *sky1_mux = to_sky1_clk_mux(mux);
	u32 val;

	regmap_read(sky1_mux->regmap, sky1_mux->offset, &val);
	val = val >> mux->shift;
	val &= mux->mask;

	return clk_mux_val_to_index(hw, mux->table, mux->flags, val);
}

static int sky1_audss_clk_mux_set_parent(struct clk_hw *hw, u8 index)
{
	struct clk_mux *mux = to_clk_mux(hw);
	struct sky1_clk_mux *sky1_mux = to_sky1_clk_mux(mux);
	u32 val = clk_mux_index_to_val(mux->table, mux->flags, index);
	unsigned long flags = 0;
	u32 reg;

	if (mux->lock)
		spin_lock_irqsave(mux->lock, flags);
	else
		__acquire(mux->lock);

	if (mux->flags & CLK_MUX_HIWORD_MASK) {
		reg = mux->mask << (mux->shift + 16);
	} else {
		regmap_read(sky1_mux->regmap, sky1_mux->offset, &reg);
		reg &= ~(mux->mask << mux->shift);
	}
	val = val << mux->shift;
	reg |= val;
	regmap_write(sky1_mux->regmap, sky1_mux->offset, reg);

	if (mux->lock)
		spin_unlock_irqrestore(mux->lock, flags);
	else
		__release(mux->lock);

	return 0;
}

static int sky1_audss_clk_mux_determine_rate(struct clk_hw *hw,
					     struct clk_rate_request *req)
{
	struct clk_mux *mux = to_clk_mux(hw);

	return clk_mux_determine_rate_flags(hw, req, mux->flags);
}

static const struct clk_ops sky1_audss_clk_mux_ops = {
	.get_parent = sky1_audss_clk_mux_get_parent,
	.set_parent = sky1_audss_clk_mux_set_parent,
	.determine_rate = sky1_audss_clk_mux_determine_rate,
};

static inline struct sky1_clk_divider *to_sky1_clk_divider(struct clk_divider *div)
{
	return container_of(div, struct sky1_clk_divider, div);
}

static unsigned long sky1_audss_clk_divider_recalc_rate(struct clk_hw *hw,
							unsigned long parent_rate)
{
	struct clk_divider *divider = to_clk_divider(hw);
	struct sky1_clk_divider *sky1_div = to_sky1_clk_divider(divider);
	unsigned int val;

	regmap_read(sky1_div->regmap, sky1_div->offset, &val);
	val = val >> divider->shift;
	val &= clk_div_mask(divider->width);

	return divider_recalc_rate(hw, parent_rate, val, divider->table,
				   divider->flags, divider->width);
}

static int sky1_audss_clk_divider_determine_rate(struct clk_hw *hw,
						 struct clk_rate_request *req)
{
	struct clk_divider *divider = to_clk_divider(hw);
	struct sky1_clk_divider *sky1_div = to_sky1_clk_divider(divider);

	/* if read only, just return current value */
	if (divider->flags & CLK_DIVIDER_READ_ONLY) {
		u32 val;

		regmap_read(sky1_div->regmap, sky1_div->offset, &val);
		val = val >> divider->shift;
		val &= clk_div_mask(divider->width);

		return divider_ro_determine_rate(hw, req, divider->table,
						 divider->width,
						 divider->flags, val);
	}

	return divider_determine_rate(hw, req, divider->table, divider->width,
				      divider->flags);
}

static int sky1_audss_clk_divider_set_rate(struct clk_hw *hw,
					   unsigned long rate,
					   unsigned long parent_rate)
{
	struct clk_divider *divider = to_clk_divider(hw);
	struct sky1_clk_divider *sky1_div = to_sky1_clk_divider(divider);
	int value;
	unsigned long flags = 0;
	u32 val;

	value = divider_get_val(rate, parent_rate, divider->table,
				divider->width, divider->flags);
	if (value < 0)
		return value;

	if (divider->lock)
		spin_lock_irqsave(divider->lock, flags);
	else
		__acquire(divider->lock);

	if (divider->flags & CLK_DIVIDER_HIWORD_MASK) {
		val = clk_div_mask(divider->width) << (divider->shift + 16);
	} else {
		regmap_read(sky1_div->regmap, sky1_div->offset, &val);
		val &= ~(clk_div_mask(divider->width) << divider->shift);
	}
	val |= (u32)value << divider->shift;
	regmap_write(sky1_div->regmap, sky1_div->offset, val);

	if (divider->lock)
		spin_unlock_irqrestore(divider->lock, flags);
	else
		__release(divider->lock);

	return 0;
}

static const struct clk_ops sky1_audss_clk_divider_ops = {
	.recalc_rate = sky1_audss_clk_divider_recalc_rate,
	.determine_rate = sky1_audss_clk_divider_determine_rate,
	.set_rate = sky1_audss_clk_divider_set_rate,
};

static inline struct sky1_clk_gate *to_sky1_clk_gate(struct clk_gate *gate)
{
	return container_of(gate, struct sky1_clk_gate, gate);
}

static void sky1_audss_clk_gate_endisable(struct clk_hw *hw, int enable)
{
	struct clk_gate *gate = to_clk_gate(hw);
	struct sky1_clk_gate *sky1_gate = to_sky1_clk_gate(gate);
	int set = gate->flags & CLK_GATE_SET_TO_DISABLE ? 1 : 0;
	unsigned long flags;
	u32 reg;

	set ^= enable;

	if (gate->lock)
		spin_lock_irqsave(gate->lock, flags);
	else
		__acquire(gate->lock);

	if (gate->flags & CLK_GATE_HIWORD_MASK) {
		reg = BIT(gate->bit_idx + 16);
		if (set)
			reg |= BIT(gate->bit_idx);
	} else {
		regmap_read(sky1_gate->regmap, sky1_gate->offset, &reg);

		if (set)
			reg |= BIT(gate->bit_idx);
		else
			reg &= ~BIT(gate->bit_idx);
	}

	regmap_write(sky1_gate->regmap, sky1_gate->offset, reg);

	if (gate->lock)
		spin_unlock_irqrestore(gate->lock, flags);
	else
		__release(gate->lock);
}

static int sky1_audss_clk_gate_enable(struct clk_hw *hw)
{
	sky1_audss_clk_gate_endisable(hw, 1);

	return 0;
}

static void sky1_audss_clk_gate_disable(struct clk_hw *hw)
{
	sky1_audss_clk_gate_endisable(hw, 0);
}

static int sky1_audss_clk_gate_is_enabled(struct clk_hw *hw)
{
	u32 reg;
	struct clk_gate *gate = to_clk_gate(hw);
	struct sky1_clk_gate *sky1_gate = to_sky1_clk_gate(gate);

	regmap_read(sky1_gate->regmap, sky1_gate->offset, &reg);

	/* if a set bit disables this clk, flip it before masking */
	if (gate->flags & CLK_GATE_SET_TO_DISABLE)
		reg ^= BIT(gate->bit_idx);

	reg &= BIT(gate->bit_idx);

	return reg ? 1 : 0;
}

static const struct clk_ops sky1_audss_clk_gate_ops = {
	.enable = sky1_audss_clk_gate_enable,
	.disable = sky1_audss_clk_gate_disable,
	.is_enabled = sky1_audss_clk_gate_is_enabled,
};

static struct clk_hw *sky1_audss_clk_register(struct device *dev,
					      const char *name,
					      const char * const *parent_names,
					      int num_parents,
					      struct regmap *regmap,
					      struct muxdiv_cfg *mux_cfg,
					      struct muxdiv_cfg *div_cfg,
					      struct gate_cfg *gate_cfg,
					      unsigned long flags,
					      spinlock_t *lock)
{
	const struct clk_ops *sky1_mux_ops = NULL;
	const struct clk_ops *sky1_div_ops = NULL;
	const struct clk_ops *sky1_gate_ops = NULL;
	struct clk_hw *hw = ERR_PTR(-ENOMEM);
	struct sky1_clk_divider *sky1_div = NULL;
	struct sky1_clk_gate *sky1_gate = NULL;
	struct sky1_clk_mux *sky1_mux = NULL;

	if (mux_cfg->offset >= 0) {
		sky1_mux = devm_kzalloc(dev, sizeof(*sky1_mux), GFP_KERNEL);
		if (!sky1_mux)
			return ERR_PTR(-ENOMEM);

		sky1_mux->mux.reg = NULL;
		sky1_mux->mux.shift = mux_cfg->shift;
		sky1_mux->mux.mask = BIT(mux_cfg->width) - 1;
		sky1_mux->mux.flags = mux_cfg->flags;
		sky1_mux->mux.lock = lock;
		sky1_mux_ops = &sky1_audss_clk_mux_ops;
		sky1_mux->regmap = regmap;
		sky1_mux->offset = mux_cfg->offset;
	}

	if (div_cfg->offset >= 0) {
		sky1_div = devm_kzalloc(dev, sizeof(*sky1_div), GFP_KERNEL);
		if (!sky1_div)
			return ERR_PTR(-ENOMEM);

		sky1_div->div.reg = NULL;
		sky1_div->div.shift = div_cfg->shift;
		sky1_div->div.width = div_cfg->width;
		sky1_div->div.flags = div_cfg->flags | CLK_DIVIDER_POWER_OF_TWO;
		sky1_div->div.lock = lock;
		sky1_div_ops = &sky1_audss_clk_divider_ops;
		sky1_div->regmap = regmap;
		sky1_div->offset = div_cfg->offset;
	}

	if (gate_cfg->offset >= 0) {
		sky1_gate = devm_kzalloc(dev, sizeof(*sky1_gate), GFP_KERNEL);
		if (!sky1_gate)
			return ERR_PTR(-ENOMEM);

		sky1_gate->gate.reg = NULL;
		sky1_gate->gate.bit_idx = gate_cfg->shift;
		sky1_gate->gate.flags = gate_cfg->flags;
		sky1_gate->gate.lock = lock;
		sky1_gate_ops = &sky1_audss_clk_gate_ops;
		sky1_gate->regmap = regmap;
		sky1_gate->offset = gate_cfg->offset;
	}

	hw = clk_hw_register_composite(dev, name, parent_names, num_parents,
				       sky1_mux ? &sky1_mux->mux.hw : NULL, sky1_mux_ops,
				       sky1_div ? &sky1_div->div.hw : NULL, sky1_div_ops,
				       sky1_gate ? &sky1_gate->gate.hw : NULL, sky1_gate_ops,
				       flags);
	if (IS_ERR(hw)) {
		dev_err(dev, "register %s clock failed with err = %ld\n",
			name, PTR_ERR(hw));
		return ERR_CAST(hw);
	}

	return hw;
}

static int sky1_audss_clks_get(struct sky1_audss_clks_priv *priv)
{
	const struct sky1_audss_clks_devtype_data *devtype_data = priv->devtype_data;
	int i;

	for (i = 0; i < devtype_data->clk_num; i++) {
		priv->clks[i] = devm_clk_get(priv->dev, devtype_data->clk_names[i]);
		if (IS_ERR(priv->clks[i]))
			return dev_err_probe(priv->dev, PTR_ERR(priv->clks[i]),
					     "failed to get clock %s", devtype_data->clk_names[i]);
	}

	return 0;
}

static int sky1_audss_clks_enable(struct sky1_audss_clks_priv *priv)
{
	const struct sky1_audss_clks_devtype_data *devtype_data = priv->devtype_data;
	int i, err;

	for (i = 0; i < devtype_data->clk_num; i++) {
		err = clk_prepare_enable(priv->clks[i]);
		if (err) {
			dev_err(priv->dev, "failed to enable clock %s\n",
				devtype_data->clk_names[i]);
			goto err_clks;
		}
	}

	return 0;

err_clks:
	while (--i >= 0)
		clk_disable_unprepare(priv->clks[i]);

	return err;
}

static void sky1_audss_clks_disable(struct sky1_audss_clks_priv *priv)
{
	const struct sky1_audss_clks_devtype_data *devtype_data = priv->devtype_data;
	int i;

	for (i = 0; i < devtype_data->clk_num; i++)
		clk_disable_unprepare(priv->clks[i]);
}

static int sky1_audss_clks_set_rate(struct sky1_audss_clks_priv *priv)
{
	const struct sky1_audss_clks_devtype_data *devtype_data = priv->devtype_data;
	int i, err;

	for (i = 0; i < devtype_data->clk_num; i++) {
		err = clk_set_rate(priv->clks[i], devtype_data->clk_rate_default[i]);
		if (err) {
			dev_err(priv->dev, "failed to set clock rate %s\n",
				devtype_data->clk_names[i]);
			return err;
		}
	}

	return 0;
}

/* register sky1 audio subsystem clocks */
static int sky1_audss_clk_probe(struct platform_device *pdev)
{
	const struct sky1_audss_clks_devtype_data *devtype_data;
	struct sky1_audss_clks_priv *priv;
	struct device_node *parent_np;
	struct device *dev = &pdev->dev;
	struct reset_control *rst_noc;
	struct clk_hw **clk_table;
	struct regmap *regmap_cru;
	int i, ret;

	parent_np = of_get_parent(pdev->dev.of_node);
	regmap_cru = syscon_node_to_regmap(parent_np);
	of_node_put(parent_np);
	if (IS_ERR(regmap_cru))
		return dev_err_probe(dev, PTR_ERR(regmap_cru),
				     "unable to get audss cru regmap");

	devtype_data = of_device_get_match_data(dev);
	if (!devtype_data)
		return -ENODEV;

	clk_data = devm_kzalloc(&pdev->dev,
				struct_size(clk_data, hws, AUDSS_MAX_CLKS),
				GFP_KERNEL);
	if (!clk_data)
		return -ENOMEM;

	clk_data->num = AUDSS_MAX_CLKS;
	clk_table = clk_data->hws;

	priv = devm_kzalloc(&pdev->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->dev = dev;
	priv->regmap_cru = regmap_cru;
	priv->devtype_data = devtype_data;

	ret = sky1_audss_clks_get(priv);
	if (ret)
		return ret;

	rst_noc = devm_reset_control_get(dev, NULL);
	if (IS_ERR(rst_noc))
		return dev_err_probe(dev, PTR_ERR(rst_noc),
				     "failed to get noc reset");
	priv->rst_noc = rst_noc;

	platform_set_drvdata(pdev, priv);

	/*
	 * Enable runtime PM here to allow the clock core using runtime PM
	 * for the registered clocks.
	 */
	pm_runtime_get_noresume(dev);
	pm_runtime_set_active(dev);
	pm_runtime_enable(dev);

	ret = sky1_audss_clks_enable(priv);
	if (ret) {
		dev_err(dev, "failed to enable clocks\n");
		goto fail_clks_enable;
	}

	ret = sky1_audss_clks_set_rate(priv);
	if (ret) {
		dev_err(dev, "failed to set clocks rate\n");
		goto fail_clks_set;
	}

	/* assert reset */
	reset_control_assert(rst_noc);
	usleep_range(1, 2);

	/* deassert reset */
	reset_control_deassert(rst_noc);

	/* audio_clk4 clock fixed divider */
	clk_table[CLK_AUD_CLK4_DIV2] =
		devm_clk_hw_register_fixed_factor(dev,
						  "audio_clk4_div2",
						  "audio_clk4",
						  CLK_GET_RATE_NOCACHE,
						  1, 2);
	if (IS_ERR(clk_table[CLK_AUD_CLK4_DIV2])) {
		ret = PTR_ERR(clk_table[CLK_AUD_CLK4_DIV2]);
		dev_err(dev, "failed to register clock %d, ret:%d\n", CLK_AUD_CLK4_DIV2, ret);
		goto fail_fixed_clk;
	}

	clk_table[CLK_AUD_CLK4_DIV4] =
		devm_clk_hw_register_fixed_factor(dev,
						  "audio_clk4_div4",
						  "audio_clk4",
						  CLK_GET_RATE_NOCACHE,
						  1, 4);
	if (IS_ERR(clk_table[CLK_AUD_CLK4_DIV4])) {
		ret = PTR_ERR(clk_table[CLK_AUD_CLK4_DIV4]);
		dev_err(dev, "failed to register clock %d, ret:%d\n", CLK_AUD_CLK4_DIV4, ret);
		goto fail_fixed_clk;
	}

	/* audio_clk5 clock fixed divider */
	clk_table[CLK_AUD_CLK5_DIV2] =
		devm_clk_hw_register_fixed_factor(dev,
						  "audio_clk5_div2",
						  "audio_clk5",
						  CLK_GET_RATE_NOCACHE,
						  1, 2);
	if (IS_ERR(clk_table[CLK_AUD_CLK5_DIV2])) {
		ret = PTR_ERR(clk_table[CLK_AUD_CLK5_DIV2]);
		dev_err(dev, "failed to register clock %d, ret:%d\n", CLK_AUD_CLK5_DIV2, ret);
		goto fail_fixed_clk;
	}

	for (i = 0; i < devtype_data->clk_cfg_size; i++) {
		clk_table[devtype_data->clk_cfg[i].id] =
			sky1_audss_clk_register(dev,
						devtype_data->clk_cfg[i].name,
						devtype_data->clk_cfg[i].parent_names,
						devtype_data->clk_cfg[i].num_parents,
						regmap_cru,
						devtype_data->clk_cfg[i].mux_cfg,
						devtype_data->clk_cfg[i].div_cfg,
						devtype_data->clk_cfg[i].gate_cfg,
						devtype_data->clk_cfg[i].flags,
						&lock);
		if (IS_ERR(clk_table[devtype_data->clk_cfg[i].id])) {
			ret = PTR_ERR(clk_table[devtype_data->clk_cfg[i].id]);
			dev_err(dev, "failed to register clock %d, ret:%d\n",
				devtype_data->clk_cfg[i].id, ret);
			goto fail_array_clk;
		}
	}

	ret = devm_of_clk_add_hw_provider(dev, of_clk_hw_onecell_get, clk_data);
	if (ret) {
		dev_err(dev, "failed to add clock provider: %d\n", ret);
		goto fail_register;
	}

	pm_runtime_put_sync(dev);

	return 0;

fail_register:
fail_array_clk:
	while (i--)
		clk_hw_unregister_composite(clk_table[devtype_data->clk_cfg[i].id]);
fail_fixed_clk:
fail_clks_set:
	pm_runtime_put_sync(dev);
fail_clks_enable:
	pm_runtime_disable(dev);
	return ret;
}

static void sky1_audss_clk_remove(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct sky1_audss_clks_priv *priv = dev_get_drvdata(dev);
	const struct sky1_audss_clks_devtype_data *devtype_data = priv->devtype_data;
	int i = 0;

	for (i = 0; i < devtype_data->clk_cfg_size; i++)
		clk_hw_unregister_composite(clk_data->hws[devtype_data->clk_cfg[i].id]);

	if (!pm_runtime_status_suspended(dev))
		pm_runtime_force_suspend(dev);

	pm_runtime_disable(dev);
}

static int __maybe_unused sky1_audss_clk_runtime_suspend(struct device *dev)
{
	struct sky1_audss_clks_priv *priv = dev_get_drvdata(dev);
	const struct sky1_audss_clks_devtype_data *devtype_data = priv->devtype_data;
	int i;

	for (i = 0; i < devtype_data->reg_save_size; i++)
		regmap_read(priv->regmap_cru,
			    devtype_data->reg_save[i][0], &devtype_data->reg_save[i][1]);

	sky1_audss_clks_disable(priv);

	return 0;
}

static int __maybe_unused sky1_audss_clk_runtime_resume(struct device *dev)
{
	struct sky1_audss_clks_priv *priv = dev_get_drvdata(dev);
	const struct sky1_audss_clks_devtype_data *devtype_data = priv->devtype_data;
	int i, ret;

	ret = sky1_audss_clks_enable(priv);
	if (ret) {
		dev_err(dev, "failed to enable clocks\n");
		return ret;
	}

	reset_control_deassert(priv->rst_noc);

	for (i = 0; i < priv->devtype_data->reg_save_size; i++)
		regmap_write(priv->regmap_cru,
			     devtype_data->reg_save[i][0], devtype_data->reg_save[i][1]);

	return 0;
}

static const struct dev_pm_ops sky1_audss_clk_pm_ops = {
	SET_RUNTIME_PM_OPS(sky1_audss_clk_runtime_suspend,
			   sky1_audss_clk_runtime_resume, NULL)
	SET_LATE_SYSTEM_SLEEP_PM_OPS(pm_runtime_force_suspend,
				     pm_runtime_force_resume)
};

static const struct sky1_audss_clks_devtype_data sky1_devtype_data = {
	.reg_save = sky1_reg_save,
	.reg_save_size = ARRAY_SIZE(sky1_reg_save),
	.clk_names = sky1_audss_clk_names,
	.clk_num = ARRAY_SIZE(sky1_audss_clk_names),
	.clk_rate_default = sky1_clk_rate_default,
	.clk_cfg = sky1_audss_clks,
	.clk_cfg_size = ARRAY_SIZE(sky1_audss_clks),
};

static const struct of_device_id sky1_audss_clk_of_match[] = {
	{ .compatible = "cix,sky1-audss-clock", .data = &sky1_devtype_data, },
	{ /* sentinel */ },
};
MODULE_DEVICE_TABLE(of, sky1_audss_clk_of_match);

static struct platform_driver sky1_audss_clk_driver = {
	.probe = sky1_audss_clk_probe,
	.remove = sky1_audss_clk_remove,
	.driver = {
		.name = "sky1-audss-clk",
		.suppress_bind_attrs = true,
		.of_match_table = sky1_audss_clk_of_match,
		.pm = &sky1_audss_clk_pm_ops,
	},
};
module_platform_driver(sky1_audss_clk_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Joakim Zhang <joakim.zhang@cixtech.com>");
MODULE_DESCRIPTION("Cixtech Sky1 Audio Subsystem Clock Controller Driver");

// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2020 Unisoc Inc.

#include <linux/hwspinlock.h>
#include <linux/iio/iio.h>
#include <linux/math64.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/nvmem-consumer.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>
#include <linux/slab.h>
#include <linux/sort.h>

/* ADC controller registers definition */
#define SPRD_ADC_CTL			0x0

#define SPRD_ADC_CH_CFG			0x4
#define SPRD_ADC_CHN_ID_MASK		GENMASK(4, 0)

#define SPRD_ADC_DATA			0x4c
#define SPRD_ADC_DATA_MASK		GENMASK(11, 0)

#define SPRD_ADC_INT_EN			0x50
#define SPRD_ADC_IRQ_EN			BIT(0)

#define SPRD_ADC_INT_CLR		0x54
#define SPRD_ADC_IRQ_CLR		BIT(0)

#define SPRD_ADC_INT_STS		0x58

#define SPRD_ADC_INT_RAW		0x5c
#define SPRD_ADC_IRQ_RAW		BIT(0)

/* Bits and mask definition for SPRD_ADC_CTL register */
#define SPRD_ADC_EN			BIT(0)
#define SPRD_ADC_CHN_RUN		BIT(1)
#define SPRD_ADC_12BIT_MODE		BIT(2)
#define SPRD_ADC_RUN_NUM_MASK		GENMASK(7, 4)
#define SPRD_ADC_RUN_NUM_SHIFT		4
#define SPRD_ADC_AVERAGE_SHIFT		8
#define SPRD_ADC_AVERAGE_MASK		GENMASK(10, 8)

/* Timeout (ms) for the trylock of hardware spinlocks */
#define SPRD_ADC_HWLOCK_TIMEOUT		5000

/* Maximum ADC channel number */
#define SPRD_ADC_CHANNEL_MAX		32

/* Timeout (us) for ADC data conversion according to ADC datasheet */
#define SPRD_ADC_RDY_TIMEOUT		1000000
#define SPRD_ADC_POLL_RAW_STATUS	500

/* ADC voltage ratio definition */
#define SPRD_RATIO_NUMERATOR_OFFSET	16
#define SPRD_RATIO_DENOMINATOR_MASK	GENMASK(15, 0)
#define RATIO(n, d)			(((n) << SPRD_RATIO_NUMERATOR_OFFSET) | (d))
#define R_INVALID			(0xffffffff)
#define RATIO_DEF			RATIO(1, 1)

/* ADC specific channel reference voltage 3.5V */
#define SPRD_ADC_REFVOL_VDD35		3500000

/* ADC default channel reference voltage is 2.8V */
#define SPRD_ADC_REFVOL_VDD28		2800000

#define SPRD_ADC_CELL_MAX		(2)
#define SPRD_ADC_INVALID_DATA		(0XFFFFFFFF)
#define SPRD_ADC_INIT_MAGIC		(0xa7a77a7a)
#define ADC_MESURE_NUMBER_SW		(15)
#define ADC_MESURE_NUMBER_HW_DEF	(3) /* 2 << 3 = 8 times */

#define CH_DATA_FMT "ch%d_d[(%d %d 0x%x 0x%x) | (%u/%u), (%u/%u), (%u/%u), (%u/%u), (%u/%u)]\n"

#define SPRD_ADC_CHANNEL(index, mask) {				\
	.type = IIO_VOLTAGE,					\
	.channel = index,					\
	.info_mask_separate = mask | BIT(IIO_CHAN_INFO_SCALE),	\
	.datasheet_name = "CH##index",				\
	.indexed = 1,						\
}

#define CH_DATA_INIT(sl, filter, isen, ip_r)		\
{							\
	.scale = sl,					\
	.isen_info = isen,				\
	.filter_info = filter,				\
	.ip_ratio = ip_r,				\
	.ch_aux_ratio = {0, 0, 0, 0},			\
	.inited = SPRD_ADC_INIT_MAGIC,			\
}

#define CALIB_INFO_INIT(ch, sl, gph)	\
{					\
	.channel = ch,			\
	.scale = sl,			\
	.graph = gph,			\
	.inited = SPRD_ADC_INIT_MAGIC,	\
}

#define GRAPH_POINT_INIT(v0, a0, v1, a1)\
					\
	.volt0 = v0,			\
	.adc0 = a0,			\
	.volt1 = v1,			\
	.adc1 = a1,			\
	.inited = SPRD_ADC_INIT_MAGIC

#define REG_BIT_INIT(b_base, reg_address, b_mask, b_offset, set, clr, func, verse)	\
{										\
	.base = b_base,								\
	.reg_addr = reg_address,						\
	.mask = b_mask,								\
	.offset = b_offset,							\
	.val_set = set,								\
	.val_clr = clr,								\
	.inited = SPRD_ADC_INIT_MAGIC,						\
	.get_val = func,							\
	.reverse = verse							\
}

enum SPRD_PMIC_TYPE {
	UMP9620_ADC,
	UMP518_ADC,
};

enum SPRD_ADC_GRAPH_TYPE {
	TWO_CELL_GRAPH,
	SPRD_ADC_GRAPH_TYPE_MAX
};

enum SPRD_ADC_GRAPH_OFFSET {
	GRAPH_BIG,
	GRAPH_SMALL,
	GRAPH_VBAT_DET,
	GRAPH_MAX
};

enum SPRD_ADC_SCALE {
	SCALE_00,
	SCALE_01,
	SCALE_10,
	SCALE_11,
	SPRD_ADC_SCALE_MAX
};

enum SPRD_ADC_REG_TYPE {
	REG_MODULE_EN,
	REG_CLK_EN,
	REG_SOFT_RST,
	REG_XTL_CTRL,
	REG_SCALE,
	REG_CFG_FIXED_ST = 11,/* FIXED CFG START */
	REG_CAL_EN,
	REG_NTC_MUX,
	REG_CFG_FIXED_END,/* FIXED CFG END */
	REG_ISEN_ST = 21,/* CURRENT MODE START */
	REG_ISEN0,
	REG_ISEN1,
	REG_ISEN2,
	REG_ISEN3,
	REG_ISEN_END,/* CURRENT MODE END */
	SPRD_ADC_REG_TYPE_MAX
};

enum SPRD_ADC_REG_BASE {
	BASE_GLB,
	BASE_ANA
};

struct sprd_adc_pm_data {
	struct regmap *clk_regmap;
	u32 clk_reg;/* adc clk26 vote reg */
	u32 clk_reg_mask;/* adc clk26 vote reg mask */
	bool dev_suspended;
};

/*bit[0-7]: scale
 *bit[8-15]: filter_info(bit8: sw filter support, bit[9-15]: hw filter val(2<<n))
 *bit[16-23]: isen_info (bit16: isen support, bit[17-23]: isen val)
 */
struct sprd_adc_channel_data {
	int scale;
	int graph;
	int isen_info;
	int filter_info;
	int ip_ratio;
	int ch_aux_ratio[SPRD_ADC_SCALE_MAX];
	int inited;
};

struct calib_info {
	int channel;
	int scale;
	int graph;
	int ch_aux_ratio[SPRD_ADC_SCALE_MAX];
	int inited;
};

struct reg_bit {
	u32 base;
	u32 reg_addr;
	u32 mask;
	u32 offset;
	u32 val_set;
	u32 val_clr;
	u32 inited;
	bool reverse;
	u32 (*get_val)(void *pri, int ch, bool set);
};

struct sprd_adc_data {
	struct iio_dev *indio_dev;
	struct device *dev;
	struct regmap *regmap;
	/* lock to protect against multiple access to the device */
	struct mutex mutex;
	/*
	 * One hardware spinlock to synchronize between the multiple
	 * subsystems which will access the unique ADC controller.
	 */
	struct hwspinlock *hwlock;
	u32 base;
	int irq;
	struct sprd_adc_channel_data ch_data[SPRD_ADC_CHANNEL_MAX];
	const struct sprd_adc_variant_data *var_data;
	struct sprd_adc_pm_data pm_data;
};

struct sprd_adc_variant_data {
	const int pmic_type;
	const struct reg_bit *const reg_list;
	const u32 glb_reg_base;
	const u32 adc_reg_base_offset;
	const u32 graph_index;
	const struct calib_info calib_infos[GRAPH_MAX];
	const int aux_ratio_comm[SPRD_ADC_SCALE_MAX];
	void (*const ch_data_init)(struct sprd_adc_data *data);
};

struct sprd_adc_linear_graph {
	const char *cell_names[SPRD_ADC_CELL_MAX+1];/* must end with NULL point */
	int cell_value[SPRD_ADC_CELL_MAX+1];
	void (*const calibrate)(struct sprd_adc_linear_graph *graph);
	const int volt0;
	int adc0;
	const int volt1;
	int adc1;
	const int inited;
};

struct sprd_adc_graphs {
	struct sprd_adc_linear_graph adc_graphs[GRAPH_MAX];
};

static void sprd_adc_calib_with_two_cell(struct sprd_adc_linear_graph *graph);
static u32 get_isen(void *pri, int ch, bool enable);
static int sprd_adc_soft_rst(struct sprd_adc_data *data);
static int sprd_adc_hw_enable(struct sprd_adc_data *data);
static inline u32 GET_REG_ADDR(struct sprd_adc_data *data, int index)
{
	u32 base = ((data->var_data->reg_list[index].base == BASE_GLB)
		    ? (data->var_data->glb_reg_base)
		    : (data->base - data->var_data->adc_reg_base_offset));
	return (base + data->var_data->reg_list[index].reg_addr);
}

static inline void ratio_to_u32_fract(u32 ratio, struct u32_fract *fract)
{
	fract->numerator = ratio >> SPRD_RATIO_NUMERATOR_OFFSET;
	fract->denominator = ratio & SPRD_RATIO_DENOMINATOR_MASK;
}

static struct sprd_adc_graphs sprd_adc_graphs_array[] = {
	[TWO_CELL_GRAPH] = {
		.adc_graphs = {
			[GRAPH_BIG] = {
				.cell_names = {"big_scale_calib1", "big_scale_calib2", NULL},
				.calibrate = sprd_adc_calib_with_two_cell,
				GRAPH_POINT_INIT(4200, 3310, 3600, 2832),
			},
			[GRAPH_SMALL] = {
				.cell_names = {"small_scale_calib1", "small_scale_calib2", NULL},
				.calibrate = sprd_adc_calib_with_two_cell,
				GRAPH_POINT_INIT(1000, 3413, 100, 341),
			},
			[GRAPH_VBAT_DET] = {
				.cell_names = {"vbat_det_cal1", "vbat_det_cal2", NULL},
				.calibrate = sprd_adc_calib_with_two_cell,
				GRAPH_POINT_INIT(1400, 3482, 200, 476),
			},
		},
	},
};

static const struct reg_bit regs_ump9620[] = {
	[REG_MODULE_EN] = REG_BIT_INIT(BASE_GLB, 0x08, BIT(5), 5, 1, 0, NULL, false),
	[REG_CLK_EN] = REG_BIT_INIT(BASE_GLB, 0x0c, GENMASK(6, 5), 5, 0x3, 0, NULL, false),
	[REG_SOFT_RST] = REG_BIT_INIT(BASE_GLB, 0x14, BIT(6), 6, 1, 0, NULL, false),
	[REG_XTL_CTRL] = REG_BIT_INIT(BASE_GLB, 0x378, BIT(8), 8, 1, 0, NULL, false),
	[REG_SCALE] = REG_BIT_INIT(BASE_ANA, 0xffff, GENMASK(10, 9), 9, -1, -1, NULL, false),
	[REG_CAL_EN] = REG_BIT_INIT(BASE_ANA, 0x4, BIT(12), 12, 1, 0, NULL, false),
	[REG_ISEN0] = REG_BIT_INIT(BASE_GLB, 0x384, BIT(0), 0, 1, 0, NULL, true),
	[REG_ISEN1] = REG_BIT_INIT(BASE_GLB, 0x384, BIT(13), 13, 1, 0, NULL, false),
	[REG_ISEN2] = REG_BIT_INIT(BASE_GLB, 0x384, GENMASK(11, 9), 9, -1, -1, get_isen, false),
	[REG_ISEN3] = REG_BIT_INIT(BASE_ANA, 0xB0, BIT(0), 0, 1, 0, NULL, false),
};

static const struct iio_chan_spec sprd_channels[] = {
	SPRD_ADC_CHANNEL(0, BIT(IIO_CHAN_INFO_PROCESSED)),
	SPRD_ADC_CHANNEL(1, BIT(IIO_CHAN_INFO_PROCESSED)),
	SPRD_ADC_CHANNEL(2, BIT(IIO_CHAN_INFO_PROCESSED)),
	SPRD_ADC_CHANNEL(3, BIT(IIO_CHAN_INFO_PROCESSED)),
	SPRD_ADC_CHANNEL(4, BIT(IIO_CHAN_INFO_PROCESSED)),
	SPRD_ADC_CHANNEL(5, BIT(IIO_CHAN_INFO_PROCESSED)),
	SPRD_ADC_CHANNEL(6, BIT(IIO_CHAN_INFO_PROCESSED)),
	SPRD_ADC_CHANNEL(7, BIT(IIO_CHAN_INFO_PROCESSED)),
	SPRD_ADC_CHANNEL(8, BIT(IIO_CHAN_INFO_PROCESSED)),
	SPRD_ADC_CHANNEL(9, BIT(IIO_CHAN_INFO_PROCESSED)),
	SPRD_ADC_CHANNEL(10, BIT(IIO_CHAN_INFO_PROCESSED)),
	SPRD_ADC_CHANNEL(11, BIT(IIO_CHAN_INFO_PROCESSED)),
	SPRD_ADC_CHANNEL(12, BIT(IIO_CHAN_INFO_PROCESSED)),
	SPRD_ADC_CHANNEL(13, BIT(IIO_CHAN_INFO_PROCESSED)),
	SPRD_ADC_CHANNEL(14, BIT(IIO_CHAN_INFO_PROCESSED)),
	SPRD_ADC_CHANNEL(15, BIT(IIO_CHAN_INFO_PROCESSED)),
	SPRD_ADC_CHANNEL(16, BIT(IIO_CHAN_INFO_PROCESSED)),
	SPRD_ADC_CHANNEL(17, BIT(IIO_CHAN_INFO_PROCESSED)),
	SPRD_ADC_CHANNEL(18, BIT(IIO_CHAN_INFO_PROCESSED)),
	SPRD_ADC_CHANNEL(19, BIT(IIO_CHAN_INFO_PROCESSED)),
	SPRD_ADC_CHANNEL(20, BIT(IIO_CHAN_INFO_RAW)),
	SPRD_ADC_CHANNEL(21, BIT(IIO_CHAN_INFO_PROCESSED)),
	SPRD_ADC_CHANNEL(22, BIT(IIO_CHAN_INFO_PROCESSED)),
	SPRD_ADC_CHANNEL(23, BIT(IIO_CHAN_INFO_PROCESSED)),
	SPRD_ADC_CHANNEL(24, BIT(IIO_CHAN_INFO_PROCESSED)),
	SPRD_ADC_CHANNEL(25, BIT(IIO_CHAN_INFO_PROCESSED)),
	SPRD_ADC_CHANNEL(26, BIT(IIO_CHAN_INFO_PROCESSED)),
	SPRD_ADC_CHANNEL(27, BIT(IIO_CHAN_INFO_PROCESSED)),
	SPRD_ADC_CHANNEL(28, BIT(IIO_CHAN_INFO_PROCESSED)),
	SPRD_ADC_CHANNEL(29, BIT(IIO_CHAN_INFO_PROCESSED)),
	SPRD_ADC_CHANNEL(30, BIT(IIO_CHAN_INFO_PROCESSED)),
	SPRD_ADC_CHANNEL(31, BIT(IIO_CHAN_INFO_PROCESSED)),
};

static void sprd_adc_calib_with_two_cell(struct sprd_adc_linear_graph *graph)
{
	int adc_calib_data0 = graph->cell_value[0];
	int adc_calib_data1 = graph->cell_value[1];
	int adc0_calib, adc1_calib;

	pr_debug("calib before: adc0: %d:, adc1:%d, calib_data0: %d, calib_data1: %d\n",
		 graph->adc0, graph->adc1, adc_calib_data0, adc_calib_data1);

	adc0_calib = (adc_calib_data0 & 0xfff0) >> 4;
	adc1_calib = (adc_calib_data1 & 0xfff0) >> 4;

	if (adc0_calib > 0 && adc1_calib > 0) {
		graph->adc0 = adc0_calib;
		graph->adc1 = adc1_calib;
	}

	pr_debug("calib aft: adc0: %d:, adc1:%d, calib_data0: %d, calib_data1: %d\n",
		 graph->adc0, graph->adc1, adc_calib_data0, adc_calib_data1);
}

static const struct calib_info *sprd_adc_calib_info_get(struct sprd_adc_data *data, int ch)
{
	int calib_index, calib_small = 1;
	const struct calib_info *calib_info;

	for (calib_index = 0; calib_index < GRAPH_MAX; calib_index++) {
		calib_info = &data->var_data->calib_infos[calib_index];
		if (calib_info->inited != SPRD_ADC_INIT_MAGIC)
			continue;

		if (calib_info->graph == GRAPH_SMALL)
			calib_small = calib_index;

		if (calib_info->channel == ch)
			return calib_info;
	}

	return &data->var_data->calib_infos[calib_small];
}

static void sprd_adc_aux_ratio_init(struct sprd_adc_data *data, int ch)
{
	u64 final_ratio;
	int scale, *aux_ratio_ch, aux_ratio_tmp[SPRD_ADC_SCALE_MAX];
	const struct calib_info *calib_match = sprd_adc_calib_info_get(data, ch);
	const int *aux_ratio_calib;
	struct u32_fract c_fract, aux_fract;

	data->ch_data[ch].graph = calib_match->graph;
	aux_ratio_ch = data->ch_data[ch].ch_aux_ratio;

	aux_ratio_calib = &data->var_data->aux_ratio_comm[0];
	memcpy(&aux_ratio_tmp[0], aux_ratio_calib, sizeof(int) * SPRD_ADC_SCALE_MAX);

	ratio_to_u32_fract(aux_ratio_calib[calib_match->scale], &c_fract);
	for (scale = 0; scale < SPRD_ADC_SCALE_MAX; scale++) {

		if (ch == calib_match->channel && scale == calib_match->scale) {
			aux_ratio_ch[scale] = RATIO_DEF;
			continue;
		}

		if (aux_ratio_tmp[scale] == R_INVALID || aux_ratio_calib[scale] == R_INVALID) {
			aux_ratio_ch[scale] = RATIO_DEF;
			continue;
		}

		ratio_to_u32_fract(aux_ratio_tmp[scale], &aux_fract);
		if (scale > calib_match->scale) {
			final_ratio =
				div_u64((u64)aux_fract.denominator * c_fract.numerator * 1000,
					c_fract.denominator * aux_fract.numerator);
			aux_ratio_ch[scale] = RATIO(1000, (int)final_ratio);
		} else {
			final_ratio =
				div_u64((u64)c_fract.denominator * aux_fract.numerator * 1000,
					aux_fract.denominator * c_fract.numerator);
			aux_ratio_ch[scale] = RATIO((int)final_ratio, 1000);
		}
	}
}

static inline void sprd_adc_ch_data_show(struct sprd_adc_data *data, int ch)
{
	struct sprd_adc_channel_data *ch_data = &data->ch_data[ch];
	struct u32_fract ip_fract, aux_fract0, aux_fract1, aux_fract2, aux_fract3;

	ratio_to_u32_fract(ch_data->ip_ratio, &ip_fract);
	ratio_to_u32_fract(ch_data->ch_aux_ratio[0], &aux_fract0);
	ratio_to_u32_fract(ch_data->ch_aux_ratio[1], &aux_fract1);
	ratio_to_u32_fract(ch_data->ch_aux_ratio[2], &aux_fract2);
	ratio_to_u32_fract(ch_data->ch_aux_ratio[3], &aux_fract3);
	dev_dbg(data->dev, CH_DATA_FMT,
		ch, ch_data->scale, ch_data->graph, ch_data->isen_info, ch_data->filter_info,
		ip_fract.numerator, ip_fract.denominator,
		aux_fract0.numerator, aux_fract0.denominator,
		aux_fract1.numerator, aux_fract1.denominator,
		aux_fract2.numerator, aux_fract2.denominator,
		aux_fract3.numerator, aux_fract3.denominator);
}

static void sprd_adc_ch_data_merge(struct sprd_adc_data *data, struct sprd_adc_channel_data *d_diff,
				   struct sprd_adc_channel_data *d_def)
{
	struct sprd_adc_channel_data *ch_data;
	int ch;

	for (ch = 0; ch < SPRD_ADC_CHANNEL_MAX; ch++) {
		ch_data = &data->ch_data[ch];
		*ch_data = ((d_diff[ch].inited == SPRD_ADC_INIT_MAGIC) ? d_diff[ch] : *d_def);
	}

	for (ch = 0; ch < SPRD_ADC_CHANNEL_MAX; ch++) {
		sprd_adc_aux_ratio_init(data, ch);
		sprd_adc_ch_data_show(data, ch);
	}
}

static int sprd_adc_graphs_calibrate(struct sprd_adc_data *data)
{
	int i, j, ret;
	struct sprd_adc_graphs *graphs;
	struct sprd_adc_linear_graph *linear_graph;
	u16 cell_value = 0;

	if (data->var_data->graph_index >= SPRD_ADC_GRAPH_TYPE_MAX)
		return -EINVAL;

	graphs = &sprd_adc_graphs_array[data->var_data->graph_index];
	for (i = 0; i < GRAPH_MAX; i++) {

		if (graphs->adc_graphs[i].inited != SPRD_ADC_INIT_MAGIC)
			continue;

		linear_graph = &graphs->adc_graphs[i];
		for (j = 0; linear_graph->cell_names[j] != NULL; j++) {
			ret = nvmem_cell_read_u16(data->dev, linear_graph->cell_names[j],
						  &cell_value);
			if (ret) {
				dev_err(data->dev, "calibrate err! name: %s: ret %d\n",
					linear_graph->cell_names[j], ret);
				return ret;
			}
			linear_graph->cell_value[j] = cell_value;
		}
	}

	for (i = 0; i < GRAPH_MAX; i++) {

		if (graphs->adc_graphs[i].inited != SPRD_ADC_INIT_MAGIC)
			continue;

		linear_graph = &graphs->adc_graphs[i];
		linear_graph->calibrate(linear_graph);
	}

	return 0;
}

static void ump9620_ch_data_init(struct sprd_adc_data *data)
{
	struct sprd_adc_channel_data ch_data_def = CH_DATA_INIT(SCALE_00, 0, 0, RATIO_DEF);
	struct sprd_adc_channel_data ch_data[SPRD_ADC_CHANNEL_MAX] = {
		[0] = CH_DATA_INIT(SCALE_01, 0, 0, RATIO_DEF),
		[5] = CH_DATA_INIT(SCALE_00, 0x1, 0x9, RATIO_DEF),
		[6] = CH_DATA_INIT(SCALE_00, 0x1, 0x9, RATIO_DEF),
		[7] = CH_DATA_INIT(SCALE_10, 0, 0, RATIO_DEF),
		[9] = CH_DATA_INIT(SCALE_10, 0, 0, RATIO_DEF),
		[10] = CH_DATA_INIT(SCALE_11, 0, 0, RATIO_DEF),
		[11] = CH_DATA_INIT(SCALE_00, 0, 0, RATIO_DEF),
		[13] = CH_DATA_INIT(SCALE_01, 0, 0, RATIO_DEF),
		[14] = CH_DATA_INIT(SCALE_00, 0, 0, RATIO(68, 900)),
		[15] = CH_DATA_INIT(SCALE_00, 0, 0, RATIO(1, 3)),
		[19] = CH_DATA_INIT(SCALE_11, 0, 0, RATIO_DEF),
		[21] = CH_DATA_INIT(SCALE_00, 0, 0, RATIO(3, 8)),
		[22] = CH_DATA_INIT(SCALE_00, 0, 0, RATIO(3, 8)),
		[23] = CH_DATA_INIT(SCALE_00, 0, 0, RATIO(3, 8)),
		[30] = CH_DATA_INIT(SCALE_11, 0, 0, RATIO_DEF),
		[31] = CH_DATA_INIT(SCALE_11, 0, 0, RATIO_DEF),
	};

	sprd_adc_ch_data_merge(data, ch_data, &ch_data_def);
}

static void sprd_adc_regs_dump(struct sprd_adc_data *data, int ch, int scale, const char *tag)
{
	u32 mod_en = 0, clk_en = 0, int_ctl = 0, int_raw = 0, adc_ctl = 0;
	u32 ch_cfg = 0, pm_clk_reg = 0, xtl_ctl = 0;
	int ret;

	ret = regmap_read(data->regmap, GET_REG_ADDR(data, REG_MODULE_EN), &mod_en);
	ret |= regmap_read(data->regmap, GET_REG_ADDR(data, REG_CLK_EN), &clk_en);
	ret |= regmap_read(data->regmap, GET_REG_ADDR(data, REG_XTL_CTRL), &xtl_ctl);
	ret |= regmap_read(data->regmap, data->base + SPRD_ADC_INT_CLR, &int_ctl);
	ret |= regmap_read(data->regmap, data->base + SPRD_ADC_INT_RAW, &int_raw);
	ret |= regmap_read(data->regmap, data->base + SPRD_ADC_CTL, &adc_ctl);
	ret |= regmap_read(data->regmap, data->base + SPRD_ADC_CH_CFG, &ch_cfg);
	if (data->pm_data.clk_regmap)
		ret |= regmap_read(data->pm_data.clk_regmap,  data->pm_data.clk_reg, &pm_clk_reg);

	if (ret) {
		dev_err(data->dev, "regs_dump err: ret %d\n", ret);
		return;
	}

	dev_err(data->dev, "[%s]: ch %d, sl %d, clk_en 0x%x, xtl_ctl 0x%x, pm_clk 0x%x\n",
		tag, ch, scale, clk_en, xtl_ctl, pm_clk_reg);
	dev_err(data->dev, "mod_en 0x%x, int_ctl 0x%x, int_raw 0x%x, adc_ctl 0x%x, ch_cfg 0x%x\n",
		mod_en, int_ctl, int_raw, adc_ctl, ch_cfg);
}

static u32 get_isen(void *pri, int ch, bool enable)
{
	struct sprd_adc_data *data = (struct sprd_adc_data *)pri;

	if (!enable)
		return 0;

	return (data->ch_data[ch].isen_info >> 1);
}

static int sprd_adc_reg_cfg(struct sprd_adc_data *data, int ch, int index, bool set)
{
	int ret;
	u32 reg_addr, val, read_val = 0;
	const struct reg_bit *const reg_inf = &data->var_data->reg_list[index];
	bool set_act = ((set && !reg_inf->reverse) || (!set && reg_inf->reverse));

	if (data->var_data->reg_list[index].inited != SPRD_ADC_INIT_MAGIC)
		return 0;

	reg_addr = GET_REG_ADDR(data, index);
	if (!reg_inf->get_val)
		val = (set_act ? reg_inf->val_set : reg_inf->val_clr);
	else
		val = reg_inf->get_val(data, ch, set_act);
	val = ((val << reg_inf->offset) & reg_inf->mask);
	ret = regmap_update_bits(data->regmap, reg_addr, reg_inf->mask, val);
	ret |= regmap_read(data->regmap, reg_addr, &read_val);
	if (ret) {
		dev_err(data->dev, "reg_cfg err: reg[%d], ret %d\n", reg_addr, ret);
		return ret;
	}

	dev_dbg(data->dev, "reg_cfg[%d %d]: reg 0x%x, mask: 0x%x, val: 0x%x, read_val: 0x%x\n",
		set, reg_inf->reverse, reg_addr, reg_inf->mask, val, read_val);

	return 0;
}

static void sprd_adc_regs_set_order(struct sprd_adc_data *data, int ch, int start, int end)
{
	int i;

	for (i = start + 1; i < end; i++)
		sprd_adc_reg_cfg(data, ch, i, true);
}

static void sprd_adc_regs_clr_order_reverse(struct sprd_adc_data *data, int ch, int start, int end)
{
	int i;

	for (i = end - 1; i > start; i--)
		sprd_adc_reg_cfg(data, ch, i, false);
}

static int sprd_adc_isen_enable(struct sprd_adc_data *data, int channel)
{
	bool isen_support = data->ch_data[channel].isen_info & 0x1;

	if (!isen_support)
		return 0;

	sprd_adc_regs_set_order(data, channel, REG_ISEN_ST, REG_ISEN_END);
	usleep_range(500, 1000);

	return 0;
}

static int sprd_adc_isen_disable(struct sprd_adc_data *data, int channel)
{
	bool isen_support = data->ch_data[channel].isen_info & 0x1;

	if (!isen_support)
		return 0;

	sprd_adc_regs_clr_order_reverse(data, channel, REG_ISEN_ST, REG_ISEN_END);
	usleep_range(500, 1000);

	return 0;
}

static void sprd_adc_config_fixed(struct sprd_adc_data *data)
{
	sprd_adc_regs_set_order(data, -1, REG_CFG_FIXED_ST, REG_CFG_FIXED_END);
}

static int sprd_adc_data_average(int *vals, int len)
{
	int i, sum = 0;

	for (i = 0; i < len; i++)
		sum += vals[i];
	return DIV_ROUND_CLOSEST(sum, len);
}

static int compare_val(const void *a, const void *b)
{
	return *(int *)a - *(int *)b;
}

static int sprd_adc_get_val_with_sw_filter(struct sprd_adc_data *data, int ch)
{
	int data_buf[ADC_MESURE_NUMBER_SW] = { 0 }, ret = 0, num = ADC_MESURE_NUMBER_SW;
	int count, result;
	unsigned int  rawdata;

	for (count = 0; count < ADC_MESURE_NUMBER_SW; count++) {
		ret |= regmap_read(data->regmap, data->base + SPRD_ADC_DATA, &rawdata);
		rawdata &= SPRD_ADC_DATA_MASK;
		data_buf[count] = rawdata;
		udelay(10);
		if (ret)
			return -EINVAL;
	}

	sort(data_buf, ADC_MESURE_NUMBER_SW, sizeof(int), compare_val, NULL);
	result = sprd_adc_data_average(&data_buf[num / 5], (num - num * 2 / 5));

	for (count = 0; count < ADC_MESURE_NUMBER_SW; count++)
		dev_dbg(data->dev, "data_buf[%d]=%d ", count, data_buf[count]);
	dev_dbg(data->dev, "result=%d\n", result);

	return result;
}

static int sprd_adc_enable(struct sprd_adc_data *data, int channel)
{
	int ret = 0;
	u32 reg_read = 0;

	if (data->pm_data.clk_regmap) {
		ret = regmap_update_bits(data->pm_data.clk_regmap, data->pm_data.clk_reg,
					 data->pm_data.clk_reg_mask,
					 data->pm_data.clk_reg_mask);
		ret |= regmap_read(data->pm_data.clk_regmap, data->pm_data.clk_reg, &reg_read);
		if (ret) {
			dev_err(data->dev, "failed to enable clk26m, channel %d\n", channel);
			return ret;
		}
		dev_dbg(data->dev, "enable clk26m: ch %d, reg_read 0x%x\n", channel, reg_read);
	}

	ret = sprd_adc_isen_enable(data, channel);
	if (ret) {
		dev_err(data->dev, "failed to enable isen\n");
		return ret;
	}

	ret = regmap_update_bits(data->regmap, data->base + SPRD_ADC_CTL,
				 SPRD_ADC_EN, SPRD_ADC_EN);
	if (ret) {
		dev_err(data->dev, "failed to set SPRD_ADC_EN\n");
		return ret;
	}

	return ret;
}

static int sprd_adc_disable(struct sprd_adc_data *data, int channel)
{
	int ret = 0;
	u32 reg_read = 0;

	ret = regmap_update_bits(data->regmap, data->base + SPRD_ADC_CTL, SPRD_ADC_EN, 0);
	if (ret) {
		dev_err(data->dev, "failed to reset SPRD_ADC_EN\n");
		return ret;
	}

	ret = sprd_adc_isen_disable(data, channel);
	if (ret) {
		dev_err(data->dev, "failed to disable isen\n");
		return ret;
	}

	if (data->pm_data.clk_regmap) {
		ret = regmap_update_bits(data->pm_data.clk_regmap, data->pm_data.clk_reg,
					 data->pm_data.clk_reg_mask, 0);
		ret |= regmap_read(data->pm_data.clk_regmap, data->pm_data.clk_reg, &reg_read);
		if (ret) {
			dev_err(data->dev, "failed to disable clk26m, channel %d\n", channel);
			return ret;
		}
		dev_dbg(data->dev, "disable clk26m: ch %d, reg_read 0x%x\n", channel, reg_read);
	}

	return ret;
}

static int sprd_adc_read(struct sprd_adc_data *data, int channel, int scale, int *val)
{
	int ret = 0, sample_num_sw;
	u32 rawdata = 0, tmp, status, scale_shift, scale_mask;
	bool filter_sw = data->ch_data[channel].filter_info & 0x1;
	int sample_num_hw = data->ch_data[channel].filter_info >> 1;

	if (data->pm_data.dev_suspended) {
		dev_err(data->dev, "adc_exp: adc bas been suspended, ignore.\n");
		return -EBUSY;
	}

	dev_dbg(data->dev, "ch_data[%d]: scale %d, graph %d, filter_info 0x%x, isen_info 0x%x\n",
		channel, data->ch_data[channel].scale, data->ch_data[channel].graph,
		data->ch_data[channel].filter_info, data->ch_data[channel].isen_info);

	ret = hwspin_lock_timeout_raw(data->hwlock, SPRD_ADC_HWLOCK_TIMEOUT);
	if (ret) {
		dev_err(data->dev, "timeout to get the hwspinlock\n");
		return ret;
	}

	ret = sprd_adc_enable(data, channel);
	if (ret)
		goto disable_adc;

	ret = regmap_update_bits(data->regmap, data->base + SPRD_ADC_INT_CLR,
				 SPRD_ADC_IRQ_CLR, SPRD_ADC_IRQ_CLR);
	if (ret)
		goto disable_adc;

	/* Configure the channel id and scale */
	scale_shift = data->var_data->reg_list[REG_SCALE].offset;
	scale_mask = data->var_data->reg_list[REG_SCALE].mask;
	tmp = (scale << scale_shift) & scale_mask;
	tmp |= channel & SPRD_ADC_CHN_ID_MASK;
	ret = regmap_update_bits(data->regmap, data->base + SPRD_ADC_CH_CFG,
				 SPRD_ADC_CHN_ID_MASK | scale_mask, tmp);
	if (ret)
		goto disable_adc;

	/* Select 12bit conversion mode, and only sample 1 time */
	tmp = SPRD_ADC_12BIT_MODE;
	sample_num_sw = (filter_sw ? ADC_MESURE_NUMBER_SW - 1 : 0);
	sample_num_hw = ((sample_num_hw > 0) ? sample_num_hw : ADC_MESURE_NUMBER_HW_DEF);
	tmp |= ((unsigned long)sample_num_sw << SPRD_ADC_RUN_NUM_SHIFT) & SPRD_ADC_RUN_NUM_MASK;
	tmp |= ((unsigned long)sample_num_hw << SPRD_ADC_AVERAGE_SHIFT) & SPRD_ADC_AVERAGE_MASK;
	ret = regmap_update_bits(data->regmap, data->base + SPRD_ADC_CTL,
				 SPRD_ADC_RUN_NUM_MASK | SPRD_ADC_12BIT_MODE |
				 SPRD_ADC_AVERAGE_MASK, tmp);
	if (ret)
		goto disable_adc;

	ret = regmap_update_bits(data->regmap, data->base + SPRD_ADC_CTL,
				 SPRD_ADC_CHN_RUN, SPRD_ADC_CHN_RUN);
	if (ret)
		goto disable_adc;

	ret = regmap_read_poll_timeout(data->regmap, data->base + SPRD_ADC_INT_RAW, status,
				       (status & SPRD_ADC_IRQ_RAW), SPRD_ADC_POLL_RAW_STATUS,
				       SPRD_ADC_RDY_TIMEOUT);
	if (ret) {
		dev_err(data->dev, "read adc timeout 0x%x\n", status);
		sprd_adc_regs_dump(data, channel, scale, "t_bef");
		sprd_adc_hw_enable(data);
		sprd_adc_soft_rst(data);
		sprd_adc_regs_dump(data, channel, scale, "t_aft");
		goto disable_adc;
	}

	if (filter_sw) {
		rawdata = sprd_adc_get_val_with_sw_filter(data, channel);
	} else {
		ret = regmap_read(data->regmap, data->base + SPRD_ADC_DATA, &rawdata);
		rawdata &= SPRD_ADC_DATA_MASK;
	}
disable_adc:
	ret = sprd_adc_disable(data, channel);

	hwspin_unlock_raw(data->hwlock);

	if (!ret)
		*val = rawdata;

	return ret;
}

static int sprd_adc_calculate_volt_by_graph(struct sprd_adc_data *data, int channel,
					    int scale, int raw_adc)
{
	int tmp;
	struct sprd_adc_graphs *graphs = &sprd_adc_graphs_array[data->var_data->graph_index];
	int graph_offset = data->ch_data[channel].graph;
	struct sprd_adc_linear_graph *linear_graph = &graphs->adc_graphs[graph_offset];

	tmp = (linear_graph->volt0 - linear_graph->volt1) * (raw_adc - linear_graph->adc1);
	tmp /= (linear_graph->adc0 - linear_graph->adc1);
	tmp += linear_graph->volt1;
	tmp = (tmp < 0 ? 0 : tmp);

	dev_dbg(data->dev, "by_graph_c%d: v0 %d a0 %d, v1 %d a1 %d, raw_adc 0x%x, vol_graph %d\n",
		channel, linear_graph->volt0, linear_graph->adc0, linear_graph->volt1,
		linear_graph->adc1, raw_adc, tmp);

	return tmp;
}

static int sprd_adc_calculate_volt_by_ratio(struct sprd_adc_data *data, int channel,
					    int scale, int vol_graph)
{
	struct u32_fract ip_fract, aux_fract;
	int pmic_type = data->var_data->pmic_type;
	u32 vol_final = vol_graph;

	ratio_to_u32_fract(data->ch_data[channel].ip_ratio, &ip_fract);
	vol_final = DIV_ROUND_CLOSEST(vol_final * ip_fract.denominator, ip_fract.numerator);

	ratio_to_u32_fract(data->ch_data[channel].ch_aux_ratio[scale], &aux_fract);
	vol_final = DIV_ROUND_CLOSEST(vol_final * aux_fract.denominator, aux_fract.numerator);

	dev_dbg(data->dev, "by_ratio_c%d: id %d, scale %d, ip_r[%d/%d], aux_r[%d/%d], vol_f %d\n",
		channel, pmic_type, scale, ip_fract.numerator, ip_fract.denominator,
		aux_fract.numerator, aux_fract.denominator, vol_final);

	return vol_final;
}

static int sprd_adc_read_processed(struct sprd_adc_data *data, int channel, int scale, int *val)
{
	int ret, raw_adc, vol_graph;

	ret = sprd_adc_read(data, channel, scale, &raw_adc);

	if (ret)
		return ret;

	vol_graph = sprd_adc_calculate_volt_by_graph(data, channel, scale, raw_adc);
	*val = sprd_adc_calculate_volt_by_ratio(data, channel, scale, vol_graph);

	return 0;
}

static int sprd_adc_ch_data_encode(struct sprd_adc_data *data, int ch)
{
	int scale = data->ch_data[ch].scale & 0xff;
	int isen_info = data->ch_data[ch].isen_info & 0xff;
	int filter_info = data->ch_data[ch].filter_info & 0xff;

	return (scale | (filter_info << 8) | (isen_info << 16));
}

static void sprd_adc_ch_data_decode(struct sprd_adc_data *data, int ch, int val)
{
	int scale_override, filter_override, isen_override;

	scale_override = (val & 0xff);
	filter_override = ((val >> 8) & 0xff);
	isen_override = ((val >> 16) & 0xff);

	if (scale_override < SPRD_ADC_SCALE_MAX && scale_override != data->ch_data[ch].scale)
		data->ch_data[ch].scale = scale_override;

	if (data->ch_data[ch].filter_info != filter_override)
		data->ch_data[ch].filter_info = filter_override;

	if (data->ch_data[ch].isen_info != isen_override)
		data->ch_data[ch].isen_info = isen_override;
}

static int sprd_adc_read_raw(struct iio_dev *indio_dev, struct iio_chan_spec const *chan,
			     int *val, int *val2, long mask)
{
	struct sprd_adc_data *data = iio_priv(indio_dev);
	int scale = data->ch_data[chan->channel].scale;
	int ret, tmp;

	switch (mask) {
	case IIO_CHAN_INFO_RAW:
		mutex_lock(&data->mutex);
		ret = sprd_adc_read(data, chan->channel, scale, &tmp);
		mutex_unlock(&data->mutex);

		if (ret)
			return ret;

		*val = tmp;
		return IIO_VAL_INT;

	case IIO_CHAN_INFO_PROCESSED:
		mutex_lock(&data->mutex);
		ret = sprd_adc_read_processed(data, chan->channel, scale, &tmp);
		mutex_unlock(&data->mutex);

		if (ret)
			return ret;

		*val = tmp;
		return IIO_VAL_INT;

	case IIO_CHAN_INFO_SCALE:
		*val = sprd_adc_ch_data_encode(data, chan->channel);
		return IIO_VAL_INT;

	default:
		return -EINVAL;
	}
}

static int sprd_adc_write_raw(struct iio_dev *indio_dev, struct iio_chan_spec const *chan,
			      int val, int val2, long mask)
{
	struct sprd_adc_data *data = iio_priv(indio_dev);

	switch (mask) {
	case IIO_CHAN_INFO_SCALE:
		sprd_adc_ch_data_decode(data, chan->channel, val);
		return 0;

	default:
		return -EINVAL;
	}
}

static int sprd_adc_soft_rst(struct sprd_adc_data *data)
{
	int ret;
	u32 reg_addr, mask;

	reg_addr = GET_REG_ADDR(data, REG_SOFT_RST);
	mask = data->var_data->reg_list[REG_SOFT_RST].mask;
	ret = regmap_update_bits(data->regmap, reg_addr, mask, mask);
	if (ret)
		return ret;

	udelay(10);

	reg_addr = GET_REG_ADDR(data, REG_SOFT_RST);
	mask = data->var_data->reg_list[REG_SOFT_RST].mask;
	ret = regmap_update_bits(data->regmap, reg_addr, mask, 0);
	if (ret)
		return ret;

	sprd_adc_config_fixed(data);

	return 0;
}
static int sprd_adc_hw_enable(struct sprd_adc_data *data)
{
	int ret;
	u32 reg_addr, mask;

	reg_addr = GET_REG_ADDR(data, REG_XTL_CTRL);
	mask = data->var_data->reg_list[REG_XTL_CTRL].mask;
	ret = regmap_update_bits(data->regmap, reg_addr, mask, mask);
	if (ret)
		return ret;

	reg_addr = GET_REG_ADDR(data, REG_MODULE_EN);
	mask = data->var_data->reg_list[REG_MODULE_EN].mask;
	ret = regmap_update_bits(data->regmap, reg_addr, mask, mask);
	if (ret)
		return ret;

	/* Enable ADC work clock */
	reg_addr = GET_REG_ADDR(data, REG_CLK_EN);
	mask = data->var_data->reg_list[REG_CLK_EN].mask;
	ret = regmap_update_bits(data->regmap, reg_addr, mask, mask);
	if (ret)
		return ret;

	return 0;
}

static void sprd_adc_hw_disable(void *_data)
{
	struct sprd_adc_data *data = _data;
	u32 reg_addr, mask;

	/* Disable ADC work clock and controller clock */
	reg_addr = GET_REG_ADDR(data, REG_CLK_EN);
	mask = data->var_data->reg_list[REG_CLK_EN].mask;
	regmap_update_bits(data->regmap, reg_addr, mask, 0);

	reg_addr = GET_REG_ADDR(data, REG_MODULE_EN);
	mask = data->var_data->reg_list[REG_MODULE_EN].mask;
	regmap_update_bits(data->regmap, reg_addr, mask, 0);
}

static void sprd_adc_free_lock(void *_data)
{
	struct sprd_adc_data *sprd_data = _data;

	hwspin_lock_free(sprd_data->hwlock);
	mutex_destroy(&sprd_data->mutex);
}

static int sprd_adc_ch_data_init(struct sprd_adc_data *data)
{
	struct device_node *np = data->dev->of_node;
	int size, ret, ch, ch_data_val, i;
	u32 *ch_data_override;

	data->var_data->ch_data_init(data);

	size = of_property_count_elems_of_size(np, "ch_data_override", sizeof(u32));
	if (size <= 0)
		return 0;

	if (size % 2) {
		dev_err(data->dev, "Pair of ch data err!\n");
		return -EINVAL;
	}

	ch_data_override = kcalloc(size, sizeof(u32), GFP_KERNEL);
	if (!ch_data_override)
		return -ENOMEM;

	ret = of_property_read_u32_array(np, "ch_data_override", ch_data_override, size);
	if (ret < 0) {
		dev_err(data->dev, "Failed to read ch data from dt: %d\n", ret);
		kfree(ch_data_override);
		return ret;
	}

	for (i = 0; i < size; i += 2) {
		ch = ch_data_override[i];
		ch_data_val = ch_data_override[i+1];
		sprd_adc_ch_data_decode(data, ch, ch_data_val);
		sprd_adc_ch_data_show(data, ch);
	}

	kfree(ch_data_override);

	return 0;
}

static int sprd_adc_pm_init(struct sprd_adc_data *data)
{
	unsigned int pm_args[2];
	u32 reg, mask;
	struct device_node *np = data->dev->of_node;

	data->pm_data.clk_regmap =
		syscon_regmap_lookup_by_phandle_args(np, "sprd_adc_pm_reg", 2, pm_args);
	if (!IS_ERR_OR_NULL(data->pm_data.clk_regmap)) {
		data->pm_data.clk_reg = reg = pm_args[0];
		data->pm_data.clk_reg_mask = mask = pm_args[1];
	} else {
		data->pm_data.clk_regmap = NULL;
	}

	data->pm_data.dev_suspended = false;

	return 0;
}

static const struct iio_info sprd_info = {
	.read_raw = &sprd_adc_read_raw,
	.write_raw = &sprd_adc_write_raw,
};

static int sprd_adc_probe(struct platform_device *pdev)
{
	struct device_node *np = pdev->dev.of_node;
	struct sprd_adc_data *sprd_data;
	const struct sprd_adc_variant_data *pdata;
	struct iio_dev *indio_dev;
	int ret;

	pdata = of_device_get_match_data(&pdev->dev);
	if (!pdata) {
		dev_err(&pdev->dev, "No matching driver data found\n");
		return -EINVAL;
	}

	indio_dev = devm_iio_device_alloc(&pdev->dev, sizeof(*sprd_data));
	if (!indio_dev)
		return -ENOMEM;

	sprd_data = iio_priv(indio_dev);

	sprd_data->regmap = dev_get_regmap(pdev->dev.parent, NULL);
	if (!sprd_data->regmap) {
		dev_err(&pdev->dev, "failed to get ADC regmap\n");
		return -ENODEV;
	}

	ret = of_property_read_u32(np, "reg", &sprd_data->base);
	if (ret) {
		dev_err(&pdev->dev, "failed to get ADC base address\n");
		return ret;
	}

	sprd_data->irq = platform_get_irq(pdev, 0);
	if (sprd_data->irq < 0) {
		dev_err(&pdev->dev, "failed to get ADC irq number\n");
		return sprd_data->irq;
	}

	ret = of_hwspin_lock_get_id(np, 0);
	if (ret < 0) {
		dev_err(&pdev->dev, "failed to get hwspinlock id\n");
		return ret;
	}

	mutex_init(&sprd_data->mutex);
	sprd_data->hwlock = hwspin_lock_request_specific(ret);
	if (!sprd_data->hwlock) {
		dev_err(&pdev->dev, "failed to request hwspinlock\n");
		return -ENXIO;
	}

	ret = devm_add_action(&pdev->dev, sprd_adc_free_lock, sprd_data);
	if (ret) {
		sprd_adc_free_lock(sprd_data);
		dev_err(&pdev->dev, "failed to add free lock action\n");
		return ret;
	}

	sprd_data->dev = &pdev->dev;
	sprd_data->var_data = pdata;
	sprd_data->indio_dev = indio_dev;

	/* ADC channel scales calibration from nvmem device */
	ret = sprd_adc_graphs_calibrate(sprd_data);
	if (ret) {
		dev_err(&pdev->dev, "failed to calib graphs from nvmem\n");
		return ret;
	}

	ret = sprd_adc_pm_init(sprd_data);
	if (ret) {
		dev_err(&pdev->dev, "adc pm init err.\n");
		return ret;
	}

	ret = sprd_adc_ch_data_init(sprd_data);
	if (ret) {
		dev_err(&pdev->dev, "ch data init err.\n");
		return ret;
	}

	ret = sprd_adc_hw_enable(sprd_data);
	if (ret) {
		dev_err(&pdev->dev, "failed to enable ADC module\n");
		return ret;
	}

	ret = devm_add_action(&pdev->dev, sprd_adc_hw_disable, sprd_data);
	if (ret) {
		sprd_adc_hw_disable(sprd_data);
		dev_err(&pdev->dev, "failed to add ADC disable action\n");
		return ret;
	}

	ret = sprd_adc_soft_rst(sprd_data);
	if (ret) {
		dev_err(&pdev->dev, "adc soft rst failed\n");
		return ret;
	}

	indio_dev->dev.parent = &pdev->dev;
	indio_dev->name = dev_name(&pdev->dev);
	indio_dev->modes = INDIO_DIRECT_MODE;
	indio_dev->info = &sprd_info;
	indio_dev->channels = sprd_channels;
	indio_dev->num_channels = ARRAY_SIZE(sprd_channels);
	ret = devm_iio_device_register(&pdev->dev, indio_dev);
	if (ret)
		dev_err(&pdev->dev, "could not register iio (ADC)");

	platform_set_drvdata(pdev, indio_dev);

	return ret;
}

static int sprd_adc_pm_suspend(struct device *dev)
{
	struct sprd_adc_data *sprd_data = iio_priv(dev_get_drvdata(dev));

	mutex_lock(&sprd_data->mutex);
	sprd_data->pm_data.dev_suspended = true;
	mutex_unlock(&sprd_data->mutex);

	return 0;
}

static int sprd_adc_pm_resume(struct device *dev)
{
	struct sprd_adc_data *sprd_data = iio_priv(dev_get_drvdata(dev));

	mutex_lock(&sprd_data->mutex);
	sprd_data->pm_data.dev_suspended = false;
	mutex_unlock(&sprd_data->mutex);

	return 0;
}

static const struct sprd_adc_variant_data ump9620_data = {
	.pmic_type = UMP9620_ADC,
	.glb_reg_base = 0x2000,
	.adc_reg_base_offset = 0x4,
	.reg_list = regs_ump9620,
	.graph_index = TWO_CELL_GRAPH,
	.calib_infos = {
		CALIB_INFO_INIT(11, SCALE_00, GRAPH_BIG),
		CALIB_INFO_INIT(1, SCALE_00, GRAPH_SMALL),
		CALIB_INFO_INIT(0, SCALE_01, GRAPH_VBAT_DET)
	},
	.aux_ratio_comm = {RATIO_DEF, RATIO(100, 133), RATIO(1000, 2600), RATIO(1000, 4060)},
	.ch_data_init = ump9620_ch_data_init,
};

static const struct sprd_adc_variant_data ump518_data = {
	.pmic_type = UMP518_ADC,
	.glb_reg_base = 0x1800,
	.adc_reg_base_offset = 0x4,
	.reg_list = regs_ump9620,
	.graph_index = TWO_CELL_GRAPH,
	.calib_infos = {
		CALIB_INFO_INIT(11, SCALE_00, GRAPH_BIG),
		CALIB_INFO_INIT(1, SCALE_00, GRAPH_SMALL),
		CALIB_INFO_INIT(0, SCALE_01, GRAPH_VBAT_DET)
	},
	.aux_ratio_comm = {RATIO_DEF, RATIO(100, 133), RATIO(1000, 2600), RATIO(1000, 4060)},
	.ch_data_init = ump9620_ch_data_init,
};

static const struct of_device_id sprd_adc_of_match[] = {
	{ .compatible = "sprd,ump9620-adc", .data = &ump9620_data},
	{ .compatible = "sprd,ump518-adc", .data = &ump518_data},
	{ }
};

static const struct dev_pm_ops sprd_adc_pm_ops = {
	.suspend_noirq = sprd_adc_pm_suspend,
	.resume_noirq = sprd_adc_pm_resume,
};

static struct platform_driver sprd_adc_driver = {
	.probe = sprd_adc_probe,
	.driver = {
		.name = "sprd-adc",
		.of_match_table = sprd_adc_of_match,
		.pm	= &sprd_adc_pm_ops,
	},
};

module_platform_driver(sprd_adc_driver);
MODULE_DESCRIPTION("SPRD PMIC ADC Driver");
MODULE_LICENSE("GPL");

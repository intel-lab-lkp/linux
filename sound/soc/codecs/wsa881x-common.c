// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2024 Linaro Ltd
 */

#include <linux/gpio/consumer.h>
#include <linux/module.h>
#include <linux/regmap.h>
#include <sound/soc.h>

#include "wsa881x-common.h"

int wsa881x_set_stream(struct snd_soc_dai *dai, void *stream, int direction)
{
#if IS_ENABLED(CONFIG_SND_SOC_WSA881X)
	struct wsa881x_priv *wsa881x = dev_get_drvdata(dai->dev);

	wsa881x->sruntime = stream;
#endif
	return 0;
}
EXPORT_SYMBOL_GPL(wsa881x_set_stream);

int wsa881x_digital_mute(struct snd_soc_dai *dai, int mute, int stream)
{
	struct snd_soc_component *component = dai->component;

	return snd_soc_component_update_bits(component,
					     WSA881X_SPKR_DRV_EN,
					     WSA881X_SPKR_DRV_EN_CLASS_PA_MASK,
					     mute ?
					     WSA881X_SPKR_DRV_EN_CLASS_PA_DIS :
					     WSA881X_SPKR_DRV_EN_CLASS_PA_EN);
}
EXPORT_SYMBOL_GPL(wsa881x_digital_mute);

void wsa881x_init_common(struct wsa881x_priv *wsa881x)
{
	struct regmap *rm = wsa881x->regmap;
	unsigned int val = 0;

	/* Bring out of analog reset */
	regmap_update_bits(rm, WSA881X_CDC_RST_CTL,
			   WSA881X_CDC_RST_CTL_ANA_RST,
			   WSA881X_CDC_RST_CTL_ANA_RST);

	/* Bring out of digital reset */
	regmap_update_bits(rm, WSA881X_CDC_RST_CTL,
			   WSA881X_CDC_RST_CTL_DIG_RST,
			   WSA881X_CDC_RST_CTL_DIG_RST);
	regmap_update_bits(rm, WSA881X_CLOCK_CONFIG,
			   WSA881X_CLOCK_SCLK_SDM_DEM_DIV2_EN,
			   WSA881X_CLOCK_SCLK_SDM_DEM_DIV2_EN);
	regmap_update_bits(rm, WSA881X_SPKR_OCP_CTL,
			   WSA881X_SPKR_OCP_CTL_RDAC_CLK_DIV2_MASK,
			   FIELD_PREP(WSA881X_SPKR_OCP_CTL_RDAC_CLK_DIV2_MASK,
				      WSA881X_SPKR_OCP_CTL_RDAC_CLK_DIV2));
	regmap_update_bits(rm, WSA881X_SPKR_MISC_CTL1,
			   WSA881X_SPKR_MISC_CTL1_DTIME_MASK,
			   FIELD_PREP(WSA881X_SPKR_MISC_CTL1_DTIME_MASK,
				      WSA881X_SPKR_MISC_CTL1_40NS));
	regmap_update_bits(rm, WSA881X_SPKR_MISC_CTL1,
			   WSA881X_SPKR_MISC_CTL1_SLEW_RATE_MASK,
			   FIELD_PREP(WSA881X_SPKR_MISC_CTL1_SLEW_RATE_MASK,
				      WSA881X_SPKR_MISC_CTL1_60NS));
	regmap_update_bits(rm, WSA881X_SPKR_BIAS_INT,
			   WSA881X_SPKR_BIAS_INT_FULL_MASK,
			   0x0);
	regmap_update_bits(rm, WSA881X_SPKR_PA_INT,
			   WSA881X_SPKR_PA_INT_COMP_CURR_MASK,
			   FIELD_PREP(WSA881X_SPKR_PA_INT_COMP_CURR_MASK,
				      WSA881X_SPKR_PA_INT_COMP_CURR_2UA0));
	regmap_update_bits(rm, WSA881X_SPKR_PA_INT,
			   WSA881X_SPKR_PA_INT_LDO_CURR_MASK,
			   FIELD_PREP(WSA881X_SPKR_PA_INT_LDO_CURR_MASK,
				      WSA881X_SPKR_PA_INT_LDO_CURR_5UA0));
	regmap_update_bits(rm, WSA881X_BOOST_LOOP_STABILITY,
			   WSA881X_BOOST_LOOP_STAB_COMP_RES_MASK,
			   FIELD_PREP(WSA881X_BOOST_LOOP_STAB_COMP_RES_MASK,
				      WSA881X_BOOST_LOOP_STAB_COMP_RES_400K));
	regmap_update_bits(rm, WSA881X_BOOST_MISC2_CTL,
			   WSA881X_BOOST_MISC2_CTL_FULL_MASK,
			   WSA881X_BOOST_MISC2_CTL_RST);
	regmap_update_bits(rm, WSA881X_BOOST_START_CTL,
			   WSA881X_BOOST_START_CTL_FAST_TRAN_MASK,
			   WSA881X_BOOST_START_CTL_FAST_TRAN_EN);
	regmap_update_bits(rm, WSA881X_BOOST_START_CTL,
			   WSA881X_BOOST_START_CTL_PULSE_SKIP_MASK,
			   FIELD_PREP(WSA881X_BOOST_START_CTL_PULSE_SKIP_MASK,
				      WSA881X_BOOST_START_CTL_PULSE_SKIP_50MA));
	regmap_update_bits(rm, WSA881X_BOOST_SLOPE_COMP_ISENSE_FB,
			   WSA881X_BOOST_SLOPE_ERR_CURR_MASK,
			   FIELD_PREP(WSA881X_BOOST_SLOPE_ERR_CURR_MASK,
				      WSA881X_BOOST_SLOPE_ERR_CURR_11UA));
	regmap_update_bits(rm, WSA881X_BOOST_SLOPE_COMP_ISENSE_FB,
			   WSA881X_BOOST_SLOPE_ISENSE_FB_MASK,
			   FIELD_PREP(WSA881X_BOOST_SLOPE_ISENSE_FB_MASK,
				      WSA881X_BOOST_SLOPE_ISENSE_FB_03));

	regmap_read(rm, WSA881X_OTP_REG_0, &val);
	if (val)
		regmap_update_bits(rm,
			WSA881X_BOOST_PRESET_OUT1,
			WSA881X_BOOST_PRESET_OUT1_1ST_LVL_MASK,
			FIELD_PREP(WSA881X_BOOST_PRESET_OUT1_1ST_LVL_MASK,
				   WSA881X_BOOST_PRESET_OUT1_5V5));

	regmap_update_bits(rm, WSA881X_BOOST_PRESET_OUT2,
			   WSA881X_BOOST_PRESET_OUT2_3RD_LVL_MASK,
			   FIELD_PREP(WSA881X_BOOST_PRESET_OUT2_3RD_LVL_MASK,
				      WSA881X_BOOST_PRESET_OUT2_9V));
	regmap_update_bits(rm, WSA881X_SPKR_DRV_EN,
			   WSA881X_SPKR_DRV_EN_INT_LDO_VOUT_MASK,
			   FIELD_PREP(WSA881X_SPKR_DRV_EN_INT_LDO_VOUT_MASK,
				      WSA881X_SPKR_DRV_EN_INT_LDO_VOUT_5V5));
	regmap_update_bits(rm, WSA881X_BOOST_CURRENT_LIMIT,
			   WSA881X_BOOST_CURRENT_LIMIT_SET_MASK,
			   FIELD_PREP(WSA881X_BOOST_CURRENT_LIMIT_SET_MASK,
				      WSA881X_BOOST_CURRENT_LIMIT_SET_4A));
	regmap_update_bits(rm, WSA881X_SPKR_OCP_CTL,
			   WSA881X_SPKR_OCP_CTL_CURR_LIMIT_MASK,
			   FIELD_PREP(WSA881X_SPKR_OCP_CTL_CURR_LIMIT_MASK,
				      WSA881X_SPKR_OCP_CTL_CURR_LIMIT_5A));
	regmap_update_bits(rm, WSA881X_SPKR_OCP_CTL,
			   WSA881X_SPKR_OCP_CTL_GLITCH_FLT_MASK,
			   FIELD_PREP(WSA881X_SPKR_OCP_CTL_GLITCH_FLT_MASK,
				      WSA881X_SPKR_OCP_CTL_GLITCH_FLT_128NS));
	regmap_update_bits(rm, WSA881X_OTP_REG_28,
			   WSA881X_OTP_REG_28_ISENSE_CAL_MASK,
			   FIELD_PREP(WSA881X_OTP_REG_28_ISENSE_CAL_MASK,
				      WSA881X_OTP_REG_28_ISENSE_CAL_RST_VAL));
	regmap_update_bits(rm, WSA881X_BONGO_RESRV_REG1,
			   WSA881X_BONGO_RESRV_REG1_TEMP_CMP_MASK,
			   WSA881X_BONGO_RESRV_REG1_TEMP_CMP_EN);
	regmap_update_bits(rm, WSA881X_BONGO_RESRV_REG1,
			   WSA881X_BONGO_RESRV_REG1_ISENSE_MASK,
			   FIELD_PREP(WSA881X_BONGO_RESRV_REG1_ISENSE_MASK,
				      WSA881X_BONGO_RESRV_REG1_ISENSE_RST_VAL));
	regmap_update_bits(rm, WSA881X_BONGO_RESRV_REG1,
			   WSA881X_BONGO_RESRV_REG1_ATEST_MASK,
			   WSA881X_BONGO_RESRV_REG1_ATEST_DIS);
	regmap_update_bits(rm, WSA881X_BONGO_RESRV_REG2,
			   WSA881X_BONGO_RESRV_REG2_FULL_MASK,
			   WSA881X_BONGO_RESRV_REG2_RST_VAL);
}
EXPORT_SYMBOL_GPL(wsa881x_init_common);

int wsa881x_probe_common(struct wsa881x_priv **wsa881x, struct device *dev)
{
	struct wsa881x_priv *wsa;

	wsa = devm_kzalloc(dev, sizeof(*wsa), GFP_KERNEL);
	if (!wsa)
		return -ENOMEM;

	wsa->dev = dev;
	wsa->sd_n = devm_gpiod_get_optional(dev, "powerdown",
					    GPIOD_FLAGS_BIT_NONEXCLUSIVE);
	if (IS_ERR(wsa->sd_n))
		return dev_err_probe(dev, PTR_ERR(wsa->sd_n),
				     "Shutdown Control GPIO not found\n");
	/*
	 * Backwards compatibility work-around.
	 *
	 * The SD_N GPIO is active low, however upstream DTS used always active
	 * high.  Changing the flag in driver and DTS will break backwards
	 * compatibility, so add a simple value inversion to work with both old
	 * and new DTS.
	 *
	 * This won't work properly with DTS using the flags properly in cases:
	 * 1. Old DTS with proper ACTIVE_LOW, however such case was broken
	 *    before as the driver required the active high.
	 * 2. New DTS with proper ACTIVE_HIGH (intended), which is rare case
	 *    (not existing upstream) but possible. This is the price of
	 *    backwards compatibility, therefore this hack should be removed at
	 *    some point.
	 */
	wsa->sd_n_val = gpiod_is_active_low(wsa->sd_n);
	if (!wsa->sd_n_val)
		dev_warn(dev,
			 "Using ACTIVE_HIGH for shutdown GPIO. Your DTB might be outdated or you use unsupported configuration for the GPIO.\n");

	dev_set_drvdata(dev, wsa);
	gpiod_direction_output(wsa->sd_n, !wsa->sd_n_val);

	*wsa881x = wsa;

	return 0;
}
EXPORT_SYMBOL_GPL(wsa881x_probe_common);

MODULE_DESCRIPTION("WSA881x codec helper driver");
MODULE_LICENSE("GPL");

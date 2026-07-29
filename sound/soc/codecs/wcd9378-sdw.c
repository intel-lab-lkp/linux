// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2026, Jorijn van der Graaf
 *
 * SoundWire slave driver for the Qualcomm WCD9378 audio codec.
 *
 * The codec presents two SoundWire slaves (RX and TX, mfg 0x0217 part
 * 0x0110); the SDCA control space is a 32-bit paged register map accessed
 * through the TX slave.
 */

#include <linux/component.h>
#include <linux/device.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/pm_runtime.h>
#include <linux/regmap.h>
#include <linux/soundwire/sdw.h>
#include <linux/soundwire/sdw_registers.h>
#include <linux/soundwire/sdw_type.h>
#include <sound/pcm_params.h>

#include "wcd-common.h"
#include "wcd9378.h"

#define WCD9378_SDW_CH(id, pn, cmask, mmask)	\
	[id] = {				\
		.port_num = pn,			\
		.ch_mask = cmask,		\
		.master_ch_mask = mmask,	\
	}

/*
 * Each ADC sits alone on its own TX device port (channel 1); by default
 * they land on channels 1/2/3 of the same master port. DMIC/MBHC masks
 * per the downstream qcom,tx_swr_ch_map.
 */
static const struct wcd_sdw_ch_info wcd9378_sdw_tx_ch_info[] = {
	WCD9378_SDW_CH(WCD9378_ADC1, WCD9378_ADC_1_PORT, BIT(0), BIT(0)),
	WCD9378_SDW_CH(WCD9378_ADC2, WCD9378_ADC_2_PORT, BIT(0), BIT(1)),
	WCD9378_SDW_CH(WCD9378_ADC3, WCD9378_ADC_3_PORT, BIT(0), BIT(2)),
	WCD9378_SDW_CH(WCD9378_DMIC0, WCD9378_DMIC_0_1_MBHC_PORT, BIT(2), BIT(0)),
	WCD9378_SDW_CH(WCD9378_DMIC1, WCD9378_DMIC_0_1_MBHC_PORT, BIT(3), BIT(1)),
	WCD9378_SDW_CH(WCD9378_MBHC, WCD9378_DMIC_0_1_MBHC_PORT, BIT(2), BIT(2)),
	WCD9378_SDW_CH(WCD9378_DMIC2, WCD9378_DMIC_2_5_PORT, BIT(0), BIT(2)),
	WCD9378_SDW_CH(WCD9378_DMIC3, WCD9378_DMIC_2_5_PORT, BIT(1), BIT(3)),
	WCD9378_SDW_CH(WCD9378_DMIC4, WCD9378_DMIC_2_5_PORT, BIT(2), BIT(0)),
	WCD9378_SDW_CH(WCD9378_DMIC5, WCD9378_DMIC_2_5_PORT, BIT(3), BIT(1)),
};

static const struct wcd_sdw_ch_info wcd9378_sdw_rx_ch_info[] = {
	WCD9378_SDW_CH(WCD9378_HPH_L, WCD9378_HPH_PORT, BIT(0), BIT(0)),
	WCD9378_SDW_CH(WCD9378_HPH_R, WCD9378_HPH_PORT, BIT(1), BIT(1)),
	WCD9378_SDW_CH(WCD9378_CLSH, WCD9378_CLSH_PORT, BIT(0), BIT(0)),
	WCD9378_SDW_CH(WCD9378_COMP_L, WCD9378_COMP_PORT, BIT(0), BIT(0)),
	WCD9378_SDW_CH(WCD9378_COMP_R, WCD9378_COMP_PORT, BIT(1), BIT(1)),
	WCD9378_SDW_CH(WCD9378_LO, WCD9378_LO_PORT, BIT(0), BIT(0)),
	WCD9378_SDW_CH(WCD9378_DSD_L, WCD9378_DSD_PORT, BIT(0), BIT(0)),
	WCD9378_SDW_CH(WCD9378_DSD_R, WCD9378_DSD_PORT, BIT(1), BIT(1)),
};

static struct sdw_dpn_prop wcd9378_dpn_prop[WCD9378_MAX_SWR_PORTS] = {
	{
		.num = 1,
		.type = SDW_DPN_SIMPLE,
		.min_ch = 1,
		.max_ch = 8,
		.simple_ch_prep_sm = true,
	}, {
		.num = 2,
		.type = SDW_DPN_SIMPLE,
		.min_ch = 1,
		.max_ch = 4,
		.simple_ch_prep_sm = true,
	}, {
		.num = 3,
		.type = SDW_DPN_SIMPLE,
		.min_ch = 1,
		.max_ch = 4,
		.simple_ch_prep_sm = true,
	}, {
		.num = 4,
		.type = SDW_DPN_SIMPLE,
		.min_ch = 1,
		.max_ch = 4,
		.simple_ch_prep_sm = true,
	}, {
		.num = 5,
		.type = SDW_DPN_SIMPLE,
		.min_ch = 1,
		.max_ch = 4,
		.simple_ch_prep_sm = true,
	}
};

int wcd9378_sdw_hw_params(struct wcd9378_sdw_priv *wcd,
			  struct snd_pcm_substream *substream,
			  struct snd_pcm_hw_params *params,
			  struct snd_soc_dai *dai)
{
	struct sdw_port_config port_config[WCD9378_MAX_SWR_PORTS];
	unsigned long ch_mask;
	int i, j;

	wcd->sconfig.ch_count = 1;
	wcd->active_ports = 0;
	for (i = 0; i < WCD9378_MAX_SWR_PORTS; i++) {
		ch_mask = wcd->port_config[i].ch_mask;
		if (!ch_mask)
			continue;

		for_each_set_bit(j, &ch_mask, 4)
			wcd->sconfig.ch_count++;

		port_config[wcd->active_ports] = wcd->port_config[i];
		wcd->active_ports++;
	}

	wcd->sconfig.bps = 1;
	wcd->sconfig.frame_rate = params_rate(params);
	wcd->sconfig.direction = wcd->is_tx ? SDW_DATA_DIR_TX : SDW_DATA_DIR_RX;
	wcd->sconfig.type = SDW_STREAM_PCM;

	return sdw_stream_add_slave(wcd->sdev, &wcd->sconfig,
				    &port_config[0], wcd->active_ports,
				    wcd->sruntime);
}
EXPORT_SYMBOL_GPL(wcd9378_sdw_hw_params);

static const struct sdw_slave_ops wcd9378_slave_ops = {
	.update_status = wcd_update_status,
	.bus_config = wcd_bus_config,
};

static const struct reg_default wcd9378_defaults[] = {
	{ WCD9378_ANA_BIAS,			0x00 },
	{ WCD9378_ANA_TX_CH1,			0x20 },
	{ WCD9378_ANA_TX_CH2,			0x00 },
	{ WCD9378_ANA_TX_CH3,			0x20 },
	{ WCD9378_ANA_TX_CH3_HPF,		0x00 },
	{ WCD9378_ANA_MICB2_RAMP,		0x00 },
	{ WCD9378_BIAS_VBG_FINE_ADJ,		0x55 },
	{ WCD9378_MBHC_CTL_SPARE_1,		0x02 },
	{ WCD9378_MICB1_TEST_CTL_2,		0x00 },
	{ WCD9378_MICB2_TEST_CTL_2,		0x00 },
	{ WCD9378_MICB3_TEST_CTL_2,		0x80 },
	{ WCD9378_TX_COM_TXFE_DIV_CTL,		0x22 },
	{ WCD9378_SLEEP_CTL,			0x16 },
	{ WCD9378_TX_NEW_CH12_MUX,		0x11 },
	{ WCD9378_TX_NEW_CH34_MUX,		0x23 },
	{ WCD9378_TOP_CLK_CFG,			0x00 },
	{ WCD9378_CDC_ANA_TX_CLK_CTL,		0x0e },
	{ WCD9378_CDC_AMIC_CTL,			0x07 },
	{ WCD9378_PDM_WD_CTL0,			0x0f },
	{ WCD9378_PDM_WD_CTL1,			0x0f },
	{ WCD9378_PLATFORM_CTL,			0x01 },
	{ WCD9378_SYS_USAGE_CTRL,		0x00 },
	{ WCD9378_HPH_UP_T0,			0x02 },
	{ WCD9378_HPH_UP_T9,			0x02 },
	{ WCD9378_HPH_DN_T0,			0x05 },
	{ WCD9378_MICB_REMAP_TABLE_VAL_3,	0x00 },
	{ WCD9378_MICB_REMAP_TABLE_VAL_4,	0x00 },
	{ WCD9378_MICB_REMAP_TABLE_VAL_5,	0x00 },
	{ WCD9378_SM0_MB_SEL,			0x00 },
	{ WCD9378_SM1_MB_SEL,			0x00 },
	{ WCD9378_SM2_MB_SEL,			0x00 },
	{ WCD9378_MB_PULLUP_EN,			0x00 },
	{ WCD9378_SMP_AMP_FUNC_ACT,		0x00 },
	{ WCD9378_CMT_GRP_MASK,			0x00 },
	{ WCD9378_SMP_JACK_IT31_MICB,		0x00 },
	{ WCD9378_SMP_JACK_IT31_USAGE,		0x03 },
	{ WCD9378_SMP_JACK_PDE34_REQ_PS,	0x03 },
	{ WCD9378_SMP_JACK_FUNC_ACT,		0x00 },
	{ WCD9378_SMP_MIC_IT11_MICB(0),		0x00 },
	{ WCD9378_SMP_MIC_IT11_USAGE(0),	0x03 },
	{ WCD9378_SMP_MIC_PDE11_REQ_PS(0),	0x03 },
	{ WCD9378_SMP_MIC_FUNC_ACT(0),		0x00 },
	{ WCD9378_SMP_MIC_IT11_MICB(1),		0x00 },
	{ WCD9378_SMP_MIC_IT11_USAGE(1),	0x03 },
	{ WCD9378_SMP_MIC_PDE11_REQ_PS(1),	0x03 },
	{ WCD9378_SMP_MIC_FUNC_ACT(1),		0x00 },
	{ WCD9378_SMP_MIC_IT11_MICB(2),		0x00 },
	{ WCD9378_SMP_MIC_IT11_USAGE(2),	0x03 },
	{ WCD9378_SMP_MIC_PDE11_REQ_PS(2),	0x03 },
	{ WCD9378_SMP_MIC_FUNC_ACT(2),		0x00 },
};

static bool wcd9378_rdwr_register(struct device *dev, unsigned int reg)
{
	switch (reg) {
	case WCD9378_ANA_BIAS:
	case WCD9378_ANA_TX_CH1:
	case WCD9378_ANA_TX_CH2:
	case WCD9378_ANA_TX_CH3:
	case WCD9378_ANA_TX_CH3_HPF:
	case WCD9378_ANA_MICB2_RAMP:
	case WCD9378_BIAS_VBG_FINE_ADJ:
	case WCD9378_MBHC_CTL_SPARE_1:
	case WCD9378_MICB1_TEST_CTL_2:
	case WCD9378_MICB2_TEST_CTL_2:
	case WCD9378_MICB3_TEST_CTL_2:
	case WCD9378_TX_COM_TXFE_DIV_CTL:
	case WCD9378_SLEEP_CTL:
	case WCD9378_TX_NEW_CH12_MUX:
	case WCD9378_TX_NEW_CH34_MUX:
	case WCD9378_HPH_RDAC_GAIN_CTL:
	case WCD9378_HPH_RDAC_HD2_CTL_L:
	case WCD9378_HPH_RDAC_HD2_CTL_R:
	case WCD9378_TOP_CLK_CFG:
	case WCD9378_CDC_ANA_TX_CLK_CTL:
	case WCD9378_CDC_AMIC_CTL:
	case WCD9378_PDM_WD_CTL0:
	case WCD9378_PDM_WD_CTL1:
	case WCD9378_PLATFORM_CTL:
	case WCD9378_SYS_USAGE_CTRL:
	case WCD9378_HPH_UP_T0:
	case WCD9378_HPH_UP_T9:
	case WCD9378_HPH_DN_T0:
	case WCD9378_MICB_REMAP_TABLE_VAL_3:
	case WCD9378_MICB_REMAP_TABLE_VAL_4:
	case WCD9378_MICB_REMAP_TABLE_VAL_5:
	case WCD9378_SM0_MB_SEL:
	case WCD9378_SM1_MB_SEL:
	case WCD9378_SM2_MB_SEL:
	case WCD9378_MB_PULLUP_EN:
	case WCD9378_SMP_AMP_FUNC_STAT:
	case WCD9378_SMP_AMP_FUNC_ACT:
	case WCD9378_CMT_GRP_MASK:
	case WCD9378_SMP_JACK_IT31_MICB:
	case WCD9378_SMP_JACK_IT31_USAGE:
	case WCD9378_SMP_JACK_PDE34_REQ_PS:
	case WCD9378_SMP_JACK_FUNC_STAT:
	case WCD9378_SMP_JACK_FUNC_ACT:
	case WCD9378_SMP_MIC_IT11_MICB(0):
	case WCD9378_SMP_MIC_IT11_USAGE(0):
	case WCD9378_SMP_MIC_PDE11_REQ_PS(0):
	case WCD9378_SMP_MIC_FUNC_STAT(0):
	case WCD9378_SMP_MIC_FUNC_ACT(0):
	case WCD9378_SMP_MIC_IT11_MICB(1):
	case WCD9378_SMP_MIC_IT11_USAGE(1):
	case WCD9378_SMP_MIC_PDE11_REQ_PS(1):
	case WCD9378_SMP_MIC_FUNC_STAT(1):
	case WCD9378_SMP_MIC_FUNC_ACT(1):
	case WCD9378_SMP_MIC_IT11_MICB(2):
	case WCD9378_SMP_MIC_IT11_USAGE(2):
	case WCD9378_SMP_MIC_PDE11_REQ_PS(2):
	case WCD9378_SMP_MIC_FUNC_STAT(2):
	case WCD9378_SMP_MIC_FUNC_ACT(2):
		return true;
	}

	return false;
}

static bool wcd9378_volatile_register(struct device *dev, unsigned int reg)
{
	switch (reg) {
	case WCD9378_FUNC_EXT_ID_0:
	case WCD9378_FUNC_EXT_ID_1:
	case WCD9378_FUNC_EXT_VER:
	case WCD9378_FUNC_STAT:
	case WCD9378_DEV_MANU_ID_0:
	case WCD9378_DEV_MANU_ID_1:
	case WCD9378_DEV_PART_ID_0:
	case WCD9378_DEV_PART_ID_1:
	case WCD9378_DEV_VER:
	case WCD9378_EFUSE_REG_16:
	case WCD9378_EFUSE_REG_29:
	case WCD9378_SEQ_TX0_STAT:
	case WCD9378_SEQ_TX1_STAT:
	case WCD9378_SEQ_TX2_STAT:
	case WCD9378_SMP_AMP_FUNC_STAT:
	case WCD9378_SMP_JACK_FUNC_STAT:
	case WCD9378_SMP_JACK_PDE34_ACT_PS:
	case WCD9378_SMP_MIC_FUNC_STAT(0):
	case WCD9378_SMP_MIC_FUNC_STAT(1):
	case WCD9378_SMP_MIC_FUNC_STAT(2):
	case WCD9378_SMP_MIC_OT10_USAGE(0):
	case WCD9378_SMP_MIC_PDE11_ACT_PS(0):
	case WCD9378_SMP_MIC_OT10_USAGE(1):
	case WCD9378_SMP_MIC_PDE11_ACT_PS(1):
	case WCD9378_SMP_MIC_OT10_USAGE(2):
	case WCD9378_SMP_MIC_PDE11_ACT_PS(2):
		return true;
	}

	return false;
}

static bool wcd9378_readable_register(struct device *dev, unsigned int reg)
{
	if (wcd9378_volatile_register(dev, reg))
		return true;

	return wcd9378_rdwr_register(dev, reg);
}

static const struct regmap_config wcd9378_regmap_config = {
	.name = "wcd9378_csr",
	.reg_bits = 32,
	.val_bits = 8,
	.cache_type = REGCACHE_MAPLE,
	.reg_defaults = wcd9378_defaults,
	.num_reg_defaults = ARRAY_SIZE(wcd9378_defaults),
	.max_register = WCD9378_MAX_REGISTER,
	.readable_reg = wcd9378_readable_register,
	.writeable_reg = wcd9378_rdwr_register,
	.volatile_reg = wcd9378_volatile_register,
};

static int wcd9378_sdw_probe(struct sdw_slave *pdev,
			     const struct sdw_device_id *id)
{
	struct device *dev = &pdev->dev;
	struct wcd9378_sdw_priv *wcd;
	unsigned int num_ports;
	int ret;

	/*
	 * The codec enumerates with the same device ID in both of its
	 * operating modes. In SDCA mode it is a single aggregated slave
	 * carrying control and data, served by the SDCA driver; this
	 * driver handles the mobile presentation, where the codec is
	 * split over an RX and a TX slave.
	 */
	if (of_property_read_bool(dev->of_node, "qcom,compute-mode"))
		return -ENODEV;

	wcd = devm_kzalloc(dev, sizeof(*wcd), GFP_KERNEL);
	if (!wcd)
		return -ENOMEM;

	/* Only the TX slave reaches the codec control register space */
	wcd->is_tx = of_property_read_bool(dev->of_node, "qcom,control-device");

	if (wcd->is_tx)
		num_ports = WCD9378_MAX_TX_SWR_PORTS;
	else
		num_ports = WCD9378_MAX_SWR_PORTS;

	/* Port map index starts at 0, however the data ports start at index 1 */
	ret = of_property_read_u32_array(dev->of_node, "qcom,port-mapping",
					 &pdev->m_port_map[1], num_ports);
	if (ret < 0)
		dev_info(dev, "Error getting static port mapping for %s (%d)\n",
			 wcd->is_tx ? "TX" : "RX", ret);

	wcd->sdev = pdev;
	dev_set_drvdata(dev, wcd);

	pdev->prop.scp_int1_mask = SDW_SCP_INT1_BUS_CLASH |
				   SDW_SCP_INT1_PARITY;
	pdev->prop.lane_control_support = true;
	pdev->prop.simple_clk_stop_capable = true;
	/* The SDCA control space sits above the 16-bit address range */
	pdev->prop.paging_support = true;
	/* Have the core program the SCP bus-clock base/scale registers */
	pdev->prop.clock_reg_supported = true;

	if (wcd->is_tx) {
		pdev->prop.source_ports = GENMASK(WCD9378_MAX_TX_SWR_PORTS, 1);
		pdev->prop.src_dpn_prop = wcd9378_dpn_prop;
		wcd->ch_info = wcd9378_sdw_tx_ch_info;
		pdev->prop.wake_capable = true;

		wcd->regmap = devm_regmap_init_sdw(pdev, &wcd9378_regmap_config);
		if (IS_ERR(wcd->regmap))
			return dev_err_probe(dev, PTR_ERR(wcd->regmap),
					     "Regmap init failed\n");

		/* Start in cache-only until device is enumerated */
		regcache_cache_only(wcd->regmap, true);
	} else {
		pdev->prop.sink_ports = GENMASK(WCD9378_MAX_SWR_PORTS, 1);
		pdev->prop.sink_dpn_prop = wcd9378_dpn_prop;
		wcd->ch_info = wcd9378_sdw_rx_ch_info;
	}

	return component_add(dev, &wcd_sdw_component_ops);
}

static void wcd9378_sdw_remove(struct sdw_slave *pdev)
{
	struct device *dev = &pdev->dev;

	/*
	 * Runtime PM for the slave is enabled in wcd-common's component
	 * bind and disabled in its unbind, which this component_del()
	 * triggers when the aggregate device tears down.
	 */
	component_del(dev, &wcd_sdw_component_ops);
}

static const struct sdw_device_id wcd9378_sdw_id[] = {
	SDW_SLAVE_ENTRY(0x0217, 0x0110, 0),
	{ },
};
MODULE_DEVICE_TABLE(sdw, wcd9378_sdw_id);

static int wcd9378_sdw_runtime_suspend(struct device *dev)
{
	struct wcd9378_sdw_priv *wcd = dev_get_drvdata(dev);

	if (wcd->regmap) {
		regcache_cache_only(wcd->regmap, true);
		regcache_mark_dirty(wcd->regmap);
	}

	return 0;
}

static int wcd9378_sdw_runtime_resume(struct device *dev)
{
	struct wcd9378_sdw_priv *wcd = dev_get_drvdata(dev);

	if (wcd->regmap) {
		regcache_cache_only(wcd->regmap, false);
		regcache_sync(wcd->regmap);
	}

	return 0;
}

static const struct dev_pm_ops wcd9378_sdw_pm_ops = {
	RUNTIME_PM_OPS(wcd9378_sdw_runtime_suspend, wcd9378_sdw_runtime_resume, NULL)
};

static struct sdw_driver wcd9378_sdw_driver = {
	.probe = wcd9378_sdw_probe,
	.remove = wcd9378_sdw_remove,
	.ops = &wcd9378_slave_ops,
	.id_table = wcd9378_sdw_id,
	.driver = {
		.name = "wcd9378-sdw",
		.pm = pm_ptr(&wcd9378_sdw_pm_ops),
	}
};
module_sdw_driver(wcd9378_sdw_driver);

MODULE_DESCRIPTION("WCD9378 SDW codec driver");
MODULE_LICENSE("GPL");

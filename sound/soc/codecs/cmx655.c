// SPDX-License-Identifier: GPL-2.0-only

#include <linux/version.h>
#include <linux/regmap.h>
#include <linux/module.h>
#include <linux/interrupt.h>
#include <linux/delay.h>
#include <linux/gpio.h>
#include <linux/of.h>
#include <sound/soc.h>
#include <sound/soc-dapm.h>
#include <sound/tlv.h>

#include "cmx655.h"

static const struct reg_default cmx655_reg_defaults[] = {
	{ CMX655_ISR, 0x00 },
	{ CMX655_ISM, 0x00 },
	{ CMX655_ISE, 0x00 },
	{ CMX655_CLKCTRL, 0x00 },
	{ CMX655_RDIVHI, 0x00 },
	{ CMX655_RDIVLO, 0x00 },
	{ CMX655_NDIVHI, 0x00 },
	{ CMX655_NDIVLO, 0x00 },
	{ CMX655_PLLCTRL, 0x00 },
	{ CMX655_SAICTRL, 0x00 },
	{ CMX655_SAIMUX, 0x00 },

	{ CMX655_RVF, 0x00 },
	{ CMX655_LDCTRL, 0x00 },
	{ CMX655_RDCTRL, 0x00 },
	{ CMX655_LEVEL, 0x00 },

	{ CMX655_NGCTRL, 0x00 },
	{ CMX655_NGTIME, 0x00 },
	{ CMX655_NGLSTAT, 0x00 },
	{ CMX655_NGRSTAT, 0x00 },

	{ CMX655_PVF, 0x00 },
	{ CMX655_PREAMP, 0x00 },
	{ CMX655_VOLUME, 0x00 },
	{ CMX655_ALCCTRL, 0x00 },
	{ CMX655_ALCTIME, 0x00 },
	{ CMX655_ALCGAIN, 0x00 },
	{ CMX655_ALCSTAT, 0x00 },
	{ CMX655_DST, 0x00 },
	{ CMX655_CPR, 0x00 },

	{ CMX655_SYSCTRL, 0x00 },
	{ CMX655_COMMAND, 0x00 },
	{ 0x34, 0x29 },
	{ 0x35, 0x40 },
	{ 0x36, 0x80 },
	{ 0x37, 0x80 },

};

static const struct regmap_range cmx655_valid_reg[] = {
	{ 0x00, 0x0a },
	{ 0x0c, 0x0f },
	{ 0x1c, 0x1f },
	{ 0x28, 0x30 },
	{ 0x32, 0x33 }
};

static const struct regmap_range cmx655_read_only_reg[] = {
	{ 0x00, 0x00 },
	{ 0x1e, 0x1f },
	{ 0x2e, 0x2e }
};

static const struct regmap_range cmx655_write_only_reg[] = {
	{ 0x33, 0x33 }
};

static const struct regmap_access_table cmx655_readable_reg = {
	.yes_ranges = cmx655_valid_reg,
	.n_yes_ranges = ARRAY_SIZE(cmx655_valid_reg),
	.no_ranges = cmx655_write_only_reg,
	.n_no_ranges = ARRAY_SIZE(cmx655_write_only_reg)
};

static const struct regmap_access_table cmx655_writeable_reg = {
	.yes_ranges = cmx655_valid_reg,
	.n_yes_ranges = ARRAY_SIZE(cmx655_valid_reg),
	.no_ranges = cmx655_read_only_reg,
	.n_no_ranges = ARRAY_SIZE(cmx655_read_only_reg)
};

static bool cmx655_volatile_reg(struct device *dev, unsigned int reg)
{
	switch (reg) {
	case CMX655_COMMAND:
	case CMX655_SYSCTRL:
	case CMX655_ISR:
	case CMX655_ISE:
		return true;
	default:
		return false;
	}
};

const struct regmap_config cmx655_regmap = {
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = CMX655_COMMAND,

	.volatile_reg = cmx655_volatile_reg,
	.wr_table = &cmx655_writeable_reg,
	.rd_table = &cmx655_readable_reg,

	.reg_defaults = cmx655_reg_defaults,
	.num_reg_defaults = ARRAY_SIZE(cmx655_reg_defaults),
	.cache_type = REGCACHE_MAPLE,
};
EXPORT_SYMBOL_GPL(cmx655_regmap);

static int cmx655_start_sys_clk(struct snd_soc_component *component)
{
	int ret;
	int i;
	unsigned int val;

	val = snd_soc_component_read(component, CMX655_ISR);
	ret = snd_soc_component_write(component, CMX655_COMMAND,
				      CMX655_CMD_CLOCK_START);
	if (ret < 0) {
		dev_err(component->dev,
			"Failed to write start clock command %d\n", ret);
		return ret;
	}

	for (i = 0; i < 100; i++) {
		val = snd_soc_component_read(component, CMX655_ISR);
		if (val & CMX655_ISR_CLKRDY)
			break;
	}

	if (i == 100)
		ret = -EIO;
	return ret;
}

static int cmx655_stop_sys_clk(struct snd_soc_component *component)
{
	return snd_soc_component_write(component, CMX655_COMMAND,
				       CMX655_CMD_CLOCK_STOP);
}

/**
 * cmx655_get_sys_clk_config(): Get the clock configuration.
 * @clk_id: Clock source setting as defined in cmx655.h
 * @primary_mode: Non-zero if the CMX655 is the DAI primary
 * @sr_setting: Setting for sample rate 0 to 3
 * @clk_src: pointer for storing clock source (PLLREF, PLLSEL and
 *           CLKSEL bits)
 * @rdiv: pointer for storing PLL's RDIV value (13 bits)
 * @ndiv: pointer for storing PLL's NDIV value (13 bits)
 * @pll_ctrl: pointer for storing PLLCTRL register value (8 bits)
 *
 * Get the clock setup for the system clock based on clock Id, DAI primary mode
 * and sample rate.
 *
 * Return: 0 for success, negative value otherwise
 */
static int cmx655_get_sys_clk_config(int clk_id,
				     int primary_mode,
				     int sr_setting,
				     int *clk_src,
				     int *rdiv, int *ndiv, int *pll_ctrl)
{
	if (clk_id == CMX655_SYSCLK_AUTO) {
		if (primary_mode != 0)
			clk_id = CMX655_SYSCLK_RCLK;
		else
			clk_id = CMX655_SYSCLK_LRCLK;
	}
	*rdiv = 0;
	*ndiv = 0;
	*pll_ctrl = 0;
	switch (clk_id) {
	case CMX655_SYSCLK_RCLK:
		*clk_src = CMX655_CLKCTRL_CLRSRC_RCLK;
		break;
	case CMX655_SYSCLK_LPO:
		*clk_src = CMX655_CLKCTRL_CLRSRC_LPO;
		break;
	case CMX655_SYSCLK_LRCLK:
		*clk_src = CMX655_CLKCTRL_CLRSRC_LRCLK;
		*rdiv = 1;
		switch (sr_setting) {
		case CMX655_CLKCTRL_SR_8K:
			*ndiv = 3072;
			*pll_ctrl = (0 << CMX655_PLLCTRL_LFILT_SHIFT) ||
			    (3 << CMX655_PLLCTRL_CPI_SHIFT);
			break;
		case CMX655_CLKCTRL_SR_16K:
			*ndiv = 1536;
			*pll_ctrl = (0 << CMX655_PLLCTRL_LFILT_SHIFT) ||
			    (3 << CMX655_PLLCTRL_CPI_SHIFT);
			break;
		case CMX655_CLKCTRL_SR_32K:
			*ndiv = 768;
			*pll_ctrl = (12 << CMX655_PLLCTRL_LFILT_SHIFT) ||
			    (3 << CMX655_PLLCTRL_CPI_SHIFT);
			break;
		case CMX655_CLKCTRL_SR_48K:
			*ndiv = 512;
			*pll_ctrl = (12 << CMX655_PLLCTRL_LFILT_SHIFT) ||
			    (3 << CMX655_PLLCTRL_CPI_SHIFT);
			break;
		default:
			return -EINVAL;
		}
		break;
	default:
		return -EINVAL;
	}
	return 0;
};

/**
 * cmx655_setup_rate(): Setup the clock and sample rate.
 * @component: sound component to set up
 * @hw_params: hardware parameters holding the sample rate
 *
 * Setup the clock and sample rate. The clock needs to be setup at the same
 * time as the sample rate in case we are using the serial port as the clock
 * source.
 * If the clock source the serial port then the PLL settings are dependent on
 * the sample rate.
 *
 * Return: 0 for success, negative for error.
 */
static int cmx655_setup_rate(struct snd_soc_component *component,
			     struct snd_pcm_hw_params *hw_params)
{
	int ret;
	struct cmx655_data *cmx655_data =
	    snd_soc_component_get_drvdata(component);
	struct cmx655_dai_data *cmx655_dai_data = &cmx655_data->dai_data;
	int srate = params_rate(hw_params);
	int primary_mode;
	int srate_setting;
	int clk_src;
	int rdiv;
	int ndiv;
	int pll_ctrl;
	int sys_ctrl;
	int vol;

	primary_mode = snd_soc_component_read(component, CMX655_SAICTRL);
	primary_mode = primary_mode & CMX655_SAI_MSTR;

	switch (srate) {
	case 8000:
		srate_setting = CMX655_CLKCTRL_SR_8K;
		break;
	case 16000:
		srate_setting = CMX655_CLKCTRL_SR_16K;
		break;
	case 32000:
		srate_setting = CMX655_CLKCTRL_SR_32K;
		break;
	case 48000:
		srate_setting = CMX655_CLKCTRL_SR_48K;
		break;
	default:
		dev_err(component->dev, "Unsupported rate %d\n", srate);
		return -EINVAL;
	}

	ret = cmx655_get_sys_clk_config(cmx655_dai_data->sys_clk,
					primary_mode, srate_setting,
					&clk_src, &rdiv, &ndiv, &pll_ctrl);
	if (ret < 0) {
		dev_err(component->dev,
			"Failed to get system clock settings %i\n", ret);
		return ret;
	}

	if (clk_src == CMX655_CLKCTRL_CLRSRC_LRCLK) {
		dev_dbg(component->dev,
			"Using LRCLK as clk source. Using LPO for setup then switch over to LRCLK later");
		cmx655_dai_data->clk_src = clk_src;
		cmx655_dai_data->best_clk_running = false;
		clk_src = CMX655_CLKCTRL_CLRSRC_LPO;
	} else {
		cmx655_dai_data->best_clk_running = true;
	}

	if (snd_soc_component_test_bits(component, CMX655_CLKCTRL,
					CMX655_CLKCTRL_CLRSRC_MASK |
					CMX655_CLKCTRL_SR_MASK,
					clk_src | srate_setting) == 0) {
		dev_dbg(component->dev, "Rate Setup correct skipping setup\n");
		return 0;
	}
	/* Turn all inputs and outputs off before disabling clock */
	sys_ctrl = snd_soc_component_read(component, CMX655_SYSCTRL);
	snd_soc_component_update_bits(component, CMX655_SYSCTRL,
				      CMX655_SYSCTRL_MICR |
				      CMX655_SYSCTRL_MICL |
				      CMX655_SYSCTRL_PAMP |
				      CMX655_SYSCTRL_LOUT, 0);

	cmx655_stop_sys_clk(component);
	/* Set new sample rate and clock source */
	snd_soc_component_update_bits(component, CMX655_CLKCTRL,
				      CMX655_CLKCTRL_CLRSRC_MASK |
				      CMX655_CLKCTRL_SR_MASK,
				      clk_src | srate_setting);
	/* Set new RDIV */
	snd_soc_component_update_bits(component, CMX655_RDIVHI,
				      0x1F, rdiv >> 8);
	snd_soc_component_update_bits(component, CMX655_RDIVLO,
				      0xFF, rdiv & 0xFF);
	/* Set new NDIV */
	snd_soc_component_update_bits(component, CMX655_NDIVHI,
				      0x1F, ndiv >> 8);
	snd_soc_component_update_bits(component, CMX655_NDIVLO,
				      0xFF, ndiv & 0xFF);
	/* Set new PLLCTRL */
	snd_soc_component_update_bits(component, CMX655_PLLCTRL,
				      0xFF, pll_ctrl & 0xFF);
	/* Now we can re-start the clock */
	ret = cmx655_start_sys_clk(component);
	if (ret < 0) {
		dev_err(component->dev,
			"System clock failed to start %i\n", ret);
		return ret;
	}
	/* Turn anything on that we turned off */
	if ((sys_ctrl & (CMX655_SYSCTRL_MICR | CMX655_SYSCTRL_MICL)) > 0) {
		snd_soc_component_update_bits(component, CMX655_SYSCTRL,
					      CMX655_SYSCTRL_MICR |
					      CMX655_SYSCTRL_MICL, sys_ctrl);
		/* Wait for filters to settle */
		if (snd_soc_component_test_bits
		    (component,
		     CMX655_RVF,
		     CMX655_VF_DCBLOCK,
		     CMX655_VF_DCBLOCK) == 0) {
			/* DC blocking filter off, Shorter wait */
			usleep_range(3500, 4000);
		} else {
			/* This allows time for Mics and DC blocking filter to settle */
			msleep(320);
		}
	}

	if ((sys_ctrl & (CMX655_SYSCTRL_PAMP | CMX655_SYSCTRL_LOUT)) > 0) {
		/* Store volume */
		vol = snd_soc_component_read(component, CMX655_VOLUME);
		/* Lower volume with smooth on */
		snd_soc_component_write(component, CMX655_VOLUME, 0x80);
		snd_soc_component_update_bits(component, CMX655_SYSCTRL,
					      CMX655_SYSCTRL_PAMP |
					      CMX655_SYSCTRL_LOUT, sys_ctrl);
		/* Restore volume */
		snd_soc_component_write(component, CMX655_VOLUME, vol);
	}

	return 0;
};

static int cmx655_set_dai_fmt(struct snd_soc_dai *dai, unsigned int fmt)
{
	struct snd_soc_component *component = dai->component;
	unsigned int reg_val = 0;
	/* Set primary bit */
	switch (fmt & SND_SOC_DAIFMT_CLOCK_PROVIDER_MASK) {
	case SND_SOC_DAIFMT_CBM_CFM:
		reg_val = reg_val | CMX655_SAI_MSTR;
		break;
	case SND_SOC_DAIFMT_CBS_CFS:
		break;
	default:
		dev_err(component->dev,
			"Unsupported digital audio interface primary mode\n");
		return -EINVAL;
	}
	/* Set data format */
	switch (fmt & SND_SOC_DAIFMT_FORMAT_MASK) {
	case SND_SOC_DAIFMT_I2S:
		reg_val = reg_val | CMX655_SAI_DLY | CMX655_SAI_POL;
		break;
	case SND_SOC_DAIFMT_LEFT_J:
		break;
	default:
		dev_err(component->dev,
			"Unsupported digital audio interface data format\n");
		return -EINVAL;
	}
	/* Change invert bits if required */
	switch (fmt & SND_SOC_DAIFMT_INV_MASK) {
	case SND_SOC_DAIFMT_NB_NF:
		break;
	case SND_SOC_DAIFMT_NB_IF:
		reg_val = reg_val ^ CMX655_SAI_POL;
		break;
	case SND_SOC_DAIFMT_IB_NF:
		reg_val = reg_val | CMX655_SAI_BINV;
		break;
	case SND_SOC_DAIFMT_IB_IF:
		reg_val = (reg_val | CMX655_SAI_BINV) ^ CMX655_SAI_POL;
		break;
	default:
		dev_err(component->dev,
			"Unknown digital audio interface polarity\n");
		return -EINVAL;
	}

	/* Write value to codec */
	snd_soc_component_write(component, CMX655_SAICTRL, reg_val);
	return 0;
}

static int cmx655_set_dai_sysclk(struct snd_soc_dai *dai,
				 int clk_id, unsigned int freq, int dir)
{
	struct cmx655_data *cmx655_data =
	    snd_soc_component_get_drvdata(dai->component);
	struct cmx655_dai_data *cmx655_dai_data = &cmx655_data->dai_data;

	switch (clk_id) {
	case CMX655_SYSCLK_MIN ... CMX655_SYSCLK_MAX:
		break;
	default:
		return -EINVAL;
	}
	cmx655_dai_data->sys_clk = clk_id;

	return 0;
}

static int cmx655_dai_prepare(struct snd_pcm_substream *stream,
			      struct snd_soc_dai *dai)
{
	int ret = 0;
	struct snd_soc_component *component = dai->component;
	struct cmx655_data *cmx655_data =
	    snd_soc_component_get_drvdata(component);
	struct cmx655_dai_data *cmx655_dai_data = &cmx655_data->dai_data;

	if (!cmx655_dai_data->best_clk_running) {
		/* Stop the clock change over to the correct one an start it again */
		ret = cmx655_stop_sys_clk(component);
		if (ret < 0) {
			dev_err(component->dev, "Failed to stop clock %d\n",
				ret);
			goto get_out;
		}
		ret =
		    snd_soc_component_update_bits(component, CMX655_CLKCTRL,
						  CMX655_CLKCTRL_CLRSRC_MASK,
						  cmx655_dai_data->clk_src);
		if (ret < 0) {
			dev_err(component->dev,
				"Failed to set new clock setup %d\n", ret);
			goto get_out;
		}
		ret = cmx655_start_sys_clk(component);
		if (ret < 0) {
			dev_warn(component->dev, "Failed to restart clock\n");
			ret = 0;
			/*
			 * This will happen if the CPU driver does not start the LRCLK
			 * until the last point.
			 * For now we will assume the clock will start
			 */
		}
		cmx655_dai_data->best_clk_running = true;
	}
get_out:
	return ret;
};

static int cmx655_hw_params(struct snd_pcm_substream *stream,
			    struct snd_pcm_hw_params *hw_params,
			    struct snd_soc_dai *dai)
{
	int ret;
	struct snd_soc_component *component = dai->component;
	struct cmx655_data *cmx655_data =
	    snd_soc_component_get_drvdata(component);
	struct cmx655_dai_data *cmx655_dai_data = &cmx655_data->dai_data;
	unsigned int enabled_streams = cmx655_dai_data->enabled_streams;

	/* Setup clock and sample rate */
	ret = cmx655_setup_rate(component, hw_params);
	if (ret < 0) {
		dev_err(component->dev, "Failed to set rates %d\n",
			ret);
		return ret;
	}

	/* Set mono bit based on channel count */
	if (params_channels(hw_params) == 1) {
		dev_dbg(component->dev, "Switching into mono mode\n");
		snd_soc_component_update_bits(component, CMX655_SAICTRL,
					      CMX655_SAI_MONO, CMX655_SAI_MONO);
	} else {
		snd_soc_component_update_bits(component, CMX655_SAICTRL,
					      CMX655_SAI_MONO, 0);
	}

	if (cmx655_data->irq)
		cmx655_data->oc_cnt = 0;
	if (enabled_streams == 0) {
		dev_dbg(component->dev,
			"First stream to enable, enabling SAI\n");
		/*
		 * If first stream to be enabled
		 * Enable SAI (serial audio interface) port
		 * We need it running before the platform starts.
		 * to avoid I2S sync errors
		 */
		snd_soc_component_update_bits(component, CMX655_SYSCTRL,
					      CMX655_SYSCTRL_SAI,
					      CMX655_SYSCTRL_SAI);
	} else {
		dev_dbg(component->dev,
			"Not first stream to enable, skipping SAI enable\n");
	}

	/* Inc enabled streams by 1 */
	cmx655_dai_data->enabled_streams = enabled_streams + 1;

	return ret;
}

static void cmx655_dai_shutdown(struct snd_pcm_substream *stream,
				struct snd_soc_dai *dai)
{
	struct snd_soc_component *component = dai->component;
	struct cmx655_data *cmx655_data =
	    snd_soc_component_get_drvdata(component);
	struct cmx655_dai_data *cmx655_dai_data = &cmx655_data->dai_data;
	unsigned int enabled_streams = cmx655_dai_data->enabled_streams;

	if (enabled_streams == 0) {
		dev_dbg(component->dev,
			"Shutdown called when SAI not running\n");
		return;
	}
	enabled_streams = enabled_streams - 1;
	cmx655_dai_data->enabled_streams = enabled_streams;
	if (enabled_streams == 0) {
		dev_dbg(component->dev,
			"Last stream to disable, disabling SAI\n");
		snd_soc_component_update_bits(component, CMX655_SYSCTRL,
					      CMX655_SYSCTRL_SAI, 0);
		cmx655_dai_data->best_clk_running = false;
	} else {
		dev_dbg(component->dev,
			"Not last stream to disable, skipping SAI disable\n");
	}
}

static const struct snd_soc_dai_ops cmx655_dai_ops = {
	.set_sysclk = cmx655_set_dai_sysclk,
	.set_fmt = cmx655_set_dai_fmt,

	.prepare = cmx655_dai_prepare,
	.hw_params = cmx655_hw_params,
	.shutdown = cmx655_dai_shutdown,
};

static struct snd_soc_dai_driver cmx655_dai_driver = {
	.name = "cmx655",
	.playback = {
		     .stream_name = "CMX655 Playback",
		     .channels_min = 1,
		     .channels_max = 2,
		     .rates = CMX655_RATES,
		     .formats = CMX655_FMTS,
		      },
	.capture = {
		    .stream_name = "CMX655 Record",
		    .channels_min = 1,
		    .channels_max = 2,
		    .rates = CMX655_RATES,
		    .formats = CMX655_FMTS,
		     },
	.ops = &cmx655_dai_ops,
	.symmetric_rate = 1
};

static irqreturn_t cmx655_irq_thread(int irq, void *data)
{
	struct snd_soc_component *component = data;
	struct cmx655_data *cmx655_data =
	    snd_soc_component_get_drvdata(component);
	unsigned int status;

	status = snd_soc_component_read(component, CMX655_ISR);
	if (status == 0)
		return IRQ_NONE;
	/* Thermal protection event */
	if (status & CMX655_ISR_THERM) {
		dev_err(component->dev,
			"CMX655 class-D over temperature detected\n");
	}
	/* Over current  event */
	if (status & CMX655_ISR_AMPOC) {
		dev_warn(component->dev,
			 "CMX655 class-D over current detected\n");
		if (cmx655_data->oc_cnt < cmx655_data->oc_cnt_max) {
			/* Re enable class-D */
			snd_soc_component_update_bits(component,
						      CMX655_SYSCTRL,
						      CMX655_SYSCTRL_PAMP,
						      CMX655_SYSCTRL_PAMP);
			if (cmx655_data->oc_cnt_max <= 10000)
				cmx655_data->oc_cnt = cmx655_data->oc_cnt + 1;
		} else {
			/* Re enable count reached, do not try again */
			dev_err(component->dev,
				"Class-D over current restart attempts exceeded\n");
		}
	}
	return IRQ_HANDLED;
}

static int cmx655_component_probe(struct snd_soc_component *component)
{
	int ret;
	struct cmx655_data *cmx655_data =
	    snd_soc_component_get_drvdata(component);

	ret = cmx655_start_sys_clk(component);
	if (ret < 0) {
		dev_err(component->dev, "Failed to start system clock %d\n",
			ret);
		goto codec_err;
	}
	cmx655_data->oc_cnt = 0;
	if (cmx655_data->irq) {
		ret =
		    request_threaded_irq(cmx655_data->irq, NULL,
					 cmx655_irq_thread, IRQF_ONESHOT,
					 "cmx655", component);
		if (ret < 0) {
			dev_err(component->dev,
				"Failed to setup interrupt %d\n", ret);
			goto interrupt_err;
		}
		snd_soc_component_write(component, CMX655_ISM,
					(CMX655_ISM_AMPOC | CMX655_ISM_THERM));
	}

	return 0;

interrupt_err:
codec_err:
	return ret;
}

static void cmx655_component_remove(struct snd_soc_component *component)
{
	struct cmx655_data *cmx655_data =
	    snd_soc_component_get_drvdata(component);

	if (cmx655_data->irq) {
		snd_soc_component_write(component, CMX655_ISM, 0);
		free_irq(cmx655_data->irq, component);
	}
}

static const DECLARE_TLV_DB_SCALE(cmx655_level, -1200, 100, 0);
static const DECLARE_TLV_DB_SCALE(cmx655_ng_thresh, -6300, 100, 0);
static const DECLARE_TLV_DB_SCALE(cmx655_vol, -9100, 100, 1);
static const DECLARE_TLV_DB_SCALE(cmx655_pre_amp, 0, 600, 0);
static const DECLARE_TLV_DB_SCALE(cmx655_dst_gain, -6200, 200, 0);
static const DECLARE_TLV_DB_SCALE(cmx655_alc_gain, 0, 100, 0);
static const DECLARE_TLV_DB_SCALE(cmx655_alc_thresh, -3100, 100, 0);

static const char *const cmx655_ngratio_text[] = {
	"1:2",
	"1:3",
	"1:4"
};

static SOC_ENUM_SINGLE_DECL(cmx655_ngratio_enum, CMX655_NGCTRL, 5,
			    cmx655_ngratio_text);
static const char *const cmx655_ngattack_text[] = {
	"1.5ms",
	"3ms",
	"4.5ms",
	"6ms",
	"12ms",
	"24ms",
	"48ms",
	"96ms"
};

static SOC_ENUM_SINGLE_DECL(cmx655_ngattack_enum, CMX655_NGTIME, 4,
			    cmx655_ngattack_text);
static const char *const cmx655_ngrelease_text[] = {
	"0.06s",
	"0.12s",
	"0.24s",
	"0.48s",
	"0.96s",
	"1.92s",
	"3.84s",
	"7.68s"
};

static SOC_ENUM_SINGLE_DECL(cmx655_ngrelease_enum, CMX655_NGTIME, 0,
			    cmx655_ngrelease_text);
static const char *const cmx655_hpf_text[] = {
	"Disabled",
	"SRate/320",
	"SRate/53.3",
	"SRate/26.7"
};

static SOC_ENUM_SINGLE_DECL(cmx655_hpf_capture_enum, CMX655_RVF, 0,
			    cmx655_hpf_text);
static SOC_ENUM_SINGLE_DECL(cmx655_hpf_playback_enum, CMX655_PVF, 0,
			    cmx655_hpf_text);
static const char *const cmx655_companding_text[] = {
	"u-Law",
	"a-Law"
};

static SOC_ENUM_SINGLE_DECL(cmx655_companding, CMX655_SAIMUX, 5,
			    cmx655_companding_text);
static const char *const cmx655_alc_ratio_text[] = {
	"1.5:1",
	"2:1",
	"4:1",
	"Inf:1"
};

static SOC_ENUM_SINGLE_DECL(cmx655_alc_ratio_enum, CMX655_ALCCTRL, 5,
			    cmx655_alc_ratio_text);
/* Note: The attack and release times are the same as the Noise gate's. */
static SOC_ENUM_SINGLE_DECL(cmx655_alc_attack_enum, CMX655_ALCTIME, 4,
			    cmx655_ngattack_text);
static SOC_ENUM_SINGLE_DECL(cmx655_alc_release_enum, CMX655_ALCTIME, 0,
			    cmx655_ngrelease_text);

static const struct snd_kcontrol_new cmx655_snd_controls[] = {
	/* Capture */
	SOC_DOUBLE_TLV("Master Capture Volume", CMX655_LEVEL, 4, 0, 15, 0,
		       cmx655_level),

	SOC_SINGLE("Digital Capture Block Switch", CMX655_RVF,
		   CMX655_VF_DCBLOCK_SHIFT,
		   1, 0),

	SOC_SINGLE("LPF Capture Switch", CMX655_RVF, 3, 1, 0),
	SOC_ENUM("HPF Capture Switch", cmx655_hpf_capture_enum),
	/* Noise gate */
	SOC_SINGLE("Noise Gate Capture Switch", CMX655_NGCTRL, 7, 1, 0),
	SOC_SINGLE_TLV("Noise Gate Threshold Capture Volume", CMX655_NGCTRL, 0, 31, 0,
		       cmx655_ng_thresh),
	SOC_ENUM("Noise Gate Ratio Capture Switch", cmx655_ngratio_enum),
	SOC_ENUM("Noise Gate Attack Capture Switch", cmx655_ngattack_enum),
	SOC_ENUM("Noise Gate Release Capture Switch", cmx655_ngrelease_enum),

	/* Playback */
	SOC_SINGLE_TLV("Master Playback Volume", CMX655_VOLUME, 0, 91, 0,
		       cmx655_vol),
	SOC_SINGLE_TLV("Preamp Playback Volume", CMX655_PREAMP, 0, 3, 0,
		       cmx655_pre_amp),
	SOC_SINGLE("Smooth Playback Switch", CMX655_VOLUME, 7, 0x01, 0),
	SOC_SINGLE("Dogital Capture Block Playback Switch", CMX655_PVF,
		   CMX655_VF_DCBLOCK_SHIFT,
		   1, 0),
	SOC_SINGLE("LPF Playback Switch", CMX655_PVF, 3, 1, 0),
	SOC_ENUM("HPF Playback Switch", cmx655_hpf_playback_enum),
	SOC_SINGLE("Soft Mute Playback Switch", CMX655_CPR, 0, 1, 0),
	SOC_SINGLE("ALC Playback Switch", CMX655_ALCCTRL, 7, 1, 0),
	SOC_ENUM("ALC Ratio Playback Switch", cmx655_alc_ratio_enum),
	SOC_SINGLE_TLV("ALC Threshold Playback Volume", CMX655_ALCCTRL,
		       0, 31, 0,
		       cmx655_alc_thresh),
	SOC_SINGLE_TLV("ALC Playback Volume", CMX655_ALCGAIN, 0, 12, 0,
		       cmx655_alc_gain),
	SOC_ENUM("ALC Attack Playback Switch", cmx655_alc_attack_enum),
	SOC_ENUM("ALC Release Playback Switch", cmx655_alc_release_enum),

	/* Digital Sidetone */
	SOC_SINGLE_TLV("Sidetone Playback Volume", CMX655_DST, 0, 31, 0,
		       cmx655_dst_gain),
	/* Companding */
	SOC_SINGLE("Companding Switch", CMX655_SAIMUX, 4, 1, 0),
	SOC_ENUM("Companding Type", cmx655_companding),
};

static const char *const cmx655_mic_mux_text[] = {
	"Normal",
	"Swapped",
	"Left only",
	"Right only"
};

static SOC_ENUM_SINGLE_DECL(cmx655_mic_mux_enum, CMX655_SAIMUX, 0,
			    cmx655_mic_mux_text);

static const char *const cmx655_amp_mux_text[] = {
	"Left",
	"Right",
	"Mean"
};

static SOC_ENUM_SINGLE_DECL(cmx655_amp_mux_enum, CMX655_SAIMUX, 2,
			    cmx655_amp_mux_text);

static const char *const cmx655_digital_sidetone_text[] = {
	"Left",
	"Right",
	"Mean"
};

static SOC_ENUM_SINGLE_DECL(cmx655_sidetone_enum, CMX655_DST, 5,
			    cmx655_digital_sidetone_text);
static const struct snd_kcontrol_new cmx655_mic_mux =
SOC_DAPM_ENUM("SAI Capture Route",
	      cmx655_mic_mux_enum);

static const struct snd_kcontrol_new cmx655_amp_mux =
SOC_DAPM_ENUM("Play_SAI Playback Route",
	      cmx655_amp_mux_enum);

static const struct snd_kcontrol_new cmx655_spkr_en[] = {
	SOC_DAPM_SINGLE_VIRT("Playback Switch", 1)
};

static const struct snd_kcontrol_new cmx655_lout_en[] = {
	SOC_DAPM_SINGLE_VIRT("Playback Switch", 1)
};

static const struct snd_kcontrol_new cmx655_sidetone_mux =
SOC_DAPM_ENUM("DST Route", cmx655_sidetone_enum);

static const struct snd_kcontrol_new cmx655_dst_en[] = {
	SOC_DAPM_SINGLE_VIRT("Playback Switch", 1)
};

static int cmx655_mic_dapm_event(struct snd_soc_dapm_widget *widget,
				 struct snd_kcontrol *control, int event)
{
	struct snd_soc_component *component =
	    snd_soc_dapm_to_component(widget->dapm);
	int reg_val;

	switch (event) {
	case SND_SOC_DAPM_POST_PMU:
		/*
		 *After turn on give MIC filters time
		 * Time can be shorter if the DC blocking filter is not enabled
		 */
		reg_val = snd_soc_component_read(component, CMX655_RVF);
		if ((reg_val && CMX655_VF_DCBLOCK) > 0) {
			/* This allows time for Mics and DC blocking filter to settle */
			msleep(320);
		} else {
			usleep_range(3500, 4000);
		}
		break;
	default:
		break;
	}
	return 0;
}

static const struct snd_soc_dapm_widget cmx655_dapm_widgets[] = {
	/* Input path widgets */
	SND_SOC_DAPM_INPUT("MICL"),
	SND_SOC_DAPM_INPUT("MICR"),

	/* Custom widgets for Mics to get them to turn on before switches */
	CMX655_DAPM_MIC("Left Mic", 1),
	CMX655_DAPM_MIC("Right Mic", 0),

	SND_SOC_DAPM_MUX("SAI Left Capture Mux", SND_SOC_NOPM, 0, 0,
			 &cmx655_mic_mux),
	SND_SOC_DAPM_MUX("SAI Right Capture Mux", SND_SOC_NOPM, 0, 0,
			 &cmx655_mic_mux),
	/*
	 * Note: SAI enable is controlled by DAI's HWparams and shutdown. Using
	 * DAPM control resulted in I2S sync errors on the platform driver
	 */
	SND_SOC_DAPM_AIF_OUT("SAI Left Out", "CMX655 Record", 0,
			     SND_SOC_NOPM, 0, 0),
	SND_SOC_DAPM_AIF_OUT("SAI Right Out", "CMX655 Record", 1,
			     SND_SOC_NOPM, 0, 0),

	/* Output path widgets */
	SND_SOC_DAPM_AIF_IN("SAI Left In", "CMX655 Playback", 0,
			    SND_SOC_NOPM, 0, 0),
	SND_SOC_DAPM_AIF_IN("SAI Right In", "CMX655 Playback", 1,
			    SND_SOC_NOPM, 0, 0),

	SND_SOC_DAPM_MUX("SAI Playback Route", SND_SOC_NOPM, 0, 0,
			 &cmx655_amp_mux),

	SND_SOC_DAPM_DAC("Power Amp", "CMX655 Playback", CMX655_SYSCTRL, 3, 0),
	SND_SOC_DAPM_DAC("Line Out", "CMX655 Playback", CMX655_SYSCTRL, 4, 0),

	SND_SOC_DAPM_SWITCH("SPKR Switch", SND_SOC_NOPM, 0, 0, cmx655_spkr_en),
	SND_SOC_DAPM_SWITCH("LOUT Switch", SND_SOC_NOPM, 0, 0, cmx655_lout_en),

	SND_SOC_DAPM_OUTPUT("SPKR"),
	SND_SOC_DAPM_OUTPUT("LOUT"),

	/* Digital side tone widgets */
	SND_SOC_DAPM_SWITCH("DST Switch", SND_SOC_NOPM, 0, 0, cmx655_dst_en),
	SND_SOC_DAPM_MUX("DST Mux", CMX655_DST, 7, 0, &cmx655_sidetone_mux),

};

static const struct snd_soc_dapm_route cmx655_dapm_routes[] = {
	/* Main output path */
	{ "SPKR", NULL, "SPKR Switch" },
	{ "LOUT", NULL, "LOUT Switch" },
	{ "SPKR Switch", "Playback Switch", "Power Amp" },
	{ "LOUT Switch", "Playback Switch", "Line Out" },
	{ "Power Amp", NULL, "SAI Playback Route" },
	{ "Line Out", NULL, "SAI Playback Route" },
	{ "SAI Playback Route", "Left", "SAI Left In" },
	{ "SAI Playback Route", "Right", "SAI Right In" },
	{ "SAI Playback Route", "Mean", "SAI Left In" },
	{ "SAI Playback Route", "Mean", "SAI Right In" },
	/* Main input path */
	{ "SAI Right Out", NULL, "SAI Right Capture Mux" },
	{ "SAI Left Out", NULL, "SAI Left Capture Mux" },
	{ "SAI Left Capture Mux", "Normal", "Left Mic" },
	{ "SAI Right Capture Mux", "Normal", "Right Mic" },
	{ "SAI Left Capture Mux", "Swapped", "Right Mic" },
	{ "SAI Right Capture Mux", "Swapped", "Left Mic" },
	{ "SAI Left Capture Mux", "Left only", "Left Mic" },
	{ "SAI Right Capture Mux", "Left only", "Left Mic" },
	{ "SAI Left Capture Mux", "Right only", "Right Mic" },
	{ "SAI Right Capture Mux", "Right only", "Right Mic" },
	{ "Right Mic", NULL, "MICR" },
	{ "Left Mic", NULL, "MICL" },
	/* Digital side tone */
	{ "DST Mux", "Left", "Left Mic" },
	{ "DST Mux", "Right", "Right Mic" },
	{ "DST Mux", "Mean", "Left Mic" },
	{ "DST Mux", "Mean", "Right Mic" },

	{ "DST Switch", "Playback Switch", "DST Mux" },

	{ "Power Amp", NULL, "DST Switch" },
	{ "Line Out", NULL, "DST Switch" },

};

static const struct snd_soc_component_driver cmx655_component_driver = {
	.controls = cmx655_snd_controls,
	.num_controls = ARRAY_SIZE(cmx655_snd_controls),
	.dapm_widgets = cmx655_dapm_widgets,
	.num_dapm_widgets = ARRAY_SIZE(cmx655_dapm_widgets),
	.dapm_routes = cmx655_dapm_routes,
	.num_dapm_routes = ARRAY_SIZE(cmx655_dapm_routes),

	.probe = cmx655_component_probe,
	.remove = cmx655_component_remove
};

static int cmx655_parse_data_from_of(const struct device_node *device_node,
				     struct cmx655_data *cmx655_data)
{
	int ret;
	unsigned int val;

	ret =
	    of_property_read_u32(device_node,
				 "cml,classd-oc-reset-attempts", &val);
	if (ret >= 0)
		cmx655_data->oc_cnt_max = val;
	else
		cmx655_data->oc_cnt_max = 5;

	return 0;
}

/**
 * cmx655_common_register_component() - registers cmx655 codec.
 * @dev: bus-specific device descriptor
 * @regmap: bus-specific register mapping
 * @irq: interrupt id for the device
 *
 * Return: 0 for success, negative number otherwise
 */
int cmx655_common_register_component(struct device *dev, struct regmap *regmap,
				     int irq)
{
	int ret;
	struct cmx655_data *cmx655_data;
	struct cmx655_dai_data *cmx655_dai_data = dev_get_platdata(dev);

	cmx655_data = devm_kzalloc(dev, sizeof(*cmx655_data), GFP_KERNEL);
	if (!cmx655_data)
		return -ENOMEM;
	cmx655_data->regmap = regmap;
	if (IS_ERR(cmx655_data->regmap))
		return PTR_ERR(cmx655_data->regmap);
	cmx655_data->irq = irq;
	if (cmx655_dai_data) {
		memcpy(&cmx655_data->dai_data, cmx655_dai_data,
		       sizeof(*cmx655_dai_data));
	}
	if (dev->of_node) {
		ret = cmx655_parse_data_from_of(dev->of_node, cmx655_data);
		if (ret < 0) {
			dev_err(dev,
				"Failed to extract data from device tree%d\n",
				ret);
			return ret;
		}
	}
	cmx655_data->reset_gpio = devm_gpiod_get_optional(dev,
							  "reset",
							  GPIOD_OUT_LOW);
	if (cmx655_data->reset_gpio) {
		gpiod_set_value_cansleep(cmx655_data->reset_gpio, 1);
		usleep_range(10, 1000);
		gpiod_set_value_cansleep(cmx655_data->reset_gpio, 0);
	} else {
		dev_dbg(dev, "No reset GPIO, using reset command\n");
		regmap_write(cmx655_data->regmap, CMX655_COMMAND,
			     CMX655_CMD_SOFT_RESET);
	}
	dev_set_drvdata(dev, cmx655_data);

	return devm_snd_soc_register_component(dev,
					       &cmx655_component_driver,
					       &cmx655_dai_driver, 1);
}
EXPORT_SYMBOL_GPL(cmx655_common_register_component);

void cmx655_common_unregister_component(struct device *dev)
{
	struct cmx655_data *cmx655_data = dev_get_drvdata(dev);

	snd_soc_unregister_component(dev);
	gpiod_set_value_cansleep(cmx655_data->reset_gpio, 1);
}
EXPORT_SYMBOL_GPL(cmx655_common_unregister_component);

MODULE_DESCRIPTION("ASoC CMX655 driver");
MODULE_AUTHOR("CML");
MODULE_LICENSE("GPL");

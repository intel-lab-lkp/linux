// SPDX-License-Identifier: GPL-2.0
/*
 * CS40L26 Advanced Haptic Driver with waveform memory,
 * integrated DSP, and closed-loop algorithms
 *
 * Copyright 2025 Cirrus Logic, Inc.
 *
 * Author: Fred Treven <ftreven@opensource.cirrus.com>
 */

#include <linux/cleanup.h>
#include <linux/mfd/core.h>
#include <linux/mfd/cs40l26.h>
#include <linux/property.h>
#include <linux/regulator/consumer.h>

static const struct mfd_cell cs40l26_devs[] = {
	{ .name = "cs40l26-codec", },
	{ .name = "cs40l26-vibra", },
};

const struct regmap_config cs40l26_regmap = {
	.reg_bits = 32,
	.val_bits = 32,
	.reg_stride = 4,
	.reg_format_endian = REGMAP_ENDIAN_BIG,
	.val_format_endian = REGMAP_ENDIAN_BIG,
	.max_register = CS40L26_LASTREG,
	.cache_type = REGCACHE_NONE,
};
EXPORT_SYMBOL_GPL(cs40l26_regmap);

static const char *const cs40l26_supplies[] = {
	"va", "vp",
};

inline void cs40l26_pm_exit(struct device *dev)
{
	pm_runtime_mark_last_busy(dev);
	pm_runtime_put_autosuspend(dev);
}
EXPORT_SYMBOL_GPL(cs40l26_pm_exit);

static int cs40l26_fw_write_raw(struct cs_dsp *dsp, const char *const name,
				const unsigned int algo_id, const u32 offset_words,
				const size_t len_words, u32 *buf)
{
	struct cs_dsp_coeff_ctl *ctl;
	__be32 *val;
	int i, ret;

	ctl = cs_dsp_get_ctl(dsp, name, WMFW_ADSP2_XM, algo_id);
	if (!ctl) {
		dev_err(dsp->dev, "Failed to find FW control %s\n", name);
		return -EINVAL;
	}

	val = kzalloc(len_words * sizeof(u32), GFP_KERNEL);
	if (!val)
		return -ENOMEM;

	for (i = 0; i < len_words; i++)
		val[i] = cpu_to_be32(buf[i]);

	ret = cs_dsp_coeff_write_ctrl(ctl, offset_words, val, len_words * sizeof(u32));
	if (ret < 0)
		dev_err(dsp->dev, "Failed to write FW control %s\n", name);

	kfree(val);

	return (ret < 0) ? ret : 0;
}

inline int cs40l26_fw_write(struct cs_dsp *dsp, const char *const name, const unsigned int algo_id,
			    u32 val)
{
	return cs40l26_fw_write_raw(dsp, name, algo_id, 0, 1, &val);
}
EXPORT_SYMBOL_GPL(cs40l26_fw_write);

static int cs40l26_fw_read_raw(struct cs_dsp *dsp, const char *const name,
			       const unsigned int algo_id, const unsigned int offset_words,
			       const size_t len_words, u32 *buf)
{
	struct cs_dsp_coeff_ctl *ctl;
	int i, ret;

	ctl = cs_dsp_get_ctl(dsp, name, WMFW_ADSP2_XM, algo_id);
	if (!ctl) {
		dev_err(dsp->dev, "Failed to find FW control %s\n", name);
		return -EINVAL;
	}

	ret = cs_dsp_coeff_read_ctrl(ctl, offset_words, buf, len_words * sizeof(u32));
	if (ret) {
		dev_err(dsp->dev, "Failed to read FW control %s\n", name);
		return ret;
	}

	for (i = 0; i < len_words; i++)
		buf[i] = be32_to_cpu(buf[i]);

	return 0;
}

inline int cs40l26_fw_read(struct cs_dsp *dsp, const char *const name, const unsigned int algo_id,
			   u32 *buf)
{
	return cs40l26_fw_read_raw(dsp, name, algo_id, 0, 1, buf);
}
EXPORT_SYMBOL_GPL(cs40l26_fw_read);

static struct cs40l26_irq *cs40l26_get_irq(struct cs40l26 *cs40l26, const int num, const int bit);

static int cs40l26_gpio1_rise_irq(void *data)
{
	struct cs40l26 *cs40l26 = data;

	if (cs40l26->wksrc_sts & CS40L26_WKSRC_STS_EN)
		dev_dbg(cs40l26->dev, "GPIO1 Rising Edge Detected\n");

	cs40l26->wksrc_sts |= CS40L26_WKSRC_STS_EN;

	return 0;
}

static int cs40l26_gpio1_fall_irq(void *data)
{
	struct cs40l26 *cs40l26 = data;

	if (cs40l26->wksrc_sts & CS40L26_WKSRC_STS_EN)
		dev_dbg(cs40l26->dev, "GPIO1 Falling Edge Detected\n");

	cs40l26->wksrc_sts |= CS40L26_WKSRC_STS_EN;

	return 0;
}

static int cs40l26_wksrc_any_irq(void *data)
{
	struct cs40l26 *cs40l26 = data;
	u32 last_wksrc, pwrmgt_sts;
	int ret;

	guard(mutex)(&cs40l26->dsp.pwr_lock);

	ret = regmap_read(cs40l26->regmap, CS40L26_PWRMGT_STS, &pwrmgt_sts);
	if (ret)
		return ret;

	cs40l26->wksrc_sts = (u8)FIELD_GET(CS40L26_WKSRC_STS_MASK, pwrmgt_sts);

	ret = cs40l26_fw_read(&cs40l26->dsp, "LAST_WAKESRC_CTL", cs40l26->dsp.fw_id, &last_wksrc);
	if (ret)
		return ret;

	cs40l26->last_wksrc_pol = (u8)(last_wksrc & CS40L26_WKSRC_GPIO_POL_MASK);

	return 0;
}

static int cs40l26_wksrc_gpio1_irq(void *data)
{
	struct cs40l26 *cs40l26 = data;

	/*
	 * The GPIO wakesource and event interrupts are not able to reliably determine
	 * the GPIO edge that triggered the interrupt (rising/falling).
	 *
	 * The driver must therefore perform this logic in order to determine the edge
	 * of the GPIO event for two cases:
	 * 1. The GPIO event is waking the device from hibernation.
	 * 2. The GPIO event occurs when the device is already awake.
	 */

	if (cs40l26->wksrc_sts & cs40l26->last_wksrc_pol) {
		dev_dbg(cs40l26->dev, "GPIO1 Falling Edge Detected\n");
		cs40l26->wksrc_sts |= CS40L26_WKSRC_STS_EN;
	} else {
		dev_dbg(cs40l26->dev, "GPIO1 rising edge detected\n");
	}

	return 0;
}

static int cs40l26_error_release(struct cs40l26 *cs40l26, const enum cs40l26_error err)
{
	int ret;

	dev_err(cs40l26->dev, "Device Reported Error: %u\n", (unsigned int)BIT(err));

	ret = regmap_clear_bits(cs40l26->regmap, CS40L26_ERROR_RELEASE, BIT(err));
	if (ret)
		return ret;

	ret = regmap_set_bits(cs40l26->regmap, CS40L26_ERROR_RELEASE, BIT(err));
	if (ret)
		return ret;

	return regmap_clear_bits(cs40l26->regmap, CS40L26_ERROR_RELEASE, BIT(err));
}

static int cs40l26_bst_ovp_err_irq(void *data)
{
	struct cs40l26 *cs40l26 = data;

	return cs40l26_error_release(cs40l26, CS40L26_ERROR_BST_OVP);
}

static int cs40l26_bst_dcm_uvp_err_irq(void *data)
{
	struct cs40l26 *cs40l26 = data;

	return cs40l26_error_release(cs40l26, CS40L26_ERROR_BST_DCM_UVP);
}

static int cs40l26_bst_short_err_irq(void *data)
{
	struct cs40l26 *cs40l26 = data;

	return cs40l26_error_release(cs40l26, CS40L26_ERROR_BST_SHORT);
}

static int cs40l26_temp_warn_rise_irq(void *data)
{
	struct cs40l26 *cs40l26 = data;

	return cs40l26_error_release(cs40l26, CS40L26_ERROR_TEMP_WARN);
}

static int cs40l26_temp_err_irq(void *data)
{
	struct cs40l26 *cs40l26 = data;

	return cs40l26_error_release(cs40l26, CS40L26_ERROR_TEMP_ERR);
}

static int cs40l26_amp_short_err_irq(void *data)
{
	struct cs40l26 *cs40l26 = data;

	return cs40l26_error_release(cs40l26, CS40L26_ERROR_AMP_SHORT);
}

static int cs40l26_dsp_queue_buffer_read(struct cs40l26 *cs40l26, u32 *val)
{
	u32 queue_rd, queue_wt, sts;
	int ret;

	guard(mutex)(&cs40l26->dsp.pwr_lock);

	ret = cs40l26_fw_read(&cs40l26->dsp, "QUEUE_WT", CS40L26_DSP_ALGO_ID, &queue_wt);
	if (ret)
		return ret;

	ret = cs40l26_fw_read(&cs40l26->dsp, "QUEUE_RD", CS40L26_DSP_ALGO_ID, &queue_rd);
	if (ret)
		return ret;

	if (queue_rd - sizeof(u32) == queue_wt) {
		ret = cs40l26_fw_read(&cs40l26->dsp, "STATUS", CS40L26_DSP_ALGO_ID, &sts);
		if (ret)
			return ret;

		if (sts) {
			dev_err(cs40l26->dev, "DSP Queue Buffer is full, message(s) missed\n");
			return -ENOSPC;
		}
	}

	if (queue_rd == queue_wt) /* DSP Queue is Empty */
		return 1;

	ret = regmap_read(cs40l26->regmap, queue_rd, val);
	if (ret)
		return ret;

	if (queue_rd == cs40l26->queue_last)
		queue_rd = cs40l26->queue_base;
	else
		queue_rd += sizeof(u32);

	return cs40l26_fw_write(&cs40l26->dsp, "QUEUE_RD", CS40L26_DSP_ALGO_ID, queue_rd);
}

static int cs40l26_dsp_queue_irq(void *data)
{
	struct cs40l26 *cs40l26 = data;
	bool end = false;
	int ret;
	u32 val;

	ret = cs40l26_dsp_queue_buffer_read(cs40l26, &val);
	if (ret == 1)
		return 0;
	else if (ret)
		return ret;

	while (!end) {
		if ((val & CS40L26_DSP_CMD_INDEX_MASK) == CS40L26_DSP_PANIC) {
			dev_err(cs40l26->dev, "DSP Panic! Error: 0x%06X\n",
				(u32)(val & CS40L26_DSP_CMD_PAYLOAD_MASK));
			return -ENOTRECOVERABLE;
		}

		switch (val) {
		case CS40L26_DSP_COMPLETE_CP:
			dev_dbg(cs40l26->dev, "DSP Queue: Control Port Haptics Completed\n");
			break;
		case CS40L26_DSP_COMPLETE_I2S:
			dev_dbg(cs40l26->dev, "DSP Queue: I2S Stream Completed\n");
			break;
		case CS40L26_DSP_TRIGGER_CP:
			dev_dbg(cs40l26->dev, "DSP Queue: Control Port Haptics Triggered\n");
			break;
		case CS40L26_DSP_TRIGGER_I2S:
			dev_dbg(cs40l26->dev, "DSP Queue: I2S Stream Triggered\n");
			break;
		case CS40L26_DSP_PM_AWAKE:
			cs40l26->wksrc_sts |= CS40L26_WKSRC_STS_EN;
			dev_dbg(cs40l26->dev, "DSP Queue: AWAKE\n");
			break;
		case CS40L26_DSP_SYS_ACK:
			dev_dbg(cs40l26->dev, "DSP Queue: Inbound PING received\n");
			break;
		default:
			dev_err(cs40l26->dev, "DSP Queue value (0x%X) unrecognized\n", val);
			return -EINVAL;
		}

		ret = cs40l26_dsp_queue_buffer_read(cs40l26, &val);
		if (ret == 1)
			end = true;
		else if (ret)
			return ret;
	}

	return 0;
}

static struct reg_sequence cs40l26_irq_masks[] = {
	REG_SEQ0(CS40L26_IRQ1_MASK_1, CS40L26_IRQ_1_ALL_MASKED),
	REG_SEQ0(CS40L26_IRQ1_MASK_2, CS40L26_IRQ_2_ALL_MASKED),
};

static void cs40l26_irq_unmask(struct cs40l26 *cs40l26, const int num, const int virq)
{
	struct cs40l26_irq *irq;

	if (num != 1 && num != 2) {
		dev_err(cs40l26->dev, "Invalid IRQ number %d\n", num);
		return;
	}

	irq = cs40l26_get_irq(cs40l26, num, virq);
	if (!irq)
		return;

	cs40l26->irq_masks[num - 1].def &= ~irq->mask;
}

static struct cs40l26_irq cs40l26_irqs_1[] = {
	CS40L26_IRQ(GPIO1_RISE, "GPIO1 Rise", cs40l26_gpio1_rise_irq),
	CS40L26_IRQ(GPIO1_FALL, "GPIO1 Fall", cs40l26_gpio1_fall_irq),
	CS40L26_IRQ(WKSRC_STS_ANY, "ANY Wake", cs40l26_wksrc_any_irq),
	CS40L26_IRQ(WKSRC_STS_GPIO1, "GPIO1 Wake", cs40l26_wksrc_gpio1_irq),
	CS40L26_IRQ(WKSRC_STS_SPI, "SPI Wake", NULL),
	CS40L26_IRQ(WKSRC_STS_I2C, "I2C Wake", NULL),
	CS40L26_IRQ(BST_OVP_FLAG_RISE, "BST OVP Rise", NULL),
	CS40L26_IRQ(BST_OVP_FLAG_FALL, "BST OVP Fall", NULL),
	CS40L26_IRQ(BST_OVP_ERR, "BST OVP Error", cs40l26_bst_ovp_err_irq),
	CS40L26_IRQ(BST_DCM_UVP_ERR, "BST UVP Error", cs40l26_bst_dcm_uvp_err_irq),
	CS40L26_IRQ(BST_SHORT_ERR, "BST Short Error", cs40l26_bst_short_err_irq),
	CS40L26_IRQ(BST_IPK_FLAG, "BST IPK Flag", NULL),
	CS40L26_IRQ(TEMP_WARN_RISE, "TEMP Warn Rise", cs40l26_temp_warn_rise_irq),
	CS40L26_IRQ(TEMP_WARN_FALL, "TEMP Warn Fall", NULL),
	CS40L26_IRQ(TEMP_ERR, "TEMP Error", cs40l26_temp_err_irq),
	CS40L26_IRQ(AMP_ERR, "AMP Error", cs40l26_amp_short_err_irq),
	CS40L26_IRQ(DSP_RX_QUEUE, "DSP Rx", cs40l26_dsp_queue_irq),
};

static struct cs40l26_irq cs40l26_irqs_2[] = {
	CS40L26_IRQ(REFCLK_PRESENT, "REFCLK Present", NULL),
	CS40L26_IRQ(REFCLK_MISSING_FALL, "REFCLK Missing Fall", NULL),
	CS40L26_IRQ(REFCLK_MISSING_RISE, "REFCLK Missing Rise", NULL),
	CS40L26_IRQ(VPMON_CLIPPED, "VPMON Clipped", NULL),
	CS40L26_IRQ(VBSTMON_CLIPPED, "VBSTMON Clipped", NULL),
	CS40L26_IRQ(VMON_CLIPPED, "VMON Clipped", NULL),
	CS40L26_IRQ(IMON_CLIPPED, "IMON Clipped", NULL),
};

static struct cs40l26_irq *cs40l26_get_irq(struct cs40l26 *cs40l26, const int num, const int bit)
{
	int i;

	if (num == 1) {
		for (i = 0; i < ARRAY_SIZE(cs40l26_irqs_1); i++) {
			if (cs40l26_irqs_1[i].virq == bit)
				return &cs40l26_irqs_1[i];
		}
	} else if (num == 2) {
		for (i = 0; i < ARRAY_SIZE(cs40l26_irqs_2); i++) {
			if (cs40l26_irqs_2[i].virq == bit)
				return &cs40l26_irqs_2[i];
		}
	} else {
		dev_err(cs40l26->dev, "Invalid IRQ number %d\n", num);
		return NULL;
	}

	dev_err(cs40l26->dev, "Failed to find IRQ corresponding to bit in IRQ%d %d\n", bit, num);

	return NULL;
}

static irqreturn_t cs40l26_irq_handler(int irq, void *data)
{
	struct cs40l26 *cs40l26 = data;
	struct cs40l26_irq *irq_s;
	unsigned long handle_bits;
	u32 eint, mask, sts;
	int i, j, ret;

	if (pm_runtime_resume_and_get(cs40l26->dev)) {
		dev_err(cs40l26->dev, "Failed to exit hibernate to service interrupt\n");
		return IRQ_NONE;
	}

	guard(mutex)(&cs40l26->lock);

	ret = regmap_read(cs40l26->regmap, CS40L26_IRQ1_STATUS, &sts);
	if (ret)
		goto err_pm;

	if (!(sts & CS40L26_IRQ_STATUS_ASSERT)) {
		dev_err(cs40l26->dev, "IRQ1 asserted with no pending interrupts\n");
		ret = -EIO;
		goto err_pm;
	}

	for (j = 0; j < 2; j++) {
		ret = regmap_read(cs40l26->regmap, CS40L26_IRQ1_MASK_1 + j * 4, &mask);
		if (ret)
			goto err_pm;

		ret = regmap_read(cs40l26->regmap, CS40L26_IRQ1_EINT_1 + j * 4, &eint);
		if (ret)
			goto err_pm;

		handle_bits = eint & ~mask;

		for_each_set_bit(i, &handle_bits, j ? CS40L26_IRQ_2_NBITS : CS40L26_IRQ_1_NBITS) {
			irq_s = cs40l26_get_irq(cs40l26, j + 1, i);
			if (!irq_s)
				continue;

			dev_dbg(cs40l26->dev, "%s", irq_s->name);

			if (irq_s->handler) {
				ret = irq_s->handler(cs40l26);
				if (ret)
					goto err_pm;
			}

			ret = regmap_write(cs40l26->regmap, CS40L26_IRQ1_EINT_1 + j * 4, BIT(i));
			if (ret)
				goto err_pm;
		}
	}

err_pm:
	cs40l26_pm_exit(cs40l26->dev);

	return IRQ_RETVAL(ret);
}

int cs40l26_dsp_write(struct cs40l26 *cs40l26, const u32 val)
{
	int i, ret;
	u32 ack;

	/* Device NAKs if hibernating, so retry if this is the case */
	for (i = 0; i < CS40L26_DSP_TIMEOUT_COUNT; i++) {
		ret = regmap_write(cs40l26->regmap, CS40L26_DSP_QUEUE, val);
		if (!ret)
			break;

		usleep_range(CS40L26_DSP_POLL_US, CS40L26_DSP_POLL_US + 100);
	}

	if (i == CS40L26_DSP_TIMEOUT_COUNT) {
		dev_err(cs40l26->dev, "Timed out writing %#X to DSP\n", val);
		return -ETIMEDOUT;
	}

	ret = regmap_read_poll_timeout(cs40l26->regmap, CS40L26_DSP_QUEUE, ack, !ack,
				       CS40L26_DSP_POLL_US,
				       CS40L26_DSP_POLL_US * CS40L26_DSP_TIMEOUT_COUNT);
	if (ret)
		dev_err(cs40l26->dev, "DSP failed to ACK %#X: %d\n", val, ret);

	return ret;
}
EXPORT_SYMBOL_GPL(cs40l26_dsp_write);

int cs40l26_dsp_state_get(struct cs40l26 *cs40l26, u32 *state)
{
	u32 dsp_state = CS40L26_DSP_STATE_NONE;
	int i, ret;

	if (cs40l26->dsp.running) {
		for (i = 0; i < CS40L26_DSP_TIMEOUT_COUNT; i++) {
			ret = cs40l26_fw_read(&cs40l26->dsp, "PM_CUR_STATE", CS40L26_PM_ALGO_ID,
					      &dsp_state);
			if (ret)
				return ret;

			if (dsp_state != CS40L26_DSP_STATE_NONE)
				break;

			usleep_range(CS40L26_DSP_POLL_US, CS40L26_DSP_POLL_US + 100);
		}

		if (i == CS40L26_DSP_TIMEOUT_COUNT) {
			dev_err(cs40l26->dev, "Timed out reading PM_CUR_STATE\n");
			return -ETIMEDOUT;
		}
	} else {
		ret = regmap_read_poll_timeout(cs40l26->regmap,
					       cs40l26->variant->info->pm_cur_state, dsp_state,
					       dsp_state != CS40L26_DSP_STATE_NONE,
					       CS40L26_DSP_POLL_US,
					       CS40L26_DSP_POLL_US * CS40L26_DSP_TIMEOUT_COUNT);
		if (ret) {
			dev_err(cs40l26->dev, "Failed to read poll for static PM_CUR_STATE\n");
			return ret;
		}
	}

	*state = dsp_state;

	return 0;
}
EXPORT_SYMBOL_GPL(cs40l26_dsp_state_get);

static bool cs40l26_dsp_can_run(struct cs40l26 *cs40l26)
{
	struct regmap *regmap = cs40l26->regmap;
	u32 dsp_state, pm_state_locks;
	int ret;

	ret = cs40l26_dsp_state_get(cs40l26, &dsp_state);
	if (ret)
		return false;

	if (dsp_state == CS40L26_DSP_STATE_ACTIVE)
		return true;

	if (dsp_state != CS40L26_DSP_STATE_STANDBY) {
		dev_err(cs40l26->dev, "DSP in bad state: %u\n", dsp_state);
		return false;
	}

	if (cs40l26->dsp.running)
		ret = cs40l26_fw_read_raw(&cs40l26->dsp, "PM_STATE_LOCKS", CS40L26_PM_ALGO_ID,
					  CS40L26_DSP_LOCK3_OFFSET_WORDS, 1, &pm_state_locks);
	else
		ret = regmap_read(regmap, cs40l26->variant->info->pm_state_locks3, &pm_state_locks);

	if (ret) {
		dev_err(cs40l26->dev, "Failed to read PM_STATE_LOCKS\n");
		return false;
	}

	return pm_state_locks & CS40L26_DSP_LOCK3_MASK;
}

static int cs40l26_prevent_hiber(struct cs40l26 *cs40l26)
{
	int i, ret;

	for (i = 0; i < CS40L26_DSP_TIMEOUT_COUNT; i++) {
		ret = cs40l26_dsp_write(cs40l26, CS40L26_DSP_CMD_PREVENT_HIBER);
		if (ret)
			return ret;

		usleep_range(CS40L26_DSP_POLL_US, CS40L26_DSP_POLL_US + 100);

		if (cs40l26_dsp_can_run(cs40l26))
			break;
	}

	if (i == CS40L26_DSP_TIMEOUT_COUNT) {
		dev_err(cs40l26->dev, "Failed to prevent hibernation\n");
		return -ETIMEDOUT;
	}

	return 0;
}

static int cs40l26_lbst_short_test(struct cs40l26 *cs40l26)
{
	u32 err, vbst_ctl_1, vbst_ctl_2;
	int ret;

	/* Read initial values to restore after test is complete */
	ret = regmap_read(cs40l26->regmap, CS40L26_VBST_CTL_2, &vbst_ctl_2);
	if (ret)
		return ret;

	ret = regmap_read(cs40l26->regmap, CS40L26_VBST_CTL_1, &vbst_ctl_1);
	if (ret)
		return ret;

	ret = regmap_update_bits(cs40l26->regmap, CS40L26_VBST_CTL_2, CS40L26_BST_CTL_SEL_MASK,
				 CS40L26_BST_CTL_SEL_FIXED);
	if (ret)
		return ret;

	ret = regmap_update_bits(cs40l26->regmap, CS40L26_VBST_CTL_1, CS40L26_BST_CTL_MASK,
				 CS40L26_BST_CTL_VP);
	if (ret)
		return ret;

	ret = regmap_set_bits(cs40l26->regmap, CS40L26_GLOBAL_ENABLES, CS40L26_GLOBAL_EN);
	if (ret)
		return ret;

	/* Wait for boost converter to power up */
	usleep_range(CS40L26_BST_TIME_US, CS40L26_BST_TIME_US + 100);

	ret = regmap_read(cs40l26->regmap, CS40L26_ERROR_RELEASE, &err);
	if (ret)
		return ret;

	if (err & BIT(CS40L26_ERROR_BST_SHORT)) {
		dev_err(cs40l26->dev, "Boost shorted at startup\n");
		return -ENOTRECOVERABLE;
	}

	/* Return to previous state before test */
	ret = regmap_clear_bits(cs40l26->regmap, CS40L26_GLOBAL_ENABLES, CS40L26_GLOBAL_EN);
	if (ret)
		return ret;

	ret = regmap_write(cs40l26->regmap, CS40L26_VBST_CTL_1, vbst_ctl_1);
	if (ret)
		return ret;

	return regmap_write(cs40l26->regmap, CS40L26_VBST_CTL_2, vbst_ctl_2);
}

static const struct reg_sequence cs40l26_a1_b1_errata[] = {
	{ CS40L26_PLL_REFCLK_DETECT_0, CS40L26_PLL_REFCLK_DET_DISABLE },
	{ 0x00000040, 0x00000055 },
	{ 0x00000040, 0x000000AA },
	{ CS40L26_TEST_LBST, CS40L26_DISABLE_EXPL_MODE },
};

static int cs40l26_a1_b1_handle_errata(struct cs40l26 *cs40l26)
{
	int ret;

	/*
	 * Boost Exploratory Mode must be disabled on 0xA1/0xB1 devices in order to ensure there is
	 * no unintentional damage to the boost inductor. Any boost short that occurs after the
	 * LBST short test at probe will not be detected.
	 */
	ret = cs40l26_lbst_short_test(cs40l26);
	if (ret)
		return ret;

	ret = regmap_multi_reg_write(cs40l26->regmap, cs40l26_a1_b1_errata,
				     ARRAY_SIZE(cs40l26_a1_b1_errata));
	if (ret)
		return ret;

	return cs_dsp_wseq_multi_write(&cs40l26->dsp, &cs40l26->wseqs[CS40L26_WSEQ_POWER_ON],
				       cs40l26_a1_b1_errata, ARRAY_SIZE(cs40l26_a1_b1_errata),
				       CS_DSP_WSEQ_FULL, false);
}

static const struct cs40l26_variant_info cs40l26_a1_b1_info = {
	.pm_cur_state = CS40L26_A1_B1_PM_CUR_STATE,
	.pm_state_locks = CS40L26_A1_B1_PM_STATE_LOCKS,
	.pm_state_locks3 = CS40L26_A1_B1_PM_STATE_LOCKS3,
	.pm_stdby_ticks = CS40L26_A1_B1_PM_STDBY_TICKS,
	.pm_active_ticks = CS40L26_A1_B1_PM_ACTIVE_TICKS,
	.halo_state = CS40L26_A1_B1_HALO_STATE,
	.event_map_1 = CS40L26_A1_B1_EVENT_MAP_1,
	.event_map_2 = CS40L26_A1_B1_EVENT_MAP_2,
	.fw_min_rev = CS40L26_FW_A1_B1_MIN_REV,
	.ram_ext_algo_id = CS40L26_EXT_ALGO_ID,
	.vibegen_algo_id = CS40L26_VIBEGEN_ALGO_ID_A1,
};

static const struct cs40l26_variant_info cs40l26_b2_info = {
	.pm_cur_state = CS40L26_B2_PM_CUR_STATE,
	.pm_state_locks = CS40L26_B2_PM_STATE_LOCKS,
	.pm_state_locks3 = CS40L26_B2_PM_STATE_LOCKS3,
	.pm_stdby_ticks = CS40L26_B2_PM_STDBY_TICKS,
	.pm_active_ticks = CS40L26_B2_PM_ACTIVE_TICKS,
	.halo_state = CS40L26_B2_HALO_STATE,
	.event_map_1 = CS40L26_B2_EVENT_MAP_1,
	.event_map_2 = CS40L26_B2_EVENT_MAP_2,
	.fw_min_rev = CS40L26_FW_B2_MIN_REV,
	.ram_ext_algo_id = CS40L26_FW_ID,
	.vibegen_algo_id = CS40L26_VIBEGEN_ALGO_ID_B2,
};

static const struct cs40l26_variant cs40l26_a1_b1_variant = {
	.info = &cs40l26_a1_b1_info,
	.handle_errata = &cs40l26_a1_b1_handle_errata,
};

static const struct cs40l26_variant cs40l26_b2_variant = {
	.info = &cs40l26_b2_info,
	.handle_errata = NULL,
};

static inline int cs40l26_pm_timeout_ms_set(struct cs40l26 *cs40l26, const u32 dsp_state,
					    const u32 timeout_ms)
{
	return regmap_write(cs40l26->regmap,
			    dsp_state == CS40L26_DSP_STATE_STANDBY ?
			     cs40l26->variant->info->pm_stdby_ticks :
			     cs40l26->variant->info->pm_active_ticks,
			     (timeout_ms * CS40L26_PM_TICKS_PER_SEC) / 1000);
}

static int cs40l26_pm_timeout_ms_get(struct cs40l26 *cs40l26, const u32 dsp_state, u32 *timeout_ms)
{
	u32 timeout_ticks;
	int ret;

	ret = regmap_read(cs40l26->regmap,
			  dsp_state == CS40L26_DSP_STATE_STANDBY ?
			  cs40l26->variant->info->pm_stdby_ticks :
			  cs40l26->variant->info->pm_active_ticks,
			  &timeout_ticks);
	if (ret)
		return ret;

	*timeout_ms = DIV_ROUND_UP(timeout_ticks * 1000, CS40L26_PM_TICKS_PER_SEC);

	return 0;
}

static int cs40l26_pm_runtime_setup(struct device *dev)
{
	int ret;

	pm_runtime_set_autosuspend_delay(dev, CS40L26_AUTOSUSPEND_DELAY_MS);
	pm_runtime_use_autosuspend(dev);
	pm_runtime_get_noresume(dev);
	ret = pm_runtime_set_active(dev);
	if (ret)
		return ret;

	return devm_pm_runtime_enable(dev);
}

static int cs40l26_dsp_pre_config(struct cs40l26 *cs40l26)
{
	u32 dsp_state, halo_state, timeout_ms;
	int i, ret;

	ret = regmap_read(cs40l26->regmap, cs40l26->variant->info->halo_state, &halo_state);
	if (ret)
		return ret;

	if (halo_state != CS40L26_DSP_HALO_STATE_RUN) {
		dev_err(cs40l26->dev, "Invalid DSP state: %u\n", halo_state);
		return -EINVAL;
	}

	ret = cs40l26_pm_timeout_ms_get(cs40l26, CS40L26_DSP_STATE_ACTIVE, &timeout_ms);
	if (ret) {
		dev_err(cs40l26->dev, "Failed to get ACTIVE timeout\n");
		return ret;
	}

	for (i = 0; i < CS40L26_DSP_STATE_TIMEOUT_COUNT; i++) {
		ret = cs40l26_dsp_state_get(cs40l26, &dsp_state);
		if (ret)
			return ret;

		if (dsp_state != CS40L26_DSP_STATE_SHUTDOWN &&
		    dsp_state != CS40L26_DSP_STATE_STANDBY)
			dev_warn(cs40l26->dev, "DSP core not safe to kill\n");
		else
			break;

		usleep_range(timeout_ms * 1000, (timeout_ms * 1000) + 100);
	}

	if (i == CS40L26_DSP_STATE_TIMEOUT_COUNT) {
		dev_err(cs40l26->dev, "DSP Core could not be shut down\n");
		return -ETIMEDOUT;
	}

	return regmap_write(cs40l26->regmap, CS40L26_DSP1_CCM_CORE_CONTROL,
			    CS40L26_DSP_CCM_CORE_KILL);
}

static const struct cs_dsp_region cs40l26_dsp_regions[] = {
	{ .type = WMFW_HALO_PM_PACKED, .base = CS40L26_DSP1_PMEM_0 },
	{ .type = WMFW_HALO_XM_PACKED, .base = CS40L26_DSP1_XMEM_PACKED_0 },
	{ .type = WMFW_HALO_YM_PACKED, .base = CS40L26_DSP1_YMEM_PACKED_0 },
	{ .type = WMFW_ADSP2_XM, .base = CS40L26_DSP1_XMEM_UNPACKED24_0 },
	{ .type = WMFW_ADSP2_YM, .base = CS40L26_DSP1_YMEM_UNPACKED24_0 },
};

static int cs40l26_get_model(struct cs40l26 *cs40l26)
{
	int ret;

	ret = regmap_read(cs40l26->regmap, CS40L26_DEVID, &cs40l26->devid);
	if (ret)
		return ret;

	ret = regmap_read(cs40l26->regmap, CS40L26_REVID, &cs40l26->revid);
	if (ret)
		return ret;

	switch (cs40l26->devid) {
	case CS40L26_DEVID_L26:
		if (cs40l26->revid != CS40L26_REVID_A1 && cs40l26->revid != CS40L26_REVID_B1)
			goto err;

		cs40l26->variant = &cs40l26_a1_b1_variant;
		break;
	case CS40L26_DEVID_L27:
		if (cs40l26->revid != CS40L26_REVID_B2)
			goto err;

		cs40l26->variant = &cs40l26_b2_variant;
		break;
	default:
		dev_err(cs40l26->dev, "Invalid device ID 0x%06X\n", cs40l26->devid);
		return -EINVAL;
	}

	dev_info(cs40l26->dev, "Cirrus Logic CS40L26 ID: 0x%06X, Revision: 0x%02X\n",
		 cs40l26->devid, cs40l26->revid);

	return 0;

err:
	dev_err(cs40l26->dev, "Invalid revision 0x%02X for device 0x%06X\n", cs40l26->revid,
		cs40l26->devid);
	return -EINVAL;
}

int cs40l26_set_pll_loop(struct cs40l26 *cs40l26, const u32 pll_loop)
{
	int i;

	/* Retry in case DSP is hibernating */
	for (i = 0; i < CS40L26_PLL_NUM_SET_ATTEMPTS; i++) {
		if (!regmap_update_bits(cs40l26->regmap, CS40L26_REFCLK_INPUT,
					CS40L26_PLL_REFCLK_LOOP_MASK,
					pll_loop << CS40L26_PLL_REFCLK_LOOP_SHIFT))
			break;
	}

	if (i == CS40L26_PLL_NUM_SET_ATTEMPTS) {
		dev_err(cs40l26->dev, "Failed to configure PLL\n");
		return -ETIMEDOUT;
	}

	return 0;
}
EXPORT_SYMBOL_GPL(cs40l26_set_pll_loop);

static int cs40l26_wseq_init(struct cs40l26 *cs40l26)
{
	struct cs_dsp *dsp = &cs40l26->dsp;

	cs40l26->wseqs[CS40L26_WSEQ_POWER_ON].ctl =
		cs_dsp_get_ctl(dsp, "POWER_ON_SEQUENCE", WMFW_ADSP2_XM, CS40L26_PM_ALGO_ID);
	if (!cs40l26->wseqs[CS40L26_WSEQ_POWER_ON].ctl) {
		dev_err(cs40l26->dev, "POWER_ON write sequence not found\n");
		return -EINVAL;
	}

	cs40l26->wseqs[CS40L26_WSEQ_ACTIVE].ctl =
		cs_dsp_get_ctl(dsp, "ACTIVE_SEQUENCE", WMFW_ADSP2_XM, CS40L26_PM_ALGO_ID);
	if (!cs40l26->wseqs[CS40L26_WSEQ_ACTIVE].ctl) {
		dev_err(cs40l26->dev, "ACTIVE write sequence not found\n");
		return -EINVAL;
	}

	cs40l26->wseqs[CS40L26_WSEQ_STANDBY].ctl =
		cs_dsp_get_ctl(dsp, "STANDBY_SEQUENCE", WMFW_ADSP2_XM, CS40L26_PM_ALGO_ID);
	if (!cs40l26->wseqs[CS40L26_WSEQ_STANDBY].ctl) {
		dev_err(cs40l26->dev, "STANDBY write sequence not found\n");
		return -EINVAL;
	}

	return cs_dsp_wseq_init(dsp, cs40l26->wseqs, CS40L26_NUM_WSEQS);
}

static int cs40l26_wksrc_config(struct cs40l26 *cs40l26)
{
	u32 wksrc;
	int ret;

	if (!strncmp(cs40l26->bus->name, "spi", strlen(cs40l26->bus->name))) {
		cs40l26_irq_unmask(cs40l26, 1, CS40L26_IRQ_WKSRC_STS_SPI);
		wksrc = CS40L26_WKSRC_POL_SPI | CS40L26_WKSRC_EN_SPI;
	} else if (!strncmp(cs40l26->bus->name, "i2c", strlen(cs40l26->bus->name))) {
		cs40l26_irq_unmask(cs40l26, 1, CS40L26_IRQ_WKSRC_STS_I2C);
		wksrc = CS40L26_WKSRC_EN_I2C;
	} else {
		dev_err(cs40l26->dev, "Invalid bus type\n");
		return -EINVAL;
	}

	cs40l26_irq_unmask(cs40l26, 1, CS40L26_IRQ_WKSRC_STS_GPIO1);
	cs40l26_irq_unmask(cs40l26, 1, CS40L26_IRQ_WKSRC_STS_ANY);

	ret = regmap_write(cs40l26->regmap, CS40L26_WAKESRC_CTL, wksrc);
	if (ret)
		return ret;

	return cs_dsp_wseq_write(&cs40l26->dsp, &cs40l26->wseqs[CS40L26_WSEQ_POWER_ON],
				 CS40L26_WAKESRC_CTL, wksrc, CS_DSP_WSEQ_L16, true);
}

static inline void cs40l26_gpio_config(struct cs40l26 *cs40l26)
{
	cs40l26_irq_unmask(cs40l26, 1, CS40L26_IRQ_GPIO1_RISE);
	cs40l26_irq_unmask(cs40l26, 1, CS40L26_IRQ_GPIO1_FALL);
}

static int cs40l26_bst_ipk_config(struct cs40l26 *cs40l26)
{
	u32 bst_ipk;
	int ret;

	bst_ipk = (clamp_val(cs40l26->bst_ipk_ua, CS40L26_BST_IPK_UA_MIN, CS40L26_BST_IPK_UA_MAX) -
		   CS40L26_BST_IPK_UA_OFFSET) / CS40L26_BST_IPK_UA_STEP;

	cs40l26_irq_unmask(cs40l26, 1, CS40L26_IRQ_BST_IPK_FLAG);

	ret = regmap_write(cs40l26->regmap, CS40L26_BST_IPK_CTL, bst_ipk);
	if (ret)
		return ret;

	return cs_dsp_wseq_write(&cs40l26->dsp, &cs40l26->wseqs[CS40L26_WSEQ_POWER_ON],
				 CS40L26_BST_IPK_CTL, bst_ipk, CS_DSP_WSEQ_L16, true);
}

static int cs40l26_bst_ctl_config(struct cs40l26 *cs40l26)
{
	u32 bst_ctl, bst_ctl_lim;
	int ret;

	ret = regmap_read(cs40l26->regmap, CS40L26_VBST_CTL_2, &bst_ctl_lim);
	if (ret)
		return ret;

	bst_ctl_lim |= FIELD_PREP(CS40L26_BST_CTL_LIM_EN_MASK, CS40L26_BST_CTL_LIM_EN);

	ret = regmap_write(cs40l26->regmap, CS40L26_VBST_CTL_2, bst_ctl_lim);
	if (ret)
		return ret;

	ret = cs_dsp_wseq_write(&cs40l26->dsp, &cs40l26->wseqs[CS40L26_WSEQ_POWER_ON],
				CS40L26_VBST_CTL_2, bst_ctl_lim, CS_DSP_WSEQ_L16, true);
	if (ret)
		return ret;

	bst_ctl = (clamp_val(cs40l26->vbst_uv, CS40L26_BST_UV_MIN, CS40L26_BST_UV_MAX) -
		   CS40L26_BST_UV_MIN) / CS40L26_BST_UV_STEP;

	ret = regmap_write(cs40l26->regmap, CS40L26_VBST_CTL_1, bst_ctl);
	if (ret)
		return ret;

	return cs_dsp_wseq_write(&cs40l26->dsp, &cs40l26->wseqs[CS40L26_WSEQ_POWER_ON],
				 CS40L26_VBST_CTL_1, bst_ctl, CS_DSP_WSEQ_L16, true);
}

static int cs40l26_irq_init(struct cs40l26 *cs40l26)
{
	int i, ret;

	/* Unmask relevant warnings and error interrupts */
	for (i = CS40L26_IRQ_BST_OVP_FLAG_RISE; i <= CS40L26_IRQ_AMP_ERR; i++)
		cs40l26_irq_unmask(cs40l26, 1, i);

	cs40l26_irq_unmask(cs40l26, 1, CS40L26_IRQ_DSP_RX_QUEUE);

	for (i = CS40L26_IRQ_VPMON_CLIPPED; i <= CS40L26_IRQ_IMON_CLIPPED; i++)
		cs40l26_irq_unmask(cs40l26, 2, i);

	for (i = CS40L26_IRQ_REFCLK_PRESENT; i <= CS40L26_IRQ_REFCLK_MISSING_RISE; i++)
		cs40l26_irq_unmask(cs40l26, 2, i);

	ret = regmap_multi_reg_write(cs40l26->regmap, cs40l26_irq_masks, 2);
	if (ret)
		return ret;

	ret = cs_dsp_wseq_multi_write(&cs40l26->dsp, &cs40l26->wseqs[CS40L26_WSEQ_POWER_ON],
				      cs40l26_irq_masks, 2, CS_DSP_WSEQ_FULL, true);
	if (ret)
		return ret;

	ret = devm_request_threaded_irq(cs40l26->dev, cs40l26->irq, NULL, cs40l26_irq_handler,
					IRQF_ONESHOT, "cs40l26", cs40l26);
	if (ret)
		dev_err(cs40l26->dev, "Failed to request IRQ\n");

	return ret;
}

static int cs40l26_hw_init(struct cs40l26 *cs40l26)
{
	int ret;

	cs40l26->irq_masks = cs40l26_irq_masks;

	ret = cs40l26_wksrc_config(cs40l26);
	if (ret)
		return ret;

	cs40l26_gpio_config(cs40l26);

	ret = cs40l26_bst_ipk_config(cs40l26);
	if (ret)
		return ret;

	ret = cs40l26_bst_ctl_config(cs40l26);
	if (ret)
		return ret;

	return cs40l26_irq_init(cs40l26);
}

static int cs40l26_cs_dsp_pre_run(struct cs_dsp *dsp)
{
	struct cs40l26 *cs40l26 = container_of(dsp, struct cs40l26, dsp);
	int ret;

	ret = cs40l26_pm_timeout_ms_set(cs40l26, CS40L26_DSP_STATE_STANDBY, 100);
	if (ret) {
		dev_err(cs40l26->dev, "Failed to set standby timeout\n");
		return ret;
	}

	ret = cs40l26_pm_timeout_ms_set(cs40l26, CS40L26_DSP_STATE_ACTIVE, 250);
	if (ret) {
		dev_err(cs40l26->dev, "Failed to set active timeout\n");
		return ret;
	}

	ret = regmap_set_bits(cs40l26->regmap, CS40L26_PWRMGT_CTL, CS40L26_MEM_RDY);
	if (ret) {
		dev_err(cs40l26->dev, "Failed to set MEM_RDY\n");
		return ret;
	}

	ret = cs40l26_fw_read(dsp, "QUEUE_BASE", CS40L26_DSP_ALGO_ID, &cs40l26->queue_base);
	if (ret)
		return ret;

	ret = cs40l26_fw_read(dsp, "QUEUE_LEN", CS40L26_DSP_ALGO_ID, &cs40l26->queue_len);
	if (ret)
		return ret;

	cs40l26->queue_last = cs40l26->queue_base + ((cs40l26->queue_len - 1) * sizeof(u32));

	ret = cs40l26_fw_write(dsp, "CALL_RAM_INIT", dsp->fw_id, 1);
	if (ret)
		return ret;

	ret = cs40l26_wseq_init(cs40l26);
	if (ret)
		return ret;

	if (cs40l26->variant->handle_errata)
		return cs40l26->variant->handle_errata(cs40l26);
	else
		return 0;
}

static int cs40l26_cs_dsp_post_run(struct cs_dsp *dsp)
{
	struct cs40l26 *cs40l26 = container_of(dsp, struct cs40l26, dsp);
	u32 halo_state;
	int ret;

	/*
	 * cs_dsp_halo_start_core() has reset the DSP core at this point.
	 * Hibernation must be disabled again.
	 */
	ret = cs40l26_prevent_hiber(cs40l26);
	if (ret)
		return ret;

	ret = cs40l26_fw_read(dsp, "HALO_STATE", dsp->fw_id, &halo_state);
	if (ret)
		return ret;

	if (halo_state != CS40L26_DSP_HALO_STATE_RUN) {
		dev_err(dsp->dev, "Invalid DSP state: %u\n", halo_state);
		return -EINVAL;
	}

	ret = cs40l26_hw_init(cs40l26);
	if (ret)
		return ret;

	dev_dbg(dsp->dev, "CS40L26/L27 DSP started successfully\n");

	ret = devm_mfd_add_devices(cs40l26->dev, PLATFORM_DEVID_AUTO, cs40l26_devs,
				   ARRAY_SIZE(cs40l26_devs), NULL, 0, NULL);
	if (ret)
		dev_err(cs40l26->dev, "Failed to add MFD child devices: %d\n", ret);

	return ret;
}

static const struct cs_dsp_client_ops cs40l26_cs_dsp_client_ops = {
	.pre_run = cs40l26_cs_dsp_pre_run,
	.post_run = cs40l26_cs_dsp_post_run,
};

static void cs40l26_cs_dsp_remove(void *data)
{
	cs_dsp_remove((struct cs_dsp *)data);
}

static struct cs_dsp_coeff_desc cs40l26_coeffs[] = {
	{ .coeff_firmware = NULL, .coeff_filename = "cs40l26.bin" },
	{ .coeff_firmware = NULL, .coeff_filename = "cs40l26-svc.bin" },
	{ .coeff_firmware = NULL, .coeff_filename = "cs40l26-dvl.bin" },
};

static int cs40l26_cs_dsp_init(struct cs40l26 *cs40l26)
{
	struct cs_dsp *dsp = &cs40l26->dsp;
	int ret;

	dsp->num = 1;
	dsp->type = WMFW_HALO;
	dsp->dev = cs40l26->dev;
	dsp->regmap = cs40l26->regmap;
	dsp->base = CS40L26_DSP_CTRL_BASE;
	dsp->base_sysinfo = CS40L26_DSP1_SYS_INFO_ID;
	dsp->mem = cs40l26_dsp_regions;
	dsp->num_mems = ARRAY_SIZE(cs40l26_dsp_regions);
	dsp->client_ops = &cs40l26_cs_dsp_client_ops;

	ret = cs_dsp_halo_init(dsp);
	if (ret) {
		dev_err(cs40l26->dev, "Failed to initialize HALO core\n");
		return ret;
	}

	return devm_add_action_or_reset(cs40l26->dev, cs40l26_cs_dsp_remove, dsp);
}

static void cs40l26_dsp_start(struct cs40l26 *cs40l26)
{
	int i, ret;

	ret = cs40l26_dsp_pre_config(cs40l26);
	if (ret) {
		dev_err(cs40l26->dev, "DSP Pre Config. Failed: %d\n", ret);
		goto err_fw_rls;
	}

	guard(mutex)(&cs40l26->lock);

	ret = cs_dsp_power_up_multiple(&cs40l26->dsp, cs40l26->wmfw, "cs40l26.wmfw", cs40l26_coeffs,
				       CS40L26_NUM_COEFF_FILES, "cs40l26");
	if (ret) {
		dev_err(cs40l26->dev, "Failed to Power Up DSP\n");
		goto err_fw_rls;
	}

	if (cs40l26->dsp.fw_id != CS40L26_FW_ID) {
		dev_err(cs40l26->dev, "Invalid firmware ID: 0x%X\n", cs40l26->dsp.fw_id);
		goto err_fw_rls;
	}

	if (cs40l26->dsp.fw_id_version < cs40l26->variant->info->fw_min_rev) {
		dev_err(cs40l26->dev, "Invalid firmware revision: 0x%X\n",
			cs40l26->dsp.fw_id_version);
		goto err_fw_rls;
	}

	ret = cs_dsp_run(&cs40l26->dsp);
	if (ret)
		dev_err(cs40l26->dev, "DSP Failed to run: %d\n", ret);

err_fw_rls:
	for (i = 0; i < CS40L26_NUM_COEFF_FILES; i++)
		release_firmware(cs40l26_coeffs[i].coeff_firmware);

	release_firmware(cs40l26->wmfw);
}

static void cs40l26_fw_upload(const struct firmware *wmfw, void *context)
{
	struct cs40l26 *cs40l26 = (struct cs40l26 *)context;
	const struct firmware *coeff;
	int i, ret;

	if (!wmfw) {
		dev_err(cs40l26->dev, "Failed to request firmware file\n");
		return;
	}

	cs40l26->wmfw = wmfw;

	for (i = 0; i < CS40L26_NUM_COEFF_FILES; i++) {
		ret = request_firmware(&coeff, cs40l26_coeffs[i].coeff_filename, cs40l26->dev);
		if (ret)
			continue;

		cs40l26_coeffs[i].coeff_firmware = coeff;
	}

	return cs40l26_dsp_start(cs40l26);
}

static int cs40l26_init(struct cs40l26 *cs40l26)
{
	int ret;

	cs40l26->bst_ipk_ua = CS40L26_BST_IPK_UA_DEFAULT;
	cs40l26->vbst_uv = CS40L26_BST_UV_MAX;
	/*
	 * Set the PLL to open-loop and remove default GPI mappings to prevent DSP lockup while
	 * the driver configures RAM firmware.
	 *
	 * The firmware will set the PLL back to closed-loop when the DSP has been started.
	 */
	ret = cs40l26_set_pll_loop(cs40l26, CS40L26_PLL_OPEN);
	if (ret)
		return ret;

	ret = regmap_write(cs40l26->regmap, cs40l26->variant->info->event_map_1,
			   CS40L26_EVENT_MAP_GPI_DISABLE);
	if (ret)
		return ret;

	ret = regmap_write(cs40l26->regmap, cs40l26->variant->info->event_map_2,
			   CS40L26_EVENT_MAP_GPI_DISABLE);
	if (ret)
		return ret;

	/* Set LRA to HI-Z to avoid fault conditions */
	return regmap_set_bits(cs40l26->regmap, CS40L26_TST_DAC_MSM_CONFIG,
			       CS40L26_SPK_DEFAULT_HIZ);
}

static int cs40l26_parse_properties(struct cs40l26 *cs40l26)
{
	struct device *dev = cs40l26->dev;
	int ret;

	ret = device_property_read_u32(dev, "cirrus,bst-ctl-microvolt", &cs40l26->vbst_uv);
	if (ret && ret != -EINVAL)
		return ret;

	ret = device_property_read_u32(dev, "cirrus,bst-ipk-microamp", &cs40l26->bst_ipk_ua);
	if (ret && ret != -EINVAL)
		return ret;

	return 0;
}

int cs40l26_probe(struct cs40l26 *cs40l26)
{
	int ret;

	mutex_init(&cs40l26->lock);

	cs40l26->reset_gpio = devm_gpiod_get_optional(cs40l26->dev, "reset", GPIOD_OUT_HIGH);
	if (!cs40l26->reset_gpio)
		return dev_err_probe(cs40l26->dev, -EINVAL, "Failed to get reset GPIO\n");

	ret = devm_regulator_bulk_get_enable(cs40l26->dev, ARRAY_SIZE(cs40l26_supplies),
					     cs40l26_supplies);
	if (ret)
		return dev_err_probe(cs40l26->dev, ret, "Failed to get supplies\n");

	usleep_range(CS40L26_MIN_RESET_PULSE_US, CS40L26_MIN_RESET_PULSE_US + 100);

	gpiod_set_value_cansleep(cs40l26->reset_gpio, 0);

	usleep_range(CS40L26_CP_READY_DELAY_US, CS40L26_CP_READY_DELAY_US + 100);

	ret = cs40l26_get_model(cs40l26);
	if (ret)
		return dev_err_probe(cs40l26->dev, ret, "Failed to get part number\n");

	ret = cs40l26_prevent_hiber(cs40l26);
	if (ret)
		return dev_err_probe(cs40l26->dev, ret, "Failed to prevent hibernation\n");

	ret = cs40l26_init(cs40l26);
	if (ret)
		return dev_err_probe(cs40l26->dev, ret, "Failed to initialize device\n");

	ret = cs40l26_parse_properties(cs40l26);
	if (ret)
		return dev_err_probe(cs40l26->dev, ret, "Failed to parse devicetree\n");

	ret = cs40l26_cs_dsp_init(cs40l26);
	if (ret)
		return dev_err_probe(cs40l26->dev, ret, "Failed to initialize CS DSP\n");

	ret = cs40l26_pm_runtime_setup(cs40l26->dev);
	if (ret)
		return dev_err_probe(cs40l26->dev, ret, "Failed to set up PM Runtime\n");

	ret = request_firmware_nowait(THIS_MODULE, FW_ACTION_UEVENT, "cs40l26.wmfw", cs40l26->dev,
				      GFP_KERNEL, cs40l26, cs40l26_fw_upload);
	if (ret)
		return dev_err_probe(cs40l26->dev, ret, "Failed to load firmware\n");

	cs40l26_pm_exit(cs40l26->dev);

	return 0;
}
EXPORT_SYMBOL_GPL(cs40l26_probe);

static int __maybe_unused cs40l26_suspend(struct device *dev)
{
	struct cs40l26 *cs40l26 = dev_get_drvdata(dev);

	guard(mutex)(&cs40l26->lock);

	dev_dbg(dev, "%s: Enabling hibernation\n", __func__);

	cs40l26->wksrc_sts = 0x00;

	/* Don't poll DSP since reading for ACK will wake the device again */
	return regmap_write(cs40l26->regmap, CS40L26_DSP_QUEUE, CS40L26_DSP_CMD_ALLOW_HIBER);
}

static int __maybe_unused cs40l26_sys_suspend(struct device *dev)
{
	struct cs40l26 *cs40l26 = dev_get_drvdata(dev);

	dev_dbg(dev, "System suspend, disabling IRQ\n");

	disable_irq(cs40l26->irq);

	return 0;
}

static int __maybe_unused cs40l26_sys_suspend_noirq(struct device *dev)
{
	struct cs40l26 *cs40l26 = dev_get_drvdata(dev);

	dev_dbg(dev, "Late system suspend, re-enabling IRQ\n");

	enable_irq(cs40l26->irq);

	return 0;
}

static int __maybe_unused cs40l26_resume(struct device *dev)
{
	struct cs40l26 *cs40l26 = dev_get_drvdata(dev);

	dev_dbg(dev, "%s: Disabling hibernation\n", __func__);

	guard(mutex)(&cs40l26->dsp.pwr_lock);

	return cs40l26_prevent_hiber(cs40l26);
}

static int __maybe_unused cs40l26_sys_resume(struct device *dev)
{
	struct cs40l26 *cs40l26 = dev_get_drvdata(dev);

	dev_dbg(dev, "System resume, re-enabling IRQ\n");

	enable_irq(cs40l26->irq);

	return 0;
}

static int __maybe_unused cs40l26_sys_resume_noirq(struct device *dev)
{
	struct cs40l26 *cs40l26 = dev_get_drvdata(dev);

	dev_dbg(dev, "Early system resume, disabling IRQ\n");

	disable_irq(cs40l26->irq);

	return 0;
}

EXPORT_GPL_DEV_PM_OPS(cs40l26_pm_ops) = {
	RUNTIME_PM_OPS(cs40l26_suspend, cs40l26_resume, NULL)
	SYSTEM_SLEEP_PM_OPS(cs40l26_sys_suspend, cs40l26_sys_resume)
	NOIRQ_SYSTEM_SLEEP_PM_OPS(cs40l26_sys_suspend_noirq, cs40l26_sys_resume_noirq)
};

MODULE_DESCRIPTION("CS40L26 Boosted Class D Amplifier for Haptics");
MODULE_AUTHOR("Fred Treven, Cirrus Logic Inc. <ftreven@opensource.cirrus.com>");
MODULE_LICENSE("GPL");
MODULE_IMPORT_NS("FW_CS_DSP");

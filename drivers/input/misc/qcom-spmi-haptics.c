// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

#include <linux/bitfield.h>
#include <linux/bits.h>
#include <linux/device.h>
#include <linux/input.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/pm_runtime.h>
#include <linux/uaccess.h>
#include <linux/workqueue.h>

/* HAP_CFG register offsets, bit fields, value constants */
#define HAP_CFG_INT_RT_STS_REG		0x10
#define  FIFO_EMPTY_BIT			BIT(1)
#define HAP_CFG_EN_CTL_REG		0x46
#define  HAPTICS_EN_BIT			BIT(7)
#define HAP_CFG_VMAX_REG		0x48
#define   VMAX_STEP_MV			50
#define   VMAX_MV_MAX			10000
#define HAP_CFG_SPMI_PLAY_REG		0x4C
#define  PLAY_EN_BIT			BIT(7)
#define  PAT_SRC_MASK			GENMASK(2, 0)
#define   PAT_SRC_FIFO			0
#define   PAT_SRC_DIRECT_PLAY		1
#define HAP_CFG_TLRA_OL_HIGH_REG	0x5C
#define  TLRA_OL_MSB_MASK		GENMASK(3, 0)
#define   TLRA_STEP_US			5
#define HAP_CFG_TLRA_OL_LOW_REG		0x5D
#define HAP_CFG_DRV_DUTY_CFG_REG	0x60
#define  ADT_DRV_DUTY_EN_BIT		BIT(7)
#define  ADT_BRK_DUTY_EN_BIT		BIT(6)
#define  DRV_DUTY_MASK			GENMASK(5, 3)
#define   AUTORES_DRV_DUTY_62P5		2
#define  BRK_DUTY_MASK			GENMASK(2, 0)
#define   AUTORES_BRK_DUTY_62P5		5
#define HAP_CFG_ZX_WIND_CFG_REG		0x62
#define  ZX_DEBOUNCE_MASK		GENMASK(6, 4)
#define   AUTORES_ZX_DEBOUNCE		3
#define  ZX_WIN_HEIGHT_MASK		GENMASK(2, 0)
#define   AUTORES_ZX_WIN_HEIGHT		2
#define HAP_CFG_AUTORES_CFG_REG		0x63
#define  AUTORES_EN_BIT			BIT(7)
#define  AUTORES_EN_DLY_MASK		GENMASK(6, 2)
#define   AUTORES_EN_DLY_CYCLES		10
#define  AUTORES_ERR_WIN_MASK		GENMASK(1, 0)
#define   AUTORES_ERR_WIN_25PCT		1
#define HAP_CFG_FAULT_CLR_REG		0x66
#define  ZX_TO_FAULT_CLR_BIT		BIT(4)
#define  SC_CLR_BIT			BIT(2)
#define  AUTO_RES_ERR_CLR_BIT		BIT(1)
#define  HPWR_RDY_FAULT_CLR_BIT		BIT(0)
#define  FAULT_CLR_ALL	(ZX_TO_FAULT_CLR_BIT | SC_CLR_BIT | \
			 AUTO_RES_ERR_CLR_BIT | HPWR_RDY_FAULT_CLR_BIT)
#define HAP_CFG_RAMP_DN_CFG2_REG	0x86
#define  AUTORES_PRE_HIZ_DLY_10US	1

/* HAP_PTN register offsets, bit fields, value constants */
#define HAP_PTN_REVISION2_REG		0x01
#define HAP_PTN_FIFO_DIN_0_REG		0x20
#define HAP_PTN_FIFO_PLAY_RATE_REG	0x24
#define  FIFO_PLAY_RATE_MASK		GENMASK(3, 0)
#define HAP_PTN_DIRECT_PLAY_REG		0x26
#define HAP_PTN_FIFO_EMPTY_CFG_REG	0x2A
#define  FIFO_THRESH_LSB		64
#define HAP_PTN_FIFO_DIN_1B_REG		0x2C
#define HAP_PTN_MEM_OP_ACCESS_REG	0x2D
#define  MEM_FLUSH_RELOAD_BIT		BIT(0)
#define HAP_PTN_MMAP_FIFO_REG		0xA0
#define  MMAP_FIFO_EXIST_BIT		BIT(7)
#define  MMAP_FIFO_LEN_MASK		GENMASK(6, 0)
#define HAP_PTN_PATX_PLAY_CFG_REG	0xA2

#define HAP530_MEM_TOTAL_BYTES		8192
#define FIFO_EMPTY_THRESH		280
#define FIFO_INIT_FILL			320

#define HAPTICS_AUTOSUSPEND_MS		1000

/*
 * FF_CUSTOM data layout (custom_data[] of type s16):
 *   [0] = play rate (PLAY_RATE_*)
 *   [1] = vmax in mV (0 = use device default from qcom,vmax-microvolt)
 *   [2..N-1] = signed 8-bit PCM samples packed one per s16 (lower byte used)
 */
#define CUSTOM_DATA_RATE_IDX		0
#define CUSTOM_DATA_VMAX_IDX		1
#define CUSTOM_DATA_SAMPLE_START	2

#define HAPTICS_MAX_EFFECTS		8

enum qcom_haptics_mode {
	HAPTICS_DIRECT_PLAY,
	HAPTICS_FIFO,
};

enum qcom_haptics_play_rate {
	PLAY_RATE_T_LRA       = 0,
	PLAY_RATE_T_LRA_DIV_2 = 1,
	PLAY_RATE_T_LRA_DIV_4 = 2,
	PLAY_RATE_T_LRA_DIV_8 = 3,
	/* 4–7 are reserved */
	PLAY_RATE_F_8KHZ      = 8,
	PLAY_RATE_F_16KHZ     = 9,
	PLAY_RATE_F_24KHZ     = 10,
	PLAY_RATE_F_32KHZ     = 11,
	PLAY_RATE_F_44P1KHZ   = 12,
	PLAY_RATE_F_48KHZ     = 13,
	PLAY_RATE_MAX	      = PLAY_RATE_F_48KHZ,
};

struct qcom_haptics_effect {
	enum qcom_haptics_mode mode;
	enum qcom_haptics_play_rate play_rate;
	u32 vmax_mv;
	s8 *fifo_data;
	u32 data_len;
};

/**
 * struct qcom_haptics
 * @dev:          underlying SPMI device
 * @regmap:       regmap for SPMI register access
 * @input:        input device exposing the FF interface
 * @cfg_base:     base address of the CFG peripheral
 * @ptn_base:     base address of the PTN peripheral
 * @t_lra_us:     LRA resonance period in microseconds
 * @vmax_mv:      maximum actuator drive voltage in millivolts
 * @fifo_len:     programmed HW FIFO depth in bytes
 * @gain:         playback gain scaler
 * @play_work:    deferred work item that starts or stops playback
 * @play_lock:    mutex lock to serialize playbacks
 * @cur_effect_id: index into @effects[] identifying the active effect
 * @fifo_empty_irq: IRQ number for the FIFO-empty interrupt
 * @play_request: true when a playback is requested
 * @pm_ref_held:  true while a pm_runtime_get is held
 * @irq_enabled:  true if fifo_empty_irq is enabled
 * @fifo_lock:    mutex protecting the FIFO streaming data
 * @fifo_data:    pointer of the data buffer for FIFO streaming
 * @data_len:     length of the data buffer for current effect
 * @data_written: number of samples written to the hardware FIFO
 * @data_done:    flag to indicate that all samples have been written
 * @effects:      table of the effects
 */
struct qcom_haptics {
	struct device *dev;
	struct regmap *regmap;
	struct input_dev *input;

	u32 cfg_base;
	u32 ptn_base;
	u32 t_lra_us;
	u32 vmax_mv;
	u32 fifo_len;
	u16 gain;

	struct work_struct play_work;
	struct mutex play_lock; /* mutex used to serialize playbacks */
	int cur_effect_id;
	int fifo_empty_irq;
	bool play_request;
	bool pm_ref_held;
	bool irq_enabled;

	struct mutex fifo_lock; /* protect the FIFO data during play */
	const s8 *fifo_data;
	u32 data_len;
	u32 data_written;
	bool data_done;

	struct qcom_haptics_effect effects[HAPTICS_MAX_EFFECTS];
};

static int cfg_write(struct qcom_haptics *h, u32 off, u32 val)
{
	return regmap_write(h->regmap, h->cfg_base + off, val);
}

static int cfg_update_bits(struct qcom_haptics *h, u32 off, u32 mask, u32 val)
{
	return regmap_update_bits(h->regmap, h->cfg_base + off, mask, val);
}

static int ptn_write(struct qcom_haptics *h, u32 off, u32 val)
{
	return regmap_write(h->regmap, h->ptn_base + off, val);
}

static int ptn_update_bits(struct qcom_haptics *h, u32 off, u32 mask, u32 val)
{
	return regmap_update_bits(h->regmap, h->ptn_base + off, mask, val);
}

static int ptn_bulk_write(struct qcom_haptics *h, u32 off,
			  const void *buf, size_t count)
{
	return regmap_bulk_write(h->regmap, h->ptn_base + off, buf, count);
}

static int haptics_clear_faults(struct qcom_haptics *h)
{
	return cfg_write(h, HAP_CFG_FAULT_CLR_REG, FAULT_CLR_ALL);
}

static int haptics_set_vmax(struct qcom_haptics *h, u32 vmax_mv)
{
	return cfg_write(h, HAP_CFG_VMAX_REG, vmax_mv / VMAX_STEP_MV);
}

static int haptics_config_lra_period(struct qcom_haptics *h)
{
	u32 tmp = h->t_lra_us / TLRA_STEP_US;
	int ret;

	ret = cfg_write(h, HAP_CFG_TLRA_OL_HIGH_REG, (tmp >> 8) & TLRA_OL_MSB_MASK);
	if (ret)
		return ret;

	return cfg_write(h, HAP_CFG_TLRA_OL_LOW_REG, tmp & 0xFF);
}

static int haptics_enable_module(struct qcom_haptics *h, bool enable)
{
	return cfg_update_bits(h, HAP_CFG_EN_CTL_REG, HAPTICS_EN_BIT,
			       enable ? HAPTICS_EN_BIT : 0);
}

static int haptics_configure_autores(struct qcom_haptics *h)
{
	int ret;

	/* AUTORES_CFG: enable, 10-cycle delay, 25% error window */
	ret = cfg_write(h, HAP_CFG_AUTORES_CFG_REG,
			AUTORES_EN_BIT |
			FIELD_PREP(AUTORES_EN_DLY_MASK, AUTORES_EN_DLY_CYCLES) |
			FIELD_PREP(AUTORES_ERR_WIN_MASK, AUTORES_ERR_WIN_25PCT));
	if (ret)
		return ret;

	/* DRV_DUTY: adaptive drive/brake duty cycles at 62.5% */
	ret = cfg_write(h, HAP_CFG_DRV_DUTY_CFG_REG,
			ADT_DRV_DUTY_EN_BIT | ADT_BRK_DUTY_EN_BIT |
			FIELD_PREP(DRV_DUTY_MASK, AUTORES_DRV_DUTY_62P5) |
			FIELD_PREP(BRK_DUTY_MASK, AUTORES_BRK_DUTY_62P5));
	if (ret)
		return ret;

	/* Pre-HIZ delay: 10 µs */
	ret = cfg_write(h, HAP_CFG_RAMP_DN_CFG2_REG, AUTORES_PRE_HIZ_DLY_10US);
	if (ret)
		return ret;

	/* Zero-cross window: debounce 3, no hysteresis, height 2 */
	return cfg_write(h, HAP_CFG_ZX_WIND_CFG_REG,
			 FIELD_PREP(ZX_DEBOUNCE_MASK, AUTORES_ZX_DEBOUNCE) |
			 FIELD_PREP(ZX_WIN_HEIGHT_MASK, AUTORES_ZX_WIN_HEIGHT));
}

static int haptics_write_fifo_chunk(struct qcom_haptics *h,
				    const s8 *data, u32 len)
{
	u32 bulk_len = ALIGN_DOWN(len, 4);
	int i, ret;

	/*
	 * FIFO data writing supports both 4-byte bulk writes using registers
	 * [HAP_PTN_FIFO_DIN_0_REG ... HAP_PTN_FIFO_DIN_3_REG], and 1-byte writes
	 * using the HAP_PTN_FIFO_DIN_1B_REG register. The 4-byte bulk write is more
	 * efficient, so use 4-byte writes for the initial 4-byte aligned data,
	 * and 1-byte writes for any trailing remainder.
	 */
	for (i = 0; i < bulk_len; i += 4) {
		ret = ptn_bulk_write(h, HAP_PTN_FIFO_DIN_0_REG, &data[i], 4);
		if (ret)
			return ret;
	}

	for (; i < len; i++) {
		ret = ptn_write(h, HAP_PTN_FIFO_DIN_1B_REG, (u8)data[i]);
		if (ret)
			return ret;
	}

	return 0;
}

/*
 * Configure the hardware FIFO memory boundary.
 * FIFO occupies addresses [0, fifo_len).
 */
static int haptics_configure_fifo_mmap(struct qcom_haptics *h)
{
	u32 fifo_len, fifo_units;

	/* Config all memory space for FIFO usage for now */
	fifo_len = HAP530_MEM_TOTAL_BYTES;
	fifo_len = ALIGN_DOWN(fifo_len, 64);
	fifo_units = fifo_len / 64;
	h->fifo_len = fifo_len;

	return ptn_write(h, HAP_PTN_MMAP_FIFO_REG,
			 MMAP_FIFO_EXIST_BIT |
			 FIELD_PREP(MMAP_FIFO_LEN_MASK, fifo_units - 1));
}

static u32 haptics_gain_scaled_vmax(struct qcom_haptics *h, u32 vmax_mv)
{
	u32 v = mult_frac(vmax_mv, h->gain, 0xFFFF);

	return max_t(u32, v, VMAX_STEP_MV);
}

static void haptics_fifo_irq_enable(struct qcom_haptics *h, bool enable)
{
	if (h->irq_enabled == enable)
		return;

	if (enable)
		enable_irq(h->fifo_empty_irq);
	else
		disable_irq(h->fifo_empty_irq);

	h->irq_enabled = enable;
}

/*
 * Must be called with play_lock held.
 * Clears PLAY_EN and resets any FIFO-specific state.
 */
static void haptics_stop_locked(struct qcom_haptics *h)
{
	int id = h->cur_effect_id;

	cfg_write(h, HAP_CFG_SPMI_PLAY_REG, 0);

	if (h->effects[id].mode == HAPTICS_FIFO) {
		ptn_write(h, HAP_PTN_FIFO_EMPTY_CFG_REG, 0);
		haptics_fifo_irq_enable(h, false);
		mutex_lock(&h->fifo_lock);
		h->fifo_data = NULL;
		mutex_unlock(&h->fifo_lock);
	}
}

static int haptics_start_direct_play(struct qcom_haptics *h, int effect_id)
{
	struct ff_effect *ffe = &h->input->ff->effects[effect_id];
	u8 amplitude = (u8)mult_frac((u32)abs(ffe->u.constant.level), 255, 0x7FFF);
	int ret;

	ret = haptics_clear_faults(h);
	if (ret)
		return ret;

	/* Enable auto-resonance for DIRECT_PLAY mode */
	ret = cfg_update_bits(h, HAP_CFG_AUTORES_CFG_REG,
			      AUTORES_EN_BIT, AUTORES_EN_BIT);
	if (ret)
		return ret;

	ret = haptics_set_vmax(h, haptics_gain_scaled_vmax(h, h->vmax_mv));
	if (ret)
		return ret;

	ret = ptn_write(h, HAP_PTN_DIRECT_PLAY_REG, amplitude);
	if (ret)
		return ret;

	return cfg_write(h, HAP_CFG_SPMI_PLAY_REG,
			 PLAY_EN_BIT | FIELD_PREP(PAT_SRC_MASK, PAT_SRC_DIRECT_PLAY));
}

static int haptics_start_fifo(struct qcom_haptics *h, int effect_id)
{
	struct qcom_haptics_effect *eff = &h->effects[effect_id];
	u32 vmax = eff->vmax_mv ? eff->vmax_mv : h->vmax_mv;
	u32 init_len;
	int ret;

	ret = haptics_clear_faults(h);
	if (ret)
		return ret;

	/* Disable auto-resonance for FIFO mode */
	ret = cfg_update_bits(h, HAP_CFG_AUTORES_CFG_REG, AUTORES_EN_BIT, 0);
	if (ret)
		return ret;

	ret = haptics_set_vmax(h, haptics_gain_scaled_vmax(h, vmax));
	if (ret)
		return ret;

	ret = ptn_update_bits(h, HAP_PTN_FIFO_PLAY_RATE_REG,
			      FIFO_PLAY_RATE_MASK,
			      FIELD_PREP(FIFO_PLAY_RATE_MASK, eff->play_rate));
	if (ret)
		return ret;

	/* Flush FIFO before loading new data */
	ret = ptn_write(h, HAP_PTN_MEM_OP_ACCESS_REG, MEM_FLUSH_RELOAD_BIT);
	if (ret)
		return ret;
	ret = ptn_write(h, HAP_PTN_MEM_OP_ACCESS_REG, 0);
	if (ret)
		return ret;

	/* Write the initial chunk and initialise streaming state */
	init_len = min_t(u32, eff->data_len, FIFO_INIT_FILL);
	ret = haptics_write_fifo_chunk(h, eff->fifo_data, init_len);
	if (ret)
		return ret;

	mutex_lock(&h->fifo_lock);
	h->fifo_data    = eff->fifo_data;
	h->data_len     = eff->data_len;
	h->data_written = init_len;
	h->data_done    = (init_len >= eff->data_len);
	mutex_unlock(&h->fifo_lock);

	/*
	 * Set empty threshold.  When threshold > 0 the hardware fires the
	 * FIFO-empty interrupt when occupancy drops below the threshold,
	 * allowing the driver to refill.  A threshold of 0 disables the IRQ.
	 */
	ret = ptn_write(h, HAP_PTN_FIFO_EMPTY_CFG_REG, h->data_done ? 0 :
			FIFO_EMPTY_THRESH / FIFO_THRESH_LSB);
	if (ret)
		return ret;
	if (!h->data_done)
		haptics_fifo_irq_enable(h, true);

	return cfg_write(h, HAP_CFG_SPMI_PLAY_REG,
			 PLAY_EN_BIT | FIELD_PREP(PAT_SRC_MASK, PAT_SRC_FIFO));
}

/*
 * Threaded IRQ handler for the FIFO-empty interrupt.
 *
 * While a FIFO play is in progress the hardware fires this interrupt when
 * the number of samples in the FIFO drops below the programmed threshold.
 * The handler refills the FIFO from the effect's data buffer.  When all
 * samples have been written the threshold is set to zero, which suppresses
 * further interrupts; the hardware drains the remaining samples naturally
 * and the play work handler stops the engine on the next invocation.
 */
static irqreturn_t haptics_fifo_empty_irq(int irq, void *dev_id)
{
	struct qcom_haptics *h = dev_id;
	u32 sts, to_write;
	int ret;

	ret = regmap_read(h->regmap,
			  h->cfg_base + HAP_CFG_INT_RT_STS_REG, &sts);
	if (ret || !(sts & FIFO_EMPTY_BIT))
		return IRQ_HANDLED;

	mutex_lock(&h->fifo_lock);

	if (!h->fifo_data) {
		mutex_unlock(&h->fifo_lock);
		return IRQ_HANDLED;
	}

	if (h->data_done) {
		ptn_write(h, HAP_PTN_FIFO_EMPTY_CFG_REG, 0);
		h->fifo_data = NULL;
		h->play_request = false;
		schedule_work(&h->play_work);
		mutex_unlock(&h->fifo_lock);
		return IRQ_HANDLED;
	}

	/* Refill: write the next chunk, conservatively sized to the threshold */
	to_write = min_t(u32, h->data_len - h->data_written,
			 h->fifo_len - FIFO_EMPTY_THRESH);
	haptics_write_fifo_chunk(h, &h->fifo_data[h->data_written], to_write);
	h->data_written += to_write;

	if (h->data_written >= h->data_len) {
		/* Last chunk enqueued; disable threshold to stop further IRQs */
		h->data_done = true;
		ptn_write(h, HAP_PTN_FIFO_EMPTY_CFG_REG, 0);
	}

	mutex_unlock(&h->fifo_lock);
	return IRQ_HANDLED;
}

static void haptics_play_work(struct work_struct *work)
{
	struct qcom_haptics *h = container_of(work, struct qcom_haptics, play_work);
	int id, ret;

	mutex_lock(&h->play_lock);

	if (!h->play_request) {
		haptics_stop_locked(h);
		if (h->pm_ref_held) {
			pm_runtime_put_autosuspend(h->dev);
			h->pm_ref_held = false;
		}
		goto unlock;
	}

	if (!h->pm_ref_held) {
		ret = pm_runtime_resume_and_get(h->dev);
		if (ret < 0) {
			dev_err(h->dev, "failed to resume device: %d\n", ret);
			goto unlock;
		}
		h->pm_ref_held = true;
	}

	id = h->cur_effect_id;
	switch (h->effects[id].mode) {
	case HAPTICS_DIRECT_PLAY:
		ret = haptics_start_direct_play(h, id);
		break;
	case HAPTICS_FIFO:
		ret = haptics_start_fifo(h, id);
		break;
	default:
		ret = -EINVAL;
	}

	if (ret) {
		dev_err(h->dev, "failed to start effect %d: %d\n", id, ret);
		if (h->pm_ref_held) {
			pm_runtime_put_autosuspend(h->dev);
			h->pm_ref_held = false;
		}
	}

unlock:
	mutex_unlock(&h->play_lock);
}

static int haptics_upload_effect(struct input_dev *dev,
				 struct ff_effect *effect,
				 struct ff_effect *old)
{
	struct qcom_haptics *h = input_get_drvdata(dev);
	struct qcom_haptics_effect *priv;
	int id = effect->id;
	u32 data_len;
	s16 *buf;
	s8 *fifo;

	if (id < 0 || id >= HAPTICS_MAX_EFFECTS)
		return -EINVAL;

	priv = &h->effects[id];

	switch (effect->type) {
	case FF_CONSTANT:
		kfree(priv->fifo_data);
		priv->fifo_data = NULL;
		priv->data_len  = 0;
		priv->mode = HAPTICS_DIRECT_PLAY;
		return 0;

	case FF_PERIODIC:
		if (effect->u.periodic.waveform != FF_CUSTOM)
			return -EINVAL;
		/*
		 * Minimum 3 elements: play-rate code + vmax + at least one sample.
		 * No upper bound: the FIFO is refilled continuously from the IRQ
		 * handler, so any length of PCM data is supported.
		 */
		if (effect->u.periodic.custom_len < 3)
			return -EINVAL;

		buf = memdup_array_user(effect->u.periodic.custom_data,
					effect->u.periodic.custom_len,
					sizeof(s16));
		if (IS_ERR(buf))
			return PTR_ERR(buf);

		if (buf[CUSTOM_DATA_RATE_IDX] > PLAY_RATE_MAX) {
			kfree(buf);
			return -EINVAL;
		}

		data_len = effect->u.periodic.custom_len - CUSTOM_DATA_SAMPLE_START;
		fifo = kmalloc(data_len, GFP_KERNEL);
		if (!fifo) {
			kfree(buf);
			return -ENOMEM;
		}

		/* Pack: one s8 sample per s16 slot (lower byte) */
		for (int i = 0; i < data_len; i++)
			fifo[i] = (s8)buf[CUSTOM_DATA_SAMPLE_START + i];

		kfree(priv->fifo_data);
		priv->fifo_data = fifo;
		priv->data_len  = data_len;
		priv->play_rate = (u8)buf[CUSTOM_DATA_RATE_IDX];
		priv->vmax_mv   = (u32)clamp_val(buf[CUSTOM_DATA_VMAX_IDX], 0, VMAX_MV_MAX);
		priv->mode = HAPTICS_FIFO;

		kfree(buf);
		return 0;

	default:
		return -EINVAL;
	}
}

static int haptics_playback(struct input_dev *dev, int effect_id, int val)
{
	struct qcom_haptics *h = input_get_drvdata(dev);

	h->cur_effect_id = effect_id;
	h->play_request  = (val > 0);
	schedule_work(&h->play_work);

	return 0;
}

static int haptics_erase(struct input_dev *dev, int effect_id)
{
	struct qcom_haptics *h = input_get_drvdata(dev);
	struct qcom_haptics_effect *priv = &h->effects[effect_id];

	kfree(priv->fifo_data);
	priv->fifo_data = NULL;
	priv->data_len  = 0;

	return 0;
}

static void haptics_set_gain(struct input_dev *dev, u16 gain)
{
	struct qcom_haptics *h = input_get_drvdata(dev);

	h->gain = gain;
}

static int qcom_haptics_probe(struct platform_device *pdev)
{
	struct qcom_haptics *h;
	struct input_dev *input;
	struct ff_device *ff;
	u32 regs[2], vmax_uv;
	int ret, irq;

	h = devm_kzalloc(&pdev->dev, sizeof(*h), GFP_KERNEL);
	if (!h)
		return -ENOMEM;

	h->dev = &pdev->dev;

	h->regmap = dev_get_regmap(pdev->dev.parent, NULL);
	if (!h->regmap)
		return dev_err_probe(h->dev, -ENODEV, "no regmap from parent\n");

	ret = device_property_read_u32_array(h->dev, "reg", regs, ARRAY_SIZE(regs));
	if (ret)
		return dev_err_probe(h->dev, ret, "failed to read 'reg' property\n");

	h->cfg_base = regs[0];
	h->ptn_base = regs[1];

	ret = device_property_read_u32(h->dev, "qcom,lra-period-us", &h->t_lra_us);
	if (ret)
		return dev_err_probe(h->dev, ret, "missing qcom,lra-period-us\n");

	ret = device_property_read_u32(h->dev, "qcom,vmax-microvolt", &vmax_uv);
	if (ret)
		return dev_err_probe(h->dev, ret, "missing qcom,vmax-microvolt\n");

	h->vmax_mv = clamp(vmax_uv / 1000, (u32)VMAX_STEP_MV, (u32)VMAX_MV_MAX);

	ret = haptics_config_lra_period(h);
	if (ret)
		return ret;

	ret = haptics_configure_autores(h);
	if (ret)
		return ret;

	ret = haptics_set_vmax(h, h->vmax_mv);
	if (ret)
		return ret;

	ret = haptics_configure_fifo_mmap(h);
	if (ret)
		return ret;

	mutex_init(&h->play_lock);
	mutex_init(&h->fifo_lock);
	INIT_WORK(&h->play_work, haptics_play_work);
	h->gain = 0xFFFF;

	irq = platform_get_irq_byname(pdev, "fifo-empty");
	if (irq < 0)
		return dev_err_probe(h->dev, irq, "failed to get fifo-empty IRQ\n");

	ret = devm_request_threaded_irq(h->dev, irq, NULL,
					haptics_fifo_empty_irq,
					IRQF_ONESHOT,
					"qcom-haptics-fifo-empty", h);
	if (ret)
		return dev_err_probe(h->dev, ret, "failed to request fifo-empty IRQ\n");

	h->fifo_empty_irq = irq;
	disable_irq_nosync(irq);

	input = devm_input_allocate_device(h->dev);
	if (!input)
		return -ENOMEM;

	input->name = "qcom-spmi-haptics";
	input_set_drvdata(input, h);
	h->input = input;

	input_set_capability(input, EV_FF, FF_CONSTANT);
	input_set_capability(input, EV_FF, FF_PERIODIC);
	input_set_capability(input, EV_FF, FF_CUSTOM);
	input_set_capability(input, EV_FF, FF_GAIN);

	ret = input_ff_create(input, HAPTICS_MAX_EFFECTS);
	if (ret)
		return ret;

	ff = input->ff;
	ff->upload   = haptics_upload_effect;
	ff->playback = haptics_playback;
	ff->erase    = haptics_erase;
	ff->set_gain = haptics_set_gain;

	pm_runtime_get_noresume(h->dev);
	pm_runtime_use_autosuspend(h->dev);
	pm_runtime_set_autosuspend_delay(h->dev, HAPTICS_AUTOSUSPEND_MS);
	devm_pm_runtime_set_active_enabled(h->dev);
	pm_runtime_put_autosuspend(h->dev);

	ret = input_register_device(input);
	if (ret) {
		input_ff_destroy(input);
		return dev_err_probe(h->dev, ret, "failed to register input device\n");
	}

	platform_set_drvdata(pdev, h);

	return 0;
}

static void qcom_haptics_remove(struct platform_device *pdev)
{
	struct qcom_haptics *h = platform_get_drvdata(pdev);
	int i;

	pm_runtime_disable(&pdev->dev);
	pm_runtime_set_suspended(&pdev->dev);

	mutex_lock(&h->play_lock);
	haptics_stop_locked(h);
	mutex_unlock(&h->play_lock);

	haptics_enable_module(h, false);
	cancel_work_sync(&h->play_work);
	for (i = 0; i < HAPTICS_MAX_EFFECTS; i++) {
		kfree(h->effects[i].fifo_data);
		h->effects[i].fifo_data = NULL;
	}

	input_unregister_device(h->input);
}

static int qcom_haptics_runtime_suspend(struct device *dev)
{
	struct qcom_haptics *h = dev_get_drvdata(dev);

	return haptics_enable_module(h, false);
}

static int qcom_haptics_runtime_resume(struct device *dev)
{
	struct qcom_haptics *h = dev_get_drvdata(dev);

	return haptics_enable_module(h, true);
}

static int qcom_haptics_suspend(struct device *dev)
{
	struct qcom_haptics *h = dev_get_drvdata(dev);

	mutex_lock(&h->play_lock);
	haptics_stop_locked(h);
	if (h->pm_ref_held) {
		pm_runtime_put_noidle(dev);
		h->pm_ref_held = false;
	}
	mutex_unlock(&h->play_lock);

	cancel_work_sync(&h->play_work);
	return pm_runtime_force_suspend(dev);
}

static int qcom_haptics_resume(struct device *dev)
{
	return pm_runtime_force_resume(dev);
}

static const struct dev_pm_ops qcom_haptics_pm_ops = {
	SYSTEM_SLEEP_PM_OPS(qcom_haptics_suspend, qcom_haptics_resume)
	RUNTIME_PM_OPS(qcom_haptics_runtime_suspend, qcom_haptics_runtime_resume,
		       NULL)
};

static const struct of_device_id qcom_haptics_of_match[] = {
	{ .compatible = "qcom,spmi-haptics" },
	{}
};
MODULE_DEVICE_TABLE(of, qcom_haptics_of_match);

static struct platform_driver qcom_haptics_driver = {
	.probe  = qcom_haptics_probe,
	.remove = qcom_haptics_remove,
	.driver = {
		.name		= "qcom-spmi-haptics",
		.of_match_table	= qcom_haptics_of_match,
		.pm		= pm_ptr(&qcom_haptics_pm_ops),
	},
};
module_platform_driver(qcom_haptics_driver);

MODULE_DESCRIPTION("Qualcomm SPMI PMIC Haptics driver");
MODULE_LICENSE("GPL");

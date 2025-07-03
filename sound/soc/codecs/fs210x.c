// SPDX-License-Identifier: GPL-2.0
/*
 * fs210x.c -- Driver for the FS2104/5S Audio Amplifier
 *
 * Copyright (C) 2016-2025 Shanghai FourSemi Semiconductor Co.,Ltd.
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_gpio.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>
#include <linux/workqueue.h>
#include <sound/soc.h>
#include <sound/tlv.h>

#include "fs210x.h"
#include "fs-amp-lib.h"

#define FS210X_DRV_VERSION		"v1.0.6"
#define FS210X_DEFAULT_FWM_NAME		"fs210x_fwm.bin"
#define FS210X_DEFAULT_DAI_NAME		"fs210x-aif"
#define FS2105S_DEVICE_ID		0x20 /* FS2105S */
#define FS210X_DEVICE_ID		0x45 /* FS2104 */
#define FS210X_REG_MAX			0xF8
#define FS210X_VOLUME_MIN		0
#define FS210X_VOLUME_MAX		511
#define FS210X_INIT_SCENE		0
#define FS210X_DEFAULT_SCENE		1
#define FS210X_START_DELAY_MS		5
#define FS210X_FAULT_CHECK_INTERVAL_MS	2000
#define FS2105S_RATES			(SNDRV_PCM_RATE_32000 | \
					 SNDRV_PCM_RATE_44100 | \
					 SNDRV_PCM_RATE_48000 | \
					 SNDRV_PCM_RATE_88200 | \
					 SNDRV_PCM_RATE_96000)
#define FS210X_RATES			(SNDRV_PCM_RATE_16000 | FS2105S_RATES)
#define FS210X_FORMATS			(SNDRV_PCM_FMTBIT_S16_LE | \
					 SNDRV_PCM_FMTBIT_S24_LE | \
					 SNDRV_PCM_FMTBIT_S24_3LE | \
					 SNDRV_PCM_FMTBIT_S32_LE)
#define FS210X_NUM_SUPPLIES		ARRAY_SIZE(fs210x_supply_names)

static const char *const fs210x_supply_names[] = {
	"pvdd",
	"dvdd",
};

struct fs210x_platform_data {
	const char *fwm_name;
	const char *dai_name;
};

struct fs210x_priv {
	struct i2c_client *i2c;
	struct device *dev;
	struct regmap *regmap;
	struct fs210x_platform_data pdata;
	struct regulator_bulk_data supplies[FS210X_NUM_SUPPLIES];
	struct gpio_desc *gpio_sdz;
	struct delayed_work start_work;
	struct delayed_work fault_check_work;
	struct fs_amp_lib amp_lib;
	const struct fs_amp_scene *cur_scene;
	struct clk *clk_bclk;
	unsigned int bclk;
	unsigned int srate;
	int scene_id;
	u16 devid;
	u16 vol[2]; /* L/R channels volume */
	bool is_inited;
	bool is_suspended;
	bool is_bclk_on;
	bool is_playing;
};

static DEFINE_MUTEX(fs210x_mutex);

static const struct fs_pll_div fs210x_pll_div[] = {
	/*    bclk,   pll1,   pll2,   pll3 */
	{   512000, 0x006C, 0x0120, 0x0001 },
	{   768000, 0x016C, 0x00C0, 0x0001 },
	{  1024000, 0x016C, 0x0090, 0x0001 },
	{  1536000, 0x016C, 0x0060, 0x0001 },
	{  2048000, 0x016C, 0x0090, 0x0002 },
	{  2304000, 0x016C, 0x0080, 0x0002 },
	{  3072000, 0x016C, 0x0090, 0x0003 },
	{  4096000, 0x016C, 0x0090, 0x0004 },
	{  4608000, 0x016C, 0x0080, 0x0004 },
	{  6144000, 0x016C, 0x0090, 0x0006 },
	{  8192000, 0x016C, 0x0090, 0x0008 },
	{  9216000, 0x016C, 0x0090, 0x0009 },
	{ 12288000, 0x016C, 0x0090, 0x000C },
	{ 16384000, 0x016C, 0x0090, 0x0010 },
	{ 18432000, 0x016C, 0x0090, 0x0012 },
	{ 24576000, 0x016C, 0x0090, 0x0018 },
	{  1411200, 0x016C, 0x0060, 0x0001 },
	{  2116800, 0x016C, 0x0080, 0x0002 },
	{  2822400, 0x016C, 0x0090, 0x0003 },
	{  4233600, 0x016C, 0x0080, 0x0004 },
	{  5644800, 0x016C, 0x0090, 0x0006 },
	{  8467200, 0x016C, 0x0090, 0x0009 },
	{ 11289600, 0x016C, 0x0090, 0x000C },
	{ 16934400, 0x016C, 0x0090, 0x0012 },
	{ 22579200, 0x016C, 0x0090, 0x0018 },
	{  2000000, 0x017C, 0x0093, 0x0002 },
};

static int fs210x_bclk_set(struct fs210x_priv *fs210x, bool on)
{
	int ret = 0;

	if (!fs210x || !fs210x->dev)
		return -EINVAL;

	if (!fs210x->clk_bclk)
		return 0;

	if ((fs210x->is_bclk_on ^ on) == 0)
		return 0;

	dev_dbg(fs210x->dev, "bclk switch %s\n", on ? "on" : "off");

	if (on) {
		clk_set_rate(fs210x->clk_bclk, fs210x->bclk);
		ret = clk_prepare_enable(fs210x->clk_bclk);
		fs210x->is_bclk_on = true;
		usleep_range(2000, 2050); /* >= 2ms */
	} else {
		clk_disable_unprepare(fs210x->clk_bclk);
		fs210x->is_bclk_on = false;
	}

	return ret;
}

static int fs210x_reg_write(struct fs210x_priv *fs210x,
			    u8 reg, u16 val)
{
	int ret;

	ret = regmap_write(fs210x->regmap, reg, val);
	if (ret) {
		dev_err(fs210x->dev, "Failed to write %02Xh: %d\n", reg, ret);
		return ret;
	}

	dev_dbg(fs210x->dev, "RW: %02x %04x\n", reg, val);

	return 0;
}

static int fs210x_reg_read(struct fs210x_priv *fs210x,
			   u8 reg, u16 *pval)
{
	unsigned int val;
	int ret;

	ret = regmap_read(fs210x->regmap, reg, &val);
	if (ret) {
		dev_err(fs210x->dev, "Failed to read %02Xh: %d\n", reg, ret);
		return ret;
	}

	if (pval)
		*pval = (u16)val;

	dev_dbg(fs210x->dev, "RR: %02x %04x\n", reg, val);

	return 0;
}

static int fs210x_reg_update_bits(struct fs210x_priv *fs210x,
				  u8 reg, u16 mask, u16 val)
{
	int ret;

	ret = regmap_update_bits(fs210x->regmap, reg, mask, val);
	if (ret) {
		dev_err(fs210x->dev, "Failed to update %02Xh: %d\n", reg, ret);
		return ret;
	}

	dev_dbg(fs210x->dev, "RU: %02x %04x %04x\n", reg, mask, val);

	return 0;
}

static int fs210x_reg_bulk_write(struct fs210x_priv *fs210x,
				 u8 reg, const void *val, u32 size)
{
	int ret;

	ret = regmap_bulk_write(fs210x->regmap, reg, val, size / 2);
	if (ret) {
		dev_err(fs210x->dev, "Failed to bulk write %02Xh: %d\n",
			reg, ret);
		return ret;
	}

	dev_dbg(fs210x->dev, "BW: %02x %d\n", reg, size);

	return 0;
}

static int fs210x_reg_dump(struct fs210x_priv *fs210x)
{
	u16 val[8]; /* 8 registers per line */
	u8 idx, reg;
	int ret;

	if (!fs210x || !fs210x->dev)
		return -EINVAL;

#define FS210X_REG_DUMP_MAX 0xCF
	for (reg = 0x00; reg <= FS210X_REG_DUMP_MAX; reg++) {
		/* Print 8 register values per line */
		idx = (reg & 0x7);
		ret = fs210x_reg_read(fs210x, reg, val + idx);
		if (ret)
			break;
		if (idx != 0x7 && reg != FS210X_REG_DUMP_MAX)
			continue;
		dev_info(fs210x->dev,
			 "%02Xh: %04x %04x %04x %04x %04x %04x %04x %04x\n",
			 (reg & 0xF8), val[0], val[1], val[2],
			 val[3], val[4], val[5], val[6], val[7]);
	}

	return ret;
}

static inline int fs210x_write_reg_val(struct fs210x_priv *fs210x,
				       const struct fs_reg_val *regv)
{
	return fs210x_reg_write(fs210x, regv->reg, regv->val);
}

static inline int fs210x_write_reg_bits(struct fs210x_priv *fs210x,
					const struct fs_reg_bits *regu)
{
	return fs210x_reg_update_bits(fs210x,
				      regu->reg,
				      regu->mask,
				      regu->val);
}

static inline int fs210x_set_cmd_pkg(struct fs210x_priv *fs210x,
				     const struct fs_cmd_pkg *pkg,
				     unsigned int *offset)
{
	int delay_us;

	if (pkg->cmd >= 0x00 && pkg->cmd <= FS210X_REG_MAX) {
		*offset = sizeof(pkg->regv);
		return fs210x_write_reg_val(fs210x, &pkg->regv);
	} else if (pkg->cmd == FS_CMD_UPDATE) {
		*offset = sizeof(pkg->regb);
		return fs210x_write_reg_bits(fs210x, &pkg->regb);
	} else if (pkg->cmd == FS_CMD_DELAY) {
		if (pkg->regv.val > FS_CMD_DELAY_MS_MAX)
			return -ENOTSUPP;
		delay_us = pkg->regv.val * 1000;
		usleep_range(delay_us, delay_us + 50);
		*offset = sizeof(pkg->regv);
		return 0;
	}

	dev_err(fs210x->dev, "Invalid pkg cmd: %d\n", pkg->cmd);

	return -ENOTSUPP;
}

static int fs210x_reg_write_table(struct fs210x_priv *fs210x,
				  const struct fs_reg_table *reg)
{
	const struct fs_cmd_pkg *pkg;
	unsigned int index, offset;
	int ret;

	if (!fs210x || !fs210x->dev)
		return -EINVAL;

	if (!reg || reg->size == 0)
		return -EFAULT;

	for (index = 0; index < reg->size; index += offset) {
		pkg = (struct fs_cmd_pkg *)(reg->buf + index);
		ret = fs210x_set_cmd_pkg(fs210x, pkg, &offset);
		if (ret) {
			dev_err(fs210x->dev, "Failed to set cmd pkg: %02X-%d\n",
				pkg->cmd, ret);
			return ret;
		}
	}

	if (index != reg->size) {
		dev_err(fs210x->dev, "Invalid reg table size: %d-%d\n",
			index, reg->size);
		return -EFAULT;
	}

	return 0;
}

static int fs210x_dev_play(struct fs210x_priv *fs210x)
{
	int ret;

	if (!fs210x->is_inited)
		return -EFAULT;

	if (fs210x->is_playing)
		return 0;

	ret = fs210x_reg_write(fs210x, FS210X_11H_SYSCTRL,
			       FS210X_11H_DPS_PLAY);
	if (!ret)
		fs210x->is_playing = true;

	usleep_range(10000, 10050); /* >= 10ms */

	return ret;
}

static int fs210x_dev_stop(struct fs210x_priv *fs210x)
{
	int ret;

	if (!fs210x->is_inited)
		return -EFAULT;

	if (!fs210x->is_playing)
		return 0;

	ret = fs210x_reg_write(fs210x, FS210X_11H_SYSCTRL,
			       FS210X_11H_DPS_PWDN);
	fs210x->is_playing = false;

	usleep_range(30000, 30050); /* >= 30ms */

	return ret;
}

static int fs210x_set_reg_table(struct fs210x_priv *fs210x,
				const struct fs_amp_scene *scene)
{
	const struct fs_amp_scene *cur_scene;
	const struct fs_reg_table *reg;

	if (!fs210x || !fs210x->dev || !scene)
		return -EINVAL;

	cur_scene = fs210x->cur_scene;
	if (!scene->reg || cur_scene == scene) {
		dev_dbg(fs210x->dev, "Skip writing reg table\n");
		return 0;
	}

	reg = scene->reg;
	dev_dbg(fs210x->dev, "reg table size: %d\n", reg->size);

	return fs210x_reg_write_table(fs210x, reg);
}

static int fs210x_set_woofer_table(struct fs210x_priv *fs210x)
{
	const struct fs_file_table *woofer;
	const struct fs_fwm_table *table;
	int ret;

	if (!fs210x || !fs210x->dev)
		return -EINVAL;

	/* NOTE: fs2105s has woofer ram only */
	if (fs210x->devid != FS2105S_DEVICE_ID)
		return 0;

	table = fs210x->amp_lib.table[FS_INDEX_WOOFER];
	if (!table) {
		dev_dbg(fs210x->dev, "Skip writing woofer table\n");
		return 0;
	}

	woofer = (struct fs_file_table *)table->buf;
	dev_dbg(fs210x->dev, "woofer table size: %d\n", woofer->size);
	/* Unit of woofer data is u32(4 bytes) */
	if (woofer->size == 0 || (woofer->size & 0x3)) {
		dev_err(fs210x->dev, "Invalid woofer size: %d\n",
			woofer->size);
		return -EINVAL;
	}

	ret = fs210x_reg_write(fs210x, FS210X_46H_DACEQA,
			       FS2105S_46H_CAM_BURST_W);
	ret |= fs210x_reg_bulk_write(fs210x, FS210X_42H_DACEQWL,
				     woofer->buf, woofer->size);

	return ret;
}

static int fs210x_set_effect_table(struct fs210x_priv *fs210x,
				   const struct fs_amp_scene *scene)
{
	const struct fs_amp_scene *cur_scene;
	const struct fs_file_table *effect;
	int half_size;
	int ret;

	if (!fs210x || !fs210x->dev || !scene)
		return -EINVAL;

	cur_scene = fs210x->cur_scene;
	if (!scene->effect || cur_scene == scene) {
		dev_dbg(fs210x->dev, "Skip writing effect table\n");
		return 0;
	}

	effect = scene->effect;
	dev_dbg(fs210x->dev, "effect table size: %d\n", effect->size);

	/* Unit of effect data is u32(4 bytes), 2 channels */
	if (effect->size == 0 || (effect->size & 0x7)) {
		dev_err(fs210x->dev, "Invalid effect size: %d\n",
			effect->size);
		return -EINVAL;
	}

	half_size = effect->size / 2;

	/* Left channel */
	ret = fs210x_reg_write(fs210x, FS210X_46H_DACEQA,
			       FS210X_46H_CAM_BURST_L);
	ret |= fs210x_reg_bulk_write(fs210x, FS210X_42H_DACEQWL,
				     effect->buf, half_size);
	if (ret)
		return ret;

	/* Right channel */
	ret = fs210x_reg_write(fs210x, FS210X_46H_DACEQA,
			       FS210X_46H_CAM_BURST_R);
	ret |= fs210x_reg_bulk_write(fs210x, FS210X_42H_DACEQWL,
				     effect->buf + half_size, half_size);

	return ret;
}

static int fs210x_access_dsp_ram(struct fs210x_priv *fs210x, bool enable)
{
	int ret;

	if (!fs210x || !fs210x->dev)
		return -EINVAL;

	if (enable) {
		ret = fs210x_reg_write(fs210x, FS210X_11H_SYSCTRL,
				       FS210X_11H_DPS_HIZ);
		ret |= fs210x_reg_write(fs210x, FS210X_0BH_ACCKEY,
					FS210X_0BH_ACCKEY_ON);
	} else {
		ret = fs210x_reg_write(fs210x, FS210X_0BH_ACCKEY,
				       FS210X_0BH_ACCKEY_OFF);
		ret |= fs210x_reg_write(fs210x, FS210X_11H_SYSCTRL,
					FS210X_11H_DPS_PWDN);
	}

	usleep_range(10000, 10050); /* >= 10ms */

	return ret;
}

static int fs210x_write_dsp_effect(struct fs210x_priv *fs210x,
				   const struct fs_amp_scene *scene,
				   int scene_id)
{
	int ret;

	if (!fs210x || !scene)
		return -EINVAL;

	ret = fs210x_access_dsp_ram(fs210x, true);
	if (ret) {
		dev_err(fs210x->dev, "Failed to access dsp: %d\n", ret);
		goto tag_exit;
	}

	ret = fs210x_set_effect_table(fs210x, scene);
	if (ret) {
		dev_err(fs210x->dev, "Failed to set effect: %d\n", ret);
		goto tag_exit;
	}

	if (scene_id == FS210X_INIT_SCENE)
		ret = fs210x_set_woofer_table(fs210x);

tag_exit:
	fs210x_reg_write(fs210x, FS210X_46H_DACEQA,
			 FS210X_46H_CAM_CLEAR);
	fs210x_access_dsp_ram(fs210x, false);

	return ret;
}

static int fs210x_check_scene(struct fs210x_priv *fs210x,
			      int scene_id, bool *skip_set)
{
	struct fs_amp_lib *amp_lib;

	if (!fs210x || !skip_set)
		return -EINVAL;

	amp_lib = &fs210x->amp_lib;
	if (amp_lib->scene_count == 0 || !amp_lib->scene) {
		dev_err(fs210x->dev, "There's no scene data\n");
		return -EINVAL;
	}

	if (scene_id < 0 || scene_id >= amp_lib->scene_count) {
		dev_err(fs210x->dev, "Invalid scene_id: %d\n", scene_id);
		return -EINVAL;
	}

	if (fs210x->scene_id == scene_id) {
		dev_dbg(fs210x->dev, "Skip to set same scene\n");
		return 0;
	}

	*skip_set = false;

	return 0;
}

static int fs210x_set_scene(struct fs210x_priv *fs210x, int scene_id)
{
	const struct fs_amp_scene *scene;
	bool skip_set = true;
	bool is_playing;
	int ret;

	if (!fs210x || !fs210x->dev)
		return -EINVAL;

	ret = fs210x_check_scene(fs210x, scene_id, &skip_set);
	if (ret || skip_set)
		return ret;

	scene = fs210x->amp_lib.scene + scene_id;
	dev_info(fs210x->dev, "Switch scene.%d: %s\n",
		 scene_id, scene->name);

	is_playing = fs210x->is_playing;
	if (is_playing)
		fs210x_dev_stop(fs210x);

	ret = fs210x_set_reg_table(fs210x, scene);
	if (ret) {
		dev_err(fs210x->dev, "Failed to set reg: %d\n", ret);
		return ret;
	}

	ret = fs210x_write_dsp_effect(fs210x, scene, scene_id);
	if (ret) {
		dev_err(fs210x->dev, "Failed to write ram: %d\n", ret);
		return ret;
	}

	fs210x->cur_scene = scene;
	fs210x->scene_id  = scene_id;

	if (is_playing)
		fs210x_dev_play(fs210x);

	return 0;
}

static inline int volume_is_valid(u16 vol)
{
	return (vol <= FS210X_VOLUME_MAX);
}

static int fs210x_set_pcm_volume(struct fs210x_priv *fs210x)
{
	u16 vol[2];
	int ret;

	if (fs210x->devid == FS2105S_DEVICE_ID) {
		/*
		 * FS2105SE volume registers: 39h(Left)/3Ah(Right):
		 * VOL[15:7]: 0 dB to -95.8125dB in steps of -0.1875dB
		 * 1FFh: 0dB
		 * 000h: -95.8125dB
		 */
		vol[0] = fs210x->vol[0] << FS2105S_39H_VOL_SHIFT;
		vol[1] = fs210x->vol[1] << FS2105S_3AH_VOL_SHIFT;
	} else {
		/*
		 * FS2104 volume registers: 39h(Left)/3Ah(Right):
		 * VOL[15:6]: 0 dB to -131.8125dB in steps of -0.1875dB
		 * 2BFh: 0dB
		 * 0C0h: -95.8125dB
		 * 000h: -131.8125dB
		 * Offset between FS2104 and FS2105S: 2BFh-1FFh=0C0h
		 */
#define VOL_OFFSET (0x2BF - 0x1FF)
		vol[0] = (fs210x->vol[0] + VOL_OFFSET) << FS210X_39H_VOL_SHIFT;
		vol[1] = (fs210x->vol[1] + VOL_OFFSET) << FS210X_3AH_VOL_SHIFT;
	}

	ret  = fs210x_reg_write(fs210x, FS210X_39H_LVOLCTRL, vol[0]);
	ret |= fs210x_reg_write(fs210x, FS210X_3AH_RVOLCTRL, vol[1]);

	return ret;
}

static int fs210x_init_chip(struct fs210x_priv *fs210x)
{
	int scene_id;
	int ret;

	/* i2c reset */
	ret = fs210x_reg_write(fs210x, FS210X_10H_PWRCTRL,
			       FS210X_10H_I2C_RESET);
	if (ret)
		goto tag_power_down;

	/* Wait >= 10ms for i2c resetting */
	usleep_range(10000, 10050);

	/* Backup scene id */
	scene_id = fs210x->scene_id;
	fs210x->scene_id = -1;

	/* Init registers/RAM by init scene */
	ret = fs210x_set_scene(fs210x, FS210X_INIT_SCENE);
	if (ret)
		goto tag_power_down;

	/*
	 * If the firmware has effect scene(s),
	 * we load effect scene by default scene or scene_id
	 */
	if (fs210x->amp_lib.scene_count > 1) {
		if (scene_id < FS210X_DEFAULT_SCENE)
			scene_id = FS210X_DEFAULT_SCENE;
		ret = fs210x_set_scene(fs210x, scene_id);
		if (ret)
			goto tag_power_down;
	}

	ret = fs210x_set_pcm_volume(fs210x);

tag_power_down:
	/* Power down the device */
	ret |= fs210x_reg_write(fs210x, FS210X_11H_SYSCTRL,
				FS210X_11H_DPS_PWDN);
	if (!ret)
		fs210x->is_inited = true;

	return ret;
}

static void fs210x_sdz_pin_set(struct fs210x_priv *fs210x, bool active)
{
	if (!fs210x || !fs210x->gpio_sdz)
		return;

	/*
	 * The SDZ pin(Shut Down) is active low, when it is pulled low,
	 * it will trigger two procedures sequentially:
	 * - Power down the chip;
	 * - Reset the chip to shut down state.
	 * NOTE:
	 * The I2C communication is nack until the SDZ pin
	 * is pulled up and wait 10ms at least.
	 */
	if (active) {
		gpiod_set_value_cansleep(fs210x->gpio_sdz, 1);
		dev_dbg(fs210x->dev, "SDZ pin is OFF\n");
		usleep_range(30000, 30050); /* >= 30ms */
	} else {
		gpiod_set_value_cansleep(fs210x->gpio_sdz, 0);
		dev_dbg(fs210x->dev, "SDZ pin is ON\n");
		usleep_range(10000, 10050); /* >= 10ms */
	}
}

static int fs210x_set_i2s_params(struct fs210x_priv *fs210x)
{
	const struct fs_i2s_srate params[] = {
		{ 16000, 0x3 },
		{ 32000, 0x7 },
		{ 44100, 0x8 },
		{ 48000, 0x9 },
		{ 88200, 0xA },
		{ 96000, 0xB },
	};
	u16 val;
	int i, ret;

	for (i = 0; i < ARRAY_SIZE(params); i++) {
		if (params[i].srate != fs210x->srate)
			continue;
		val = params[i].i2ssr << FS210X_17H_I2SSR_SHIFT;
		ret = fs210x_reg_update_bits(fs210x,
					     FS210X_17H_I2SCTRL,
					     FS210X_17H_I2SSR_MASK,
					     val);
		return ret;
	}

	dev_err(fs210x->dev, "Invalid sample rate: %d\n", fs210x->srate);

	return -EINVAL;
}

static int fs210x_get_pll_div(struct fs210x_priv *fs210x,
			      const struct fs_pll_div **pll_div)
{
	int i;

	if (!fs210x || !pll_div)
		return -EINVAL;

	for (i = 0; i < ARRAY_SIZE(fs210x_pll_div); i++) {
		if (fs210x_pll_div[i].bclk != fs210x->bclk)
			continue;
		*pll_div = fs210x_pll_div + i;
		return 0;
	}

	dev_err(fs210x->dev, "No PLL table for bclk: %d\n", fs210x->bclk);

	return -EFAULT;
}

static int fs210x_set_hw_params(struct fs210x_priv *fs210x)
{
	const struct fs_pll_div *pll_div;
	int ret;

	ret = fs210x_set_i2s_params(fs210x);
	if (ret) {
		dev_err(fs210x->dev, "Failed to set i2s params: %d\n", ret);
		return ret;
	}

	/* Set pll params */
	ret = fs210x_get_pll_div(fs210x, &pll_div);
	if (ret)
		return ret;

	ret  = fs210x_reg_write(fs210x, FS210X_A1H_PLLCTRL1, pll_div->pll1);
	ret |= fs210x_reg_write(fs210x, FS210X_A2H_PLLCTRL2, pll_div->pll2);
	ret |= fs210x_reg_write(fs210x, FS210X_A3H_PLLCTRL3, pll_div->pll3);

	return ret;
}

static int fs210x_mute(struct fs210x_priv *fs210x, bool mute)
{
	int ret;

	if (mute) {
		cancel_delayed_work_sync(&fs210x->fault_check_work);
		cancel_delayed_work_sync(&fs210x->start_work);
		mutex_lock(&fs210x_mutex);
		ret = fs210x_dev_stop(fs210x);
		mutex_unlock(&fs210x_mutex);
		return ret;
	}

	/*
	 * According to the power up/down sequence of FS210x,
	 * the FS210x requests the I2S clock has been present
	 * and stable(>= 2ms) before it playing.
	 */
	if (fs210x->clk_bclk) {
		mutex_lock(&fs210x_mutex);
		ret = fs210x_dev_play(fs210x);
		mutex_unlock(&fs210x_mutex);
	} else {
		schedule_delayed_work(&fs210x->start_work,
				      msecs_to_jiffies(FS210X_START_DELAY_MS));
		ret = 0;
	}

	schedule_delayed_work(&fs210x->fault_check_work,
			      msecs_to_jiffies(FS210X_FAULT_CHECK_INTERVAL_MS));

	return ret;
}

static int fs210x_dai_set_fmt(struct snd_soc_dai *dai, unsigned int fmt)
{
	struct fs210x_priv *fs210x;

	fs210x = snd_soc_component_get_drvdata(dai->component);

	dev_dbg(fs210x->dev, "fmt: %x\n", fmt);

	switch (fmt & SND_SOC_DAIFMT_MASTER_MASK) {
	case SND_SOC_DAIFMT_CBC_CFC:
		/* Only supports slave mode */
		break;
	default:
		dev_err(fs210x->dev, "Only supports slave mode\n");
		return -EINVAL;
	}

	return 0;
}

static int fs210x_dai_hw_params(struct snd_pcm_substream *substream,
				struct snd_pcm_hw_params *params,
				struct snd_soc_dai *dai)
{
	struct fs210x_priv *fs210x;
	int chn_num;
	int ret;

	if (substream->stream != SNDRV_PCM_STREAM_PLAYBACK)
		return 0;

	fs210x = snd_soc_component_get_drvdata(dai->component);

	fs210x->srate = params_rate(params);
	fs210x->bclk  = snd_soc_params_to_bclk(params);
	chn_num = params_channels(params);
	if (chn_num == 1) /* mono */
		fs210x->bclk *= 2; /* I2S bus has 2 channels */

	dev_info(fs210x->dev, "hw params: %d-%d-%d\n",
		 fs210x->srate, chn_num, fs210x->bclk);

	/* The FS2105S can't support 16kHz sample rate. */
	if (fs210x->devid == FS2105S_DEVICE_ID && fs210x->srate == 16000)
		return -ENOTSUPP;

	mutex_lock(&fs210x_mutex);
	ret = fs210x_set_hw_params(fs210x);
	mutex_unlock(&fs210x_mutex);
	if (ret)
		dev_err(fs210x->dev, "Failed to set hw params: %d\n", ret);

	return ret;
}

static int fs210x_dai_mute(struct snd_soc_dai *dai, int mute, int stream)
{
	struct fs210x_priv *fs210x;
	int ret;

	if (stream != SNDRV_PCM_STREAM_PLAYBACK)
		return 0;

	fs210x = snd_soc_component_get_drvdata(dai->component);

	if (!fs210x->is_inited)
		return -EFAULT;

	if (mute) {
		ret = fs210x_mute(fs210x, true);
		fs210x_bclk_set(fs210x, false);
		dev_info(fs210x->dev, "playback mute\n");
	} else {
		fs210x_bclk_set(fs210x, true);
		ret = fs210x_mute(fs210x, false);
		dev_info(fs210x->dev, "playback unmute\n");
	}

	return ret;
}

static void fs210x_start_work(struct work_struct *work)
{
	struct fs210x_priv *fs210x;
	int ret;

	fs210x = container_of(work, struct fs210x_priv, start_work.work);

	mutex_lock(&fs210x_mutex);

	ret = fs210x_dev_play(fs210x);
	if (ret)
		dev_err(fs210x->dev, "Failed to start playing: %d\n", ret);

	mutex_unlock(&fs210x_mutex);
}

static void fs210x_fault_check_work(struct work_struct *work)
{
	struct fs210x_priv *fs210x;
	u16 status;
	int ret;

	fs210x = container_of(work, struct fs210x_priv, fault_check_work.work);

	mutex_lock(&fs210x_mutex);
	if (!fs210x->is_inited || !fs210x->is_playing) {
		mutex_unlock(&fs210x_mutex);
		return;
	}

	ret = fs210x_reg_read(fs210x, FS210X_05H_ANASTAT, &status);
	mutex_unlock(&fs210x_mutex);
	if (ret)
		return;

	if (!(status & FS210X_05H_PVDD_MASK))
		dev_err(fs210x->dev, "PVDD fault\n");
	if (status & FS210X_05H_OCDL_MASK)
		dev_err(fs210x->dev, "OC detected\n");
	if (status & FS210X_05H_UVDL_MASK)
		dev_err(fs210x->dev, "UV detected\n");
	if (status & FS210X_05H_OVDL_MASK)
		dev_err(fs210x->dev, "OV detected\n");
	if (status & FS210X_05H_OTPDL_MASK)
		dev_err(fs210x->dev, "OT detected\n");
	if (status & FS210X_05H_OCRDL_MASK)
		dev_err(fs210x->dev, "OCR detected\n");
	if (status & FS210X_05H_OCLDL_MASK)
		dev_err(fs210x->dev, "OCL detected\n");
	if (status & FS210X_05H_DCRDL_MASK)
		dev_err(fs210x->dev, "DCR detected\n");
	if (status & FS210X_05H_DCLDL_MASK)
		dev_err(fs210x->dev, "DCL detected\n");
	if (status & FS210X_05H_SRDL_MASK)
		dev_err(fs210x->dev, "SR detected\n");
	if (status & FS210X_05H_OTWDL_MASK)
		dev_err(fs210x->dev, "OTW detected\n");
	if (!(status & FS210X_05H_AMPS_MASK))
		dev_err(fs210x->dev, "Amplifier unready\n");
	if (!(status & FS210X_05H_PLLS_MASK))
		dev_err(fs210x->dev, "PLL unlock\n");
	if (!(status & FS210X_05H_ANAS_MASK))
		dev_err(fs210x->dev, "Analog power fault\n");

	if (status != FS210X_05H_ANASTAT_OK)
		fs210x_reg_dump(fs210x);

	schedule_delayed_work(&fs210x->fault_check_work,
			      msecs_to_jiffies(FS210X_FAULT_CHECK_INTERVAL_MS));
}

static int fs210x_get_drvdata_from_kctrl(struct snd_kcontrol *kctrl,
					 struct fs210x_priv **fs210x)
{
	struct snd_soc_component *cmpnt;

	if (!kctrl) {
		pr_err("fs210x: kcontrol is null\n");
		return -EINVAL;
	}

	cmpnt = snd_soc_kcontrol_component(kctrl);
	if (!cmpnt) {
		pr_err("fs210x: component is null\n");
		return -EINVAL;
	}

	*fs210x = snd_soc_component_get_drvdata(cmpnt);

	return 0;
}

static int fs210x_pcm_volume_info(struct snd_kcontrol *kcontrol,
				  struct snd_ctl_elem_info *uinfo)
{
	uinfo->type = SNDRV_CTL_ELEM_TYPE_INTEGER;
	uinfo->count = 2;

	uinfo->value.integer.min = FS210X_VOLUME_MIN;
	uinfo->value.integer.max = FS210X_VOLUME_MAX;

	return 0;
}

static int fs210x_pcm_volume_get(struct snd_kcontrol *kcontrol,
				 struct snd_ctl_elem_value *ucontrol)
{
	struct fs210x_priv *fs210x;
	int ret;

	ret = fs210x_get_drvdata_from_kctrl(kcontrol, &fs210x);
	if (ret) {
		pr_err("pcm_volume_get: fs210x is null\n");
		return -EINVAL;
	}

	mutex_lock(&fs210x_mutex);
	ucontrol->value.integer.value[0] = fs210x->vol[0];
	ucontrol->value.integer.value[1] = fs210x->vol[1];
	mutex_unlock(&fs210x_mutex);

	return 0;
}

static int fs210x_pcm_volume_put(struct snd_kcontrol *kcontrol,
				 struct snd_ctl_elem_value *ucontrol)
{
	struct fs210x_priv *fs210x;
	long *pval;
	int ret;

	ret = fs210x_get_drvdata_from_kctrl(kcontrol, &fs210x);
	if (ret || !fs210x->dev) {
		pr_err("pcm_volume_put: fs210x is null\n");
		return -EINVAL;
	}

	pval = ucontrol->value.integer.value;
	if (!volume_is_valid(*pval) || !volume_is_valid(*(pval + 1))) {
		dev_err(fs210x->dev, "Invalid volume: %ld-%ld\n",
			*pval, *(pval + 1));
		return -EINVAL;
	}

	fs210x->vol[0] = (u16)*pval;
	fs210x->vol[1] = (u16)*(pval + 1);
	dev_dbg(fs210x->dev, "Set volume: %d-%d\n",
		fs210x->vol[0], fs210x->vol[1]);

	mutex_lock(&fs210x_mutex);

	if (fs210x->is_suspended) {
		mutex_unlock(&fs210x_mutex);
		return 0;
	}

	ret = fs210x_set_pcm_volume(fs210x);
	if (ret)
		dev_err(fs210x->dev, "Failed to set volume: %d\n", ret);

	mutex_unlock(&fs210x_mutex);

	return ret;
}

static int fs210x_effect_scene_info(struct snd_kcontrol *kcontrol,
				    struct snd_ctl_elem_info *uinfo)
{
	const struct fs_amp_scene *scene;
	struct fs210x_priv *fs210x;
	const char *name = "N/A";
	int idx, count;
	int ret;

	ret = fs210x_get_drvdata_from_kctrl(kcontrol, &fs210x);
	if (ret || !fs210x->dev) {
		pr_err("scene_effect_info: fs210x is null\n");
		return -EINVAL;
	}

	uinfo->type = SNDRV_CTL_ELEM_TYPE_ENUMERATED;
	uinfo->count = 1;

	count = fs210x->amp_lib.scene_count - 1; /* Skip init scene */
	if (count < 1) {
		uinfo->value.enumerated.items = 0;
		return 0;
	}

	uinfo->value.enumerated.items = count;
	if (uinfo->value.enumerated.item >= count)
		uinfo->value.enumerated.item = count - 1;

	idx = uinfo->value.enumerated.item;
	scene = fs210x->amp_lib.scene + idx + 1;
	if (scene->name)
		name = scene->name;

	strscpy(uinfo->value.enumerated.name, name, strlen(name) + 1);
	dev_dbg(fs210x->dev, "Scene name.%d: %s\n", idx, name);

	return 0;
}

static int fs210x_effect_scene_get(struct snd_kcontrol *kcontrol,
				   struct snd_ctl_elem_value *ucontrol)
{
	struct fs210x_priv *fs210x;
	int index;
	int ret;

	ret = fs210x_get_drvdata_from_kctrl(kcontrol, &fs210x);
	if (ret || !fs210x->dev) {
		pr_err("scene_effect_get: fs210x is null\n");
		return -EINVAL;
	}

	/* The id of effect scene is from 1 to N. */
	if (fs210x->scene_id < 1)
		return -EINVAL;

	mutex_lock(&fs210x_mutex);
	/*
	 * FS210x has scene(s) as below:
	 * init scene: id = 0
	 * effect scene(s): id = 1~N (optional)
	 * effect_index = scene_id - 1
	 */
	index = fs210x->scene_id - 1;
	ucontrol->value.integer.value[0] = index;
	dev_dbg(fs210x->dev, "Get scene index: %d\n", index);
	mutex_unlock(&fs210x_mutex);

	return 0;
}

static int fs210x_effect_scene_put(struct snd_kcontrol *kcontrol,
				   struct snd_ctl_elem_value *ucontrol)
{
	struct fs210x_priv *fs210x;
	int scene_id;
	int ret;

	ret = fs210x_get_drvdata_from_kctrl(kcontrol, &fs210x);
	if (ret || !fs210x->dev) {
		pr_err("scene_effect_put: fs210x is null\n");
		return -EINVAL;
	}

	mutex_lock(&fs210x_mutex);

	/*
	 * FS210x has scene(s) as below:
	 * init scene: id = 0(It's set in fs210x_init_chip() only)
	 * effect scene(s): id = 1~N (optional)
	 * scene_id = effect_index + 1.
	 */
	scene_id = ucontrol->value.integer.value[0] + 1;
	if (fs210x->is_suspended) {
		fs210x->scene_id = scene_id;
		mutex_unlock(&fs210x_mutex);
		return 0;
	}

	ret = fs210x_set_scene(fs210x, scene_id);
	if (ret)
		dev_err(fs210x->dev, "Failed to set scene: %d\n", ret);

	mutex_unlock(&fs210x_mutex);

	return ret;
}

static const struct snd_soc_dai_ops fs210x_dai_ops = {
	.set_fmt		= fs210x_dai_set_fmt,
	.hw_params		= fs210x_dai_hw_params,
	.mute_stream		= fs210x_dai_mute,
};

static const struct snd_soc_dai_driver fs210x_dai = {
	.name = FS210X_DEFAULT_DAI_NAME,
	.playback = {
		.stream_name = "Playback",
		.channels_min = 1,
		.channels_max = 2,
		.rates = FS210X_RATES,
		.formats = FS210X_FORMATS,
	},
	.capture = {
		.stream_name = "Capture",
		.channels_min = 1,
		.channels_max = 2,
		.rates = FS210X_RATES,
		.formats = FS210X_FORMATS,
	},
	.ops = &fs210x_dai_ops,
	.symmetric_rate = 1,
	.symmetric_sample_bits = 1,
};

static const struct snd_kcontrol_new fs210x_controls[] = {
	FS_SOC_ENUM_EXT("PCM Playback Volume",
			fs210x_pcm_volume_info,
			fs210x_pcm_volume_get,
			fs210x_pcm_volume_put),
};

static const struct snd_kcontrol_new fs210x_scene_control[] = {
	FS_SOC_ENUM_EXT("Effect Scene",
			fs210x_effect_scene_info,
			fs210x_effect_scene_get,
			fs210x_effect_scene_put),
};

static const struct snd_soc_dapm_widget fs210x_dapm_widgets[] = {
	SND_SOC_DAPM_AIF_IN("AIF IN", "Playback", 0, SND_SOC_NOPM, 0, 0),
	SND_SOC_DAPM_AIF_OUT("AIF OUT", "Capture", 0, SND_SOC_NOPM, 0, 0),
	SND_SOC_DAPM_OUTPUT("OUTL"),
	SND_SOC_DAPM_OUTPUT("OUTR"),
	SND_SOC_DAPM_INPUT("SDO"),
};

static const struct snd_soc_dapm_route fs210x_dapm_routes[] = {
	{ "OUTL", NULL, "AIF IN" },
	{ "OUTR", NULL, "AIF IN" },
	{ "AIF OUT", NULL, "SDO" },
};

static int fs210x_add_optional_controls(struct fs210x_priv *fs210x,
					struct snd_soc_component *cmpnt)
{
	int count;

	if (!fs210x || !cmpnt)
		return -EINVAL;

	/*
	 * If the firmware has no scene or only init scene,
	 * we skip adding this mixer control.
	 */
	if (fs210x->amp_lib.scene_count < 2)
		return 0;

	count = ARRAY_SIZE(fs210x_scene_control);

	return snd_soc_add_component_controls(cmpnt,
					      fs210x_scene_control,
					      count);
}

static int fs210x_get_bclk(struct fs210x_priv *fs210x,
			   struct snd_soc_component *cmpnt)
{
	struct clk *bclk;
	int ret;

	bclk = devm_clk_get(fs210x->dev, "bclk");
	if (IS_ERR_OR_NULL(bclk)) {
		ret = bclk ? PTR_ERR(bclk) : -ENODATA;
		if (ret == -EPROBE_DEFER)
			return ret;
		/*
		 * If the SOC doesn't have the bclk clock source,
		 * we skip setting the bclk clock.
		 */
		return 0;
	}

	fs210x->clk_bclk = bclk;
	dev_dbg(fs210x->dev, "Got I2S bclk clock\n");

	return 0;
}

static int fs210x_probe(struct snd_soc_component *cmpnt)
{
	struct fs210x_priv *fs210x;
	int ret;

	fs210x = snd_soc_component_get_drvdata(cmpnt);
	if (!fs210x || !fs210x->dev)
		return -EINVAL;

	fs210x->amp_lib.dev   = fs210x->dev;
	fs210x->amp_lib.devid = fs210x->devid;

	ret = fs_amp_load_firmware(&fs210x->amp_lib, fs210x->pdata.fwm_name);
	if (ret) {
		dev_err(fs210x->dev, "Failed to load firmware: %d\n", ret);
		return ret;
	}

	ret = fs210x_add_optional_controls(fs210x, cmpnt);
	if (ret) {
		dev_err(fs210x->dev, "Failed to add opt-ctrl: %d\n", ret);
		return ret;
	}

	INIT_DELAYED_WORK(&fs210x->fault_check_work, fs210x_fault_check_work);
	INIT_DELAYED_WORK(&fs210x->start_work, fs210x_start_work);

	fs210x_get_bclk(fs210x, cmpnt);

	mutex_lock(&fs210x_mutex);
	ret = fs210x_init_chip(fs210x);
	mutex_unlock(&fs210x_mutex);
	if (ret)
		dev_err(fs210x->dev, "Failed to probe: %d\n", ret);

	return ret;
}

static void fs210x_remove(struct snd_soc_component *cmpnt)
{
	struct fs210x_priv *fs210x;

	fs210x = snd_soc_component_get_drvdata(cmpnt);
	if (!fs210x || !fs210x->dev)
		return;

	cancel_delayed_work_sync(&fs210x->start_work);
	cancel_delayed_work_sync(&fs210x->fault_check_work);

	dev_dbg(fs210x->dev, "Codec removed\n");
}

#ifdef CONFIG_PM
static int fs210x_suspend(struct snd_soc_component *cmpnt)
{
	struct fs210x_priv *fs210x;
	int ret;

	fs210x = snd_soc_component_get_drvdata(cmpnt);
	if (!fs210x || !fs210x->dev)
		return -EINVAL;

	cancel_delayed_work_sync(&fs210x->start_work);
	cancel_delayed_work_sync(&fs210x->fault_check_work);

	mutex_lock(&fs210x_mutex);
	fs210x->cur_scene = NULL;
	fs210x->is_inited = false;
	fs210x->is_playing = false;
	fs210x->is_suspended = true;

	fs210x_sdz_pin_set(fs210x, true);
	mutex_unlock(&fs210x_mutex);

	ret = regulator_bulk_disable(FS210X_NUM_SUPPLIES, fs210x->supplies);
	if (ret) {
		dev_err(fs210x->dev, "Failed to suspend: %d\n", ret);
		return ret;
	}

	dev_info(fs210x->dev, "pm suspended\n");

	return 0;
}

static int fs210x_resume(struct snd_soc_component *cmpnt)
{
	struct fs210x_priv *fs210x;
	int ret;

	fs210x = snd_soc_component_get_drvdata(cmpnt);
	if (!fs210x || !fs210x->dev)
		return -EINVAL;

	ret = regulator_bulk_enable(FS210X_NUM_SUPPLIES, fs210x->supplies);
	if (ret) {
		dev_err(fs210x->dev, "Failed to enable supplies: %d\n", ret);
		return ret;
	}

	mutex_lock(&fs210x_mutex);
	fs210x_sdz_pin_set(fs210x, false);

	fs210x->is_suspended = false;
	ret = fs210x_init_chip(fs210x);
	mutex_unlock(&fs210x_mutex);

	dev_info(fs210x->dev, "pm resumed\n");

	return ret;
}
#else
#define fs210x_suspend NULL
#define fs210x_resume NULL
#endif // CONFIG_PM

static const struct snd_soc_component_driver fs210x_soc_component_dev = {
	.probe			= fs210x_probe,
	.remove			= fs210x_remove,
	.suspend		= fs210x_suspend,
	.resume			= fs210x_resume,
	.controls		= fs210x_controls,
	.num_controls		= ARRAY_SIZE(fs210x_controls),
	.dapm_widgets		= fs210x_dapm_widgets,
	.num_dapm_widgets	= ARRAY_SIZE(fs210x_dapm_widgets),
	.dapm_routes		= fs210x_dapm_routes,
	.num_dapm_routes	= ARRAY_SIZE(fs210x_dapm_routes),
	.use_pmdown_time	= 1,
};

static const struct regmap_config fs210x_regmap = {
	.reg_bits		= 8,
	.val_bits		= 16,
	.max_register		= FS210X_REG_MAX,
	.val_format_endian	= REGMAP_ENDIAN_BIG,
	.cache_type		= REGCACHE_NONE,
};

static int fs210x_detect_device(struct fs210x_priv *fs210x)
{
	u16 devid;
	int ret;

	ret = fs210x_reg_read(fs210x, FS210X_03H_DEVID, &devid);
	if (ret)
		return ret;

	fs210x->devid = HI_U16(devid);

	switch (fs210x->devid) {
	case FS210X_DEVICE_ID:
		dev_info(fs210x->dev, "FS2104 detected\n");
		break;
	case FS2105S_DEVICE_ID:
		dev_info(fs210x->dev, "FS2105S detected\n");
		break;
	default:
		dev_err(fs210x->dev, "DEVID: 0x%04X dismatch\n", devid);
		return -ENODEV;
	}

	return 0;
}

static int fs210x_parse_dts(struct fs210x_priv *fs210x,
			    struct fs210x_platform_data *pdata)
{
	struct device_node *node = fs210x->dev->of_node;
	int i, ret;

	if (!node)
		return 0;

	ret = of_property_read_string(node, "fs,fwm-name", &pdata->fwm_name);
	if (ret)
		pdata->fwm_name = FS210X_DEFAULT_FWM_NAME;

	ret = of_property_read_string(node, "fs,dai-name", &pdata->dai_name);
	if (ret)
		pdata->dai_name = FS210X_DEFAULT_DAI_NAME;

	fs210x->gpio_sdz = devm_gpiod_get_optional(fs210x->dev,
						   "sdz", GPIOD_OUT_HIGH);
	if (IS_ERR_OR_NULL(fs210x->gpio_sdz)) {
		ret = fs210x->gpio_sdz ? PTR_ERR(fs210x->gpio_sdz) : -ENODATA;
		fs210x->gpio_sdz = NULL;
		if (ret == -EPROBE_DEFER)
			return ret;
		dev_dbg(fs210x->dev, "Assuming gpio_sdz is unused\n");
	} else {
		dev_dbg(fs210x->dev, "gpio_sdz: %d\n",
			desc_to_gpio(fs210x->gpio_sdz));
	}

	for (i = 0; i < FS210X_NUM_SUPPLIES; i++)
		fs210x->supplies[i].supply = fs210x_supply_names[i];

	ret = devm_regulator_bulk_get(fs210x->dev,
				      ARRAY_SIZE(fs210x->supplies),
				      fs210x->supplies);
	if (ret) {
		dev_err(fs210x->dev, "Failed to get supplies: %d\n", ret);
		return ret;
	}

	return 0;
}

static int fs210x_parse_platdata(struct fs210x_priv *fs210x)
{
	struct fs210x_platform_data *pdata;
	int ret;

	pdata = &fs210x->pdata;
	ret = fs210x_parse_dts(fs210x, pdata);
	if (ret) {
		dev_err(fs210x->dev, "Failed to parse dts: %d\n", ret);
		return ret;
	}

	return 0;
}

static void fs210x_deinit(struct fs210x_priv *fs210x)
{
	fs210x_sdz_pin_set(fs210x, true);
	regulator_bulk_disable(FS210X_NUM_SUPPLIES, fs210x->supplies);
}

static int fs210x_init(struct fs210x_priv *fs210x)
{
	int ret;

	ret = fs210x_parse_platdata(fs210x);
	if (ret) {
		dev_err(fs210x->dev, "Failed to parse platdata: %d\n", ret);
		return ret;
	}

	ret = regulator_bulk_enable(FS210X_NUM_SUPPLIES, fs210x->supplies);
	if (ret) {
		dev_err(fs210x->dev, "Failed to enable supplies: %d\n", ret);
		return ret;
	}

	/* Make sure the SDZ pin is pulled down enough time. */
	usleep_range(10000, 10050); /* >= 10ms */
	fs210x_sdz_pin_set(fs210x, false);

	ret = fs210x_detect_device(fs210x);
	if (ret) {
		fs210x_deinit(fs210x);
		return ret;
	}

	fs210x->scene_id     = -1; /* Invalid scene */
	fs210x->cur_scene    = NULL;
	fs210x->is_playing   = false;
	fs210x->is_inited    = false;
	fs210x->is_suspended = false;
	fs210x->vol[0]       = FS210X_VOLUME_MAX;
	fs210x->vol[1]       = FS210X_VOLUME_MAX;

	return 0;
}

static int fs210x_register_snd_component(struct fs210x_priv *fs210x)
{
	struct snd_soc_dai_driver *dai_drv;
	int ret;

	dai_drv = devm_kmemdup(fs210x->dev, &fs210x_dai,
			       sizeof(fs210x_dai), GFP_KERNEL);
	if (!dai_drv)
		return -ENOMEM;

	dai_drv->name = fs210x->pdata.dai_name;
	if (fs210x->devid == FS2105S_DEVICE_ID) {
		dai_drv->playback.rates = FS2105S_RATES;
		dai_drv->capture.rates  = FS2105S_RATES;
	}

	ret = snd_soc_register_component(fs210x->dev,
					 &fs210x_soc_component_dev,
					 dai_drv, 1);
	return ret;
}

static int fs210x_i2c_probe(struct i2c_client *client)
{
	struct fs210x_priv *fs210x;
	int ret;

	dev_info(&client->dev, "version: %s\n", FS210X_DRV_VERSION);

	fs210x = devm_kzalloc(&client->dev, sizeof(*fs210x), GFP_KERNEL);
	if (!fs210x)
		return -ENOMEM;

	fs210x->i2c = client;
	fs210x->dev = &client->dev;
	i2c_set_clientdata(client, fs210x);

	fs210x->regmap = devm_regmap_init_i2c(client, &fs210x_regmap);
	if (IS_ERR_OR_NULL(fs210x->regmap)) {
		dev_err(fs210x->dev, "Failed to get regmap\n");
		ret = fs210x->regmap ? PTR_ERR(fs210x->regmap) : -ENODATA;
		return ret;
	}

	mutex_lock(&fs210x_mutex);
	ret = fs210x_init(fs210x);
	mutex_unlock(&fs210x_mutex);
	if (ret)
		return ret;

	ret = fs210x_register_snd_component(fs210x);
	if (ret) {
		fs210x_deinit(fs210x);
		return ret;
	}

	return 0;
}

static void fs210x_i2c_remove(struct i2c_client *client)
{
	struct fs210x_priv *fs210x = i2c_get_clientdata(client);

	snd_soc_unregister_component(fs210x->dev);
	fs210x_deinit(fs210x);
}

static const struct i2c_device_id fs210x_i2c_id[] = {
	{ "fs2104" },
	{ "fs2105s" },
	{}
};
MODULE_DEVICE_TABLE(i2c, fs210x_i2c_id);

#ifdef CONFIG_OF
static const struct of_device_id fs210x_of_match[] = {
	{ .compatible = "foursemi,fs2104", },
	{ .compatible = "foursemi,fs2105s", },
	{},
};
MODULE_DEVICE_TABLE(of, fs210x_of_match);
#endif // CONFIG_OF

static struct i2c_driver fs210x_i2c_driver = {
	.driver = {
		.name = "fs210x",
		.of_match_table = of_match_ptr(fs210x_of_match),
	},
	.id_table = fs210x_i2c_id,
	.probe    = fs210x_i2c_probe,
	.remove   = fs210x_i2c_remove,
};

module_i2c_driver(fs210x_i2c_driver);

MODULE_AUTHOR("Nick Li <nick.li@foursemi.com>");
MODULE_DESCRIPTION("FS2104/5S Audio Amplifier Driver");
MODULE_VERSION(FS210X_DRV_VERSION);
MODULE_LICENSE("GPL v2");

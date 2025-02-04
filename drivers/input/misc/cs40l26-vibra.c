// SPDX-License-Identifier: GPL-2.0
/*
 * CS40L26 Advanced Haptic Driver with waveform memory,
 * integrated DSP, and closed-loop algorithms
 *
 * Copyright 2025 Cirrus Logic, Inc.
 *
 * Author: Fred Treven <ftreven@opensource.cirrus.com>
 */

#include <linux/bitfield.h>
#include <linux/input.h>
#include <linux/mfd/cs40l26.h>

#define CS40L26_EFFECTS_MAX	1

#define CS40L26_NUM_PCT_MAP_VALUES	101

#define CS40L26_STOP_PLAYBACK	0x05000000

#define CS40L26_MAX_INDEX_MASK	GENMASK(15, 0)

#define CS40L26_RAM_INDEX_START	0x01000000
#define CS40L26_RAM_INDEX_END	0x0100007F

#define CS40L26_ROM_INDEX_START	0x01800000
#define CS40L26_ROM_INDEX_END	0x01800026
#define CS40L26_NUM_ROM_WAVES	(CS40L26_ROM_INDEX_END - CS40L26_ROM_INDEX_START + 1)

#define CS40L26_BUZZGEN_INDEX_START	0x01800080
#define CS40L26_BUZZGEN_INDEX_END	0x01800085

#define CS40L26_BUZZGEN_PER_MS_MAX	10
#define CS40L26_BUZZGEN_PER_MS_MIN	4

#define CS40L26_BUZZGEN_LEVEL_MIN	0x00
#define CS40L26_BUZZGEN_LEVEL_MAX	0xFF

#define CS40L26_BUZZGEN_NUM_CONFIGS	(CS40L26_BUZZGEN_INDEX_END - CS40L26_BUZZGEN_INDEX_START)

enum cs40l26_bank {
	CS40L26_BANK_RAM,
	CS40L26_BANK_ROM,
	CS40L26_BANK_BUZ,
};

struct cs40l26_effect {
	enum cs40l26_bank bank;
	u32 index;
	int id;
	struct list_head list;
};

struct cs40l26_vibra {
	struct cs40l26 *cs40l26;
	struct input_dev *input;
	struct workqueue_struct *vib_wq;
	struct list_head effect_head;
};

struct cs40l26_work {
	struct ff_effect *ff_effect;
	struct cs40l26_vibra *vib;
	struct work_struct work;
	s16 *custom_data;
	int custom_len;
	u16 gain_pct;
	int count;
	int error;
};

struct cs40l26_buzzgen_config {
	const char *duration_name;
	const char *freq_name;
	const char *level_name;
	int effect_id;
};

static struct cs40l26_buzzgen_config cs40l26_buzzgen_configs[] = {
	{
		.duration_name = "BUZZ_EFFECTS2_BUZZ_DURATION",
		.freq_name = "BUZZ_EFFECTS2_BUZZ_FREQ",
		.level_name = "BUZZ_EFFECTS2_BUZZ_LEVEL",
		.effect_id = -1
	},
	{
		.duration_name = "BUZZ_EFFECTS3_BUZZ_DURATION",
		.freq_name = "BUZZ_EFFECTS3_BUZZ_FREQ",
		.level_name = "BUZZ_EFFECTS3_BUZZ_LEVEL",
		.effect_id = -1
	},
	{
		.duration_name = "BUZZ_EFFECTS4_BUZZ_DURATION",
		.freq_name = "BUZZ_EFFECTS4_BUZZ_FREQ",
		.level_name = "BUZZ_EFFECTS4_BUZZ_LEVEL",
		.effect_id = -1
	},
	{
		.duration_name = "BUZZ_EFFECTS5_BUZZ_DURATION",
		.freq_name = "BUZZ_EFFECTS5_BUZZ_FREQ",
		.level_name = "BUZZ_EFFECTS5_BUZZ_LEVEL",
		.effect_id = -1
	},
	{
		.duration_name = "BUZZ_EFFECTS6_BUZZ_DURATION",
		.freq_name = "BUZZ_EFFECTS6_BUZZ_FREQ",
		.level_name = "BUZZ_EFFECTS6_BUZZ_LEVEL",
		.effect_id = -1
	},
};

static int cs40l26_buzzgen_find_slot(int id)
{
	int effect_id, lowest_available_slot = -1, slot;

	for (slot = CS40L26_BUZZGEN_NUM_CONFIGS - 1; slot >= 0; slot--) {
		effect_id = cs40l26_buzzgen_configs[slot].effect_id;

		if (effect_id == id)
			return slot;
		else if (effect_id == -1)
			lowest_available_slot = slot;
	}

	return lowest_available_slot;
}

static int cs40l26_sine_upload(struct cs40l26_vibra *vib, struct cs40l26_work *work_data,
			       struct cs40l26_effect *effect)
{
	struct cs_dsp *dsp = &vib->cs40l26->dsp;
	unsigned int duration, freq, level;
	int error, slot;

	slot = cs40l26_buzzgen_find_slot(work_data->ff_effect->id);
	if (slot == -1) {
		dev_err(vib->cs40l26->dev, "No free BUZZGEN slot available\n");
		return -ENOSPC;
	}

	cs40l26_buzzgen_configs[slot].effect_id = work_data->ff_effect->id;

	/* Firmware expects duration in ms divided by 4 */
	duration = (unsigned int)DIV_ROUND_UP(work_data->ff_effect->replay.length, 4);

	freq = (unsigned int)(1000 / clamp_val(work_data->ff_effect->u.periodic.period,
					       CS40L26_BUZZGEN_PER_MS_MIN,
					       CS40L26_BUZZGEN_PER_MS_MAX));

	level = (unsigned int)clamp_val(work_data->ff_effect->u.periodic.magnitude,
					CS40L26_BUZZGEN_LEVEL_MIN, CS40L26_BUZZGEN_LEVEL_MAX);

	guard(mutex)(&dsp->pwr_lock);

	error = cs40l26_fw_write(dsp, cs40l26_buzzgen_configs[slot].duration_name,
				 CS40L26_BUZZGEN_ALGO_ID, duration);
	if (error)
		return error;

	error = cs40l26_fw_write(dsp, cs40l26_buzzgen_configs[slot].freq_name,
				 CS40L26_BUZZGEN_ALGO_ID, freq);
	if (error)
		return error;

	error = cs40l26_fw_write(dsp, cs40l26_buzzgen_configs[slot].level_name,
				 CS40L26_BUZZGEN_ALGO_ID, level);
	if (error)
		return error;

	effect->id = work_data->ff_effect->id;
	effect->bank = CS40L26_BANK_BUZ;

	/* BUZZGEN slot 1 is reserved for OTP buzz so offset of 1 required */
	effect->index = CS40L26_BUZZGEN_INDEX_START + slot + 1;

	return 0;
}

static int cs40l26_num_ram_waves(struct cs40l26_vibra *vib)
{
	u32 nwaves;
	int error;

	guard(mutex)(&vib->cs40l26->dsp.pwr_lock);

	error = cs40l26_fw_read(&vib->cs40l26->dsp, "NUM_OF_WAVES",
				vib->cs40l26->variant->info->vibegen_algo_id, &nwaves);

	return error ? error : (int)nwaves;
}

static int cs40l26_trigger_index_get(struct cs40l26_vibra *vib, struct cs40l26_work *work_data,
				     enum cs40l26_bank bank, u32 *trigger_index)
{
	u16 index = (u16)(work_data->custom_data[1] & CS40L26_MAX_INDEX_MASK);
	struct device *dev = vib->cs40l26->dev;
	int error = 0, nwaves;
	u32 index_start;

	switch (bank) {
	case CS40L26_BANK_RAM:
		nwaves = cs40l26_num_ram_waves(vib);
		if (nwaves < 0) {
			error = nwaves;
		} else if (nwaves == 0) {
			dev_err(dev, "No waveforms in RAM bank\n");
			error = -ENODATA;
		}

		index_start = CS40L26_RAM_INDEX_START;
		break;
	case CS40L26_BANK_ROM:
		nwaves = CS40L26_NUM_ROM_WAVES;
		index_start = CS40L26_ROM_INDEX_START;
		break;
	default:
		dev_err(dev, "Invalid bank %u\n", bank);
		error = -EINVAL;
	}

	if (error)
		return error;

	if (index > nwaves - 1) {
		dev_err(dev, "Index %u invalid for bank %u (%d waveforms)\n", index, bank, nwaves);
		return -EINVAL;
	}

	*trigger_index = index + index_start;

	return 0;
}

static int cs40l26_custom_upload(struct cs40l26_vibra *vib, struct cs40l26_work *work_data,
				 struct cs40l26_effect *effect)
{
	size_t data_len = work_data->ff_effect->u.periodic.custom_len;
	enum cs40l26_bank bank;
	int error;

	if (data_len != 2) {
		dev_err(vib->cs40l26->dev, "Invalid custom data length %zd\n", data_len);
		return -EINVAL;
	}

	bank = (enum cs40l26_bank)work_data->custom_data[0];

	error = cs40l26_trigger_index_get(vib, work_data, bank, &effect->index);
	if (error)
		return error;

	effect->id = work_data->ff_effect->id;
	effect->bank = bank;

	return 0;
}

static struct cs40l26_effect *cs40l26_find_effect(struct cs40l26_vibra *vib, int id)
{
	struct cs40l26_effect *effect;

	if (list_empty(&vib->effect_head))
		return NULL;

	list_for_each_entry(effect, &vib->effect_head, list) {
		if (effect->id == id)
			return effect;
	}

	return NULL;
}

static void cs40l26_upload_worker(struct work_struct *work)
{
	struct cs40l26_work *work_data = container_of(work, struct cs40l26_work, work);
	struct cs40l26_vibra *vib = work_data->vib;
	struct device *dev = vib->cs40l26->dev;
	struct cs40l26_effect *effect;
	bool new_effect = false;
	int error;

	error = pm_runtime_resume_and_get(dev);
	if (error) {
		work_data->error = error;
		return;
	}

	effect = cs40l26_find_effect(vib, work_data->ff_effect->id);
	if (!effect) {
		effect = devm_kzalloc(dev, sizeof(struct cs40l26_effect), GFP_KERNEL);
		if (!effect) {
			cs40l26_pm_exit(dev);

			work_data->error = -ENOMEM;
			return;
		}

		new_effect = true;
	}

	if (work_data->ff_effect->u.periodic.waveform == FF_CUSTOM) {
		error = cs40l26_custom_upload(vib, work_data, effect);
	} else if (work_data->ff_effect->u.periodic.waveform == FF_SINE) {
		error = cs40l26_sine_upload(vib, work_data, effect);
	} else {
		dev_err(dev, "Type 0x%X unsupported\n", work_data->ff_effect->u.periodic.waveform);
		error = -EINVAL;
	}

	if (error) {
		if (new_effect)
			devm_kfree(dev, effect);

		cs40l26_pm_exit(dev);

		work_data->error = error;
		return;
	}

	if (new_effect)
		list_add(&effect->list, &vib->effect_head);

	cs40l26_pm_exit(dev);

	work_data->error = 0;
}

static int cs40l26_upload(struct input_dev *dev, struct ff_effect *effect, struct ff_effect *old)
{
	struct cs40l26_vibra *vib = input_get_drvdata(dev);
	bool custom = false;
	struct cs40l26_work *work_data;
	int error;

	work_data = kzalloc(sizeof(struct cs40l26_work), GFP_KERNEL);
	if (!work_data)
		return -ENOMEM;

	if (effect->u.periodic.waveform == FF_CUSTOM) {
		work_data->custom_data = memdup_array_user(effect->u.periodic.custom_data,
							   effect->u.periodic.custom_len,
							   sizeof(s16));
		if (IS_ERR(work_data->custom_data)) {
			error = PTR_ERR(work_data->custom_data);
			goto out_free;
		}

		custom = true;
		work_data->custom_len = effect->u.periodic.custom_len;
	}

	work_data->vib = vib;
	work_data->ff_effect = effect;

	INIT_WORK(&work_data->work, cs40l26_upload_worker);

	queue_work(vib->vib_wq, &work_data->work);
	flush_work(&work_data->work);

	error = work_data->error;

out_free:
	if (custom)
		kfree(work_data->custom_data);

	kfree(work_data);

	return error;
}

static void cs40l26_stop_playback_worker(struct work_struct *work)
{
	struct cs40l26_work *work_data = container_of(work, struct cs40l26_work, work);
	struct cs40l26_vibra *vib = work_data->vib;

	if (pm_runtime_resume_and_get(vib->cs40l26->dev))
		goto out_free;

	if (cs40l26_dsp_write(vib->cs40l26, CS40L26_STOP_PLAYBACK))
		dev_err(vib->cs40l26->dev, "Failed to stop haptic playback\n");

	cs40l26_pm_exit(vib->cs40l26->dev);
out_free:
	kfree(work_data);
}

static void cs40l26_start_playback_worker(struct work_struct *work)
{
	struct cs40l26_work *work_data = container_of(work, struct cs40l26_work, work);
	struct cs40l26 *cs40l26 = work_data->vib->cs40l26;
	struct cs40l26_effect *effect;
	u16 duration;
	int id;

	id = work_data->ff_effect->id;

	duration = work_data->ff_effect->replay.length;

	if (pm_runtime_resume_and_get(cs40l26->dev))
		goto out_free;

	guard(mutex)(&cs40l26->dsp.pwr_lock);

	if (cs40l26_fw_write(&cs40l26->dsp, "TIMEOUT_MS", cs40l26->variant->info->vibegen_algo_id,
			     duration))
		goto out_pm;

	effect = cs40l26_find_effect(work_data->vib, id);
	if (effect) {
		while (--work_data->count >= 0) {
			if (cs40l26_dsp_write(cs40l26, effect->index))
				goto out_pm;

			usleep_range(duration, duration + 100);
		}
	} else {
		dev_err(cs40l26->dev, "No effect found with ID %d\n", id);
	}

out_pm:
	cs40l26_pm_exit(cs40l26->dev);

out_free:
	kfree(work_data);
}

static int cs40l26_playback(struct input_dev *dev, int effect_id, int val)
{
	struct cs40l26_vibra *vib = input_get_drvdata(dev);
	struct cs40l26_work *work_data;

	work_data = kzalloc(sizeof(struct cs40l26_work), GFP_ATOMIC);
	if (!work_data)
		return -ENOMEM;

	work_data->vib = vib;

	if (val > 0) {
		work_data->ff_effect = &dev->ff->effects[effect_id];
		work_data->count = val;
		INIT_WORK(&work_data->work, cs40l26_start_playback_worker);
	} else {
		INIT_WORK(&work_data->work, cs40l26_stop_playback_worker);
	}

	queue_work(vib->vib_wq, &work_data->work);

	return 0;
}

static int cs40l26_sine_erase(struct cs40l26_vibra *vib, int id)
{
	int slot = cs40l26_buzzgen_find_slot(id);

	if (slot == -1) {
		dev_err(vib->cs40l26->dev, "No BUZZGEN ID matching %d\n", id);
		return -EINVAL;
	}

	cs40l26_buzzgen_configs[slot].effect_id = -1;

	return 0;
}

static void cs40l26_erase_worker(struct work_struct *work)
{
	struct cs40l26_work *work_data = container_of(work, struct cs40l26_work, work);
	struct cs40l26_vibra *vib = work_data->vib;
	struct device *dev = vib->cs40l26->dev;
	int id = work_data->ff_effect->id;
	struct cs40l26_effect *effect;
	int error;

	error = pm_runtime_resume_and_get(dev);
	if (error) {
		work_data->error = error;
		return;
	}

	effect = cs40l26_find_effect(vib, id);
	if (!effect) {
		dev_err(dev, "Cannot erase effect with ID %d, no such effect\n", id);
		error = -EINVAL;
		goto out_pm;
	}

	if (effect->bank == CS40L26_BANK_BUZ) {
		error = cs40l26_sine_erase(vib, id);
		if (error)
			goto out_pm;
	}

	list_del(&effect->list);
	devm_kfree(dev, effect);

out_pm:
	cs40l26_pm_exit(dev);

	work_data->error = error;
}

static int cs40l26_erase(struct input_dev *dev, int effect_id)
{
	struct cs40l26_vibra *vib = input_get_drvdata(dev);
	struct cs40l26_work *work_data;
	int error;

	work_data = kzalloc(sizeof(struct cs40l26_work), GFP_KERNEL);
	if (!work_data)
		return -ENOMEM;

	work_data->vib = vib;
	work_data->error = 0;
	work_data->ff_effect = &dev->ff->effects[effect_id];

	INIT_WORK(&work_data->work, cs40l26_erase_worker);

	queue_work(vib->vib_wq, &work_data->work);
	flush_work(&work_data->work);

	error = work_data->error;

	kfree(work_data);

	return error;
}

/* LUT for converting gain percentage to attenuation in dB */
static const u32 cs40l26_atten_lut_q21_2[CS40L26_NUM_PCT_MAP_VALUES] = {
	/* MUTE */ 400, 160, 136, 122, 112, 104, 98, 92, 88, 84, 80, 77, 74,
	71, 68, 66, 64, 62, 60, 58, 56, 54, 53,	51, 50, 48, 47, 45, 44, 43,
	42, 41, 40, 39, 37, 36,	35, 35, 34, 33, 32, 31, 30, 29, 29, 28, 27,
	26, 26, 25, 24,	23, 23, 22, 21, 21, 20, 20, 19, 18, 18, 17, 17, 16,
	16, 15, 14, 14, 13, 13, 12, 12, 11, 11, 10, 10, 10, 9, 9, 8, 8, 7,
	7, 6, 6, 6, 5, 5, 4, 4, 4, 3, 3, 3, 2, 2, 1, 1, 1, 0, 0, /* 100% */
};

static void cs40l26_set_gain_worker(struct work_struct *work)
{
	struct cs40l26_work *work_data = container_of(work, struct cs40l26_work, work);
	struct cs40l26_vibra *vib = work_data->vib;
	struct cs40l26 *cs40l26 = vib->cs40l26;
	int error;

	error = pm_runtime_resume_and_get(vib->cs40l26->dev);
	if (error) {
		dev_err(vib->cs40l26->dev, "%s: Failed to exit hibernate\n", __func__);
		goto out_free;
	}

	guard(mutex)(&vib->cs40l26->dsp.pwr_lock);

	error = cs40l26_fw_write(&vib->cs40l26->dsp, "SOURCE_ATTENUATION",
				 cs40l26->variant->info->ram_ext_algo_id,
				 cs40l26_atten_lut_q21_2[work_data->gain_pct]);
	if (error)
		dev_err(vib->cs40l26->dev, "Failed to set attenuation\n");

	cs40l26_pm_exit(vib->cs40l26->dev);

out_free:
	kfree(work_data);
}

static void cs40l26_set_gain(struct input_dev *dev, u16 gain)
{
	struct cs40l26_vibra *vib = input_get_drvdata(dev);
	struct cs40l26_work *work_data;

	if (gain >= CS40L26_NUM_PCT_MAP_VALUES) {
		dev_err(vib->cs40l26->dev, "Gain value %u%% out of bounds\n", gain);
		return;
	}

	work_data = kzalloc(sizeof(struct cs40l26_work), GFP_ATOMIC);
	if (!work_data)
		return;

	work_data->gain_pct = gain;
	work_data->vib = vib;

	INIT_WORK(&work_data->work, cs40l26_set_gain_worker);

	queue_work(vib->vib_wq, &work_data->work);
}

static void cs40l26_remove_wq(void *data)
{
	flush_workqueue(data);
	destroy_workqueue((struct workqueue_struct *)data);
}

static int cs40l26_vibra_probe(struct platform_device *pdev)
{
	struct cs40l26 *cs40l26 = dev_get_drvdata(pdev->dev.parent);
	struct cs40l26_vibra *vib;
	int error;

	vib = devm_kzalloc(cs40l26->dev, sizeof(struct cs40l26_vibra), GFP_KERNEL);
	if (!vib)
		return -ENOMEM;

	vib->cs40l26 = cs40l26;

	vib->input = devm_input_allocate_device(vib->cs40l26->dev);
	if (!vib->input)
		return -ENOMEM;

	vib->input->id.product = cs40l26->devid;
	vib->input->id.version = cs40l26->revid;
	vib->input->name = "cs40l26_vibra";

	input_set_drvdata(vib->input, vib);
	input_set_capability(vib->input, EV_FF, FF_PERIODIC);
	input_set_capability(vib->input, EV_FF, FF_CUSTOM);
	input_set_capability(vib->input, EV_FF, FF_SINE);
	input_set_capability(vib->input, EV_FF, FF_GAIN);

	error = input_ff_create(vib->input, 1);
	if (error) {
		dev_err(vib->cs40l26->dev, "Failed to create input device\n");
		return error;
	}

	clear_bit(FF_RUMBLE, vib->input->ffbit);

	vib->input->ff->upload = cs40l26_upload;
	vib->input->ff->playback = cs40l26_playback;
	vib->input->ff->set_gain = cs40l26_set_gain;
	vib->input->ff->erase = cs40l26_erase;

	INIT_LIST_HEAD(&vib->effect_head);

	vib->vib_wq = alloc_ordered_workqueue("vib_wq", WQ_HIGHPRI);
	if (!vib->vib_wq)
		return -ENOMEM;

	error = devm_add_action_or_reset(vib->cs40l26->dev, cs40l26_remove_wq, vib->vib_wq);
	if (error)
		return error;

	error = input_register_device(vib->input);
	if (error)
		return error;

	dev_info(vib->cs40l26->dev, "Loaded cs40l26-vibra with %d RAM waveforms\n",
		 cs40l26_num_ram_waves(vib));

	return 0;
}

static const struct platform_device_id cs40l26_vibra_id_match[] = {
	{ "cs40l26-vibra", },
	{}
};
MODULE_DEVICE_TABLE(platform, cs40l26_vibra_id_match);

static struct platform_driver cs40l26_vibra_driver = {
	.probe		= cs40l26_vibra_probe,
	.id_table	= cs40l26_vibra_id_match,
	.driver		= {
		.name	= "cs40l26-vibra",
	},
};
module_platform_driver(cs40l26_vibra_driver);

MODULE_DESCRIPTION("CS40L26 Boosted Class D Amplifier for Haptics");
MODULE_AUTHOR("Fred Treven, Cirrus Logic Inc. <ftreven@opensource.cirrus.com>");
MODULE_LICENSE("GPL");

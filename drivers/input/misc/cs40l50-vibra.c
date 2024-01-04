// SPDX-License-Identifier: GPL-2.0
/*
 * CS40L50 Advanced Haptic Driver with waveform memory,
 * integrated DSP, and closed-loop algorithms
 *
 * Copyright 2023 Cirrus Logic, Inc.
 *
 * Author: James Ogletree <james.ogletree@cirrus.com>
 */

#include <linux/input.h>
#include <linux/mfd/cs40l50.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>

/* Wavetables */
#define CS40L50_RAM_INDEX_START		0x1000000
#define CS40L50_RAM_INDEX_END		0x100007F
#define CS40L50_RTH_INDEX_START		0x1400000
#define CS40L50_RTH_INDEX_END		0x1400001
#define CS40L50_ROM_INDEX_START		0x1800000
#define CS40L50_ROM_INDEX_END		0x180001A
#define CS40L50_OWT_HEADER_SIZE		12
#define CS40L50_OWT_TYPE_PCM		8
#define CS40L50_OWT_TYPE_PWLE		12
#define CS40L50_OWT_PCM_ID		0x0
#define CS40L50_OWT_CUSTOM_DATA_SIZE	2

/* DSP */
#define CS40L50_GPIO_BASE		0x2804140
#define CS40L50_OWT_BASE		0x2805C34
#define CS40L50_OWT_SIZE		0x2805C38
#define CS40L50_OWT_NEXT		0x2805C3C

/* GPIO */
#define CS40L50_GPIO_NUM_MASK		GENMASK(14, 12)
#define CS40L50_GPIO_EDGE_MASK		BIT(15)
#define CS40L50_GPIO_MAPPING_INVALID	0
#define CS40L50_GPIO_DISABLE		0x1FF

enum vibra_bank_type {
	WVFRM_BANK_RAM,
	WVFRM_BANK_ROM,
	WVFRM_BANK_OWT,
	WVFRM_BANK_NUM,
};

/* Describes an area in DSP memory populated by effects */
struct vibra_bank {
	enum vibra_bank_type bank;
	u32 base_index;
	u32 max_index;
};

struct vibra_effect {
	enum vibra_bank_type bank;
	struct list_head list;
	u32 gpio_mapping;
	u32 index;
	int id;
};

/* Describes haptic interface of loaded DSP firmware */
struct vibra_dsp {
	struct vibra_bank *banks;
	u32 gpio_base_reg;
	u32 owt_offset_reg;
	u32 owt_size_reg;
	u32 owt_base_reg;
	u32 mailbox_reg;
	u32 push_owt_cmd;
	u32 delete_owt_cmd;
	u32 stop_cmd;
};

/* Describes configuration and state of haptic operations */
struct vibra_info {
	struct device *dev;
	struct regmap *regmap;
	struct input_dev *input;
	struct mutex lock;
	struct workqueue_struct *vibe_wq;
	struct work_struct vibe_start_work;
	struct work_struct vibe_stop_work;
	struct work_struct erase_work;
	struct work_struct add_work;
	struct ff_effect *start_effect;
	struct ff_effect *erase_effect;
	struct ff_effect add_effect;
	struct list_head effect_head;
	struct vibra_dsp dsp;
	int erase_error;
	int add_error;
};

static struct vibra_effect *vibra_find_effect(int id, struct list_head *effect_head)
{
	struct vibra_effect *effect;

	list_for_each_entry(effect, effect_head, list)
		if (effect->id == id)
			return effect;

	return NULL;
}

static int vibra_effect_bank_set(struct vibra_info *info,
				 struct vibra_effect *effect,
				 struct ff_periodic_effect add_effect)
{
	s16 bank = add_effect.custom_data[0] & 0xffffu;
	unsigned int len = add_effect.custom_len;

	if (bank >= WVFRM_BANK_NUM) {
		dev_err(info->dev, "Invalid waveform bank: %d\n", bank);
		return -EINVAL;
	}

	effect->bank = len > CS40L50_OWT_CUSTOM_DATA_SIZE ? WVFRM_BANK_OWT : bank;

	return 0;
}

static int vibra_effect_mapping_set(struct vibra_info *info, u16 button,
				    struct vibra_effect *effect)
{
	u32 gpio_num, gpio_edge;

	if (button) {
		gpio_num = FIELD_GET(CS40L50_GPIO_NUM_MASK, button);
		gpio_edge = FIELD_GET(CS40L50_GPIO_EDGE_MASK, button);
		effect->gpio_mapping = info->dsp.gpio_base_reg +
				       (gpio_num * 8) - gpio_edge;

		return regmap_write(info->regmap, effect->gpio_mapping, button);
	}

	effect->gpio_mapping = CS40L50_GPIO_MAPPING_INVALID;

	return 0;
}

static int vibra_effect_index_set(struct vibra_info *info,
				  struct vibra_effect *effect,
				  struct ff_periodic_effect add_effect)
{
	struct vibra_effect *owt_effect;
	u32 base_index, max_index;

	base_index = info->dsp.banks[effect->bank].base_index;
	max_index = info->dsp.banks[effect->bank].max_index;

	effect->index = base_index;

	switch (effect->bank) {
	case WVFRM_BANK_OWT:
		list_for_each_entry(owt_effect, &info->effect_head, list)
			if (owt_effect->bank == WVFRM_BANK_OWT)
				effect->index++;
		break;
	case WVFRM_BANK_ROM:
	case WVFRM_BANK_RAM:
		effect->index += add_effect.custom_data[1] & 0xffffu;
		break;
	default:
		dev_err(info->dev, "Bank not supported: %d\n", effect->bank);
		return -EINVAL;
	}

	if (effect->index > max_index || effect->index < base_index) {
		dev_err(info->dev, "Index out of bounds: %u\n", effect->index);
		return -ENOSPC;
	}

	return 0;
}

/* Describes a header for an effect in the OWT bank */
struct owt_header {
	u32 type;
	u32 data_words;
	u32 offset;
} __packed;

static int vibra_upload_owt(struct vibra_info *info, struct vibra_effect *effect,
			    struct ff_periodic_effect add_effect)
{
	u32 len, wt_offset, wt_size;
	struct owt_header header;
	u8 *out_data;
	int error;

	error = regmap_read(info->regmap, info->dsp.owt_offset_reg, &wt_offset);
	if (error)
		return error;

	error = regmap_read(info->regmap, info->dsp.owt_size_reg, &wt_size);
	if (error)
		return error;

	len = 2 * add_effect.custom_len;

	if ((wt_size * sizeof(u32)) < CS40L50_OWT_HEADER_SIZE + len)
		return -ENOSPC;

	out_data = kzalloc(CS40L50_OWT_HEADER_SIZE + len, GFP_KERNEL);
	if (!out_data)
		return -ENOMEM;

	header.type = add_effect.custom_data[0] == CS40L50_OWT_PCM_ID ? CS40L50_OWT_TYPE_PCM :
									CS40L50_OWT_TYPE_PWLE;
	header.offset = CS40L50_OWT_HEADER_SIZE / sizeof(u32);
	header.data_words = len / sizeof(u32);

	memcpy(out_data, &header, sizeof(header));
	memcpy(out_data + CS40L50_OWT_HEADER_SIZE, add_effect.custom_data, len);

	error = regmap_bulk_write(info->regmap, info->dsp.owt_base_reg +
				  (wt_offset * sizeof(u32)), out_data,
				  CS40L50_OWT_HEADER_SIZE + len);
	if (error)
		goto err_free;

	error = regmap_write(info->regmap, info->dsp.mailbox_reg,
			     info->dsp.push_owt_cmd);

err_free:
	kfree(out_data);

	return error;
}

static void vibra_add_worker(struct work_struct *work)
{
	struct vibra_info *info = container_of(work, struct vibra_info, add_work);
	struct ff_effect add_effect = info->add_effect;
	struct vibra_effect *effect;
	bool is_new = false;
	int error;

	error = pm_runtime_resume_and_get(info->dev);
	if (error < 0) {
		info->add_error = error;
		return;
	}

	mutex_lock(&info->lock);

	effect = vibra_find_effect(add_effect.id, &info->effect_head);
	if (!effect) {
		effect = kzalloc(sizeof(*effect), GFP_KERNEL);
		if (!effect) {
			error = -ENOMEM;
			goto err_mutex;
		}
		effect->id = add_effect.id;
		is_new = true;
	}

	error = vibra_effect_bank_set(info, effect, add_effect.u.periodic);
	if (error)
		goto err_free;

	error = vibra_effect_index_set(info, effect, add_effect.u.periodic);
	if (error)
		goto err_free;

	error = vibra_effect_mapping_set(info, add_effect.trigger.button, effect);
	if (error)
		goto err_free;

	if (effect->bank == WVFRM_BANK_OWT)
		error = vibra_upload_owt(info, effect,  add_effect.u.periodic);

err_free:
	if (is_new) {
		if (error)
			kfree(effect);
		else
			list_add(&effect->list, &info->effect_head);
	}

err_mutex:
	mutex_unlock(&info->lock);

	pm_runtime_mark_last_busy(info->dev);
	pm_runtime_put_autosuspend(info->dev);

	info->add_error = error;
}

static int vibra_add(struct input_dev *dev, struct ff_effect *effect,
		     struct ff_effect *old)
{
	struct vibra_info *info = input_get_drvdata(dev);
	u32 len = effect->u.periodic.custom_len;

	if (effect->type != FF_PERIODIC || effect->u.periodic.waveform != FF_CUSTOM) {
		dev_err(info->dev, "Type (%#X) or waveform (%#X) unsupported\n",
			effect->type, effect->u.periodic.waveform);
		return -EINVAL;
	}

	memcpy(&info->add_effect, effect, sizeof(struct ff_effect));

	info->add_effect.u.periodic.custom_data = kcalloc(len, sizeof(s16), GFP_KERNEL);
	if (!info->add_effect.u.periodic.custom_data)
		return -ENOMEM;

	if (copy_from_user(info->add_effect.u.periodic.custom_data,
			   effect->u.periodic.custom_data, sizeof(s16) * len)) {
		info->add_error = -EFAULT;
		goto out_free;
	}

	queue_work(info->vibe_wq, &info->add_work);
	flush_work(&info->add_work);
out_free:
	kfree(info->add_effect.u.periodic.custom_data);
	info->add_effect.u.periodic.custom_data = NULL;

	return info->add_error;
}

static void vibra_start_worker(struct work_struct *work)
{
	struct vibra_info *info = container_of(work, struct vibra_info,
					       vibe_start_work);
	struct vibra_effect *effect;

	if (pm_runtime_resume_and_get(info->dev) < 0)
		return;

	mutex_lock(&info->lock);

	effect = vibra_find_effect(info->start_effect->id, &info->effect_head);
	if (effect)
		regmap_write(info->regmap, info->dsp.mailbox_reg, effect->index);

	mutex_unlock(&info->lock);

	if (!effect)
		dev_err(info->dev, "Effect to play not found\n");

	pm_runtime_mark_last_busy(info->dev);
	pm_runtime_put_autosuspend(info->dev);
}

static void vibra_stop_worker(struct work_struct *work)
{
	struct vibra_info *info = container_of(work, struct vibra_info,
					       vibe_stop_work);

	if (pm_runtime_resume_and_get(info->dev) < 0)
		return;

	mutex_lock(&info->lock);

	regmap_write(info->regmap, info->dsp.mailbox_reg, info->dsp.stop_cmd);

	mutex_unlock(&info->lock);

	pm_runtime_mark_last_busy(info->dev);
	pm_runtime_put_autosuspend(info->dev);
}

static int vibra_playback(struct input_dev *dev, int effect_id, int val)
{
	struct vibra_info *info = input_get_drvdata(dev);

	if (val > 0) {
		info->start_effect = &dev->ff->effects[effect_id];
		queue_work(info->vibe_wq, &info->vibe_start_work);
	} else {
		queue_work(info->vibe_wq, &info->vibe_stop_work);
	}

	return 0;
}

static void vibra_erase_worker(struct work_struct *work)
{
	struct vibra_info *info = container_of(work, struct vibra_info,
					       erase_work);
	struct vibra_effect *owt_effect, *erase_effect;
	int error;

	error = pm_runtime_resume_and_get(info->dev);
	if (error < 0) {
		info->erase_error = error;
		return;
	}

	mutex_lock(&info->lock);

	erase_effect = vibra_find_effect(info->erase_effect->id,
					 &info->effect_head);
	if (!erase_effect) {
		dev_err(info->dev, "Effect to erase not found\n");
		error = -EINVAL;
		goto err_mutex;
	}

	if (erase_effect->gpio_mapping != CS40L50_GPIO_MAPPING_INVALID) {
		error = regmap_write(info->regmap, erase_effect->gpio_mapping,
				     CS40L50_GPIO_DISABLE);
		if (error)
			goto err_mutex;
	}

	if (erase_effect->bank == WVFRM_BANK_OWT) {
		error = regmap_write(info->regmap, info->dsp.mailbox_reg,
				     info->dsp.delete_owt_cmd | erase_effect->index);
		if (error)
			goto err_mutex;

		list_for_each_entry(owt_effect, &info->effect_head, list)
			if (owt_effect->bank == WVFRM_BANK_OWT &&
			    owt_effect->index > erase_effect->index)
				owt_effect->index--;
	}

	list_del(&erase_effect->list);
	kfree(erase_effect);

err_mutex:
	mutex_unlock(&info->lock);

	pm_runtime_mark_last_busy(info->dev);
	pm_runtime_put_autosuspend(info->dev);

	info->erase_error = error;
}

static int vibra_erase(struct input_dev *dev, int effect_id)
{
	struct vibra_info *info = input_get_drvdata(dev);

	info->erase_effect = &dev->ff->effects[effect_id];

	queue_work(info->vibe_wq, &info->erase_work);
	flush_work(&info->erase_work);

	return info->erase_error;
}

static struct vibra_bank cs40l50_banks[] = {
	{
		.bank =		WVFRM_BANK_RAM,
		.base_index =	CS40L50_RAM_INDEX_START,
		.max_index =	CS40L50_RAM_INDEX_END,
	},
	{
		.bank =		WVFRM_BANK_ROM,
		.base_index =	CS40L50_ROM_INDEX_START,
		.max_index =	CS40L50_ROM_INDEX_END,
	},
	{
		.bank =		WVFRM_BANK_OWT,
		.base_index =	CS40L50_RTH_INDEX_START,
		.max_index =	CS40L50_RTH_INDEX_END,
	},
};

static struct vibra_dsp cs40l50_dsp = {
	.banks = cs40l50_banks,
	.gpio_base_reg = CS40L50_GPIO_BASE,
	.owt_base_reg = CS40L50_OWT_BASE,
	.owt_offset_reg = CS40L50_OWT_NEXT,
	.owt_size_reg = CS40L50_OWT_SIZE,
	.mailbox_reg = CS40L50_DSP_QUEUE,
	.push_owt_cmd = CS40L50_OWT_PUSH,
	.delete_owt_cmd = CS40L50_OWT_DELETE,
	.stop_cmd = CS40L50_STOP_PLAYBACK,
};

static void vibra_input_unregister(void *data)
{
	input_unregister_device((struct input_dev *)data);
}

static void vibra_remove_wq(void *data)
{
	struct vibra_info *info = data;

	flush_workqueue(info->vibe_wq);
	destroy_workqueue(info->vibe_wq);
}

static int cs40l50_vibra_probe(struct platform_device *pdev)
{
	struct cs40l50 *cs40l50 = dev_get_drvdata(pdev->dev.parent);
	struct vibra_info *info;
	int error;

	info = devm_kzalloc(pdev->dev.parent, sizeof(*info), GFP_KERNEL);
	if (!info)
		return -ENOMEM;

	info->dev = cs40l50->dev;
	info->regmap = cs40l50->regmap;

	info->input = devm_input_allocate_device(info->dev);
	if (!info->input)
		return -ENOMEM;

	info->input->id.product = cs40l50->devid & 0xFFFF;
	info->input->id.version = cs40l50->revid;
	info->input->name = "cs40l50_vibra";

	input_set_drvdata(info->input, info);
	input_set_capability(info->input, EV_FF, FF_PERIODIC);
	input_set_capability(info->input, EV_FF, FF_CUSTOM);

	error = input_ff_create(info->input, FF_MAX_EFFECTS);
	if (error) {
		dev_err(info->dev, "Failed to create input device\n");
		return error;
	}

	info->input->ff->upload = vibra_add;
	info->input->ff->playback = vibra_playback;
	info->input->ff->erase = vibra_erase;

	INIT_LIST_HEAD(&info->effect_head);

	info->dsp = cs40l50_dsp;

	info->vibe_wq = alloc_ordered_workqueue("vibe_wq", 0);
	if (!info->vibe_wq)
		return -ENOMEM;

	error = devm_add_action_or_reset(info->dev, vibra_remove_wq, info);
	if (error)
		return error;

	mutex_init(&info->lock);

	INIT_WORK(&info->vibe_start_work, vibra_start_worker);
	INIT_WORK(&info->vibe_stop_work, vibra_stop_worker);
	INIT_WORK(&info->erase_work, vibra_erase_worker);
	INIT_WORK(&info->add_work, vibra_add_worker);

	error = input_register_device(info->input);
	if (error) {
		dev_err(info->dev, "Failed to register input device\n");
		input_free_device(info->input);
		return error;
	}

	return devm_add_action_or_reset(info->dev, vibra_input_unregister,
					info->input);
}

static const struct platform_device_id vibra_id_match[] = {
	{ "cs40l50-vibra", },
	{}
};
MODULE_DEVICE_TABLE(platform, vibra_id_match);

static struct platform_driver cs40l50_vibra_driver = {
	.probe		= cs40l50_vibra_probe,
	.id_table	= vibra_id_match,
	.driver		= {
		.name	= "cs40l50-vibra",
	},
};
module_platform_driver(cs40l50_vibra_driver);

MODULE_DESCRIPTION("CS40L50 Advanced Haptic Driver");
MODULE_AUTHOR("James Ogletree, Cirrus Logic Inc. <james.ogletree@cirrus.com>");
MODULE_LICENSE("GPL");

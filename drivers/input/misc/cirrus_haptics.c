// SPDX-License-Identifier: GPL-2.0
/*
 * Helper functions for dealing with wavetable
 * formats and DSP interfaces used by Cirrus
 * haptic drivers.
 *
 * Copyright 2023 Cirrus Logic, Inc.
 */

#include <linux/firmware/cirrus/cs_dsp.h>
#include <linux/input.h>
#include <linux/input/cirrus_haptics.h>
#include <linux/pm_runtime.h>

static int cs_hap_pseq_init(struct cs_hap *haptics)
{
	struct cs_hap_pseq_op *op;
	int error, i, num_words;
	u8 operation;
	u32 *words;

	if (!haptics->dsp.pseq_size || !haptics->dsp.pseq_reg)
		return 0;

	INIT_LIST_HEAD(&haptics->pseq_head);

	words = kcalloc(haptics->dsp.pseq_size, sizeof(u32), GFP_KERNEL);
	if (!words)
		return -ENOMEM;

	error = regmap_bulk_read(haptics->regmap, haptics->dsp.pseq_reg,
				 words, haptics->dsp.pseq_size);
	if (error)
		goto err_free;

	for (i = 0; i < haptics->dsp.pseq_size; i += num_words) {
		operation = FIELD_GET(PSEQ_OP_MASK, words[i]);
		switch (operation) {
		case PSEQ_OP_END:
		case PSEQ_OP_WRITE_UNLOCK:
			num_words = PSEQ_OP_END_WORDS;
			break;
		case PSEQ_OP_WRITE_ADDR8:
		case PSEQ_OP_WRITE_H16:
		case PSEQ_OP_WRITE_L16:
			num_words = PSEQ_OP_WRITE_X16_WORDS;
			break;
		case PSEQ_OP_WRITE_FULL:
			num_words = PSEQ_OP_WRITE_FULL_WORDS;
			break;
		default:
			error = -EINVAL;
			dev_err(haptics->dev, "Unsupported op: %u\n", operation);
			goto err_free;
		}

		op = devm_kzalloc(haptics->dev, sizeof(*op), GFP_KERNEL);
		if (!op) {
			error = -ENOMEM;
			goto err_free;
		}

		op->size = num_words * sizeof(u32);
		memcpy(op->words, &words[i], op->size);
		op->offset = i * sizeof(u32);
		op->operation = operation;
		list_add(&op->list, &haptics->pseq_head);

		if (operation == PSEQ_OP_END)
			break;
	}

	if (operation != PSEQ_OP_END)
		error = -ENOENT;

err_free:
	kfree(words);

	return error;
}

static int cs_hap_pseq_find_end(struct cs_hap *haptics,
				struct cs_hap_pseq_op **op_end)
{
	u8 operation = PSEQ_OP_WRITE_FULL;
	struct cs_hap_pseq_op *op;

	list_for_each_entry(op, &haptics->pseq_head, list) {
		operation = op->operation;
		if (operation == PSEQ_OP_END)
			break;
	}

	if (operation != PSEQ_OP_END) {
		dev_err(haptics->dev, "Missing PSEQ list terminator\n");
		return -ENOENT;
	}

	*op_end = op;

	return 0;
}

static struct cs_hap_pseq_op *cs_hap_pseq_find_op(struct cs_hap_pseq_op *match_op,
						  struct list_head *pseq_head)
{
	struct cs_hap_pseq_op *op;

	list_for_each_entry(op, pseq_head, list) {
		if (op->operation == PSEQ_OP_END)
			break;
		if (op->operation != match_op->operation ||
		    op->words[0] != match_op->words[0])
			continue;
		switch (op->operation) {
		case PSEQ_OP_WRITE_FULL:
			if (FIELD_GET(GENMASK(23, 8), op->words[1]) ==
			    FIELD_GET(GENMASK(23, 8), match_op->words[1]))
				return op;
			break;
		case PSEQ_OP_WRITE_H16:
		case PSEQ_OP_WRITE_L16:
			if (FIELD_GET(GENMASK(23, 16), op->words[1]) ==
			    FIELD_GET(GENMASK(23, 16), match_op->words[1]))
				return op;
			break;
		default:
			break;
		}
	}

	return NULL;
}

int cs_hap_pseq_write(struct cs_hap *haptics, u32 addr,
		      u32 data, bool update, u8 op_code)
{
	struct cs_hap_pseq_op *op, *op_end, *op_new;
	struct cs_dsp_chunk ch;
	u32 pseq_bytes;
	int error;

	op_new = devm_kzalloc(haptics->dev, sizeof(*op_new), GFP_KERNEL);
	if (!op_new)
		return -ENOMEM;

	op_new->operation = op_code;

	ch = cs_dsp_chunk((void *) op_new->words,
			  PSEQ_OP_WRITE_FULL_WORDS * sizeof(u32));
	cs_dsp_chunk_write(&ch, 8, op_code);
	switch (op_code) {
	case PSEQ_OP_WRITE_FULL:
		cs_dsp_chunk_write(&ch, 32, addr);
		cs_dsp_chunk_write(&ch, 32, data);
		break;
	case PSEQ_OP_WRITE_L16:
	case PSEQ_OP_WRITE_H16:
		cs_dsp_chunk_write(&ch, 24, addr);
		cs_dsp_chunk_write(&ch, 16, data);
		break;
	default:
		error = -EINVAL;
		goto op_new_free;
	}

	op_new->size = cs_dsp_chunk_bytes(&ch);

	if (update) {
		op = cs_hap_pseq_find_op(op_new, &haptics->pseq_head);
		if (!op) {
			error = -EINVAL;
			goto op_new_free;
		}
	}

	error = cs_hap_pseq_find_end(haptics, &op_end);
	if (error)
		goto op_new_free;

	pseq_bytes = haptics->dsp.pseq_size * sizeof(u32);

	if (pseq_bytes - op_end->offset < op_new->size) {
		error = -ENOMEM;
		goto op_new_free;
	}

	if (update) {
		op_new->offset = op->offset;
	} else {
		op_new->offset = op_end->offset;
		op_end->offset += op_new->size;
	}

	error = regmap_raw_write(haptics->regmap, haptics->dsp.pseq_reg +
				 op_new->offset, op_new->words, op_new->size);
	if (error)
		goto op_new_free;

	if (update) {
		list_replace(&op->list, &op_new->list);
	} else {
		error = regmap_raw_write(haptics->regmap, haptics->dsp.pseq_reg +
					 op_end->offset, op_end->words,
					 op_end->size);
		if (error)
			goto op_new_free;

		list_add(&op_new->list, &haptics->pseq_head);
	}

	return 0;

op_new_free:
	devm_kfree(haptics->dev, op_new);

	return error;
}
EXPORT_SYMBOL_GPL(cs_hap_pseq_write);

int cs_hap_pseq_multi_write(struct cs_hap *haptics,
			    const struct reg_sequence *reg_seq,
			    int num_regs, bool update, u8 op_code)
{
	int error, i;

	for (i = 0; i < num_regs; i++) {
		error = cs_hap_pseq_write(haptics, reg_seq[i].reg,
					  reg_seq[i].def, update, op_code);
		if (error)
			return error;
	}

	return 0;
}
EXPORT_SYMBOL_GPL(cs_hap_pseq_multi_write);

static struct cs_hap_effect *cs_hap_find_effect(int id,
						struct list_head *effect_head)
{
	struct cs_hap_effect *effect;

	list_for_each_entry(effect, effect_head, list)
		if (effect->id == id)
			return effect;

	return NULL;
}

static int cs_hap_effect_bank_set(struct cs_hap *haptics,
				  struct cs_hap_effect *effect,
				  struct ff_periodic_effect add_effect)
{
	s16 bank = add_effect.custom_data[0] & 0xffffu;
	unsigned int len = add_effect.custom_len;

	if (bank >= WVFRM_BANK_NUM) {
		dev_err(haptics->dev, "Invalid waveform bank: %d\n", bank);
		return -EINVAL;
	}

	effect->bank = len > CUSTOM_DATA_SIZE ? WVFRM_BANK_OWT : bank;

	return 0;
}

static int cs_hap_effect_mapping_set(struct cs_hap *haptics, u16 button,
				     struct cs_hap_effect *effect)
{
	u32 gpio_num, gpio_edge;

	if (button) {
		gpio_num = FIELD_GET(BTN_NUM_MASK, button);
		gpio_edge = FIELD_GET(BTN_EDGE_MASK, button);
		effect->mapping = haptics->dsp.gpio_base_reg +
				  (gpio_num * 8) - gpio_edge;

		return regmap_write(haptics->regmap, effect->mapping, button);
	}

	effect->mapping = GPIO_MAPPING_INVALID;

	return 0;
}

static int cs_hap_effect_index_set(struct cs_hap *haptics,
				   struct cs_hap_effect *effect,
				   struct ff_periodic_effect add_effect)
{
	struct cs_hap_effect *owt_effect;
	u32 base_index, max_index;

	base_index = haptics->banks[effect->bank].base_index;
	max_index = haptics->banks[effect->bank].max_index;

	effect->index = base_index;

	switch (effect->bank) {
	case WVFRM_BANK_OWT:
		list_for_each_entry(owt_effect, &haptics->effect_head, list)
			if (owt_effect->bank == WVFRM_BANK_OWT)
				effect->index++;
		break;
	case WVFRM_BANK_ROM:
	case WVFRM_BANK_RAM:
		effect->index += add_effect.custom_data[1] & 0xffffu;
		break;
	default:
		dev_err(haptics->dev, "Bank not supported: %d\n", effect->bank);
		return -EINVAL;
	}

	if (effect->index > max_index || effect->index < base_index) {
		dev_err(haptics->dev, "Index out of bounds: %u\n", effect->index);
		return -ENOSPC;
	}

	return 0;
}

static int cs_hap_upload_pwle(struct cs_hap *haptics,
			      struct cs_hap_effect *effect,
			      struct ff_periodic_effect add_effect)
{
	u32 len, wt_offset, wt_size_words;
	struct cs_hap_pwle_header header;
	u8 *out_data;
	int error;

	error = regmap_read(haptics->regmap, haptics->dsp.owt_offset_reg,
			    &wt_offset);
	if (error)
		return error;

	error = regmap_read(haptics->regmap, haptics->dsp.owt_size_reg,
			    &wt_size_words);
	if (error)
		return error;

	len = 2 * add_effect.custom_len;

	if ((wt_size_words * sizeof(u32)) < OWT_HEADER_SIZE + len)
		return -ENOSPC;

	out_data = kzalloc(OWT_HEADER_SIZE + len, GFP_KERNEL);
	if (!out_data)
		return -ENOMEM;

	header.type = add_effect.custom_data[0] == PCM_ID ? OWT_TYPE_PCM :
							    OWT_TYPE_PWLE;
	header.offset = OWT_HEADER_SIZE / sizeof(u32);
	header.data_words = len / sizeof(u32);

	memcpy(out_data, &header, sizeof(header));
	memcpy(out_data + OWT_HEADER_SIZE, add_effect.custom_data, len);

	error = regmap_bulk_write(haptics->regmap, haptics->dsp.owt_base_reg +
				  (wt_offset * sizeof(u32)), out_data,
				  OWT_HEADER_SIZE + len);
	if (error)
		goto err_free;

	error = regmap_write(haptics->regmap, haptics->dsp.mailbox_reg,
			     haptics->dsp.push_owt_cmd);

err_free:
	kfree(out_data);

	return error;
}

static void cs_hap_add_worker(struct work_struct *work)
{
	struct cs_hap *haptics = container_of(work, struct cs_hap,
					      add_work);
	struct ff_effect add_effect = haptics->add_effect;
	bool is_new = false;
	struct cs_hap_effect *effect;
	int error;

	if (haptics->runtime_pm) {
		error = pm_runtime_resume_and_get(haptics->dev);
		if (error < 0) {
			haptics->add_error = error;
			return;
		}
	}

	mutex_lock(&haptics->lock);

	effect = cs_hap_find_effect(add_effect.id, &haptics->effect_head);
	if (!effect) {
		effect = kzalloc(sizeof(*effect), GFP_KERNEL);
		if (!effect) {
			error = -ENOMEM;
			goto err_mutex;
		}
		effect->id = add_effect.id;
		is_new = true;
	}

	error = cs_hap_effect_bank_set(haptics, effect, add_effect.u.periodic);
	if (error)
		goto err_free;

	error = cs_hap_effect_index_set(haptics, effect, add_effect.u.periodic);
	if (error)
		goto err_free;

	error = cs_hap_effect_mapping_set(haptics, add_effect.trigger.button,
					  effect);
	if (error)
		goto err_free;

	if (effect->bank == WVFRM_BANK_OWT)
		error = cs_hap_upload_pwle(haptics, effect,
					   add_effect.u.periodic);

err_free:
	if (is_new) {
		if (error)
			kfree(effect);
		else
			list_add(&effect->list, &haptics->effect_head);
	}

err_mutex:
	mutex_unlock(&haptics->lock);

	if (haptics->runtime_pm) {
		pm_runtime_mark_last_busy(haptics->dev);
		pm_runtime_put_autosuspend(haptics->dev);
	}

	haptics->add_error = error;
}

static void cs_hap_erase_worker(struct work_struct *work)
{
	struct cs_hap *haptics = container_of(work, struct cs_hap,
					      erase_work);
	int error = 0;
	struct cs_hap_effect *owt_effect, *erase_effect;

	if (haptics->runtime_pm) {
		error = pm_runtime_resume_and_get(haptics->dev);
		if (error < 0) {
			haptics->erase_error = error;
			return;
		}
	}

	mutex_lock(&haptics->lock);

	erase_effect = cs_hap_find_effect(haptics->erase_effect->id,
					  &haptics->effect_head);
	if (!erase_effect) {
		dev_err(haptics->dev, "Effect to erase does not exist\n");
		error = -EINVAL;
		goto err_mutex;
	}

	if (erase_effect->mapping != GPIO_MAPPING_INVALID) {
		error = regmap_write(haptics->regmap, erase_effect->mapping,
				     GPIO_DISABLE);
		if (error)
			goto err_mutex;
	}

	if (erase_effect->bank == WVFRM_BANK_OWT) {
		error = regmap_write(haptics->regmap, haptics->dsp.mailbox_reg,
				     haptics->dsp.delete_owt_cmd |
				     erase_effect->index);
		if (error)
			goto err_mutex;

		list_for_each_entry(owt_effect, &haptics->effect_head, list)
			if (owt_effect->bank == WVFRM_BANK_OWT &&
			    owt_effect->index > erase_effect->index)
				owt_effect->index--;
	}

	list_del(&erase_effect->list);
	kfree(erase_effect);

err_mutex:
	mutex_unlock(&haptics->lock);

	if (haptics->runtime_pm) {
		pm_runtime_mark_last_busy(haptics->dev);
		pm_runtime_put_autosuspend(haptics->dev);
	}

	haptics->erase_error = error;
}

static void cs_hap_vibe_start_worker(struct work_struct *work)
{
	struct cs_hap *haptics = container_of(work, struct cs_hap,
					      vibe_start_work);
	struct cs_hap_effect *effect;
	int error;

	if (haptics->runtime_pm) {
		error = pm_runtime_resume_and_get(haptics->dev);
		if (error < 0) {
			haptics->start_error = error;
			return;
		}
	}

	mutex_lock(&haptics->lock);

	effect = cs_hap_find_effect(haptics->start_effect->id,
				    &haptics->effect_head);
	if (effect) {
		error = regmap_write(haptics->regmap, haptics->dsp.mailbox_reg,
				     effect->index);
	} else {
		dev_err(haptics->dev, "Effect to start does not exist\n");
		error = -EINVAL;
	}

	mutex_unlock(&haptics->lock);

	if (haptics->runtime_pm) {
		pm_runtime_mark_last_busy(haptics->dev);
		pm_runtime_put_autosuspend(haptics->dev);
	}

	haptics->start_error = error;
}

static void cs_hap_vibe_stop_worker(struct work_struct *work)
{
	struct cs_hap *haptics = container_of(work, struct cs_hap,
					      vibe_stop_work);
	int error;

	if (haptics->runtime_pm) {
		error = pm_runtime_resume_and_get(haptics->dev);
		if (error < 0) {
			haptics->stop_error = error;
			return;
		}
	}

	mutex_lock(&haptics->lock);
	error = regmap_write(haptics->regmap, haptics->dsp.mailbox_reg,
			     haptics->dsp.stop_cmd);
	mutex_unlock(&haptics->lock);

	if (haptics->runtime_pm) {
		pm_runtime_mark_last_busy(haptics->dev);
		pm_runtime_put_autosuspend(haptics->dev);
	}

	haptics->stop_error = error;
}

int cs_hap_init(struct cs_hap *haptics)
{
	haptics->vibe_wq = alloc_ordered_workqueue("vibe_wq", 0);
	if (!haptics->vibe_wq)
		return -ENOMEM;

	mutex_init(&haptics->lock);

	INIT_WORK(&haptics->vibe_start_work, cs_hap_vibe_start_worker);
	INIT_WORK(&haptics->vibe_stop_work, cs_hap_vibe_stop_worker);
	INIT_WORK(&haptics->erase_work, cs_hap_erase_worker);
	INIT_WORK(&haptics->add_work, cs_hap_add_worker);

	return cs_hap_pseq_init(haptics);
}
EXPORT_SYMBOL_GPL(cs_hap_init);

void cs_hap_remove(struct cs_hap *haptics)
{
	flush_workqueue(haptics->vibe_wq);
	destroy_workqueue(haptics->vibe_wq);
}
EXPORT_SYMBOL_GPL(cs_hap_remove);

MODULE_DESCRIPTION("Cirrus Logic Haptics Support");
MODULE_LICENSE("GPL");

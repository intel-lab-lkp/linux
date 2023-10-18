// SPDX-License-Identifier: GPL-2.0
/*
 * CS40L50 Advanced Haptic Driver with waveform memory,
 * integrated DSP, and closed-loop algorithms
 *
 * Copyright 2023 Cirrus Logic, Inc.
 *
 */

#include <linux/firmware/cirrus/wmfw.h>
#include <linux/mfd/cs40l50.h>
#include <linux/platform_device.h>
#include <linux/property.h>

static int cs40l50_add(struct input_dev *dev,
		       struct ff_effect *effect,
		       struct ff_effect *old)
{
	struct cs40l50_private *cs40l50 = input_get_drvdata(dev);
	u32 len = effect->u.periodic.custom_len;

	if (effect->type != FF_PERIODIC || effect->u.periodic.waveform != FF_CUSTOM) {
		dev_err(cs40l50->dev, "Type (%#X) or waveform (%#X) unsupported\n",
			effect->type, effect->u.periodic.waveform);
		return -EINVAL;
	}

	memcpy(&cs40l50->haptics.add_effect, effect, sizeof(struct ff_effect));

	cs40l50->haptics.add_effect.u.periodic.custom_data = kcalloc(len,
								     sizeof(s16),
								     GFP_KERNEL);
	if (!cs40l50->haptics.add_effect.u.periodic.custom_data)
		return -ENOMEM;

	if (copy_from_user(cs40l50->haptics.add_effect.u.periodic.custom_data,
			   effect->u.periodic.custom_data, sizeof(s16) * len)) {
		cs40l50->haptics.add_error = -EFAULT;
		goto out_free;
	}

	queue_work(cs40l50->haptics.vibe_wq, &cs40l50->haptics.add_work);
	flush_work(&cs40l50->haptics.add_work);

out_free:
	kfree(cs40l50->haptics.add_effect.u.periodic.custom_data);
	cs40l50->haptics.add_effect.u.periodic.custom_data = NULL;

	return cs40l50->haptics.add_error;
}

static int cs40l50_playback(struct input_dev *dev, int effect_id, int val)
{
	struct cs40l50_private *cs40l50 = input_get_drvdata(dev);

	if (val > 0) {
		cs40l50->haptics.start_effect = &dev->ff->effects[effect_id];
		queue_work(cs40l50->haptics.vibe_wq,
			   &cs40l50->haptics.vibe_start_work);
	} else {
		queue_work(cs40l50->haptics.vibe_wq,
			   &cs40l50->haptics.vibe_stop_work);
	}

	return 0;
}

static int cs40l50_erase(struct input_dev *dev, int effect_id)
{
	struct cs40l50_private *cs40l50 = input_get_drvdata(dev);

	cs40l50->haptics.erase_effect = &dev->ff->effects[effect_id];

	queue_work(cs40l50->haptics.vibe_wq, &cs40l50->haptics.erase_work);
	flush_work(&cs40l50->haptics.erase_work);

	return cs40l50->haptics.erase_error;
}

static const struct reg_sequence cs40l50_int_vamp_seq[] = {
	{ CS40L50_BST_LPMODE_SEL,	CS40L50_DCM_LOW_POWER },
	{ CS40L50_BLOCK_ENABLES2,	CS40L50_OVERTEMP_WARN },
};

static const struct reg_sequence cs40l50_irq_mask_seq[] = {
	{ CS40L50_IRQ1_MASK_2,	CS40L50_IRQ_MASK_2_OVERRIDE },
	{ CS40L50_IRQ1_MASK_20,	CS40L50_IRQ_MASK_20_OVERRIDE },
};

static int cs40l50_hw_init(struct cs40l50_private *cs40l50)
{
	int error;

	error = regmap_multi_reg_write(cs40l50->regmap,
				       cs40l50_int_vamp_seq,
				       ARRAY_SIZE(cs40l50_int_vamp_seq));
	if (error)
		return error;

	error = cs_hap_pseq_multi_write(&cs40l50->haptics,
					cs40l50_int_vamp_seq,
					ARRAY_SIZE(cs40l50_int_vamp_seq),
					false, PSEQ_OP_WRITE_FULL);
	if (error)
		return error;

	error = regmap_multi_reg_write(cs40l50->regmap, cs40l50_irq_mask_seq,
				       ARRAY_SIZE(cs40l50_irq_mask_seq));
	if (error)
		return error;

	return cs_hap_pseq_multi_write(&cs40l50->haptics, cs40l50_irq_mask_seq,
				       ARRAY_SIZE(cs40l50_irq_mask_seq), false,
				       PSEQ_OP_WRITE_FULL);
}

static const struct cs_dsp_client_ops cs40l50_cs_dsp_client_ops;

static const struct cs_dsp_region cs40l50_dsp_regions[] = {
	{
		.type = WMFW_HALO_PM_PACKED,
		.base = CS40L50_DSP1_PMEM_0
	},
	{
		.type = WMFW_HALO_XM_PACKED,
		.base = CS40L50_DSP1_XMEM_PACKED_0
	},
	{
		.type = WMFW_HALO_YM_PACKED,
		.base = CS40L50_DSP1_YMEM_PACKED_0
	},
	{
		.type = WMFW_ADSP2_XM,
		.base = CS40L50_DSP1_XMEM_UNPACKED24_0
	},
	{
		.type = WMFW_ADSP2_YM,
		.base = CS40L50_DSP1_YMEM_UNPACKED24_0
	},
};

static int cs40l50_cs_dsp_init(struct cs40l50_private *cs40l50)
{
	cs40l50->dsp.num = 1;
	cs40l50->dsp.type = WMFW_HALO;
	cs40l50->dsp.dev = cs40l50->dev;
	cs40l50->dsp.regmap = cs40l50->regmap;
	cs40l50->dsp.base = CS40L50_CORE_BASE;
	cs40l50->dsp.base_sysinfo = CS40L50_SYS_INFO_ID;
	cs40l50->dsp.mem = cs40l50_dsp_regions;
	cs40l50->dsp.num_mems = ARRAY_SIZE(cs40l50_dsp_regions);
	cs40l50->dsp.no_core_startstop = true;
	cs40l50->dsp.client_ops = &cs40l50_cs_dsp_client_ops;

	return cs_dsp_halo_init(&cs40l50->dsp);
}

static struct cs_hap_bank cs40l50_banks[] = {
	{
		.bank =		WVFRM_BANK_RAM,
		.base_index =	CS40L50_RAM_BANK_INDEX_START,
		.max_index =	CS40L50_RAM_BANK_INDEX_START,
	},
	{
		.bank =		WVFRM_BANK_ROM,
		.base_index =	CS40L50_ROM_BANK_INDEX_START,
		.max_index =	CS40L50_ROM_BANK_INDEX_END,
	},
	{
		.bank =		WVFRM_BANK_OWT,
		.base_index =	CS40L50_RTH_INDEX_START,
		.max_index =	CS40L50_RTH_INDEX_END,
	},
};

static int cs40l50_cs_hap_init(struct cs40l50_private *cs40l50)
{
	cs40l50->haptics.regmap = cs40l50->regmap;
	cs40l50->haptics.dev = cs40l50->dev;
	cs40l50->haptics.banks = cs40l50_banks;
	cs40l50->haptics.dsp.gpio_base_reg = CS40L50_GPIO_BASE;
	cs40l50->haptics.dsp.owt_base_reg = CS40L50_OWT_BASE;
	cs40l50->haptics.dsp.owt_offset_reg = CS40L50_OWT_NEXT;
	cs40l50->haptics.dsp.owt_size_reg = CS40L50_OWT_SIZE;
	cs40l50->haptics.dsp.mailbox_reg = CS40L50_DSP_MBOX;
	cs40l50->haptics.dsp.pseq_reg = CS40L50_POWER_ON_SEQ;
	cs40l50->haptics.dsp.push_owt_cmd = CS40L50_OWT_PUSH;
	cs40l50->haptics.dsp.delete_owt_cmd = CS40L50_OWT_DELETE;
	cs40l50->haptics.dsp.stop_cmd = CS40L50_STOP_PLAYBACK;
	cs40l50->haptics.dsp.pseq_size = CS40L50_PSEQ_SIZE;
	cs40l50->haptics.runtime_pm = true;

	return cs_hap_init(&cs40l50->haptics);
}

static void cs40l50_upload_wt(const struct firmware *bin, void *context)
{
	struct cs40l50_private *cs40l50 = context;
	u32 nwaves;

	mutex_lock(&cs40l50->lock);

	if (cs40l50->wmfw) {
		if (regmap_write(cs40l50->regmap, CS40L50_CCM_CORE_CONTROL,
				 CS40L50_CLOCK_DISABLE))
			goto err_mutex;

		if (regmap_write(cs40l50->regmap, CS40L50_RAM_INIT,
				 CS40L50_RAM_INIT_FLAG))
			goto err_mutex;

		if (regmap_write(cs40l50->regmap, CS40L50_PWRMGT_CTL,
				 CS40L50_MEM_RDY_HW))
			goto err_mutex;
	}

	cs_dsp_power_up(&cs40l50->dsp, cs40l50->wmfw, "cs40l50.wmfw",
			bin, "cs40l50.bin", "cs40l50");

	if (cs40l50->wmfw) {
		if (regmap_write(cs40l50->regmap, CS40L50_CCM_CORE_CONTROL,
				 CS40L50_CLOCK_ENABLE))
			goto err_mutex;
	}

	if (regmap_read(cs40l50->regmap, CS40L50_NUM_OF_WAVES, &nwaves))
		goto err_mutex;

	cs40l50->haptics.banks[WVFRM_BANK_RAM].max_index += (nwaves - 1);

err_mutex:
	mutex_unlock(&cs40l50->lock);
	release_firmware(bin);
	release_firmware(cs40l50->wmfw);
}

static void cs40l50_upload_patch(const struct firmware *wmfw, void *context)
{
	struct cs40l50_private *cs40l50 = context;

	cs40l50->wmfw = wmfw;

	if (request_firmware_nowait(THIS_MODULE, FW_ACTION_UEVENT, CS40L50_WT,
				    cs40l50->dev, GFP_KERNEL,
				    cs40l50, cs40l50_upload_wt))
		release_firmware(cs40l50->wmfw);
}

static int cs40l50_input_init(struct cs40l50_private *cs40l50)
{
	int error;

	cs40l50->input = devm_input_allocate_device(cs40l50->dev);
	if (!cs40l50->input)
		return -ENOMEM;

	cs40l50->input->id.product = cs40l50->devid & 0xFFFF;
	cs40l50->input->id.version = cs40l50->revid;
	cs40l50->input->name = "cs40l50_vibra";

	input_set_drvdata(cs40l50->input, cs40l50);
	input_set_capability(cs40l50->input, EV_FF, FF_PERIODIC);
	input_set_capability(cs40l50->input, EV_FF, FF_CUSTOM);

	error = input_ff_create(cs40l50->input, FF_MAX_EFFECTS);
	if (error)
		return error;

	cs40l50->input->ff->upload = cs40l50_add;
	cs40l50->input->ff->playback = cs40l50_playback;
	cs40l50->input->ff->erase = cs40l50_erase;

	INIT_LIST_HEAD(&cs40l50->haptics.effect_head);

	error = input_register_device(cs40l50->input);
	if (error)
		goto err_free_dev;

	return cs40l50_cs_hap_init(cs40l50);

err_free_dev:
	input_free_device(cs40l50->input);
	return error;
}
static int cs40l50_vibra_probe(struct platform_device *pdev)
{
	struct cs40l50_private *cs40l50 = dev_get_drvdata(pdev->dev.parent);
	int error;

	error = cs40l50_input_init(cs40l50);
	if (error)
		return error;

	error = cs40l50_hw_init(cs40l50);
	if (error)
		goto err_input;

	error = cs40l50_cs_dsp_init(cs40l50);
	if (error)
		goto err_input;

	error = request_firmware_nowait(THIS_MODULE, FW_ACTION_UEVENT,
					CS40L50_FW, cs40l50->dev,
					GFP_KERNEL, cs40l50,
					cs40l50_upload_patch);
	if (error)
		goto err_dsp;

	return 0;

err_dsp:
	cs_dsp_remove(&cs40l50->dsp);
err_input:
	input_unregister_device(cs40l50->input);
	cs_hap_remove(&cs40l50->haptics);

	return error;
}

static int cs40l50_vibra_remove(struct platform_device *pdev)
{
	struct cs40l50_private *cs40l50 = dev_get_drvdata(pdev->dev.parent);

	input_unregister_device(cs40l50->input);
	cs_hap_remove(&cs40l50->haptics);

	if (cs40l50->dsp.booted)
		cs_dsp_power_down(&cs40l50->dsp);
	if (&cs40l50->dsp)
		cs_dsp_remove(&cs40l50->dsp);

	return 0;
}

static const struct platform_device_id cs40l50_id_vibra[] = {
	{"cs40l50-vibra", },
	{}
};
MODULE_DEVICE_TABLE(platform, cs40l50_id_vibra);

static struct platform_driver cs40l50_vibra_driver = {
	.probe		= cs40l50_vibra_probe,
	.remove		= cs40l50_vibra_remove,
	.id_table	= cs40l50_id_vibra,
	.driver		= {
		.name	= "cs40l50-vibra",
	},
};
module_platform_driver(cs40l50_vibra_driver);

MODULE_DESCRIPTION("CS40L50 Advanced Haptic Driver");
MODULE_AUTHOR("James Ogletree, Cirrus Logic Inc. <james.ogletree@cirrus.com>");
MODULE_LICENSE("GPL");

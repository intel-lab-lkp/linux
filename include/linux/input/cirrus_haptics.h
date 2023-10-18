/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Helper functions for dealing with wavetable
 * formats and DSP interfaces used by Cirrus
 * haptic drivers.
 *
 * Copyright 2023 Cirrus Logic, Inc.
 */

#ifndef __CIRRUS_HAPTICS_H
#define __CIRRUS_HAPTICS_H

#include <linux/property.h>
#include <linux/regmap.h>

/* Power-on write sequencer */
#define PSEQ_OP_MASK			GENMASK(23, 16)
#define PSEQ_OP_SHIFT			16
#define PSEQ_OP_WRITE_FULL_WORDS	3
#define PSEQ_OP_WRITE_X16_WORDS		2
#define PSEQ_OP_END_WORDS		1
#define PSEQ_OP_WRITE_FULL		0x00
#define PSEQ_OP_WRITE_ADDR8		0x02
#define PSEQ_OP_WRITE_L16		0x04
#define PSEQ_OP_WRITE_H16		0x05
#define PSEQ_OP_WRITE_UNLOCK		0xFD
#define PSEQ_OP_END			0xFF

/* Open wavetable */
#define OWT_HEADER_SIZE		12
#define OWT_TYPE_PCM		8
#define OWT_TYPE_PWLE		12
#define PCM_ID			0x0
#define CUSTOM_DATA_SIZE	2

/* GPIO */
#define BTN_NUM_MASK		GENMASK(14, 12)
#define BTN_EDGE_MASK		BIT(15)
#define GPIO_MAPPING_INVALID	0
#define GPIO_DISABLE		0x1FF

enum cs_hap_bank_type {
	WVFRM_BANK_RAM,
	WVFRM_BANK_ROM,
	WVFRM_BANK_OWT,
	WVFRM_BANK_NUM,
};

struct cs_hap_pseq_op {
	struct list_head list;
	u32 words[3];
	u16 offset;
	u8 operation;
	u8 size;
};

struct cs_hap_effect {
	enum cs_hap_bank_type bank;
	struct list_head list;
	u32 mapping;
	u32 index;
	int id;
};

struct cs_hap_pwle_header {
	u32 type;
	u32 data_words;
	u32 offset;
} __packed;

struct cs_hap_bank {
	enum cs_hap_bank_type bank;
	u32 base_index;
	u32 max_index;
};

struct cs_hap_dsp {
	u32 gpio_base_reg;
	u32 owt_offset_reg;
	u32 owt_size_reg;
	u32 owt_base_reg;
	u32 mailbox_reg;
	u32 pseq_reg;
	u32 push_owt_cmd;
	u32 delete_owt_cmd;
	u32 stop_cmd;
	u32 pseq_size;
};

struct cs_hap {
	struct regmap *regmap;
	struct mutex lock;
	struct device *dev;
	struct list_head pseq_head;
	struct cs_hap_bank *banks;
	struct cs_hap_dsp dsp;
	struct workqueue_struct *vibe_wq;
	struct work_struct vibe_start_work;
	struct work_struct vibe_stop_work;
	struct work_struct erase_work;
	struct work_struct add_work;
	struct ff_effect *start_effect;
	struct ff_effect *erase_effect;
	struct ff_effect add_effect;
	struct list_head effect_head;
	int erase_error;
	int start_error;
	int stop_error;
	int add_error;
	bool runtime_pm;
};

int cs_hap_pseq_write(struct cs_hap *haptics, u32 addr,
		      u32 data, bool update, u8 op_code);
int cs_hap_pseq_multi_write(struct cs_hap *haptics,
			    const struct reg_sequence *reg_seq,
			    int num_regs, bool update, u8 op_code);
int cs_hap_init(struct cs_hap *haptics);
void cs_hap_remove(struct cs_hap *haptics);

#endif

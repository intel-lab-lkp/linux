// SPDX-License-Identifier: GPL-2.0
//
// Copyright (c) 2018 MediaTek Inc.

#include <linux/completion.h>
#include <linux/errno.h>
#include <linux/dma-mapping.h>
#include <linux/module.h>
#include <linux/mailbox_controller.h>
#include <linux/of.h>
#include <linux/soc/mediatek/mtk-cmdq.h>

#define CMDQ_WRITE_ENABLE_MASK	BIT(0)
#define CMDQ_POLL_ENABLE_MASK	BIT(0)
/* dedicate the last GPR_R15 to assign the register address to be poll */
#define CMDQ_POLL_ADDR_GPR	(15)
#define CMDQ_EOC_IRQ_EN		BIT(0)
#define CMDQ_IMMEDIATE_VALUE	0
#define CMDQ_REG_TYPE		1
#define CMDQ_JUMP_RELATIVE	0
#define CMDQ_JUMP_ABSOLUTE	1

#define CMDQ_OPERAND_GET_IDX_VALUE(operand) \
	({ \
		struct cmdq_operand *op = operand; \
		op->reg ? op->idx : op->value; \
	})
#define CMDQ_OPERAND_TYPE(operand) \
	((operand)->reg ? CMDQ_REG_TYPE : CMDQ_IMMEDIATE_VALUE)

struct cmdq_instruction {
	union {
		u32 value;
		u32 mask;
		struct {
			u16 arg_c;
			u16 src_reg;
		};
	};
	union {
		u16 offset;
		u16 event;
		u16 reg_dst;
	};
	union {
		u8 subsys;
		struct {
			u8 sop:5;
			u8 arg_c_t:1;
			u8 src_t:1;
			u8 dst_t:1;
		};
	};
	u8 op;
};

int cmdq_dev_get_client_reg(struct device *dev,
			    struct cmdq_client_reg *client_reg, int idx)
{
	struct of_phandle_args spec;
	int err;

	if (!client_reg)
		return -ENOENT;

	err = of_parse_phandle_with_fixed_args(dev->of_node,
					       "mediatek,gce-client-reg",
					       3, idx, &spec);
	if (err < 0) {
		dev_warn(dev,
			"error %d can't parse gce-client-reg property (%d)",
			err, idx);

		return err;
	}

	client_reg->subsys = (u8)spec.args[0];
	client_reg->offset = (u16)spec.args[1];
	client_reg->size = (u16)spec.args[2];
	of_node_put(spec.np);

	return 0;
}
EXPORT_SYMBOL(cmdq_dev_get_client_reg);

struct cmdq_client *cmdq_mbox_create(struct device *dev, int index)
{
	struct cmdq_client *client;

	client = kzalloc(sizeof(*client), GFP_KERNEL);
	if (!client)
		return (struct cmdq_client *)-ENOMEM;

	client->client.dev = dev;
	client->client.tx_block = false;
	client->client.knows_txdone = true;
	client->chan = mbox_request_channel(&client->client, index);

	if (IS_ERR(client->chan)) {
		long err;

		dev_err(dev, "failed to request channel\n");
		err = PTR_ERR(client->chan);
		kfree(client);

		return ERR_PTR(err);
	}

	return client;
}
EXPORT_SYMBOL(cmdq_mbox_create);

void cmdq_mbox_destroy(struct cmdq_client *client)
{
	mbox_free_channel(client->chan);
	kfree(client);
}
EXPORT_SYMBOL(cmdq_mbox_destroy);

int cmdq_pkt_create(struct cmdq_client *client, struct cmdq_pkt *pkt, size_t size)
{
	struct device *dev;
	dma_addr_t dma_addr;

	pkt->va_base = kzalloc(size, GFP_KERNEL);
	if (!pkt->va_base)
		return -ENOMEM;

	pkt->buf_size = size;

	dev = client->chan->mbox->dev;
	dma_addr = dma_map_single(dev, pkt->va_base, pkt->buf_size,
				  DMA_TO_DEVICE);
	if (dma_mapping_error(dev, dma_addr)) {
		dev_err(dev, "dma map failed, size=%u\n", (u32)(u64)size);
		kfree(pkt->va_base);
		return -ENOMEM;
	}

	pkt->pa_base = dma_addr;

	return 0;
}
EXPORT_SYMBOL(cmdq_pkt_create);

void cmdq_pkt_destroy(struct cmdq_client *client, struct cmdq_pkt *pkt)
{
	dma_unmap_single(client->chan->mbox->dev, pkt->pa_base, pkt->buf_size,
			 DMA_TO_DEVICE);
	kfree(pkt->va_base);
}
EXPORT_SYMBOL(cmdq_pkt_destroy);

static int cmdq_pkt_append_command(struct cmdq_pkt *pkt,
				   struct cmdq_instruction inst)
{
	struct cmdq_instruction *cmd_ptr;

	if (unlikely(pkt->cmd_buf_size + CMDQ_INST_SIZE > pkt->buf_size)) {
		/*
		 * In the case of allocated buffer size (pkt->buf_size) is used
		 * up, the real required size (pkt->cmdq_buf_size) is still
		 * increased, so that the user knows how much memory should be
		 * ultimately allocated after appending all commands and
		 * flushing the command packet. Therefor, the user can call
		 * cmdq_pkt_create() again with the real required buffer size.
		 */
		pkt->cmd_buf_size += CMDQ_INST_SIZE;
		WARN_ONCE(1, "%s: buffer size %u is too small !\n",
			__func__, (u32)pkt->buf_size);
		return -ENOMEM;
	}

	cmd_ptr = pkt->va_base + pkt->cmd_buf_size;
	*cmd_ptr = inst;
	pkt->cmd_buf_size += CMDQ_INST_SIZE;

	return 0;
}

int cmdq_pkt_write(struct cmdq_pkt *pkt, u8 subsys, u16 offset, u32 value)
{
	struct cmdq_instruction inst;

	inst.op = CMDQ_CODE_WRITE;
	inst.value = value;
	inst.offset = offset;
	inst.subsys = subsys;

	return cmdq_pkt_append_command(pkt, inst);
}
EXPORT_SYMBOL(cmdq_pkt_write);

int cmdq_pkt_write_mask(struct cmdq_pkt *pkt, u8 subsys,
			u16 offset, u32 value, u32 mask)
{
	struct cmdq_instruction inst = { {0} };
	u16 offset_mask = offset;
	int err;

	if (mask != 0xffffffff) {
		inst.op = CMDQ_CODE_MASK;
		inst.mask = ~mask;
		err = cmdq_pkt_append_command(pkt, inst);
		if (err < 0)
			return err;

		offset_mask |= CMDQ_WRITE_ENABLE_MASK;
	}
	err = cmdq_pkt_write(pkt, subsys, offset_mask, value);

	return err;
}
EXPORT_SYMBOL(cmdq_pkt_write_mask);

int cmdq_pkt_read_s(struct cmdq_pkt *pkt, u16 high_addr_reg_idx, u16 addr_low,
		    u16 reg_idx)
{
	struct cmdq_instruction inst = {};

	inst.op = CMDQ_CODE_READ_S;
	inst.dst_t = CMDQ_REG_TYPE;
	inst.sop = high_addr_reg_idx;
	inst.reg_dst = reg_idx;
	inst.src_reg = addr_low;

	return cmdq_pkt_append_command(pkt, inst);
}
EXPORT_SYMBOL(cmdq_pkt_read_s);

int cmdq_pkt_write_s(struct cmdq_pkt *pkt, u16 high_addr_reg_idx,
		     u16 addr_low, u16 src_reg_idx)
{
	struct cmdq_instruction inst = {};

	inst.op = CMDQ_CODE_WRITE_S;
	inst.src_t = CMDQ_REG_TYPE;
	inst.sop = high_addr_reg_idx;
	inst.offset = addr_low;
	inst.src_reg = src_reg_idx;

	return cmdq_pkt_append_command(pkt, inst);
}
EXPORT_SYMBOL(cmdq_pkt_write_s);

int cmdq_pkt_write_s_mask(struct cmdq_pkt *pkt, u16 high_addr_reg_idx,
			  u16 addr_low, u16 src_reg_idx, u32 mask)
{
	struct cmdq_instruction inst = {};
	int err;

	inst.op = CMDQ_CODE_MASK;
	inst.mask = ~mask;
	err = cmdq_pkt_append_command(pkt, inst);
	if (err < 0)
		return err;

	inst.mask = 0;
	inst.op = CMDQ_CODE_WRITE_S_MASK;
	inst.src_t = CMDQ_REG_TYPE;
	inst.sop = high_addr_reg_idx;
	inst.offset = addr_low;
	inst.src_reg = src_reg_idx;

	return cmdq_pkt_append_command(pkt, inst);
}
EXPORT_SYMBOL(cmdq_pkt_write_s_mask);

int cmdq_pkt_write_s_value(struct cmdq_pkt *pkt, u8 high_addr_reg_idx,
			   u16 addr_low, u32 value)
{
	struct cmdq_instruction inst = {};

	inst.op = CMDQ_CODE_WRITE_S;
	inst.sop = high_addr_reg_idx;
	inst.offset = addr_low;
	inst.value = value;

	return cmdq_pkt_append_command(pkt, inst);
}
EXPORT_SYMBOL(cmdq_pkt_write_s_value);

int cmdq_pkt_write_s_mask_value(struct cmdq_pkt *pkt, u8 high_addr_reg_idx,
				u16 addr_low, u32 value, u32 mask)
{
	struct cmdq_instruction inst = {};
	int err;

	inst.op = CMDQ_CODE_MASK;
	inst.mask = ~mask;
	err = cmdq_pkt_append_command(pkt, inst);
	if (err < 0)
		return err;

	inst.op = CMDQ_CODE_WRITE_S_MASK;
	inst.sop = high_addr_reg_idx;
	inst.offset = addr_low;
	inst.value = value;

	return cmdq_pkt_append_command(pkt, inst);
}
EXPORT_SYMBOL(cmdq_pkt_write_s_mask_value);

int cmdq_pkt_mem_move(struct cmdq_pkt *pkt, dma_addr_t src_addr, dma_addr_t dst_addr)
{
	const u16 high_addr_reg_idx  = CMDQ_THR_SPR_IDX0;
	const u16 value_reg_idx = CMDQ_THR_SPR_IDX1;
	int ret;

	/* read the value of src_addr into high_addr_reg_idx */
	ret = cmdq_pkt_assign(pkt, high_addr_reg_idx, CMDQ_ADDR_HIGH(src_addr));
	if (ret < 0)
		return ret;
	ret = cmdq_pkt_read_s(pkt, high_addr_reg_idx, CMDQ_ADDR_LOW(src_addr), value_reg_idx);
	if (ret < 0)
		return ret;

	/* write the value of value_reg_idx into dst_addr */
	ret = cmdq_pkt_assign(pkt, high_addr_reg_idx, CMDQ_ADDR_HIGH(dst_addr));
	if (ret < 0)
		return ret;
	ret = cmdq_pkt_write_s(pkt, high_addr_reg_idx, CMDQ_ADDR_LOW(dst_addr), value_reg_idx);
	if (ret < 0)
		return ret;

	return 0;
}
EXPORT_SYMBOL(cmdq_pkt_mem_move);

int cmdq_pkt_wfe(struct cmdq_pkt *pkt, u16 event, bool clear)
{
	struct cmdq_instruction inst = { {0} };
	u32 clear_option = clear ? CMDQ_WFE_UPDATE : 0;

	if (event >= CMDQ_MAX_EVENT)
		return -EINVAL;

	inst.op = CMDQ_CODE_WFE;
	inst.value = CMDQ_WFE_OPTION | clear_option;
	inst.event = event;

	return cmdq_pkt_append_command(pkt, inst);
}
EXPORT_SYMBOL(cmdq_pkt_wfe);

int cmdq_pkt_acquire_event(struct cmdq_pkt *pkt, u16 event)
{
	struct cmdq_instruction inst = {};

	if (event >= CMDQ_MAX_EVENT)
		return -EINVAL;

	inst.op = CMDQ_CODE_WFE;
	inst.value = CMDQ_WFE_UPDATE | CMDQ_WFE_UPDATE_VALUE | CMDQ_WFE_WAIT;
	inst.event = event;

	return cmdq_pkt_append_command(pkt, inst);
}
EXPORT_SYMBOL(cmdq_pkt_acquire_event);

int cmdq_pkt_clear_event(struct cmdq_pkt *pkt, u16 event)
{
	struct cmdq_instruction inst = { {0} };

	if (event >= CMDQ_MAX_EVENT)
		return -EINVAL;

	inst.op = CMDQ_CODE_WFE;
	inst.value = CMDQ_WFE_UPDATE;
	inst.event = event;

	return cmdq_pkt_append_command(pkt, inst);
}
EXPORT_SYMBOL(cmdq_pkt_clear_event);

int cmdq_pkt_set_event(struct cmdq_pkt *pkt, u16 event)
{
	struct cmdq_instruction inst = {};

	if (event >= CMDQ_MAX_EVENT)
		return -EINVAL;

	inst.op = CMDQ_CODE_WFE;
	inst.value = CMDQ_WFE_UPDATE | CMDQ_WFE_UPDATE_VALUE;
	inst.event = event;

	return cmdq_pkt_append_command(pkt, inst);
}
EXPORT_SYMBOL(cmdq_pkt_set_event);

int cmdq_pkt_poll(struct cmdq_pkt *pkt, u8 subsys,
		  u16 offset, u32 value)
{
	struct cmdq_instruction inst = { {0} };
	int err;

	inst.op = CMDQ_CODE_POLL;
	inst.value = value;
	inst.offset = offset;
	inst.subsys = subsys;
	err = cmdq_pkt_append_command(pkt, inst);

	return err;
}
EXPORT_SYMBOL(cmdq_pkt_poll);

int cmdq_pkt_poll_mask(struct cmdq_pkt *pkt, u8 subsys,
		       u16 offset, u32 value, u32 mask)
{
	struct cmdq_instruction inst = { {0} };
	int err;

	inst.op = CMDQ_CODE_MASK;
	inst.mask = ~mask;
	err = cmdq_pkt_append_command(pkt, inst);
	if (err < 0)
		return err;

	offset = offset | CMDQ_POLL_ENABLE_MASK;
	err = cmdq_pkt_poll(pkt, subsys, offset, value);

	return err;
}
EXPORT_SYMBOL(cmdq_pkt_poll_mask);

int cmdq_pkt_poll_addr(struct cmdq_pkt *pkt, dma_addr_t addr, u32 value, u32 mask)
{
	struct cmdq_instruction inst = { {0} };
	u8 use_mask = 0;
	int ret;

	/*
	 * Append an MASK instruction to set the mask for following POLL instruction
	 * which enables use_mask bit.
	 */
	if (mask != GENMASK(31, 0)) {
		inst.op = CMDQ_CODE_MASK;
		inst.mask = ~mask;
		ret = cmdq_pkt_append_command(pkt, inst);
		if (ret < 0)
			return ret;
		use_mask = CMDQ_POLL_ENABLE_MASK;
	}

	/*
	 * POLL is an legacy operation in GCE and it does not support SPR and CMDQ_CODE_LOGIC,
	 * so it can not use cmdq_pkt_assign to keep polling register address to SPR.
	 * If user wants to poll a register address which doesn't have a subsys id,
	 * user needs to use GPR and CMDQ_CODE_MASK to move polling register address to GPR.
	 */
	inst.op = CMDQ_CODE_MASK;
	inst.dst_t = CMDQ_REG_TYPE;
	inst.sop = CMDQ_POLL_ADDR_GPR;
	inst.value = addr;
	ret = cmdq_pkt_append_command(pkt, inst);
	if (ret < 0)
		return ret;

	/* Append POLL instruction to poll the register address assign to GPR previously. */
	inst.op = CMDQ_CODE_POLL;
	inst.dst_t = CMDQ_REG_TYPE;
	inst.sop = CMDQ_POLL_ADDR_GPR;
	inst.offset = use_mask;
	inst.value = value;
	ret = cmdq_pkt_append_command(pkt, inst);
	if (ret < 0)
		return ret;

	return 0;
}
EXPORT_SYMBOL(cmdq_pkt_poll_addr);

int cmdq_pkt_logic_command(struct cmdq_pkt *pkt, u16 result_reg_idx,
			   struct cmdq_operand *left_operand,
			   enum cmdq_logic_op s_op,
			   struct cmdq_operand *right_operand)
{
	struct cmdq_instruction inst = { {0} };
	u32 left_idx_value;
	u32 right_idx_value;

	if (!left_operand || !right_operand || s_op >= CMDQ_LOGIC_MAX)
		return -EINVAL;

	left_idx_value = CMDQ_OPERAND_GET_IDX_VALUE(left_operand);
	right_idx_value = CMDQ_OPERAND_GET_IDX_VALUE(right_operand);
	inst.op = CMDQ_CODE_LOGIC;
	inst.dst_t = CMDQ_REG_TYPE;
	inst.src_t = CMDQ_OPERAND_TYPE(left_operand);
	inst.arg_c_t = CMDQ_OPERAND_TYPE(right_operand);
	inst.sop = s_op;
	inst.reg_dst = result_reg_idx;
	inst.src_reg = left_idx_value;
	inst.arg_c = right_idx_value;

	return cmdq_pkt_append_command(pkt, inst);
}
EXPORT_SYMBOL(cmdq_pkt_logic_command);

int cmdq_pkt_assign(struct cmdq_pkt *pkt, u16 reg_idx, u32 value)
{
	struct cmdq_instruction inst = {};

	inst.op = CMDQ_CODE_LOGIC;
	inst.dst_t = CMDQ_REG_TYPE;
	inst.reg_dst = reg_idx;
	inst.value = value;
	return cmdq_pkt_append_command(pkt, inst);
}
EXPORT_SYMBOL(cmdq_pkt_assign);

int cmdq_pkt_jump_abs(struct cmdq_pkt *pkt, dma_addr_t addr, u8 shift_pa)
{
	struct cmdq_instruction inst = {};

	inst.op = CMDQ_CODE_JUMP;
	inst.offset = CMDQ_JUMP_ABSOLUTE;
	inst.value = addr >> shift_pa;
	return cmdq_pkt_append_command(pkt, inst);
}
EXPORT_SYMBOL(cmdq_pkt_jump_abs);

int cmdq_pkt_jump_rel(struct cmdq_pkt *pkt, s32 offset, u8 shift_pa)
{
	struct cmdq_instruction inst = { {0} };

	inst.op = CMDQ_CODE_JUMP;
	inst.value = (u32)offset >> shift_pa;
	return cmdq_pkt_append_command(pkt, inst);
}
EXPORT_SYMBOL(cmdq_pkt_jump_rel);

int cmdq_pkt_eoc(struct cmdq_pkt *pkt)
{
	struct cmdq_instruction inst = { {0} };

	inst.op = CMDQ_CODE_EOC;
	inst.value = CMDQ_EOC_IRQ_EN;
	return cmdq_pkt_append_command(pkt, inst);
}
EXPORT_SYMBOL(cmdq_pkt_eoc);

int cmdq_pkt_finalize(struct cmdq_pkt *pkt)
{
	struct cmdq_instruction inst = { {0} };
	int err;

	/* insert EOC and generate IRQ for each command iteration */
	inst.op = CMDQ_CODE_EOC;
	inst.value = CMDQ_EOC_IRQ_EN;
	err = cmdq_pkt_append_command(pkt, inst);
	if (err < 0)
		return err;

	/* JUMP to end */
	inst.op = CMDQ_CODE_JUMP;
	inst.value = CMDQ_JUMP_PASS >>
		cmdq_get_shift_pa(((struct cmdq_client *)pkt->cl)->chan);
	err = cmdq_pkt_append_command(pkt, inst);

	return err;
}
EXPORT_SYMBOL(cmdq_pkt_finalize);

int cmdq_sec_insert_backup_cookie(struct cmdq_pkt *pkt)
{
	struct cmdq_client *cl = (struct cmdq_client *)pkt->cl;
	struct cmdq_operand left, right;
	dma_addr_t addr;

	addr = cmdq_sec_get_exec_cnt_addr(cl->chan);
	cmdq_pkt_assign(pkt, CMDQ_THR_SPR_IDX1, CMDQ_ADDR_HIGH(addr));
	cmdq_pkt_read_s(pkt, CMDQ_THR_SPR_IDX1, CMDQ_ADDR_LOW(addr), CMDQ_THR_SPR_IDX1);

	left.reg = true;
	left.idx = CMDQ_THR_SPR_IDX1;
	right.reg = false;
	right.value = 1;
	cmdq_pkt_logic_command(pkt, CMDQ_THR_SPR_IDX1, &left, CMDQ_LOGIC_ADD, &right);

	addr = cmdq_sec_get_cookie_addr(cl->chan);
	cmdq_pkt_assign(pkt, CMDQ_THR_SPR_IDX2, CMDQ_ADDR_HIGH(addr));
	cmdq_pkt_write_s(pkt, CMDQ_THR_SPR_IDX2, CMDQ_ADDR_LOW(addr), CMDQ_THR_SPR_IDX1);
	cmdq_pkt_set_event(pkt, cmdq_sec_get_eof_event_id(cl->chan));

	return 0;
}
EXPORT_SYMBOL_GPL(cmdq_sec_insert_backup_cookie);

static int cmdq_sec_realloc_addr_list(struct cmdq_pkt *pkt, const u32 count)
{
	struct cmdq_sec_data *sec_data = (struct cmdq_sec_data *)pkt->sec_data;
	void *prev = (void *)(unsigned long)sec_data->addr_metadatas, *curr;

	if (count <= sec_data->addr_metadata_max_cnt)
		return 0;

	curr = kcalloc(count, sizeof(*sec_data), GFP_KERNEL);
	if (!curr)
		return -ENOMEM;

	if (count && sec_data->addr_metadatas)
		memcpy(curr, prev, sizeof(*sec_data) * sec_data->addr_metadata_max_cnt);

	kfree(prev);

	sec_data->addr_metadatas = (uintptr_t)curr;
	sec_data->addr_metadata_max_cnt = count;
	return 0;
}

void cmdq_sec_pkt_free_sec_data(struct cmdq_pkt *pkt)
{
	kfree(pkt->sec_data);
}
EXPORT_SYMBOL_GPL(cmdq_sec_pkt_free_sec_data);

int cmdq_sec_pkt_alloc_sec_data(struct cmdq_pkt *pkt)
{
	struct cmdq_sec_data *sec_data;

	if (pkt->sec_data)
		return 0;

	sec_data = kzalloc(sizeof(*sec_data), GFP_KERNEL);
	if (!sec_data)
		return -ENOMEM;

	pkt->sec_data = (void *)sec_data;

	return 0;
}
EXPORT_SYMBOL_GPL(cmdq_sec_pkt_alloc_sec_data);

static int cmdq_sec_append_metadata(struct cmdq_pkt *pkt,
				    const enum cmdq_iwc_addr_metadata_type type,
				    const u32 base, const u32 offset)
{
	struct cmdq_sec_data *sec_data;
	struct iwc_cmdq_addr_metadata_t *meta;
	int idx, max, ret;

	pr_debug("[%s %d] pkt:%p type:%u base:%#x offset:%#x",
		 __func__, __LINE__, pkt, type, base, offset);

	ret = cmdq_sec_pkt_alloc_sec_data(pkt);
	if (ret < 0)
		return ret;

	sec_data = (struct cmdq_sec_data *)pkt->sec_data;
	idx = sec_data->addr_metadata_cnt;
	if (idx >= CMDQ_IWC_MAX_ADDR_LIST_LENGTH) {
		pr_err("idx:%u reach over:%u", idx, CMDQ_IWC_MAX_ADDR_LIST_LENGTH);
		return -EFAULT;
	}

	if (!sec_data->addr_metadata_max_cnt)
		max = ADDR_METADATA_MAX_COUNT_ORIGIN;
	else if (idx >= sec_data->addr_metadata_max_cnt)
		max = sec_data->addr_metadata_max_cnt * 2;
	else
		max = sec_data->addr_metadata_max_cnt;

	ret = cmdq_sec_realloc_addr_list(pkt, max);
	if (ret)
		return ret;

	if (!sec_data->addr_metadatas) {
		pr_info("addr_metadatas is missing");

		meta = kzalloc(sizeof(*meta), GFP_KERNEL);
		if (!meta)
			return -ENOMEM;

		sec_data->addr_metadatas = (uintptr_t)(void *)meta;
	}
	meta = (struct iwc_cmdq_addr_metadata_t *)(uintptr_t)sec_data->addr_metadatas;

	meta[idx].type = type;
	meta[idx].base_handle = base;
	meta[idx].offset = offset;
	sec_data->addr_metadata_cnt += 1;
	return 0;
}

int cmdq_sec_pkt_set_data(struct cmdq_pkt *pkt, enum cmdq_sec_scenario scenario)
{
	struct cmdq_sec_data *sec_data;
	int ret;

	if (!pkt) {
		pr_err("invalid pkt:%p", pkt);
		return -EINVAL;
	}

	ret = cmdq_sec_pkt_alloc_sec_data(pkt);
	if (ret < 0)
		return ret;

	pr_debug("[%s %d] pkt:%p sec_data:%p scen:%u",
		 __func__, __LINE__, pkt, pkt->sec_data, scenario);

	sec_data = (struct cmdq_sec_data *)pkt->sec_data;
	sec_data->scenario = scenario;

	return 0;
}
EXPORT_SYMBOL_GPL(cmdq_sec_pkt_set_data);

int cmdq_sec_pkt_write(struct cmdq_pkt *pkt, u8 subsys, u16 offset,
		       enum cmdq_iwc_addr_metadata_type type,
		       u32 base, u32 base_offset)
{
	cmdq_pkt_write(pkt, subsys, offset, base);

	return cmdq_sec_append_metadata(pkt, type, base, base_offset);
}
EXPORT_SYMBOL_GPL(cmdq_sec_pkt_write);

MODULE_LICENSE("GPL v2");

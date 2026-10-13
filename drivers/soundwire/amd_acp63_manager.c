// SPDX-License-Identifier: (GPL-2.0 OR BSD-3-Clause)
/*
 * SoundWire AMD ACP6.3 (and compatible) Manager hardware ops
 *
 * Copyright 2023-24, 2026 Advanced Micro Devices, Inc.
 */

#include <linux/completion.h>
#include <linux/device.h>
#include <linux/io.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/soundwire/sdw.h>
#include <linux/soundwire/sdw_registers.h>
#include <linux/pm_runtime.h>
#include <linux/wait.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>
#include "bus.h"
#include "amd_init.h"
#include "amd_manager.h"

#define to_amd_sdw(b)	container_of(b, struct amd_sdw_manager, bus)

struct sdw_manager_dp_reg {
	u32 frame_fmt_reg;
	u32 sample_int_reg;
	u32 hctrl_dp0_reg;
	u32 offset_reg;
	u32 lane_ctrl_ch_en_reg;
};

/*
 * SDW0 Manager instance registers  6 CPU DAI (3 TX & 3 RX Ports)
 * whereas SDW1  Manager Instance registers 2 CPU DAI (one TX & one RX port)
 * Below is the CPU DAI <->Manager port number mapping
 * i.e SDW0 Pin0 -> port number 0 -> AUDIO0 TX
 *     SDW0 Pin1 -> Port number 1 -> AUDIO1 TX
 *     SDW0 Pin2 -> Port number 2 -> AUDIO2 TX
 *     SDW0 Pin3 -> port number 3 -> AUDIO0 RX
 *     SDW0 Pin4 -> Port number 4 -> AUDIO1 RX
 *     SDW0 Pin5 -> Port number 5 -> AUDIO2 RX
 *  Whereas for SDW1 instance
 *  SDW1 Pin0 -> port number 0 -> AUDIO1 TX
 *  SDW1 Pin1 -> Port number 1 -> AUDIO1 RX
 *  Same mapping should be used for programming DMA controller registers in SoundWire DMA driver.
 * i.e if AUDIO0 TX channel is selected then we need to use AUDIO0 TX registers for DMA programming
 * in SoundWire DMA driver.
 */

static struct sdw_manager_dp_reg acp63_sdw0_dp_reg[AMD_ACP63_SDW0_MAX_DAI] =  {
	{ACP_SW_AUDIO0_TX_FRAME_FORMAT, ACP_SW_AUDIO0_TX_SAMPLEINTERVAL, ACP_SW_AUDIO0_TX_HCTRL_DP0,
	 ACP_SW_AUDIO0_TX_OFFSET_DP0, ACP_SW_AUDIO0_TX_CHANNEL_ENABLE_DP0},
	{ACP_SW_AUDIO1_TX_FRAME_FORMAT, ACP_SW_AUDIO1_TX_SAMPLEINTERVAL, ACP_SW_AUDIO1_TX_HCTRL,
	 ACP_SW_AUDIO1_TX_OFFSET, ACP_SW_AUDIO1_TX_CHANNEL_ENABLE_DP0},
	{ACP_SW_AUDIO2_TX_FRAME_FORMAT, ACP_SW_AUDIO2_TX_SAMPLEINTERVAL, ACP_SW_AUDIO2_TX_HCTRL,
	 ACP_SW_AUDIO2_TX_OFFSET, ACP_SW_AUDIO2_TX_CHANNEL_ENABLE_DP0},
	{ACP_SW_AUDIO0_RX_FRAME_FORMAT, ACP_SW_AUDIO0_RX_SAMPLEINTERVAL, ACP_SW_AUDIO0_RX_HCTRL_DP0,
	 ACP_SW_AUDIO0_RX_OFFSET_DP0, ACP_SW_AUDIO0_RX_CHANNEL_ENABLE_DP0},
	{ACP_SW_AUDIO1_RX_FRAME_FORMAT, ACP_SW_AUDIO1_RX_SAMPLEINTERVAL, ACP_SW_AUDIO1_RX_HCTRL,
	 ACP_SW_AUDIO1_RX_OFFSET, ACP_SW_AUDIO1_RX_CHANNEL_ENABLE_DP0},
	{ACP_SW_AUDIO2_RX_FRAME_FORMAT, ACP_SW_AUDIO2_RX_SAMPLEINTERVAL, ACP_SW_AUDIO2_RX_HCTRL,
	 ACP_SW_AUDIO2_RX_OFFSET, ACP_SW_AUDIO2_RX_CHANNEL_ENABLE_DP0},
};

static struct sdw_manager_dp_reg acp63_sdw1_dp_reg[AMD_ACP63_SDW1_MAX_DAI] =  {
	{ACP_SW_AUDIO1_TX_FRAME_FORMAT, ACP_SW_AUDIO1_TX_SAMPLEINTERVAL, ACP_SW_AUDIO1_TX_HCTRL,
	 ACP_SW_AUDIO1_TX_OFFSET, ACP_SW_AUDIO1_TX_CHANNEL_ENABLE_DP0},
	{ACP_SW_AUDIO1_RX_FRAME_FORMAT, ACP_SW_AUDIO1_RX_SAMPLEINTERVAL, ACP_SW_AUDIO1_RX_HCTRL,
	 ACP_SW_AUDIO1_RX_OFFSET, ACP_SW_AUDIO1_RX_CHANNEL_ENABLE_DP0}
};

static struct sdw_manager_dp_reg acp70_sdw_dp_reg[AMD_ACP70_SDW_MAX_DAI] =  {
	{ACP_SW_AUDIO0_TX_FRAME_FORMAT, ACP_SW_AUDIO0_TX_SAMPLEINTERVAL, ACP_SW_AUDIO0_TX_HCTRL_DP0,
	 ACP_SW_AUDIO0_TX_OFFSET_DP0, ACP_SW_AUDIO0_TX_CHANNEL_ENABLE_DP0},
	{ACP_SW_AUDIO1_TX_FRAME_FORMAT, ACP_SW_AUDIO1_TX_SAMPLEINTERVAL, ACP_SW_AUDIO1_TX_HCTRL,
	 ACP_SW_AUDIO1_TX_OFFSET, ACP_SW_AUDIO1_TX_CHANNEL_ENABLE_DP0},
	{ACP_SW_AUDIO2_TX_FRAME_FORMAT, ACP_SW_AUDIO2_TX_SAMPLEINTERVAL, ACP_SW_AUDIO2_TX_HCTRL,
	 ACP_SW_AUDIO2_TX_OFFSET, ACP_SW_AUDIO2_TX_CHANNEL_ENABLE_DP0},
	{ACP_SW_AUDIO0_RX_FRAME_FORMAT, ACP_SW_AUDIO0_RX_SAMPLEINTERVAL, ACP_SW_AUDIO0_RX_HCTRL_DP0,
	 ACP_SW_AUDIO0_RX_OFFSET_DP0, ACP_SW_AUDIO0_RX_CHANNEL_ENABLE_DP0},
	{ACP_SW_AUDIO1_RX_FRAME_FORMAT, ACP_SW_AUDIO1_RX_SAMPLEINTERVAL, ACP_SW_AUDIO1_RX_HCTRL,
	 ACP_SW_AUDIO1_RX_OFFSET, ACP_SW_AUDIO1_RX_CHANNEL_ENABLE_DP0},
	{ACP_SW_AUDIO2_RX_FRAME_FORMAT, ACP_SW_AUDIO2_RX_SAMPLEINTERVAL, ACP_SW_AUDIO2_RX_HCTRL,
	 ACP_SW_AUDIO2_RX_OFFSET, ACP_SW_AUDIO2_RX_CHANNEL_ENABLE_DP0},
};

static u32 sdw_manager_reg_mask_array[AMD_ACP63_SDW_MAX_MANAGER_COUNT] =  {
		AMD_SDW0_EXT_INTR_MASK,
		AMD_SDW1_EXT_INTR_MASK
};

static int amd_acp63_sdw_clk_init_ctrl(struct amd_sdw_manager *amd_manager)
{
	struct sdw_bus *bus = &amd_manager->bus;
	struct sdw_master_prop *prop = &bus->prop;
	u32 divider;

	dev_dbg(amd_manager->dev, "mclk %d max %d row %d col %d frame_rate:%d\n",
		prop->mclk_freq, prop->max_clk_freq, prop->default_row,
		prop->default_col, prop->default_frame_rate);

	if (!prop->default_frame_rate || !prop->default_row) {
		dev_err(amd_manager->dev, "Default frame_rate %d or row %d is invalid\n",
			prop->default_frame_rate, prop->default_row);
		return -EINVAL;
	}

	/* Set clock divider */
	divider = (prop->mclk_freq / bus->params.curr_dr_freq);
	writel(divider, amd_manager->mmio + ACP_SW_CLK_FREQUENCY_CTRL);

	/* Set frame shape base on the actual bus frequency. */
	prop->default_col = bus->params.curr_dr_freq /
			    prop->default_frame_rate / prop->default_row;
	amd_manager->cols_index = sdw_find_col_index(prop->default_col);
	amd_manager->rows_index = sdw_find_row_index(prop->default_row);
	bus->params.col = prop->default_col;
	bus->params.row = prop->default_row;
	return 0;
}

static int amd_acp63_init_sdw_manager(struct amd_sdw_manager *amd_manager)
{
	u32 val;
	int ret;

	writel(AMD_SDW_ENABLE, amd_manager->mmio + ACP_SW_EN);
	ret = readl_poll_timeout(amd_manager->mmio + ACP_SW_EN_STATUS, val, val, ACP_DELAY_US,
				 AMD_SDW_TIMEOUT);
	if (ret)
		return ret;

	/* SoundWire manager bus reset */
	writel(AMD_SDW_BUS_RESET_REQ, amd_manager->mmio + ACP_SW_BUS_RESET_CTRL);
	ret = readl_poll_timeout(amd_manager->mmio + ACP_SW_BUS_RESET_CTRL, val,
				 (val & AMD_SDW_BUS_RESET_DONE), ACP_DELAY_US, AMD_SDW_TIMEOUT);
	if (ret)
		return ret;

	writel(AMD_SDW_BUS_RESET_CLEAR_REQ, amd_manager->mmio + ACP_SW_BUS_RESET_CTRL);
	ret = readl_poll_timeout(amd_manager->mmio + ACP_SW_BUS_RESET_CTRL, val, !val,
				 ACP_DELAY_US, AMD_SDW_TIMEOUT);
	if (ret) {
		dev_err(amd_manager->dev, "Failed to reset SoundWire manager instance%d\n",
			amd_manager->instance);
		return ret;
	}

	writel(AMD_SDW_DISABLE, amd_manager->mmio + ACP_SW_EN);
	return readl_poll_timeout(amd_manager->mmio + ACP_SW_EN_STATUS, val, !val, ACP_DELAY_US,
				  AMD_SDW_TIMEOUT);
}

static int amd_acp63_enable_sdw_manager(struct amd_sdw_manager *amd_manager)
{
	u32 val;

	writel(AMD_SDW_ENABLE, amd_manager->mmio + ACP_SW_EN);
	return readl_poll_timeout(amd_manager->mmio + ACP_SW_EN_STATUS, val, val, ACP_DELAY_US,
				  AMD_SDW_TIMEOUT);
}

static int amd_acp63_disable_sdw_manager(struct amd_sdw_manager *amd_manager)
{
	u32 val;

	writel(AMD_SDW_DISABLE, amd_manager->mmio + ACP_SW_EN);
	/*
	 * After invoking manager disable sequence, check whether
	 * manager has executed clock stop sequence. In this case,
	 * manager should ignore checking enable status register.
	 */
	val = readl(amd_manager->mmio + ACP_SW_CLK_RESUME_CTRL);
	if (val)
		return 0;
	return readl_poll_timeout(amd_manager->mmio + ACP_SW_EN_STATUS, val, !val, ACP_DELAY_US,
				  AMD_SDW_TIMEOUT);
}

static void amd_acp63_enable_sdw_interrupts(struct amd_sdw_manager *amd_manager)
{
	u32 val;

	mutex_lock(amd_manager->acp_sdw_lock);
	val = sdw_manager_reg_mask_array[amd_manager->instance];
	amd_updatel(amd_manager->acp_mmio, ACP_EXTERNAL_INTR_CNTL(amd_manager->instance), val, val);
	mutex_unlock(amd_manager->acp_sdw_lock);

	writel(AMD_SDW_IRQ_MASK_0TO7, amd_manager->mmio +
		       ACP_SW_STATE_CHANGE_STATUS_MASK_0TO7);
	writel(AMD_SDW_IRQ_MASK_8TO11, amd_manager->mmio +
		       ACP_SW_STATE_CHANGE_STATUS_MASK_8TO11);
	writel(AMD_SDW_IRQ_ERROR_MASK, amd_manager->mmio + ACP_SW_ERROR_INTR_MASK);
}

static void amd_acp63_disable_sdw_interrupts(struct amd_sdw_manager *amd_manager)
{
	u32 irq_mask;

	mutex_lock(amd_manager->acp_sdw_lock);
	irq_mask = sdw_manager_reg_mask_array[amd_manager->instance];
	amd_updatel(amd_manager->acp_mmio, ACP_EXTERNAL_INTR_CNTL(amd_manager->instance),
		    irq_mask, 0);
	mutex_unlock(amd_manager->acp_sdw_lock);

	writel(0x00, amd_manager->mmio + ACP_SW_STATE_CHANGE_STATUS_MASK_0TO7);
	writel(0x00, amd_manager->mmio + ACP_SW_STATE_CHANGE_STATUS_MASK_8TO11);
	writel(0x00, amd_manager->mmio + ACP_SW_ERROR_INTR_MASK);
}

static void amd_acp63_sdw_set_frameshape(struct amd_sdw_manager *amd_manager)
{
	u32 frame_size;

	frame_size = (amd_manager->rows_index << 3) | amd_manager->cols_index;
	writel(frame_size, amd_manager->mmio + ACP_SW_FRAMESIZE);
}

static void amd_acp63_sdw_wake_enable(struct amd_sdw_manager *amd_manager, bool enable)
{
	u32 wake_ctrl;

	wake_ctrl = readl(amd_manager->mmio + ACP_SW_STATE_CHANGE_STATUS_MASK_8TO11);
	if (enable)
		wake_ctrl |= AMD_SDW_WAKE_INTR_MASK;
	else
		wake_ctrl &= ~AMD_SDW_WAKE_INTR_MASK;

	writel(wake_ctrl, amd_manager->mmio + ACP_SW_STATE_CHANGE_STATUS_MASK_8TO11);
}

static int amd_acp63_sdw_set_device_state(struct amd_sdw_manager *amd_manager,
					  u32 target_device_state)
{
	u32 sdw_dev_state;

	sdw_dev_state = readl(amd_manager->acp_mmio + AMD_SDW_DEVICE_STATE);
	switch (amd_manager->instance) {
	case ACP_SDW0:
		u32p_replace_bits(&sdw_dev_state, target_device_state,
				  AMD_SDW0_DEVICE_STATE_MASK);
		break;
	case ACP_SDW1:
		u32p_replace_bits(&sdw_dev_state, target_device_state,
				  AMD_SDW1_DEVICE_STATE_MASK);
		break;
	default:
		return -EINVAL;
	}
	writel(sdw_dev_state, amd_manager->acp_mmio + AMD_SDW_DEVICE_STATE);
	sdw_dev_state = readl(amd_manager->acp_mmio + AMD_SDW_DEVICE_STATE);
	dev_dbg(amd_manager->dev, "AMD_SDW_DEVICE_STATE:0x%x\n", sdw_dev_state);
	return 0;
}

static int amd_acp63_sdw_host_wake_enable(struct amd_sdw_manager *amd_manager, bool enable)
{
	u32 intr_cntl1;
	u32 sdw_host_wake_irq_mask;

	if (!amd_manager->wake_en_mask)
		return 0;

	switch (amd_manager->instance) {
	case ACP_SDW0:
		sdw_host_wake_irq_mask = AMD_SDW0_HOST_WAKE_INTR_MASK;
		break;
	case ACP_SDW1:
		sdw_host_wake_irq_mask = AMD_SDW1_HOST_WAKE_INTR_MASK;
		break;
	default:
		return -EINVAL;
	}

	intr_cntl1 = readl(amd_manager->acp_mmio + ACP_EXTERNAL_INTR_CNTL(ACP_SDW1));
	if (enable)
		intr_cntl1 |= sdw_host_wake_irq_mask;
	else
		intr_cntl1 &= ~sdw_host_wake_irq_mask;
	writel(intr_cntl1, amd_manager->acp_mmio + ACP_EXTERNAL_INTR_CNTL(ACP_SDW1));
	return 0;
}

static int amd_acp63_sdw_send_cmd_get_resp(struct amd_sdw_manager *amd_manager, u32 lower_data,
					   u32 upper_data, u64 *response)
{
	u32 lower_resp, upper_resp;
	u32 sts;
	int ret;

	ret = readl_poll_timeout(amd_manager->mmio + ACP_SW_IMM_CMD_STS, sts,
				 !(sts & AMD_SDW_IMM_CMD_BUSY), ACP_DELAY_US, AMD_SDW_TIMEOUT);
	if (ret) {
		dev_err(amd_manager->dev, "SDW%x previous cmd status clear failed\n",
			amd_manager->instance);
		return ret;
	}

	if (sts & AMD_SDW_IMM_RES_VALID) {
		dev_err(amd_manager->dev, "SDW%x manager is in bad state\n", amd_manager->instance);
		writel(AMD_SDW_IMM_RES_VALID, amd_manager->mmio + ACP_SW_IMM_CMD_STS);
	}
	writel(upper_data, amd_manager->mmio + ACP_SW_IMM_CMD_UPPER_WORD);
	writel(lower_data, amd_manager->mmio + ACP_SW_IMM_CMD_LOWER_QWORD);

	ret = readl_poll_timeout(amd_manager->mmio + ACP_SW_IMM_CMD_STS, sts,
				 (sts & AMD_SDW_IMM_RES_VALID), ACP_DELAY_US, AMD_SDW_TIMEOUT);
	if (ret) {
		dev_err(amd_manager->dev, "SDW%x cmd response timeout occurred\n",
			amd_manager->instance);
		return ret;
	}
	upper_resp = readl(amd_manager->mmio + ACP_SW_IMM_RESP_UPPER_WORD);
	lower_resp = readl(amd_manager->mmio + ACP_SW_IMM_RESP_LOWER_QWORD);

	writel(AMD_SDW_IMM_RES_VALID, amd_manager->mmio + ACP_SW_IMM_CMD_STS);
	ret = readl_poll_timeout(amd_manager->mmio + ACP_SW_IMM_CMD_STS, sts,
				 !(sts & AMD_SDW_IMM_RES_VALID), ACP_DELAY_US, AMD_SDW_TIMEOUT);
	if (ret) {
		dev_err(amd_manager->dev, "SDW%x cmd status retry failed\n",
			amd_manager->instance);
		return ret;
	}
	*response = ((u64)upper_resp << 32) | lower_resp;
	return 0;
}

static void amd_acp63_sdw_read_and_process_ping_status(struct amd_sdw_manager *amd_manager)
{
	u64 response;

	mutex_lock(&amd_manager->bus.msg_lock);
	if (acp_sdw_send_cmd_get_resp(amd_manager, 0, 0, &response)) {
		mutex_unlock(&amd_manager->bus.msg_lock);
		dev_err_ratelimited(amd_manager->dev, "SDW%x ping status timeout\n",
				    amd_manager->instance);
		return;
	}
	amd_manager->ping_slave_stat =
		FIELD_GET(AMD_SDW_MCP_SLAVE_STAT_0_3, response) |
		(FIELD_GET(AMD_SDW_MCP_SLAVE_STAT_4_11, response) << 8);
	amd_sdw_process_ping_status(response, amd_manager);
	mutex_unlock(&amd_manager->bus.msg_lock);
}

static u32 amd_acp63_sdw_read_ping_status(struct sdw_bus *bus)
{
	struct amd_sdw_manager *amd_manager = to_amd_sdw(bus);
	u64 response;
	u32 slave_stat;

	/* Called by sdw_show_ping_status() which holds msg_lock. */
	lockdep_assert_held(&bus->msg_lock);

	if (acp_sdw_send_cmd_get_resp(amd_manager, 0, 0, &response)) {
		dev_err_ratelimited(amd_manager->dev, "SDW%x ping status timeout\n",
				    amd_manager->instance);
		return amd_manager->ping_slave_stat == U32_MAX ? 0 : amd_manager->ping_slave_stat;
	}
	/* slave status from ping response */
	slave_stat = FIELD_GET(AMD_SDW_MCP_SLAVE_STAT_0_3, response);
	slave_stat |= FIELD_GET(AMD_SDW_MCP_SLAVE_STAT_4_11, response) << 8;
	dev_dbg(amd_manager->dev, "slave_stat:0x%x\n", slave_stat);
	amd_manager->ping_slave_stat = slave_stat;
	return slave_stat;
}

int amd_acp63_sdw_compute_params(struct sdw_bus *bus, struct sdw_stream_runtime *stream)
{
	struct amd_sdw_manager *amd_manager = to_amd_sdw(bus);
	struct sdw_transport_data t_data = {0};
	struct sdw_master_runtime *m_rt;
	struct sdw_port_runtime *p_rt;
	struct sdw_bus_params *b_params = &bus->params;
	int port_bo, hstart, hstop, sample_int;
	unsigned int rate, bps, channels;
	unsigned int stream_slot_size, max_slots;
	static unsigned int next_offset[AMD_ACP63_SDW_MAX_MANAGER_COUNT] = {1};
	unsigned int inst_id = amd_manager->instance;

	port_bo = 0;
	hstart = 1;
	hstop = bus->params.col - 1;
	t_data.hstop = hstop;
	t_data.hstart = hstart;

	list_for_each_entry(m_rt, &bus->m_rt_list, bus_node) {
		rate = m_rt->stream->params.rate;
		bps = m_rt->stream->params.bps;
		channels = m_rt->stream->params.ch_count;
		sample_int = (bus->params.curr_dr_freq / rate);

		/* Compute slots required for this stream dynamically */
		stream_slot_size = bps * channels;

		list_for_each_entry(p_rt, &m_rt->port_list, port_node) {
			if (p_rt->num >= amd_manager->max_ports) {
				dev_err(bus->dev, "Port %d exceeds max ports %d\n",
					p_rt->num, amd_manager->max_ports);
				return -EINVAL;
			}

			if (!amd_manager->port_offset_map[p_rt->num]) {
				/*
				 * port block offset calculation for 6MHz bus clock frequency with
				 * different frame sizes 50 x 10 and 125 x 2
				 */
				if (bus->params.curr_dr_freq == 12000000) {
					max_slots = bus->params.row * (bus->params.col - 1);
					if (next_offset[inst_id] + stream_slot_size <=
					    (max_slots - 1)) {
						amd_manager->port_offset_map[p_rt->num] =
									next_offset[inst_id];
						next_offset[inst_id] += stream_slot_size;
					} else {
						dev_err(bus->dev,
							"No space for port %d\n", p_rt->num);
						return -ENOMEM;
					}
				} else {
					 /*
					  * port block offset calculation for 12MHz bus clock
					  * frequency
					  */
					amd_manager->port_offset_map[p_rt->num] =
									(p_rt->num * 64) + 1;
				}
			}
			port_bo = amd_manager->port_offset_map[p_rt->num];
			dev_dbg(bus->dev,
				"Port=%d hstart=%d hstop=%d port_bo=%d slots=%d max_ports=%d\n",
				p_rt->num, hstart, hstop, port_bo, stream_slot_size,
				amd_manager->max_ports);

			sdw_fill_xport_params(&p_rt->transport_params, p_rt->num,
					      false, SDW_BLK_GRP_CNT_1, sample_int,
					      port_bo, port_bo >> 8, hstart, hstop,
					      SDW_BLK_PKG_PER_PORT, p_rt->lane);

			sdw_fill_port_params(&p_rt->port_params,
					     p_rt->num, bps,
					     SDW_PORT_FLOW_MODE_ISOCH,
					     b_params->m_data_mode);
			t_data.hstart = hstart;
			t_data.hstop = hstop;
			t_data.block_offset = port_bo;
			t_data.sub_block_offset = 0;
		}
		sdw_compute_slave_ports(m_rt, &t_data);
	}
	return 0;
}

static int amd_acp63_sdw_port_params(struct sdw_bus *bus, struct sdw_port_params *p_params,
				     unsigned int bank)
{
	struct amd_sdw_manager *amd_manager = to_amd_sdw(bus);
	u32 frame_fmt_reg, dpn_frame_fmt;

	dev_dbg(amd_manager->dev, "p_params->num:0x%x\n", p_params->num);
	switch (amd_manager->acp_rev) {
	case ACP63_PCI_REV_ID:
		switch (amd_manager->instance) {
		case ACP_SDW0:
			frame_fmt_reg = acp63_sdw0_dp_reg[p_params->num].frame_fmt_reg;
			break;
		case ACP_SDW1:
			frame_fmt_reg = acp63_sdw1_dp_reg[p_params->num].frame_fmt_reg;
			break;
		default:
			return -EINVAL;
		}
		break;
	case ACP70_PCI_REV_ID:
	case ACP71_PCI_REV_ID:
	case ACP72_PCI_REV_ID:
		frame_fmt_reg = acp70_sdw_dp_reg[p_params->num].frame_fmt_reg;
		break;
	default:
		return -EINVAL;
	}

	dpn_frame_fmt = readl(amd_manager->mmio + frame_fmt_reg);
	u32p_replace_bits(&dpn_frame_fmt, p_params->flow_mode, AMD_DPN_FRAME_FMT_PFM);
	u32p_replace_bits(&dpn_frame_fmt, p_params->data_mode, AMD_DPN_FRAME_FMT_PDM);
	u32p_replace_bits(&dpn_frame_fmt, p_params->bps - 1, AMD_DPN_FRAME_FMT_WORD_LEN);
	writel(dpn_frame_fmt, amd_manager->mmio + frame_fmt_reg);
	return 0;
}

static int amd_acp63_sdw_transport_params(struct sdw_bus *bus,
					  struct sdw_transport_params *params,
					  enum sdw_reg_bank bank)
{
	struct amd_sdw_manager *amd_manager = to_amd_sdw(bus);
	u32 dpn_frame_fmt;
	u32 dpn_sampleinterval;
	u32 dpn_hctrl;
	u32 dpn_offsetctrl;
	u32 dpn_lanectrl;
	u32 frame_fmt_reg, sample_int_reg, hctrl_dp0_reg;
	u32 offset_reg, lane_ctrl_ch_en_reg;

	switch (amd_manager->acp_rev) {
	case ACP63_PCI_REV_ID:
		switch (amd_manager->instance) {
		case ACP_SDW0:
			frame_fmt_reg = acp63_sdw0_dp_reg[params->port_num].frame_fmt_reg;
			sample_int_reg = acp63_sdw0_dp_reg[params->port_num].sample_int_reg;
			hctrl_dp0_reg = acp63_sdw0_dp_reg[params->port_num].hctrl_dp0_reg;
			offset_reg = acp63_sdw0_dp_reg[params->port_num].offset_reg;
			lane_ctrl_ch_en_reg =
					acp63_sdw0_dp_reg[params->port_num].lane_ctrl_ch_en_reg;
			break;
		case ACP_SDW1:
			frame_fmt_reg = acp63_sdw1_dp_reg[params->port_num].frame_fmt_reg;
			sample_int_reg = acp63_sdw1_dp_reg[params->port_num].sample_int_reg;
			hctrl_dp0_reg = acp63_sdw1_dp_reg[params->port_num].hctrl_dp0_reg;
			offset_reg = acp63_sdw1_dp_reg[params->port_num].offset_reg;
			lane_ctrl_ch_en_reg =
					acp63_sdw1_dp_reg[params->port_num].lane_ctrl_ch_en_reg;
			break;
		default:
			return -EINVAL;
		}
		break;
	case ACP70_PCI_REV_ID:
	case ACP71_PCI_REV_ID:
	case ACP72_PCI_REV_ID:
		frame_fmt_reg = acp70_sdw_dp_reg[params->port_num].frame_fmt_reg;
		sample_int_reg = acp70_sdw_dp_reg[params->port_num].sample_int_reg;
		hctrl_dp0_reg = acp70_sdw_dp_reg[params->port_num].hctrl_dp0_reg;
		offset_reg = acp70_sdw_dp_reg[params->port_num].offset_reg;
		lane_ctrl_ch_en_reg = acp70_sdw_dp_reg[params->port_num].lane_ctrl_ch_en_reg;
		break;
	default:
		return -EINVAL;
	}
	writel(AMD_SDW_SSP_COUNTER_VAL, amd_manager->mmio + ACP_SW_SSP_COUNTER);

	dpn_frame_fmt = readl(amd_manager->mmio + frame_fmt_reg);
	u32p_replace_bits(&dpn_frame_fmt, params->blk_pkg_mode, AMD_DPN_FRAME_FMT_BLK_PKG_MODE);
	u32p_replace_bits(&dpn_frame_fmt, params->blk_grp_ctrl, AMD_DPN_FRAME_FMT_BLK_GRP_CTRL);
	u32p_replace_bits(&dpn_frame_fmt, SDW_STREAM_PCM, AMD_DPN_FRAME_FMT_PCM_OR_PDM);
	writel(dpn_frame_fmt, amd_manager->mmio + frame_fmt_reg);

	dpn_sampleinterval = params->sample_interval - 1;
	writel(dpn_sampleinterval, amd_manager->mmio + sample_int_reg);

	dpn_hctrl = FIELD_PREP(AMD_DPN_HCTRL_HSTOP, params->hstop);
	dpn_hctrl |= FIELD_PREP(AMD_DPN_HCTRL_HSTART, params->hstart);
	writel(dpn_hctrl, amd_manager->mmio + hctrl_dp0_reg);

	dpn_offsetctrl = FIELD_PREP(AMD_DPN_OFFSET_CTRL_1, params->offset1);
	dpn_offsetctrl |= FIELD_PREP(AMD_DPN_OFFSET_CTRL_2, params->offset2);
	writel(dpn_offsetctrl, amd_manager->mmio + offset_reg);

	/*
	 * lane_ctrl_ch_en_reg will be used to program lane_ctrl and ch_mask
	 * parameters.
	 */
	dpn_lanectrl = readl(amd_manager->mmio + lane_ctrl_ch_en_reg);
	u32p_replace_bits(&dpn_lanectrl, params->lane_ctrl, AMD_DPN_CH_EN_LCTRL);
	writel(dpn_lanectrl, amd_manager->mmio + lane_ctrl_ch_en_reg);
	return 0;
}

static int amd_acp63_sdw_port_enable(struct sdw_bus *bus,
				     struct sdw_enable_ch *enable_ch,
				     unsigned int bank)
{
	struct amd_sdw_manager *amd_manager = to_amd_sdw(bus);
	u32 dpn_ch_enable;
	u32 lane_ctrl_ch_en_reg;

	switch (amd_manager->acp_rev) {
	case ACP63_PCI_REV_ID:
		switch (amd_manager->instance) {
		case ACP_SDW0:
			lane_ctrl_ch_en_reg =
					acp63_sdw0_dp_reg[enable_ch->port_num].lane_ctrl_ch_en_reg;
			break;
		case ACP_SDW1:
			lane_ctrl_ch_en_reg =
					acp63_sdw1_dp_reg[enable_ch->port_num].lane_ctrl_ch_en_reg;
			break;
		default:
			return -EINVAL;
		}
		break;
	case ACP70_PCI_REV_ID:
	case ACP71_PCI_REV_ID:
	case ACP72_PCI_REV_ID:
		lane_ctrl_ch_en_reg = acp70_sdw_dp_reg[enable_ch->port_num].lane_ctrl_ch_en_reg;
		break;
	default:
		return -EINVAL;
	}

	/*
	 * lane_ctrl_ch_en_reg will be used to program lane_ctrl and ch_mask
	 * parameters.
	 */
	dpn_ch_enable = readl(amd_manager->mmio + lane_ctrl_ch_en_reg);
	u32p_replace_bits(&dpn_ch_enable, enable_ch->ch_mask, AMD_DPN_CH_EN_CHMASK);
	if (enable_ch->enable)
		writel(dpn_ch_enable, amd_manager->mmio + lane_ctrl_ch_en_reg);
	else
		writel(0, amd_manager->mmio + lane_ctrl_ch_en_reg);
	return 0;
}

const struct sdw_master_port_ops amd_acp63_sdw_port_ops = {
	.dpn_set_port_params = amd_acp63_sdw_port_params,
	.dpn_set_port_transport_params = amd_acp63_sdw_transport_params,
	.dpn_port_enable_ch = amd_acp63_sdw_port_enable,
};

const struct sdw_master_ops amd_acp63_sdw_ops = {
	.read_prop = amd_prop_read,
	.xfer_msg = amd_sdw_xfer_msg,
	.read_ping_status = amd_acp63_sdw_read_ping_status,
};

void amd_acp63_sdw_update_slave_status_work(struct amd_sdw_manager *amd_manager)
{
	int retry_count = 0;

	if (amd_manager->status[0] == SDW_SLAVE_ATTACHED) {
		writel(0, amd_manager->mmio + ACP_SW_STATE_CHANGE_STATUS_MASK_0TO7);
		writel(0, amd_manager->mmio + ACP_SW_STATE_CHANGE_STATUS_MASK_8TO11);
	}

update_status:
	sdw_handle_slave_status(&amd_manager->bus, amd_manager->status);
	/*
	 * During the peripheral enumeration sequence, the SoundWire manager interrupts
	 * are masked. Once the device number programming is done for all peripherals,
	 * interrupts will be unmasked. Read the peripheral device status from ping command
	 * and process the response. This sequence will ensure all peripheral devices enumerated
	 * and initialized properly.
	 */
	if (amd_manager->status[0] == SDW_SLAVE_ATTACHED) {
		if (retry_count++ < SDW_MAX_DEVICES) {
			writel(AMD_SDW_IRQ_MASK_0TO7, amd_manager->mmio +
			       ACP_SW_STATE_CHANGE_STATUS_MASK_0TO7);
			writel(AMD_SDW_IRQ_MASK_8TO11, amd_manager->mmio +
			       ACP_SW_STATE_CHANGE_STATUS_MASK_8TO11);
			acp_sdw_read_and_process_ping_status(amd_manager);
			goto update_status;
		} else {
			dev_err_ratelimited(amd_manager->dev,
					    "Device0 detected after %d iterations\n",
					    retry_count);
		}
	}
}

static void amd_acp63_sdw_process_wake_event(struct amd_sdw_manager *amd_manager)
{
	dev_dbg(amd_manager->dev, "SoundWire Wake event reported\n");
	pm_request_resume(amd_manager->dev);
	writel(0x00, amd_manager->acp_mmio + ACP_SW_WAKE_EN(amd_manager->instance));
}

void amd_acp63_sdw_irq_thread(struct amd_sdw_manager *amd_manager)
{
	u32 status_change_8to11;
	u32 status_change_0to7;

	status_change_8to11 = readl(amd_manager->mmio + ACP_SW_STATE_CHANGE_STATUS_8TO11);
	status_change_0to7 = readl(amd_manager->mmio + ACP_SW_STATE_CHANGE_STATUS_0TO7);
	if (!status_change_0to7 && !status_change_8to11)
		return;

	dev_dbg(amd_manager->dev, "[SDW%d] SDW INT: 0to7=0x%x, 8to11=0x%x\n",
		amd_manager->instance, status_change_0to7, status_change_8to11);

	if (status_change_8to11 & AMD_SDW_WAKE_STAT_MASK) {
		acp_sdw_process_wake_event(amd_manager);
		status_change_8to11 &= ~AMD_SDW_WAKE_STAT_MASK;
	}

	if (status_change_8to11 & AMD_SDW_PREQ_INTR_STAT) {
		acp_sdw_read_and_process_ping_status(amd_manager);
	} else {
		/* Check for the updated status on peripheral device */
		amd_sdw_update_slave_status(status_change_0to7, status_change_8to11, amd_manager);
	}
	if (status_change_8to11 || status_change_0to7)
		schedule_work(&amd_manager->amd_sdw_work);
	writel(0x00, amd_manager->mmio + ACP_SW_STATE_CHANGE_STATUS_8TO11);
	writel(0x00, amd_manager->mmio + ACP_SW_STATE_CHANGE_STATUS_0TO7);
}

static int amd_acp63_sdw_clock_stop_exit(struct amd_sdw_manager *amd_manager)
{
	int ret;
	u32 val;

	if (amd_manager->clk_stopped) {
		val = readl(amd_manager->mmio + ACP_SW_CLK_RESUME_CTRL);
		val |= AMD_SDW_CLK_RESUME_REQ;
		writel(val, amd_manager->mmio + ACP_SW_CLK_RESUME_CTRL);
		ret = readl_poll_timeout(amd_manager->mmio + ACP_SW_CLK_RESUME_CTRL, val,
					 (val & AMD_SDW_CLK_RESUME_DONE), ACP_DELAY_US,
					 AMD_SDW_TIMEOUT);
		if (val & AMD_SDW_CLK_RESUME_DONE) {
			writel(0, amd_manager->mmio + ACP_SW_CLK_RESUME_CTRL);
			ret = sdw_bus_exit_clk_stop(&amd_manager->bus);
			if (ret < 0)
				dev_err(amd_manager->dev, "bus failed to exit clock stop %d\n",
					ret);
			amd_manager->clk_stopped = false;
		}
	}
	if (amd_manager->clk_stopped) {
		dev_err(amd_manager->dev, "SDW%x clock stop exit failed\n", amd_manager->instance);
		return 0;
	}
	dev_dbg(amd_manager->dev, "SDW%x clock stop exit successful\n", amd_manager->instance);
	return 0;
}

static int amd_acp63_sdw_suspend(struct amd_sdw_manager *amd_manager)
{
	struct sdw_bus *bus = &amd_manager->bus;
	int ret;

	if (bus->prop.hw_disabled) {
		dev_dbg(bus->dev, "SoundWire manager %d is disabled, ignoring\n",
			bus->link_id);
		return 0;
	}

	if (amd_manager->power_mode_mask & AMD_SDW_CLK_STOP_MODE) {
		acp_disable_sdw_interrupts(amd_manager);
		cancel_work_sync(&amd_manager->amd_sdw_irq_thread);
		cancel_work_sync(&amd_manager->amd_sdw_work);
		acp_sdw_wake_enable(amd_manager, false);
		ret = amd_sdw_host_wake_enable_by_rev(amd_manager, false);
		if (ret)
			return ret;
		ret = amd_sdw_clock_stop(amd_manager);
		if (ret)
			return ret;
		acp_enable_sdw_interrupts(amd_manager);
	} else if (amd_manager->power_mode_mask & AMD_SDW_POWER_OFF_MODE) {
		acp_disable_sdw_interrupts(amd_manager);
		cancel_work_sync(&amd_manager->amd_sdw_irq_thread);
		cancel_work_sync(&amd_manager->amd_sdw_work);
		acp_sdw_wake_enable(amd_manager, false);
		ret = amd_sdw_host_wake_enable_by_rev(amd_manager, false);
		if (ret)
			return ret;
		/*
		 * As per hardware programming sequence on AMD platforms,
		 * clock stop should be invoked first before powering-off
		 */
		ret = amd_sdw_clock_stop(amd_manager);
		if (ret)
			return ret;
		ret = acp_disable_sdw_manager(amd_manager);
		if (ret)
			return ret;
	}
	switch (amd_manager->acp_rev) {
	case ACP63_PCI_REV_ID:
		break;
	case ACP70_PCI_REV_ID:
	case ACP71_PCI_REV_ID:
	case ACP72_PCI_REV_ID:
		ret = acp_sdw_set_device_state(amd_manager, AMD_SDW_DEVICE_STATE_D3);
		if (ret)
			return ret;
		break;
	default:
		dev_err(amd_manager->dev, "unexpected acp_rev 0x%x\n", amd_manager->acp_rev);
		return -EINVAL;
	}
	return 0;
}

static int amd_acp63_sdw_suspend_runtime(struct amd_sdw_manager *amd_manager)
{
	struct sdw_bus *bus = &amd_manager->bus;
	int ret;
	u32 val;

	if (bus->prop.hw_disabled) {
		dev_dbg(bus->dev, "SoundWire manager %d is disabled,\n",
			bus->link_id);
		return 0;
	}
	if (amd_manager->power_mode_mask & AMD_SDW_CLK_STOP_MODE) {
		acp_disable_sdw_interrupts(amd_manager);
		cancel_work_sync(&amd_manager->amd_sdw_irq_thread);
		cancel_work_sync(&amd_manager->amd_sdw_work);
		acp_sdw_wake_enable(amd_manager, true);
		ret = amd_sdw_host_wake_enable_by_rev(amd_manager, true);
		if (ret)
			return ret;
		ret = amd_sdw_clock_stop(amd_manager);
		if (ret)
			return ret;
		acp_enable_sdw_interrupts(amd_manager);
	} else if (amd_manager->power_mode_mask & AMD_SDW_POWER_OFF_MODE) {
		acp_disable_sdw_interrupts(amd_manager);
		cancel_work_sync(&amd_manager->amd_sdw_irq_thread);
		cancel_work_sync(&amd_manager->amd_sdw_work);
		acp_sdw_wake_enable(amd_manager, true);
		ret = amd_sdw_host_wake_enable_by_rev(amd_manager, true);
		if (ret)
			return ret;
		ret = amd_sdw_clock_stop(amd_manager);
		if (ret)
			return ret;
		ret = acp_disable_sdw_manager(amd_manager);
		if (ret)
			return ret;
	}
	switch (amd_manager->acp_rev) {
	case ACP63_PCI_REV_ID:
		break;
	case ACP70_PCI_REV_ID:
	case ACP71_PCI_REV_ID:
	case ACP72_PCI_REV_ID:
		ret = acp_sdw_set_device_state(amd_manager, AMD_SDW_DEVICE_STATE_D3);
		if (ret)
			return ret;
		if (amd_manager->wake_en_mask) {
			val = readl(amd_manager->acp_mmio + ACP_PME_EN);
			if (!val) {
				writel(1, amd_manager->acp_mmio + ACP_PME_EN);
				val = readl(amd_manager->acp_mmio + ACP_PME_EN);
				dev_dbg(amd_manager->dev, "ACP_PME_EN:0x%x\n", val);
			}
		}
		break;
	default:
		dev_err(amd_manager->dev, "unexpected acp_rev 0x%x\n", amd_manager->acp_rev);
		return -EINVAL;
	}
	return 0;
}

static int amd_acp63_sdw_resume_runtime(struct amd_sdw_manager *amd_manager)
{
	struct sdw_bus *bus = &amd_manager->bus;
	int ret;
	u32 val;

	if (bus->prop.hw_disabled) {
		dev_dbg(bus->dev, "SoundWire manager %d is disabled, ignoring\n",
			bus->link_id);
		return 0;
	}

	if (amd_manager->power_mode_mask & AMD_SDW_CLK_STOP_MODE) {
		ret = acp_sdw_clock_stop_exit(amd_manager);
		if (ret)
			return ret;
		ret = amd_sdw_host_wake_enable_by_rev(amd_manager, false);
		if (ret)
			return ret;
	} else if (amd_manager->power_mode_mask & AMD_SDW_POWER_OFF_MODE) {
		writel(0x00, amd_manager->acp_mmio + ACP_SW_WAKE_EN(amd_manager->instance));
		/*
		 * POWER_OFF_MODE tears the bus down completely; invalidate the
		 * cached ping status on entry, before any step that can fail and
		 * return early, so that a partially-failed resume leaves the first
		 * post-resume ping timeout returning 0 rather than stale
		 * pre-suspend data. CLK_STOP_MODE leaves peripherals attached, so
		 * the cache remains valid there.
		 */
		mutex_lock(&amd_manager->bus.msg_lock);
		amd_manager->ping_slave_stat = U32_MAX;
		mutex_unlock(&amd_manager->bus.msg_lock);
		ret = amd_sdw_host_wake_enable_by_rev(amd_manager, false);
		if (ret)
			return ret;
		val = readl(amd_manager->mmio + ACP_SW_CLK_RESUME_CTRL);
		if (val) {
			val |= AMD_SDW_CLK_RESUME_REQ;
			writel(val, amd_manager->mmio + ACP_SW_CLK_RESUME_CTRL);
			ret = readl_poll_timeout(amd_manager->mmio + ACP_SW_CLK_RESUME_CTRL, val,
						 (val & AMD_SDW_CLK_RESUME_DONE), ACP_DELAY_US,
						 AMD_SDW_TIMEOUT);
			if (val & AMD_SDW_CLK_RESUME_DONE) {
				writel(0, amd_manager->mmio + ACP_SW_CLK_RESUME_CTRL);
				amd_manager->clk_stopped = false;
			}
		}
		sdw_clear_slave_status(bus, SDW_UNATTACH_REQUEST_MASTER_RESET);
		ret = acp_sdw_clk_init_ctrl(amd_manager);
		if (ret)
			return ret;
		ret = acp_init_sdw_manager(amd_manager);
		if (ret)
			return ret;
		acp_enable_sdw_interrupts(amd_manager);
		ret = acp_enable_sdw_manager(amd_manager);
		if (ret)
			return ret;
		acp_sdw_set_frameshape(amd_manager);
	}
	switch (amd_manager->acp_rev) {
	case ACP63_PCI_REV_ID:
		break;
	case ACP70_PCI_REV_ID:
	case ACP71_PCI_REV_ID:
	case ACP72_PCI_REV_ID:
		ret = acp_sdw_set_device_state(amd_manager, AMD_SDW_DEVICE_STATE_D0);
		if (ret)
			return ret;
		break;
	default:
		dev_err(amd_manager->dev, "unexpected acp_rev 0x%x\n", amd_manager->acp_rev);
		return -EINVAL;
	}
	return 0;
}

static const struct amd_sdw_hw_ops amd_acp63_hw_ops = {
	.init_sdw_manager		= amd_acp63_init_sdw_manager,
	.enable_sdw_manager		= amd_acp63_enable_sdw_manager,
	.disable_sdw_manager		= amd_acp63_disable_sdw_manager,
	.enable_sdw_interrupts		= amd_acp63_enable_sdw_interrupts,
	.disable_sdw_interrupts		= amd_acp63_disable_sdw_interrupts,
	.set_frameshape			= amd_acp63_sdw_set_frameshape,
	.send_cmd_get_resp		= amd_acp63_sdw_send_cmd_get_resp,
	.wake_enable			= amd_acp63_sdw_wake_enable,
	.host_wake_enable		= amd_acp63_sdw_host_wake_enable,
	.set_device_state		= amd_acp63_sdw_set_device_state,
	.read_and_process_ping_status	= amd_acp63_sdw_read_and_process_ping_status,
	.process_wake_event		= amd_acp63_sdw_process_wake_event,
	.clk_init_ctrl			= amd_acp63_sdw_clk_init_ctrl,
	.clock_stop_exit		= amd_acp63_sdw_clock_stop_exit,
	.suspend			= amd_acp63_sdw_suspend,
	.suspend_runtime		= amd_acp63_sdw_suspend_runtime,
	.resume_runtime			= amd_acp63_sdw_resume_runtime,
};

int amd_sdw_hw_init_ops(struct amd_sdw_manager *amd_manager)
{
	switch (amd_manager->acp_rev) {
	case ACP63_PCI_REV_ID:
	case ACP70_PCI_REV_ID:
	case ACP71_PCI_REV_ID:
	case ACP72_PCI_REV_ID:
		amd_manager->hw_ops = &amd_acp63_hw_ops;
		break;
	default:
		return -EINVAL;
	}
	return 0;
}

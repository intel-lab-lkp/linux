/* SPDX-License-Identifier: (GPL-2.0 OR BSD-3-Clause) */
/*
 * Copyright (C) 2023-24 Advanced Micro Devices, Inc. All rights reserved.
 */

#ifndef __AMD_MANAGER_H
#define __AMD_MANAGER_H

#include <linux/soundwire/sdw_amd.h>

/*
 * Register offset between successive SoundWire manager instances.
 * This value applies to ACP6.3, ACP7.0, ACP7.1, and ACP7.2. A new
 * revision with a different offset must define its own constant.
 */
#define ACP63_SDW_MANAGER_REG_OFFSET				0xc00
#define AMD_SDW_DEFAULT_ROWS				50
#define AMD_SDW_DEFAULT_COLUMNS				10
#define ACP_PAD_PULLDOWN_CTRL				0x0001448
#define ACP_SW_PAD_KEEPER_EN				0x0001454
#define ACP_SW0_WAKE_EN					0x0001458
#define ACP_EXTERNAL_INTR_CNTL0				0x0001a04
#define ACP_EXTERNAL_INTR_STAT0				0x0001a0c
#define ACP_EXTERNAL_INTR_CNTL(i)			(ACP_EXTERNAL_INTR_CNTL0 + ((i) * 4))
#define ACP_EXTERNAL_INTR_STAT(i)			(ACP_EXTERNAL_INTR_STAT0 + ((i) * 4))
#define ACP_SW_WAKE_EN(i)				(ACP_SW0_WAKE_EN + ((i) * 8))

#define ACP_SW_EN					0x0003000
#define ACP_SW_EN_STATUS				0x0003004
#define ACP_SW_FRAMESIZE				0x0003008
#define ACP_SW_SSP_COUNTER				0x000300c
#define ACP_SW_AUDIO0_TX_EN				0x0003010
#define ACP_SW_AUDIO0_TX_EN_STATUS			0x0003014
#define ACP_SW_AUDIO0_TX_FRAME_FORMAT			0x0003018
#define ACP_SW_AUDIO0_TX_SAMPLEINTERVAL			0x000301c
#define ACP_SW_AUDIO0_TX_HCTRL_DP0			0x0003020
#define ACP_SW_AUDIO0_TX_HCTRL_DP1			0x0003024
#define ACP_SW_AUDIO0_TX_HCTRL_DP2			0x0003028
#define ACP_SW_AUDIO0_TX_HCTRL_DP3			0x000302c
#define ACP_SW_AUDIO0_TX_OFFSET_DP0			0x0003030
#define ACP_SW_AUDIO0_TX_OFFSET_DP1			0x0003034
#define ACP_SW_AUDIO0_TX_OFFSET_DP2			0x0003038
#define ACP_SW_AUDIO0_TX_OFFSET_DP3			0x000303c
#define ACP_SW_AUDIO0_TX_CHANNEL_ENABLE_DP0		0x0003040
#define ACP_SW_AUDIO0_TX_CHANNEL_ENABLE_DP1		0x0003044
#define ACP_SW_AUDIO0_TX_CHANNEL_ENABLE_DP2		0x0003048
#define ACP_SW_AUDIO0_TX_CHANNEL_ENABLE_DP3		0x000304c
#define ACP_SW_AUDIO1_TX_EN				0x0003050
#define ACP_SW_AUDIO1_TX_EN_STATUS			0x0003054
#define ACP_SW_AUDIO1_TX_FRAME_FORMAT			0x0003058
#define ACP_SW_AUDIO1_TX_SAMPLEINTERVAL			0x000305c
#define ACP_SW_AUDIO1_TX_HCTRL				0x0003060
#define ACP_SW_AUDIO1_TX_OFFSET				0x0003064
#define ACP_SW_AUDIO1_TX_CHANNEL_ENABLE_DP0		0x0003068
#define ACP_SW_AUDIO2_TX_EN				0x000306c
#define ACP_SW_AUDIO2_TX_EN_STATUS			0x0003070
#define ACP_SW_AUDIO2_TX_FRAME_FORMAT			0x0003074
#define ACP_SW_AUDIO2_TX_SAMPLEINTERVAL			0x0003078
#define ACP_SW_AUDIO2_TX_HCTRL				0x000307c
#define ACP_SW_AUDIO2_TX_OFFSET				0x0003080
#define ACP_SW_AUDIO2_TX_CHANNEL_ENABLE_DP0		0x0003084
#define ACP_SW_AUDIO0_RX_EN				0x0003088
#define ACP_SW_AUDIO0_RX_EN_STATUS			0x000308c
#define ACP_SW_AUDIO0_RX_FRAME_FORMAT			0x0003090
#define ACP_SW_AUDIO0_RX_SAMPLEINTERVAL			0x0003094
#define ACP_SW_AUDIO0_RX_HCTRL_DP0			0x0003098
#define ACP_SW_AUDIO0_RX_HCTRL_DP1			0x000309c
#define ACP_SW_AUDIO0_RX_HCTRL_DP2			0x0003100
#define ACP_SW_AUDIO0_RX_HCTRL_DP3			0x0003104
#define ACP_SW_AUDIO0_RX_OFFSET_DP0			0x0003108
#define ACP_SW_AUDIO0_RX_OFFSET_DP1			0x000310c
#define ACP_SW_AUDIO0_RX_OFFSET_DP2			0x0003110
#define ACP_SW_AUDIO0_RX_OFFSET_DP3			0x0003114
#define ACP_SW_AUDIO0_RX_CHANNEL_ENABLE_DP0		0x0003118
#define ACP_SW_AUDIO0_RX_CHANNEL_ENABLE_DP1		0x000311c
#define ACP_SW_AUDIO0_RX_CHANNEL_ENABLE_DP2		0x0003120
#define ACP_SW_AUDIO0_RX_CHANNEL_ENABLE_DP3		0x0003124
#define ACP_SW_AUDIO1_RX_EN				0x0003128
#define ACP_SW_AUDIO1_RX_EN_STATUS			0x000312c
#define ACP_SW_AUDIO1_RX_FRAME_FORMAT			0x0003130
#define ACP_SW_AUDIO1_RX_SAMPLEINTERVAL			0x0003134
#define ACP_SW_AUDIO1_RX_HCTRL				0x0003138
#define ACP_SW_AUDIO1_RX_OFFSET				0x000313c
#define ACP_SW_AUDIO1_RX_CHANNEL_ENABLE_DP0		0x0003140
#define ACP_SW_AUDIO2_RX_EN				0x0003144
#define ACP_SW_AUDIO2_RX_EN_STATUS			0x0003148
#define ACP_SW_AUDIO2_RX_FRAME_FORMAT			0x000314c
#define ACP_SW_AUDIO2_RX_SAMPLEINTERVAL			0x0003150
#define ACP_SW_AUDIO2_RX_HCTRL				0x0003154
#define ACP_SW_AUDIO2_RX_OFFSET				0x0003158
#define ACP_SW_AUDIO2_RX_CHANNEL_ENABLE_DP0		0x000315c
#define ACP_SW_BPT_PORT_EN				0x0003160
#define ACP_SW_BPT_PORT_EN_STATUS			0x0003164
#define ACP_SW_BPT_PORT_FRAME_FORMAT			0x0003168
#define ACP_SW_BPT_PORT_SAMPLEINTERVAL			0x000316c
#define ACP_SW_BPT_PORT_HCTRL				0x0003170
#define ACP_SW_BPT_PORT_OFFSET				0x0003174
#define ACP_SW_BPT_PORT_CHANNEL_ENABLE			0x0003178
#define ACP_SW_BPT_PORT_FIRST_BYTE_ADDR			0x000317c
#define ACP_SW_CLK_RESUME_CTRL				0x0003180
#define ACP_SW_CLK_RESUME_DELAY_CNTR			0x0003184
#define ACP_SW_BUS_RESET_CTRL				0x0003188
#define ACP_SW_PRBS_ERR_STATUS				0x000318c
#define ACP_SW_IMM_CMD_UPPER_WORD			0x0003230
#define ACP_SW_IMM_CMD_LOWER_QWORD			0x0003234
#define ACP_SW_IMM_RESP_UPPER_WORD			0x0003238
#define ACP_SW_IMM_RESP_LOWER_QWORD			0x000323c
#define ACP_SW_IMM_CMD_STS				0x0003240
#define ACP_SW_BRA_BASE_ADDRESS				0x0003244
#define ACP_SW_BRA_TRANSFER_SIZE			0x0003248
#define ACP_SW_BRA_DMA_BUSY				0x000324c
#define ACP_SW_BRA_RESP					0x0003250
#define ACP_SW_BRA_RESP_FRAME_ADDR			0x0003254
#define ACP_SW_BRA_CURRENT_TRANSFER_SIZE		0x0003258
#define ACP_SW_STATE_CHANGE_STATUS_0TO7			0x000325c
#define ACP_SW_STATE_CHANGE_STATUS_8TO11		0x0003260
#define ACP_SW_STATE_CHANGE_STATUS_MASK_0TO7		0x0003264
#define ACP_SW_STATE_CHANGE_STATUS_MASK_8TO11		0x0003268
#define ACP_SW_CLK_FREQUENCY_CTRL			0x000326c
#define ACP_SW_ERROR_INTR_MASK				0x0003270
#define ACP_SW_PHY_TEST_MODE_DATA_OFF			0x0003274

#define ACP_DELAY_US					10
#define AMD_SDW_TIMEOUT					1000
#define AMD_SDW_DEFAULT_CLK_FREQ			12000000

#define AMD_SDW_MCP_RESP_ACK				BIT(0)
#define AMD_SDW_MCP_RESP_NACK				BIT(1)
#define AMD_SDW_MCP_RESP_RDATA				GENMASK(14, 7)

#define AMD_SDW_MCP_CMD_SSP_TAG				BIT(31)
#define AMD_SDW_MCP_CMD_COMMAND				GENMASK(14, 12)
#define AMD_SDW_MCP_CMD_DEV_ADDR			GENMASK(11, 8)
#define AMD_SDW_MCP_CMD_REG_ADDR_HIGH			GENMASK(7, 0)
#define AMD_SDW_MCP_CMD_REG_ADDR_LOW			GENMASK(31, 24)
#define AMD_SDW_MCP_CMD_REG_DATA			GENMASK(14, 7)
#define AMD_SDW_MCP_SLAVE_STAT_0_3			GENMASK(14, 7)
#define AMD_SDW_MCP_SLAVE_STAT_4_11			GENMASK_ULL(39, 24)
#define AMD_SDW_MCP_SLAVE_STATUS_MASK			GENMASK(1, 0)
#define AMD_SDW_MCP_SLAVE_STATUS_BITS			GENMASK(3, 2)
#define AMD_SDW_MCP_SLAVE_STATUS_8TO_11			GENMASK_ULL(15, 0)
#define AMD_SDW_MCP_SLAVE_STATUS_VALID_MASK(x)		BIT(((x) * 4))
#define AMD_SDW_MCP_SLAVE_STAT_SHIFT_MASK(x)		(((x) * 4) + 1)

#define AMD_SDW_MASTER_SUSPEND_DELAY_MS			2000
#define AMD_SDW_QUIRK_MASK_BUS_ENABLE			BIT(0)

#define AMD_SDW_IMM_RES_VALID		1
#define AMD_SDW_IMM_CMD_BUSY		2
#define AMD_SDW_ENABLE			1
#define AMD_SDW_DISABLE			0
#define AMD_SDW_BUS_RESET_CLEAR_REQ	0
#define AMD_SDW_BUS_RESET_REQ		1
#define AMD_SDW_BUS_RESET_DONE		2
#define AMD_SDW_BUS_BASE_FREQ		24000000

#define AMD_SDW0_EXT_INTR_MASK		0x200000
#define AMD_SDW1_EXT_INTR_MASK		4
#define AMD_SDW_IRQ_MASK_0TO7		0x77777777
#define AMD_SDW_IRQ_MASK_8TO11		0x000c7777
#define AMD_SDW_IRQ_ERROR_MASK		0xff
#define AMD_ACP63_SDW0_MAX_TX_PORTS		3
#define AMD_ACP63_SDW0_MAX_RX_PORTS		3
#define AMD_ACP63_SDW1_MAX_TX_PORTS		1
#define AMD_ACP63_SDW1_MAX_RX_PORTS		1
#define AMD_ACP70_SDW_MAX_TX_PORTS		3
#define AMD_ACP70_SDW_MAX_RX_PORTS		3
#define AMD_ACP63_SDW0_MAX_DAI		6
#define AMD_ACP63_SDW1_MAX_DAI		2
#define AMD_ACP70_SDW_MAX_DAI		6
#define AMD_SDW_SLAVE_0_ATTACHED	5
#define AMD_SDW_SSP_COUNTER_VAL		3

#define AMD_DPN_FRAME_FMT_PFM				GENMASK(1, 0)
#define AMD_DPN_FRAME_FMT_PDM				GENMASK(3, 2)
#define AMD_DPN_FRAME_FMT_BLK_PKG_MODE			BIT(4)
#define AMD_DPN_FRAME_FMT_BLK_GRP_CTRL			GENMASK(6, 5)
#define AMD_DPN_FRAME_FMT_WORD_LEN			GENMASK(12, 7)
#define AMD_DPN_FRAME_FMT_PCM_OR_PDM			BIT(13)
#define AMD_DPN_HCTRL_HSTOP				GENMASK(3, 0)
#define AMD_DPN_HCTRL_HSTART				GENMASK(7, 4)
#define AMD_DPN_OFFSET_CTRL_1				GENMASK(7, 0)
#define AMD_DPN_OFFSET_CTRL_2				GENMASK(15, 8)
#define AMD_DPN_CH_EN_LCTRL				GENMASK(2, 0)
#define AMD_DPN_CH_EN_CHMASK				GENMASK(10, 3)
#define AMD_SDW_STAT_MAX_RETRY_COUNT			100
#define AMD_SDW0_PAD_PULLDOWN_CTRL_ENABLE_MASK		0x7f9f
#define AMD_SDW1_PAD_PULLDOWN_CTRL_ENABLE_MASK		0x7ffa
#define AMD_SDW0_PAD_PULLDOWN_CTRL_DISABLE_MASK		0x60
#define AMD_SDW1_PAD_PULLDOWN_CTRL_DISABLE_MASK		5
#define AMD_SDW0_PAD_KEEPER_EN_MASK			1
#define AMD_SDW1_PAD_KEEPER_EN_MASK			0x10
#define AMD_SDW0_PAD_KEEPER_DISABLE_MASK		0x1e
#define AMD_SDW1_PAD_KEEPER_DISABLE_MASK		0xf
#define AMD_SDW_PREQ_INTR_STAT				BIT(19)
#define AMD_SDW_CLK_STOP_DONE				1
#define AMD_SDW_CLK_RESUME_REQ				2
#define AMD_SDW_CLK_RESUME_DONE				3
#define AMD_SDW_WAKE_STAT_MASK				BIT(16)
#define AMD_SDW_WAKE_INTR_MASK				BIT(16)
#define AMD_SDW0_HOST_WAKE_INTR_MASK			BIT(22)
#define AMD_SDW1_HOST_WAKE_INTR_MASK			BIT(23)
#define AMD_SDW_DEVICE_STATE				0x1430
#define AMD_SDW0_DEVICE_STATE_MASK			GENMASK(1, 0)
#define AMD_SDW1_DEVICE_STATE_MASK			GENMASK(3, 2)
#define AMD_SDW_DEVICE_STATE_D0				0
#define AMD_SDW_DEVICE_STATE_D3				3
#define ACP_PME_EN					0x0001400

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

#define AMD_SDW_HW_OPS(amd_manager, cb)	((amd_manager)->hw_ops->cb)

/**
 * struct amd_sdw_hw_ops - hardware-revision-specific SoundWire manager ops
 *
 * @init_sdw_manager: enable the SoundWire manager hardware and perform bus reset
 * @enable_sdw_manager: set the manager enable bit and wait for it to assert
 * @disable_sdw_manager: clear the manager enable bit and wait for it to deassert
 * @enable_sdw_interrupts: unmask ACP-level and per-manager interrupt bits
 * @disable_sdw_interrupts: mask ACP-level and per-manager interrupt bits
 * @set_frameshape: write the row/column frame-shape register
 * @send_cmd_get_resp: issue an immediate command and return the response;
 *   returns 0 on success with the raw u64 response in @response,
 *   or negative on hardware timeout
 * @wake_enable: enable or disable the in-band SoundWire wake interrupt
 * @host_wake_enable: enable or disable the host-side wake interrupt
 * @set_device_state: write the D0/D3 device power state for the manager instance
 * @read_and_process_ping_status: issue a PING command and process the slave
 *   attach/detach status; called from the IRQ work path
 * @process_wake_event: handle an in-band wake notification from a slave
 * @irq_thread: hw_ops callback invoked by the thin work_struct wrapper on
 *   each interrupt; reads status registers and dispatches slave events
 * @update_slave_status_work: hw_ops callback invoked by the status-update
 *   work item; reads slave status and updates the SoundWire core
 * @clk_init_ctrl: set the clock divider and update the frame shape registers
 *   based on the current bus clock frequency
 * @clock_stop_exit: restart the SoundWire bus clock after a CLK_STOP_MODE
 *   suspend; implementation differs across ACP revisions
 * @suspend: system-suspend hook; stops the bus and powers down the manager
 * @suspend_runtime: runtime-suspend hook; same sequence as @suspend
 * @resume_runtime: runtime-resume hook; reinitialises the manager and restarts
 *   the bus after a POWER_OFF_MODE or CLK_STOP_MODE suspend
 */
struct amd_sdw_hw_ops {
	int (*init_sdw_manager)(struct amd_sdw_manager *amd_manager);
	int (*enable_sdw_manager)(struct amd_sdw_manager *amd_manager);
	int (*disable_sdw_manager)(struct amd_sdw_manager *amd_manager);
	void (*enable_sdw_interrupts)(struct amd_sdw_manager *amd_manager);
	void (*disable_sdw_interrupts)(struct amd_sdw_manager *amd_manager);
	void (*set_frameshape)(struct amd_sdw_manager *amd_manager);
	int (*send_cmd_get_resp)(struct amd_sdw_manager *amd_manager, u32 lower_data,
				 u32 upper_data, u64 *response);
	void (*wake_enable)(struct amd_sdw_manager *amd_manager, bool enable);
	int (*host_wake_enable)(struct amd_sdw_manager *amd_manager, bool enable);
	int (*set_device_state)(struct amd_sdw_manager *amd_manager, u32 target_device_state);
	void (*read_and_process_ping_status)(struct amd_sdw_manager *amd_manager);
	void (*process_wake_event)(struct amd_sdw_manager *amd_manager);
	void (*irq_thread)(struct amd_sdw_manager *amd_manager);
	void (*update_slave_status_work)(struct amd_sdw_manager *amd_manager);
	int (*clk_init_ctrl)(struct amd_sdw_manager *amd_manager);
	int (*clock_stop_exit)(struct amd_sdw_manager *amd_manager);
	int (*suspend)(struct amd_sdw_manager *amd_manager);
	int (*suspend_runtime)(struct amd_sdw_manager *amd_manager);
	int (*resume_runtime)(struct amd_sdw_manager *amd_manager);
};

static inline int acp_init_sdw_manager(struct amd_sdw_manager *amd_manager)
{
	if (amd_manager && amd_manager->hw_ops && amd_manager->hw_ops->init_sdw_manager)
		return AMD_SDW_HW_OPS(amd_manager, init_sdw_manager)(amd_manager);
	return -EOPNOTSUPP;
}

static inline int acp_enable_sdw_manager(struct amd_sdw_manager *amd_manager)
{
	if (amd_manager && amd_manager->hw_ops && amd_manager->hw_ops->enable_sdw_manager)
		return AMD_SDW_HW_OPS(amd_manager, enable_sdw_manager)(amd_manager);
	return -EOPNOTSUPP;
}

static inline int acp_disable_sdw_manager(struct amd_sdw_manager *amd_manager)
{
	if (amd_manager && amd_manager->hw_ops && amd_manager->hw_ops->disable_sdw_manager)
		return AMD_SDW_HW_OPS(amd_manager, disable_sdw_manager)(amd_manager);
	return -EOPNOTSUPP;
}

static inline void acp_enable_sdw_interrupts(struct amd_sdw_manager *amd_manager)
{
	if (amd_manager && amd_manager->hw_ops && amd_manager->hw_ops->enable_sdw_interrupts)
		AMD_SDW_HW_OPS(amd_manager, enable_sdw_interrupts)(amd_manager);
}

static inline void acp_disable_sdw_interrupts(struct amd_sdw_manager *amd_manager)
{
	if (amd_manager && amd_manager->hw_ops && amd_manager->hw_ops->disable_sdw_interrupts)
		AMD_SDW_HW_OPS(amd_manager, disable_sdw_interrupts)(amd_manager);
}

static inline void acp_sdw_set_frameshape(struct amd_sdw_manager *amd_manager)
{
	if (amd_manager && amd_manager->hw_ops && amd_manager->hw_ops->set_frameshape)
		AMD_SDW_HW_OPS(amd_manager, set_frameshape)(amd_manager);
}

static inline int acp_sdw_send_cmd_get_resp(struct amd_sdw_manager *amd_manager,
					    u32 lower_data, u32 upper_data, u64 *response)
{
	if (amd_manager && amd_manager->hw_ops && amd_manager->hw_ops->send_cmd_get_resp)
		return AMD_SDW_HW_OPS(amd_manager, send_cmd_get_resp)(amd_manager,
								      lower_data, upper_data,
								      response);
	return -EOPNOTSUPP;
}

static inline void acp_sdw_wake_enable(struct amd_sdw_manager *amd_manager, bool enable)
{
	if (amd_manager && amd_manager->hw_ops && amd_manager->hw_ops->wake_enable)
		AMD_SDW_HW_OPS(amd_manager, wake_enable)(amd_manager, enable);
}

static inline int acp_sdw_host_wake_enable(struct amd_sdw_manager *amd_manager, bool enable)
{
	if (amd_manager && amd_manager->hw_ops && amd_manager->hw_ops->host_wake_enable)
		return AMD_SDW_HW_OPS(amd_manager, host_wake_enable)(amd_manager, enable);
	return -EOPNOTSUPP;
}

static inline int acp_sdw_set_device_state(struct amd_sdw_manager *amd_manager,
					   u32 target_device_state)
{
	if (amd_manager && amd_manager->hw_ops && amd_manager->hw_ops->set_device_state)
		return AMD_SDW_HW_OPS(amd_manager, set_device_state)(amd_manager,
								      target_device_state);
	return -EOPNOTSUPP;
}

static inline void acp_sdw_read_and_process_ping_status(struct amd_sdw_manager *amd_manager)
{
	if (amd_manager && amd_manager->hw_ops && amd_manager->hw_ops->read_and_process_ping_status)
		AMD_SDW_HW_OPS(amd_manager, read_and_process_ping_status)(amd_manager);
}

static inline void acp_sdw_process_wake_event(struct amd_sdw_manager *amd_manager)
{
	if (amd_manager && amd_manager->hw_ops && amd_manager->hw_ops->process_wake_event)
		AMD_SDW_HW_OPS(amd_manager, process_wake_event)(amd_manager);
}

static inline void acp_sdw_irq_thread(struct amd_sdw_manager *amd_manager)
{
	if (amd_manager && amd_manager->hw_ops && amd_manager->hw_ops->irq_thread)
		AMD_SDW_HW_OPS(amd_manager, irq_thread)(amd_manager);
}

static inline void acp_sdw_update_slave_status_work(struct amd_sdw_manager *amd_manager)
{
	if (amd_manager && amd_manager->hw_ops && amd_manager->hw_ops->update_slave_status_work)
		AMD_SDW_HW_OPS(amd_manager, update_slave_status_work)(amd_manager);
}

static inline int acp_sdw_clk_init_ctrl(struct amd_sdw_manager *amd_manager)
{
	if (amd_manager && amd_manager->hw_ops && amd_manager->hw_ops->clk_init_ctrl)
		return AMD_SDW_HW_OPS(amd_manager, clk_init_ctrl)(amd_manager);
	return -EOPNOTSUPP;
}

static inline int acp_sdw_clock_stop_exit(struct amd_sdw_manager *amd_manager)
{
	if (amd_manager && amd_manager->hw_ops && amd_manager->hw_ops->clock_stop_exit)
		return AMD_SDW_HW_OPS(amd_manager, clock_stop_exit)(amd_manager);
	return -EOPNOTSUPP;
}

static inline int acp_sdw_suspend(struct device *dev)
{
	struct amd_sdw_manager *amd_manager = dev_get_drvdata(dev);

	if (amd_manager && amd_manager->hw_ops && amd_manager->hw_ops->suspend)
		return AMD_SDW_HW_OPS(amd_manager, suspend)(amd_manager);
	return -EOPNOTSUPP;
}

static inline int acp_sdw_suspend_runtime(struct device *dev)
{
	struct amd_sdw_manager *amd_manager = dev_get_drvdata(dev);

	if (amd_manager && amd_manager->hw_ops && amd_manager->hw_ops->suspend_runtime)
		return AMD_SDW_HW_OPS(amd_manager, suspend_runtime)(amd_manager);
	return -EOPNOTSUPP;
}

static inline int acp_sdw_resume_runtime(struct device *dev)
{
	struct amd_sdw_manager *amd_manager = dev_get_drvdata(dev);

	if (amd_manager && amd_manager->hw_ops && amd_manager->hw_ops->resume_runtime)
		return AMD_SDW_HW_OPS(amd_manager, resume_runtime)(amd_manager);
	return -EOPNOTSUPP;
}

#endif

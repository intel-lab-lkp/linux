// SPDX-License-Identifier: GPL-2.0
/* Copyright (C) 2021 - 2023 Intel Corporation. */
/* Copyright (c) 2025 Realtek Semiconductor Corp. */

#include <linux/bits.h>
#include <linux/kernel.h>
#include <linux/ktime.h>
#include <linux/bitfield.h>
#include <linux/debugfs.h>
#include <linux/module.h>
#include <linux/property.h>
#include <linux/regmap.h>
#include <linux/list.h>
#include <linux/gpio/driver.h>

#include <linux/i3c/device.h>
#include <linux/i3c/master.h>

#define I3C_HUB_TP_MAX_COUNT 0x08

#define I3C_DCR_HUB 0xC2

#define GPIO_BANK_SZ  0x02
#define GPIO_MAX_BANK I3C_HUB_TP_MAX_COUNT

/* I3C HUB REGISTERS */

/*
 * In this driver Controller - Target convention is used. All the abbreviations are
 * based on this convention. For instance: CP - Controller Port, TP - Target Port.
 */

/* Device Information Registers */
#define I3C_HUB_DEV_INFO_0 0x00
#define I3C_HUB_DEV_INFO_1 0x01
#define I3C_HUB_PID_5	   0x02
#define I3C_HUB_PID_4	   0x03
#define I3C_HUB_PID_3	   0x04
#define I3C_HUB_PID_2	   0x05
#define I3C_HUB_PID_1	   0x06
#define I3C_HUB_PID_0	   0x07
#define I3C_HUB_BCR	   0x08
#define I3C_HUB_DCR	   0x09
#define I3C_HUB_DEV_CAPAB  0x0A

#define I3C_HUB_DEV_REV	   0x0B
#define I3C_HUB_DEV_REV_LDO_MASK   GENMASK(7, 6)
#define I3C_HUB_DEV_REV_LDO_GET(x) FIELD_GET(I3C_HUB_DEV_REV_LDO_MASK, (x))

/* Device Configuration Registers */
#define I3C_HUB_PROTECTION_CODE	  0x10
#define REGISTERS_LOCK_CODE	  0x00
#define REGISTERS_UNLOCK_CODE	  0x69
#define CP1_REGISTERS_UNLOCK_CODE 0x6A

#define I3C_HUB_CP_CONF	  0x11
#define I3C_HUB_TP_ENABLE 0x12
#define TPN_ENABLE(n)	  BIT(n)

#define I3C_HUB_DEV_CONF	0x13
#define I3C_HUB_IO_STRENGTH	0x14
#define TP0145_IO_STRENGTH_MASK GENMASK(1, 0)
#define TP0145_IO_STRENGTH(x)	(((x) << 0) & TP0145_IO_STRENGTH_MASK)
#define TP2367_IO_STRENGTH_MASK GENMASK(3, 2)
#define TP2367_IO_STRENGTH(x)	(((x) << 2) & TP2367_IO_STRENGTH_MASK)
#define CP0_IO_STRENGTH_MASK	GENMASK(5, 4)
#define CP0_IO_STRENGTH(x)	(((x) << 4) & CP0_IO_STRENGTH_MASK)
#define CP1_IO_STRENGTH_MASK	GENMASK(7, 6)
#define CP1_IO_STRENGTH(x)	(((x) << 6) & CP1_IO_STRENGTH_MASK)
#define IO_STRENGTH_20_OHM	0x00
#define IO_STRENGTH_30_OHM	0x01
#define IO_STRENGTH_40_OHM	0x02
#define IO_STRENGTH_50_OHM	0x03

#define I3C_HUB_NET_OPER_MODE_CONF 0x15
#define I3C_HUB_NET_ALWAYS_I3C_EN  BIT(5)

#define I3C_HUB_LDO_CONF	   0x16
#define CP0_LDO_VOLTAGE_MASK	   GENMASK(1, 0)
#define CP0_LDO_VOLTAGE(x)	   (((x) << 0) & CP0_LDO_VOLTAGE_MASK)
#define CP1_LDO_VOLTAGE_MASK	   GENMASK(3, 2)
#define CP1_LDO_VOLTAGE(x)	   (((x) << 2) & CP1_LDO_VOLTAGE_MASK)
#define TP0145_LDO_VOLTAGE_MASK	   GENMASK(5, 4)
#define TP0145_LDO_VOLTAGE(x)	   (((x) << 4) & TP0145_LDO_VOLTAGE_MASK)
#define TP2367_LDO_VOLTAGE_MASK	   GENMASK(7, 6)
#define TP2367_LDO_VOLTAGE(x)	   (((x) << 6) & TP2367_LDO_VOLTAGE_MASK)
#define LDO_VOLTAGE_1_0V	   0x00
#define LDO_VOLTAGE_1_1V	   0x01
#define LDO_VOLTAGE_1_2V	   0x02
#define LDO_VOLTAGE_1_8V	   0x03

#define I3C_HUB_TP_IO_MODE_CONF	 0x17
#define TPN_IO_MODE_CON(n)	 BIT(n)
#define I3C_HUB_TP_SMBUS_AGNT_EN 0x18
#define TPN_SMBUS_MODE_EN(n)	 BIT(n)

#define I3C_HUB_LDO_AND_PULLUP_CONF 0x19
#define LDO_ENABLE_DISABLE_MASK	    GENMASK(3, 0)
#define CP0_LDO_EN		    BIT(0)
#define CP1_LDO_EN		    BIT(1)
/*
 * I3C HUB does not provide a way to control LDO or pull-up for individual ports. It is possible
 * for group of ports TP0/TP1/TP4/TP5 and TP2/TP3/TP6/TP7.
 */
#define TP0145_LDO_EN		    BIT(2)
#define TP2367_LDO_EN		    BIT(3)
#define TP0145_PULLUP_CONF_MASK	    GENMASK(7, 6)
#define TP0145_PULLUP_CONF(x)	    (((x) << 6) & TP0145_PULLUP_CONF_MASK)
#define TP2367_PULLUP_CONF_MASK	    GENMASK(5, 4)
#define TP2367_PULLUP_CONF(x)	    (((x) << 4) & TP2367_PULLUP_CONF_MASK)
#define PULLUP_250R		    0x00
#define PULLUP_500R		    0x01
#define PULLUP_1K		    0x02
#define PULLUP_2K		    0x03

#define I3C_HUB_CP_IBI_CONF	 0x1A
#define I3C_HUB_TP_IBI_CONF	 0x1B
#define I3C_HUB_IBI_MDB_CUSTOM	 0x1C
#define I3C_HUB_JEDEC_CONTEXT_ID 0x1D
#define I3C_HUB_TP_GPIO_MODE_EN	 0x1E
#define TPN_GPIO_MODE_EN(n)	 BIT(n)

/* Device Status and IBI Registers */
#define I3C_HUB_DEV_AND_IBI_STS	      0x20
#define PEC_ERROR_FLAG		      BIT(0)
#define PARITY_ERROR_FLAG	      BIT(1)
#define CONTROLLER_MSG_PENDING_FLAG   BIT(2)
#define TP_IO_FLAG_STATUS	      BIT(3)
#define SMBUS_AGENT_EVENT_FLAG_STATUS BIT(4)

#define I3C_HUB_TP_SMBUS_AGNT_IBI_STS 0x21

/* Controller Port Control/Status Registers */
#define I3C_HUB_CP_MUX_SET		      0x38
#define CONTROLLER_PORT_MUX_REQ		      BIT(0)
#define I3C_HUB_CP_MUX_STS		      0x39
#define CONTROLLER_PORT_MUX_CONNECTION_STATUS BIT(0)

/* Target Dynamic Address Assignment Flag Registers */
#define I3C_HUB_TARGET_DA_FLAG_BYTE_BASE  0x40
#define I3C_HUB_TARGET_DA_FLAG_BYTE_COUNT 16

/* Target Ports Control Registers */
#define I3C_HUB_TP_SMBUS_AGNT_TRANS_START 0x50
#define I3C_HUB_TP_NET_CON_CONF		  0x51
#define TPN_NET_CON(n)			  BIT(n)

#define I3C_HUB_TP_PULLUP_EN 0x53
#define TPN_PULLUP_EN(n)     BIT(n)

#define I3C_HUB_TP_SCL_OUT_EN	 0x54
#define I3C_HUB_TP_SDA_OUT_EN	 0x55
#define I3C_HUB_TP_SCL_OUT_LEVEL 0x56
#define I3C_HUB_TP_SDA_OUT_LEVEL 0x57

#define I3C_HUB_TP_IN_DETECT_MODE_CONF 0x58
#define SCL0145_IO_IN_DET_CFG_MASK     GENMASK(1, 0)
#define SCL0145_IO_IN_DET_CFG(x)       (((x) << 0) & SCL0145_IO_IN_DET_CFG_MASK)
#define SDA0145_IO_IN_DET_CFG_MASK     GENMASK(3, 2)
#define SDA0145_IO_IN_DET_CFG(x)       (((x) << 2) & SDA0145_IO_IN_DET_CFG_MASK)
#define SCL2367_IO_IN_DET_CFG_MASK     GENMASK(5, 4)
#define SCL2367_IO_IN_DET_CFG(x)       (((x) << 4) & SCL2367_IO_IN_DET_CFG_MASK)
#define SDA2367_IO_IN_DET_CFG_MASK     GENMASK(7, 6)
#define SDA2367_IO_IN_DET_CFG(x)       (((x) << 6) & SDA2367_IO_IN_DET_CFG_MASK)

#define I3C_HUB_TP_SCL_IN_DETECT_IBI_EN 0x59
#define I3C_HUB_TP_SDA_IN_DETECT_IBI_EN 0x5A

/* Target Ports Status Registers */
#define I3C_HUB_TP_SCL_IN_LEVEL_STS  0x60
#define I3C_HUB_TP_SDA_IN_LEVEL_STS  0x61
#define I3C_HUB_TP_SCL_IN_DETECT_FLG 0x62
#define I3C_HUB_TP_SDA_IN_DETECT_FLG 0x63

/* SMBus Agent Configuration and Status Registers */
#define I3C_HUB_TP0_SMBUS_AGNT_STS	      0x64
#define I3C_HUB_TP1_SMBUS_AGNT_STS	      0x65
#define I3C_HUB_TP2_SMBUS_AGNT_STS	      0x66
#define I3C_HUB_TP3_SMBUS_AGNT_STS	      0x67
#define I3C_HUB_TP4_SMBUS_AGNT_STS	      0x68
#define I3C_HUB_TP5_SMBUS_AGNT_STS	      0x69
#define I3C_HUB_TP6_SMBUS_AGNT_STS	      0x6A
#define I3C_HUB_TP7_SMBUS_AGNT_STS	      0x6B

#define I3C_HUB_ONCHIP_TD_AND_SMBUS_AGNT_CONF 0x6C
#define TARGET_AGENT_BUF_FULL_SDA_LOW_EN      BIT(5)

/* Transaction status checking mask */
#define I3C_HUB_CONTROLLER_AGENT_STATUS_MASK		   (0xF0 | BIT(0))
#define I3C_HUB_CONTROLLER_AGENT_FINISH_FLAG		   BIT(0)
/* SMBus Controller Agent Return Codes */
#define I3C_HUB_CONTROLLER_AGENT_RET_CODE_SHIFT		   4
#define I3C_HUB_CONTROLLER_AGENT_RET_CODE_SUCCESS	   0x0
#define I3C_HUB_CONTROLLER_AGENT_RET_CODE_ADDRESS_NACK	   0x1
#define I3C_HUB_CONTROLLER_AGENT_RET_CODE_DEVICE_BUSY	   0x2
#define I3C_HUB_CONTROLLER_AGENT_RET_CODE_READ_NOT_READY   0x3
#define I3C_HUB_CONTROLLER_AGENT_RET_CODE_SYNC_RECOVERED   0x4
#define I3C_HUB_CONTROLLER_AGENT_RET_CODE_SYNC_BUS_CLEAR   0x5
#define I3C_HUB_CONTROLLER_AGENT_RET_CODE_BUS_FAULT	   0x6
#define I3C_HUB_CONTROLLER_AGENT_RET_CODE_ARBITRATION_LOST 0x7
#define I3C_HUB_CONTROLLER_AGENT_RET_CODE_SCL_TIMEOUT	   0x8

#define I3C_HUB_TARGET_BUF_STATUS_MASK GENMASK(3, 1)
#define I3C_HUB_TARGET_BUF_0_RECEIVE   BIT(1)
#define I3C_HUB_TARGET_BUF_1_RECEIVE   BIT(2)
#define I3C_HUB_TARGET_BUF_OVRFL       BIT(3)

/* Special Function Registers */
#define I3C_HUB_LDO_AND_CPSEL_STS     0x79
#define CP_SDA1_LEVEL		      BIT(7)
#define CP_SCL1_LEVEL		      BIT(6)
#define CP_SEL_PIN_INPUT_CODE_MASK    GENMASK(5, 4)
#define CP_SEL_PIN_INPUT_CODE_GET(x)  (((x) & CP_SEL_PIN_INPUT_CODE_MASK) >> 4)
#define CP_SDA1_SCL1_PINS_CODE_MASK   GENMASK(7, 6)
#define CP_SDA1_SCL1_PINS_CODE_GET(x) (((x) & CP_SDA1_SCL1_PINS_CODE_MASK) >> 6)
#define VCCIO1_PWR_GOOD		      BIT(3)
#define VCCIO0_PWR_GOOD		      BIT(2)
#define CP1_VCCIO_PWR_GOOD	      BIT(1)
#define CP0_VCCIO_PWR_GOOD	      BIT(0)

#define I3C_HUB_BUS_RESET_SCL_TIMEOUT	0x7A
#define I3C_HUB_ONCHIP_TD_PROTO_ERR_FLG 0x7B
#define I3C_HUB_DEV_CMD			0x7C
#define I3C_HUB_ONCHIP_TD_STS		0x7D
#define I3C_HUB_ONCHIP_TD_ADDR_CONF	0x7E
#define I3C_HUB_PAGE_PTR		0x7F

/* Paged Transaction Registers */
#define I3C_HUB_CONTROLLER_BUFFER_PAGE	   0x10
#define I3C_HUB_CONTROLLER_AGENT_BUFF	   0x80
#define I3C_HUB_CONTROLLER_AGENT_BUFF_DATA 0x84
#define I3C_HUB_TARGET_BUFF_LENGTH	   0x80
#define I3C_HUB_TARGET_BUFF_ADDRESS	   0x81
#define I3C_HUB_TARGET_BUFF_DATA	   0x82

/* TP DT setting */
#define I3C_HUB_DT_TP_MODE_DISABLED    0x00
#define I3C_HUB_DT_TP_MODE_I3C	       0x01
#define I3C_HUB_DT_TP_MODE_SMBUS       0x02
#define I3C_HUB_DT_TP_MODE_GPIO	       0x03
#define I3C_HUB_DT_TP_MODE_NOT_DEFINED 0xFF

/* TP IO mode */
#define I3C_HUB_DT_TP_IO_MODE_OD_PP	  0x00
#define I3C_HUB_DT_TP_IO_MODE_OD	  0x01
#define I3C_HUB_DT_TP_IO_MODE_NOT_DEFINED 0xFF

/* SMBus transaction types fields */
#define I3C_HUB_SMBUS_100_KHZ  0x00
#define I3C_HUB_SMBUS_200_KHZ  BIT(1)
#define I3C_HUB_SMBUS_400_KHZ  BIT(2)
#define I3C_HUB_SMBUS_1000_KHZ (BIT(1) | BIT(2))

/* SMBus xfer type for i3c_hub_smbus_msg xfer_type parameter */
#define I3C_HUB_SMBUS_XFER_WRITE 0
#define I3C_HUB_SMBUS_XFER_READ	 1
#define I3C_HUB_SMBUS_XFER_WR_RD 2

/* Hub buffer size */
#define I3C_HUB_CONTROLLER_BUFFER_SIZE 88
#define I3C_HUB_TARGET_BUFFER_SIZE     80
#define I3C_HUB_SMBUS_DESCRIPTOR_SIZE  4
#define I3C_HUB_SMBUS_PAYLOAD_SIZE \
	(I3C_HUB_CONTROLLER_BUFFER_SIZE - I3C_HUB_SMBUS_DESCRIPTOR_SIZE)
#define I3C_HUB_SMBUS_TARGET_PAYLOAD_SIZE (I3C_HUB_TARGET_BUFFER_SIZE - 2)

/* Hub SMBus status register read interval (microseconds, ceil) */
#define I3C_HUB_SMBUS_STATUS_READ_INTERVAL_US_CEIL(len, clk_khz) \
	DIV_ROUND_UP(1000U * 9U * (u32)(len), (u32)(clk_khz))

/* ID Extraction */
#define I3C_HUB_ID_CP_SDA_SCL 0x00
#define I3C_HUB_ID_CP_SEL     0x01

/* IBI */
#define IBI_MAX_PAYLOAD_LEN 2
#define IBI_SLOT_NUMS	    6

#define I3C_HUB_IO_CTRL_PAGE			0x81
#define I3C_HUB_CFG_TP_SCL_L_ACK_CLK		0xDB
#define I3C_HUB_CFG_TP_SCL_L_ACK_CLK_EN		BIT(6)
#define I3C_HUB_CFG_TP_SCL_L_ACK_CLK_COUNT_MASK GENMASK(5, 0)
#define I3C_HUB_CFG_TP_SCL_L_ACK_CLK_COUNT_VAL	0x18

#define I3C_HUB_CFG_TP_SCL_H_ACK_CLK		0xDC
#define I3C_HUB_CFG_TP_SCL_H_ACK_CLK_EN		BIT(4)
#define I3C_HUB_CFG_TP_SCL_H_ACK_CLK_COUNT_MASK GENMASK(3, 0)
#define I3C_HUB_CFG_TP_SCL_H_ACK_CLK_COUNT_VAL(x) \
	((x) & I3C_HUB_CFG_TP_SCL_H_ACK_CLK_COUNT_MASK)

#define I3C_HUB_EFUSE_PAGE	  0x7B
#define I3C_HUB_EFUSE_OFFSET_A0	  0xA0
#define I3C_HUB_FAST_RSON_EN	  BIT(5)
#define I3C_HUB_EFUSE_OFFSET_A3	  0xA3
#define I3C_HUB_FAST_DRV_LOOP_DIS BIT(5)

#define I3C_HUB_EFUSE_OFFSET_9D 0x9D
#define I3C_HUB_TP_OD_VOL_LEVEL BIT(0)
#define I3C_HUB_TP_OD_VREF	BIT(1)

#define I3C_HUB_EFUSE_OFFSET_9E		  0x9E
#define I3C_HUB_FAST_DRV_H_ADD_CYCLE_MASK GENMASK(5, 4)
#define I3C_HUB_FAST_DRV_H_ADD_CYCLE_VAL(x) \
	(((x) << 4) & I3C_HUB_FAST_DRV_H_ADD_CYCLE_MASK)
#define I3C_HUB_IBI_ACK_RD_CYCLE_MASK	  GENMASK(3, 0)
#define I3C_HUB_IBI_ACK_RD_CYCLE_VAL	  (5)

struct i3c_hub_dev_info {
	const char *model;
	u16 part_id;
	u8 n_ports;
};

static const struct i3c_hub_dev_info i3c_hub_dev_info_unknown = {
	.model = "Unknown",
	.part_id = 0,
	.n_ports = 8,
};

static const struct i3c_hub_dev_info i3c_hub_dev_info_table[] = {
	{ "RTS4900", 0x4000, 4 }, { "RTS4901", 0x4100, 4 },
	{ "RTS4902", 0x8000, 8 }, { "RTS4903", 0x8100, 8 },
	{ "RTS4904", 0x4001, 4 }, { "RTS4906", 0x8001, 8 }
};

struct tp_setting {
	u8 mode;
	bool pullup_en;
	u8 io_mode;
	bool always_enable;
	u32 poll_interval_ms;
	u32 clock_frequency;
};

struct dt_settings {
	bool cp0_ldo_en;
	bool cp1_ldo_en;
	bool tp0145_ldo_en;
	bool tp2367_ldo_en;
	u32 cp0_ldo_volt;
	u32 cp1_ldo_volt;
	u32 tp0145_ldo_volt;
	u32 tp2367_ldo_volt;
	u32 tp0145_pullup;
	u32 tp2367_pullup;
	u32 cp0_io_strength;
	u32 cp1_io_strength;
	u32 tp0145_io_strength;
	u32 tp2367_io_strength;
	struct tp_setting tp[I3C_HUB_TP_MAX_COUNT];
	bool hub_net_always_i3c;
	u8 tp_scl_h_ack_cycles;
	bool handshake_optimize;
	u8 fast_drv_h_add_cycles;
	bool fast_rson_en;
	bool tp_od_vol_optimize;
	bool tp_od_vref_optimize;
};

struct smbus_backend {
	struct i2c_client *client;
	struct list_head list;
};

struct i2c_adapter_group {
	struct i2c_adapter i2c;
	u8 tp_mask;
	u8 tp_port;
	u8 used;
	struct device_node *of_node;
	struct i3c_hub *priv;
	struct mutex mutex; /* protects SMBus adapter state and transfers */

	struct delayed_work delayed_work_polling;
	struct list_head backend_entry;
	u8 last_processed_buf;

	u8 status;
	struct completion completion;
};

struct logical_bus {
	bool available; /* Logical bus configuration is available in DT. */
	bool registered; /* Logical bus was registered in the framework. */
	u8 tp_id;
	u8 tp_map;
	struct i3c_master_controller controller;
	struct device_node *of_node;
	struct i3c_hub *priv;
};

struct hub_gpio {
	struct gpio_chip chip;
	int tp[GPIO_MAX_BANK];
	s8 port_to_index[I3C_HUB_TP_MAX_COUNT];
	int nums;
	struct irq_chip irq_chip;
	struct mutex irq_mutex; /* protects GPIO IRQ enable/disable operations */
};

struct i3c_hub {
	struct i3c_device *i3cdev;
	struct i3c_master_controller *driving_master;
	struct regmap *regmap;
	const struct i3c_hub_dev_info *dev_info;
	struct dt_settings settings;
	struct delayed_work delayed_work;
	int hub_pin_sel_id;
	int hub_pin_cp1_id;
	int hub_dt_sel_id;
	int hub_dt_cp1_id;

	struct logical_bus logical_bus[I3C_HUB_TP_MAX_COUNT];
	struct i2c_adapter_group smbus_port_adapter[I3C_HUB_TP_MAX_COUNT];
	u8 smbus_ibi_en_mask;
	struct mutex page_mutex; /* protects regmap page register access */

	/* Offset for reading HUB's register. */
	u8 reg_addr;
	struct dentry *debug_dir;
	struct hub_gpio gpio;
};

/* Global mutex for serializing regmap access across all i3c hubs. */
static DEFINE_MUTEX(i3c_hub_regmap_mutex);

static u8 i3c_hub_ldo_dt_to_reg(u32 microvolt)
{
	switch (microvolt) {
	case 1100000:
		return LDO_VOLTAGE_1_1V;
	case 1200000:
		return LDO_VOLTAGE_1_2V;
	case 1800000:
		return LDO_VOLTAGE_1_8V;
	default:
		return LDO_VOLTAGE_1_0V;
	}
}

static u8 i3c_hub_pullup_dt_to_reg(u32 ohms)
{
	switch (ohms) {
	case 250:
		return PULLUP_250R;
	case 500:
		return PULLUP_500R;
	case 1000:
		return PULLUP_1K;
	default:
		return PULLUP_2K;
	}
}

static u8 i3c_hub_io_strength_dt_to_reg(u32 ohms)
{
	switch (ohms) {
	case 50:
		return IO_STRENGTH_50_OHM;
	case 40:
		return IO_STRENGTH_40_OHM;
	case 30:
		return IO_STRENGTH_30_OHM;
	default:
		return IO_STRENGTH_20_OHM;
	}
}

static bool i3c_hub_smbus_validate_clock_frequency(u32 hz)
{
	switch (hz) {
	case 100000:
	case 200000:
	case 400000:
	case 1000000:
		return true;
	default:
		return false;
	}
}

static inline u8 i3c_hub_smbus_rate_bits_from_hz(u32 hz)
{
	switch (hz) {
	case 100000:
		return I3C_HUB_SMBUS_100_KHZ;
	case 200000:
		return I3C_HUB_SMBUS_200_KHZ;
	case 1000000:
		return I3C_HUB_SMBUS_1000_KHZ;
	default:
		return I3C_HUB_SMBUS_400_KHZ;
	}
}

static void i3c_hub_tp_of_get_setting(struct device *dev,
				      const struct device_node *node,
				      struct tp_setting tp_setting[])
{
	struct i3c_hub *priv = dev_get_drvdata(dev);
	const char *mode_str, *io_mode_str;
	struct device_node *tp_node;
	u32 id, val;

	for_each_available_child_of_node(node, tp_node) {
		if (!tp_node->name || of_node_cmp(tp_node->name, "target-port"))
			continue;

		if (!tp_node->full_name ||
		    (sscanf(tp_node->full_name, "target-port@%u", &id) != 1)) {
			dev_warn(dev,
				 "Invalid target port node found in DT: %s\n",
				 tp_node->full_name);
			continue;
		}

		if (id >= priv->dev_info->n_ports) {
			dev_warn(dev,
				 "Invalid target port index found in DT: %i\n",
				 id);
			continue;
		}

		priv->smbus_port_adapter[id].of_node = tp_node;

		if (!of_property_read_string(tp_node, "realtek,mode",
					     &mode_str)) {
			if (!strcmp(mode_str, "i3c"))
				tp_setting[id].mode = I3C_HUB_DT_TP_MODE_I3C;
			else if (!strcmp(mode_str, "smbus"))
				tp_setting[id].mode = I3C_HUB_DT_TP_MODE_SMBUS;
			else if (!strcmp(mode_str, "gpio"))
				tp_setting[id].mode = I3C_HUB_DT_TP_MODE_GPIO;
			else if (!strcmp(mode_str, "disabled"))
				tp_setting[id].mode =
					I3C_HUB_DT_TP_MODE_DISABLED;
		}

		if (!of_property_read_string(tp_node, "realtek,io-mode",
					     &io_mode_str)) {
			if (!strcmp(io_mode_str, "od"))
				tp_setting[id].io_mode =
					I3C_HUB_DT_TP_IO_MODE_OD;
			else if (!strcmp(io_mode_str, "od-pp"))
				tp_setting[id].io_mode =
					I3C_HUB_DT_TP_IO_MODE_OD_PP;
		}

		tp_setting[id].pullup_en =
			of_property_read_bool(tp_node, "realtek,pullup-enable");
		tp_setting[id].always_enable =
			of_property_read_bool(tp_node, "realtek,always-enable");

		if (!of_property_read_u32(tp_node,
					  "realtek,polling-interval-ms", &val))
			tp_setting[id].poll_interval_ms = val;

		if (!of_property_read_u32(tp_node, "clock-frequency", &val)) {
			if (i3c_hub_smbus_validate_clock_frequency(val))
				tp_setting[id].clock_frequency = val;
			else
				dev_warn(dev,
					 "Unsupported TP%d smbus clock-frequency: %u Hz, using default %u Hz\n",
					 id, val,
					 tp_setting[id].clock_frequency);
		}
	}
}

static void i3c_hub_of_get_conf_static(struct device *dev,
				       const struct device_node *node)
{
	struct i3c_hub *priv = dev_get_drvdata(dev);
	u8 u8val;
	u32 val;

	priv->settings.cp0_ldo_en =
		of_property_read_bool(node, "realtek,cp0-ldo-enable");
	priv->settings.cp1_ldo_en =
		of_property_read_bool(node, "realtek,cp1-ldo-enable");
	priv->settings.tp0145_ldo_en =
		of_property_read_bool(node, "realtek,tp0145-ldo-enable");
	priv->settings.tp2367_ldo_en =
		of_property_read_bool(node, "realtek,tp2367-ldo-enable");

	if (!of_property_read_u32(node, "realtek,cp0-ldo-microvolt", &val))
		priv->settings.cp0_ldo_volt = val;
	if (!of_property_read_u32(node, "realtek,cp1-ldo-microvolt", &val))
		priv->settings.cp1_ldo_volt = val;
	if (!of_property_read_u32(node, "realtek,tp0145-ldo-microvolt", &val))
		priv->settings.tp0145_ldo_volt = val;
	if (!of_property_read_u32(node, "realtek,tp2367-ldo-microvolt", &val))
		priv->settings.tp2367_ldo_volt = val;

	if (!of_property_read_u32(node, "realtek,tp0145-pullup-ohms", &val))
		priv->settings.tp0145_pullup = val;
	if (!of_property_read_u32(node, "realtek,tp2367-pullup-ohms", &val))
		priv->settings.tp2367_pullup = val;

	if (!of_property_read_u32(node,
				  "realtek,cp0-output-impedance-ohms", &val))
		priv->settings.cp0_io_strength = val;
	if (!of_property_read_u32(node,
				  "realtek,cp1-output-impedance-ohms", &val))
		priv->settings.cp1_io_strength = val;
	if (!of_property_read_u32(node,
				  "realtek,tp0145-output-impedance-ohms", &val))
		priv->settings.tp0145_io_strength = val;
	if (!of_property_read_u32(node,
				  "realtek,tp2367-output-impedance-ohms", &val))
		priv->settings.tp2367_io_strength = val;

	priv->settings.hub_net_always_i3c =
		of_property_read_bool(node, "realtek,hub-net-always-i3c");

	if (!of_property_read_u8(node, "realtek,tp-scl-h-ack-cycles", &u8val))
		priv->settings.tp_scl_h_ack_cycles = u8val;

	i3c_hub_tp_of_get_setting(dev, node, priv->settings.tp);

	priv->settings.handshake_optimize =
		of_property_read_bool(node, "realtek,handshake-optimize");

	if (!of_property_read_u8(node, "realtek,fast-drv-h-add-cycles",
				 &u8val))
		priv->settings.fast_drv_h_add_cycles = u8val;

	priv->settings.fast_rson_en =
		of_property_read_bool(node, "realtek,fast-rson-en");

	priv->settings.tp_od_vol_optimize =
		of_property_read_bool(node, "realtek,tp-od-vol-optimize");

	priv->settings.tp_od_vref_optimize =
		of_property_read_bool(node, "realtek,tp-od-vref-optimize");
}

static const struct i3c_hub_dev_info *
i3c_hub_lookup_dev_info(struct i3c_hub *priv)
{
	int i, ret;
	u16 part_id = 0;
	u32 val = 0;

	ret = regmap_read(priv->regmap, I3C_HUB_DEV_INFO_0, &val);
	if (ret)
		return ERR_PTR(ret);

	part_id = (val & 0xFF) << 8;

	ret = regmap_read(priv->regmap, I3C_HUB_DEV_REV, &val);
	if (ret)
		return ERR_PTR(ret);

	part_id |= I3C_HUB_DEV_REV_LDO_GET(val);

	for (i = 0; i < ARRAY_SIZE(i3c_hub_dev_info_table); i++) {
		if (i3c_hub_dev_info_table[i].part_id == part_id)
			return &i3c_hub_dev_info_table[i];
	}
	return &i3c_hub_dev_info_unknown;
}

static void i3c_hub_of_default_configuration(struct device *dev)
{
	struct i3c_hub *priv = dev_get_drvdata(dev);
	int id;

	priv->settings.cp0_ldo_en = false;
	priv->settings.cp1_ldo_en = false;
	priv->settings.tp0145_ldo_en = false;
	priv->settings.tp2367_ldo_en = false;
	priv->settings.cp0_ldo_volt = 0;
	priv->settings.cp1_ldo_volt = 0;
	priv->settings.tp0145_ldo_volt = 0;
	priv->settings.tp2367_ldo_volt = 0;
	priv->settings.tp0145_pullup = UINT_MAX;
	priv->settings.tp2367_pullup = UINT_MAX;
	priv->settings.cp0_io_strength = 0;
	priv->settings.cp1_io_strength = 0;
	priv->settings.tp0145_io_strength = 0;
	priv->settings.tp2367_io_strength = 0;
	priv->settings.hub_net_always_i3c = false;
	priv->settings.tp_scl_h_ack_cycles = 0;
	priv->settings.handshake_optimize = false;
	priv->settings.fast_drv_h_add_cycles = 3;
	priv->settings.fast_rson_en = false;
	priv->settings.tp_od_vol_optimize = false;
	priv->settings.tp_od_vref_optimize = false;

	for (id = 0; id < I3C_HUB_TP_MAX_COUNT; ++id) {
		priv->settings.tp[id].mode = I3C_HUB_DT_TP_MODE_NOT_DEFINED;
		priv->settings.tp[id].pullup_en = false;
		priv->settings.tp[id].io_mode =
			I3C_HUB_DT_TP_IO_MODE_NOT_DEFINED;
		priv->settings.tp[id].poll_interval_ms = 0;
		priv->settings.tp[id].clock_frequency = 400000;
	}
}

static int i3c_hub_hw_configure_pullup(struct device *dev)
{
	struct i3c_hub *priv = dev_get_drvdata(dev);
	u8 mask = 0, value = 0;

	if (priv->settings.tp0145_pullup != UINT_MAX) {
		mask |= TP0145_PULLUP_CONF_MASK;
		if (priv->settings.tp0145_pullup != 0)
			value |= TP0145_PULLUP_CONF(
					i3c_hub_pullup_dt_to_reg(priv->settings.tp0145_pullup));
	}

	if (priv->settings.tp2367_pullup != UINT_MAX) {
		mask |= TP2367_PULLUP_CONF_MASK;
		if (priv->settings.tp2367_pullup != 0)
			value |= TP2367_PULLUP_CONF(
					i3c_hub_pullup_dt_to_reg(priv->settings.tp2367_pullup));
	}

	return regmap_update_bits(priv->regmap, I3C_HUB_LDO_AND_PULLUP_CONF,
				  mask, value);
}

static int i3c_hub_hw_configure_ldo(struct device *dev)
{
	struct i3c_hub *priv = dev_get_drvdata(dev);
	u8 ldo_config_mask = 0, ldo_config_val = 0;
	u8 ldo_disable_mask = 0, ldo_en_val = 0;
	u32 reg_val;
	int ret;
	u8 val;

	/* Enable or Disable LDO's. If there is no DT entry - disable LDO for safety reasons */
	if (priv->settings.cp0_ldo_en)
		ldo_en_val |= CP0_LDO_EN;
	if (priv->settings.cp1_ldo_en)
		ldo_en_val |= CP1_LDO_EN;
	if (priv->settings.tp0145_ldo_en)
		ldo_en_val |= TP0145_LDO_EN;
	if (priv->settings.tp2367_ldo_en)
		ldo_en_val |= TP2367_LDO_EN;

	/* Get current LDOs configuration */
	ret = regmap_read(priv->regmap, I3C_HUB_LDO_CONF, &reg_val);
	if (ret)
		return ret;

	/* LDOs Voltage level (Skip if not defined in the DT)
	 * Set the mask only if there is a change from current value
	 */
	if (priv->settings.cp0_ldo_volt != 0) {
		val = CP0_LDO_VOLTAGE(i3c_hub_ldo_dt_to_reg(priv->settings.cp0_ldo_volt));
		if ((reg_val & CP0_LDO_VOLTAGE_MASK) != val) {
			ldo_config_mask |= CP0_LDO_VOLTAGE_MASK;
			ldo_disable_mask |= CP0_LDO_EN;
			ldo_config_val |= val;
		}
	}
	if (priv->settings.cp1_ldo_volt != 0) {
		val = CP1_LDO_VOLTAGE(i3c_hub_ldo_dt_to_reg(priv->settings.cp1_ldo_volt));
		if ((reg_val & CP1_LDO_VOLTAGE_MASK) != val) {
			ldo_config_mask |= CP1_LDO_VOLTAGE_MASK;
			ldo_disable_mask |= CP1_LDO_EN;
			ldo_config_val |= val;
		}
	}
	if (priv->settings.tp0145_ldo_volt != 0) {
		val = TP0145_LDO_VOLTAGE(i3c_hub_ldo_dt_to_reg(priv->settings.tp0145_ldo_volt));
		if ((reg_val & TP0145_LDO_VOLTAGE_MASK) != val) {
			ldo_config_mask |= TP0145_LDO_VOLTAGE_MASK;
			ldo_disable_mask |= TP0145_LDO_EN;
			ldo_config_val |= val;
		}
	}
	if (priv->settings.tp2367_ldo_volt != 0) {
		val = TP2367_LDO_VOLTAGE(i3c_hub_ldo_dt_to_reg(priv->settings.tp2367_ldo_volt));
		if ((reg_val & TP2367_LDO_VOLTAGE_MASK) != val) {
			ldo_config_mask |= TP2367_LDO_VOLTAGE_MASK;
			ldo_disable_mask |= TP2367_LDO_EN;
			ldo_config_val |= val;
		}
	}

	/*
	 * Update LDO voltage configuration only if value is changed from already existing register
	 * value. It is a good practice to disable the LDO's before making any voltage changes.
	 * Presence of config mask indicates voltage change to be applied.
	 */
	if (ldo_config_mask) {
		/* Disable LDO's before making voltage changes */
		ret = regmap_update_bits(priv->regmap,
					 I3C_HUB_LDO_AND_PULLUP_CONF,
					 ldo_disable_mask, 0);
		if (ret)
			return ret;

		/* Update the LDOs configuration */
		ret = regmap_update_bits(priv->regmap, I3C_HUB_LDO_CONF,
					 ldo_config_mask, ldo_config_val);
		if (ret)
			return ret;
	}

	/* Update the LDOs Enable/disable register. This will enable only LDOs enabled in DT */
	return regmap_update_bits(priv->regmap, I3C_HUB_LDO_AND_PULLUP_CONF,
				  LDO_ENABLE_DISABLE_MASK, ldo_en_val);
}

static int i3c_hub_hw_configure_io_strength(struct device *dev)
{
	struct i3c_hub *priv = dev_get_drvdata(dev);
	u8 mask_all = 0, val_all = 0;
	u32 reg_val;
	u8 val;
	struct dt_settings tmp;
	int ret;

	/* Get IO strength configuration to figure out what needs to be changed */
	ret = regmap_read(priv->regmap, I3C_HUB_IO_STRENGTH, &reg_val);
	if (ret)
		return ret;

	tmp = priv->settings;
	if (tmp.cp0_io_strength != 0) {
		val = CP0_IO_STRENGTH(i3c_hub_io_strength_dt_to_reg(tmp.cp0_io_strength));
		mask_all |= CP0_IO_STRENGTH_MASK;
		val_all |= val;
	}
	if (tmp.cp1_io_strength != 0) {
		val = CP1_IO_STRENGTH(i3c_hub_io_strength_dt_to_reg(tmp.cp1_io_strength));
		mask_all |= CP1_IO_STRENGTH_MASK;
		val_all |= val;
	}
	if (tmp.tp0145_io_strength != 0) {
		val = TP0145_IO_STRENGTH(i3c_hub_io_strength_dt_to_reg(tmp.tp0145_io_strength));
		mask_all |= TP0145_IO_STRENGTH_MASK;
		val_all |= val;
	}
	if (tmp.tp2367_io_strength != 0) {
		val = TP2367_IO_STRENGTH(i3c_hub_io_strength_dt_to_reg(tmp.tp2367_io_strength));
		mask_all |= TP2367_IO_STRENGTH_MASK;
		val_all |= val;
	}

	/* Set IO strength if required */
	return regmap_update_bits(priv->regmap, I3C_HUB_IO_STRENGTH, mask_all,
				  val_all);
}

static int i3c_hub_hw_configure_tp(struct device *dev)
{
	struct i3c_hub *priv = dev_get_drvdata(dev);
	u8 pullup_mask = 0, pullup_val = 0;
	u8 smbus_mask = 0, smbus_val = 0;
	u8 gpio_mask = 0, gpio_val = 0;
	u8 i3c_mask = 0, i3c_val = 0;
	u8 io_mode_mask = 0, io_mode_val = 0;
	int ret;
	int i, index;

	memset(priv->gpio.port_to_index, -1, sizeof(priv->gpio.port_to_index));

	for (i = 0; i < priv->dev_info->n_ports; ++i) {
		if (priv->settings.tp[i].mode !=
		    I3C_HUB_DT_TP_MODE_NOT_DEFINED) {
			i3c_mask |= TPN_NET_CON(i);
			smbus_mask |= TPN_SMBUS_MODE_EN(i);
			gpio_mask |= TPN_GPIO_MODE_EN(i);
			io_mode_mask |= TPN_IO_MODE_CON(i);

			if (priv->settings.tp[i].mode ==
			    I3C_HUB_DT_TP_MODE_I3C) {
				i3c_val |= TPN_NET_CON(i);
			} else if (priv->settings.tp[i].mode ==
				   I3C_HUB_DT_TP_MODE_SMBUS) {
				smbus_val |= TPN_SMBUS_MODE_EN(i);
			} else if (priv->settings.tp[i].mode ==
				   I3C_HUB_DT_TP_MODE_GPIO) {
				gpio_val |= TPN_GPIO_MODE_EN(i);
				priv->gpio.nums += GPIO_BANK_SZ;
				index = priv->gpio.nums / GPIO_BANK_SZ - 1;
				priv->gpio.tp[index] = i;
				priv->gpio.port_to_index[i] = index;
			}
		}
		pullup_mask |= TPN_PULLUP_EN(i);
		if (priv->settings.tp[i].pullup_en)
			pullup_val |= TPN_PULLUP_EN(i);
		if (priv->settings.tp[i].io_mode !=
		    I3C_HUB_DT_TP_IO_MODE_NOT_DEFINED) {
			if (priv->settings.tp[i].io_mode ==
			    I3C_HUB_DT_TP_IO_MODE_OD)
				io_mode_val |= TPN_IO_MODE_CON(i);
		} else if (priv->settings.tp[i].mode ==
			   I3C_HUB_DT_TP_MODE_SMBUS) {
			io_mode_val |= TPN_IO_MODE_CON(i);
		}
	}

	ret = regmap_update_bits(priv->regmap, I3C_HUB_TP_IO_MODE_CONF,
				 io_mode_mask, io_mode_val);
	if (ret)
		return ret;

	ret = regmap_update_bits(priv->regmap, I3C_HUB_TP_PULLUP_EN,
				 pullup_mask, pullup_val);
	if (ret)
		return ret;

	ret = regmap_update_bits(priv->regmap, I3C_HUB_TP_SMBUS_AGNT_EN,
				 smbus_mask, smbus_val);
	if (ret)
		return ret;

	ret = regmap_update_bits(priv->regmap, I3C_HUB_TP_GPIO_MODE_EN,
				 gpio_mask, gpio_val);
	if (ret)
		return ret;

	/* Request for HUB Network connection in case any TP is configured in I3C mode */
	if (i3c_val) {
		ret = regmap_write(priv->regmap, I3C_HUB_CP_MUX_SET,
				   CONTROLLER_PORT_MUX_REQ);
		if (ret)
			return ret;
		/* TODO: verify if connection is done */
	}

	/* Enable TP here in case TP was configured */
	ret = regmap_update_bits(priv->regmap, I3C_HUB_TP_ENABLE,
				 i3c_mask | smbus_mask | gpio_mask,
				 i3c_val | smbus_val | gpio_val);
	if (ret)
		return ret;

	return regmap_write(priv->regmap, I3C_HUB_TP_NET_CON_CONF, i3c_val);
}

static int i3c_hub_hw_configure_misc(struct device *dev)
{
	struct i3c_hub *priv = dev_get_drvdata(dev);
	int ret;
	u8 reg = I3C_HUB_TARGET_DA_FLAG_BYTE_BASE;
	u8 val[I3C_HUB_TARGET_DA_FLAG_BYTE_COUNT];

	if (!priv->settings.hub_net_always_i3c)
		return 0;

	memset(val, 0xff, I3C_HUB_TARGET_DA_FLAG_BYTE_COUNT);

	ret = regmap_update_bits(priv->regmap, I3C_HUB_NET_OPER_MODE_CONF,
				 I3C_HUB_NET_ALWAYS_I3C_EN,
				 I3C_HUB_NET_ALWAYS_I3C_EN);
	if (ret)
		return ret;

	ret = regmap_bulk_write(priv->regmap, reg, val,
				I3C_HUB_TARGET_DA_FLAG_BYTE_COUNT);
	return ret;
}

typedef int (*i3c_hub_cfg_fn)(struct i3c_hub *priv);

static int i3c_hub_hw_cfg_with_page(struct i3c_hub *priv, u8 page,
				    i3c_hub_cfg_fn op)
{
	int ret;

	if (!op)
		return -EINVAL;

	mutex_lock(&priv->page_mutex);
	ret = regmap_write(priv->regmap, I3C_HUB_PAGE_PTR, page);
	if (ret)
		goto unlock;

	ret = op(priv);
unlock:
	regmap_write(priv->regmap, I3C_HUB_PAGE_PTR, 0x00);
	mutex_unlock(&priv->page_mutex);
	return ret;
}

static int i3c_hub_cfg_op_fuse_latch(struct i3c_hub *priv)
{
	int ret;

	if (priv->settings.handshake_optimize) {
		ret = regmap_update_bits(priv->regmap, I3C_HUB_EFUSE_OFFSET_9E,
					 I3C_HUB_IBI_ACK_RD_CYCLE_MASK,
					 I3C_HUB_IBI_ACK_RD_CYCLE_VAL);
		if (ret)
			return ret;
	}

	ret = regmap_update_bits(priv->regmap, I3C_HUB_EFUSE_OFFSET_A3,
				 I3C_HUB_FAST_DRV_LOOP_DIS,
				 I3C_HUB_FAST_DRV_LOOP_DIS);
	if (ret)
		return ret;

	if (priv->settings.tp_od_vol_optimize) {
		ret = regmap_update_bits(priv->regmap, I3C_HUB_EFUSE_OFFSET_9D,
					 I3C_HUB_TP_OD_VOL_LEVEL,
					 I3C_HUB_TP_OD_VOL_LEVEL);
		if (ret)
			return ret;
	}

	if (priv->settings.tp_od_vref_optimize) {
		ret = regmap_update_bits(priv->regmap, I3C_HUB_EFUSE_OFFSET_9D,
					 I3C_HUB_TP_OD_VREF,
					 I3C_HUB_TP_OD_VREF);
		if (ret)
			return ret;
	}

	ret = regmap_update_bits(priv->regmap, I3C_HUB_EFUSE_OFFSET_9E,
				 I3C_HUB_FAST_DRV_H_ADD_CYCLE_MASK,
				 I3C_HUB_FAST_DRV_H_ADD_CYCLE_VAL(
						priv->settings.fast_drv_h_add_cycles));
	if (ret)
		return ret;

	ret = regmap_update_bits(priv->regmap, I3C_HUB_EFUSE_OFFSET_A0,
				 I3C_HUB_FAST_RSON_EN,
				 priv->settings.fast_rson_en ? I3C_HUB_FAST_RSON_EN : 0);
	return ret;
}

static int i3c_hub_hw_configure_fuse_latch(struct device *dev)
{
	struct i3c_hub *priv = dev_get_drvdata(dev);

	return i3c_hub_hw_cfg_with_page(priv, I3C_HUB_EFUSE_PAGE,
					i3c_hub_cfg_op_fuse_latch);
}

static int i3c_hub_cfg_op_io(struct i3c_hub *priv)
{
	int ret;
	u8 reg = I3C_HUB_CFG_TP_SCL_L_ACK_CLK;

	/* cfg tp scl low ack clk */
	ret = regmap_write(priv->regmap, reg,
			   I3C_HUB_CFG_TP_SCL_L_ACK_CLK_EN |
				   I3C_HUB_CFG_TP_SCL_L_ACK_CLK_COUNT_VAL);
	if (ret)
		return ret;

	/* cfg tp scl high ack clk */
	reg = I3C_HUB_CFG_TP_SCL_H_ACK_CLK;
	if (priv->settings.tp_scl_h_ack_cycles == 0)
		return 0;

	ret = regmap_write(priv->regmap, reg,
			   I3C_HUB_CFG_TP_SCL_H_ACK_CLK_EN |
				   I3C_HUB_CFG_TP_SCL_H_ACK_CLK_COUNT_VAL(
						   priv->settings.tp_scl_h_ack_cycles));

	return ret;
}

static int i3c_hub_hw_configure_io(struct device *dev)
{
	struct i3c_hub *priv = dev_get_drvdata(dev);

	return i3c_hub_hw_cfg_with_page(priv, I3C_HUB_IO_CTRL_PAGE,
					i3c_hub_cfg_op_io);
}

static int i3c_hub_configure_hw(struct device *dev)
{
	int ret;
	struct i3c_hub *priv = dev_get_drvdata(dev);

	ret = i3c_hub_hw_configure_ldo(dev);
	if (ret)
		return ret;

	ret = i3c_hub_hw_configure_io_strength(dev);
	if (ret)
		return ret;

	ret = i3c_hub_hw_configure_pullup(dev);
	if (ret)
		return ret;

	if (priv->dev_info->part_id) {
		ret = i3c_hub_hw_configure_misc(dev);
		if (ret)
			return ret;

		ret = i3c_hub_hw_configure_fuse_latch(dev);
		if (ret)
			return ret;

		ret = i3c_hub_hw_configure_io(dev);
		if (ret)
			return ret;
	}

	ret = i3c_hub_hw_configure_tp(dev);
	return ret;
}

static void i3c_hub_of_get_conf_runtime(struct device *dev,
					const struct device_node *node)
{
	struct i3c_hub *priv = dev_get_drvdata(dev);
	struct device_node *i3c_node;
	int i3c_id;
	u8 tp_mask;

	for_each_available_child_of_node(node, i3c_node) {
		if (!i3c_node->full_name ||
		    (sscanf(i3c_node->full_name, "i3c%i@%hhx", &i3c_id,
			    &tp_mask) != 2))
			continue;

		if (i3c_id < priv->dev_info->n_ports) {
			priv->logical_bus[i3c_id].available = true;
			priv->logical_bus[i3c_id].of_node = i3c_node;
			priv->logical_bus[i3c_id].tp_map = tp_mask;
			priv->logical_bus[i3c_id].priv = priv;
			priv->logical_bus[i3c_id].tp_id = i3c_id;
		}
	}
}

static const struct i3c_device_id i3c_hub_ids[] = {
	I3C_CLASS(I3C_DCR_HUB, NULL),
	{},
};

static int i3c_hub_read_id(struct device *dev)
{
	struct i3c_hub *priv = dev_get_drvdata(dev);
	u32 reg_val;
	int ret;

	ret = regmap_read(priv->regmap, I3C_HUB_LDO_AND_CPSEL_STS, &reg_val);
	if (ret) {
		dev_err(dev, "Failed to read status register\n");
		return -1;
	}

	priv->hub_pin_sel_id = CP_SEL_PIN_INPUT_CODE_GET(reg_val);
	priv->hub_pin_cp1_id = CP_SDA1_SCL1_PINS_CODE_GET(reg_val);
	return 0;
}

static struct device_node *i3c_hub_get_dt_hub_node(struct device_node *node,
						   struct i3c_hub *priv)
{
	struct device_node *hub_node_no_id = NULL;
	struct device_node *hub_node;
	u32 hub_id;
	u32 id_mask;
	u32 dt_id;
	u32 pin_id;
	int found_id = 0;

	for_each_available_child_of_node(node, hub_node) {
		id_mask = 0;
		priv->hub_dt_sel_id = -1;
		priv->hub_dt_cp1_id = -1;

		if (strstr(hub_node->name, "hub")) {
			if (!of_property_read_u32(hub_node, "realtek,id",
						  &hub_id)) {
				id_mask |= 0x0f;
				priv->hub_dt_sel_id = hub_id;
			}

			if (!of_property_read_u32(hub_node, "realtek,id-cp1",
						  &hub_id)) {
				id_mask |= 0xf0;
				priv->hub_dt_cp1_id = hub_id;
			}

			dt_id = ((u32)priv->hub_dt_cp1_id & 0x0f) << 4 |
				((u32)priv->hub_dt_sel_id & 0x0f);
			pin_id = ((u32)priv->hub_pin_cp1_id & 0x0f) << 4 |
				 ((u32)priv->hub_pin_sel_id & 0x0f);

			if (id_mask != 0 &&
			    (dt_id & id_mask) == (pin_id & id_mask))
				found_id = 1;

			if (!found_id) {
				/*
				 * Just keep reference to first HUB node with no ID in case no ID
				 * matching
				 */
				if (!hub_node_no_id &&
				    priv->hub_dt_sel_id == -1 &&
				    priv->hub_dt_cp1_id == -1)
					hub_node_no_id = hub_node;
			} else {
				return hub_node;
			}
		}
	}

	return hub_node_no_id;
}

static int fops_access_reg_get(void *ctx, u64 *val)
{
	struct i3c_hub *priv = ctx;
	u32 reg_val;
	int ret;

	ret = regmap_read(priv->regmap, priv->reg_addr, &reg_val);
	if (ret)
		return ret;

	*val = reg_val & 0xFF;
	return 0;
}

static int fops_access_reg_set(void *ctx, u64 val)
{
	struct i3c_hub *priv = ctx;

	return regmap_write(priv->regmap, priv->reg_addr, val & 0xFF);
}

DEFINE_DEBUGFS_ATTRIBUTE(fops_access_reg, fops_access_reg_get,
			 fops_access_reg_set, "0x%llX\n");

static int i3c_hub_debugfs_init(struct i3c_hub *priv, const char *hub_id)
{
	struct dentry *entry, *dt_conf_dir, *reg_dir;
	struct dt_settings *settings = NULL;
	int i;

	entry = debugfs_create_dir(hub_id, NULL);
	if (IS_ERR(entry))
		return PTR_ERR(entry);

	priv->debug_dir = entry;

	if (priv->dev_info)
		debugfs_create_str("model", 0400, priv->debug_dir,
				   (char **)&priv->dev_info->model);

	entry = debugfs_create_dir("dt-conf", priv->debug_dir);
	if (IS_ERR(entry))
		goto err_remove;

	dt_conf_dir = entry;

	settings = &priv->settings;
	debugfs_create_bool("cp0-ldo-en", 0400, dt_conf_dir,
			    &settings->cp0_ldo_en);
	debugfs_create_bool("cp1-ldo-en", 0400, dt_conf_dir,
			    &settings->cp1_ldo_en);
	debugfs_create_u32("cp0-ldo-volt", 0400, dt_conf_dir,
			   &settings->cp0_ldo_volt);
	debugfs_create_u32("cp1-ldo-volt", 0400, dt_conf_dir,
			   &settings->cp1_ldo_volt);
	debugfs_create_bool("tp0145-ldo-en", 0400, dt_conf_dir,
			    &settings->tp0145_ldo_en);
	debugfs_create_bool("tp2367-ldo-en", 0400, dt_conf_dir,
			    &settings->tp2367_ldo_en);
	debugfs_create_u32("tp0145-ldo-volt", 0400, dt_conf_dir,
			   &settings->tp0145_ldo_volt);
	debugfs_create_u32("tp2367-ldo-volt", 0400, dt_conf_dir,
			   &settings->tp2367_ldo_volt);
	debugfs_create_u32("tp0145-pullup", 0400, dt_conf_dir,
			   &settings->tp0145_pullup);
	debugfs_create_u32("tp2367-pullup", 0400, dt_conf_dir,
			   &settings->tp2367_pullup);
	debugfs_create_bool("hub-net-always-i3c", 0400, dt_conf_dir,
			    &settings->hub_net_always_i3c);
	debugfs_create_u8("tp-scl-h-ack-cycles", 0400, dt_conf_dir,
			  &settings->tp_scl_h_ack_cycles);
	debugfs_create_bool("handshake-optimize", 0400, dt_conf_dir,
			    &settings->handshake_optimize);
	debugfs_create_u8("fast-drv-h-add-cycles", 0400, dt_conf_dir,
			  &settings->fast_drv_h_add_cycles);
	debugfs_create_bool("fast-rson-en", 0400, dt_conf_dir,
			    &settings->fast_rson_en);
	debugfs_create_bool("tp-od-vol-optimize", 0400, dt_conf_dir,
			    &settings->tp_od_vol_optimize);
	debugfs_create_bool("tp-od-vref-optimize", 0400, dt_conf_dir,
			    &settings->tp_od_vref_optimize);

	for (i = 0; i < priv->dev_info->n_ports; ++i) {
		char file_name[32];

		sprintf(file_name, "tp%i.mode", i);
		debugfs_create_u8(file_name, 0400, dt_conf_dir,
				  &settings->tp[i].mode);
		sprintf(file_name, "tp%i.pullup_en", i);
		debugfs_create_bool(file_name, 0400, dt_conf_dir,
				    &settings->tp[i].pullup_en);
		sprintf(file_name, "tp%i.io_mode", i);
		debugfs_create_u8(file_name, 0400, dt_conf_dir,
				  &settings->tp[i].io_mode);
		sprintf(file_name, "tp%i.poll_interval_ms", i);
		debugfs_create_u32(file_name, 0400, dt_conf_dir,
				   &settings->tp[i].poll_interval_ms);
		sprintf(file_name, "tp%i.clock_frequency", i);
		debugfs_create_u32(file_name, 0400, dt_conf_dir,
				   &settings->tp[i].clock_frequency);
	}

	entry = debugfs_create_dir("reg", priv->debug_dir);
	if (IS_ERR(entry))
		goto err_remove;

	reg_dir = entry;

	entry = debugfs_create_file_unsafe("access", 0600, reg_dir, priv,
					   &fops_access_reg);
	if (IS_ERR(entry))
		goto err_remove;

	debugfs_create_u8("offset", 0600, reg_dir, &priv->reg_addr);

	return 0;

err_remove:
	debugfs_remove_recursive(priv->debug_dir);
	return PTR_ERR(entry);
}

static void i3c_hub_trans_pre_cb(struct logical_bus *bus)
{
	struct i3c_hub *priv = bus->priv;
	struct device *dev = i3cdev_to_dev(priv->i3cdev);
	int ret;

	if (priv->settings.tp[bus->tp_id].always_enable)
		return;

	ret = regmap_update_bits(priv->regmap, I3C_HUB_TP_NET_CON_CONF,
				 GENMASK(bus->tp_id, bus->tp_id), bus->tp_map);
	if (ret)
		dev_warn(dev, "Failed to open Target Port(s)\n");
}

static void i3c_hub_trans_post_cb(struct logical_bus *bus)
{
	struct i3c_hub *priv = bus->priv;
	struct device *dev = i3cdev_to_dev(priv->i3cdev);
	int ret;

	if (priv->settings.tp[bus->tp_id].always_enable)
		return;

	ret = regmap_update_bits(priv->regmap, I3C_HUB_TP_NET_CON_CONF,
				 GENMASK(bus->tp_id, bus->tp_id), 0x00);
	if (ret)
		dev_warn(dev, "Failed to close Target Port(s)\n");
}

static struct logical_bus *bus_from_i3c_desc(struct i3c_dev_desc *desc)
{
	struct i3c_master_controller *controller = i3c_dev_get_master(desc);

	return container_of(controller, struct logical_bus, controller);
}

static struct logical_bus *bus_from_i2c_desc(struct i2c_dev_desc *desc)
{
	struct i3c_master_controller *controller = i2c_dev_get_master(desc);

	return container_of(controller, struct logical_bus, controller);
}

static struct i3c_master_controller *
parent_from_controller(struct i3c_master_controller *controller)
{
	struct logical_bus *bus =
		container_of(controller, struct logical_bus, controller);

	return bus->priv->driving_master;
}

static struct i3c_master_controller *
parent_controller_from_i3c_desc(struct i3c_dev_desc *desc)
{
	struct i3c_master_controller *controller = i3c_dev_get_master(desc);
	struct logical_bus *bus =
		container_of(controller, struct logical_bus, controller);

	return bus->priv->driving_master;
}

static struct i3c_master_controller *
parent_controller_from_i2c_desc(struct i2c_dev_desc *desc)
{
	struct i3c_master_controller *controller = desc->common.master;
	struct logical_bus *bus =
		container_of(controller, struct logical_bus, controller);

	return bus->priv->driving_master;
}

static struct i3c_master_controller *
update_i3c_i2c_desc_parent(struct i3c_i2c_dev_desc *desc,
			   struct i3c_master_controller *parent)
{
	struct i3c_master_controller *orig_parent = desc->master;

	desc->master = parent;

	return orig_parent;
}

static void restore_i3c_i2c_desc_parent(struct i3c_i2c_dev_desc *desc,
					struct i3c_master_controller *parent)
{
	desc->master = parent;
}

static int i3c_hub_read_transaction_status(struct i3c_hub *priv, u8 target_port,
					   u8 target_port_status, u32 data_len)
{
	unsigned int status_read;
	int ret;
	struct i2c_adapter_group *smbus =
		&priv->smbus_port_adapter[target_port];
	u32 smbus_clk = priv->settings.tp[target_port].clock_frequency / 1000;
	u8 status;
	u8 ret_code;

	if (!priv->settings.tp[target_port].poll_interval_ms) {
		ret = wait_for_completion_timeout(&smbus->completion,
						  smbus->i2c.timeout);
		if (!ret) {
			dev_err(&priv->i3cdev->dev,
				"Status read timeout reached on target port %d\n",
				target_port);
			return -ETIMEDOUT;
		}

		status = (u8)smbus->status &
			 I3C_HUB_CONTROLLER_AGENT_STATUS_MASK;
	} else {
		ret = regmap_read_poll_timeout(priv->regmap,
					       target_port_status,
					       status_read,
					       (u8)status_read &
					       I3C_HUB_CONTROLLER_AGENT_FINISH_FLAG,
					       I3C_HUB_SMBUS_STATUS_READ_INTERVAL_US_CEIL(
						       data_len, smbus_clk),
					       jiffies_to_usecs(smbus->i2c.timeout));

		if (ret) {
			dev_err(&priv->i3cdev->dev,
				"Status read timeout reached on target port %d\n",
				target_port);
			return ret;
		}

		ret = regmap_write(priv->regmap, target_port_status,
				   I3C_HUB_CONTROLLER_AGENT_FINISH_FLAG);
		if (ret)
			return ret;

		status = (u8)status_read & I3C_HUB_CONTROLLER_AGENT_STATUS_MASK;
	}

	ret_code = status >> I3C_HUB_CONTROLLER_AGENT_RET_CODE_SHIFT;

	switch (ret_code) {
	case I3C_HUB_CONTROLLER_AGENT_RET_CODE_SUCCESS:
		return 0;
	case I3C_HUB_CONTROLLER_AGENT_RET_CODE_ADDRESS_NACK:
		dev_dbg(&priv->i3cdev->dev,
			"TP%u SMBus: Address NACK (device not present)\n",
			target_port);
		return -ENXIO;
	case I3C_HUB_CONTROLLER_AGENT_RET_CODE_DEVICE_BUSY:
		dev_dbg(&priv->i3cdev->dev,
			"TP%u SMBus: Device busy (data NACK after address ACK)\n",
			target_port);
		return -EREMOTEIO;
	case I3C_HUB_CONTROLLER_AGENT_RET_CODE_READ_NOT_READY:
		dev_dbg(&priv->i3cdev->dev,
			"TP%u SMBus: Device read not ready (read address NACK after write)\n",
			target_port);
		return -ENXIO;
	case I3C_HUB_CONTROLLER_AGENT_RET_CODE_SYNC_RECOVERED:
		dev_dbg(&priv->i3cdev->dev,
			"TP%u SMBus: Sync issue recovered (SDA stuck low, recovered by 9 SCL pulses)\n",
			target_port);
		return -EAGAIN;
	case I3C_HUB_CONTROLLER_AGENT_RET_CODE_SYNC_BUS_CLEAR:
		dev_dbg(&priv->i3cdev->dev,
			"TP%u SMBus: Sync issue bus clear (recovered by SCL low 35ms)\n",
			target_port);
		return -EAGAIN;
	case I3C_HUB_CONTROLLER_AGENT_RET_CODE_BUS_FAULT:
		dev_err(&priv->i3cdev->dev,
			"TP%u SMBus: Bus fault (SDA stuck low remains after recovery)\n",
			target_port);
		return -EIO;
	case I3C_HUB_CONTROLLER_AGENT_RET_CODE_ARBITRATION_LOST:
		dev_dbg(&priv->i3cdev->dev, "TP%u SMBus: Arbitration lost\n",
			target_port);
		return -EAGAIN;
	case I3C_HUB_CONTROLLER_AGENT_RET_CODE_SCL_TIMEOUT:
		dev_err(&priv->i3cdev->dev, "TP%u SMBus: SCL timeout\n",
			target_port);
		return -ETIMEDOUT;
	default:
		dev_err(&priv->i3cdev->dev,
			"TP%u SMBus: Reserved/unknown return code 0x%x\n",
			target_port, ret_code);
		return -EIO;
	}
}

/*
 * i3c_hub_smbus_msg() - This starts a smbus transaction by writing a descriptor
 * and a message to the hub registers. Controller buffer page is determined by multiplying the
 * target port index by four and adding the base page number to it.
 * @hub: a pointer to the i3c hub main structure
 * @target_port: a number of the port where the transaction will happen
 * @xfers: i2c_msg struct received from the master_xfers callback
 * @nxfers_i: the number of the current message
 * @xfer_type: transfer type:
 *   - I3C_HUB_SMBUS_XFER_WRITE (0): single write
 *   - I3C_HUB_SMBUS_XFER_READ  (1): single read
 *   - I3C_HUB_SMBUS_XFER_WR_RD (2): write followed by read
 *     (uses xfers[nxfers_i] as write and xfers[nxfers_i+1] as read)
 *
 * Return: 0 on success, negative errno on failure from hub status or regmap ops.
 * Note: for WR_RD the caller must ensure xfers[nxfers_i+1] exists, the address
 * matches, and write_len + read_len <= I3C_HUB_SMBUS_PAYLOAD_SIZE.
 */
static int i3c_hub_smbus_msg(struct i3c_hub *hub, struct i2c_msg *xfers,
			     u8 target_port, u8 nxfers_i, u8 xfer_type)
{
	u8 transaction_type = I3C_HUB_SMBUS_400_KHZ;
	u8 controller_buffer_page =
		I3C_HUB_CONTROLLER_BUFFER_PAGE + 4 * target_port;
	int write_length = 0, read_length = 0;
	u8 target_port_status = I3C_HUB_TP0_SMBUS_AGNT_STS + target_port;
	u8 target_port_code = BIT(target_port);
	u8 rw_address = xfers[nxfers_i].addr << 1;
	u8 desc[I3C_HUB_SMBUS_DESCRIPTOR_SIZE] = { 0 };
	int ret = 0;

	transaction_type =
		i3c_hub_smbus_rate_bits_from_hz(hub->settings.tp[target_port].clock_frequency);

	switch (xfer_type) {
	case I3C_HUB_SMBUS_XFER_WRITE:
		write_length = xfers[nxfers_i].len;
		break;
	case I3C_HUB_SMBUS_XFER_READ:
		read_length = xfers[nxfers_i].len;
		rw_address |= BIT(0);
		break;
	case I3C_HUB_SMBUS_XFER_WR_RD:
		write_length = xfers[nxfers_i].len;
		read_length = xfers[nxfers_i + 1].len;
		transaction_type |= BIT(0);
		break;
	default:
		return -EINVAL;
	}

	/* Assemble descriptor */
	desc[0] = rw_address;
	desc[1] = transaction_type;
	desc[2] = write_length;
	desc[3] = read_length;

	mutex_lock(&hub->page_mutex);
	ret = regmap_write(hub->regmap, I3C_HUB_PAGE_PTR,
			   controller_buffer_page);
	if (ret)
		goto unlock;

	ret = regmap_bulk_write(hub->regmap, I3C_HUB_CONTROLLER_AGENT_BUFF,
				desc, I3C_HUB_SMBUS_DESCRIPTOR_SIZE);
	if (ret)
		goto unlock;

	if (write_length) {
		ret = regmap_bulk_write(hub->regmap,
					I3C_HUB_CONTROLLER_AGENT_BUFF_DATA,
					xfers[nxfers_i].buf, write_length);
		if (ret)
			goto unlock;
	}

	ret = regmap_write(hub->regmap, I3C_HUB_PAGE_PTR, 0x00);
	mutex_unlock(&hub->page_mutex);
	if (ret)
		return ret;

	/* Start transaction */
	ret = regmap_write(hub->regmap, I3C_HUB_TP_SMBUS_AGNT_TRANS_START,
			   target_port_code);
	if (ret)
		return ret;

	/* Get transaction status */
	ret = i3c_hub_read_transaction_status(hub, target_port,
					      target_port_status,
					      write_length + read_length);
	if (ret)
		return ret;

	/* if read_length is non-zero, read back the data */
	if (read_length) {
		mutex_lock(&hub->page_mutex);
		ret = regmap_write(hub->regmap, I3C_HUB_PAGE_PTR,
				   controller_buffer_page);
		if (ret)
			goto unlock;

		if (xfer_type == I3C_HUB_SMBUS_XFER_READ) {
			ret = regmap_bulk_read(hub->regmap,
					       I3C_HUB_CONTROLLER_AGENT_BUFF_DATA,
					       xfers[nxfers_i].buf, read_length);
		} else {
			ret = regmap_bulk_read(hub->regmap,
					       I3C_HUB_CONTROLLER_AGENT_BUFF_DATA +
					       write_length,
					       xfers[nxfers_i + 1].buf, read_length);
		}
		if (ret)
			goto unlock;

		ret = regmap_write(hub->regmap, I3C_HUB_PAGE_PTR, 0x00);
		mutex_unlock(&hub->page_mutex);
	}

	return ret;
unlock:
	regmap_write(hub->regmap, I3C_HUB_PAGE_PTR, 0x00);
	mutex_unlock(&hub->page_mutex);
	return ret;
}

static inline bool i3c_hub_can_combine_wr_rd(const struct i2c_msg *w,
					     const struct i2c_msg *r)
{
	/* w: write, r: read; same addr; total length within payload */
	return !(w->flags & I2C_M_RD) && (r->flags & I2C_M_RD) &&
	       w->addr == r->addr &&
	       (w->len + r->len) <= I3C_HUB_SMBUS_PAYLOAD_SIZE;
}

/**
 * i3c_hub_smbus_port_adapter_xfer() - i3c hub smbus transfer logic
 * @adap: i2c_adapter corresponding with single port in the i3c hub
 * @xfers: all messages descriptors and data
 * @nxfers: amount of single messages in a transfer
 *
 * Return: function returns the sum of correctly sent messages (only those with hub return
 * status 0x01)
 */
static int i3c_hub_smbus_port_adapter_xfer(struct i2c_adapter *adap,
					   struct i2c_msg *xfers, int nxfers)
{
	struct i2c_adapter_group *smbus = i2c_get_adapdata(adap);
	struct i3c_hub *hub = smbus->priv;
	int ret_sum = 0, ret, len, type, nxfers_i;
	const struct i2c_msg *cur = NULL, *next = NULL;

	for (nxfers_i = 0; nxfers_i < nxfers; nxfers_i++) {
		cur = &xfers[nxfers_i];
		len = cur->len;
		type = cur->flags & I2C_M_RD ? I3C_HUB_SMBUS_XFER_READ :
					       I3C_HUB_SMBUS_XFER_WRITE;

		/* Per-message length limit check */
		if (len > I3C_HUB_SMBUS_PAYLOAD_SIZE) {
			dev_err(&adap->dev,
				"Message nr. %d not sent - length over %d bytes.\n",
				nxfers_i, I3C_HUB_SMBUS_PAYLOAD_SIZE);
			continue;
		}

		/* Try to combine write followed by read to the same address */
		if (type == I3C_HUB_SMBUS_XFER_WRITE &&
		    (nxfers_i + 1) < nxfers) {
			next = &xfers[nxfers_i + 1];
			if (i3c_hub_can_combine_wr_rd(cur, next))
				type = I3C_HUB_SMBUS_XFER_WR_RD;
		}

		ret = i3c_hub_smbus_msg(hub, xfers, smbus->tp_port, nxfers_i,
					type);
		if (ret)
			return ret;

		if (type == I3C_HUB_SMBUS_XFER_WR_RD) {
			ret_sum += 2;
			nxfers_i++; /* skip the next read message */

		} else {
			ret_sum++;
		}
	}
	return ret_sum;
}

static int i3c_hub_bus_init(struct i3c_master_controller *controller)
{
	struct logical_bus *bus =
		container_of(controller, struct logical_bus, controller);

	controller->this = bus->priv->i3cdev->desc;
	return 0;
}

static void i3c_hub_bus_cleanup(struct i3c_master_controller *controller)
{
	controller->this = NULL;
}

static int i3c_hub_attach_i3c_dev(struct i3c_dev_desc *dev)
{
	struct i3c_master_controller *parent =
		parent_controller_from_i3c_desc(dev);
	struct i3c_master_controller *orig_parent;
	int ret;

	orig_parent = update_i3c_i2c_desc_parent(&dev->common, parent);
	ret = parent->ops->attach_i3c_dev(dev);
	restore_i3c_i2c_desc_parent(&dev->common, orig_parent);
	return ret;
}

static int i3c_hub_reattach_i3c_dev(struct i3c_dev_desc *dev, u8 old_dyn_addr)
{
	struct i3c_master_controller *parent =
		parent_controller_from_i3c_desc(dev);
	struct i3c_master_controller *orig_parent;
	int ret;

	orig_parent = update_i3c_i2c_desc_parent(&dev->common, parent);
	ret = parent->ops->reattach_i3c_dev(dev, old_dyn_addr);
	restore_i3c_i2c_desc_parent(&dev->common, orig_parent);
	return ret;
}

static void i3c_hub_detach_i3c_dev(struct i3c_dev_desc *dev)
{
	struct i3c_master_controller *parent =
		parent_controller_from_i3c_desc(dev);
	struct i3c_master_controller *orig_parent;

	orig_parent = update_i3c_i2c_desc_parent(&dev->common, parent);
	parent->ops->detach_i3c_dev(dev);
	restore_i3c_i2c_desc_parent(&dev->common, orig_parent);
}

static int i3c_hub_do_daa(struct i3c_master_controller *controller)
{
	struct i3c_master_controller *parent =
		parent_from_controller(controller);
	int ret;

	down_write(&parent->bus.lock);
	ret = parent->ops->do_daa(parent);
	up_write(&parent->bus.lock);
	return ret;
}

static bool i3c_hub_supports_ccc_cmd(struct i3c_master_controller *controller,
				     const struct i3c_ccc_cmd *cmd)
{
	struct i3c_master_controller *parent =
		parent_from_controller(controller);

	return parent->ops->supports_ccc_cmd(parent, cmd);
}

static int i3c_hub_send_ccc_cmd(struct i3c_master_controller *controller,
				struct i3c_ccc_cmd *cmd)
{
	struct i3c_master_controller *parent =
		parent_from_controller(controller);
	struct logical_bus *bus =
		container_of(controller, struct logical_bus, controller);
	int ret;

	if (cmd->id == I3C_CCC_RSTDAA(true))
		return 0;

	i3c_hub_trans_pre_cb(bus);
	down_read(&parent->bus.lock);
	ret = parent->ops->send_ccc_cmd(parent, cmd);
	up_read(&parent->bus.lock);
	i3c_hub_trans_post_cb(bus);

	return ret;
}

static int i3c_hub_priv_xfers(struct i3c_dev_desc *dev,
			      struct i3c_xfer *xfers, int nxfers,
			      enum i3c_xfer_mode mode)
{
	struct i3c_master_controller *parent =
		parent_controller_from_i3c_desc(dev);
	struct i3c_master_controller *orig_parent;
	struct logical_bus *bus = bus_from_i3c_desc(dev);
	int res;

	i3c_hub_trans_pre_cb(bus);
	orig_parent = update_i3c_i2c_desc_parent(&dev->common, parent);
	down_read(&parent->bus.lock);
	res = parent->ops->i3c_xfers(dev, xfers, nxfers, mode);
	up_read(&parent->bus.lock);
	restore_i3c_i2c_desc_parent(&dev->common, orig_parent);
	i3c_hub_trans_post_cb(bus);

	return res;
}

static int i3c_hub_attach_i2c_dev(struct i2c_dev_desc *dev)
{
	struct i3c_master_controller *parent =
		parent_controller_from_i2c_desc(dev);
	struct i3c_master_controller *orig_parent;
	int ret;

	orig_parent = update_i3c_i2c_desc_parent(&dev->common, parent);
	ret = parent->ops->attach_i2c_dev(dev);
	restore_i3c_i2c_desc_parent(&dev->common, orig_parent);
	return ret;
}

static void i3c_hub_detach_i2c_dev(struct i2c_dev_desc *dev)
{
	struct i3c_master_controller *parent =
		parent_controller_from_i2c_desc(dev);
	struct i3c_master_controller *orig_parent;

	orig_parent = update_i3c_i2c_desc_parent(&dev->common, parent);
	parent->ops->detach_i2c_dev(dev);
	restore_i3c_i2c_desc_parent(&dev->common, orig_parent);
}

static int i3c_hub_i2c_xfers(struct i2c_dev_desc *dev, struct i2c_msg *xfers,
			     int nxfers)
{
	struct i3c_master_controller *parent =
		parent_controller_from_i2c_desc(dev);
	struct logical_bus *bus = bus_from_i2c_desc(dev);
	struct i3c_master_controller *orig_parent;
	int ret;

	i3c_hub_trans_pre_cb(bus);
	orig_parent = update_i3c_i2c_desc_parent(&dev->common, parent);
	ret = parent->ops->i2c_xfers(dev, xfers, nxfers);
	restore_i3c_i2c_desc_parent(&dev->common, orig_parent);
	i3c_hub_trans_post_cb(bus);
	return ret;
}

static int i3c_hub_request_ibi(struct i3c_dev_desc *dev,
			       const struct i3c_ibi_setup *req)
{
	struct i3c_master_controller *parent =
		parent_controller_from_i3c_desc(dev);
	struct logical_bus *bus = bus_from_i3c_desc(dev);
	struct i3c_master_controller *orig_parent;
	int ret;

	i3c_hub_trans_pre_cb(bus);
	orig_parent = update_i3c_i2c_desc_parent(&dev->common, parent);
	down_read(&parent->bus.lock);
	ret = parent->ops->request_ibi(dev, req);
	up_read(&parent->bus.lock);
	restore_i3c_i2c_desc_parent(&dev->common, orig_parent);
	i3c_hub_trans_post_cb(bus);
	return ret;
}

static void i3c_hub_free_ibi(struct i3c_dev_desc *dev)
{
	struct i3c_master_controller *parent =
		parent_controller_from_i3c_desc(dev);
	struct logical_bus *bus = bus_from_i3c_desc(dev);
	struct i3c_master_controller *orig_parent;

	i3c_hub_trans_pre_cb(bus);
	orig_parent = update_i3c_i2c_desc_parent(&dev->common, parent);
	down_read(&parent->bus.lock);
	parent->ops->free_ibi(dev);
	up_read(&parent->bus.lock);
	restore_i3c_i2c_desc_parent(&dev->common, orig_parent);
	i3c_hub_trans_post_cb(bus);
}

static int i3c_hub_enable_ibi(struct i3c_dev_desc *dev)
{
	struct i3c_master_controller *parent =
		parent_controller_from_i3c_desc(dev);
	struct logical_bus *bus = bus_from_i3c_desc(dev);
	struct i3c_master_controller *orig_parent;
	int ret;

	i3c_hub_trans_pre_cb(bus);
	orig_parent = update_i3c_i2c_desc_parent(&dev->common, parent);
	down_read(&parent->bus.lock);
	ret = parent->ops->enable_ibi(dev);
	up_read(&parent->bus.lock);
	restore_i3c_i2c_desc_parent(&dev->common, orig_parent);
	i3c_hub_trans_post_cb(bus);
	return ret;
}

static int i3c_hub_disable_ibi(struct i3c_dev_desc *dev)
{
	struct i3c_master_controller *parent =
		parent_controller_from_i3c_desc(dev);
	struct logical_bus *bus = bus_from_i3c_desc(dev);
	struct i3c_master_controller *orig_parent;
	int ret;

	i3c_hub_trans_pre_cb(bus);
	orig_parent = update_i3c_i2c_desc_parent(&dev->common, parent);
	down_read(&parent->bus.lock);
	ret = parent->ops->disable_ibi(dev);
	up_read(&parent->bus.lock);
	restore_i3c_i2c_desc_parent(&dev->common, orig_parent);
	i3c_hub_trans_post_cb(bus);
	return ret;
}

static void i3c_hub_recycle_ibi_slot(struct i3c_dev_desc *dev,
				     struct i3c_ibi_slot *slot)
{
	struct i3c_master_controller *parent =
		parent_controller_from_i3c_desc(dev);
	struct i3c_master_controller *orig_parent;

	orig_parent = update_i3c_i2c_desc_parent(&dev->common, parent);
	parent->ops->recycle_ibi_slot(dev, slot);
	restore_i3c_i2c_desc_parent(&dev->common, orig_parent);
}

static const struct i3c_master_controller_ops i3c_hub_i3c_ops = {
	.bus_init = i3c_hub_bus_init,
	.bus_cleanup = i3c_hub_bus_cleanup,
	.attach_i3c_dev = i3c_hub_attach_i3c_dev,
	.reattach_i3c_dev = i3c_hub_reattach_i3c_dev,
	.detach_i3c_dev = i3c_hub_detach_i3c_dev,
	.do_daa = i3c_hub_do_daa,
	.supports_ccc_cmd = i3c_hub_supports_ccc_cmd,
	.send_ccc_cmd = i3c_hub_send_ccc_cmd,
	.i3c_xfers = i3c_hub_priv_xfers,
	.attach_i2c_dev = i3c_hub_attach_i2c_dev,
	.detach_i2c_dev = i3c_hub_detach_i2c_dev,
	.i2c_xfers = i3c_hub_i2c_xfers,
	.request_ibi = i3c_hub_request_ibi,
	.free_ibi = i3c_hub_free_ibi,
	.enable_ibi = i3c_hub_enable_ibi,
	.disable_ibi = i3c_hub_disable_ibi,
	.recycle_ibi_slot = i3c_hub_recycle_ibi_slot,
};

static int i3c_hub_logic_register(struct i3c_master_controller *controller,
				  struct device *parent)
{
	return i3c_master_register(controller, parent, &i3c_hub_i3c_ops, false);
}

static u32 i3c_hub_smbus_funcs(struct i2c_adapter *adapter)
{
	return (I2C_FUNC_SMBUS_EMUL | I2C_FUNC_I2C) & ~I2C_FUNC_SMBUS_QUICK;
}

#if IS_ENABLED(CONFIG_I2C_SLAVE)
static int reg_i2c_target(struct i2c_client *client)
{
	struct i2c_adapter_group *smbus = i2c_get_adapdata(client->adapter);
	struct smbus_backend *backend;
	int ret = 0;

	if (!smbus)
		return -EINVAL;

	mutex_lock(&smbus->mutex);

	list_for_each_entry(backend, &smbus->backend_entry, list) {
		if (backend->client->addr == client->addr) {
			ret = -EBUSY;
			goto out;
		}
	}

	backend = kzalloc(sizeof(*backend), GFP_KERNEL);
	if (!backend) {
		ret = -ENOMEM;
		goto out;
	}

	backend->client = client;
	list_add(&backend->list, &smbus->backend_entry);

out:
	mutex_unlock(&smbus->mutex);
	return ret;
}

static int unreg_i2c_target(struct i2c_client *client)
{
	struct i2c_adapter_group *smbus = i2c_get_adapdata(client->adapter);
	struct smbus_backend *backend;
	bool found = false;

	if (!smbus)
		return -EINVAL;

	mutex_lock(&smbus->mutex);

	list_for_each_entry(backend, &smbus->backend_entry, list) {
		if (backend->client->addr == client->addr) {
			list_del(&backend->list);
			kfree(backend);
			found = true;
			break;
		}
	}

	mutex_unlock(&smbus->mutex);
	return found ? 0 : -ENODEV;
}
#endif /* CONFIG_I2C_SLAVE */

static const struct i2c_algorithm i3c_hub_smbus_algo = {
	.master_xfer = i3c_hub_smbus_port_adapter_xfer,
	.functionality = i3c_hub_smbus_funcs,
#if IS_ENABLED(CONFIG_I2C_SLAVE)
	.reg_slave = reg_i2c_target,
	.unreg_slave = unreg_i2c_target,
#endif
};

static void i3c_hub_delayed_work(struct work_struct *work)
{
	struct i3c_hub *priv =
		container_of(work, typeof(*priv), delayed_work.work);
	struct device *dev = i3cdev_to_dev(priv->i3cdev);
	struct logical_bus *bus;
	struct i2c_adapter_group *smbus;
	int ret;
	int i;
	unsigned int reg_val = 0;

	/* record reg 81: tp hubnetwork connection setting */
	ret = regmap_read(priv->regmap, I3C_HUB_TP_NET_CON_CONF, &reg_val);
	if (ret) {
		dev_warn(dev, "Failed to read hubnetwork connection setting\n");
		return;
	}

	ret = regmap_write(priv->regmap, I3C_HUB_TP_NET_CON_CONF, 0x00);
	if (ret) {
		dev_warn(dev, "Failed to close Target Port(s)\n");
		return;
	}

	for (i = 0; i < priv->dev_info->n_ports; ++i) {
		bus = &priv->logical_bus[i];
		if (bus->available) {
			ret = regmap_update_bits(priv->regmap,
						 I3C_HUB_TP_NET_CON_CONF,
						 GENMASK(bus->tp_id, bus->tp_id),
						 bus->tp_map);
			if (ret) {
				dev_warn(dev,
					 "Failed to open Target Port(s)\n");
				return;
			}

			dev->of_node = bus->of_node;
			ret = i3c_hub_logic_register(&bus->controller, dev);
			if (ret) {
				dev_warn(dev,
					 "Failed to register i3c controller - bus id:%i\n",
					 i);
				return;
			}
			bus->registered = true;

			ret = regmap_update_bits(priv->regmap,
						 I3C_HUB_TP_NET_CON_CONF,
						 GENMASK(bus->tp_id, bus->tp_id),
						 0x00);
			if (ret) {
				dev_warn(dev,
					 "Failed to close Target Port(s)\n");
				return;
			}

			if (!priv->settings.tp[i].always_enable)
				reg_val &= ~GENMASK(bus->tp_id, bus->tp_id);
		}
	}

	/* update tp hubnetwork connection setting */
	ret = regmap_write(priv->regmap, I3C_HUB_TP_NET_CON_CONF, reg_val);
	if (ret) {
		dev_warn(dev, "Failed to open Target Port(s)\n");
		return;
	}

	ret = i3c_master_do_daa(priv->driving_master);
	if (ret) {
		dev_warn(dev, "Failed to run DAA\n");
		return;
	}

	for (i = 0; i < priv->dev_info->n_ports; i++) {
		smbus = &priv->smbus_port_adapter[i];
		if (!smbus->used)
			continue;

		if (!priv->settings.tp[i].poll_interval_ms)
			continue;

		schedule_delayed_work(&smbus->delayed_work_polling,
				      msecs_to_jiffies(priv->settings.tp[i].poll_interval_ms));
	}
}

static int send_smbus_target_data_to_backend(struct i2c_adapter_group *smbus,
					     u8 address, u8 *local_buffer,
					     u8 len)
{
#if IS_ENABLED(CONFIG_I2C_SLAVE)
	struct smbus_backend *backend;
	struct i2c_client *client;
	int i, ret;
	u8 tmp;

	mutex_lock(&smbus->mutex);

	list_for_each_entry(backend, &smbus->backend_entry, list) {
		client = backend->client;
		if (client->addr == address >> 1) {
			mutex_unlock(&smbus->mutex);
			ret = i2c_slave_event(client, I2C_SLAVE_WRITE_REQUESTED,
					      &address);
			if (ret)
				return ret;

			for (i = 0; i < len; i++) {
				ret = i2c_slave_event(client,
						      I2C_SLAVE_WRITE_RECEIVED,
						      &local_buffer[i]);
				if (ret)
					return ret;
			}

			return i2c_slave_event(client, I2C_SLAVE_STOP, &tmp);
		}
	}

	mutex_unlock(&smbus->mutex);
#endif /* CONFIG_I2C_SLAVE */
	return -ENXIO;
}

static int read_smbus_target_buffer_page(struct i2c_adapter_group *smbus,
					 u8 target_buffer_page, u8 *address,
					 u8 *local_buffer, u8 *len)
{
	struct i3c_hub *hub = smbus->priv;
	struct device *dev = i3cdev_to_dev(hub->i3cdev);
	u32 status;
	int ret;

	mutex_lock(&hub->page_mutex);
	regmap_write(hub->regmap, I3C_HUB_PAGE_PTR, target_buffer_page);

	ret = regmap_read(hub->regmap, I3C_HUB_TARGET_BUFF_LENGTH, &status);
	if (ret)
		goto error;

	*len = status - 1;
	if (!*len)
		goto error;

	if (*len > I3C_HUB_SMBUS_TARGET_PAYLOAD_SIZE) {
		dev_warn_ratelimited(dev,
				     "Received message too big for hub buffer\n");
		ret = -EMSGSIZE;
		*len = 0;
		goto error;
	}

	ret = regmap_read(hub->regmap, I3C_HUB_TARGET_BUFF_ADDRESS, &status);
	if (ret)
		goto error;

	*address = status;

	ret = regmap_bulk_read(hub->regmap, I3C_HUB_TARGET_BUFF_DATA,
			       local_buffer, *len);

error:
	regmap_write(hub->regmap, I3C_HUB_PAGE_PTR, 0x00);
	mutex_unlock(&hub->page_mutex);
	return ret;
}

static int process_smbus_controller_status(struct i2c_adapter_group *smbus,
					   u8 reg, u32 status)
{
	struct i3c_hub *hub = smbus->priv;
	int ret = 0;

	if (status & I3C_HUB_CONTROLLER_AGENT_FINISH_FLAG) {
		smbus->status = status;
		ret = regmap_write(hub->regmap, reg,
				   I3C_HUB_CONTROLLER_AGENT_FINISH_FLAG);
		complete(&smbus->completion);
	}

	return ret;
}

/**
 * Controller buffer page is determined by adding the first buffer page number to port
 * index multiplied by four. The two target buffer page numbers are determined the same
 * way but they are offset by 2 and 3 from the controller page.
 */
static int process_smbus_target_status(struct i2c_adapter_group *smbus, u8 reg,
				       u32 status)
{
	struct i3c_hub *hub = smbus->priv;
	struct device *dev = i3cdev_to_dev(hub->i3cdev);
	u8 controller_buffer_page =
		I3C_HUB_CONTROLLER_BUFFER_PAGE + 4 * smbus->tp_port;
	u8 local_buffer[I3C_HUB_SMBUS_TARGET_PAYLOAD_SIZE] = { 0 };
	u8 target_buffer_page, address = 0, len = 0, flag;
	int ret;

	if (smbus->last_processed_buf)
		status &= ~smbus->last_processed_buf;

	if (status & I3C_HUB_TARGET_BUF_0_RECEIVE) {
		target_buffer_page = controller_buffer_page + 2;
		flag = I3C_HUB_TARGET_BUF_0_RECEIVE;
	} else if (status & I3C_HUB_TARGET_BUF_1_RECEIVE) {
		target_buffer_page = controller_buffer_page + 3;
		flag = I3C_HUB_TARGET_BUF_1_RECEIVE;
	} else {
		return -EINVAL;
	}

	ret = read_smbus_target_buffer_page(smbus, target_buffer_page, &address,
					    local_buffer, &len);
	if (ret && ret != -EMSGSIZE) {
		dev_dbg(dev, "Failed to read target buffer page: %d\n", ret);
		return ret;
	}

	smbus->last_processed_buf = flag;

	if (status & I3C_HUB_TARGET_BUF_OVRFL)
		flag |= I3C_HUB_TARGET_BUF_OVRFL;

	ret = regmap_write(hub->regmap, reg, flag);
	if (ret) {
		dev_dbg(dev, "Failed to clear target port status\n");
		return ret;
	}

	if (len) {
		ret = send_smbus_target_data_to_backend(smbus, address,
							local_buffer, len);
		if (ret) {
			dev_dbg(dev, "Failed to send data to backend: %d\n",
				ret);
			return ret;
		}
	}

	return 0;
}

static int i3c_hub_process_smbus_status(struct i2c_adapter_group *smbus)
{
	struct i3c_hub *hub = smbus->priv;
	u8 target_port_status = I3C_HUB_TP0_SMBUS_AGNT_STS + smbus->tp_port;
	struct device *dev = i3cdev_to_dev(hub->i3cdev);
	u32 status;
	int ret = 0;
	u32 poll_interval_ms =
		hub->settings.tp[smbus->tp_port].poll_interval_ms;

	ret = regmap_read(hub->regmap, target_port_status, &status);
	if (ret)
		return ret;

	/* smbus controller agent status */
	if (!poll_interval_ms) {
		ret = process_smbus_controller_status(smbus, target_port_status,
						      status);
		if (ret)
			dev_warn_ratelimited(dev,
					     "Failed to process smbus controller status\n");
	}

	/* smbus target agent status */
	status &= I3C_HUB_TARGET_BUF_STATUS_MASK;

	while (status) {
		ret = process_smbus_target_status(smbus, target_port_status,
						  status);
		if (ret) {
			dev_warn_ratelimited(dev,
					     "Failed to process smbus target status: %d\n",
					     ret);
			break;
		}

		if (!poll_interval_ms)
			break;

		ret = regmap_read(hub->regmap, target_port_status, &status);
		if (ret)
			break;
		status &= I3C_HUB_TARGET_BUF_STATUS_MASK;
	}

	return ret;
}

/**
 * i3c_hub_delayed_work_polling() - This delayed work is a polling mechanism to
 * find if any transaction happened.
 */
static void i3c_hub_delayed_work_polling(struct work_struct *work)
{
	struct i2c_adapter_group *smbus =
		container_of(work, typeof(*smbus), delayed_work_polling.work);
	struct device *dev = i3cdev_to_dev(smbus->priv->i3cdev);
	int ret;

	if (!list_empty(&smbus->backend_entry)) {
		ret = i3c_hub_process_smbus_status(smbus);
		if (ret)
			dev_warn_ratelimited(dev,
					     "Failed to process TP %u smbus status: %d\n",
					     smbus->tp_port, ret);
	}

	schedule_delayed_work(&smbus->delayed_work_polling,
			      msecs_to_jiffies(
				      smbus->priv->settings.tp[smbus->tp_port].poll_interval_ms));
}

static int i3c_hub_smbus_ibi_handler(struct i3c_hub *hub,
				     const struct i3c_ibi_payload *payload)
{
	struct i2c_adapter_group *smbus;
	u8 tp, tps;
	int val, ret = 0, rc;
	struct device *dev = i3cdev_to_dev(hub->i3cdev);

	if (payload->len < 2) {
		ret = regmap_read(hub->regmap, I3C_HUB_TP_SMBUS_AGNT_IBI_STS,
				  &val);
		if (ret)
			return ret;

		tps = (u8)val;
	} else {
		tps = ((const u8 *)payload->data)[1];
	}

	if (!tps)
		return 0;

	while (tps) {
		tp = (u8)__ffs((unsigned long)tps);
		tps &= (tps - 1);

		if (hub->settings.tp[tp].poll_interval_ms)
			continue;

		smbus = &hub->smbus_port_adapter[tp];
		rc = i3c_hub_process_smbus_status(smbus);
		if (rc) {
			dev_warn_ratelimited(dev,
					     "Failed to process TP %u smbus status: %d\n",
					     tp, rc);
		}
	}

	return 0;
}

/*
 * Sysfs attribute: clock_frequency
 * Read/Write the SMBus clock frequency for this adapter's port.
 */
static ssize_t clock_frequency_show(struct device *dev,
				    struct device_attribute *attr, char *buf)
{
	struct i2c_adapter *adap = to_i2c_adapter(dev);
	struct i2c_adapter_group *smbus = i2c_get_adapdata(adap);
	struct i3c_hub *hub = smbus->priv;

	return sprintf(buf, "%u\n",
		       hub->settings.tp[smbus->tp_port].clock_frequency);
}

static ssize_t clock_frequency_store(struct device *dev,
				     struct device_attribute *attr,
				     const char *buf, size_t count)
{
	struct i2c_adapter *adap = to_i2c_adapter(dev);
	struct i2c_adapter_group *smbus = i2c_get_adapdata(adap);
	struct i3c_hub *hub = smbus->priv;
	u32 val;
	int ret;

	ret = kstrtou32(buf, 0, &val);
	if (ret)
		return ret;

	if (!i3c_hub_smbus_validate_clock_frequency(val))
		return -EINVAL;

	hub->settings.tp[smbus->tp_port].clock_frequency = val;

	return count;
}
static DEVICE_ATTR_RW(clock_frequency);

static struct attribute *i3c_hub_smbus_attrs[] = {
	&dev_attr_clock_frequency.attr,
	NULL,
};

ATTRIBUTE_GROUPS(i3c_hub_smbus);

static int i3c_hub_smbus_tp_algo(struct i3c_hub *priv, int i)
{
	struct device *dev = i3cdev_to_dev(priv->i3cdev);
	int ret;
	struct i2c_adapter_group *smbus = &priv->smbus_port_adapter[i];
	struct i2c_adapter *i2c = &smbus->i2c;

	mutex_init(&smbus->mutex);
	INIT_LIST_HEAD(&smbus->backend_entry);
	smbus->priv = priv;
	smbus->tp_port = i;
	smbus->tp_mask = BIT(i);

	init_completion(&smbus->completion);
	i2c->owner = THIS_MODULE;
	i2c->algo = &i3c_hub_smbus_algo;
	i2c->dev.parent = dev;
	i2c->dev.of_node = smbus->of_node;
	i2c->dev.groups = i3c_hub_smbus_groups;
	i2c->timeout = HZ;
	i2c->retries = 3;
	snprintf(i2c->name, sizeof(i2c->name), "hub%s.port%d", dev_name(dev),
		 smbus->tp_port);

	if (priv->settings.tp[i].poll_interval_ms) {
		ret = regmap_clear_bits(priv->regmap, I3C_HUB_TP_IBI_CONF,
					smbus->tp_mask);
		if (ret)
			return ret;
		INIT_DELAYED_WORK(&smbus->delayed_work_polling,
				  i3c_hub_delayed_work_polling);
		priv->smbus_ibi_en_mask &= ~smbus->tp_mask;
	} else {
		ret = regmap_set_bits(priv->regmap, I3C_HUB_TP_IBI_CONF,
				      smbus->tp_mask);
		if (ret)
			return ret;
		priv->smbus_ibi_en_mask |= smbus->tp_mask;
	}

	/* Enable SDA hold-low when both SMBus Target Agent buffers are full.
	 * Used as a flow-control mechanism for MCTP to avoid upstream TX timeout
	 * when target buffers are not serviced in time.
	 */
	ret = regmap_set_bits(priv->regmap,
			      I3C_HUB_ONCHIP_TD_AND_SMBUS_AGNT_CONF,
			      TARGET_AGENT_BUF_FULL_SDA_LOW_EN);
	if (ret)
		return ret;

	i2c_set_adapdata(i2c, smbus);

	ret = i2c_add_adapter(i2c);
	if (ret)
		return ret;

	smbus->used = 1;
	return ret;
}

static int i3c_hub_gpio_direction_input(struct gpio_chip *gc, unsigned int off)
{
	struct i3c_hub *hub = gpiochip_get_data(gc);
	struct hub_gpio *gpio = &hub->gpio;
	int ret = 0;
	u8 reg, mask = 0;

	reg = off % GPIO_BANK_SZ ? I3C_HUB_TP_SDA_OUT_EN :
				   I3C_HUB_TP_SCL_OUT_EN;
	mask = BIT(gpio->tp[off / GPIO_BANK_SZ]);

	ret = regmap_update_bits(hub->regmap, reg, mask, 0);
	return ret;
}

static int i3c_hub_gpio_direction_output(struct gpio_chip *gc, unsigned int off,
					 int val)
{
	struct i3c_hub *hub = gpiochip_get_data(gc);
	struct hub_gpio *gpio = &hub->gpio;
	int ret = 0;
	u8 reg, mask = 0;

	reg = off % GPIO_BANK_SZ ? I3C_HUB_TP_SDA_OUT_EN :
				   I3C_HUB_TP_SCL_OUT_EN;
	mask = BIT(gpio->tp[off / GPIO_BANK_SZ]);

	ret = regmap_update_bits(hub->regmap, reg, mask, mask);
	if (ret)
		return ret;

	ret = regmap_update_bits(hub->regmap, reg + 2, mask, val ? mask : 0);
	return ret;
}

static int i3c_hub_gpio_get_value(struct gpio_chip *gc, unsigned int off)
{
	struct i3c_hub *hub = gpiochip_get_data(gc);
	struct hub_gpio *gpio = &hub->gpio;
	int ret = 0, val = 0, dir;
	u8 reg, shift = 0;

	dir = gc->get_direction(gc, off);
	if (dir)
		reg = off % GPIO_BANK_SZ ? I3C_HUB_TP_SDA_IN_LEVEL_STS :
					   I3C_HUB_TP_SCL_IN_LEVEL_STS;
	else
		reg = off % GPIO_BANK_SZ ? I3C_HUB_TP_SDA_OUT_LEVEL :
					   I3C_HUB_TP_SCL_OUT_LEVEL;

	shift = gpio->tp[off / GPIO_BANK_SZ];

	ret = regmap_read(hub->regmap, reg, &val);
	if (ret)
		return ret;

	ret = (val >> shift) & 0x01;
	return ret;
}

static int i3c_hub_gpio_set_value(struct gpio_chip *gc, unsigned int off, int val)
{
	struct i3c_hub *hub = gpiochip_get_data(gc);
	struct hub_gpio *gpio = &hub->gpio;
	u8 reg, mask = 0;

	reg = off % GPIO_BANK_SZ ? I3C_HUB_TP_SDA_OUT_LEVEL :
				   I3C_HUB_TP_SCL_OUT_LEVEL;
	mask = BIT(gpio->tp[off / GPIO_BANK_SZ]);

	return regmap_update_bits(hub->regmap, reg, mask, val ? mask : 0);
}

static int i3c_hub_gpio_get_direction(struct gpio_chip *gc, unsigned int off)
{
	struct i3c_hub *hub = gpiochip_get_data(gc);
	struct hub_gpio *gpio = &hub->gpio;
	int ret = 0, dir = 0;
	u8 reg, shift = 0;

	reg = off % GPIO_BANK_SZ ? I3C_HUB_TP_SDA_OUT_EN :
				   I3C_HUB_TP_SCL_OUT_EN;
	shift = gpio->tp[off / GPIO_BANK_SZ];

	ret = regmap_read(hub->regmap, reg, &dir);
	if (ret)
		return ret;

	ret = ~(dir >> shift) & 0x01;
	return ret;
}

static void i3c_hub_gpio_irq_mask(struct irq_data *d)
{
	struct gpio_chip *gc = irq_data_get_irq_chip_data(d);
	struct i3c_hub *hub = gpiochip_get_data(gc);
	struct hub_gpio *gpio = &hub->gpio;
	u8 reg, hwirq = 0, mask = 0;

	hwirq = irqd_to_hwirq(d);

	reg = hwirq % GPIO_BANK_SZ ? I3C_HUB_TP_SDA_IN_DETECT_IBI_EN :
				     I3C_HUB_TP_SCL_IN_DETECT_IBI_EN;
	mask = BIT(gpio->tp[hwirq / GPIO_BANK_SZ]);

	regmap_update_bits(hub->regmap, reg, mask, 0);
}

static void i3c_hub_gpio_irq_unmask(struct irq_data *d)
{
	struct gpio_chip *gc = irq_data_get_irq_chip_data(d);
	struct i3c_hub *hub = gpiochip_get_data(gc);
	struct hub_gpio *gpio = &hub->gpio;
	u8 reg, hwirq = 0, mask = 0;

	hwirq = irqd_to_hwirq(d);

	reg = hwirq % GPIO_BANK_SZ ? I3C_HUB_TP_SDA_IN_DETECT_IBI_EN :
				     I3C_HUB_TP_SCL_IN_DETECT_IBI_EN;
	mask = BIT(gpio->tp[hwirq / GPIO_BANK_SZ]);

	regmap_update_bits(hub->regmap, reg, mask, mask);
}

static int i3c_hub_gpio_irq_set_type(struct irq_data *d, unsigned int flow_type)
{
	struct gpio_chip *gc = irq_data_get_irq_chip_data(d);
	struct i3c_hub *hub = gpiochip_get_data(gc);
	struct hub_gpio *gpio = &hub->gpio;
	u8 hwirq = 0, mask = 0, val, tp, reg;
	int ret;

	if (!(flow_type & IRQ_TYPE_EDGE_BOTH)) {
		dev_err(&hub->i3cdev->dev, "irq %d: unsupported type %d\n",
			d->irq, flow_type);
		return -EINVAL;
	}

	hwirq = irqd_to_hwirq(d);
	tp = gpio->tp[hwirq / GPIO_BANK_SZ];

	if (tp == 0 || tp == 1 || tp == 4 || tp == 5) {
		if (hwirq % GPIO_BANK_SZ) {
			mask = SDA0145_IO_IN_DET_CFG_MASK;
			val = SDA0145_IO_IN_DET_CFG(flow_type);
			reg = I3C_HUB_TP_SDA_IN_DETECT_FLG;
		} else {
			mask = SCL0145_IO_IN_DET_CFG_MASK;
			val = SCL0145_IO_IN_DET_CFG(flow_type);
			reg = I3C_HUB_TP_SCL_IN_DETECT_FLG;
		}
	} else {
		if (hwirq % GPIO_BANK_SZ) {
			mask = SDA2367_IO_IN_DET_CFG_MASK;
			val = SDA2367_IO_IN_DET_CFG(flow_type);
			reg = I3C_HUB_TP_SDA_IN_DETECT_FLG;
		} else {
			mask = SCL2367_IO_IN_DET_CFG_MASK;
			val = SCL2367_IO_IN_DET_CFG(flow_type);
			reg = I3C_HUB_TP_SCL_IN_DETECT_FLG;
		}
	}

	ret = regmap_write(hub->regmap, reg, BIT(tp));
	if (ret)
		return ret;

	ret = regmap_update_bits(hub->regmap, I3C_HUB_TP_IN_DETECT_MODE_CONF,
				 mask, val);
	return ret;
}

static void i3c_hub_gpio_irq_bus_lock(struct irq_data *d)
{
	struct gpio_chip *gc = irq_data_get_irq_chip_data(d);
	struct i3c_hub *hub = gpiochip_get_data(gc);
	struct hub_gpio *gpio = &hub->gpio;

	mutex_lock(&gpio->irq_mutex);
}

static void i3c_hub_gpio_irq_bus_unlock(struct irq_data *d)
{
	struct gpio_chip *gc = irq_data_get_irq_chip_data(d);
	struct i3c_hub *hub = gpiochip_get_data(gc);
	struct hub_gpio *gpio = &hub->gpio;

	mutex_unlock(&gpio->irq_mutex);
}

static void i3c_hub_setup_gpio(struct i3c_hub *hub)
{
	struct hub_gpio *gpio = &hub->gpio;
	struct gpio_chip *gc = &gpio->chip;
	struct gpio_irq_chip *girq;

	gc->direction_input = i3c_hub_gpio_direction_input;
	gc->direction_output = i3c_hub_gpio_direction_output;
	gc->get = i3c_hub_gpio_get_value;
	gc->set = i3c_hub_gpio_set_value;
	gc->get_direction = i3c_hub_gpio_get_direction;
	gc->can_sleep = true;

	gc->base = -1;
	gc->ngpio = gpio->nums;
	gc->label = dev_name(&hub->i3cdev->dev);
	gc->parent = &hub->i3cdev->dev;
	gc->owner = THIS_MODULE;

	/* irq_chip support */
	mutex_init(&gpio->irq_mutex);

	gpio->irq_chip.name = dev_name(&hub->i3cdev->dev);
	gpio->irq_chip.irq_mask = i3c_hub_gpio_irq_mask;
	gpio->irq_chip.irq_unmask = i3c_hub_gpio_irq_unmask;
	gpio->irq_chip.irq_set_type = i3c_hub_gpio_irq_set_type;
	gpio->irq_chip.irq_bus_lock = i3c_hub_gpio_irq_bus_lock;
	gpio->irq_chip.irq_bus_sync_unlock = i3c_hub_gpio_irq_bus_unlock;

	girq = &gpio->chip.irq;

	/* This will let us handle the parent IRQ in the driver */
	girq->parent_handler = NULL;
	girq->num_parents = 0;
	girq->parents = NULL;
	girq->default_type = IRQ_TYPE_NONE;
	girq->handler = handle_simple_irq;
	girq->threaded = true;
	girq->first = 0;

	girq->chip = &gpio->irq_chip;
}

static void i3c_hub_io_ibi_handler(struct i3c_hub *hub,
				   const struct i3c_ibi_payload *payload)
{
	struct hub_gpio *gpio = &hub->gpio;
	struct gpio_chip *gc = &gpio->chip;
	u8 level, hwirq, tmp;
	u8 pending[GPIO_BANK_SZ];
	u8 tp[GPIO_BANK_SZ];
	int i, irq, ret, index;

	ret = regmap_bulk_read(hub->regmap, I3C_HUB_TP_SCL_OUT_EN, tp,
			       GPIO_BANK_SZ);
	if (ret) {
		dev_err(&hub->i3cdev->dev, "Failed to read OUT_EN: %d\n", ret);
		return;
	}

	ret = regmap_bulk_read(hub->regmap, I3C_HUB_TP_SCL_IN_DETECT_FLG,
			       pending, GPIO_BANK_SZ);
	if (ret) {
		dev_err(&hub->i3cdev->dev, "Failed to read DETECT_FLG: %d\n",
			ret);
		return;
	}

	for (i = 0; i < GPIO_BANK_SZ; i++) {
		tmp = ~tp[i] & pending[i];

		while (tmp) {
			level = __ffs(tmp);
			tmp &= ~(1 << level);

			/* Check if this port is in GPIO mode */
			index = gpio->port_to_index[level];
			if (index < 0) {
				/* Non-GPIO mode port, skip without clearing.
				 * This can happen because IN_DETECT IBI enable is
				 * configured in groups (e.g., TP0145/TP2367), not
				 * per individual port. Simply skip - the flag is
				 * harmless and will be overwritten by next detection.
				 */
				dev_dbg(&hub->i3cdev->dev,
					"IBI detect flag on non-GPIO port %d, skipping\n",
					level);
				continue;
			}

			hwirq = index * 2 + i;
			irq = irq_find_mapping(gc->irq.domain, hwirq);

			/* Clear the flag after processing */
			regmap_write(hub->regmap,
				     I3C_HUB_TP_SCL_IN_DETECT_FLG + i,
				     BIT(level));

			if (unlikely(irq <= 0)) {
				dev_warn_ratelimited(gc->parent,
						     "unmapped interrupt %d\n",
						     hwirq);
			} else {
				handle_nested_irq(irq);
			}
		}
	}
}

static void i3c_hub_ibi_handler(struct i3c_device *dev,
				const struct i3c_ibi_payload *payload)
{
	struct i3c_hub *priv = i3cdev_get_drvdata(dev);
	int ret, val = 0;
	u8 status = 0;

	if (!payload->len) {
		dev_dbg(&dev->dev,
			"Zero-length IBI payload, reading status register\n");
		ret = regmap_read(priv->regmap, I3C_HUB_DEV_AND_IBI_STS, &val);
		if (ret) {
			dev_warn_ratelimited(&dev->dev,
					     "Failed to read IBI status: %d\n",
					     ret);
			return;
		}
		status = (u8)val;
	} else {
		if (!payload->data) {
			dev_warn_ratelimited(&dev->dev,
					     "IBI payload data is NULL with len=%d\n",
					     payload->len);
			return;
		}
		status = ((const u8 *)payload->data)[0];
	}

	if (status & TP_IO_FLAG_STATUS)
		i3c_hub_io_ibi_handler(priv, payload);

	if (status & SMBUS_AGENT_EVENT_FLAG_STATUS) {
		ret = i3c_hub_smbus_ibi_handler(priv, payload);
		if (ret) {
			dev_warn_ratelimited(&dev->dev,
					     "Failed to handle SMBus IBI: %d\n",
					     ret);
			return;
		}
	}
}

static inline void i3c_hub_regmap_lock(void *ctx)
{
	mutex_lock(&i3c_hub_regmap_mutex);
}

static inline void i3c_hub_regmap_unlock(void *ctx)
{
	mutex_unlock(&i3c_hub_regmap_mutex);
}

static int i3c_hub_probe(struct i3c_device *i3cdev)
{
	const struct regmap_config i3c_hub_regmap_config = {
		.reg_bits = 8,
		.val_bits = 8,
		.lock = i3c_hub_regmap_lock,
		.unlock = i3c_hub_regmap_unlock,
		.lock_arg = NULL,
	};
	struct device *dev = &i3cdev->dev;
	struct device_node *node = NULL;
	struct regmap *regmap;
	struct i3c_hub *priv;
	char hub_id[32];
	int ret;
	int i;
	struct i3c_ibi_setup ibireq = {};

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->i3cdev = i3cdev;
	priv->driving_master = i3c_dev_get_master(i3cdev->desc);
	mutex_init(&priv->page_mutex);
	i3cdev_set_drvdata(i3cdev, priv);
	INIT_DELAYED_WORK(&priv->delayed_work, i3c_hub_delayed_work);
	i3c_hub_of_default_configuration(dev);

	regmap = devm_regmap_init_i3c(i3cdev, &i3c_hub_regmap_config);
	if (IS_ERR(regmap)) {
		ret = PTR_ERR(regmap);
		dev_err(dev, "Failed to register I3C HUB regmap\n");
		return ret;
	}
	priv->regmap = regmap;

	priv->dev_info = i3c_hub_lookup_dev_info(priv);
	if (IS_ERR(priv->dev_info)) {
		ret = PTR_ERR(priv->dev_info);
		dev_err(dev, "Failed to lookup HUB dev info\n");
		return ret;
	}

	sprintf(hub_id, "i3c-hub-%d-%llx",
		i3c_dev_get_master(i3cdev->desc)->bus.id,
		i3cdev->desc->info.pid);
	ret = i3c_hub_debugfs_init(priv, hub_id);
	if (ret)
		dev_dbg(dev, "Failed to initialized DebugFS.\n");

	ret = i3c_hub_read_id(dev);
	if (ret)
		goto error;

	priv->hub_dt_sel_id = -1;
	priv->hub_dt_cp1_id = -1;
	if (priv->hub_pin_cp1_id >= 0 && priv->hub_pin_sel_id >= 0)
		/* Find hub node in DT matching HW ID or just first without ID provided in DT */
		node = i3c_hub_get_dt_hub_node(dev->parent->of_node, priv);

	if (!node) {
		dev_info(dev,
			 "No DT entry - running with hardware defaults.\n");
	} else {
		of_node_get(node);
		i3c_hub_of_get_conf_static(dev, node);
		i3c_hub_of_get_conf_runtime(dev, node);
		of_node_put(node);
	}

	/* Unlock access to protected registers */
	ret = regmap_write(priv->regmap, I3C_HUB_PROTECTION_CODE,
			   REGISTERS_UNLOCK_CODE);
	if (ret) {
		dev_err(dev, "Failed to unlock HUB's protected registers\n");
		goto error;
	}

	/* Register logic for native smbus ports */
	for (i = 0; i < priv->dev_info->n_ports; i++) {
		priv->smbus_port_adapter[i].used = 0;
		if (priv->settings.tp[i].mode == I3C_HUB_DT_TP_MODE_SMBUS) {
			ret = i3c_hub_smbus_tp_algo(priv, i);
			if (ret)
				dev_warn(dev,
					 "Failed to setup SMBus adapter, port: %d\n",
					 i);
		}
	}

	ret = i3c_hub_configure_hw(dev);
	if (ret) {
		dev_err(dev, "Failed to configure the HUB\n");
		goto error;
	}

	/* Lock access to protected registers */
	ret = regmap_write(priv->regmap, I3C_HUB_PROTECTION_CODE,
			   REGISTERS_LOCK_CODE);
	if (ret) {
		dev_err(dev, "Failed to lock HUB's protected registers\n");
		goto error;
	}

	/* IBI */
	ibireq.handler = i3c_hub_ibi_handler;
	ibireq.max_payload_len = IBI_MAX_PAYLOAD_LEN;
	ibireq.num_slots = IBI_SLOT_NUMS;

	ret = i3c_device_request_ibi(i3cdev, &ibireq);
	if (ret) {
		dev_err(dev, "Failed to requeset ibi!\n");
		goto error;
	}

	ret = i3c_device_enable_ibi(i3cdev);
	if (ret) {
		dev_err(dev, "Failed to enable ibi!\n");
		goto err_free_ibi;
	}

	/* TBD: Apply special/security lock here using DEV_CMD register */

	if (priv->gpio.nums > 0) {
		i3c_hub_setup_gpio(priv);

		ret = devm_gpiochip_add_data(dev, &priv->gpio.chip, priv);
		if (ret) {
			dev_err(dev, "gpiochip add data fail!\n");
			goto err_dis_ibi;
		}
	}

	schedule_delayed_work(&priv->delayed_work, msecs_to_jiffies(100));

	return 0;

err_dis_ibi:
	i3c_device_disable_ibi(i3cdev);
err_free_ibi:
	i3c_device_free_ibi(i3cdev);
error:
	debugfs_remove_recursive(priv->debug_dir);
	return ret;
}

static void i3c_hub_remove(struct i3c_device *i3cdev)
{
	struct i3c_hub *priv = i3cdev_get_drvdata(i3cdev);
	int i;

	i3c_device_disable_ibi(i3cdev);
	i3c_device_free_ibi(i3cdev);

	for (i = 0; i < priv->dev_info->n_ports; i++) {
		if (priv->smbus_port_adapter[i].used) {
			cancel_delayed_work_sync(&priv->smbus_port_adapter[i].delayed_work_polling);
			i2c_del_adapter(&priv->smbus_port_adapter[i].i2c);
		}

		if (priv->logical_bus[i].registered)
			i3c_master_unregister(&priv->logical_bus[i].controller);
	}

	cancel_delayed_work_sync(&priv->delayed_work);
	debugfs_remove_recursive(priv->debug_dir);
}

static struct i3c_driver i3c_hub = {
	.driver.name = "rts490xa-i3c-hub",
	.id_table = i3c_hub_ids,
	.probe = i3c_hub_probe,
	.remove = i3c_hub_remove,
};

module_i3c_driver(i3c_hub);

MODULE_DESCRIPTION("RTS490XA I3C HUB driver");
MODULE_LICENSE("GPL");

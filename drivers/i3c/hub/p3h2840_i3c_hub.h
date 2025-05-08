/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright 2025 NXP
 * This header file contain private device structure definition, Reg address and its bit
 * mapping etc.
 */

#ifndef P3H2840_I3C_HUB_H
#define P3H2840_I3C_HUB_H

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/regmap.h>
#include <linux/i3c/device.h>
#include <linux/i3c/master.h>
#include <linux/mutex.h>
#include <linux/rwsem.h>
#include <linux/i2c.h>
#include <linux/slab.h>
#include <linux/of.h>
#include <linux/of_address.h>

/* I3C HUB REGISTERS */

/* Device Information Registers */
#define P3H2x4x_DEV_INFO_0					0x00
#define P3H2x4x_DEV_INFO_1					0x01
#define P3H2x4x_PID_5						0x02
#define P3H2x4x_PID_4						0x03
#define P3H2x4x_PID_3						0x04
#define P3H2x4x_PID_2						0x05
#define P3H2x4x_PID_1						0x06
#define P3H2x4x_PID_0						0x07
#define P3H2x4x_BCR						0x08
#define P3H2x4x_DCR						0x09
#define P3H2x4x_DEV_CAPAB					0x0a
#define P3H2x4x_DEV_REV						0x0b

/* Device Configuration Registers */
#define P3H2x4x_DEV_REG_PROTECTION_CODE				0x10
#define P3H2x4x_REGISTERS_LOCK_CODE				0x00
#define P3H2x4x_REGISTERS_UNLOCK_CODE				0x69
#define P3H2x4x_CP1_REGISTERS_UNLOCK_CODE			0x6a

#define P3H2x4x_CP_CONF						0x11
#define P3H2x4x_TP_ENABLE					0x12
#define P3H2x4x_TPn_ENABLE(n)					BIT(n)

#define P3H2x4x_DEV_CONF					0x13
#define P3H2x4x_IO_STRENGTH					0x14
#define P3H2x4x_TP0145_IO_STRENGTH_MASK				GENMASK(1, 0)
#define P3H2x4x_TP0145_IO_STRENGTH(x)	\
		FIELD_PREP(P3H2x4x_TP0145_IO_STRENGTH_MASK, x)
#define P3H2x4x_TP2367_IO_STRENGTH_MASK				GENMASK(3, 2)
#define P3H2x4x_TP2367_IO_STRENGTH(x)	\
		FIELD_PREP(P3H2x4x_TP2367_IO_STRENGTH_MASK, x)
#define P3H2x4x_CP0_IO_STRENGTH_MASK				GENMASK(5, 4)
#define P3H2x4x_CP0_IO_STRENGTH(x)	\
		FIELD_PREP(P3H2x4x_CP0_IO_STRENGTH_MASK, x)
#define P3H2x4x_CP1_IO_STRENGTH_MASK				GENMASK(7, 6)
#define P3H2x4x_CP1_IO_STRENGTH(x)	\
		FIELD_PREP(P3H2x4x_CP1_IO_STRENGTH_MASK, x)

#define P3H2x4x_NET_OPER_MODE_CONF				0x15
#define P3H2x4x_VCCIO_LDO_CONF					0x16
#define P3H2x4x_CP0_VCCIO_LDO_VOLTAGE_MASK			GENMASK(1, 0)
#define P3H2x4x_CP0_VCCIO_LDO_VOLTAGE(x)	\
		FIELD_PREP(P3H2x4x_CP0_VCCIO_LDO_VOLTAGE_MASK, x)
#define P3H2x4x_CP1_VCCIO_LDO_VOLTAGE_MASK			GENMASK(3, 2)
#define P3H2x4x_CP1_VCCIO_LDO_VOLTAGE(x)	\
		FIELD_PREP(P3H2x4x_CP1_VCCIO_LDO_VOLTAGE_MASK, x)
#define P3H2x4x_TP0145_VCCIO_LDO_VOLTAGE_MASK			GENMASK(5, 4)
#define P3H2x4x_TP0145_VCCIO_LDO_VOLTAGE(x)	\
		FIELD_PREP(P3H2x4x_TP0145_VCCIO_LDO_VOLTAGE_MASK, x)
#define P3H2x4x_TP2367_VCCIO_LDO_VOLTAGE_MASK			GENMASK(7, 6)
#define P3H2x4x_TP2367_VCCIO_LDO_VOLTAGE(x)	\
		FIELD_PREP(P3H2x4x_TP2367_VCCIO_LDO_VOLTAGE_MASK, x)

#define P3H2x4x_TP_IO_MODE_CONF					0x17
#define P3H2x4x_TP_SMBUS_AGNT_EN				0x18
#define P3H2x4x_TPn_SMBUS_MODE_EN(n)				BIT(n)
#define P3H2x4x_TPn_I2C_MODE_EN(n)				BIT(n)

#define P3H2x4x_LDO_AND_PULLUP_CONF				0x19
#define P3H2x4x_LDO_ENABLE_DISABLE_MASK				GENMASK(3, 0)
#define P3H2x4x_CP0_EN_LDO				        BIT(0)
#define P3H2x4x_CP1_EN_LDO				        BIT(1)
#define P3H2x4x_TP0145_EN_LDO					BIT(2)
#define P3H2x4x_TP2367_EN_LDO					BIT(3)

#define P3H2x4x_TP0145_PULLUP_CONF_MASK				GENMASK(7, 6)
#define P3H2x4x_TP0145_PULLUP_CONF(x)	\
		FIELD_PREP(P3H2x4x_TP0145_PULLUP_CONF_MASK, x)
#define P3H2x4x_TP2367_PULLUP_CONF_MASK				GENMASK(5, 4)
#define P3H2x4x_TP2367_PULLUP_CONF(x)	\
		FIELD_PREP(P3H2x4x_TP2367_PULLUP_CONF_MASK, x)

#define P3H2x4x_CP_IBI_CONF					0x1a

#define P3H2x4x_TP_SMBUS_AGNT_IBI_CONFIG			0x1b
#define P3H2x4x_TPn_IBI_EN(n)					BIT(n)
#define P3H2x4x_ALL_TP_IBI_EN					0xff
#define P3H2x4x_ALL_TP_IBI_DIS					0x00

#define P3H2x4x_IBI_MDB_CUSTOM					0x1c
#define P3H2x4x_JEDEC_CONTEXT_ID				0x1d
#define P3H2x4x_TP_GPIO_MODE_EN					0x1e
#define P3H2x4x_TPn_GPIO_MODE_EN(n)				BIT(n)

/* Device Status and IBI Registers */
#define P3H2x4x_DEV_AND_IBI_STS					0x20
#define P3H2x4x_TP_SMBUS_AGNT_IBI_STS				0x21
#define P3H2x4x_SMBUS_AGENT_EVENT_FLAG_STATUS			BIT(4)

/* Controller Port Control/Status Registers */
#define P3H2x4x_CP_MUX_SET					0x38
#define P3H2x4x_CONTROLLER_PORT_MUX_REQ				BIT(0)
#define P3H2x4x_CP_MUX_STS					0x39
#define P3H2x4x_CONTROLLER_PORT_MUX_CONNECTION_STATUS		BIT(0)

/* Target Ports Control Registers */
#define P3H2x4x_TP_SMBUS_AGNT_TRANS_START			0x50
#define P3H2x4x_TP_NET_CON_CONF					0x51
#define P3H2x4x_TPn_NET_CON(n)					BIT(n)

#define P3H2x4x_TP_PULLUP_EN					0x53
#define P3H2x4x_TPn_PULLUP_EN(n)				BIT(n)

#define P3H2x4x_TP_SCL_OUT_EN					0x54
#define P3H2x4x_TP_SDA_OUT_EN					0x55
#define P3H2x4x_TP_SCL_OUT_LEVEL				0x56
#define P3H2x4x_TP_SDA_OUT_LEVEL				0x57
#define P3H2x4x_TP_IN_DETECT_MODE_CONF				0x58
#define P3H2x4x_TP_SCL_IN_DETECT_IBI_EN				0x59
#define P3H2x4x_TP_SDA_IN_DETECT_IBI_EN				0x5a

/* Target Ports Status Registers */
#define P3H2x4x_TP_SCL_IN_LEVEL_STS				0x60
#define P3H2x4x_TP_SDA_IN_LEVEL_STS				0x61
#define P3H2x4x_TP_SCL_IN_DETECT_FLG				0x62
#define P3H2x4x_TP_SDA_IN_DETECT_FLG				0x63

/* SMBus Agent Configuration and Status Registers */
#define P3H2x4x_TP0_SMBUS_AGNT_STS				0x64
#define P3H2x4x_TP1_SMBUS_AGNT_STS				0x65
#define P3H2x4x_TP2_SMBUS_AGNT_STS				0x66
#define P3H2x4x_TP3_SMBUS_AGNT_STS				0x67
#define P3H2x4x_TP4_SMBUS_AGNT_STS				0x68
#define P3H2x4x_TP5_SMBUS_AGNT_STS				0x69
#define P3H2x4x_TP6_SMBUS_AGNT_STS				0x6a
#define P3H2x4x_TP7_SMBUS_AGNT_STS				0x6b
#define P3H2x4x_ONCHIP_TD_AND_SMBUS_AGNT_CONF			0x6c

/* buf receive flag set */
#define P3H2x4x_TARGET_BUF_CA_TF				BIT(0)
#define P3H2x4x_TARGET_BUF_0_RECEIVE				BIT(1)
#define P3H2x4x_TARGET_BUF_1_RECEIVE				BIT(2)
#define P3H2x4x_TARGET_BUF_0_1_RECEIVE				GENMASK(2, 1)
#define P3H2x4x_TARGET_BUF_OVRFL				GENMASK(3, 1)
#define BUF_RECEIVED_FLAG_MASK					GENMASK(3, 1)
#define BUF_RECEIVED_FLAG_TF_MASK				GENMASK(3, 0)

#define P3H2x4x_TARGET_AGENT_LOCAL_DEV				0x11
#define P3H2x4x_TARGET_BUFF_0_PAGE				0x12
#define P3H2x4x_TARGET_BUFF_1_PAGE				0x13

/* Special Function Registers */
#define P3H2x4x_LDO_AND_CPSEL_STS				0x79
#define P3H2x4x_CP_SDA1_LEVEL					BIT(7)
#define P3H2x4x_CP_SCL1_LEVEL					BIT(6)

#define P3H2x4x_CP_SEL_PIN_INPUT_CODE_MASK			GENMASK(5, 4)
#define P3H2x4x_CP_SEL_PIN_INPUT_CODE_GET(x)	\
		(((x) & P3H2x4x_CP_SEL_PIN_INPUT_CODE_MASK) >> 4)
#define P3H2x4x_CP_SDA1_SCL1_PINS_CODE_MASK			GENMASK(7, 6)
#define P3H2x4x_CP_SDA1_SCL1_PINS_CODE_GET(x)	\
		(((x) & P3H2x4x_CP_SDA1_SCL1_PINS_CODE_MASK) >> 6)
#define P3H2x4x_VCCIO1_PWR_GOOD					BIT(3)
#define P3H2x4x_VCCIO0_PWR_GOOD					BIT(2)
#define P3H2x4x_CP1_VCCIO_PWR_GOOD				BIT(1)
#define P3H2x4x_CP0_VCCIO_PWR_GOOD				BIT(0)

#define P3H2x4x_BUS_RESET_SCL_TIMEOUT				0x7a
#define P3H2x4x_ONCHIP_TD_PROTO_ERR_FLG				0x7b
#define P3H2x4x_DEV_CMD						0x7c
#define P3H2x4x_ONCHIP_TD_STS					0x7d
#define P3H2x4x_ONCHIP_TD_ADDR_CONF				0x7e
#define P3H2x4x_PAGE_PTR					0x7f

/* Paged Transaction Registers */
#define P3H2x4x_CONTROLLER_BUFFER_PAGE				0x10
#define P3H2x4x_CONTROLLER_AGENT_BUFF				0x80
#define P3H2x4x_CONTROLLER_AGENT_BUFF_DATA			0x84

#define P3H2x4x_TARGET_BUFF_LENGTH				0x80
#define P3H2x4x_TARGET_BUFF_ADDRESS				0x81
#define P3H2x4x_TARGET_BUFF_DATA				0x82

#define P3H2x4x_TP_MAX_COUNT					0x08
#define P3H2x4x_CP_MAX_COUNT					0x02
#define P3H2x4x_TP_LOCAL_DEV					0x08

/* LDO Disable/Enable DT settings */
#define P3H2x4x_LDO_VOLT_1_0V					0x00
#define P3H2x4x_LDO_VOLT_1_1V					0x01
#define P3H2x4x_LDO_VOLT_1_2V					0x02
#define P3H2x4x_LDO_VOLT_1_8V					0x03
#define P3H2x4x_DT_LDO_VOLT_NOT_SET				0x04

#define P3H2x4x_LDO_DISABLED					0x00
#define P3H2x4x_LDO_ENABLED					0x01

#define P3H2x4x_IBI_DISABLED					0x00
#define P3H2x4x_IBI_ENABLED					0x01

/* Pullup selection DT settings */
#define P3H2x4x_TP_PULLUP_250R					0x00
#define P3H2x4x_TP_PULLUP_500R					0x01
#define P3H2x4x_TP_PULLUP_1000R					0x02
#define P3H2x4x_TP_PULLUP_2000R					0x03
#define P3H2x4x_TP_PULLUP_NOT_SET				0x04

#define P3H2x4x_TP_PULLUP_DISABLED				0x00
#define P3H2x4x_TP_PULLUP_ENABLED				0x01

#define P3H2x4x_IO_STRENGTH_20_OHM				0x00
#define P3H2x4x_IO_STRENGTH_30_OHM				0x01
#define P3H2x4x_IO_STRENGTH_40_OHM				0x02
#define P3H2x4x_IO_STRENGTH_50_OHM				0x03
#define P3H2x4x_IO_STRENGTH_NOT_SET				0x04

#define P3H2x4x_TP_MODE_I3C					0x00
#define P3H2x4x_TP_MODE_SMBUS					0x01
#define P3H2x4x_TP_MODE_GPIO					0x02
#define P3H2x4x_TP_MODE_I2C					0x03
#define P3H2x4x_TP_MODE_NOT_SET					0x04

#define ONE_BYTE_SIZE						0x01

/* holding SDA low when both SMBus Target Agent received data buffers are full.
 * This feature can be used as a flow-control mechanism for MCTP applications to
 * avoid MCTP transmitters on Target Ports time out when the SMBus agent buffers
 * are not serviced in time by upstream controller and only receives write message
 * from its downstream ports.
 * SMBUS_AGENT_TX_RX_LOOPBACK_EN/TARGET_AGENT_BUF_FULL_SDA_LOW_EN
 */

#define P3H2x4x_TARGET_AGENT_DFT_IBI_CONF			0x20
#define P3H2x4x_TARGET_AGENT_DFT_IBI_CONF_MASK			0x21

/* Transaction status checking mask */
#define P3H2x4x_XFER_SUCCESS					0x01
#define P3H2x4x_TP_BUFFER_STATUS_MASK				0x0f
#define P3H2x4x_TP_TRANSACTION_CODE_MASK			0xf0

/* SMBus transaction types fields */
#define P3H2x4x_SMBUS_400kHz					BIT(2)

/* SMBus polling */
#define P3H2x4x_POLLING_ROLL_PERIOD_MS				10

/* Hub buffer size */
#define P3H2x4x_CONTROLLER_BUFFER_SIZE				88
#define P3H2x4x_TARGET_BUFFER_SIZE				80
#define P3H2x4x_SMBUS_DESCRIPTOR_SIZE				4
#define P3H2x4x_SMBUS_PAYLOAD_SIZE	\
		(P3H2x4x_CONTROLLER_BUFFER_SIZE - P3H2x4x_SMBUS_DESCRIPTOR_SIZE)
#define P3H2x4x_SMBUS_TARGET_PAYLOAD_SIZE	(P3H2x4x_TARGET_BUFFER_SIZE - 2)

/* Hub SMBus transaction time */
#define P3H2x4x_SMBUS_400kHz_TRANSFER_TIMEOUT(x)		((20 * (x)) + 80)

#define P3H2x4x_NO_PAGE_PER_TP					4

#define P3H2x4x_MAX_PAYLOAD_LEN					2
#define P3H2x4x_NUM_SLOTS					10

#define P3H2x4x_HUB_ID						0

/* Reg config for Regmap */
#define P3H2x4x_REG_BITS					8
#define P3H2x4x_VAL_BITS					8

enum p3h2x4x_tp {
	TP_0,
	TP_1,
	TP_2,
	TP_3,
	TP_4,
	TP_5,
	TP_6,
	TP_7,
};

enum p3h2x4x_rcv_buf {
	RCV_BUF_0,
	RCV_BUF_1,
	RCV_BUF_OF,
};

struct p3h2x4x_setting {
	const char *const name;
	const u8 value;
};

struct tp_setting {
	int mode;
	bool pullup_en;
	bool ibi_en;
	bool always_enable;
};

/*
 * struct svc_i3c_i2c_dev_data - Device specific data
 * @index: Index in the master tables corresponding to this device
 * @ibi: IBI slot index in the master structure
 * @ibi_pool: IBI pool associated to this device
 */
struct dt_settings {
	bool cp0_ldo_en;
	bool cp1_ldo_en;
	bool tp0145_ldo_en;
	bool tp2367_ldo_en;
	int cp0_ldo_volt;
	int cp1_ldo_volt;
	int tp0145_ldo_volt;
	int tp2367_ldo_volt;
	int tp0145_pullup;
	int tp2367_pullup;
	int cp0_io_strength;
	int cp1_io_strength;
	int tp0145_io_strength;
	int tp2367_io_strength;
	struct tp_setting tp[P3H2x4x_TP_MAX_COUNT];
};

struct smbus_device {
	struct i2c_client *client;
	const char *compatible;
	int addr;
	struct list_head list;
	struct device_node *tp_device_dt_node;
};

struct tp_bus {
	bool dt_available;      /* bus configuration is available in DT. */
	bool is_registered;	    /* bus was registered in the framework. */
	u8 tp_mask;
	u8 tp_port;
	u8 local_dev_list[P3H2x4x_TP_LOCAL_DEV];
	u8 local_dev_count;
	struct i3c_master_controller i3c_port_controller;
	struct i2c_adapter smbus_port_adapter;
	struct device_node *of_node;
	struct p3h2x4x *priv;
	struct list_head tp_device_entry;
	struct mutex port_mutex;      /* per port mutex */
};

struct p3h2x4x {
	struct i3c_device *i3cdev;
	struct i2c_client *i2cdev;
	struct i3c_master_controller *driving_master;
	struct regmap *regmap;
	struct mutex etx_mutex;      /* all port mutex */
	struct dt_settings settings;
	struct tp_bus tp_bus[P3H2x4x_TP_MAX_COUNT];
	/* Offset for reading HUB's register. */
	u8 tp_ibi_mask;
	u8 tp_i3c_bus_mask;
	u8 tp_always_enable_mask;
	u8 is_slave_reg;
	bool is_p3h2x4x_in_i3c;
};

/*
 * p3h2x4x_tp_add_downstream_device - prove downstream devices for target port who
 * configured as SMBus.
 * @priv: p3h2x4x device structure.
 * Return: 0 in case of success, a negative EINVAL code if the error.
 */
int p3h2x4x_tp_add_downstream_device(struct p3h2x4x *priv);

/*
 * p3h2x4x_tp_smbus_algo - add i2c adapter for target port who
 * configured as SMBus.
 * @priv: p3h2x4x device structure.
 * @tp: target port.
 * Return: 0 in case of success, a negative EINVAL code if the error.
 */
int p3h2x4x_tp_smbus_algo(struct p3h2x4x *priv, int tp);

/*
 * p3h2x4x_tp_i3c_algo - register i3c master for target port who
 * configured as I3C.
 * @priv: p3h2x4x device structure.
 * @tp: target port.
 * Return: 0 in case of success, a negative EINVAL code if the error.
 */
int p3h2x4x_tp_i3c_algo(struct p3h2x4x *priv, int tp);

/*
 * p3h2x4x_ibi_handler - IBI handler.
 * @i3cdev: i3c device.
 * @payload: two byte IBI payload data.
 */
void p3h2x4x_ibi_handler(struct i3c_device *i3cdev,
			 const struct i3c_ibi_payload *payload);
#endif /* P3H2840_I3C_HUB_H */

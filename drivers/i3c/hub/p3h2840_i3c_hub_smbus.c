// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright 2025-2026 NXP
 * This P3H2X4X driver file contain functions for SMBus/I2C virtual Bus creation and read/write.
 */
#include <linux/mfd/p3h2840.h>
#include <linux/regmap.h>

#include "p3h2840_i3c_hub.h"

enum p3h2x4x_smbus_desc_idx {
	P3H2X4X_DESC_ADDR,
	P3H2X4X_DESC_TYPE,
	P3H2X4X_DESC_WRITE_LEN,
	P3H2X4X_DESC_READ_LEN,
};

static int p3h2x4x_read_smbus_transaction_status(struct p3h2x4x_i3c_hub_dev *hub,
						 u8 target_port_status,
						 u8 data_length)
{
	u32 status_read;
	u8 status;
	int ret;

	fsleep(P3H2X4X_SMBUS_400kHz_TRANSFER_TIMEOUT(data_length));

	ret = regmap_read(hub->regmap, target_port_status, &status_read);
	if (ret)
		return ret;

	status = (u8)status_read;

	status = (status & P3H2X4X_TP_TRANSACTION_CODE_MASK)
		  >> P3H2X4X_SMBUS_CNTRL_STATUS_TXN_SHIFT;

	switch (status) {
	case P3H2X4X_SMBUS_CNTRL_STATUS_TXN_OK:
		return 0;
	case P3H2X4X_SMBUS_CNTRL_STATUS_TXN_ADDR_NAK:
		return -ENXIO;
	case P3H2X4X_SMBUS_CNTRL_STATUS_TXN_DATA_NAK:
		return -EIO;
	case P3H2X4X_SMBUS_CNTRL_STATUS_TXN_SCL_TO:
		return -ETIMEDOUT;
	case P3H2X4X_SMBUS_CNTRL_STATUS_TXN_ARB_LOSS:
		return -EAGAIN;
	default:
		return -EIO;
	}
}

/*
 * p3h2x4x_tp_i2c_xfer_msg() - This starts a SMBus write transaction by writing a descriptor
 * and a message to the p3h2x4x registers. Controller buffer page is determined by multiplying the
 * target port index by four and adding the base page number to it.
 */
static int p3h2x4x_tp_i2c_xfer_msg(struct p3h2x4x_i3c_hub_dev *p3h2x4x_i3c_hub,
				   struct i2c_msg *xfers,
				   u8 target_port,
				   u8 nxfers_i, u8 rw)
{
	u8 controller_buffer_page = P3H2X4X_CONTROLLER_BUFFER_PAGE + 4 * target_port;
	u8 target_port_status = P3H2X4X_TP0_SMBUS_AGNT_STS + target_port;
	u8 desc[P3H2X4X_SMBUS_DESCRIPTOR_SIZE] = { 0 };
	u8 transaction_type = P3H2X4X_SMBUS_400kHz;
	int write_length, read_length;
	u8 addr = xfers[nxfers_i].addr;
	u8 rw_address = 2 * addr;
	int ret, ret2;

	if (rw == 2) { /* write and read */
		write_length = xfers[nxfers_i].len;
		read_length =  xfers[nxfers_i + 1].len;
	} else if (rw == 1) {
		rw_address |= P3H2X4X_SET_BIT(0);
		write_length = 0;
		read_length =  xfers[nxfers_i].len;
	} else {
		write_length = xfers[nxfers_i].len;
		read_length = 0;
	}

	desc[P3H2X4X_DESC_ADDR] = rw_address;
	if (rw == 2)
		desc[P3H2X4X_DESC_TYPE] = transaction_type | P3H2X4X_SET_BIT(0);
	else
		desc[P3H2X4X_DESC_TYPE] = transaction_type;
	desc[P3H2X4X_DESC_WRITE_LEN] = write_length;
	desc[P3H2X4X_DESC_READ_LEN] = read_length;

	ret = regmap_write(p3h2x4x_i3c_hub->regmap, target_port_status,
			   P3H2X4X_TP_BUFFER_STATUS_MASK);
	if (ret)
		goto out;

	ret = regmap_write(p3h2x4x_i3c_hub->regmap, P3H2X4X_PAGE_PTR, controller_buffer_page);

	if (ret)
		goto out;

	ret = regmap_bulk_write(p3h2x4x_i3c_hub->regmap, P3H2X4X_CONTROLLER_AGENT_BUFF,
				desc, P3H2X4X_SMBUS_DESCRIPTOR_SIZE);

	if (ret)
		goto out;

	if (!(rw % 2)) {
		ret = regmap_bulk_write(p3h2x4x_i3c_hub->regmap,
					P3H2X4X_CONTROLLER_AGENT_BUFF_DATA,
					xfers[nxfers_i].buf, xfers[nxfers_i].len);
		if (ret)
			goto out;
	}

	ret = regmap_write(p3h2x4x_i3c_hub->regmap, P3H2X4X_TP_SMBUS_AGNT_TRANS_START,
			   p3h2x4x_i3c_hub->tp_bus[target_port].tp_mask);

	if (ret)
		goto out;

	ret = p3h2x4x_read_smbus_transaction_status(p3h2x4x_i3c_hub,
						    target_port_status,
						    (write_length + read_length));
	if (ret)
		goto out;

	if (rw) {
		if (rw == 2)
			nxfers_i += 1;

		ret = regmap_bulk_read(p3h2x4x_i3c_hub->regmap,
				       P3H2X4X_CONTROLLER_AGENT_BUFF_DATA + write_length,
				       xfers[nxfers_i].buf, xfers[nxfers_i].len);
		if (ret)
			goto out;
	}
out:
	ret2 = regmap_write(p3h2x4x_i3c_hub->regmap,
			    P3H2X4X_PAGE_PTR, 0x00);
	if (!ret && ret2)
		ret = ret2;

	return ret;
}

/*
 * This function will be called whenever you call I2C read, write APIs like
 * i2c_master_send(), i2c_master_recv() etc.
 */
static s32 p3h2x4x_tp_i2c_xfer(struct i2c_adapter *adap, struct i2c_msg *msgs, int num)
{
	int ret_sum = 0, ret;
	u8 msg_count, rw;

	struct tp_bus *bus = i2c_get_adapdata(adap);
	struct p3h2x4x_i3c_hub_dev *p3h2x4x_i3c_hub = bus->p3h2x4x_i3c_hub;

	guard(mutex)(&p3h2x4x_i3c_hub->etx_mutex);
	guard(mutex)(&bus->port_mutex);

	for (msg_count = 0; msg_count < num; msg_count++) {
		if (msgs[msg_count].len > P3H2X4X_SMBUS_PAYLOAD_SIZE) {
			dev_err(p3h2x4x_i3c_hub->dev,
				"Message nr. %d not sent - length over %d bytes.\n",
				msg_count, P3H2X4X_SMBUS_PAYLOAD_SIZE);
			continue;
		}

		rw = (msgs[msg_count].flags & I2C_M_RD) ? 1 : 0;
		if (!rw) {
			/* If a read message is immediately followed by a write message to
			 * the same address,  consider combining them into a single transaction.
			 */
			if (msg_count + 1 < num &&
			    msgs[msg_count].addr == msgs[msg_count + 1].addr &&
			    (msgs[msg_count + 1].flags & I2C_M_RD)) {
				rw = 2;
				msg_count += 1;
				ret_sum += 1;
			}
		}

		ret = p3h2x4x_tp_i2c_xfer_msg(p3h2x4x_i3c_hub,
					      msgs,
					      bus->tp_port,
					      (rw == 2) ? (msg_count - 1) : msg_count,
					       rw);
		if (ret)
			return ret;

		ret_sum++;
	}
	return ret_sum;
}

static u32 p3h2x4x_tp_smbus_funcs(struct i2c_adapter *adapter)
{
	return I2C_FUNC_I2C | I2C_FUNC_SMBUS_BLOCK_DATA;
}

/*
 * I2C algorithm Structure
 */
static struct i2c_algorithm p3h2x4x_tp_i2c_algorithm = {
	.master_xfer    = p3h2x4x_tp_i2c_xfer,
	.functionality  = p3h2x4x_tp_smbus_funcs,
};

/**
 * p3h2x4x_tp_smbus_algo - add i2c adapter for target port who
 * configured as SMBus.
 * @hub: p3h2x4x device structure.
 * Return: 0 in case of success, negative error code on failur.
 */
int p3h2x4x_tp_smbus_algo(struct p3h2x4x_i3c_hub_dev *hub)
{
	int ret;
	u8 tp;

	for (tp = 0; tp < P3H2X4X_TP_MAX_COUNT; tp++) {
		if (!hub->tp_bus[tp].of_node ||
		    hub->hub_config.tp_config[tp].mode != P3H2X4X_TP_MODE_SMBUS)
			continue;

		/* Allocate adapter */
		struct i2c_adapter *smbus_adapter =
			devm_kzalloc(hub->dev, sizeof(*smbus_adapter), GFP_KERNEL);
		if (!smbus_adapter)
			return -ENOMEM;

		/* Initialize adapter */
		smbus_adapter->owner = THIS_MODULE;
		smbus_adapter->class = I2C_CLASS_HWMON;
		smbus_adapter->algo = &p3h2x4x_tp_i2c_algorithm;
		smbus_adapter->dev.parent = hub->dev;
		smbus_adapter->dev.of_node = hub->tp_bus[tp].of_node;
		snprintf(smbus_adapter->name, sizeof(smbus_adapter->name),
			 "p3h2x4x-i3c-hub.tp-port-%d", tp);

		i2c_set_adapdata(smbus_adapter, &hub->tp_bus[tp]);

		/* Register adapter */
		ret = i2c_add_adapter(smbus_adapter);
		if (ret) {
			devm_kfree(hub->dev, smbus_adapter);
			return ret;
		}

		hub->tp_bus[tp].is_registered = true;
		hub->hub_config.tp_config[tp].ibi_en = false;
		hub->tp_bus[tp].tp_smbus_adapter = smbus_adapter;
	}

	/*
	 * holding SDA low when both SMBus Target Agent received data buffers are full.
	 * This feature can be used as a flow-control mechanism for MCTP applications to
	 * avoid MCTP transmitters on Target Ports time out when the SMBus agent buffers
	 * are not serviced in time by upstream controller and only receives write message
	 * from its downstream ports.
	 */
	ret = regmap_update_bits(hub->regmap, P3H2X4X_ONCHIP_TD_AND_SMBUS_AGNT_CONF,
				 P3H2X4X_TARGET_AGENT_DFT_IBI_CONF_MASK,
				 P3H2X4X_TARGET_AGENT_DFT_IBI_CONF);
	if (ret)
		return ret;

	return regmap_write(hub->regmap, P3H2X4X_TP_SMBUS_AGNT_IBI_CONFIG, P3H2X4X_IBI_DISABLED);
}

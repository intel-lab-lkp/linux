// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright 2025 NXP
 * This P3H2x4x driver file contain functions for SMBus/I2C virtual Bus creation and read/write.
 */
#include "p3h2840_i3c_hub.h"

static struct device *i2cdev_to_dev(struct i2c_client *i2cdev)
{
	return &i2cdev->dev;
}

static void p3h2x4x_read_smbus_agent_rx_buf(struct i3c_device *i3cdev, enum p3h2x4x_rcv_buf rfbuf,
					    enum p3h2x4x_tp tp, bool is_of)
{
	struct device *dev = i3cdev_to_dev(i3cdev);
	struct p3h2x4x *priv = dev_get_drvdata(dev);
	struct smbus_device *backend = NULL;

	u8 target_buffer_page, flag_clear = 0x0f, rx_data, temp, i;
	u8 slave_rx_buffer[P3H2x4x_SMBUS_TARGET_PAYLOAD_SIZE] = { 0 };
	u32 packet_len, slave_address, ret;

	target_buffer_page = (((rfbuf) ? P3H2x4x_TARGET_BUFF_1_PAGE : P3H2x4x_TARGET_BUFF_0_PAGE)
							+  (P3H2x4x_NO_PAGE_PER_TP * tp));
	ret = regmap_write(priv->regmap, P3H2x4x_PAGE_PTR, target_buffer_page);
	if (ret)
		goto ibi_err;

	/* read buffer length */
	ret = regmap_read(priv->regmap, P3H2x4x_TARGET_BUFF_LENGTH, &packet_len);
	if (ret)
		goto ibi_err;

	if (packet_len)
		packet_len = packet_len - 1;

	if (packet_len > P3H2x4x_SMBUS_TARGET_PAYLOAD_SIZE) {
		dev_err(dev, "Received message too big for p3h2x4x buffer\n");
		return;
	}

	/* read slave  address */
	ret = regmap_read(priv->regmap, P3H2x4x_TARGET_BUFF_ADDRESS, &slave_address);
	if (ret)
		goto ibi_err;

	/* read data */
	if (packet_len) {
		ret = regmap_bulk_read(priv->regmap, P3H2x4x_TARGET_BUFF_DATA,
				       slave_rx_buffer, packet_len);
		if (ret)
			goto ibi_err;
	}

	if (is_of)
		flag_clear = BUF_RECEIVED_FLAG_TF_MASK;
	else
		flag_clear = (((rfbuf == RCV_BUF_0) ? P3H2x4x_TARGET_BUF_0_RECEIVE :
					P3H2x4x_TARGET_BUF_1_RECEIVE));

	/* notify slave driver about received data */
	list_for_each_entry(backend, &priv->tp_bus[tp].tp_device_entry, list) {
		if (slave_address >> 1 == backend->addr && priv->is_slave_reg) {
			i2c_slave_event(backend->client, I2C_SLAVE_WRITE_REQUESTED,
					(u8 *)&slave_address);

			for (i = 0; i < packet_len; i++) {
				rx_data = slave_rx_buffer[i];
				i2c_slave_event(backend->client, I2C_SLAVE_WRITE_RECEIVED,
						&rx_data);
			}
			i2c_slave_event(backend->client, I2C_SLAVE_STOP, &temp);
			break;
		}
	}

ibi_err:
	regmap_write(priv->regmap, P3H2x4x_PAGE_PTR, 0x00);
	regmap_write(priv->regmap, P3H2x4x_TP0_SMBUS_AGNT_STS + tp, flag_clear);
}

/**
 * p3h2x4x_ibi_handler - IBI handler.
 * @i3cdev: i3c device.
 * @payload: two byte IBI payload data.
 *
 */
void p3h2x4x_ibi_handler(struct i3c_device *i3cdev,
			 const struct i3c_ibi_payload *payload)
{
	struct device *dev = i3cdev_to_dev(i3cdev);
	struct p3h2x4x *priv = dev_get_drvdata(dev);

	u32 target_port_status, payload_byte_one, payload_byte_two;
	u8 i;
	int  ret;

	payload_byte_one = (*(int *)payload->data);
	payload_byte_two = (*(int *)(payload->data + 4));

	if (!(payload_byte_one & P3H2x4x_SMBUS_AGENT_EVENT_FLAG_STATUS))
		goto err;

	mutex_lock(&priv->etx_mutex);
	ret = regmap_write(priv->regmap, P3H2x4x_TP_SMBUS_AGNT_IBI_CONFIG, P3H2x4x_ALL_TP_IBI_DIS);
	if (ret) {
		dev_err(dev, "Failed to Disable IBI\n");
		goto err;
	}

	for (i = 0; i < P3H2x4x_TP_MAX_COUNT; ++i) {
		if (priv->tp_bus[i].is_registered && (payload_byte_two >> i) & 0x01) {
			ret = regmap_read(priv->regmap, P3H2x4x_TP0_SMBUS_AGNT_STS + i,
					  &target_port_status);
			if (ret) {
				dev_err(dev, "target port read status failed %d\n", ret);
				goto err;
			}

			/* process data receive buffer */
			switch (target_port_status & BUF_RECEIVED_FLAG_MASK) {
			case P3H2x4x_TARGET_BUF_CA_TF:
				break;
			case P3H2x4x_TARGET_BUF_0_RECEIVE:
				p3h2x4x_read_smbus_agent_rx_buf(i3cdev, RCV_BUF_0, i, false);
				break;
			case P3H2x4x_TARGET_BUF_1_RECEIVE:
				p3h2x4x_read_smbus_agent_rx_buf(i3cdev, RCV_BUF_1, i, false);
				break;
			case P3H2x4x_TARGET_BUF_0_1_RECEIVE:
				p3h2x4x_read_smbus_agent_rx_buf(i3cdev, RCV_BUF_0, i, false);
				p3h2x4x_read_smbus_agent_rx_buf(i3cdev, RCV_BUF_1, i, false);
				break;
			case P3H2x4x_TARGET_BUF_OVRFL:
				p3h2x4x_read_smbus_agent_rx_buf(i3cdev, RCV_BUF_0, i, false);
				p3h2x4x_read_smbus_agent_rx_buf(i3cdev, RCV_BUF_1, i, true);
				dev_err(dev, "Overflow, reading buffer zero and one\n");
				break;
			default:
				regmap_write(priv->regmap, P3H2x4x_TP0_SMBUS_AGNT_STS + i,
					     BUF_RECEIVED_FLAG_TF_MASK);
				break;
			}
		}
	}
err:
	regmap_write(priv->regmap, P3H2x4x_TP_SMBUS_AGNT_IBI_CONFIG, priv->tp_ibi_mask);
	mutex_unlock(&priv->etx_mutex);
}

static int p3h2x4x_read_p3h2x4x_id(struct device *dev)
{
	struct p3h2x4x *priv = dev_get_drvdata(dev);
	u32 reg_val;
	int ret;

	ret = regmap_read(priv->regmap, P3H2x4x_LDO_AND_CPSEL_STS, &reg_val);
	if (ret) {
		dev_err(dev, "Failed to read status register\n");
		return ret;
	}
	if (P3H2x4x_CP_SEL_PIN_INPUT_CODE_GET(reg_val) == 3)
		return 1;
	else
		return 0;
}

static void unlock_port_ext_mutex(struct mutex *ext, struct mutex *port)
{
	mutex_unlock(ext);
	mutex_unlock(port);
}

static void lock_port_ext_mutex(struct mutex *ext, struct mutex *port)
{
	mutex_lock(ext);
	mutex_lock(port);
}

static int p3h2x4x_read_smbus_transaction_status(struct p3h2x4x *priv,
						 u8 target_port_status,
						 u8 *status, u8 data_length)
{
	unsigned int status_read;
	int ret;

	mutex_unlock(&priv->etx_mutex);
	fsleep(P3H2x4x_SMBUS_400kHz_TRANSFER_TIMEOUT(data_length));
	mutex_lock(&priv->etx_mutex);

	ret = regmap_read(priv->regmap, target_port_status, &status_read);
	if (ret)
		return ret;

	*status = (u8)status_read;

	if ((*status & P3H2x4x_TP_BUFFER_STATUS_MASK) == P3H2x4x_XFER_SUCCESS)
		return 0;

	dev_err(&priv->i3cdev->dev, "Status read timeout reached\n");
	return 0;
}

/*
 * p3h2x4x_tp_i2c_xfer_msg() - This starts a SMBus write transaction by writing a descriptor
 * and a message to the p3h2x4x registers. Controller buffer page is determined by multiplying the
 * target port index by four and adding the base page number to it.
 */
static int p3h2x4x_tp_i2c_xfer_msg(struct p3h2x4x *priv,
				   struct i2c_msg *xfers,
				   u8 target_port,
				   u8 nxfers_i, u8 rw, u8 *return_status)
{
	u8 controller_buffer_page = P3H2x4x_CONTROLLER_BUFFER_PAGE + 4 * target_port;
	u8 target_port_status = P3H2x4x_TP0_SMBUS_AGNT_STS + target_port;
	u8 desc[P3H2x4x_SMBUS_DESCRIPTOR_SIZE] = { 0 };
	u8 transaction_type = P3H2x4x_SMBUS_400kHz;
	u8 target_port_code = BIT(target_port);
	int write_length = xfers[nxfers_i].len;
	int read_length = xfers[nxfers_i].len;
	u8 addr = xfers[nxfers_i].addr;
	u8 rw_address = 2 * addr;
	u8 status;
	int ret;

	if (rw) {
		rw_address |= BIT(0);
		write_length = 0;
	} else {
		read_length = 0;
	}

	desc[0] = rw_address;
	desc[1] = transaction_type;
	desc[2] = write_length;
	desc[3] = read_length;

	ret = regmap_write(priv->regmap, target_port_status,
			   P3H2x4x_TP_BUFFER_STATUS_MASK);
	if (ret)
		return ret;

	ret = regmap_write(priv->regmap, P3H2x4x_PAGE_PTR, controller_buffer_page);
	if (ret)
		return ret;

	ret = regmap_bulk_write(priv->regmap, P3H2x4x_CONTROLLER_AGENT_BUFF,
				desc, P3H2x4x_SMBUS_DESCRIPTOR_SIZE);
	if (ret)
		return ret;

	if (!rw) {
		ret = regmap_bulk_write(priv->regmap,
					P3H2x4x_CONTROLLER_AGENT_BUFF_DATA,
					xfers[nxfers_i].buf, xfers[nxfers_i].len);
		if (ret)
			return ret;
	}

	ret = regmap_write(priv->regmap, P3H2x4x_TP_SMBUS_AGNT_TRANS_START,
			   target_port_code);
	if (ret)
		return ret;

	ret = p3h2x4x_read_smbus_transaction_status(priv, target_port_status,
						    &status,
						    (write_length + read_length));
	if (ret)
		return ret;

	*return_status = status;

	if (rw) {
		ret = regmap_bulk_read(priv->regmap,
				       P3H2x4x_CONTROLLER_AGENT_BUFF_DATA,
				       xfers[nxfers_i].buf, xfers[nxfers_i].len);
		if (ret)
			return ret;
	}

	ret = regmap_write(priv->regmap, P3H2x4x_PAGE_PTR, 0x00);
	if (ret)
		return ret;

	return 0;
}

/*
 * This function will be called whenever you call I2C read, write APIs like
 * i2c_master_send(), i2c_master_recv() etc.
 */
static s32 p3h2x4x_tp_i2c_xfer(struct i2c_adapter *adap, struct i2c_msg *msgs, int num)
{
	u8 return_status;
	int ret_sum = 0;
	u8 msg_count;
	int ret;
	u8 rw;

	struct device *dev;
	struct tp_bus *bus =
		container_of(adap, struct tp_bus, smbus_port_adapter);
	struct p3h2x4x *priv = bus->priv;

	if (priv->is_p3h2x4x_in_i3c)
		dev = i3cdev_to_dev(priv->i3cdev);
	else
		dev = i2cdev_to_dev(priv->i2cdev);

	lock_port_ext_mutex(&priv->etx_mutex, &bus->port_mutex);

	if (priv->is_p3h2x4x_in_i3c) {
		ret = i3c_device_disable_ibi(priv->i3cdev);
		if (ret) {
			dev_err(dev, "Failed to Disable IBI\n");
			unlock_port_ext_mutex(&priv->etx_mutex, &bus->port_mutex);
			return ret;
		}
	}

	for (msg_count = 0; msg_count < num; msg_count++) {
		if (msgs[msg_count].len > P3H2x4x_SMBUS_PAYLOAD_SIZE) {
			dev_err(&adap->dev,
				"Message nr. %d not sent - length over %d bytes.\n",
				msg_count, P3H2x4x_SMBUS_PAYLOAD_SIZE);
			continue;
		}
		rw = msgs[msg_count].flags % 2;

		ret = p3h2x4x_tp_i2c_xfer_msg(priv,
					      msgs,
					      bus->tp_port,
					      msg_count, rw, &return_status);

		if (ret)
			goto error;

		if (return_status == P3H2x4x_XFER_SUCCESS)
			ret_sum++;
	}

error:
	if (priv->is_p3h2x4x_in_i3c) {
		ret =  i3c_device_enable_ibi(priv->i3cdev);
		if (ret) {
			dev_err(dev, "Failed to Enable IBI\n");
			unlock_port_ext_mutex(&priv->etx_mutex, &bus->port_mutex);
			return ret;
		}
	}
	unlock_port_ext_mutex(&priv->etx_mutex, &bus->port_mutex);
	return ret_sum;
}

static int p3h2x4x_tp_smbus_xfer_msg(struct p3h2x4x *priv,
				     u8 target_port,
				     u8 addr,
				     u8 rw,
				     u8 cmd,
				     int sz,
				     union i2c_smbus_data *data,
				     u8 *return_status)
{
	u8 controller_buffer_page = P3H2x4x_CONTROLLER_BUFFER_PAGE + 4 * target_port;
	u8 target_port_status = P3H2x4x_TP0_SMBUS_AGNT_STS + target_port;
	u8 desc[P3H2x4x_SMBUS_DESCRIPTOR_SIZE] = { 0 };
	u8 transaction_type = P3H2x4x_SMBUS_400kHz;
	u8 target_port_code = BIT(target_port);
	u8 buf[I2C_SMBUS_BLOCK_MAX + 2] = {0};
	u8 read_length = 0, write_length = 0;
	u8 rw_address = 2 * addr;
	struct device *dev;
	int ret, i;
	u8 status;

	if (priv->is_p3h2x4x_in_i3c)
		dev = i3cdev_to_dev(priv->i3cdev);
	else
		dev = i2cdev_to_dev(priv->i2cdev);

	/* Map the size to what the chip understands */
	switch (sz) {
	case I2C_SMBUS_QUICK:
	case I2C_SMBUS_BYTE:
		if (rw)	{
			buf[0] = data->byte;
			read_length = ONE_BYTE_SIZE;
			write_length = 0;
			rw_address |= BIT(0);
		} else {
			buf[0] = cmd;
			write_length = ONE_BYTE_SIZE;
			read_length = 0;
		}
		break;
	case I2C_SMBUS_BYTE_DATA:
		if (rw) {   /* read write */
			buf[0] = cmd;
			write_length = ONE_BYTE_SIZE;
			read_length = ONE_BYTE_SIZE;
			transaction_type |= BIT(0);
		} else {  /* only write */
			buf[0] = cmd;
			buf[1] = data->byte;
			write_length = ONE_BYTE_SIZE + 1;
			read_length = 0;
		}
		break;
	case I2C_SMBUS_WORD_DATA:
		if (rw) {         /* read write */
			buf[0] = cmd;
			write_length = ONE_BYTE_SIZE;
			read_length = 2;
			transaction_type |= BIT(0);
		} else {  /* only write */
			buf[0] = cmd;
			buf[1] = data->word & 0xff;
			buf[2] = (data->word & 0xff00) >> 8;
			write_length = ONE_BYTE_SIZE + 2;
			read_length = 0;
		}
		break;
	case I2C_SMBUS_BLOCK_DATA:
		if (rw) {         /* read write */
			buf[0] = cmd;
			write_length = ONE_BYTE_SIZE;
			read_length = data->block[0] + 1;
			transaction_type |= BIT(0);
		} else {  /* only write */
			buf[0] = cmd;
			for (i = 0 ; i <= data->block[0]; i++)
				buf[i + 1] = data->block[i];

			write_length = data->block[0] + 2;
			read_length = 0;
		}
		break;
	case I2C_SMBUS_I2C_BLOCK_DATA:
		if (rw) {         /* read write */
			buf[0] = cmd;
			write_length = ONE_BYTE_SIZE;
			read_length = data->block[0];
			transaction_type |= BIT(0);
		} else {  /* only write */
			buf[0] = cmd;
			for (i = 0 ; i < data->block[0]; i++)
				buf[i + 1] = data->block[i + 1];

			write_length = data->block[0] + 1;
			read_length = 0;
		}
		break;
	default:
		dev_warn(dev, "Unsupported transaction %d\n", sz);
		break;
	}

	desc[0] = rw_address;
	desc[1] = transaction_type;
	desc[2] = write_length;
	desc[3] = read_length;

	ret = regmap_write(priv->regmap, target_port_status,
			   P3H2x4x_TP_BUFFER_STATUS_MASK);
	if (ret)
		return ret;

	ret = regmap_write(priv->regmap, P3H2x4x_PAGE_PTR, controller_buffer_page);
	if (ret)
		return ret;

	ret = regmap_bulk_write(priv->regmap, P3H2x4x_CONTROLLER_AGENT_BUFF,
				desc, P3H2x4x_SMBUS_DESCRIPTOR_SIZE);
	if (ret)
		return ret;

	if (write_length) {
		ret = regmap_bulk_write(priv->regmap,
					P3H2x4x_CONTROLLER_AGENT_BUFF_DATA,
					buf, write_length);
		if (ret)
			return ret;
	}

	ret = regmap_write(priv->regmap, P3H2x4x_TP_SMBUS_AGNT_TRANS_START,
			   target_port_code);
	if (ret)
		return ret;

	ret = p3h2x4x_read_smbus_transaction_status(priv, target_port_status, &status,
						    (write_length + read_length));
	if (ret)
		return ret;

	*return_status = status;

	if (rw) {
		switch (sz) {
		case I2C_SMBUS_QUICK:
		case I2C_SMBUS_BYTE:
		case I2C_SMBUS_BYTE_DATA:
			{
				ret = regmap_bulk_read(priv->regmap,
						       P3H2x4x_CONTROLLER_AGENT_BUFF_DATA +
						       write_length,
						       &data->byte, read_length);
				break;
			}
		case I2C_SMBUS_WORD_DATA:
			{
				ret = regmap_bulk_read(priv->regmap,
						       P3H2x4x_CONTROLLER_AGENT_BUFF_DATA +
						       write_length,
						       (u8 *)&data->word, read_length);
				break;
			}
		case I2C_SMBUS_BLOCK_DATA:
			{
				ret = regmap_bulk_read(priv->regmap,
						       P3H2x4x_CONTROLLER_AGENT_BUFF_DATA +
						       write_length,
						       data->block, read_length);
				break;
			}
		case I2C_SMBUS_I2C_BLOCK_DATA:
			{
				ret = regmap_bulk_read(priv->regmap,
						       P3H2x4x_CONTROLLER_AGENT_BUFF_DATA +
						       write_length,
						       data->block + 1, read_length);
				break;
			}
		default:
				dev_warn(dev, "Unsupported transaction %d\n", sz);
				break;
		}

		if (ret)
			return ret;
	}

	ret = regmap_write(priv->regmap, P3H2x4x_PAGE_PTR, 0x00);
	if (ret)
		return ret;

	return 0;
}

static s32 p3h2x4x_tp_smbus_xfer(struct i2c_adapter *adap, u16 addr, unsigned short flags,
				 char read_write, u8 command, int size,
				 union i2c_smbus_data *data)
{
	struct tp_bus *bus =
		container_of(adap, struct tp_bus, smbus_port_adapter);

	struct p3h2x4x *priv = bus->priv;
	struct device *dev;

	if (priv->is_p3h2x4x_in_i3c)
		dev = i3cdev_to_dev(priv->i3cdev);
	else
		dev = i2cdev_to_dev(priv->i2cdev);

	int ret, ret_status;
	u8 return_status;

	lock_port_ext_mutex(&priv->etx_mutex, &bus->port_mutex);

	if (priv->is_p3h2x4x_in_i3c) {
		ret = i3c_device_disable_ibi(priv->i3cdev);
		if (ret) {
			dev_err(dev, "Failed to Disable IBI\n");
			unlock_port_ext_mutex(&priv->etx_mutex, &bus->port_mutex);
			return ret;
		}
	}

	ret_status = p3h2x4x_tp_smbus_xfer_msg(priv,
					       (u8)bus->tp_port,
					       (u8)addr,
					       (u8)read_write,
					       (u8)command,
					       size,
					       data,
					       &return_status);

	if (priv->is_p3h2x4x_in_i3c) {
		ret = i3c_device_enable_ibi(priv->i3cdev);
		if (ret) {
			dev_err(dev, "Failed to Enable IBI\n");
			unlock_port_ext_mutex(&priv->etx_mutex, &bus->port_mutex);
			return ret;
		}
	}
	unlock_port_ext_mutex(&priv->etx_mutex, &bus->port_mutex);

	if (ret_status)
		return ret_status;

	if (return_status == P3H2x4x_XFER_SUCCESS)
		return 0;
	else
		return -EINVAL;
}

static u32 p3h2x4x_tp_smbus_funcs(struct i2c_adapter *adapter)
{
	return I2C_FUNC_SMBUS_BYTE | I2C_FUNC_SMBUS_BYTE_DATA |
			I2C_FUNC_SMBUS_QUICK | I2C_FUNC_SMBUS_WORD_DATA |
			I2C_FUNC_SMBUS_I2C_BLOCK  | I2C_FUNC_SMBUS_BLOCK_DATA |
			I2C_FUNC_I2C;
}

#if IS_ENABLED(CONFIG_I2C_SLAVE)
static int p3h2x4x_tp_i2c_reg_slave(struct i2c_client *slave)
{
	struct tp_bus *bus =
		container_of(slave->adapter, struct tp_bus, smbus_port_adapter);
	struct p3h2x4x *priv = bus->priv;

	priv->is_slave_reg = true;

	return 0;
}

static int p3h2x4x_tp_i2c_unreg_slave(struct i2c_client *slave)
{
	struct tp_bus *bus =
		container_of(slave->adapter, struct tp_bus, smbus_port_adapter);
	struct p3h2x4x *priv = bus->priv;

	priv->is_slave_reg = false;

	return 0;
}
#endif

/*
 * I2C algorithm Structure
 */
static struct i2c_algorithm p3h2x4x_tp_i2c_algorithm = {
	.master_xfer    = p3h2x4x_tp_i2c_xfer,
	.smbus_xfer = p3h2x4x_tp_smbus_xfer,
#if IS_ENABLED(CONFIG_I2C_SLAVE)
	.reg_slave = p3h2x4x_tp_i2c_reg_slave,
	.unreg_slave = p3h2x4x_tp_i2c_unreg_slave,
#endif
	.functionality  = p3h2x4x_tp_smbus_funcs,
};

/*
 * p3h2x4x_tp_add_downstream_device - prove downstream devices for target port who
 * configured as SMBus.
 * @priv: p3h2x4x device structure.
 * Return: 0 in case of success, a negative EINVAL code if the error.
 */
int p3h2x4x_tp_add_downstream_device(struct p3h2x4x *priv)
{
	struct i2c_board_info smbus_tp_device_info = {0};
	struct smbus_device *backend = NULL;
	struct tp_bus *bus;
	struct device *dev;
	int i;

	if (priv->is_p3h2x4x_in_i3c)
		dev = i3cdev_to_dev(priv->i3cdev);
	else
		dev = i2cdev_to_dev(priv->i2cdev);

	for (i = 0; i < P3H2x4x_TP_MAX_COUNT; i++) {
		bus = &priv->tp_bus[i];
		if (!bus->is_registered)
			continue;

		list_for_each_entry(backend,
				    &bus->tp_device_entry, list) {
			smbus_tp_device_info.addr = backend->addr;
			smbus_tp_device_info.flags = I2C_CLIENT_SLAVE;
			smbus_tp_device_info.of_node = backend->tp_device_dt_node;
			snprintf(smbus_tp_device_info.type, I2C_NAME_SIZE, backend->compatible);
			backend->client = i2c_new_client_device(&bus->smbus_port_adapter,
								&smbus_tp_device_info);
			if (IS_ERR(backend->client)) {
				dev_warn(dev, "Error while registering backend\n");
				return -EINVAL;
			}
		}
	}
	return 0;
}

/*
 * p3h2x4x_tp_smbus_algo - add i2c adapter for target port who
 * configured as SMBus.
 * @priv: p3h2x4x device structure.
 * @tp: target port.
 * Return: 0 in case of success, a negative EINVAL code if the error.
 */
int p3h2x4x_tp_smbus_algo(struct p3h2x4x *priv, int tp)
{
	int ret;
	struct device *dev;

	if (priv->is_p3h2x4x_in_i3c)
		dev = i3cdev_to_dev(priv->i3cdev);
	else
		dev = i2cdev_to_dev(priv->i2cdev);

	priv->tp_bus[tp].smbus_port_adapter.owner = THIS_MODULE;
	priv->tp_bus[tp].smbus_port_adapter.class = I2C_CLASS_HWMON;
	priv->tp_bus[tp].smbus_port_adapter.algo = &p3h2x4x_tp_i2c_algorithm;

	sprintf(priv->tp_bus[tp].smbus_port_adapter.name, "p3h2x4x-cp-%d.tp-port-%d",
		p3h2x4x_read_p3h2x4x_id(dev), tp);

	ret = i2c_add_adapter(&priv->tp_bus[tp].smbus_port_adapter);
	if (ret) {
		dev_warn(dev, "failled to add adapter for tp %d\n", tp);
		return -EINVAL;
	}
	priv->tp_bus[tp].is_registered = true;

	return 0;
}

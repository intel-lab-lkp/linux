// SPDX-License-Identifier: GPL-2.0
/*
 * (C) Copyright 2017, Fresco Logic, Incorporated.
 * (C) Copyright 2018-2019, Artem Mygaiev
 */

#include <linux/unaligned.h>

#include <drm/drm_edid.h>

#include "fl2000.h"

/*
 * The I2C controller requires a mandatory 8-bit sub-address for any read/write operation. Each
 * read or write operates on 8-bit data. Every exchange shall consist of 2 messages (sub-address +
 * data) combined. The USB transfer always aligns the address to a 4-byte boundary.
 */
#define I2C_CMESSAGES_NUM  2
#define I2C_REG_ADDR_SIZE  (sizeof(u8))
#define I2C_REG_DATA_SIZE  (sizeof(u8))
#define I2C_XFER_ADDR_MASK (~0x3ul)

static inline int fl2000_i2c_read_dword(struct regmap *regmap, u16 addr, u8 offset, u32 *data)
{
	return fl2000_i2c_dword(regmap, true, addr, offset, data);
}

static inline int fl2000_i2c_write_dword(struct regmap *regmap, u16 addr, u8 offset, u32 *data)
{
	return fl2000_i2c_dword(regmap, false, addr, offset, data);
}

static int fl2000_i2c_xfer(struct i2c_adapter *adapter, struct i2c_msg *msgs, int num)
{
	int ret;
	bool read;
	u16 addr;
	u8 idx;
	u8 offset;
	u32 data;

	/*
	 * The adapter quirks only let through:
	 * - 2 messages of 1 byte each, first write then read
	 * - 1 message of at most 2 bytes
	 */
	if (num == 2)
		read = true;
	else if (msgs[0].len == 2 && !(msgs[0].flags & I2C_M_RD))
		read = false;
	else
		return -EOPNOTSUPP;

	addr = msgs[0].addr;
	offset = msgs[0].buf[0] & I2C_XFER_ADDR_MASK;
	idx = msgs[0].buf[0] - offset;

	/*
	 * The FL2000 I2C engine only transfers aligned dwords, so the offset is aligned down to a
	 * 4-byte boundary and the requested byte is picked out of (or merged into) the dword.
	 */
	if (read) {
		ret = fl2000_i2c_read_dword(adapter->algo_data, addr, offset, &data);
		if (ret)
			return ret;

		msgs[1].buf[0] = data >> (idx * 8);
	} else {
		/*
		 * Since the FL2000 I2C engine always operates on 4-byte messages, we need to read
		 * before write in order not to corrupt the unrelated registers of the same dword.
		 */
		ret = fl2000_i2c_read_dword(adapter->algo_data, addr, offset, &data);
		if (ret)
			return ret;

		data &= ~(0xffU << (idx * 8));
		data |= (u32)msgs[0].buf[1] << (idx * 8);

		ret = fl2000_i2c_write_dword(adapter->algo_data, addr, offset, &data);
		if (ret)
			return ret;
	}

	return num;
}

/*
 * Monitor DDC on the FL2000 bus. The bridge's own EDID command does not work behind this engine,
 * and the VGA connector has no bridge at all, so EDID is read with the FL2000 I2C engine at address
 * 0x50, 4 bytes per operation with an 8-bit offset, which covers EDID blocks 0 and 1.
 */
#define FL2000_DDC_ADDR 0x50

/**
 * fl2000_ddc_read_block() - read one EDID block through the FL2000 I2C engine
 * @adapter: the FL2000 I2C adapter
 * @buf: destination, @len bytes
 * @block: EDID block number (0 or 1)
 * @len: bytes to read, at most EDID_LENGTH
 *
 * The signature matches what drm_edid_read_custom() expects, with the adapter as context.
 *
 * Return: 0 on success, a negative error code otherwise
 */
int fl2000_ddc_read_block(struct i2c_adapter *adapter, u8 *buf, unsigned int block, size_t len)
{
	struct regmap *regmap = adapter->algo_data;
	unsigned int start = block * EDID_LENGTH;
	int ret = 0;

	if (block > 1 || len > EDID_LENGTH)
		return -EOPNOTSUPP;

	/* One operation of the engine is a sequence of register accesses: keep others out */
	i2c_lock_bus(adapter, I2C_LOCK_SEGMENT);
	for (unsigned int i = 0; i < len; i += 4) {
		u32 data;

		ret = fl2000_i2c_dword(regmap, true, FL2000_DDC_ADDR, start + i, &data);
		if (ret)
			break;

		put_unaligned_le32(data, buf + i);
	}
	i2c_unlock_bus(adapter, I2C_LOCK_SEGMENT);

	return ret;
}

static u32 fl2000_i2c_func(struct i2c_adapter *adap)
{
	return I2C_FUNC_I2C;
}

static const struct i2c_algorithm fl2000_i2c_algorithm = {
	.xfer = fl2000_i2c_xfer,
	.functionality = fl2000_i2c_func,
};

static const struct i2c_adapter_quirks fl2000_i2c_quirks = {
	/* Combined transfers only: an address write, then a read, on the same address */
	.flags = I2C_AQ_COMB_WRITE_THEN_READ | I2C_AQ_NO_ZERO_LEN,
	.max_num_msgs = I2C_CMESSAGES_NUM,
	.max_write_len = 2 * I2C_REG_DATA_SIZE,
	.max_read_len = I2C_REG_DATA_SIZE,
	.max_comb_1st_msg_len = I2C_REG_ADDR_SIZE,
	.max_comb_2nd_msg_len = I2C_REG_DATA_SIZE,
};

struct i2c_adapter *fl2000_i2c_init(struct usb_interface *interface, struct regmap *regmap)
{
	int ret;
	struct i2c_adapter *adapter;
	struct usb_device *usb_dev = interface_to_usbdev(interface);
	u8 usb_path[32];

	/* Adapter must be allocated before anything else */
	adapter = devm_kzalloc(&interface->dev, sizeof(*adapter), GFP_KERNEL);
	if (!adapter)
		return ERR_PTR(-ENOMEM);

	adapter->owner = THIS_MODULE;
	adapter->algo = &fl2000_i2c_algorithm;
	adapter->quirks = &fl2000_i2c_quirks;
	adapter->algo_data = regmap;
	adapter->dev.parent = &interface->dev;
	strscpy(adapter->name, "FL2000 bridge I2C bus", sizeof(adapter->name));

	ret = devm_i2c_add_adapter(&interface->dev, adapter);
	if (ret)
		return ERR_PTR(ret);

	usb_make_path(usb_dev, usb_path, sizeof(usb_path));
	dev_dbg(&interface->dev, "Created FL2000 bridge I2C bus %d at interface %s\n",
		i2c_adapter_id(adapter), usb_path);

	return adapter;
}

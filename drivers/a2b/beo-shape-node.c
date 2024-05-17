// SPDX-License-Identifier: GPL-2.0-only
/*
 * Beosound Shape A2B transceiver node driver
 *
 * Copyright (c) 2023-2024 Alvin Šipraga <alsi@bang-olufsen.dk>
 *
 * This is basically an AD2425 driver. But in order to flash the STM32
 * microcontroller on the Beosound Shape, some help is needed on the part of the
 * A2B node driver.
 *
 * Here is a simplified block diagram of the problem this driver is dealing
 * with:
 *
 *                                ┌───────────┐
 *                        ┌───────│ regulator │
 *                        │       └──────▲────┘
 *                        │ 5V           │ GPIO enable
 *      ┌──────┐  A2B ┌───▼──┐  I2C  ┌───────┐
 *      │ A2B  │/\/\/\│ A2B  │───────│ STM32 │
 *      │ main │\/\/\/│ sub  │       │  MCU  │
 *      └──────┘      └──────┘       └───────┘
 *
 * The Shape's MCU is an STM32F072. It has a bootloader. The bootloader can
 * either enter firmware update (DFU) mode, or jump to the Bang & Olufsen
 * application code (APP). DFU mode is a proprietary implementation and does not
 * refer to the standard STM32 bootloader mode. DFU mode allows for the APP
 * code to be updated.
 *
 * Whether the bootloader enters DFU or APP mode depends on a flag kept in the
 * MCU's non-volatile flash memory. The MCU can be moved into DFU or APP mode by
 * issuing a command which sets the flag to DFU (resp. APP) mode and then
 * performs a software reset. The MCU responds over I2C in both modes, but the
 * commands are in general different. The command to read the flag is the same
 * for both modes, which allows the driver to determine the current state.
 *
 * When the MCU undergoes software reset, its GPIOs enter their default state
 * and this causes the A2B transceiver on the board to lose power due to a
 * hardware pull-down on the GPIO enable line of its supply regulator. This A2B
 * node driver supervises the process to ensure that the A2B discovery process
 * only continues when all currently discovered nodes have had their MCU
 * firmware updated.
 *
 * An obvious question is why not let an MCU-specific I2C driver handle the
 * firmware update. The answer lies in the issue of device probe order and
 * topology: suppose that an I2C driver flashed the MCU instead. Then what is
 * likely to happen is that further downstream nodes also get discovered and
 * potentially probed in between one of the transitions between APP/DFU
 * mode. This process is wasted as at some point there will be a bus drop and
 * all those new devices must also be cleaned up. Worse yet is if further
 * downstream MCU I2C drivers begin flashing as well, leading to a big mess of
 * devices coming and going during boot. By blocking the creation of a2b_func
 * devices and discovery of further nodes until this MCU reset flip-flopping is
 * complete, the chaos is kept to a minimum.
 *
 * After the firmware is up-to-date, the driver reverts to the standard
 * behaviour of the generic ad24xx-node driver.
 *
 * The firmware is split into 2048 byte sectors, and each sector has 16
 * blocks. Each block is written with a single I2C command. After each block
 * write command, an ACK must be read back successfully to continue with the
 * next block write. The MCU must only be put into APP mode when all blocks have
 * successfully been written - doing otherwise will cause the bootloader's
 * checksum verification to fail and it will then unconditionally fall into the
 * standard STM32 bootloader every time.
 */

#include <linux/delay.h>
#include <linux/firmware.h>
#include <linux/i2c.h>
#include <linux/iopoll.h>

#include "ad24xx-node.h"

/* The MCU answers on this I2C address */
#define MCU_ADDRESS		0x65

/* Firmware properties */
#define FW_ADDR			0x08004000
#define FW_SIZE			0x1B800
#define FW_BLKSZ		128
#define FW_SECSZ		2048
#define FW_BLKS_PER_SEC		(FW_SECSZ / FW_BLKSZ)
#define FW_SECTORS		(FW_SIZE / FW_SECSZ)
#define FW_VER32_ADDR		0x0801F7F8
#define FW_VER32_OFFSET		(FW_VER32_ADDR - FW_ADDR)

#define FW_VER32_0		0xFF000000
#define FW_VER32_1		0x00FF0000
#define FW_VER32_2		0x0000FF00
#define FW_VER32_3		0x000000FF
#define FW_VER32_TO_FW_VER(fw_ver32)		    \
	(FIELD_GET(FW_VER32_0, (fw_ver32)) * 1000 + \
	 FIELD_GET(FW_VER32_1, (fw_ver32)) * 100 +  \
	 FIELD_GET(FW_VER32_2, (fw_ver32)) * 10 +   \
	 FIELD_GET(FW_VER32_3, (fw_ver32)) * 1)
#define FW_VER32_FIELDS(fw_ver32)	   \
	FIELD_GET(FW_VER32_0, (fw_ver32)), \
	FIELD_GET(FW_VER32_1, (fw_ver32)), \
	FIELD_GET(FW_VER32_2, (fw_ver32)), \
	FIELD_GET(FW_VER32_3, (fw_ver32))
#define FW_VER32(fw_ver32)	FW_VER32_FIELDS(fw_ver32)
#define FW_VER32_FIELDS_FMT	"%u.%u.%u.%u"
#define FW_VER32_FMT		FW_VER32_FIELDS_FMT

#define FW_VER_FIELDS(fw_ver)	     \
	(((fw_ver) % 10000) / 1000), \
	 (((fw_ver) % 1000) / 100),  \
	 (((fw_ver) % 100) / 10),    \
	 (((fw_ver) % 10))
#define FW_VER(fw_ver)		FW_VER_FIELDS(fw_ver)
#define FW_VER_FIELDS_FMT	"%u.%u.%u.%u"
#define FW_VER_FMT		FW_VER_FIELDS_FMT

/* The DFU flag indicates whether or not the MCU is in DFU mode or not */
#define FLAG_APP_MODE		0x00
#define FLAG_DFU_MODE		0xDD

/* DFU constants */
#define DFU_ACK			0xAA
#define DFU_NACK		0xBB

/* Read commands in APP mode */
#define APP_READ_DFU_FLAG	0x00
#define APP_READ_ITEM_NO	0x01
#define APP_READ_TYPE_NO	0x02
#define APP_READ_SERIAL_NO	0x03
#define APP_READ_HW_VER		0x04
#define APP_READ_BTL_VER	0x05
#define APP_READ_APP_VER	0x06
#define APP_READ_DSP_VER	0x07
#define APP_READ_NTC_VALUE	0x08
#define APP_READ_DSP_DELAY	0x09
#define APP_READ_DSP_GAIN	0x0A
#define APP_READ_DSP_ROOMEQ	0x0B
#define APP_READ_DSP_ROOMEQ2	0x0C

/* Write commands in APP mode */
#define APP_WRITE_ENTER_DFU_MODE	0x01

/* Read commands in DFU mode */
#define DFU_READ_DFU_FLAG	APP_READ_DFU_FLAG
#define DFU_READ_ACK		0x02

/* Write commands in DFU mode */
#define DFU_WRITE_BLOCK			0x01
#define DFU_WRITE_ENTER_APP_MODE	0x02

static unsigned int force_fwupd;
module_param(force_fwupd, uint, 0644);
MODULE_PARM_DESC(force_fwupd, "force firmware update ignoring version check");

static int beo_shape_node_enter_app_mode(struct a2b_node *node)
{
	struct i2c_msg xfer[1];
	u8 buf[2] = {
		DFU_WRITE_ENTER_APP_MODE,
		0xFF - DFU_WRITE_ENTER_APP_MODE, /* checksum */
	};
	int ret;

	xfer[0].addr = MCU_ADDRESS;
	xfer[0].flags = 0;
	xfer[0].len = 2;
	xfer[0].buf = buf;

	ret = a2b_node_i2c_xfer(node, xfer, 1);
	if (ret < 0)
		return ret;

	/* Wait for the A2B transceiver to lose power */
	msleep(1000);

	return 0;
}

static int beo_shape_node_enter_dfu_mode(struct a2b_node *node)
{
	struct i2c_msg xfer[1];
	u8 reg = APP_WRITE_ENTER_DFU_MODE;
	int ret;

	xfer[0].addr = MCU_ADDRESS;
	xfer[0].flags = 0;
	xfer[0].len = 1;
	xfer[0].buf = &reg;

	ret = a2b_node_i2c_xfer(node, xfer, 1);
	if (ret < 0)
		return ret;

	/* Wait for the A2B transceiver to lose power */
	msleep(1000);

	return 0;
}

static int beo_shape_node_read(struct a2b_node *node, u8 reg, u8 *buf, u16 len)
{
	struct i2c_msg xfer[2];
	int ret;

	xfer[0].addr = MCU_ADDRESS;
	xfer[0].flags = 0;
	xfer[0].len = 1;
	xfer[0].buf = &reg;

	xfer[1].addr = MCU_ADDRESS;
	xfer[1].flags = I2C_M_RD;
	xfer[1].len = len;
	xfer[1].buf = buf;

	ret = a2b_node_i2c_xfer(node, xfer, 2);
	if (ret < 0)
		return ret;

	return 0;
}

static int beo_shape_node_read8(struct a2b_node *node, u8 reg, u8 *val)
{
	return beo_shape_node_read(node, reg, val, 1);
}

static int beo_shape_node_read16(struct a2b_node *node, u8 reg, u16 *val)
{
	int ret;

	ret = beo_shape_node_read(node, reg, (u8 *)val, 2);
	if (ret)
		return ret;

	*val = __le16_to_cpu(*val);

	return 0;
}

static int beo_shape_node_read32(struct a2b_node *node, u8 reg, u32 *val)
{
	int ret;

	ret = beo_shape_node_read(node, reg, (u8 *)val, 4);
	if (ret)
		return ret;

	*val = __le32_to_cpu(*val);

	return 0;
}

static int beo_shape_node_get_dfu_flag(struct a2b_node *node, u8 *flag)
{
	return beo_shape_node_read8(node, APP_READ_DFU_FLAG, flag);
}

static int beo_shape_node_get_app_ver(struct a2b_node *node, u16 *ver)
{
	return beo_shape_node_read16(node, APP_READ_APP_VER, ver);
}

static int beo_shape_node_get_item_no(struct a2b_node *node, u32 *item_no)
{
	return beo_shape_node_read32(node, APP_READ_ITEM_NO, item_no);
}

static int beo_shape_node_get_type_no(struct a2b_node *node, u32 *type_no)
{
	return beo_shape_node_read32(node, APP_READ_TYPE_NO, type_no);
}

static int beo_shape_node_get_serial_no(struct a2b_node *node, u32 *serial_no)
{
	return beo_shape_node_read32(node, APP_READ_SERIAL_NO, serial_no);
}

static int beo_shape_node_get_hw_ver(struct a2b_node *node, u32 *hw_ver)
{
	return beo_shape_node_read32(node, APP_READ_HW_VER, hw_ver);
}

static const char *beo_shape_node_hw_ver_string(u32 hw_ver)
{
	const char *hw_string[] = { "unknown", "ES1",  "ES2",  "ES3",
				    "EVT1",    "EVT2", "DVT1", "DVT2",
				    "PVT",     "MP1",  "MP2" };
	if (hw_ver >= ARRAY_SIZE(hw_string))
		return "unknown";

	return hw_string[hw_ver];
}

static int beo_shape_node_write_fw_blk(struct a2b_node *node,
				       const struct firmware *fw, u8 sec,
				       u8 blk)
{
	u32 offset = (sec * FW_SECSZ) + (blk * FW_BLKSZ);
	union {
		struct {
			u8 cmd;
			u8 data[FW_BLKSZ];
			u8 sec;
			u8 blk;
			u8 csum;
		};
		u8 raw[FW_BLKSZ + 4];
	} buf;
	struct i2c_msg xfer[1];
	unsigned int retries = 3;
	u8 ack = 0;
	int ret;
	int i;

	buf.cmd = DFU_WRITE_BLOCK;
	memcpy(buf.data, fw->data + offset, FW_BLKSZ);
	buf.sec = sec;
	buf.blk = blk;
	buf.csum = 0;

	for (i = 0; i < sizeof(buf) - 1; i++)
		buf.csum += buf.raw[i];
	buf.csum = 0xFF - buf.csum;

	xfer[0].addr = MCU_ADDRESS;
	xfer[0].flags = 0;
	xfer[0].len = sizeof(buf);
	xfer[0].buf = buf.raw;

retry:
	ret = a2b_node_i2c_xfer(node, xfer, 1);
	if (ret < 0)
		return ret;

	/*
	 * These sleeps are stolen from the firmware code. They might be too
	 * generous. But issuing a DFU_READ_ACK command too early will clobber
	 * the I2C RX buffer in the MCU while it is reading from that buffer to
	 * write a block. So the sleeps are crucial.
	 */
	if (blk == FW_BLKS_PER_SEC - 1)
		msleep(100);
	else
		msleep(3);

	/*
	 * An ACK indicates that the checksum at the end of the previous
	 * DFU_WRITE_BLOCK command was correct on the receiving (MCU) end.
	 */
	ret = beo_shape_node_read8(node, DFU_READ_ACK, &ack);
	if (ret)
		return ret;

	if (ack != DFU_ACK) {
		if (--retries > 0)
			goto retry;

		dev_err_ratelimited(&node->dev,
				    "got NACK on write of sec %d blk %d\n", sec,
				    blk);
		return -EIO;
	}

	return 0;
}

static int beo_shape_node_write_fw(struct a2b_node *node,
				   const struct firmware *fw)
{
	u8 sec, blk;
	int ret;

	for (sec = 0; sec < FW_SECTORS; sec++) {
		for (blk = 0; blk < FW_BLKS_PER_SEC; blk++) {
			ret = beo_shape_node_write_fw_blk(node, fw, sec, blk);
			if (ret)
				return ret;
		}
	}

	/*
	 * The firmware might silently ignore (but still ACK) subsequent
	 * commands for some reason... give it a moment.
	 */
	msleep(100);

	return 0;
}

struct beo_shape_node {
	bool resetting;
};

static int beo_shape_node_setup(struct a2b_node *node)
{
	struct beo_shape_node *shape;
	const struct firmware *fw;
	u32 fw_ver32;
	u16 fw_ver;
	int ret;
	u8 flag;

	if (node->priv)
		shape = node->priv;
	else {
		shape = devm_kzalloc(&node->dev, sizeof(*shape), GFP_KERNEL);
		if (!shape)
			return -ENOMEM;

		node->priv = shape;
	}

	/*
	 * A reset command was already sent to flip the MCU into APP or DFU
	 * mode. Nothing left to do until a bus drop. Just continue deferring
	 * probe.
	 */
	if (shape->resetting)
		return -EPROBE_DEFER;

	ret = beo_shape_node_get_dfu_flag(node, &flag);
	if (ret)
		return ret;

	ret = request_firmware(&fw, "beo/shape.bin", &node->dev);
	if (ret)
		return ret;

	if (fw->size != FW_SIZE) {
		ret = -EINVAL;
		goto release_fw;
	}

	/*
	 * The firmware binary contains a 32 bit version field at a fixed
	 * offset. There is also a 16 bit representation of the version returned
	 * by the APP over I2C. The data is interchangeable so we convert to a
	 * 16 bit representation to test whether or not the Shape needs a
	 * firmware update.
	 */
	fw_ver32 = *((u32 *)&fw->data[FW_VER32_OFFSET]);
	fw_ver = FW_VER32_TO_FW_VER(fw_ver32);

	if (flag != FLAG_DFU_MODE) {
		u32 hw_ver = 0;
		u32 type_no;
		u32 item_no;
		u32 serial_no;
		u16 app_ver;

		/*
		 * The APP firmware returns 0 on some read commands while it is
		 * still initializing. It doesn't send I2C NAKs. Due to this,
		 * the driver has to poll something to figure out when the
		 * firmware is actually ready. From what I can see, the HW
		 * revision is the last thing to get populated out of the
		 * miscellaneous read registers, and also not at all likely to
		 * be 0 thereafter. So let's use that. Give it up to 3 seconds.
		 */
		ret = read_poll_timeout(beo_shape_node_get_hw_ver, ret,
					(ret != 0 || hw_ver != 0), 100e3, 2e6,
					true, node, &hw_ver);
		if (ret)
			goto release_fw;

		ret = beo_shape_node_get_app_ver(node, &app_ver);
		if (ret)
			goto release_fw;

		ret = beo_shape_node_get_type_no(node, &type_no);
		if (ret)
			goto release_fw;

		ret = beo_shape_node_get_item_no(node, &item_no);
		if (ret)
			goto release_fw;

		ret = beo_shape_node_get_serial_no(node, &serial_no);
		if (ret)
			goto release_fw;

		dev_info(&node->dev,
			 "shape hw %u (%s) fw " FW_VER_FMT
			 " type %u item %u serial %u \n",
			 hw_ver, beo_shape_node_hw_ver_string(hw_ver),
			 FW_VER(app_ver), type_no, item_no, serial_no);

		if (app_ver != fw_ver || (BIT(node->addr) & force_fwupd)) {
			dev_info(&node->dev, "entering DFU mode\n");

			/*
			 * Unset the bit now that we are updating this shape in
			 * order to avoid an infinite update loop
			 */
			force_fwupd &= ~BIT(node->addr);

			ret = beo_shape_node_enter_dfu_mode(node);
			if (ret)
				goto release_fw;

			/* Expect a bus drop now */
			shape->resetting = true;
			ret = -EPROBE_DEFER;
			goto release_fw;
		}
	} else {
		dev_info(&node->dev, "writing fw " FW_VER32_FMT "\n",
			 FW_VER32(fw_ver32));

		ret = beo_shape_node_write_fw(node, fw);
		if (ret)
			goto release_fw;

		dev_info(&node->dev, "entering APP mode\n");

		ret = beo_shape_node_enter_app_mode(node);
		if (ret)
			goto release_fw;

		/* Expect a bus drop now */
		shape->resetting = true;
		ret = -EPROBE_DEFER;
		goto release_fw;
	}

release_fw:
	release_firmware(fw);

	if (ret)
		return ret;

	return ad24xx_node_setup(node);
}

static struct a2b_node_ops beo_shape_node_ops = {
	.set_respcycs = ad24xx_node_set_respcycs,
	.set_switching = ad24xx_node_set_switching,
	.is_last = ad24xx_node_is_last,
	.setup = beo_shape_node_setup,
	.teardown = ad24xx_node_teardown,
};

static int beo_shape_node_probe(struct device *dev)
{
	struct a2b_node *node = to_a2b_node(dev);
	int ret;

	node->ops = &beo_shape_node_ops;
	node->chip_info = of_device_get_match_data(dev);

	ret = a2b_register_node(node);
	if (ret)
		return ret;

	return 0;
}

static void beo_shape_node_remove(struct device *dev)
{
	struct a2b_node *node = to_a2b_node(dev);

	a2b_unregister_node(node);
}

static const struct of_device_id beo_shape_node_of_match_table[] = {
	{
		.compatible = "beo,shape-node",
		.data = &ad24xx_chip_info[A2B_AD2425],
	},
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, beo_shape_node_of_match_table);

static struct a2b_driver beo_shape_node_driver = {
	.driver = {
		.name = "beo-shape-node",
		.of_match_table = beo_shape_node_of_match_table,
		.probe_type = PROBE_PREFER_ASYNCHRONOUS,
	},
	.probe = beo_shape_node_probe,
	.remove = beo_shape_node_remove,
};
module_a2b_driver(beo_shape_node_driver);

MODULE_AUTHOR("Alvin Šipraga <alsi@bang-olufsen.dk>");
MODULE_DESCRIPTION("Beosound Shape A2B transceiver node driver");
MODULE_LICENSE("GPL");

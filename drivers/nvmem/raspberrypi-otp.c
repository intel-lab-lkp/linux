// SPDX-License-Identifier: GPL-2.0-only

#include <linux/module.h>
#include <linux/mod_devicetable.h>
#include <linux/nvmem-provider.h>
#include <linux/platform_device.h>
#include <linux/types.h>
#include <soc/bcm2835/raspberrypi-firmware.h>

struct rpi_otp_priv {
	struct rpi_firmware *fw;
	u32 read_tag;
	u32 write_tag;
};

struct rpi_otp_header {
	__le32 start;
	__le32 count;
	__le32 data[] __counted_by_le(count);
};

static int rpi_otp_read(void *context, unsigned int offset, void *buf, size_t bytes)
{
	struct rpi_otp_priv *priv = context;
	struct rpi_otp_header *fwbuf;
	u32 count;
	int ret;

	count = bytes / 4;

	fwbuf = kzalloc(struct_size(fwbuf, data, count), GFP_KERNEL);
	if (!fwbuf)
		return -ENOMEM;

	fwbuf->start = cpu_to_le32(offset / 4);
	fwbuf->count = cpu_to_le32(count);

	ret = rpi_firmware_property(priv->fw, priv->read_tag, fwbuf,
				    sizeof(struct rpi_otp_header) + bytes);
	if (ret)
		goto out;

	memcpy(buf, fwbuf->data, bytes);

out:
	kfree(fwbuf);
	return ret;
}

static int rpi_otp_write(void *context, unsigned int offset, void *val, size_t bytes)
{
	struct rpi_otp_priv *priv = context;
	struct rpi_otp_header *fwbuf;
	u32 count;
	int ret;

	count = bytes / 4;

	fwbuf = kzalloc(struct_size(fwbuf, data, count), GFP_KERNEL);
	if (!fwbuf)
		return -ENOMEM;

	fwbuf->start = cpu_to_le32(offset / 4);
	fwbuf->count = cpu_to_le32(count);
	memcpy(fwbuf->data, val, bytes);

	ret = rpi_firmware_property(priv->fw, priv->write_tag, fwbuf,
				    sizeof(struct rpi_otp_header) + bytes);

	kfree(fwbuf);
	return ret;
}

static int rpi_otp_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct nvmem_device *nvmem;
	struct rpi_otp_priv *priv;
	const struct rpi_otp_driver_data *data;
	struct nvmem_config config = {
		.read_only = false,
		.word_size = 4,
		.stride = 4,
		.reg_read = rpi_otp_read,
		.reg_write = rpi_otp_write,
		.id = NVMEM_DEVID_NONE,
	};

	data = dev_get_platdata(dev);
	if (!data)
		return -ENODEV;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->fw = dev_get_drvdata(dev->parent);
	priv->read_tag = data->read_tag;
	priv->write_tag = data->write_tag;
	config.dev = dev;
	config.priv = priv;
	config.name = data->name;
	config.size = data->size;
	config.root_only = data->root_only;

	nvmem = devm_nvmem_register(dev, &config);
	if (IS_ERR(nvmem))
		return dev_err_probe(dev, PTR_ERR(nvmem), "error registering nvmem config\n");

	return 0;
}

static const struct platform_device_id raspberrypi_otp_id[] = {
	{ "raspberrypi-otp" },
	{},
};
MODULE_DEVICE_TABLE(platform, raspberrypi_otp_id);

static struct platform_driver raspberrypi_otp_driver = {
	.probe	= rpi_otp_probe,
	.driver = {
		.name	= "raspberrypi-otp",
	},
	.id_table = raspberrypi_otp_id,
};
module_platform_driver(raspberrypi_otp_driver);

MODULE_AUTHOR("Gregor Herburger <gregor.herburger@linutronix.de>");
MODULE_DESCRIPTION("Raspberry Pi OTP driver");
MODULE_LICENSE("GPL");

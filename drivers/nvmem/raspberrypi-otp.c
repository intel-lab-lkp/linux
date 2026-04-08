// SPDX-License-Identifier: GPL-2.0-only

#include <linux/module.h>
#include <linux/nvmem-provider.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <soc/bcm2835/raspberrypi-firmware.h>

struct rpi_otp_priv {
	struct rpi_firmware *fw;
	struct device *dev;
	u32 read_tag;
	u32 write_tag;
};

struct rpi_otp_driver_data {
	const char *name;
	u32 read_tag;
	u32 write_tag;
};

struct rpi_otp_header {
	u32 start;
	u32 count;
	u32 data[];
};

static int rpi_otp_read(void *context, unsigned int offset, void *buf, size_t bytes)
{
	struct rpi_otp_priv *priv = context;
	struct rpi_otp_header *fwbuf;
	int ret;

	fwbuf = kmalloc(sizeof(struct rpi_otp_header) + bytes, GFP_KERNEL);
	if (!fwbuf)
		return -ENOMEM;

	fwbuf->start = offset / 4;
	fwbuf->count = bytes / 4;

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
	int ret;

	fwbuf = kmalloc(sizeof(struct rpi_otp_header) + bytes, GFP_KERNEL);
	if (!fwbuf)
		return -ENOMEM;

	fwbuf->start = offset / 4;
	fwbuf->count = bytes / 4;
	memcpy(fwbuf->data, val, bytes);

	ret = rpi_firmware_property(priv->fw, priv->write_tag, fwbuf,
				    sizeof(struct rpi_otp_header) + bytes);

	kfree(fwbuf);
	return ret;
}

static const struct rpi_otp_driver_data rpi_otp_customer = {
	.name = "rpi-otp-customer",
	.read_tag = RPI_FIRMWARE_GET_CUSTOMER_OTP,
	.write_tag = RPI_FIRMWARE_SET_CUSTOMER_OTP,
};

static const struct rpi_otp_driver_data rpi_otp_private = {
	.name = "rpi-otp-private",
	.read_tag = RPI_FIRMWARE_GET_PRIVATE_OTP,
	.write_tag = RPI_FIRMWARE_SET_PRIVATE_OTP,
};

static int rpi_otp_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct nvmem_device *nvmem;
	struct rpi_otp_priv *priv;
	struct device_node *np;
	const struct rpi_otp_driver_data *data;
	struct nvmem_config config = {
		.read_only = false,
		.word_size = 4,
		.stride = 4,
		.reg_read = rpi_otp_read,
		.reg_write = rpi_otp_write,
		.size = 32,
	};

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	data = device_get_match_data(dev);
	if (!data)
		return -ENODEV;

	np = of_get_parent(dev->of_node);
	if (!np) {
		dev_err(dev, "Missing firmware node\n");
		return -ENOENT;
	}

	priv->fw = devm_rpi_firmware_get(&pdev->dev, np);
	of_node_put(np);
	if (!priv->fw)
		return -EPROBE_DEFER;

	priv->dev = dev;
	priv->read_tag = data->read_tag;
	priv->write_tag = data->write_tag;
	config.dev = dev;
	config.priv = priv;
	config.name = data->name;

	nvmem = devm_nvmem_register(dev, &config);
	if (IS_ERR(nvmem))
		return dev_err_probe(dev, PTR_ERR(nvmem), "error registering nvmem config\n");

	return 0;
}

static const struct of_device_id rpi_otp_of_match[] = {
	{
		.compatible = "raspberrypi,firmware-otp-customer",
		.data = &rpi_otp_customer
	},
	{
		.compatible = "raspberrypi,firmware-otp-private",
		.data = &rpi_otp_private,
	},
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, rpi_otp_of_match);

static struct platform_driver raspberry_otp_driver = {
	.probe	= rpi_otp_probe,
	.driver = {
		.name	= "rpi-otp",
		.of_match_table = rpi_otp_of_match,
	},
};
module_platform_driver(raspberry_otp_driver);

MODULE_AUTHOR("Gregor Herburger <gregor.herburger@linutronix.de>");
MODULE_DESCRIPTION("Raspberry OTP driver");
MODULE_LICENSE("GPL");

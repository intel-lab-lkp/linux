// SPDX-License-Identifier: GPL-2.0-or-later

#include <linux/array_size.h>
#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/i2c.h>
#include <linux/interrupt.h>
#include <linux/mfd/asus-transformer-ec.h>
#include <linux/mfd/core.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/property.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/types.h>
#include <linux/unaligned.h>

#define ASUSEC_RSP_BUFFER_SIZE		8

struct asus_ec_chip_data {
	const char *name;
	const struct mfd_cell *mfd_devices;
	unsigned int num_devices;
	bool clr_fmode; /* clear Factory Mode bit in EC control register */
};

struct asus_ec_data {
	struct asusec_info info;
	struct mutex ecreq_lock; /* prevent simultaneous access */
	struct gpio_desc *ecreq;
	struct i2c_client *self;
	const struct asus_ec_chip_data *data;
	char ec_data[DOCKRAM_ENTRY_BUFSIZE];
	bool logging_disabled;
};

struct dockram_ec_data {
	struct mutex ctl_lock; /* prevent simultaneous access */
	char ctl_data[DOCKRAM_ENTRY_BUFSIZE];
};

#define to_ec_data(ec) \
	container_of(ec, struct asus_ec_data, info)

/**
 * asus_dockram_read - Read a register from the DockRAM device.
 * @client: Handle to the DockRAM device.
 * @reg: Register to read.
 * @buf: Byte array into which data will be read; must be large enough to
 *	 hold the data returned by the DockRAM.
 *
 * This executes the DockRAM read based on the SMBus "block read" protocol
 * or its emulation. It extracts DOCKRAM_ENTRY_SIZE bytes from the set
 * register address.
 *
 * Returns a negative errno code else zero on success.
 */
int asus_dockram_read(struct i2c_client *client, int reg, char *buf)
{
	struct device *dev = &client->dev;
	int ret;

	memset(buf, 0, DOCKRAM_ENTRY_BUFSIZE);
	ret = i2c_smbus_read_i2c_block_data(client, reg,
					    DOCKRAM_ENTRY_BUFSIZE, buf);
	if (ret < 0)
		return ret;

	if (buf[0] > DOCKRAM_ENTRY_SIZE) {
		dev_err(dev, "bad data len; buffer: %*ph; ret: %d\n",
			DOCKRAM_ENTRY_BUFSIZE, buf, ret);
		return -EPROTO;
	}

	dev_dbg(dev, "got data; buffer: %*ph; ret: %d\n",
		DOCKRAM_ENTRY_BUFSIZE, buf, ret);

	return 0;
}
EXPORT_SYMBOL_GPL(asus_dockram_read);

/**
 * asus_dockram_write - Write a byte array to a register of the DockRAM device.
 * @client: Handle to the DockRAM device.
 * @reg: Register to write to.
 * @buf: Byte array to be written (up to DOCKRAM_ENTRY_SIZE bytes).
 *
 * This executes the DockRAM write based on the SMBus "block write"
 * protocol or its emulation. It writes DOCKRAM_ENTRY_SIZE bytes to the
 * specified register address.
 *
 * Returns a negative errno code else zero on success.
 */
int asus_dockram_write(struct i2c_client *client, int reg, const char *buf)
{
	if (buf[0] > DOCKRAM_ENTRY_SIZE)
		return -EINVAL;

	dev_dbg(&client->dev, "sending data; buffer: %*ph\n", buf[0] + 1, buf);

	return i2c_smbus_write_i2c_block_data(client, reg, buf[0] + 1, buf);
}
EXPORT_SYMBOL_GPL(asus_dockram_write);

/**
 * asus_dockram_access_ctl - Read from or write to the DockRAM control register.
 * @client: Handle to the DockRAM device.
 * @out: Pointer to a variable where the register value will be stored.
 * @mask: Bitmask of bits to be cleared.
 * @xor: Bitmask of bits to be set (via XOR).
 *
 * This performs a control register read if @out is provided and both @mask
 * and @xor are zero. Otherwise, it performs a control register update if
 * @mask and @xor are provided.
 *
 * Returns a negative errno code else zero on success.
 */
int asus_dockram_access_ctl(struct i2c_client *client, u64 *out, u64 mask,
			    u64 xor)
{
	struct dockram_ec_data *priv = i2c_get_clientdata(client);
	char *buf = priv->ctl_data;
	u64 val;
	int ret = 0;

	guard(mutex)(&priv->ctl_lock);

	ret = asus_dockram_read(client, ASUSEC_DOCKRAM_CONTROL, buf);
	if (ret < 0)
		goto exit;

	if (buf[0] != ASUSEC_CTL_SIZE) {
		ret = -EPROTO;
		goto exit;
	}

	val = get_unaligned_le64(buf + 1);

	if (out)
		*out = val;

	if (mask || xor) {
		put_unaligned_le64((val & ~mask) ^ xor, buf + 1);
		ret = asus_dockram_write(client, ASUSEC_DOCKRAM_CONTROL, buf);
	}

exit:
	if (ret < 0)
		dev_err(&client->dev, "Failed to access control flags: %d\n",
			ret);

	return ret;
}
EXPORT_SYMBOL_GPL(asus_dockram_access_ctl);

static void asus_ec_remove_notifier(struct device *dev, void *res)
{
	struct asusec_info *ec = dev_get_drvdata(dev->parent);
	struct notifier_block **nb = res;

	blocking_notifier_chain_unregister(&ec->notify_list, *nb);
}

/**
 * devm_asus_ec_register_notifier - Managed registration of notifier to an
 *				    ASUS EC blocking notifier chain.
 * @pdev: Device requesting the notifier (used for resource management).
 * @nb: Notifier block to be registered.
 *
 * Register a notifier to the ASUS EC blocking notifier chain. The notifier
 * will be automatically unregistered when the requesting device is detached.
 *
 * Return: 0 on success or a negative error code on failure.
 */
int devm_asus_ec_register_notifier(struct platform_device *pdev,
				   struct notifier_block *nb)
{
	struct asusec_info *ec = dev_get_drvdata(pdev->dev.parent);
	struct notifier_block **res;
	int ret;

	res = devres_alloc(asus_ec_remove_notifier, sizeof(*res), GFP_KERNEL);
	if (!res)
		return -ENOMEM;

	*res = nb;
	ret = blocking_notifier_chain_register(&ec->notify_list, nb);
	if (ret) {
		devres_free(res);
		return ret;
	}

	devres_add(&pdev->dev, res);

	return 0;
}
EXPORT_SYMBOL_GPL(devm_asus_ec_register_notifier);

static int asus_ec_signal_request(const struct asusec_info *ec)
{
	struct asus_ec_data *priv = to_ec_data(ec);

	guard(mutex)(&priv->ecreq_lock);

	dev_dbg(&priv->self->dev, "EC request\n");

	gpiod_set_value_cansleep(priv->ecreq, 1);
	msleep(50);

	gpiod_set_value_cansleep(priv->ecreq, 0);
	msleep(200);

	return 0;
}

static int asus_ec_write(struct asus_ec_data *priv, u16 data)
{
	int ret = i2c_smbus_write_word_data(priv->self, ASUSEC_WRITE_BUF, data);

	dev_dbg(&priv->self->dev, "EC write: %04x, ret = %d\n", data, ret);
	return ret;
}

static int asus_ec_read(struct asus_ec_data *priv, bool in_irq)
{
	int ret = i2c_smbus_read_i2c_block_data(priv->self, ASUSEC_READ_BUF,
						sizeof(priv->ec_data),
						priv->ec_data);

	dev_dbg(&priv->self->dev, "EC read: %*ph, ret = %d%s\n",
		sizeof(priv->ec_data), priv->ec_data,
		ret, in_irq ? "; in irq" : "");

	return ret;
}

/**
 * asus_ec_i2c_command - Send a 16-bit command to the ASUS EC.
 * @ec: Pointer to the shared ASUS EC structure.
 * @data: The 16-bit command (word) to be sent.
 *
 * Return: 0 on success or a negative error code on failure.
 */
int asus_ec_i2c_command(const struct asusec_info *ec, u16 data)
{
	return asus_ec_write(to_ec_data(ec), data);
}
EXPORT_SYMBOL_GPL(asus_ec_i2c_command);

static void asus_ec_clear_buffer(struct asus_ec_data *priv)
{
	int retry = ASUSEC_RSP_BUFFER_SIZE;

	while (retry--) {
		if (asus_ec_read(priv, false) < 0)
			continue;

		if (priv->ec_data[1] & ASUSEC_OBF_MASK)
			continue;

		break;
	}
}

static int asus_ec_log_info(struct asus_ec_data *priv, unsigned int reg,
			    const char *name, char **out)
{
	char buf[DOCKRAM_ENTRY_BUFSIZE];
	int ret;

	ret = asus_dockram_read(priv->info.dockram, reg, buf);
	if (ret < 0)
		return ret;

	if (!priv->logging_disabled)
		dev_info(&priv->self->dev, "%-14s: %.*s\n", name,
			 buf[0], buf + 1);

	if (out)
		*out = kstrndup(buf + 1, buf[0], GFP_KERNEL);

	return 0;
}

static int asus_ec_reset(struct asus_ec_data *priv)
{
	int retry, ret;

	for (retry = 0; retry < 3; retry++) {
		ret = asus_ec_write(priv, 0);
		if (!ret)
			return 0;

		msleep(300);
	}

	return ret;
}

static int asus_ec_magic_debug(struct asus_ec_data *priv)
{
	u64 flag;
	int ret;

	ret = asus_ec_get_ctl(&priv->info, &flag);
	if (ret < 0)
		return ret;

	flag &= ASUSEC_CTL_SUSB_MODE;
	dev_info(&priv->self->dev, "EC FW behaviour: %s\n",
		 flag ? "susb on when receive ec_req" :
		 "susb on when system wakeup");

	return 0;
}

static int asus_ec_set_factory_mode(struct asus_ec_data *priv, bool on)
{
	dev_info(&priv->self->dev, "Entering %s mode.\n", on ? "factory" :
		 "normal");
	return asus_ec_update_ctl(&priv->info, ASUSEC_CTL_FACTORY_MODE,
				  on ? ASUSEC_CTL_FACTORY_MODE : 0);
}

static void asus_ec_handle_smi(struct asus_ec_data *priv, unsigned int code);

static irqreturn_t asus_ec_interrupt(int irq, void *dev_id)
{
	struct asus_ec_data *priv = dev_id;
	unsigned long notify_action;
	int ret;

	ret = asus_ec_read(priv, true);
	if (ret <= 0 || !(priv->ec_data[1] & ASUSEC_OBF_MASK))
		return IRQ_NONE;

	notify_action = priv->ec_data[1];
	if (notify_action & ASUSEC_SMI_MASK) {
		unsigned int code = priv->ec_data[2];

		asus_ec_handle_smi(priv, code);

		notify_action |= code << 8;
		dev_dbg(&priv->self->dev, "SMI code: 0x%02x\n", code);
	}

	blocking_notifier_call_chain(&priv->info.notify_list,
				     notify_action, priv->ec_data);

	return IRQ_HANDLED;
}

static int asus_ec_detect(struct asus_ec_data *priv)
{
	char *model = NULL;
	int ret;

	ret = asus_ec_reset(priv);
	if (ret)
		goto err_exit;

	asus_ec_clear_buffer(priv);

	ret = asus_ec_log_info(priv, ASUSEC_DOCKRAM_INFO_MODEL, "model", &model);
	if (ret)
		goto err_exit;

	ret = asus_ec_log_info(priv, ASUSEC_DOCKRAM_INFO_FW, "FW version", NULL);
	if (ret)
		goto err_exit;

	ret = asus_ec_log_info(priv, ASUSEC_DOCKRAM_INFO_CFGFMT, "Config format", NULL);
	if (ret)
		goto err_exit;

	ret = asus_ec_log_info(priv, ASUSEC_DOCKRAM_INFO_HW, "HW version", NULL);
	if (ret)
		goto err_exit;

	priv->logging_disabled = true;

	ret = asus_ec_magic_debug(priv);
	if (ret)
		goto err_exit;

	priv->info.model = model;
	priv->info.name = priv->data->name;

	if (priv->data->clr_fmode)
		asus_ec_set_factory_mode(priv, false);

err_exit:
	if (ret)
		dev_err(&priv->self->dev, "failed to access EC: %d\n", ret);

	return ret;
}

static void asus_ec_handle_smi(struct asus_ec_data *priv, unsigned int code)
{
	dev_dbg(&priv->self->dev, "SMI interrupt: 0x%02x\n", code);

	switch (code) {
	case ASUSEC_SMI_HANDSHAKE:
	case ASUSEC_SMI_RESET:
		asus_ec_detect(priv);
		break;
	}
}

static ssize_t dockram_read(struct file *filp, char __user *buf,
			    size_t count, loff_t *ppos)
{
	struct i2c_client *client = filp->private_data;
	unsigned int reg, rsize;
	ssize_t n_read = 0, val;
	loff_t off = *ppos;
	char *data;
	int ret;

	reg = off / DOCKRAM_ENTRY_SIZE;
	off %= DOCKRAM_ENTRY_SIZE;
	rsize = DOCKRAM_ENTRIES * DOCKRAM_ENTRY_SIZE;

	if (!count)
		return 0;

	data = kmalloc(DOCKRAM_ENTRY_BUFSIZE, GFP_KERNEL);

	while (reg < DOCKRAM_ENTRIES) {
		unsigned int len = DOCKRAM_ENTRY_SIZE - off;

		if (len > rsize)
			len = rsize;

		ret = asus_dockram_read(client, reg, data);
		if (ret < 0) {
			if (!n_read)
				n_read = ret;
			break;
		}

		val = copy_to_user(buf, data + 1 + off, len);
		if (val == len)
			return -EFAULT;

		*ppos += len;
		n_read += len;

		if (len == rsize)
			break;

		rsize -= len;
		buf += len;
		off = 0;
		++reg;
	}

	kfree(data);

	return n_read;
}

static int dockram_write_one(struct i2c_client *client, int reg,
			     const char __user *buf, size_t count)
{
	struct dockram_ec_data *priv = i2c_get_clientdata(client);
	int ret;

	if (!count || count > DOCKRAM_ENTRY_SIZE)
		return -EINVAL;
	if (buf[0] != count - 1)
		return -EINVAL;

	guard(mutex)(&priv->ctl_lock);

	priv->ctl_data[0] = (u8)count;
	memcpy(priv->ctl_data + 1, buf, count);
	ret = asus_dockram_write(client, reg, priv->ctl_data);

	return ret;
}

static ssize_t dockram_write(struct file *filp, const char __user *buf,
			     size_t count, loff_t *ppos)
{
	struct i2c_client *client = filp->private_data;
	unsigned int reg;
	loff_t off = *ppos;
	int ret;

	if (off % DOCKRAM_ENTRY_SIZE != 0)
		return -EINVAL;

	reg = off / DOCKRAM_ENTRY_SIZE;
	if (reg >= DOCKRAM_ENTRIES)
		return -EINVAL;

	ret = dockram_write_one(client, reg, buf, count);

	return ret < 0 ? ret : count;
}

static const struct debugfs_short_fops dockram_fops = {
	.read	= dockram_read,
	.write	= dockram_write,
	.llseek	= default_llseek,
};

static int control_reg_get(void *client, u64 *val)
{
	return asus_dockram_access_ctl(client, val, 0, 0);
}

static int control_reg_set(void *client, u64 val)
{
	return asus_dockram_access_ctl(client, NULL, ~0ull, val);
}

DEFINE_DEBUGFS_ATTRIBUTE(control_reg_fops, control_reg_get,
			 control_reg_set, "%016llx\n");

static int ec_request_set(void *ec, u64 val)
{
	if (val)
		asus_ec_signal_request(ec);

	return 0;
}

DEFINE_DEBUGFS_ATTRIBUTE(ec_request_fops, NULL, ec_request_set, "%llu\n");

static int ec_irq_set(void *ec, u64 val)
{
	struct asus_ec_data *priv = to_ec_data(ec);

	if (val)
		irq_wake_thread(priv->self->irq, priv);

	return 0;
}

DEFINE_DEBUGFS_ATTRIBUTE(ec_irq_fops, NULL, ec_irq_set, "%llu\n");

static void asus_ec_debugfs_remove(void *debugfs_root)
{
	debugfs_remove_recursive(debugfs_root);
}

static void devm_asus_ec_debugfs_init(struct device *dev)
{
	struct asusec_info *ec = dev_get_drvdata(dev);
	struct asus_ec_data *priv = to_ec_data(ec);
	struct dentry *debugfs_root, *dockram_dir;
	char *name = devm_kasprintf(dev, GFP_KERNEL, "asus-ec-%s",
				    priv->data->name);

	debugfs_root = debugfs_create_dir(name, NULL);
	dockram_dir = debugfs_create_dir("dockram", debugfs_root);

	debugfs_create_file("ec_irq", 0200, debugfs_root, ec,
			    &ec_irq_fops);
	debugfs_create_file("ec_request", 0200, debugfs_root, ec,
			    &ec_request_fops);
	debugfs_create_file("control_reg", 0644, dockram_dir,
			    priv->info.dockram, &control_reg_fops);
	debugfs_create_file("dockram", 0644, dockram_dir,
			    priv->info.dockram, &dockram_fops);

	devm_add_action_or_reset(dev, asus_ec_debugfs_remove, debugfs_root);
}

static void asus_ec_release_dockram_dev(void *client)
{
	i2c_unregister_device(client);
}

static struct i2c_client *devm_asus_dockram_get(struct device *dev)
{
	struct i2c_client *parent = to_i2c_client(dev);
	struct i2c_client *dockram;
	struct dockram_ec_data *priv;
	int ret;

	dockram = i2c_new_ancillary_device(parent, "dockram",
					   parent->addr + 2);
	if (IS_ERR(dockram))
		return dockram;

	ret = devm_add_action_or_reset(dev, asus_ec_release_dockram_dev,
				       dockram);
	if (ret)
		return ERR_PTR(ret);

	priv = devm_kzalloc(&dockram->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return ERR_PTR(-ENOMEM);

	i2c_set_clientdata(dockram, priv);
	mutex_init(&priv->ctl_lock);

	return dockram;
}

static int asus_ec_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct asus_ec_data *priv;
	int ret;

	if (!i2c_check_functionality(client->adapter, I2C_FUNC_SMBUS_I2C_BLOCK))
		return dev_err_probe(dev, -ENXIO,
			"I2C bus is missing required SMBus block mode support\n");

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->data = device_get_match_data(dev);
	if (!priv->data)
		return -ENODEV;

	i2c_set_clientdata(client, priv);
	priv->self = client;

	priv->info.dockram = devm_asus_dockram_get(dev);
	if (IS_ERR(priv->info.dockram))
		return dev_err_probe(dev, PTR_ERR(priv->info.dockram),
				     "failed to get dockram\n");

	priv->ecreq = devm_gpiod_get(dev, "request", GPIOD_OUT_LOW);
	if (IS_ERR(priv->ecreq))
		return dev_err_probe(dev, PTR_ERR(priv->ecreq),
				     "failed to get request GPIO\n");

	BLOCKING_INIT_NOTIFIER_HEAD(&priv->info.notify_list);
	mutex_init(&priv->ecreq_lock);

	asus_ec_signal_request(&priv->info);

	ret = asus_ec_detect(priv);
	if (ret)
		return dev_err_probe(dev, ret, "failed to detect EC version\n");

	ret = devm_request_threaded_irq(dev, client->irq, NULL,
					&asus_ec_interrupt,
					IRQF_ONESHOT | IRQF_SHARED,
					client->name, priv);
	if (ret)
		return dev_err_probe(dev, ret, "failed to register IRQ\n");

	/* Parent I2C controller uses DMA, ASUS EC and child devices do not */
	client->dev.coherent_dma_mask = 0;
	client->dev.dma_mask = &client->dev.coherent_dma_mask;

	if (IS_ENABLED(CONFIG_DEBUG_FS))
		devm_asus_ec_debugfs_init(dev);

	return devm_mfd_add_devices(dev, 0, priv->data->mfd_devices,
				    priv->data->num_devices, NULL, 0, NULL);
}

static const struct mfd_cell asus_ec_sl101_dock_mfd_devices[] = {
	{
		.name = "asus-transformer-ec-kbc",
	},
};

static const struct asus_ec_chip_data asus_ec_sl101_dock_data = {
	.name = "dock",
	.mfd_devices = asus_ec_sl101_dock_mfd_devices,
	.num_devices = ARRAY_SIZE(asus_ec_sl101_dock_mfd_devices),
	.clr_fmode = false,
};

static const struct mfd_cell asus_ec_tf101_dock_mfd_devices[] = {
	{
		.name = "asus-transformer-ec-battery",
		.id = 1,
	}, {
		.name = "asus-transformer-ec-charger",
		.id = 1,
	}, {
		.name = "asus-transformer-ec-led",
		.id = 1,
	}, {
		.name = "asus-transformer-ec-keys",
	}, {
		.name = "asus-transformer-ec-kbc",
	},
};

static const struct asus_ec_chip_data asus_ec_tf101_dock_data = {
	.name = "dock",
	.mfd_devices = asus_ec_tf101_dock_mfd_devices,
	.num_devices = ARRAY_SIZE(asus_ec_tf101_dock_mfd_devices),
	.clr_fmode = false,
};

static const struct mfd_cell asus_ec_tf201_pad_mfd_devices[] = {
	{
		.name = "asus-transformer-ec-battery",
		.id = 0,
	}, {
		.name = "asus-transformer-ec-led",
		.id = 0,
	},
};

static const struct asus_ec_chip_data asus_ec_tf201_pad_data = {
	.name = "pad",
	.mfd_devices = asus_ec_tf201_pad_mfd_devices,
	.num_devices = ARRAY_SIZE(asus_ec_tf201_pad_mfd_devices),
	.clr_fmode = true,
};

static const struct mfd_cell asus_ec_tf600t_pad_mfd_devices[] = {
	{
		.name = "asus-transformer-ec-battery",
		.id = 0,
	}, {
		.name = "asus-transformer-ec-charger",
		.id = 0,
	}, {
		.name = "asus-transformer-ec-led",
		.id = 0,
	},
};

static const struct asus_ec_chip_data asus_ec_tf600t_pad_data = {
	.name = "pad",
	.mfd_devices = asus_ec_tf600t_pad_mfd_devices,
	.num_devices = ARRAY_SIZE(asus_ec_tf600t_pad_mfd_devices),
	.clr_fmode = true,
};

static const struct of_device_id asus_ec_match[] = {
	{ .compatible = "asus,sl101-ec-dock", .data = &asus_ec_sl101_dock_data },
	{ .compatible = "asus,tf101-ec-dock", .data = &asus_ec_tf101_dock_data },
	{ .compatible = "asus,tf201-ec-pad", .data = &asus_ec_tf201_pad_data },
	{ .compatible = "asus,tf600t-ec-pad", .data = &asus_ec_tf600t_pad_data },
	{ }
};
MODULE_DEVICE_TABLE(of, asus_ec_match);

static struct i2c_driver asus_ec_driver = {
	.driver	= {
		.name = "asus-transformer-ec",
		.of_match_table = asus_ec_match,
	},
	.probe = asus_ec_probe,
};
module_i2c_driver(asus_ec_driver);

MODULE_AUTHOR("Michał Mirosław <mirq-linux@rere.qmqm.pl>");
MODULE_AUTHOR("Svyatoslav Ryhel <clamor95@gmail.com>");
MODULE_DESCRIPTION("ASUS Transformer's EC driver");
MODULE_LICENSE("GPL");

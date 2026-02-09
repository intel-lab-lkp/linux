// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * ASUS EC: DockRAM
 */

#include <linux/debugfs.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/i2c.h>
#include <linux/mfd/asus-ec.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/sysfs.h>
#include <linux/types.h>
#include <linux/unaligned.h>

struct dockram_ec_data {
	struct mutex ctl_lock; /* prevent simultaneous access */
	char ctl_data[DOCKRAM_ENTRY_BUFSIZE];
};

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
	ret = i2c_smbus_read_i2c_block_data(client, reg, DOCKRAM_ENTRY_BUFSIZE, buf);
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
 * This executes the DockRAM write based on the SMBus "block write" protocol
 * or its emulation. It writes DOCKRAM_ENTRY_SIZE bytes to the specified
 * register address.
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

DEFINE_DEBUGFS_ATTRIBUTE(control_reg_fops, control_reg_get, control_reg_set, "%016llx\n");

/**
 * asus_dockram_debugfs_init - Register DockRAM debugfs.
 * @client: Handle to the DockRAM device.
 * @debugfs_root: Pointer to the root directory dentry.
 *
 * Creates a debugfs setup for DockRAM in the directory of the linked EC.
 * The debugfs setup must be removed by the caller.
 *
 * Return: 0 on success or a negative error code on failure.
 */
void asus_dockram_debugfs_init(struct i2c_client *client, struct dentry *debugfs_root)
{
	struct dentry *dockram_dir;

	dockram_dir = debugfs_create_dir("dockram", debugfs_root);

	debugfs_create_file("control_reg", 0644, dockram_dir, client, &control_reg_fops);
	debugfs_create_file("dockram", 0644, dockram_dir, client, &dockram_fops);
}
EXPORT_SYMBOL_GPL(asus_dockram_debugfs_init);

static int asus_dockram_probe(struct i2c_client *client)
{
	struct dockram_ec_data *priv;
	struct device *dev = &client->dev;

	if (!i2c_check_functionality(client->adapter, I2C_FUNC_SMBUS_I2C_BLOCK))
		return dev_err_probe(dev, -ENXIO,
			"I2C bus is missing required SMBus block mode support\n");

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	i2c_set_clientdata(client, priv);
	mutex_init(&priv->ctl_lock);

	return 0;
}

static const struct of_device_id asus_dockram_ids[] = {
	{ .compatible = "asus,dockram" },
	{ }
};
MODULE_DEVICE_TABLE(of, asus_dockram_ids);

static struct i2c_driver asus_dockram_driver = {
	.driver	= {
		.name = "asus-dockram",
		.of_match_table = of_match_ptr(asus_dockram_ids),
	},
	.probe = asus_dockram_probe,
};
module_i2c_driver(asus_dockram_driver);

static void devm_i2c_device_release(struct device *dev, void *res)
{
	struct i2c_client **pdev = res;
	struct i2c_client *child = *pdev;

	if (child)
		put_device(&child->dev);
}

static struct i2c_client *devm_i2c_device_get_by_phandle(struct device *dev,
							 const char *name,
							 int index)
{
	struct device_node *np;
	struct i2c_client **pdev;

	pdev = devres_alloc(devm_i2c_device_release, sizeof(*pdev),
			    GFP_KERNEL);
	if (!pdev)
		return ERR_PTR(-ENOMEM);

	np = of_parse_phandle(dev_of_node(dev), name, index);
	if (!np) {
		devres_free(pdev);
		dev_err(dev, "can't resolve phandle %s: %d\n", name, index);
		return ERR_PTR(-ENODEV);
	}

	*pdev = of_find_i2c_device_by_node(np);
	of_node_put(np);

	if (!*pdev) {
		devres_free(pdev);
		return ERR_PTR(-EPROBE_DEFER);
	}

	devres_add(dev, pdev);

	return *pdev;
}

/**
 * devm_asus_dockram_get - Device-managed request of DockRAM via device phandle.
 * @parent: Parent device which requests the DockRAM device.
 *
 * Request the DockRAM device by phandle from the parent's device-tree node.
 * The DockRAM device phandle will be automatically released and DockRAM will be
 * detached on parent driver detach.
 *
 * Return: Pointer to the DockRAM I2C device on success, or an ERR_PTR on failure.
 */
struct i2c_client *devm_asus_dockram_get(struct device *parent)
{
	struct i2c_client *dockram =
		devm_i2c_device_get_by_phandle(parent, "asus,dockram", 0);

	if (IS_ERR(dockram))
		return dockram;
	if (!dockram->dev.driver)
		return ERR_PTR(-EPROBE_DEFER);
	if (dockram->dev.driver != &asus_dockram_driver.driver)
		return ERR_PTR(-EBUSY);

	return dockram;
}
EXPORT_SYMBOL_GPL(devm_asus_dockram_get);

MODULE_AUTHOR("Michał Mirosław <mirq-linux@rere.qmqm.pl>");
MODULE_DESCRIPTION("ASUS Transformer's dockram driver");
MODULE_LICENSE("GPL");

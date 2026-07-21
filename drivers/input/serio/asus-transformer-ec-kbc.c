// SPDX-License-Identifier: GPL-2.0-or-later

#include <linux/err.h>
#include <linux/i2c.h>
#include <linux/i8042.h>
#include <linux/mfd/asus-transformer-ec.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/serio.h>

struct asus_ec_kbc_data {
	struct notifier_block nb;
	struct asusec_core *ec;
	struct i2c_client *parent;
	struct serio *sdev[2];
};

static int asus_ec_kbc_notify(struct notifier_block *nb,
			      unsigned long action, void *data_)
{
	struct asus_ec_kbc_data *priv = container_of(nb, struct asus_ec_kbc_data, nb);
	unsigned int port_idx, n;
	u8 *data = data_;

	if (action & (ASUSEC_SMI_MASK | ASUSEC_SCI_MASK))
		return NOTIFY_DONE;
	else if (action & ASUSEC_AUX_MASK)
		port_idx = 1;
	else if (action & (ASUSEC_KBC_MASK | ASUSEC_KEY_MASK))
		port_idx = 0;
	else
		return NOTIFY_DONE;

	/*
	 * The data[0] is the length of the packet including itself. The data[]
	 * buffer has to be at least 3 bytes (length + ctrl + 1 data byte) and
	 * must not exceed the EC entry size.
	 */
	if (data[0] < 2 || data[0] > ASUSEC_ENTRY_SIZE)
		return NOTIFY_BAD;

	n = data[0] - 2;
	data += 2;

	if (port_idx == 0) {
		/*
		 * Remap keyboard key codes to match AT layout:
		 * SEARCH: RIGHT-META [E0 27] -> LEFT-ALT   [11]
		 * MENU:   COMPOSE    [E0 2F] -> RIGHT-META [E0 27]
		 */
		if ((n == 2 || (n == 3 && data[1] == 0xF0)) && data[0] == 0xE0) {
			u8 *keycode = &data[n - 1];

			switch (*keycode) {
			case 0x27:
				*keycode = 0x11;
				++data;
				--n;
				break;
			case 0x2F:
				*keycode = 0x27;
				break;
			}
		}
	}

	while (n--)
		serio_interrupt(priv->sdev[port_idx], *data++, 0);

	return NOTIFY_OK;
}

static int asus_ec_serio_write(struct serio *port, unsigned char data)
{
	struct asus_ec_kbc_data *priv = port->port_data;

	return i2c_smbus_write_word_data(priv->parent, ASUSEC_WRITE_BUF,
					 (data << 8) | port->id.extra);
}

static void asus_ec_serio_remove(void *data)
{
	serio_unregister_port(data);
}

static int asus_ec_register_serio(struct platform_device *pdev, int idx,
				  const char *name, int cmd)
{
	struct asus_ec_kbc_data *priv = platform_get_drvdata(pdev);
	struct i2c_client *parent = priv->parent;
	struct serio *port = kzalloc_obj(*port);

	if (!port)
		return -ENOMEM;

	priv->sdev[idx] = port;
	port->dev.parent = &pdev->dev;
	port->id.type = SERIO_8042;
	port->id.extra = cmd & 0xFF;
	port->write = asus_ec_serio_write;
	port->port_data = (void *)priv;
	snprintf(port->name, sizeof(port->name), "%s %s",
		 priv->ec->model, name);
	snprintf(port->phys, sizeof(port->phys), "i2c-%u-%04x/serio%d",
		 i2c_adapter_id(parent->adapter), parent->addr, idx);

	serio_register_port(port);

	return devm_add_action_or_reset(&pdev->dev, asus_ec_serio_remove, port);
}

static void asus_ec_notifier_chain_unregister(void *data)
{
	struct asus_ec_kbc_data *priv = data;
	struct asusec_core *ec = priv->ec;

	blocking_notifier_chain_unregister(&ec->notify_list, &priv->nb);
}

static int asus_ec_kbc_probe(struct platform_device *pdev)
{
	struct asusec_core *ec = dev_get_drvdata(pdev->dev.parent);
	struct asus_ec_kbc_data *priv;
	int error;

	priv = devm_kzalloc(&pdev->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	platform_set_drvdata(pdev, priv);

	priv->ec = ec;
	priv->parent = to_i2c_client(pdev->dev.parent);

	priv->nb.notifier_call = asus_ec_kbc_notify;

	error = blocking_notifier_chain_register(&ec->notify_list, &priv->nb);
	if (error)
		return dev_err_probe(&pdev->dev, error,
				     "failed to register blocking notifier chain");

	error = devm_add_action_or_reset(&pdev->dev,
					 asus_ec_notifier_chain_unregister,
					 priv);
	if (error)
		return error;

	error = asus_ec_register_serio(pdev, 0, "Keyboard", 0);
	if (error)
		return error;

	error = asus_ec_register_serio(pdev, 1, "Touchpad", I8042_CMD_AUX_SEND);
	if (error)
		return error;

	return 0;
}

static struct platform_driver asus_ec_kbc_driver = {
	.driver.name = "asus-transformer-ec-kbc",
	.probe = asus_ec_kbc_probe,
};
module_platform_driver(asus_ec_kbc_driver);

MODULE_ALIAS("platform:asus-transformer-ec-kbc");
MODULE_AUTHOR("Michał Mirosław <mirq-linux@rere.qmqm.pl>");
MODULE_DESCRIPTION("ASUS Transformer's Dock keyboard and touchpad driver");
MODULE_LICENSE("GPL");

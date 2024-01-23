// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright 2023 NXP.
 *
 * Author: Frank Li <Frank.Li@nxp.com>
 */

#include <linux/delay.h>
#include <linux/kernel.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/i3c/device.h>
#include <linux/i3c/master.h>
#include <linux/slab.h>
#include <linux/console.h>
#include <linux/serial_core.h>
#include <linux/interrupt.h>
#include <linux/workqueue.h>
#include <linux/tty_flip.h>

static DEFINE_IDR(i3c_tty_minors);
static DEFINE_MUTEX(i3c_tty_minors_lock);

static struct tty_driver *i3c_tty_driver;

#define I3C_TTY_MINORS		8
#define I3C_TTY_TRANS_SIZE	16
#define I3C_TTY_RX_STOP		0
#define I3C_TTY_RETRY		20
#define I3C_TTY_YIELD_US	100

struct ttyi3c_port {
	struct tty_port port;
	int minor;
	spinlock_t xlock; /* protect xmit */
	u8 tx_buff[I3C_TTY_TRANS_SIZE];
	u8 rx_buff[I3C_TTY_TRANS_SIZE];
	struct i3c_device *i3cdev;
	struct work_struct txwork;
	struct work_struct rxwork;
	struct completion txcomplete;
	unsigned long status;
	u32 buf_overrun;
};

static const struct i3c_device_id i3c_ids[] = {
	I3C_DEVICE(0x011B, 0x1000, NULL),
	{ /* sentinel */ },
};

static int i3c_port_activate(struct tty_port *port, struct tty_struct *tty)
{
	struct ttyi3c_port *sport = container_of(port, struct ttyi3c_port, port);
	int ret;

	ret = tty_port_alloc_xmit_buf(port);
	if (ret < 0)
		return ret;

	sport->status = 0;

	ret = i3c_device_enable_ibi(sport->i3cdev);
	if (ret) {
		tty_port_free_xmit_buf(port);
		return ret;
	}

	return 0;
}

static void i3c_port_shutdown(struct tty_port *port)
{
	struct ttyi3c_port *sport =
		container_of(port, struct ttyi3c_port, port);

	i3c_device_disable_ibi(sport->i3cdev);
	tty_port_free_xmit_buf(port);
}

static void i3c_port_destruct(struct tty_port *port)
{
	struct ttyi3c_port *sport =
		container_of(port, struct ttyi3c_port, port);

	mutex_lock(&i3c_tty_minors_lock);
	idr_remove(&i3c_tty_minors, sport->minor);
	mutex_unlock(&i3c_tty_minors_lock);
}

static const struct tty_port_operations i3c_port_ops = {
	.shutdown = i3c_port_shutdown,
	.activate = i3c_port_activate,
	.destruct = i3c_port_destruct,
};

static ssize_t i3c_write(struct tty_struct *tty, const unsigned char *buf, size_t count)
{
	struct ttyi3c_port *sport = tty->driver_data;
	unsigned long flags;
	bool is_empty;
	int ret;

	spin_lock_irqsave(&sport->xlock, flags);
	ret = kfifo_in(&sport->port.xmit_fifo, buf, count);
	is_empty = kfifo_is_empty(&sport->port.xmit_fifo);
	spin_unlock_irqrestore(&sport->xlock, flags);

	if (!is_empty)
		queue_work(system_unbound_wq, &sport->txwork);

	return ret;
}

static int i3c_put_char(struct tty_struct *tty, unsigned char ch)
{
	struct ttyi3c_port *sport = tty->driver_data;
	unsigned long flags;
	int ret;

	spin_lock_irqsave(&sport->xlock, flags);
	ret = kfifo_put(&sport->port.xmit_fifo, ch);
	spin_unlock_irqrestore(&sport->xlock, flags);

	return ret;
}

static void i3c_flush_chars(struct tty_struct *tty)
{
	struct ttyi3c_port *sport = tty->driver_data;

	queue_work(system_unbound_wq, &sport->txwork);
}

static unsigned int i3c_write_room(struct tty_struct *tty)
{
	struct ttyi3c_port *sport = tty->driver_data;

	return kfifo_avail(&sport->port.xmit_fifo);
}

static void i3c_throttle(struct tty_struct *tty)
{
	struct ttyi3c_port *sport = tty->driver_data;

	clear_bit(I3C_TTY_RX_STOP, &sport->status);
}

static void i3c_unthrottle(struct tty_struct *tty)
{
	struct ttyi3c_port *sport = tty->driver_data;

	set_bit(I3C_TTY_RX_STOP, &sport->status);

	queue_work(system_unbound_wq, &sport->rxwork);
}

static int i3c_open(struct tty_struct *tty, struct file *filp)
{
	struct ttyi3c_port *sport = container_of(tty->port, struct ttyi3c_port, port);

	tty->driver_data = sport;

	return tty_port_open(&sport->port, tty, filp);
}

static void i3c_close(struct tty_struct *tty, struct file *filp)
{
	tty_port_close(tty->port, tty, filp);
}

static const struct tty_operations i3c_tty_ops = {
	.open = i3c_open,
	.close = i3c_close,
	.write = i3c_write,
	.put_char = i3c_put_char,
	.flush_chars = i3c_flush_chars,
	.write_room = i3c_write_room,
	.throttle = i3c_throttle,
	.unthrottle = i3c_unthrottle,
};

static void i3c_controller_irq_handler(struct i3c_device *dev,
				       const struct i3c_ibi_payload *payload)
{
	struct ttyi3c_port *sport = dev_get_drvdata(&dev->dev);

	/* i3c_unthrottle also queue the work to fetch pending data in target side */
	queue_work(system_unbound_wq, &sport->rxwork);
}

static void tty_i3c_rxwork(struct work_struct *work)
{
	struct ttyi3c_port *sport = container_of(work, struct ttyi3c_port, rxwork);
	struct i3c_priv_xfer xfers;
	u32 retry = I3C_TTY_RETRY;
	u16 status = BIT(0);
	int ret;

	memset(&xfers, 0, sizeof(xfers));
	xfers.data.in = sport->rx_buff;
	xfers.len = I3C_TTY_TRANS_SIZE;
	xfers.rnw = 1;

	do {
		if (test_bit(I3C_TTY_RX_STOP, &sport->status))
			break;

		i3c_device_do_priv_xfers(sport->i3cdev, &xfers, 1);

		if (xfers.actual_len) {
			ret = tty_insert_flip_string(&sport->port, sport->rx_buff,
						     xfers.actual_len);
			if (ret < xfers.actual_len)
				sport->buf_overrun++;

			retry = I3C_TTY_RETRY;
			continue;
		}

		status = BIT(0);
		i3c_device_getstatus_format1(sport->i3cdev, &status);
		/*
		 * Target side needs some time to fill data into fifo. Target side may not
		 * have hardware update status in real time. Software update status always
		 * needs some delays.
		 *
		 * Generally, target side have circular buffer in memory, it will be moved
		 * into FIFO by CPU or DMA. 'status' just show if circular buffer empty. But
		 * there are gap, especially CPU have not response irq to fill FIFO in time.
		 * So xfers.actual will be zero, wait for little time to avoid flood
		 * transfer in i3c bus.
		 */
		usleep_range(I3C_TTY_YIELD_US, 10 * I3C_TTY_YIELD_US);
		retry--;

	} while (retry && (status & BIT(0)));

	tty_flip_buffer_push(&sport->port);
}

static void tty_i3c_txwork(struct work_struct *work)
{
	struct ttyi3c_port *sport = container_of(work, struct ttyi3c_port, txwork);
	struct i3c_priv_xfer xfers;
	u32 retry = I3C_TTY_RETRY;
	unsigned long flags;
	int ret;

	xfers.rnw = 0;
	xfers.data.out = sport->tx_buff;

	while (!kfifo_is_empty(&sport->port.xmit_fifo)) {
		spin_lock_irqsave(&sport->xlock, flags);
		xfers.len = kfifo_out_peek(&sport->port.xmit_fifo, sport->tx_buff,
					   I3C_TTY_TRANS_SIZE);
		spin_unlock_irqrestore(&sport->xlock, flags);
		ret = i3c_device_do_priv_xfers(sport->i3cdev, &xfers, 1);
		if (ret) {
			/*
			 * Target side may not move data out of FIFO. delay can't resolve problem,
			 * just reduce some possiblity. Target can't end I3C SDR mode write
			 * transfer, discard data is reasonable when FIFO overrun.
			 */
			usleep_range(I3C_TTY_YIELD_US, 10 * I3C_TTY_YIELD_US);
			retry--;
		} else {
			retry = I3C_TTY_RETRY;
		}

		if (ret == 0 || retry == 0) {
			/* when retry == 0, means need discard the data */
			spin_lock_irqsave(&sport->xlock, flags);
			ret = kfifo_out(&sport->port.xmit_fifo, sport->tx_buff, xfers.len);
			spin_unlock_irqrestore(&sport->xlock, flags);
		}
	}

	spin_lock_irqsave(&sport->xlock, flags);
	if (kfifo_len(&sport->port.xmit_fifo) < WAKEUP_CHARS)
		tty_port_tty_wakeup(&sport->port);
	spin_unlock_irqrestore(&sport->xlock, flags);
}

static int i3c_probe(struct i3c_device *i3cdev)
{
	struct ttyi3c_port *sport;
	struct device *tty_dev;
	struct i3c_ibi_setup req;
	int minor;
	int ret;

	sport = devm_kzalloc(&i3cdev->dev, sizeof(*sport), GFP_KERNEL);
	if (!sport)
		return -ENOMEM;

	sport->i3cdev = i3cdev;

	dev_set_drvdata(&i3cdev->dev, sport);

	req.max_payload_len = 8;
	req.num_slots = 4;
	req.handler = &i3c_controller_irq_handler;

	ret = i3c_device_request_ibi(i3cdev, &req);
	if (ret)
		return -EINVAL;

	mutex_lock(&i3c_tty_minors_lock);
	minor = idr_alloc(&i3c_tty_minors, sport, 0, I3C_TTY_MINORS, GFP_KERNEL);
	mutex_unlock(&i3c_tty_minors_lock);

	if (minor < 0) {
		ret = -EINVAL;
		goto err_idr_alloc;
	}

	spin_lock_init(&sport->xlock);
	INIT_WORK(&sport->txwork, tty_i3c_txwork);
	INIT_WORK(&sport->rxwork, tty_i3c_rxwork);
	init_completion(&sport->txcomplete);

	tty_port_init(&sport->port);
	sport->port.ops = &i3c_port_ops;

	tty_dev = tty_port_register_device(&sport->port, i3c_tty_driver, minor,
					   &i3cdev->dev);
	if (IS_ERR(tty_dev)) {
		ret = PTR_ERR(tty_dev);
		goto err_tty_port_register;
	}

	sport->minor = minor;

	return 0;

err_tty_port_register:
	tty_port_put(&sport->port);

	mutex_lock(&i3c_tty_minors_lock);
	idr_remove(&i3c_tty_minors, minor);
	mutex_unlock(&i3c_tty_minors_lock);

err_idr_alloc:
	i3c_device_free_ibi(i3cdev);

	return ret;
}

static void i3c_remove(struct i3c_device *dev)
{
	struct ttyi3c_port *sport = dev_get_drvdata(&dev->dev);

	tty_port_unregister_device(&sport->port, i3c_tty_driver, sport->minor);
	cancel_work_sync(&sport->txwork);

	tty_port_put(&sport->port);

	mutex_lock(&i3c_tty_minors_lock);
	idr_remove(&i3c_tty_minors, sport->minor);
	mutex_unlock(&i3c_tty_minors_lock);

	i3c_device_free_ibi(sport->i3cdev);
}

static struct i3c_driver i3c_driver = {
	.driver = {
		.name = "ttyi3c",
	},
	.probe = i3c_probe,
	.remove = i3c_remove,
	.id_table = i3c_ids,
};

static int __init i3c_tty_init(void)
{
	int ret;

	i3c_tty_driver = tty_alloc_driver(I3C_TTY_MINORS,
					  TTY_DRIVER_REAL_RAW | TTY_DRIVER_DYNAMIC_DEV);

	if (IS_ERR(i3c_tty_driver))
		return PTR_ERR(i3c_tty_driver);

	i3c_tty_driver->driver_name = "ttyI3C";
	i3c_tty_driver->name = "ttyI3C";
	i3c_tty_driver->minor_start = 0,
	i3c_tty_driver->type = TTY_DRIVER_TYPE_SERIAL,
	i3c_tty_driver->subtype = SERIAL_TYPE_NORMAL,
	i3c_tty_driver->init_termios = tty_std_termios;
	i3c_tty_driver->init_termios.c_cflag = B9600 | CS8 | CREAD | HUPCL |
					       CLOCAL;
	i3c_tty_driver->init_termios.c_lflag = 0;

	tty_set_operations(i3c_tty_driver, &i3c_tty_ops);

	ret = tty_register_driver(i3c_tty_driver);
	if (ret)
		goto err_tty_register_driver;

	ret = i3c_driver_register(&i3c_driver);
	if (ret)
		goto err_i3c_driver_register;

	return 0;

err_i3c_driver_register:
	tty_unregister_driver(i3c_tty_driver);

err_tty_register_driver:
	tty_driver_kref_put(i3c_tty_driver);

	return ret;
}

static void __exit i3c_tty_exit(void)
{
	i3c_driver_unregister(&i3c_driver);
	tty_unregister_driver(i3c_tty_driver);
	tty_driver_kref_put(i3c_tty_driver);
	idr_destroy(&i3c_tty_minors);
}

module_init(i3c_tty_init);
module_exit(i3c_tty_exit);

MODULE_LICENSE("GPL");

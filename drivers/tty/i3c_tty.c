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

#define I3C_TTY_MINORS		256
#define I3C_TTY_TRANS_SIZE	16
#define I3C_TTY_RX_STOP		BIT(0)
#define I3C_TTY_RETRY		20
#define I3C_TTY_YIELD_US	100

struct ttyi3c_port {
	struct tty_port port;
	int minor;
	unsigned int txfifo_size;
	unsigned int rxfifo_size;
	struct circ_buf xmit;
	spinlock_t xlock; /* protect xmit */
	void *buffer;
	struct i3c_device *i3cdev;
	struct work_struct txwork;
	struct work_struct rxwork;
	struct completion txcomplete;
	struct workqueue_struct *workqueue;
	atomic_t status;
};

static const struct i3c_device_id i3c_ids[] = {
	I3C_DEVICE(0x011B, 0x1000, NULL),
	{ /* sentinel */ },
};

static int i3c_port_activate(struct tty_port *port, struct tty_struct *tty)
{
	struct ttyi3c_port *sport = container_of(port, struct ttyi3c_port, port);

	atomic_set(&sport->status, 0);

	return i3c_device_enable_ibi(sport->i3cdev);
}

static void i3c_port_shutdown(struct tty_port *port)
{
	struct ttyi3c_port *sport =
		container_of(port, struct ttyi3c_port, port);

	i3c_device_disable_ibi(sport->i3cdev);
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

static struct ttyi3c_port *i3c_get_by_minor(unsigned int minor)
{
	struct ttyi3c_port *sport;

	mutex_lock(&i3c_tty_minors_lock);
	sport = idr_find(&i3c_tty_minors, minor);
	mutex_unlock(&i3c_tty_minors_lock);

	return sport;
}

static int i3c_install(struct tty_driver *driver, struct tty_struct *tty)
{
	struct ttyi3c_port *sport;
	int ret;

	sport = i3c_get_by_minor(tty->index);
	if (!sport)
		return -ENODEV;

	ret = tty_standard_install(driver, tty);
	if (ret)
		return ret;

	tty->driver_data = sport;

	return 0;
}

static ssize_t i3c_write(struct tty_struct *tty, const unsigned char *buf, size_t count)
{
	struct ttyi3c_port *sport = tty->driver_data;
	struct circ_buf *circ = &sport->xmit;
	unsigned long flags;
	int c, ret = 0;

	spin_lock_irqsave(&sport->xlock, flags);
	while (1) {
		c = CIRC_SPACE_TO_END(circ->head, circ->tail, UART_XMIT_SIZE);
		if (count < c)
			c = count;
		if (c <= 0)
			break;

		memcpy(circ->buf + circ->head, buf, c);
		circ->head = (circ->head + c) & (UART_XMIT_SIZE - 1);
		buf += c;
		count -= c;
		ret += c;
	}
	spin_unlock_irqrestore(&sport->xlock, flags);

	if (circ->head != circ->tail)
		queue_work(sport->workqueue, &sport->txwork);

	return ret;
}

static int i3c_put_char(struct tty_struct *tty, unsigned char ch)
{
	struct ttyi3c_port *sport = tty->driver_data;
	struct circ_buf *circ = &sport->xmit;
	unsigned long flags;
	int ret = 0;

	spin_lock_irqsave(&sport->xlock, flags);

	if (sport && CIRC_SPACE(circ->head, circ->tail, UART_XMIT_SIZE) != 0) {
		circ->buf[circ->head] = ch;
		circ->head = (circ->head + 1) & (UART_XMIT_SIZE - 1);
		ret = 1;
	}

	spin_unlock_irqrestore(&sport->xlock, flags);

	return ret;
}

static void i3c_flush_chars(struct tty_struct *tty)
{
	struct ttyi3c_port *sport = tty->driver_data;

	queue_work(sport->workqueue, &sport->txwork);
}

static unsigned int i3c_write_room(struct tty_struct *tty)
{
	struct ttyi3c_port *sport = tty->driver_data;
	struct circ_buf *circ = &sport->xmit;
	unsigned long flags;
	int ret = 0;

	spin_lock_irqsave(&sport->xlock, flags);
	ret = CIRC_SPACE(circ->head, circ->tail, UART_XMIT_SIZE);
	spin_unlock_irqrestore(&sport->xlock, flags);

	return ret;
}

static void i3c_throttle(struct tty_struct *tty)
{
	struct ttyi3c_port *sport = tty->driver_data;

	atomic_or(I3C_TTY_RX_STOP, &sport->status);
}

static void i3c_unthrottle(struct tty_struct *tty)
{
	struct ttyi3c_port *sport = tty->driver_data;

	atomic_andnot(I3C_TTY_RX_STOP, &sport->status);

	queue_work(sport->workqueue, &sport->rxwork);
}

static int i3c_open(struct tty_struct *tty, struct file *filp)
{
	struct ttyi3c_port *sport = tty->driver_data;

	return tty_port_open(&sport->port, tty, filp);
}

static void i3c_close(struct tty_struct *tty, struct file *filp)
{
	struct ttyi3c_port *sport = tty->driver_data;

	if (!sport)
		return;

	tty_port_close(tty->port, tty, filp);
}

static void i3c_wait_until_sent(struct tty_struct *tty, int timeout)
{
	struct ttyi3c_port *sport = tty->driver_data;

	wait_for_completion_timeout(&sport->txcomplete, timeout);
	reinit_completion(&sport->txcomplete);
}

static const struct tty_operations i3c_tty_ops = {
	.install = i3c_install,
	.open = i3c_open,
	.close = i3c_close,
	.write = i3c_write,
	.put_char = i3c_put_char,
	.flush_chars = i3c_flush_chars,
	.write_room = i3c_write_room,
	.throttle = i3c_throttle,
	.unthrottle = i3c_unthrottle,
	.wait_until_sent = i3c_wait_until_sent,
};

static void i3c_controller_irq_handler(struct i3c_device *dev,
				       const struct i3c_ibi_payload *payload)
{
	struct ttyi3c_port *sport = dev_get_drvdata(&dev->dev);

	queue_work(sport->workqueue, &sport->rxwork);
}

static void tty_i3c_rxwork(struct work_struct *work)
{
	struct ttyi3c_port *sport = container_of(work, struct ttyi3c_port, rxwork);
	struct i3c_priv_xfer xfers;
	int retry = I3C_TTY_RETRY;
	u16 status = BIT(0);

	do {
		memset(&xfers, 0, sizeof(xfers));
		xfers.data.in = sport->buffer;
		xfers.len = I3C_TTY_TRANS_SIZE;
		xfers.rnw = 1;

		if (I3C_TTY_RX_STOP & atomic_read(&sport->status))
			break;

		i3c_device_do_priv_xfers(sport->i3cdev, &xfers, 1);

		if (xfers.actual_len) {
			tty_insert_flip_string(&sport->port, sport->buffer, xfers.actual_len);
			retry = 20;
			continue;
		} else {
			status = BIT(0);
			i3c_device_getstatus_format1(sport->i3cdev, &status);
			/*
			 * Target side need some time to fill data into fifo. Target side may not
			 * have hardware update status in real time. Software update status always
			 * need some delays.
			 *
			 * Generally, target side have cicular buffer in memory, it will be moved
			 * into FIFO by CPU or DMA. 'status' just show if cicular buffer empty. But
			 * there are gap, espcially CPU have not response irq to fill FIFO in time.
			 * So xfers.actual will be zero, wait for little time to avoid flood
			 * transfer in i3c bus.
			 */
			usleep_range(I3C_TTY_YIELD_US, 10 * I3C_TTY_YIELD_US);
			retry--;
		}

	} while (retry && (status & BIT(0)));

	tty_flip_buffer_push(&sport->port);
}

static void tty_i3c_txwork(struct work_struct *work)
{
	struct ttyi3c_port *sport = container_of(work, struct ttyi3c_port, txwork);
	struct circ_buf *circ = &sport->xmit;
	int cnt = CIRC_CNT(circ->head, circ->tail, UART_XMIT_SIZE);
	struct i3c_priv_xfer xfers;
	int retry = I3C_TTY_RETRY;
	unsigned long flags;
	int actual;
	int ret;

	while (cnt > 0 && retry) {
		xfers.rnw = 0;
		xfers.len = CIRC_CNT_TO_END(circ->head, circ->tail, UART_XMIT_SIZE);
		xfers.len = min_t(u16, 16, xfers.len);
		xfers.data.out = circ->buf + circ->tail;

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
			retry = 0;
		}

		actual = xfers.len;

		circ->tail = (circ->tail + actual) & (UART_XMIT_SIZE - 1);

		if (CIRC_CNT(circ->head, circ->tail, UART_XMIT_SIZE) < WAKEUP_CHARS)
			tty_port_tty_wakeup(&sport->port);

		cnt = CIRC_CNT(circ->head, circ->tail, UART_XMIT_SIZE);
	}

	spin_lock_irqsave(&sport->xlock, flags);
	if (circ->head == circ->tail)
		complete(&sport->txcomplete);
	spin_unlock_irqrestore(&sport->xlock, flags);
}

static int i3c_probe(struct i3c_device *i3cdev)
{
	struct ttyi3c_port *port;
	struct device *tty_dev;
	struct i3c_ibi_setup req;
	int minor;
	int ret;

	port = devm_kzalloc(&i3cdev->dev, sizeof(*port), GFP_KERNEL);
	if (!port)
		return -ENOMEM;

	port->i3cdev = i3cdev;
	port->buffer = devm_kzalloc(&i3cdev->dev, UART_XMIT_SIZE, GFP_KERNEL);
	if (!port->buffer)
		return -ENOMEM;

	port->xmit.buf = devm_kzalloc(&i3cdev->dev, UART_XMIT_SIZE, GFP_KERNEL);
	if (!port->xmit.buf)
		return -ENOMEM;

	dev_set_drvdata(&i3cdev->dev, port);

	req.max_payload_len = 8;
	req.num_slots = 4;
	req.handler = &i3c_controller_irq_handler;

	ret = i3c_device_request_ibi(i3cdev, &req);
	if (ret)
		return -EINVAL;

	mutex_lock(&i3c_tty_minors_lock);
	minor = idr_alloc(&i3c_tty_minors, port, 0, I3C_TTY_MINORS, GFP_KERNEL);
	mutex_unlock(&i3c_tty_minors_lock);

	if (minor < 0)
		return -EINVAL;

	spin_lock_init(&port->xlock);
	INIT_WORK(&port->txwork, tty_i3c_txwork);
	INIT_WORK(&port->rxwork, tty_i3c_rxwork);
	init_completion(&port->txcomplete);

	port->workqueue = alloc_workqueue("%s", 0, 0, dev_name(&i3cdev->dev));
	if (!port->workqueue)
		return -ENOMEM;

	tty_port_init(&port->port);
	port->port.ops = &i3c_port_ops;

	tty_dev = tty_port_register_device(&port->port, i3c_tty_driver, minor,
					   &i3cdev->dev);
	if (IS_ERR(tty_dev)) {
		destroy_workqueue(port->workqueue);
		return PTR_ERR(tty_dev);
	}

	port->minor = minor;

	return 0;
}

void i3c_remove(struct i3c_device *dev)
{
	struct ttyi3c_port *sport = dev_get_drvdata(&dev->dev);

	tty_port_unregister_device(&sport->port, i3c_tty_driver, sport->minor);
	cancel_work_sync(&sport->txwork);
	destroy_workqueue(sport->workqueue);
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
	if (ret) {
		tty_driver_kref_put(i3c_tty_driver);
		return ret;
	}

	ret = i3c_driver_register(&i3c_driver);
	if (ret) {
		tty_unregister_driver(i3c_tty_driver);
		tty_driver_kref_put(i3c_tty_driver);
	}

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

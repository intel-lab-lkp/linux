// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2023 NXP
 * Author: Frank Li <Frank.Li@nxp.com>
 */

#include <linux/iopoll.h>
#include <linux/i3c/slave.h>
#include <linux/serial_core.h>
#include <linux/slab.h>
#include <linux/tty_flip.h>

static DEFINE_IDR(i3c_tty_minors);
static DEFINE_MUTEX(i3c_tty_minors_lock);

static struct tty_driver *i3c_tty_driver;

#define I3C_TTY_MINORS		256

#define I3C_TX_NOEMPTY		BIT(0)
#define I3C_TTY_TRANS_SIZE	16
#define I3C_TTY_IBI_TX		BIT(0)

struct ttyi3c_port {
	struct tty_port port;
	int minor;
	struct i3c_slave_func *i3cdev;
	struct circ_buf xmit;
	int tail_in_queue;
	struct completion txcomplete;
	spinlock_t xlock;
	void *buffer;
	struct work_struct work;
	struct workqueue_struct	*workqueue;
	u16 status;
};

static void i3c_slave_tty_rx_complete(struct i3c_request *req)
{
	struct ttyi3c_port *port = req->context;

	if (req->status == I3C_REQUEST_CANCEL) {
		i3c_slave_ctrl_free_request(req);
		return;
	}

	for (int i = 0; i < req->actual; i++)
		tty_insert_flip_char(&port->port, *(u8 *)(req->buf + i), 0);

	tty_flip_buffer_push(&port->port);
	req->actual = 0;
	req->status = 0;
	i3c_slave_ctrl_queue(req, GFP_KERNEL);
}

static void i3c_slave_tty_tx_complete(struct i3c_request *req)
{
	struct ttyi3c_port *sport = req->context;
	struct circ_buf *xmit = &sport->xmit;
	unsigned long flags;
	int cnt;

	if (req->status == I3C_REQUEST_CANCEL) {
		i3c_slave_ctrl_free_request(req);
		return;
	}

	spin_lock_irqsave(&sport->xlock, flags);
	xmit->tail = (xmit->tail + req->actual) & (UART_XMIT_SIZE - 1);
	cnt = CIRC_CNT(xmit->head, xmit->tail, UART_XMIT_SIZE);
	if (cnt == 0)
		complete(&sport->txcomplete);
	else
		queue_work(sport->workqueue, &sport->work);

	spin_unlock_irqrestore(&sport->xlock, flags);

	if (cnt < WAKEUP_CHARS)
		tty_port_tty_wakeup(&sport->port);

	i3c_slave_ctrl_free_request(req);
}

static void i3c_slave_tty_i3c_work(struct work_struct *work)
{
	struct ttyi3c_port *sport = container_of(work, struct ttyi3c_port, work);
	struct circ_buf *xmit = &sport->xmit;
	int cnt = CIRC_CNT(xmit->head, sport->tail_in_queue, UART_XMIT_SIZE);
	u8 ibi;

	if (cnt == 0)
		return;

	while (cnt > 0) {
		struct i3c_request *req = i3c_slave_ctrl_alloc_request(sport->i3cdev->ctrl,
								       GFP_KERNEL);
		if (!req)
			return;

		req->length = CIRC_CNT_TO_END(xmit->head, sport->tail_in_queue, UART_XMIT_SIZE);

		req->buf =  xmit->buf + sport->tail_in_queue;
		req->complete = i3c_slave_tty_tx_complete;
		req->context = sport;
		req->tx = true;

		if (i3c_slave_ctrl_queue(req, GFP_KERNEL))
			return;

		sport->tail_in_queue += req->length;
		sport->tail_in_queue &= UART_XMIT_SIZE - 1;

		cnt = CIRC_CNT(xmit->head, sport->tail_in_queue, UART_XMIT_SIZE);
	}

	ibi = I3C_TTY_IBI_TX;
	i3c_slave_ctrl_raise_ibi(sport->i3cdev->ctrl, &ibi, 1);
}

static int i3c_port_activate(struct tty_port *port, struct tty_struct *tty)
{
	struct ttyi3c_port *sport = container_of(port, struct ttyi3c_port, port);
	const struct i3c_slave_ctrl_features *feature;
	struct i3c_slave_func *func = sport->i3cdev;
	struct i3c_request *req;
	int rxfifo_size;
	int offset = 0;

	feature = i3c_slave_ctrl_get_features(func->ctrl);
	if (!feature)
		return -EINVAL;

	rxfifo_size = feature->rx_fifo_sz;

	if (!rxfifo_size)
		rxfifo_size = I3C_TTY_TRANS_SIZE;

	do {
		req = i3c_slave_ctrl_alloc_request(func->ctrl, GFP_KERNEL);
		if (!req)
			goto err;

		req->buf = (void *) (sport->buffer + offset);
		req->length = rxfifo_size;
		req->context = sport;
		req->complete = i3c_slave_tty_rx_complete;
		offset += rxfifo_size;

		if (i3c_slave_ctrl_queue(req, GFP_KERNEL))
			goto err;
	} while (req && (offset + rxfifo_size) < UART_XMIT_SIZE);

	reinit_completion(&sport->txcomplete);

	return 0;
err:
	i3c_slave_ctrl_cancel_all_reqs(func->ctrl, false);
	return -ENOMEM;
}

static void i3c_port_shutdown(struct tty_port *port)
{
	struct ttyi3c_port *sport =
		container_of(port, struct ttyi3c_port, port);

	cancel_work_sync(&sport->work);

	i3c_slave_ctrl_cancel_all_reqs(sport->i3cdev->ctrl, true);
	i3c_slave_ctrl_cancel_all_reqs(sport->i3cdev->ctrl, false);

	sport->xmit.tail = sport->tail_in_queue = sport->xmit.head;

	i3c_slave_ctrl_fifo_flush(sport->i3cdev->ctrl, true);
	i3c_slave_ctrl_fifo_flush(sport->i3cdev->ctrl, false);
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

static int i3c_slave_tty_bind(struct i3c_slave_func *func)
{
	struct ttyi3c_port *sport;
	struct device *tty_dev;
	int minor;
	int ret;

	sport = dev_get_drvdata(&func->dev);

	if (i3c_slave_ctrl_set_config(func->ctrl, func)) {
		dev_err(&func->dev, "failure set i3c config\n");
		return -EINVAL;
	}

	sport->buffer = (void *)get_zeroed_page(GFP_KERNEL);
	if (!sport->buffer)
		return -ENOMEM;

	sport->xmit.buf = (void *)get_zeroed_page(GFP_KERNEL);
	if (!sport->xmit.buf)
		goto err_alloc_xmit;


	spin_lock_init(&sport->xlock);
	init_completion(&sport->txcomplete);

	mutex_lock(&i3c_tty_minors_lock);
	ret = minor = idr_alloc(&i3c_tty_minors, sport, 0, I3C_TTY_MINORS, GFP_KERNEL);
	mutex_unlock(&i3c_tty_minors_lock);

	if (minor < 0)
		goto err_idr_alloc;

	tty_port_init(&sport->port);
	sport->port.ops = &i3c_port_ops;

	tty_dev = tty_port_register_device(&sport->port, i3c_tty_driver, minor,
					   &func->dev);
	if (IS_ERR(tty_dev)) {
		ret = PTR_ERR(tty_dev);
		goto err_register_port;
	}

	sport->minor = minor;
	ret = i3c_slave_ctrl_enable(func->ctrl);
	if (ret)
		goto err_ctrl_enable;

	return 0;

err_ctrl_enable:
	tty_port_unregister_device(&sport->port, i3c_tty_driver, sport->minor);
err_register_port:
	mutex_lock(&i3c_tty_minors_lock);
	idr_remove(&i3c_tty_minors, sport->minor);
	mutex_unlock(&i3c_tty_minors_lock);
err_idr_alloc:
	i3c_slave_ctrl_cancel_all_reqs(func->ctrl, false);
	free_page((unsigned long)sport->xmit.buf);
err_alloc_xmit:
	free_page((unsigned long)sport->buffer);

	dev_err(&func->dev, "bind failure\n");

	return ret;
}

static void i3c_slave_tty_unbind(struct i3c_slave_func *func)
{
	struct ttyi3c_port *sport;

	sport = dev_get_drvdata(&func->dev);

	cancel_work_sync(&sport->work);

	i3c_slave_ctrl_disable(func->ctrl);
	i3c_slave_ctrl_cancel_all_reqs(func->ctrl, 0);
	i3c_slave_ctrl_cancel_all_reqs(func->ctrl, 1);

	tty_port_unregister_device(&sport->port, i3c_tty_driver, sport->minor);

	free_page((unsigned long)sport->buffer);
}

static struct i3c_slave_func_ops i3c_func_ops = {
	.bind   = i3c_slave_tty_bind,
	.unbind = i3c_slave_tty_unbind,
};

static int i3c_tty_probe(struct i3c_slave_func *func)
{
	struct device *dev = &func->dev;
	struct ttyi3c_port *port;

	port = devm_kzalloc(dev, sizeof(*port), GFP_KERNEL);
	if (!port)
		return -ENOMEM;

	port->i3cdev = func;
	dev_set_drvdata(&func->dev, port);

	port->workqueue = alloc_workqueue("%s", 0, 0, dev_name(&func->dev));
	if (!port->workqueue)
		return -ENOMEM;

	INIT_WORK(&port->work, i3c_slave_tty_i3c_work);

	return 0;
}

static void  i3c_tty_remove(struct i3c_slave_func *func)
{
	struct ttyi3c_port *port;

	port = dev_get_drvdata(&func->dev);

	destroy_workqueue(port->workqueue);
}

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
	i3c_slave_ctrl_set_status_format1(sport->i3cdev->ctrl, sport->status | I3C_TX_NOEMPTY);
	spin_unlock_irqrestore(&sport->xlock, flags);

	if (circ->head != circ->tail)
		queue_work(sport->workqueue, &sport->work);

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
	struct circ_buf *circ = &sport->xmit;
	unsigned long flags;

	spin_lock_irqsave(&sport->xlock, flags);
	if (CIRC_SPACE(circ->head, circ->tail, UART_XMIT_SIZE))
		queue_work(sport->workqueue, &sport->work);
	spin_unlock_irqrestore(&sport->xlock, flags);
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

	i3c_slave_ctrl_cancel_all_reqs(sport->i3cdev->ctrl, false);
}

static void i3c_unthrottle(struct tty_struct *tty)
{
	struct ttyi3c_port *sport = tty->driver_data;

	i3c_port_activate(&sport->port, tty);
}

static int i3c_open(struct tty_struct *tty, struct file *filp)
{
	struct ttyi3c_port *sport = tty->driver_data;
	int ret;

	if (!i3c_slave_ctrl_get_addr(sport->i3cdev->ctrl)) {
		dev_info(&sport->i3cdev->dev, "No slave addr assigned, try hotjoin");
		ret = i3c_slave_ctrl_hotjoin(sport->i3cdev->ctrl);
		if (ret) {
			dev_err(&sport->i3cdev->dev, "Hotjoin failure, check connection");
			return ret;
		}
	}

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
	struct circ_buf *circ = &sport->xmit;
	int val;
	int ret;
	u8 ibi = I3C_TTY_IBI_TX;
	int retry = 100;

	if (circ->head != circ->tail) {

		do {
			ret = wait_for_completion_timeout(&sport->txcomplete, timeout / 100);
			if (ret)
				break;
			i3c_slave_ctrl_raise_ibi(sport->i3cdev->ctrl, &ibi, 1);
		} while (retry--);

		reinit_completion(&sport->txcomplete);
	}

	read_poll_timeout(i3c_slave_ctrl_fifo_status, val, !val, 100, timeout, false,
			  sport->i3cdev->ctrl, true);

	i3c_slave_ctrl_set_status_format1(sport->i3cdev->ctrl, sport->status & (~I3C_TX_NOEMPTY));
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

DECLARE_I3C_SLAVE_FUNC(tty, i3c_tty_probe, i3c_tty_remove, &i3c_func_ops);

static int __init i3c_tty_init(void)
{
	int ret;

	i3c_tty_driver = tty_alloc_driver(
		I3C_TTY_MINORS, TTY_DRIVER_REAL_RAW | TTY_DRIVER_DYNAMIC_DEV);

	if (IS_ERR(i3c_tty_driver))
		return PTR_ERR(i3c_tty_driver);

	i3c_tty_driver->driver_name = "ttySI3C", i3c_tty_driver->name = "ttySI3C",
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

	ret = i3c_slave_func_register_driver(&ttyi3c_func);
	if (ret) {
		tty_unregister_driver(i3c_tty_driver);
		tty_driver_kref_put(i3c_tty_driver);
	}

	return ret;
}

static void __exit i3c_tty_exit(void)
{
	i3c_slave_func_unregister_driver(&ttyi3c_func);
	tty_unregister_driver(i3c_tty_driver);
	tty_driver_kref_put(i3c_tty_driver);
	idr_destroy(&i3c_tty_minors);
}

module_init(i3c_tty_init);
module_exit(i3c_tty_exit);

MODULE_LICENSE("GPL");


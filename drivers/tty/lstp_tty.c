// SPDX-License-Identifier: GPL-2.0-only
/*
 * UART driver for LSTP USB interface.
 *
 * Copyright (c) 2026, NVIDIA CORPORATION & AFFILIATES.  All rights reserved.
 */

#include <linux/bits.h>
#include <linux/cleanup.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/serial.h>
#include <linux/termios.h>
#include <linux/tty.h>
#include <linux/tty_buffer.h>
#include <linux/tty_driver.h>
#include <linux/tty_flip.h>
#include <linux/workqueue.h>
#include <linux/xarray.h>

#include <linux/usb/lstp.h>

enum lstp_uart_cmd {
	LSTP_UART_CMD_WRITE = 0x00,
};

struct lstp_uart_config {
	__le32 speed;
	u8 data_bits;
	u8 stop_bits;
	u8 parity_flow; /* [3:0] parity, [7:4] flow control */
} __packed;

#define LSTP_UART_CFG_PARITY(pf) ((pf) & 0x0F)
#define LSTP_UART_CFG_FLOW(pf) (((pf) >> 4) & 0x0F)

enum lstp_uart_parity {
	LSTP_UART_PARITY_NONE = 0,
	LSTP_UART_PARITY_ODD = 1,
	LSTP_UART_PARITY_EVEN = 2,
	LSTP_UART_PARITY_MARK = 3,
	LSTP_UART_PARITY_SPACE = 4,
};

enum lstp_uart_flow {
	LSTP_UART_FLOW_NONE = 0,
	LSTP_UART_FLOW_XONXOFF = 1,
	LSTP_UART_FLOW_RTSCTS = 2,
	LSTP_UART_FLOW_DTRDSR = 3,
};

/* clang-format off */

/* Tag bits OR'd into the WRITE command byte. */
#define LSTP_UART_TAG_CARRIER_DOWN BIT(6) /* dev→host: carrier lost */
#define LSTP_UART_TAG_TX_OVERRUN   BIT(5) /* dev→host: TX data dropped */
#define LSTP_UART_TAG_BREAK_DET    BIT(4) /* dev→host: break detected */
#define LSTP_UART_TAG_GEN_BREAK    BIT(3) /* host→dev: send break */

/* clang-format on */

/* Bits [1:0] = command; [6:2] = tags; [7] = req/resp flag. */
#define LSTP_UART_CMD_MASK GENMASK(1, 0)

/* Firmware NACKs WRITE on a full TX FIFO; 5 x 10 ms covers a 500-byte drain at 115200. */
#define LSTP_UART_NAK_MAX_RETRIES 5
#define LSTP_UART_NAK_RETRY_DELAY_MS 10

#define LSTP_UART_MINORS 256

/* TX coalesces: lstp_tty_write() appends to @coal_buf, write_work drains @tx_snapshot. */
struct lstp_uart_ctx {
	struct tty_port port;
	struct lstp_channel *ch;
	int minor;
	bool registered; /* tty device live for @minor; under lstp_minors_lock */
	size_t max_payload;
	tcflag_t hw_cflag;
	tcflag_t hw_iflag;

	spinlock_t write_lock; /* protects the TX fields below and @iocount.tx */
	u8 *coal_buf;
	u8 *tx_snapshot; /* stable copy handed to lstp_request() */
	size_t coal_len;
	size_t tx_inflight_len;
	bool tx_in_flight;
	struct work_struct write_work;

	/*
	 * Each field is written under one lock -- @write_lock for tx, the
	 * channel's irq_cb_lock for the rest -- and read under neither by
	 * lstp_tty_get_icount(), so writes use WRITE_ONCE() and reads READ_ONCE().
	 */
	struct serial_icounter_struct iocount;
};

static struct tty_driver *lstp_tty_driver;

static DEFINE_XARRAY_ALLOC(lstp_minors);
/* Nests inside tty_lock; never hold it across tty_vhangup(), which takes tty_lock. */
static DEFINE_MUTEX(lstp_minors_lock);

static int lstp_alloc_minor(struct lstp_uart_ctx *ctx)
{
	u32 minor;
	int ret;

	guard(mutex)(&lstp_minors_lock);
	ret = xa_alloc(&lstp_minors, &minor, ctx, XA_LIMIT(0, LSTP_UART_MINORS - 1), GFP_KERNEL);
	return ret ? ret : minor;
}

/*
 * Called only from lstp_port_destruct(), so the minor stays reserved for the
 * life of the port: recycling it earlier would let an open of the reused
 * index resolve through driver->ttys[] to the previous port's tty.
 */
static void lstp_free_minor(struct lstp_uart_ctx *ctx)
{
	guard(mutex)(&lstp_minors_lock);

	if (ctx->minor < 0)
		return;

	xa_erase(&lstp_minors, ctx->minor);
	ctx->minor = -1;
}

/*
 * Resolve a live port by minor, taking a tty_port reference under the lock
 * lstp_port_destruct() erases the entry under. The context pins its channel,
 * so @ctx->ch is valid while the reference is held. The entry outlives
 * @registered: a minor between unbind and destruction is found and refused.
 */
static struct lstp_uart_ctx *lstp_get_by_minor(unsigned int minor)
{
	guard(mutex)(&lstp_minors_lock);
	struct lstp_uart_ctx *ctx = xa_load(&lstp_minors, minor);

	if (!ctx || !ctx->registered || lstp_channel_disconnected(ctx->ch))
		return NULL;

	/* Fails once the last put is in, ahead of the destructor's erase. */
	if (!tty_port_get(&ctx->port))
		return NULL;
	return ctx;
}

/* clang-format off */
static speed_t lstp_baud_to_speed(u32 baud)
{
	static const struct {
		u32     baud;
		speed_t speed;
	} map[] = {
		{      50, B50      }, {      75, B75      }, {     110, B110     },
		{     134, B134     }, {     150, B150     }, {     200, B200     },
		{     300, B300     }, {     600, B600     }, {    1200, B1200    },
		{    1800, B1800    }, {    2400, B2400    }, {    4800, B4800    },
		{    9600, B9600    }, {   19200, B19200   }, {   38400, B38400   },
		{   57600, B57600   }, {  115200, B115200  }, {  230400, B230400  },
		{  460800, B460800  }, {  500000, B500000  }, {  576000, B576000  },
		{  921600, B921600  }, { 1000000, B1000000 }, { 1152000, B1152000 },
		{ 1500000, B1500000 }, { 2000000, B2000000 }, { 2500000, B2500000 },
		{ 3000000, B3000000 }, { 3500000, B3500000 }, { 4000000, B4000000 },
	};

	for (size_t i = 0; i < ARRAY_SIZE(map); i++)
		if (map[i].baud == baud)
			return map[i].speed;

	return B0;
}

/* clang-format on */

static tcflag_t lstp_build_cflag(const struct lstp_uart_config *cfg)
{
	tcflag_t cflag = CREAD | HUPCL | CLOCAL;
	u8 parity = LSTP_UART_CFG_PARITY(cfg->parity_flow);
	u8 flow = LSTP_UART_CFG_FLOW(cfg->parity_flow);

	cflag |= lstp_baud_to_speed(le32_to_cpu(cfg->speed));

	switch (cfg->data_bits) {
	case 5:
		cflag |= CS5;
		break;
	case 6:
		cflag |= CS6;
		break;
	case 7:
		cflag |= CS7;
		break;
	default:
		cflag |= CS8;
		break;
	}

	if (cfg->stop_bits == 2)
		cflag |= CSTOPB;

	switch (parity) {
	case LSTP_UART_PARITY_ODD:
		cflag |= PARENB | PARODD;
		break;
	case LSTP_UART_PARITY_EVEN:
		cflag |= PARENB;
		break;
	case LSTP_UART_PARITY_MARK:
		cflag |= PARENB | PARODD | CMSPAR;
		break;
	case LSTP_UART_PARITY_SPACE:
		cflag |= PARENB | CMSPAR;
		break;
	}

	if (flow == LSTP_UART_FLOW_RTSCTS)
		cflag |= CRTSCTS;

	return cflag;
}

static tcflag_t lstp_build_iflag(const struct lstp_uart_config *cfg)
{
	if (LSTP_UART_CFG_FLOW(cfg->parity_flow) == LSTP_UART_FLOW_XONXOFF)
		return IXON | IXOFF;
	return 0;
}

static int lstp_uart_parse_config(struct lstp_uart_ctx *ctx, struct lstp_channel *ch,
				  const struct lstp_uart_config *cfg)
{
	static const char parity_chars[] = "NOEMS";
	u8 parity = LSTP_UART_CFG_PARITY(cfg->parity_flow);

	ctx->hw_cflag = lstp_build_cflag(cfg);
	ctx->hw_iflag = lstp_build_iflag(cfg);

	dev_dbg(ch->dev, "%s: ch_%d: %u baud, %u%c%c, parity %u, flow %u\n", __func__, ch->ch_id,
		le32_to_cpu(cfg->speed), cfg->data_bits, cfg->stop_bits == 2 ? '2' : '1',
		parity < sizeof(parity_chars) - 1 ? parity_chars[parity] : '?', parity,
		LSTP_UART_CFG_FLOW(cfg->parity_flow));

	return 0;
}

static int lstp_uart_send_with_retry(struct lstp_uart_ctx *ctx, u8 cmd, const void *payload,
				     size_t len)
{
	struct lstp_channel *ch = ctx->ch;
	int lstp_status = LSTP_NACK;
	int ret = 0;
	int attempt;

	for (attempt = 0; lstp_status == LSTP_NACK && attempt < LSTP_UART_NAK_MAX_RETRIES;
	     attempt++) {
		if (attempt)
			fsleep(LSTP_UART_NAK_RETRY_DELAY_MS * USEC_PER_MSEC);

		ret = lstp_request(ch, cmd, payload, len, NULL, 0, NULL, &lstp_status);
	}

	if (ret && ret != -ENODEV)
		dev_warn_ratelimited(ch->dev,
				     "%s: ch_%d: Failed to write after %d attempt(s) (%pe)\n",
				     __func__, ch->ch_id, attempt, ERR_PTR(ret));

	return ret;
}

/* @tx_snapshot is written only here, so it stays stable across the retry loop. */
static void lstp_uart_write_work(struct work_struct *work)
{
	struct lstp_uart_ctx *ctx = container_of(work, struct lstp_uart_ctx, write_work);
	size_t len;

	scoped_guard(spinlock_irqsave, &ctx->write_lock) {
		len = ctx->coal_len;
		if (len == 0)
			return;
		memcpy(ctx->tx_snapshot, ctx->coal_buf, len);
		ctx->coal_len = 0;
		ctx->tx_in_flight = true;
		ctx->tx_inflight_len = len;
	}

	tty_port_tty_wakeup(&ctx->port);

	int ret = lstp_uart_send_with_retry(ctx, LSTP_UART_CMD_WRITE, ctx->tx_snapshot, len);

	scoped_guard(spinlock_irqsave, &ctx->write_lock) {
		ctx->tx_in_flight = false;
		if (!ret)
			WRITE_ONCE(ctx->iocount.tx, ctx->iocount.tx + len);
		if (ctx->coal_len > 0)
			schedule_work(&ctx->write_work);
	}

	tty_port_tty_wakeup(&ctx->port);
}

/* ops->irq_packet: push RX into the flip buffer, NACKing when full so firmware retries. */
static void lstp_uart_irq_callback(struct lstp_channel *ch, struct lstp_packet *pkt)
{
	struct lstp_uart_ctx *ctx = ch->priv;
	u16 rx_len = le16_to_cpu(pkt->hdr.length);
	u8 cmd = pkt->hdr.cmd;
	bool have_break = cmd & LSTP_UART_TAG_BREAK_DET;
	bool carrier_down = cmd & LSTP_UART_TAG_CARRIER_DOWN;
	bool tx_overrun = cmd & LSTP_UART_TAG_TX_OVERRUN;
	size_t need_room = rx_len + (have_break ? 1 : 0);
	u8 status = LSTP_SUCCESS;

	if ((cmd & LSTP_UART_CMD_MASK) != LSTP_UART_CMD_WRITE) {
		dev_warn_ratelimited(ch->dev, "%s: ch_%d: unexpected UART command 0x%02x\n",
				     __func__, ch->ch_id, cmd);
		lstp_send_irq_resp(ch, LSTP_NOT_SUPP);
		return;
	}

	size_t room = tty_buffer_request_room(&ctx->port, need_room);

	if (room < need_room) {
		WRITE_ONCE(ctx->iocount.buf_overrun, ctx->iocount.buf_overrun + 1);
		status = LSTP_NACK;
	} else {
		if (have_break) {
			WRITE_ONCE(ctx->iocount.brk, ctx->iocount.brk + 1);
			tty_insert_flip_char(&ctx->port, 0, TTY_BREAK);
		}
		if (rx_len) {
			tty_insert_flip_string(&ctx->port, pkt->payload, rx_len);
			WRITE_ONCE(ctx->iocount.rx, ctx->iocount.rx + rx_len);
		}
		tty_flip_buffer_push(&ctx->port);
	}

	if (tx_overrun) {
		WRITE_ONCE(ctx->iocount.overrun, ctx->iocount.overrun + 1);
		dev_warn_ratelimited(ch->dev,
				     "%s: ch_%d: RX overrun on link (firmware dropped bytes)\n",
				     __func__, ch->ch_id);
	}

	if (carrier_down) {
		WRITE_ONCE(ctx->iocount.dcd, ctx->iocount.dcd + 1);
		/* Honors CLOCAL: no-op when userspace asked to ignore carrier. */
		tty_port_tty_hangup(&ctx->port, true);
	}

	lstp_send_irq_resp(ch, status);
}

static int lstp_port_activate(struct tty_port *port, struct tty_struct *tty)
{
	struct lstp_uart_ctx *ctx = container_of(port, struct lstp_uart_ctx, port);

	/*
	 * Re-test @registered: .install() resolved it under tty_mutex, which
	 * the core drops before .open. lstp_uart_stop() clears the flag before
	 * it samples @port->tty, so an open that gets past here has already
	 * published its tty and stop() will hang it up.
	 */
	scoped_guard(mutex, &lstp_minors_lock)
		if (!ctx->registered)
			return -ENODEV;

	tty->termios.c_cflag = ctx->hw_cflag;
	tty->termios.c_iflag = ctx->hw_iflag;

	/* TTY_NO_WRITE_SPLIT: let N_TTY pass a whole write down, not byte at a time. */
	set_bit(TTY_NO_WRITE_SPLIT, &tty->flags);
	return 0;
}

static void lstp_port_shutdown(struct tty_port *port)
{
	struct lstp_uart_ctx *ctx = container_of(port, struct lstp_uart_ctx, port);

	cancel_work_sync(&ctx->write_work);

	/*
	 * Normally already empty. What survives the close drain and the hangup
	 * flush is a drain still stalled when closing_wait expires: bytes queued
	 * with no worker after the cancel above, so write_room() would stay
	 * short by that much for the life of the port.
	 */
	scoped_guard(spinlock_irqsave, &ctx->write_lock) {
		ctx->coal_len = 0;
		ctx->tx_inflight_len = 0;
	}
}

static void lstp_port_destruct(struct tty_port *port)
{
	struct lstp_uart_ctx *ctx = container_of(port, struct lstp_uart_ctx, port);
	struct lstp_channel *ch = ctx->ch;

	lstp_free_minor(ctx);
	kfree(ctx->tx_snapshot);
	kfree(ctx->coal_buf);
	kfree(ctx);
	lstp_channel_put(ch);
}

static const struct tty_port_operations lstp_port_ops = {
	.activate = lstp_port_activate,
	.shutdown = lstp_port_shutdown,
	.destruct = lstp_port_destruct,
};

static int lstp_tty_install(struct tty_driver *driver, struct tty_struct *tty)
{
	struct lstp_uart_ctx *ctx = lstp_get_by_minor(tty->index);

	if (!ctx)
		return -ENODEV;

	/*
	 * tty_port_install(), as it sets tty->port: tty_init_dev() would use
	 * driver->ports[], which tty_unregister_device() leaves naming the
	 * minor's last context.
	 */
	int ret = tty_port_install(&ctx->port, driver, tty);

	if (ret) {
		tty_port_put(&ctx->port);
		return ret;
	}

	/*
	 * The tty now owns the lookup's reference; lstp_tty_cleanup() drops it.
	 * @driver_data is the context, not the channel, so this tty never
	 * reaches a later bind's context.
	 */
	tty->driver_data = ctx;
	return 0;
}

static int lstp_tty_open(struct tty_struct *tty, struct file *filp)
{
	return tty_port_open(tty->port, tty, filp);
}

static void lstp_tty_close(struct tty_struct *tty, struct file *filp)
{
	tty_port_close(tty->port, tty, filp);
}

static void lstp_tty_hangup(struct tty_struct *tty)
{
	tty_port_hangup(tty->port);
}

static void lstp_tty_cleanup(struct tty_struct *tty)
{
	/* Drops what .install was handed. Runs for every installed tty. */
	tty_port_put(tty->port);
}

static ssize_t lstp_tty_write(struct tty_struct *tty, const unsigned char *buf, size_t count)
{
	struct lstp_uart_ctx *ctx = tty->driver_data;
	struct lstp_channel *ch = ctx->ch;

	if (!count)
		return 0;

	guard(spinlock_irqsave)(&ctx->write_lock);

	if (lstp_channel_disconnected(ch))
		return -EIO;

	size_t room = ctx->max_payload - ctx->coal_len;

	if (!room)
		return 0;

	count = min(count, room);
	memcpy(ctx->coal_buf + ctx->coal_len, buf, count);
	ctx->coal_len += count;

	schedule_work(&ctx->write_work);

	return count;
}

static unsigned int lstp_tty_write_room(struct tty_struct *tty)
{
	struct lstp_uart_ctx *ctx = tty->driver_data;
	struct lstp_channel *ch = ctx->ch;

	guard(spinlock_irqsave)(&ctx->write_lock);

	if (lstp_channel_disconnected(ch))
		return 0;
	return ctx->max_payload - ctx->coal_len;
}

static unsigned int lstp_tty_chars_in_buffer(struct tty_struct *tty)
{
	struct lstp_uart_ctx *ctx = tty->driver_data;
	struct lstp_channel *ch = ctx->ch;

	guard(spinlock_irqsave)(&ctx->write_lock);

	if (lstp_channel_disconnected(ch))
		return 0;

	unsigned int count = ctx->coal_len;

	if (ctx->tx_in_flight)
		count += ctx->tx_inflight_len;
	return count;
}

static void lstp_tty_flush_buffer(struct tty_struct *tty)
{
	struct lstp_uart_ctx *ctx = tty->driver_data;

	scoped_guard(spinlock_irqsave, &ctx->write_lock)
		ctx->coal_len = 0;
	/* Does not cancel an already in-flight TX. */
	cancel_work(&ctx->write_work);
}

/*
 * Firmware config is fixed. tty_termios_copy_hw() restores the speeds and all
 * of c_cflag but CREAD, HUPCL and CLOCAL; it leaves c_iflag alone, so the
 * flow-control bits are restored by hand below.
 */
static void lstp_tty_set_termios(struct tty_struct *tty, const struct ktermios *old_termios)
{
	struct lstp_uart_ctx *ctx = tty->driver_data;

	tty_termios_copy_hw(&tty->termios, old_termios);

	/* XON/XOFF is a property of the link, not something stty may change. */
	tty->termios.c_iflag &= ~(IXON | IXOFF | IXANY);
	tty->termios.c_iflag |= ctx->hw_iflag & (IXON | IXOFF | IXANY);
}

/* Firmware fires a fixed-duration break; any non-zero state fires it, as cdc-acm does. */
static int lstp_tty_break_ctl(struct tty_struct *tty, int state)
{
	struct lstp_uart_ctx *ctx = tty->driver_data;

	if (!state)
		return 0;

	/* Hangup clears initialized under port->mutex, so it waits out a break in flight. */
	guard(mutex)(&ctx->port.mutex);
	if (!tty_port_initialized(&ctx->port))
		return -EIO;

	return lstp_uart_send_with_retry(ctx, LSTP_UART_CMD_WRITE | LSTP_UART_TAG_GEN_BREAK, NULL,
					 0);
}

static int lstp_tty_get_icount(struct tty_struct *tty, struct serial_icounter_struct *icount)
{
	struct lstp_uart_ctx *ctx = tty->driver_data;

	/* tty_get_icount() has zeroed *icount; these are the fields the driver counts. */
	icount->dcd = READ_ONCE(ctx->iocount.dcd);
	icount->rx = READ_ONCE(ctx->iocount.rx);
	icount->tx = READ_ONCE(ctx->iocount.tx);
	icount->overrun = READ_ONCE(ctx->iocount.overrun);
	icount->brk = READ_ONCE(ctx->iocount.brk);
	icount->buf_overrun = READ_ONCE(ctx->iocount.buf_overrun);
	return 0;
}

static const struct tty_operations lstp_ops = {
	.install = lstp_tty_install,
	.open = lstp_tty_open,
	.close = lstp_tty_close,
	.cleanup = lstp_tty_cleanup,
	.hangup = lstp_tty_hangup,
	.write = lstp_tty_write,
	.write_room = lstp_tty_write_room,
	.chars_in_buffer = lstp_tty_chars_in_buffer,
	.flush_buffer = lstp_tty_flush_buffer,
	.set_termios = lstp_tty_set_termios,
	.break_ctl = lstp_tty_break_ctl,
	.get_icount = lstp_tty_get_icount,
};

/*
 * Set up the tty_port, coalescing buffers and minor for one bind. The bind
 * holds the port's initial reference, dropped by lstp_uart_remove(), and each
 * installed tty another, so the context and its minor last until the later of
 * the unbind and the last close.
 */
static int lstp_uart_init(struct lstp_channel *ch, const struct lstp_channel_init_info *info)
{
	if (info->config_len < sizeof(struct lstp_uart_config)) {
		dev_err(ch->dev, "%s: ch_%d: UART config too short: %zu < %zu\n", __func__,
			ch->ch_id, info->config_len, sizeof(struct lstp_uart_config));
		return -EINVAL;
	}

	struct lstp_uart_ctx *ctx = kzalloc_obj(*ctx);

	if (!ctx)
		return -ENOMEM;

	/* From here the only unwind is tty_port_put(); lstp_port_destruct() frees the rest. */
	tty_port_init(&ctx->port);
	ctx->port.ops = &lstp_port_ops;
	ctx->minor = -1;
	ctx->max_payload = ch->max_tx_payload;
	ctx->ch = lstp_channel_get(ch);
	spin_lock_init(&ctx->write_lock);

	int ret = lstp_uart_parse_config(ctx, ch, info->config);

	if (ret)
		goto err_put_port;

	ctx->coal_buf = kzalloc(ctx->max_payload, GFP_KERNEL);
	if (!ctx->coal_buf) {
		ret = -ENOMEM;
		goto err_put_port;
	}

	ctx->tx_snapshot = kzalloc(ctx->max_payload, GFP_KERNEL);
	if (!ctx->tx_snapshot) {
		ret = -ENOMEM;
		goto err_put_port;
	}

	ret = lstp_alloc_minor(ctx);
	if (ret < 0) {
		dev_err(ch->dev, "%s: ch_%d: Failed to allocate minor (%pe)\n", __func__, ch->ch_id,
			ERR_PTR(ret));
		goto err_put_port;
	}
	ctx->minor = ret;

	INIT_WORK(&ctx->write_work, lstp_uart_write_work);
	ch->priv = ctx;

	dev_dbg(ch->dev, "%s: ch_%d: Initialized as %s\n", __func__, ch->ch_id, ch->display_name);
	return 0;

err_put_port:
	tty_port_put(&ctx->port);
	return ret;
}

static int lstp_uart_start(struct lstp_channel *ch)
{
	struct lstp_uart_ctx *ctx = ch->priv;

	/*
	 * Mark registered before publishing: tty_port_register_device() adds
	 * the cdev and emits the uevent that sends udev straight to open(),
	 * and lstp_tty_install() refuses a minor that is not marked. No node
	 * exists at this index yet, so nothing can reach the map early.
	 */
	scoped_guard(mutex, &lstp_minors_lock)
		ctx->registered = true;

	struct device *tty_dev =
		tty_port_register_device(&ctx->port, lstp_tty_driver, ctx->minor, ch->dev);

	if (IS_ERR(tty_dev)) {
		dev_err(ch->dev, "%s: ch_%d: Could not register TTY (%pe)\n", __func__, ch->ch_id,
			tty_dev);
		scoped_guard(mutex, &lstp_minors_lock)
			ctx->registered = false;
		return PTR_ERR(tty_dev);
	}

	/* Nothing below may fail: really_probe() skips .remove() when .probe() errors. */
	dev_info(ch->dev, "%s: ch_%d: Started as %s\n", __func__, ch->ch_id, ch->display_name);
	return 0;
}

/*
 * Hang up the tty and unregister its device node, from the aux_driver
 * .remove(). The minor and its map entry stay with the context until its last
 * reference goes; clearing @registered is what refuses opens in between.
 */
static void lstp_uart_stop(struct lstp_channel *ch)
{
	struct lstp_uart_ctx *ctx = ch->priv;

	/*
	 * Clear @registered before sampling @port->tty, not after:
	 * tty_port_open() publishes the tty before it calls .activate(), so an
	 * open can go live between a sample and a later clear and never be
	 * hung up here.
	 */
	scoped_guard(mutex, &lstp_minors_lock)
		ctx->registered = false;

	struct tty_struct *tty = tty_port_tty_get(&ctx->port);

	if (tty) {
		/*
		 * Synchronous: fds move to hung_up_tty_fops and lstp_port_shutdown()
		 * cancels write_work, so no write is submitted after this returns.
		 */
		tty_vhangup(tty);
		tty_kref_put(tty);
	}

	tty_unregister_device(lstp_tty_driver, ctx->minor);
}

/*
 * Nodes come and go with channel binds. The core drops a minor's saved termios
 * when it is re-registered, so termios does not survive an unbind/bind cycle.
 */
static int __init lstp_uart_module_setup(void)
{
	lstp_tty_driver =
		tty_alloc_driver(LSTP_UART_MINORS, TTY_DRIVER_REAL_RAW | TTY_DRIVER_DYNAMIC_DEV);
	if (IS_ERR(lstp_tty_driver))
		return PTR_ERR(lstp_tty_driver);

	lstp_tty_driver->driver_name = "lstp";
	lstp_tty_driver->name = "ttyLSTP";
	lstp_tty_driver->major = 0;
	lstp_tty_driver->minor_start = 0;
	lstp_tty_driver->type = TTY_DRIVER_TYPE_SERIAL;
	lstp_tty_driver->subtype = SERIAL_TYPE_NORMAL;
	lstp_tty_driver->init_termios = tty_std_termios;
	/* Overridden per-device in port_activate; this is the visible default. */
	lstp_tty_driver->init_termios.c_cflag = B115200 | CS8 | CREAD | HUPCL | CLOCAL;
	tty_set_operations(lstp_tty_driver, &lstp_ops);

	int ret = tty_register_driver(lstp_tty_driver);

	if (ret) {
		tty_driver_kref_put(lstp_tty_driver);
		return ret;
	}

	return 0;
}

static void lstp_uart_module_teardown(void)
{
	tty_unregister_driver(lstp_tty_driver);
	tty_driver_kref_put(lstp_tty_driver);
	xa_destroy(&lstp_minors);
}

static int lstp_uart_probe(struct auxiliary_device *auxdev, const struct auxiliary_device_id *id)
{
	struct lstp_channel *ch = lstp_channel_from_auxdev(auxdev);
	const struct lstp_channel_init_info *info = dev_get_platdata(&auxdev->dev);

	if (!info)
		return -ENODEV;

	int ret = lstp_uart_init(ch, info);

	if (ret)
		return ret;

	struct lstp_uart_ctx *ctx = ch->priv;

	ret = lstp_uart_start(ch);
	if (ret)
		tty_port_put(&ctx->port);
	return ret;
}

/* A tty still installed keeps the context until its cleanup. */
static void lstp_uart_remove(struct auxiliary_device *auxdev)
{
	struct lstp_channel *ch = lstp_channel_from_auxdev(auxdev);
	struct lstp_uart_ctx *ctx = ch->priv;

	lstp_uart_stop(ch);
	tty_port_put(&ctx->port);
}

static const struct lstp_ops lstp_uart_ops = {
	.irq_packet = lstp_uart_irq_callback,
};

static const struct auxiliary_device_id lstp_uart_id_table[] = { { .name = "lstp.uart" }, {} };
MODULE_DEVICE_TABLE(auxiliary, lstp_uart_id_table);

static struct lstp_auxiliary_driver lstp_uart_drv = {
	.adrv = {
		.name = "uart",
		.probe = lstp_uart_probe,
		.remove = lstp_uart_remove,
		.id_table = lstp_uart_id_table,
	},
	.ops = &lstp_uart_ops,
};

/* The tty_driver goes first: a probe registers its channel's tty device on it. */
static int __init lstp_uart_module_init(void)
{
	int ret = lstp_uart_module_setup();

	if (ret)
		return ret;

	ret = lstp_auxiliary_driver_register(&lstp_uart_drv);
	if (ret)
		lstp_uart_module_teardown();

	return ret;
}
module_init(lstp_uart_module_init);

static void __exit lstp_uart_module_exit(void)
{
	lstp_auxiliary_driver_unregister(&lstp_uart_drv);
	lstp_uart_module_teardown();
}
module_exit(lstp_uart_module_exit);

MODULE_IMPORT_NS("LSTP");
MODULE_DESCRIPTION("LSTP UART driver");
MODULE_LICENSE("GPL");

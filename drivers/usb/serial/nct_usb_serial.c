// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2024-2025 Nuvoton Corp.
 * Copyright (C) 2024-2025 Sheng-Yuan Huang <syhuang3@nuvoton.com>
 *
 * Nuvoton USB to serial adapter driver
 *
 * This device interface consists of one control endpoint for configuration,
 * one bulk-out endpoint used for transmitting data for all serial ports,
 * and one bulk-in endpoint for receiving data from all serial ports.
 * The status of the ports may be reported via either an interrupt endpoint
 * or the bulk-in endpoint, depending on the device configuration.
 */

#include <linux/bitops.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/serial.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/tty.h>
#include <linux/tty_driver.h>
#include <linux/tty_flip.h>
#include <linux/uaccess.h>
#include <linux/usb.h>
#include <linux/usb/serial.h>
#include <linux/version.h>

#define NCT_VENDOR_ID			0x0416
#define NCT_PRODUCT_ID			0x200B
#define NCT_USB_CLASS			0xFF
#define NCT_USB_SUBCLASS		0x0
#define NCT_USB_PROTOCOL		0x1

#define NCT_MAX_VENDOR_READ_SIZE	8

static const struct usb_device_id id_table[] = {
	{USB_DEVICE_AND_INTERFACE_INFO(NCT_VENDOR_ID, NCT_PRODUCT_ID, NCT_USB_CLASS,
				       NCT_USB_SUBCLASS, NCT_USB_PROTOCOL)},
	{} /* Terminating entry */
};

#define NCT_DRVNAME "nct_mtuart"

MODULE_DEVICE_TABLE(usb, id_table);

#define NCT_MAX_SEND_BULK_SIZE 128
#define NCT_EMPTY_PORT	0xFF	/* The port does not exist in FW (For device status) */

/*
 * The max loop count when disconnecting for the
 * send-work
 */
#define NCT_DISCONN_QUEUE_LOOP_CNT	10

/* Hardware configure */
#define NCT_MAX_NUM_COM_DEVICES		8
#define NCT_MAX_PACKAGE_SIZE		4096 /* The max size of one writing package */
#define NCT_MAX_BULK_IN_SIZE		512
#define NCT_MAX_BULK_OUT_SIZE		512

#define NCT_DEFAULT_BAUD		14 /* 115200 */
static const unsigned int NCT_BAUD_SUP[] = {
	/* It should be the same as FW's baud-rate table */
	B0,	B50,	B75,	B150,	B300,	B600,	B1200,
	B1800,	B2400,	B4800,	B9600,	B19200,	B38400,	B57600,
	B115200, B230400, B460800, B921600, B1500000
};

/* USB request */
#define NCT_VENDOR_COM_READ_REQUEST_TYPE	0xc0
#define NCT_VENDOR_COM_WRITE_REQUEST_TYPE	0x40
#define NCT_VENDOR_COM_READ_REQUEST		0x01
#define NCT_VENDOR_COM_WRITE_REQUEST		0x01
/* Index definition */
enum {
	NCT_VCOM_INDEX_0 = 0,
	NCT_VCOM_INDEX_1,
	NCT_VCOM_INDEX_2,
	NCT_VCOM_INDEX_3,
	NCT_VCOM_INDEX_4,
	NCT_VCOM_INDEX_5,
	NCT_VCOM_INDEX_GLOBAL = 0xF,
};

/* Command */
enum {
	NCT_VCOM_GET_NUM_PORTS = 0,
	NCT_VCOM_GET_PORTS_SUPPORT,
	NCT_VCOM_GET_BAUD,
	NCT_VCOM_SET_INIT,
	NCT_VCOM_SET_CONFIG,
	NCT_VCOM_SET_BAUD,
	NCT_VCOM_SET_HCR,
	NCT_VCOM_SET_OPEN_PORT,
	NCT_VCOM_SET_CLOSE_PORT,
	NCT_VCOM_SILENT,
	/* Use bulk-in status instead of interrupt-in status */
	NCT_VCON_SET_BULK_IN_STATUS,
};

union nct_vendor_cmd {
	struct pkg0 {
		u16 index:4;
		u16 cmd:8;
	} p;
	u16 val;
} __packed;

#define NCT_HDR_MAGIC		0xA5
#define NCT_HDR_MAGIC2		0x5A
#define NCT_HDR_MAGIC_STATUS	0x5B

struct nct_packet_header {
	unsigned int magic:8;
	unsigned int magic2:8;
	unsigned int idx:4;
	unsigned int len:12;
} __packed;

/* The definitions are for the feilds of nct_ctrl_msg */
#define NCT_VCOM_1_STOP_BIT		0
#define NCT_VCOM_2_STOP_BITS		1
#define NCT_VCOM_PARITY_NONE		0
#define NCT_VCOM_PARITY_ODD		1
#define NCT_VCOM_PARITY_EVEN		2
#define NCT_VCOM_DL5			0
#define NCT_VCOM_DL6			1
#define NCT_VCOM_DL7			2
#define NCT_VCOM_DL8			3
#define NCT_VCOM_DISABLE_FLOW_CTRL	0
#define NCT_VCOM_XOFF			1
#define NCT_VCOM_RTS_CTS		2
union nct_ctrl_msg {
	struct pkg1 {
		u16 stop_bit:1;
		u16 parity:2;
		u16 data_len:2;
		u16 flow:2;
		u16 spd:5;
		u16 reserved:4;
	} p;
	u16 val;
} __packed;

#define NCT_USR_RDR	0x01
#define NCT_USR_ORR	0x02
#define NCT_USR_PBER	0x04
#define NCT_USR_NSER	0x08
#define NCT_USR_SBD	0x10
#define NCT_USR_TBRE	0x20
#define NCT_USR_TSRE	0x40
#define NCT_USR_RFEI	0x80
#define NCT_HSR_TCTS	0x01
#define NCT_HSR_TDSR	0x02
#define NCT_HSR_FERI	0x04
#define NCT_HSR_TDCD	0x08
#define NCT_HSR_CTS	0x10
#define NCT_HSR_DSR	0x20
#define NCT_HSR_RI	0x40
#define NCT_HSR_DCD	0x80
#define NCT_HCR_DTR	0x01
#define NCT_HCR_RTS	0x02
#define NCT_UART_STATE_MSR_MASK (NCT_HSR_TCTS | NCT_HSR_TDSR | NCT_HSR_TDCD | NCT_HSR_DCD)
struct nct_port_status {
	u8 index;
	u8 usr;
	u8 hsr;
	u8 hcr;
};

struct nct_serial {
	spinlock_t serial_lock; /* Protects the private data in structure 'usb_serial' */
	bool device_init;

	/* Reading data information */
	struct nct_tty_port *cur_port;
	int cur_len;

	bool status_trans_mode;
	u8 en_device_mask;
	u8 last_assigned_hw_idx;
	struct usb_endpoint_descriptor *bulk_out_ep;
};

struct nct_tty_port {
	union nct_ctrl_msg msg;

	unsigned long sysrq; /* Sysrq timeout */
	u8 hw_idx;
	u8 usr;
	u8 hsr;
	u8 hcr;
	/*
	 * Flow control - stop writing data to device.
	 * 0:Write enalbe, 1:Stop writing
	 */
	bool flow_stop_wrt;

	spinlock_t port_lock; /* Protects the port data */
	bool write_urb_in_use;
};

/* Functions */

/* Read from USB control pipe */
static int nct_vendor_read(struct usb_interface *intf, union nct_vendor_cmd cmd,
			   unsigned char *buf, int size)
{
	struct device *dev = &intf->dev;
	struct usb_device *udev = interface_to_usbdev(intf);
	u8 *tmp_buf;
	int res;

	tmp_buf = kmalloc(NCT_MAX_VENDOR_READ_SIZE, GFP_KERNEL);
	if (!tmp_buf)
		return -ENOMEM;

	if (size > NCT_MAX_VENDOR_READ_SIZE)
		dev_err(dev, NCT_DRVNAME ": %s - failed to read [%04x]: over size %d\n",
			__func__, cmd.p.cmd, size);

	res = usb_control_msg(udev, usb_rcvctrlpipe(udev, 0),
			      NCT_VENDOR_COM_READ_REQUEST,
			      NCT_VENDOR_COM_READ_REQUEST_TYPE,
			      cmd.val,
			      intf->cur_altsetting->desc.bInterfaceNumber,
			      tmp_buf, size, 100);

	if (res < 0) {
		dev_err(dev, NCT_DRVNAME ": %s - failed to read [%04x]: %d\n", __func__,
			cmd.p.cmd, res);

		kfree(tmp_buf);
		return res;
	}
	memcpy(buf, tmp_buf, res);
	kfree(tmp_buf);

	return res;
}

static int nct_vendor_write(struct usb_interface *intf, union nct_vendor_cmd cmd, u16 val)
{
	struct device *dev = &intf->dev;
	struct usb_device *udev = interface_to_usbdev(intf);
	int res;
	u8 *buf_val;

	buf_val = kmalloc(2, GFP_KERNEL);
	if (!buf_val)
		return -ENOMEM;

	/* Copy data to the buffer for sending */
	buf_val[0] = val & 0xff;
	buf_val[1] = (val >> 8) & 0xff;

	res = usb_control_msg(udev, usb_sndctrlpipe(udev, 0),
			      NCT_VENDOR_COM_WRITE_REQUEST,
			      NCT_VENDOR_COM_WRITE_REQUEST_TYPE,
			      cmd.val,
			      intf->cur_altsetting->desc.bInterfaceNumber,
			      buf_val,
			      2,
			      100);
	kfree(buf_val);
	if (res < 0)
		dev_err(dev, NCT_DRVNAME ": %s - failed to write [%04x]: %d\n",
			__func__, cmd.p.cmd, res);
	else
		res = 0; /* Set to 0 to align with the design. */

	return res;
}

static u16 nct_set_baud(struct usb_interface *intf, u16 index, unsigned int cflag)
{
	union nct_ctrl_msg msg;
	union nct_vendor_cmd cmd;
	u16 i;

	msg.val = 0;
	cmd.p.cmd = NCT_VCOM_SET_BAUD;
	msg.p.spd = NCT_DEFAULT_BAUD;
	cmd.p.index = index;
	dev_dbg(&intf->dev, NCT_DRVNAME ": %s tty baud: 0x%X\n", __func__,
		(cflag & CBAUD));
	for (i = 0; i < ARRAY_SIZE(NCT_BAUD_SUP); i++) {
		if ((cflag & CBAUD) == NCT_BAUD_SUP[i]) {
			msg.p.spd = i;
			dev_dbg(&intf->dev,
				NCT_DRVNAME ": %s index %d set baud: NCT_BAUD_SUP[%d]=%d\n",
				__func__, cmd.p.index, msg.p.spd, NCT_BAUD_SUP[i]);
			if (nct_vendor_write(intf, cmd, msg.val))
				dev_err(&intf->dev,
					NCT_DRVNAME ": %s - Set index: %d speed error\n",
					__func__, cmd.p.index);

			break;
		}
	}

	return msg.p.spd;
}

static void nct_serial_set_termios(struct tty_struct *tty,
				   struct usb_serial_port *port,
				   const struct ktermios *old)
{
	struct nct_tty_port *tport = usb_get_serial_port_data(port);
	struct usb_serial *serial = port->serial;
	struct usb_interface *intf = serial->interface;
	struct ktermios *termios = &tty->termios;
	union nct_ctrl_msg msg;
	union nct_vendor_cmd cmd;
	unsigned int cflag = termios->c_cflag;
	int ret;
	speed_t baud;

	baud = tty_get_baud_rate(tty);
	cmd.p.index = (u16)tport->hw_idx;
	cmd.p.cmd = NCT_VCOM_SET_CONFIG;
	msg.val = 0;
	msg.p.stop_bit =
	    (cflag & CSTOPB) ? (NCT_VCOM_2_STOP_BITS) : (NCT_VCOM_1_STOP_BIT);
	if (cflag & PARENB)
		msg.p.parity = (cflag & PARODD) ? (NCT_VCOM_PARITY_ODD) :
						  (NCT_VCOM_PARITY_EVEN);
	else
		msg.p.parity = NCT_VCOM_PARITY_NONE;

	switch (cflag & CSIZE) {
	case CS5:
		msg.p.data_len = NCT_VCOM_DL5;
		break;
	case CS6:
		msg.p.data_len = NCT_VCOM_DL6;
		break;
	case CS7:
		msg.p.data_len = NCT_VCOM_DL7;
		break;
	default:
	case CS8:
		msg.p.data_len = NCT_VCOM_DL8;
		break;
	}
	if (C_CRTSCTS(tty)) {
		msg.p.flow = NCT_VCOM_RTS_CTS;
		/* Flow control - Set flag of RTSCTS */
		tty_port_set_cts_flow(tty->port, true);

	} else if (I_IXON(tty)) {
		msg.p.flow = NCT_VCOM_XOFF;
		/* Flow control - Clear flag of RTSCTS */
		tty_port_set_cts_flow(tty->port, false);
	} else {
		msg.p.flow = NCT_VCOM_DISABLE_FLOW_CTRL;
		/* Flow control - Clear flag of RTSCTS */
		tty_port_set_cts_flow(tty->port, false);
	}
	ret = nct_vendor_write(intf, cmd, msg.val);
	if (ret)
		dev_err(&intf->dev,
			NCT_DRVNAME ": %s - Set index: %d set configure error\n",
			__func__, cmd.p.index);

	tport->msg.val = msg.val;

	/*
	 * Set baud if speed changed
	 * Note: 'nct_set_baud()' also send the speed to the FW
	 */
	if (!old ||
	    old->c_cflag != termios->c_cflag ||
	    old->c_ispeed != termios->c_ispeed ||
	    old->c_ospeed != termios->c_ospeed)
		tport->msg.p.spd = nct_set_baud(intf, cmd.p.index, cflag);

	tty_encode_baud_rate(tty, baud, baud);
}

static int nct_serial_break(struct tty_struct *tty, int break_state)
{
	struct usb_serial_port *port = tty->driver_data;
	struct nct_tty_port *tport = usb_get_serial_port_data(port);
	struct usb_serial *serial = port->serial;
	struct usb_interface *intf = serial->interface;
	union nct_vendor_cmd cmd;

	cmd.p.index = tport->hw_idx;
	cmd.p.cmd = NCT_VCOM_SILENT;

	return nct_vendor_write(intf, cmd, 0);
}

static int nct_tiocmset_helper(struct tty_struct *tty, unsigned int set,
			       unsigned int clear)
{
	struct usb_serial_port *port = tty->driver_data;
	struct nct_tty_port *tport = usb_get_serial_port_data(port);
	struct usb_serial *serial = port->serial;
	struct usb_interface *intf = serial->interface;
	union nct_ctrl_msg msg;
	union nct_vendor_cmd cmd;
	unsigned long flags;
	u8 hcr = 0;

	if (set & TIOCM_RTS)
		hcr |= NCT_HCR_RTS;
	if (set & TIOCM_DTR)
		hcr |= NCT_HCR_DTR;
	if (clear & TIOCM_RTS)
		hcr &= ~NCT_HCR_RTS;
	if (clear & TIOCM_DTR)
		hcr &= ~NCT_HCR_DTR;
	cmd.p.index = (u16)tport->hw_idx;
	cmd.p.cmd = NCT_VCOM_SET_HCR;
	msg.val = (u16)hcr;
	spin_lock_irqsave(&tport->port_lock, flags);
	tport->hcr = hcr;
	spin_unlock_irqrestore(&tport->port_lock, flags);

	dev_dbg(&intf->dev,
		NCT_DRVNAME ": %s: index/cmd/val(hcr)=%X, %X, %X [RTS=%X, DTR=%X]\n",
		__func__, cmd.p.index, cmd.p.cmd, msg.val, hcr & NCT_HCR_RTS,
		hcr & NCT_HCR_DTR);

	return nct_vendor_write(intf, cmd, msg.val);
}

static int nct_serial_tiocmget(struct tty_struct *tty)
{
	struct usb_serial_port *port = tty->driver_data;
	struct nct_tty_port *tport = usb_get_serial_port_data(port);
	struct usb_serial *serial = port->serial;
	struct usb_interface *intf = serial->interface;

	unsigned long flags;
	unsigned int res;
	u8 hcr, hsr;

	spin_lock_irqsave(&tport->port_lock, flags);
	hcr = tport->hcr;
	hsr = tport->hsr;
	spin_unlock_irqrestore(&tport->port_lock, flags);
	res = ((hcr & NCT_HCR_DTR) ? TIOCM_DTR : 0) |
		((hcr & NCT_HCR_RTS) ? TIOCM_RTS : 0) |
		((hsr & NCT_HSR_CTS) ? TIOCM_CTS : 0) |
		((hsr & NCT_HSR_DSR) ? TIOCM_DSR : 0) |
		((hsr & NCT_HSR_TDCD) ? TIOCM_RI : 0) |
		((hsr & NCT_HSR_DCD) ? TIOCM_CD : 0);

	dev_dbg(&intf->dev, NCT_DRVNAME ": DTR/RTS/CTS/DSR=%X,%X,%X,%X\n",
		(hcr & NCT_HCR_DTR), (hcr & NCT_HCR_RTS),
		(hsr & NCT_HSR_CTS), (hsr & NCT_HSR_DSR));

	return res;
}

static int nct_serial_tiocmset(struct tty_struct *tty, unsigned int set,
			       unsigned int clear)
{
	return nct_tiocmset_helper(tty, set, clear);
}

static void nct_rx_throttle(struct tty_struct *tty)
{
	unsigned int set;
	unsigned int clear = 0;

	/* If we are implementing RTS/CTS, control that line */
	if (C_CRTSCTS(tty)) {
		set = 0;
		clear = TIOCM_RTS;
		nct_tiocmset_helper(tty, set, clear);
	}
}

static void nct_rx_unthrottle(struct tty_struct *tty)
{
	unsigned int set;
	unsigned int clear = 0;

	/* If we are implementing RTS/CTS, control that line */
	if (C_CRTSCTS(tty)) {
		set = 0;
		set |= TIOCM_RTS;
		nct_tiocmset_helper(tty, set, clear);
	}
}

static int nct_serial_write_data(struct tty_struct *tty, struct usb_serial_port *port,
				 const unsigned char *buf, int count)
{
	int ret;
	unsigned long flags;
	struct nct_packet_header hdr;
	int wr_len;
	struct nct_tty_port *tport = usb_get_serial_port_data(port);

	wr_len = min((unsigned int)count, NCT_MAX_SEND_BULK_SIZE - sizeof(hdr));

	if (!wr_len)
		return 0;

	spin_lock_irqsave(&tport->port_lock, flags);

	if (tport->write_urb_in_use) {
		spin_unlock_irqrestore(&tport->port_lock, flags);
		return 0;
	}

	/* Fill header */
	hdr.magic = NCT_HDR_MAGIC;
	hdr.magic2 = NCT_HDR_MAGIC2;
	hdr.idx = tport->hw_idx; /* The 'hw_idx' is based on 1 */

	/* Copy data */
	memcpy(port->write_urb->transfer_buffer + sizeof(hdr),
	       (const void *)buf, wr_len);

	hdr.len = wr_len; /* File filed 'len' of header */

	/* Filled urb data */
	memcpy(port->write_urb->transfer_buffer, (const void *)&hdr,
	       sizeof(hdr)); /* Copy header after filling all other fields */

	/* Set urb length(Total length) */
	port->write_urb->transfer_buffer_length = wr_len + sizeof(hdr);

	port->write_urb->transfer_flags |= URB_ZERO_PACKET;

	ret = usb_submit_urb(port->write_urb, GFP_ATOMIC);
	if (ret < 0) {
		dev_err(&port->dev,
			NCT_DRVNAME ": %s: usb_submit_urb failed, ret=%d, hw_idx=%d\n",
			__func__, ret, tport->hw_idx);
	} else {
		tport->write_urb_in_use = true; /* Set it as busy */
		ret = wr_len + sizeof(hdr);
	}

	spin_unlock_irqrestore(&tport->port_lock, flags);

	if (ret > sizeof(hdr))
		ret = ret - sizeof(hdr);

	dev_dbg(&port->dev, NCT_DRVNAME ": %s: returning %d\n", __func__, ret);
	return ret;
}

static int nct_serial_write(struct tty_struct *tty, struct usb_serial_port *port,
			    const unsigned char *buf, int count)
{
	struct nct_tty_port *tport = usb_get_serial_port_data(port);

	if (!port) {
		pr_err(NCT_DRVNAME ": %s: port is NULL!\n", __func__);
		return -EIO;
	}
	if (!port->write_urb) {
		dev_err(&port->dev, NCT_DRVNAME ": %s: write_urb not initialized!\n",
			__func__);
		return -EIO;
	}
	if (!port->write_urb->transfer_buffer) {
		dev_err(&port->dev, NCT_DRVNAME ": %s: transfer_buffer not initialized!\n",
			__func__);
		return -EIO;
	}

	/* Flow control */
	if (tty_port_cts_enabled(tty->port))
		if (tport->flow_stop_wrt)
			return 0;

	return nct_serial_write_data(tty, port, buf, count);
}

static void nct_write_bulk_callback(struct urb *urb)
{
	struct usb_serial_port *port = urb->context;
	struct nct_tty_port *tport;
	unsigned long flags;
	int status = urb->status;

	/* Port and serial sanity check */
	if (!port) {
		pr_err(NCT_DRVNAME ": %s: port is NULL, status=%d\n",
		       __func__, status);
		return;
	}

	tport = usb_get_serial_port_data(port);
	if  (!tport) {
		dev_err(&port->dev, NCT_DRVNAME ": %s: port->private is NULL, status=%d\n",
			__func__, status);
		return;
	}

	spin_lock_irqsave(&tport->port_lock, flags);
	tport->write_urb_in_use = false;
	spin_unlock_irqrestore(&tport->port_lock, flags);

	tty_port_tty_wakeup(&port->port);
}

static unsigned int nct_write_room(struct tty_struct *tty)
{
	struct usb_serial_port *port = tty->driver_data;
	struct nct_tty_port *tport = usb_get_serial_port_data(port);
	unsigned long flags;
	unsigned int room;

	spin_lock_irqsave(&tport->port_lock, flags);
	if (tport->write_urb_in_use)
		room = 0;
	else
		room = NCT_MAX_SEND_BULK_SIZE - sizeof(struct nct_packet_header);
	spin_unlock_irqrestore(&tport->port_lock, flags);

	dev_dbg(&port->dev, NCT_DRVNAME ": %s: port=%d, room=%u\n", __func__,
		tport->hw_idx, room);
	return room;
}

static unsigned int nct_chars_in_buffer(struct tty_struct *tty)
{
	struct usb_serial_port *port = tty->driver_data;
	struct nct_tty_port *tport = usb_get_serial_port_data(port);
	unsigned long flags;
	unsigned int chars;

	spin_lock_irqsave(&tport->port_lock, flags);
	if (tport->write_urb_in_use)
		chars = NCT_MAX_SEND_BULK_SIZE - sizeof(struct nct_packet_header);
	else
		chars = 0;
	spin_unlock_irqrestore(&tport->port_lock, flags);

	dev_dbg(&port->dev, NCT_DRVNAME ": %s: port=%d, chars=%d\n", __func__,
		tport->hw_idx, chars);
	return chars;
}

/*
 *  Starts reads urb on all ports. It is to avoid potential issues caused by
 *  multiple ports being opened almost simultaneously.
 *  It must be called AFTER startup, with urbs initialized.
 *  Returns 0 if successful, non-zero error otherwise.
 */
static int nct_startup_device(struct usb_serial *serial)
{
	int ret = 0;
	struct nct_serial *serial_priv = usb_get_serial_data(serial);
	struct usb_serial_port *port;
	unsigned long flags;

	/* Be sure this happens exactly once */
	spin_lock_irqsave(&serial_priv->serial_lock, flags);

	if (serial_priv->device_init) {
		spin_unlock_irqrestore(&serial_priv->serial_lock, flags);
		return 0;
	}
	serial_priv->device_init = true;
	spin_unlock_irqrestore(&serial_priv->serial_lock, flags);

	/* Start reading from bulk in endpoint */
	port = serial->port[0];
	if (!port->read_urb)
		dev_dbg(&port->dev, NCT_DRVNAME ": %s: port->read_urb is null, index=%d\n",
			__func__, 0);

	ret = usb_submit_urb(port->read_urb, GFP_KERNEL);
	if (ret)
		dev_err(&port->dev,
			NCT_DRVNAME ": %s: usb_submit_urb failed, ret=%d, port=%d\n",
			__func__, ret, 0);

	/* For getting status from interrupt-in */
	if (!serial_priv->status_trans_mode) {
		/* Start reading from interrupt pipe */
		port = serial->port[0];
		ret = usb_submit_urb(port->interrupt_in_urb, GFP_KERNEL);
		if (ret)
			dev_err(&port->dev,
				NCT_DRVNAME ": %s: usb_submit_urb(intr) failed, ret=%d, port=%d\n",
				__func__, ret, 0);
	}
	return ret;
}

static void nct_serial_port_end(struct usb_serial_port *port)
{
	struct nct_tty_port *tport = usb_get_serial_port_data(port);
	struct usb_serial *serial = port->serial;
	struct usb_interface *intf = serial->interface;
	union nct_ctrl_msg msg;
	union nct_vendor_cmd cmd;

	/* Send 'Close Port' to the device */
	cmd.p.index = (u16)tport->hw_idx;
	cmd.p.cmd = NCT_VCOM_SET_CLOSE_PORT;
	msg.val = 0;
	if (!intf) {
		pr_err(NCT_DRVNAME ": %s: No intf => do not send 'close' event\n",
		       __func__);
		return;
	}
	nct_vendor_write(intf, cmd, msg.val);
}

static int nct_open(struct tty_struct *tty, struct usb_serial_port *port)
{
	union nct_vendor_cmd cmd;
	union nct_ctrl_msg msg;
	struct nct_tty_port *tport = usb_get_serial_port_data(port);
	struct usb_serial *serial = port->serial;
	struct nct_serial *serial_priv = usb_get_serial_data(serial);
	struct usb_interface *intf = serial->interface;

	if (!port->serial)
		return -ENXIO;

	/* Allocate write_urb */
	if (!port->write_urb) {
		port->write_urb = usb_alloc_urb(0, GFP_KERNEL);
		if (!port->write_urb) {
			dev_err(&port->dev, NCT_DRVNAME ": %s: Failed to allocate write URB\n",
				__func__);
			return -ENOMEM;
		}
	}

	/* Allocate bulk_out_buffer */
	port->write_urb->transfer_buffer = kmalloc(NCT_MAX_SEND_BULK_SIZE, GFP_KERNEL);
	if (!port->write_urb->transfer_buffer) {
		usb_free_urb(port->write_urb);
		port->write_urb = NULL;
		return -ENOMEM;
	}

	/* Clear(init) buffer */
	memset(port->write_urb->transfer_buffer, 0, NCT_MAX_SEND_BULK_SIZE);

	/* Set write_urb */
	usb_fill_bulk_urb(port->write_urb, serial->dev,
			  usb_sndbulkpipe(serial->dev, serial_priv->bulk_out_ep->bEndpointAddress),
			  port->write_urb->transfer_buffer, NCT_MAX_SEND_BULK_SIZE,
			  nct_write_bulk_callback, port);

	/* Be sure the device is started up */
	if (nct_startup_device(port->serial) != 0)
		return -ENXIO;

	cmd.p.index = (u16)tport->hw_idx;
	cmd.p.cmd = NCT_VCOM_SET_OPEN_PORT;
	msg.val = 0;
	nct_vendor_write(intf, cmd, msg.val);
	/*
	 * Delay 1ms for firmware to configure hardware after opening the port.
	 * (Especially at high speed)
	 */
	usleep_range(1000, 2000);
	return 0;
}

static void nct_close(struct usb_serial_port *port)
{
	struct nct_tty_port *tport = usb_get_serial_port_data(port);
	unsigned long flags;

	mutex_lock(&port->serial->disc_mutex);
	/* If disconnected, don't send the close-command to the firmware */
	if (port->serial->disconnected)
		goto exit;

	nct_serial_port_end(port);

exit:
	/* Shutdown any outstanding bulk writes */
	usb_kill_urb(port->write_urb);

	/* Free transfer_buffer */
	kfree(port->write_urb->transfer_buffer);
	port->write_urb->transfer_buffer = NULL;

	if (tport) {
		spin_lock_irqsave(&tport->port_lock, flags);
		tport->write_urb_in_use = false;
		spin_unlock_irqrestore(&tport->port_lock, flags);
	}

	mutex_unlock(&port->serial->disc_mutex);
}

static void nct_update_status(struct usb_serial *serial, unsigned char *data)
{
	struct nct_port_status *nps = (struct nct_port_status *)data;
	struct usb_interface *intf = serial->interface;
	struct nct_tty_port *tport;
	struct tty_struct *tty;
	struct usb_serial_port *port;
	unsigned long flags;
	bool found;
	int i;

	if (nps->index >= NCT_MAX_NUM_COM_DEVICES) {
		if (nps->index != NCT_EMPTY_PORT) /* Un-used port */
			dev_warn(&intf->dev,
				 NCT_DRVNAME ": %s: Receive wrong H/W index\n", __func__);
		return;
	}
	if (!(nps->hsr & (NCT_UART_STATE_MSR_MASK | NCT_HSR_CTS)))
		return; /* No any state changed. */
	tport = NULL;
	found = false;
	for (i = 0; i < serial->type->num_ports; i++) {
		port = serial->port[i];

		if (!port) {
			dev_err(&intf->dev, NCT_DRVNAME ": %s: port[%d] is NULL\n",
				__func__, i);
			continue;
		}

		tport = usb_get_serial_port_data(port);

		if (!tport) {
			dev_err(&intf->dev,
				NCT_DRVNAME ": %s: Get NULL port data for port[%d]\n",
				__func__, i);
			continue;
		}

		if (tport->hw_idx == nps->index) {
			found = true;
			break;
		}
	}

	if (!found) {
		dev_err(&intf->dev,
			NCT_DRVNAME ": %s: Decode serial packet index failed.\n",
			__func__);
		return;
	}

	if (!tport) {
		dev_err(&intf->dev,
			NCT_DRVNAME ": %s: Decode serial packet index failed.\n",
			__func__);
		return;
	}

	spin_lock_irqsave(&tport->port_lock, flags);
	tport->usr = nps->usr;
	tport->hsr = nps->hsr;
	tport->hcr = nps->hcr;
	tport->sysrq = (tport->sysrq & ~0x01) | (-(nps->usr & NCT_USR_SBD) & 0x01);
	spin_unlock_irqrestore(&tport->port_lock, flags);

	if (serial->disconnected) {
		dev_err(&intf->dev,
			NCT_DRVNAME ": %s: Device disconnected, skipping update_status\n",
			__func__);
		return;
	}

	tty = tty_port_tty_get(&port->port);
	if (!tty)
		return; /* The port has been closed. */

	if (nps->hsr & NCT_UART_STATE_MSR_MASK) {
		if (nps->hsr & NCT_HSR_DCD) {
			if (tty) {
				struct tty_ldisc *ld = tty_ldisc_ref(tty);

				if (ld) {
					if (ld->ops->dcd_change)
						ld->ops->dcd_change(tty, 0x01);
					tty_ldisc_deref(ld);
				}
				wake_up_interruptible(&tty->port->open_wait);
			}
		}
	}

	/* Flow control */
	if (tty_port_cts_enabled(&port->port)) {
		if ((nps->hsr & NCT_HSR_CTS)) {
			if (tport->flow_stop_wrt)
				tport->flow_stop_wrt = false;
		} else {
			tport->flow_stop_wrt = true;
		}
	}

	tty_kref_put(tty);
}

static void nct_usb_serial_read(struct urb *urb)
{
	struct usb_serial_port *port =  urb->context;
	struct usb_serial *serial = port->serial;
	struct usb_interface *intf = serial->interface;
	struct nct_serial *serial_priv = usb_get_serial_data(serial);
	struct nct_tty_port *tport;
	struct nct_packet_header *hdr = NULL;
	unsigned char *data = urb->transfer_buffer;
	int i, j;
	int actual_len = urb->actual_length;
	int len = 0;
	struct nct_port_status *nps;
	unsigned long flags;

	if (!urb->actual_length)
		return;

again:
	spin_lock_irqsave(&serial_priv->serial_lock, flags);
	tport = serial_priv->cur_port;
	if (!tport) {
		/*
		 * Handle a new data package (i.e., it is not
		 * the remaining data without a header).
		 * The package does not need to be combined this time.
		 */

		for (i = 0; i < urb->actual_length; i++) {
			hdr = (struct nct_packet_header *)data;
			/* Decode the header */

			if (serial_priv->status_trans_mode) {
				/*
				 * Status data is also transmitted via bulk-in
				 * pipe.
				 */
				if (hdr->magic == NCT_HDR_MAGIC &&
				    hdr->magic2 == NCT_HDR_MAGIC_STATUS &&
				    hdr->len == 24 && actual_len >= 28) {
					/*
					 * Notice: actual_len will be decreased,
					 * it is equal to urb->actual_length
					 * only at the beginning.
					 */

					/*
					 * Status report.
					 * It should be a standalone package in
					 * one URB
					 */
					data += sizeof(struct nct_packet_header);
					actual_len -=
						sizeof(struct nct_packet_header);

					nps = (struct nct_port_status *)data;

					for (j = 0; j < actual_len - 4; j++) {
						nct_update_status(serial,
								  (unsigned char *)nps);
						nps++;
					}

					spin_unlock_irqrestore(&serial_priv->serial_lock, flags);
					return;
				}
			}

			if (hdr->magic == NCT_HDR_MAGIC &&
			    hdr->magic2 == NCT_HDR_MAGIC2 &&
			    hdr->idx <= NCT_MAX_NUM_COM_DEVICES &&
			    hdr->len <= 512)
				break;

			data++;
			actual_len--;
			if (!actual_len) {
				dev_err(&intf->dev, NCT_DRVNAME
					": %s: Decode serial packet size failed.\n", __func__);
				spin_unlock_irqrestore(&serial_priv->serial_lock, flags);
				return;
			}
		}
		/*
		 * Search tty port
		 * Search the tty device by the idx in header, and check if
		 * it is registered or opened.
		 * If it is, record them. The record will be used later for
		 * 2 purposes:
		 *   (1) If the current data package is incomplete, the following
		 *       incoming data will not include a header.
		 *   (2) To determine which device will be used for transmission.
		 */
		tport = NULL;
		for (i = 0; i < serial->type->num_ports; i++) {
			port = serial->port[i];
			tport = usb_get_serial_port_data(port);
			if (tport->hw_idx != hdr->idx)
				continue;

			break;
		}
		if (!tport) {
			dev_err(&intf->dev,
				NCT_DRVNAME ": %s: Decode serial packet index failed.\n",
				__func__);
			spin_unlock_irqrestore(&serial_priv->serial_lock, flags);
			return;
		}
		/*
		 * Calculate the data length.
		 * Then, check if the length specified in the header matches
		 * the data length. If not, it indicates that the data we
		 * received spans across two (or more) packets.
		 */
		actual_len -= sizeof(struct nct_packet_header);
		data += sizeof(struct nct_packet_header);
		/* actual_len: the data length of the data we got this time */
		if (hdr->len > actual_len) {
			/*
			 * It means the length specified in the header (the
			 * custom header) is greater than the length of the
			 * data we received.
			 * Therefore, the data we received this time does not
			 * span across another packet (i.e. no new header).
			 */
			len = actual_len;
			/*
			 * cur_len: Record how many data does not handle yet
			 */
			serial_priv->cur_len = hdr->len - len;
			/*
			 * Record the current port. When we got remained data of
			 * the package next time
			 */
			serial_priv->cur_port = tport;
		} else {
			/*
			 * The data we got crosses packages(not belong
			 * to the same header). We only handle data by
			 * the length in header. And we will handle
			 * another package when 'goto "again" '.
			 */
			len = hdr->len;
		}
	} else { /*  Handling the remained data which crosses package */
		if (serial_priv->cur_len > actual_len) {
			/*
			 * The unhandled part of the data exceeds the data we
			 * received this time. We only handle the data we
			 * have, expecting more data to be received later.
			 */
			len = actual_len;
		} else {
			/*
			 * This means the package has been fully handled.
			 * Clear 'cur_port' as no additional data needs to be
			 * attached to the current package.
			 */
			len = serial_priv->cur_len;
			serial_priv->cur_port = NULL;
		}
		serial_priv->cur_len -= len;
	}
	spin_unlock_irqrestore(&serial_priv->serial_lock, flags);
	/*
	 * The per character mucking around with sysrq path it too slow for
	 * stuff like 3G modems, so shortcircuit it in the 99.9999999% of
	 * cases where the USB serial is not a console anyway.
	 */
	if (tport->sysrq) {
		for (i = 0; i < len; i++, data++)
			tty_insert_flip_char(&port->port, *data, TTY_NORMAL);
	} else {
		tty_insert_flip_string(&port->port, data, len);
		data += len;
	}
	/*
	 * Send data to the tty device (according to the port identified above).
	 */
	tty_flip_buffer_push(&port->port);
	actual_len -= len;

	/*
	 * It means that the data we received this time contains two or
	 * more data packages, so it needs to continue processing the next
	 * data packages.
	 */
	if (actual_len > 0)
		goto again;
}

static void nct_process_read_bulk(struct urb *urb)
{
	struct usb_serial_port *port =  urb->context;
	bool stopped = false;
	int status = urb->status;
	int ret;

	switch (status) {
	case 0:
		nct_usb_serial_read(urb);
		break;
	case -ENOENT:
	case -ECONNRESET:
	case -ESHUTDOWN:
		dev_dbg(&port->dev, NCT_DRVNAME ": %s - urb stopped: %d\n",
			__func__, status);
		stopped = true;
		break;
	case -EPIPE:
		dev_dbg(&port->dev, NCT_DRVNAME ": %s - urb stopped: %d\n",
			__func__, status);
		stopped = true;
		break;
	case -ETIME:
		dev_dbg(&port->dev, NCT_DRVNAME ": %s - urb ETIME t: %d\n",
			__func__, status);
		break;
	case -ETIMEDOUT:
		dev_dbg(&port->dev, NCT_DRVNAME ": %s - urb ETIMEDOUT t: %d\n",
			__func__, status);
		break;
	default:
		dev_dbg(&port->dev, NCT_DRVNAME ": %s - nonzero urb status: %d\n",
			__func__, status);
		break;
	}

	if (stopped)
		return;

	ret = usb_submit_urb(urb, GFP_ATOMIC);
	if (ret != 0 && ret != -EPERM)
		dev_err(&port->dev,
			NCT_DRVNAME ": %s: failed resubmitting urb, ret=%d\n",
			__func__, ret);
}

static void nct_read_bulk_callback(struct urb *urb)
{
	struct usb_serial_port *port = urb->context;
	struct nct_serial *serial_priv;

	/* Port sanity check, do not resubmit if port is not valid */
	if (urb->status == -ESHUTDOWN)
		return;

	if (!port) {
		pr_err(NCT_DRVNAME ": %s: port or serial is NULL, status=%d\n",
		       __func__, urb->status);
		return;
	}

	if (!port->serial) {
		dev_err(&port->dev, NCT_DRVNAME ": %s: serial is NULL, status=%d\n",
			__func__, urb->status);
		return;
	}

	if (port->serial->disconnected)
		return;

	serial_priv = usb_get_serial_port_data(port);
	if (!serial_priv) {
		dev_err(&port->dev, NCT_DRVNAME ": %s: port->private is NULL, status=%d\n",
			__func__, urb->status);
		return;
	}

	if (!port->serial) {
		dev_err(&port->dev, NCT_DRVNAME ": %s: serial is NULL, status=%d\n",
			__func__, urb->status);
		return;
	}

	serial_priv = usb_get_serial_data(port->serial);
	if (!serial_priv) {
		dev_err(&port->dev,
			NCT_DRVNAME ": %s: serial->private is NULL, status=%d\n",
			__func__, urb->status);
		return;
	}

	/* Processing data */
	nct_process_read_bulk(urb);
}

static int nct_usb_attach(struct usb_serial *serial)
{
	return 0;
}

static int nct_calc_num_ports(struct usb_serial *serial,
			      struct usb_serial_endpoints *epds)
{
	union nct_vendor_cmd cmd;
	u8 buf[8];
	struct nct_serial *serial_priv = usb_get_serial_data(serial);
	struct usb_interface *intf = serial->interface;
	int ret;
	int i;
	int num_ports;

	//Send init command
	cmd.p.index = NCT_VCOM_INDEX_GLOBAL;
	cmd.p.cmd = NCT_VCOM_SET_INIT;
	ret = nct_vendor_write(intf, cmd, 0);
	if (ret) {
		dev_err(&intf->dev,
			NCT_DRVNAME ": %s - Set COM init error\n",
			__func__);
		return ret;
	}

	/* Get ports' index supported by the device(/FW) */
	cmd.p.index = NCT_VCOM_INDEX_GLOBAL;
	cmd.p.cmd = NCT_VCOM_GET_PORTS_SUPPORT;
	ret = nct_vendor_read(intf, cmd, buf, 1);
	if (ret != 1) {
		dev_err(&intf->dev,
			NCT_DRVNAME ": %s - Get COM port index error\n",
			__func__);
		return 0;
	}
	serial_priv->en_device_mask = buf[0];
	serial_priv->last_assigned_hw_idx = 0; /* Note: hw_idx is based on 1 */
	dev_info(&intf->dev, NCT_DRVNAME ": %s Enabled devices mask:%X\n",
		 __func__, buf[0]);

	for (i = 0, num_ports = 0; i < NCT_MAX_NUM_COM_DEVICES; i++) {
		if ((buf[0] & (1 << i)) == 0)
			continue;	/* The port is disabled */

		num_ports++;
	}

	return num_ports;
}

static int nct_probe(struct usb_serial *serial, const struct usb_device_id *id)
{
	struct nct_serial *serial_priv;
	int i;
	struct usb_endpoint_descriptor *endpoint;
	struct usb_interface *intf = serial->interface;
	struct usb_host_interface *iface_desc;

	serial_priv = kzalloc(sizeof(*serial_priv), GFP_KERNEL);
	if (!serial_priv)
		return -ENOMEM;

	spin_lock_init(&serial_priv->serial_lock);
	usb_set_serial_data(serial, serial_priv);

	iface_desc = intf->cur_altsetting;

	/* For bulk-out */
	for (i = 0; i < iface_desc->desc.bNumEndpoints; i++) {
		endpoint = &iface_desc->endpoint[i].desc;
		if (usb_endpoint_is_bulk_out(endpoint)) {
			serial_priv->bulk_out_ep = endpoint;
			break;
		}
	}

	/*
	 * Initialize the mode as 'Status data is transmitted via
	 * bulk-in pipe'.
	 */
	serial_priv->status_trans_mode = true;
	serial->type->num_interrupt_in = 0;
	for (i = 0; i < iface_desc->desc.bNumEndpoints; i++) {
		endpoint = &iface_desc->endpoint[i].desc;

		if (usb_endpoint_is_int_in(endpoint)) {
			/* Status data is transmitted via interrupt-in pipe. */
			serial_priv->status_trans_mode = false;
			serial->type->num_interrupt_in = 1;
			break;
		}
	}

	return 0;
}

static int nct_port_init(struct usb_serial_port *port, unsigned int port_num)
{
	struct nct_tty_port *tport;
	struct usb_serial *serial = port->serial;
	struct nct_serial *serial_priv = usb_get_serial_data(serial);
	unsigned long flags;
	int i;

	tport = kzalloc(sizeof(*tport), GFP_KERNEL);
	if (!tport)
		return -ENOMEM;

	/* Assigned the hw_idx */
	spin_lock_init(&tport->port_lock);

	spin_lock_irqsave(&tport->port_lock, flags);
	for (i = serial_priv->last_assigned_hw_idx + 1; i < NCT_MAX_NUM_COM_DEVICES; i++) {
		if ((serial_priv->en_device_mask & (1 << i)) == 0)
			continue;	/* the port is disabled */

		tport->hw_idx = i;
		serial_priv->last_assigned_hw_idx = i;
		break;
	}
	spin_unlock_irqrestore(&tport->port_lock, flags);

	usb_set_serial_port_data(port, tport);

	return 0;
}

static void nct_interrupt_in_callback(struct urb *urb)
{
	struct usb_serial_port *port = urb->context;
	int status = urb->status;
	struct usb_serial *serial = port->serial;

	unsigned char *data = urb->transfer_buffer;
	int retval;
	int i;
	int actual_len = urb->actual_length;
	struct nct_port_status *nps;

	switch (status) {
	case 0:
		/* Success */
		if ((actual_len % 4) != 0)
			return;

		nps = (struct nct_port_status *)data;

		for (i = 0; i < (actual_len / 4); i++) {
			nct_update_status(serial, (unsigned char *)nps);
			nps++;
		}
		break;

	case -ECONNRESET:
	case -ENOENT:
	case -ESHUTDOWN:
		/* This urb is terminated, clean up */
		return;
	default:
		break;
	}
	retval = usb_submit_urb(urb, GFP_ATOMIC);
	if (retval)
		dev_err(&port->dev,
			NCT_DRVNAME ": %s: Submit intr URB failed, ret=%d\n",
			__func__, retval);
}

static void nct_disconnect(struct usb_serial *serial)
{
	int i;
	struct nct_serial *serial_priv = usb_get_serial_data(serial);
	unsigned long flags;

	/* Reset status */
	spin_lock_irqsave(&serial_priv->serial_lock, flags);
	serial_priv->device_init = false;
	serial_priv->cur_port = NULL;
	serial_priv->cur_len = 0;
	spin_unlock_irqrestore(&serial_priv->serial_lock, flags);

	/* Stop reads and writes on all ports */
	for (i = 0; i < serial->type->num_ports; i++) {
		if (!serial->port[i])
			continue;
		usb_kill_urb(serial->port[i]->read_urb);
		usb_kill_urb(serial->port[i]->write_urb);
	}
	if (!serial_priv->status_trans_mode)
		if (serial->port[0] && serial->port[0]->interrupt_in_urb)
			usb_kill_urb(serial->port[0]->interrupt_in_urb);
}

static int nct_port_probe(struct usb_serial_port *port)
{
	return nct_port_init(port, port->port_number);
}

static void nct_port_remove(struct usb_serial_port *port)
{
	struct nct_tty_port *priv;

	priv = usb_get_serial_port_data(port);
	kfree(priv);
}

static void nct_release(struct usb_serial *serial)
{
	struct nct_serial *serial_priv;

	serial_priv = usb_get_serial_data(serial);
	kfree(serial_priv);
}

static int nct_suspend(struct usb_serial *serial, pm_message_t message)
{
	struct nct_serial *serial_priv = usb_get_serial_data(serial);
	int i;

	/* Stop all URBs */
	for (i = 0; i < serial->type->num_ports; i++) {
		if (serial->port[i]) {
			usb_kill_urb(serial->port[i]->read_urb);
			usb_kill_urb(serial->port[i]->write_urb);
		}
	}

	if (!serial_priv->status_trans_mode)
		if (serial->port[0] && serial->port[0]->interrupt_in_urb)
			usb_kill_urb(serial->port[0]->interrupt_in_urb);

	return 0;
}

static int nct_resume(struct usb_serial *serial)
{
	struct nct_serial *serial_priv = usb_get_serial_data(serial);
	struct usb_interface *intf = serial->interface;
	struct usb_host_interface *iface_desc;
	struct usb_endpoint_descriptor *endpoint;
	unsigned long flags;
	int i, ret = 0;

	/* Reacquire endpoint descriptors */
	iface_desc = intf->cur_altsetting;
	for (i = 0; i < iface_desc->desc.bNumEndpoints; i++) {
		endpoint = &iface_desc->endpoint[i].desc;
		if (usb_endpoint_is_bulk_out(endpoint)) {
			serial_priv->bulk_out_ep = endpoint;
			break;
		}
	}

	/* Reset driver internal state */
	spin_lock_irqsave(&serial_priv->serial_lock, flags);
	serial_priv->cur_port = NULL;
	serial_priv->cur_len = 0;
	spin_unlock_irqrestore(&serial_priv->serial_lock, flags);

	/* Resubmit URBs */
	if (serial->port[0] && serial->port[0]->read_urb) {
		ret = usb_submit_urb(serial->port[0]->read_urb, GFP_KERNEL);
		if (ret)
			dev_err(&intf->dev,
				NCT_DRVNAME ": %s: Submit read URB failed, ret=%d\n",
				__func__, ret);
	}

	if (!serial_priv->status_trans_mode) {
		if (serial->port[0] && serial->port[0]->interrupt_in_urb) {
			ret = usb_submit_urb(serial->port[0]->interrupt_in_urb,
					     GFP_KERNEL);
			if (ret)
				dev_err(&intf->dev,
					NCT_DRVNAME ": %s: Submit interrupt URB failed, ret=%d\n",
					__func__, ret);
		}
	}

	/* Restore status flags */
	spin_lock_irqsave(&serial_priv->serial_lock, flags);
	serial_priv->device_init = true;  /* Reset initialization flag */
	spin_unlock_irqrestore(&serial_priv->serial_lock, flags);

	return 0;
}

static struct usb_serial_driver nct_usb_serial_device = {
	.driver	= {
		.name =		NCT_DRVNAME,
	},
	.description =		"Nuvoton USB to serial adapter",
	.id_table =		id_table,
	.num_ports =		6,
	.num_bulk_in =		1,
	.num_bulk_out =		1,
	.open =			nct_open,
	.close =		nct_close,
	.write =		nct_serial_write,
	.write_room =		nct_write_room,
	.write_bulk_callback =	nct_write_bulk_callback,
	.read_bulk_callback =	nct_read_bulk_callback,
	.read_int_callback =	nct_interrupt_in_callback,
	.chars_in_buffer =	nct_chars_in_buffer,
	.throttle =		nct_rx_throttle,
	.unthrottle =		nct_rx_unthrottle,
	.probe =		nct_probe,
	.calc_num_ports =	nct_calc_num_ports,
	.set_termios =		nct_serial_set_termios,
	.break_ctl =		nct_serial_break,
	.tiocmget =		nct_serial_tiocmget,
	.tiocmset =		nct_serial_tiocmset,
	.attach	=		nct_usb_attach,
	.disconnect =		nct_disconnect,
	.release =		nct_release,
	.port_probe =		nct_port_probe,
	.port_remove =		nct_port_remove,
	.suspend =		nct_suspend,
	.resume =		nct_resume,
};

static struct usb_serial_driver * const nct_serial_drivers[] = {
	&nct_usb_serial_device, NULL
};

module_usb_serial_driver(nct_serial_drivers, id_table);
MODULE_DESCRIPTION("Nuvoton USB to serial adaptor driver");
MODULE_AUTHOR("Sheng-Yuan Huang <syhuang3@nuvoton.com>");
MODULE_LICENSE("GPL v2");

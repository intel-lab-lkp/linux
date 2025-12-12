// SPDX-License-Identifier: GPL-2.0-only
/*
 * I2C and SMBus driver of EIO embedded driver
 *
 * Copyright (C) 2025 Advantech Co., Ltd.
 */

#include <linux/device.h>
#include <linux/delay.h>
#include <linux/errno.h>
#include <linux/i2c.h>
#include <linux/mfd/core.h>
#include <linux/mfd/eio.h>
#include <linux/module.h>
#include <linux/regmap.h>

#define SUPPORTED_COMMON (I2C_FUNC_I2C | \
			  I2C_FUNC_SMBUS_QUICK | \
			  I2C_FUNC_SMBUS_BYTE | \
			  I2C_FUNC_SMBUS_BYTE_DATA | \
			  I2C_FUNC_SMBUS_WORD_DATA | \
			  I2C_FUNC_SMBUS_I2C_BLOCK)
#define SUPPORTED_SMB	(SUPPORTED_COMMON | I2C_FUNC_SMBUS_BLOCK_DATA)
#define SUPPORTED_I2C	(SUPPORTED_COMMON | I2C_FUNC_10BIT_ADDR)

#define MAX_I2C_SMB		4

#define REG_PNP_INDEX		0x299
#define REG_PNP_DATA		0x29A
#define REG_SUB_PNP_INDEX	0x499
#define REG_SUB_PNP_DATA	0x49A
#define REG_EXT_MODE_ENTER	0x87
#define REG_EXT_MODE_EXIT	0xAA
#define REG_LDN			0x07

#define LDN_I2C0		0x20
#define LDN_I2C1		0x21
#define LDN_SMBUS0		0x22
#define LDN_SMBUS1		0x23

#define REG_BASE_HI		0x60
#define REG_BASE_LO		0x61

#define I2C_REG_CTRL		0x00
#define I2C_CTRL_STOP		BIT(1)

#define I2C_REG_STAT		0x01
#define I2C_STAT_RXREADY	BIT(6)
#define I2C_STAT_TXDONE		BIT(5)
#define I2C_STAT_NAK_ERR	BIT(4)
#define I2C_STAT_ARL_ERR	BIT(3)
#define I2C_STAT_SLV_STP	BIT(2)
#define I2C_STAT_BUSY		BIT(1)
#define I2C_STAT_MST_SLV	BIT(0)

#define I2C_REG_MYADDR		0x02
#define I2C_REG_ADDR		0x03
#define I2C_REG_DATA		0x04
#define I2C_REG_PRESCALE1	0x05
#define I2C_REG_PRESCALE2	0x06

#define I2C_REG_ECTRL		0x07
#define I2C_ECTRL_RST		BIT(7)

#define I2C_REG_SEM		0x08
#define I2C_SEM_INUSE		BIT(1)

#define SMB_REG_HC2		0x0C

#define SMB_REG_HS		0x00
#define SMB_HS_BUSY		BIT(0)
#define SMB_HS_FINISH		BIT(1)
#define SMB_HS_ARL_ERR		BIT(3)
#define SMB_HS_FAILED		BIT(4)
#define SMB_HS_RX_READY		BIT(5)
#define SMB_HS_INUSE		BIT(6)
#define SMB_HS_TX_DONE		BIT(7)

#define SMB_REG_HS2		0x01
#define SMB_HS2_HNOTIFY		BIT(0)
#define SMB_HS2_PEC_ERR		BIT(1)
#define SMB_HS2_NACK_ERR	BIT(2)
#define SMB_HS2_ALERT_STS	BIT(3)
#define SMB_HS2_TO_ERR		BIT(4)
#define SMB_HS2_SSTOP_STS	BIT(5)
#define SMB_HS2_STX_REQ		BIT(6)
#define SMB_HS2_SMODE		BIT(7)

#define SMB_REG_HC		0x02
#define SMB_HC_I2C_NACKEN	BIT(0)
#define SMB_HC_KILL		BIT(1)
#define SMB_HC_CMD_SHIFT	2
#define SMB_HC_LAST_BYTE	BIT(5)
#define SMB_HC_START		BIT(6)
#define SMB_HC_PEC_EN		BIT(7)

#define SMB_REG_HCMD		0x03
#define SMB_REG_HADDR		0x04
#define SMB_REG_HD0		0x05
#define SMB_REG_HD1		0x06
#define SMB_REG_HBLOCK		0x07
#define SMB_REG_HPEC		0x08
#define SMB_REG_SADDR		0x09
#define SMB_REG_SD0		0x0A
#define SMB_REG_SD1		0x0B

#define SMB_REG_HC2		0x0C
#define SMB_HC2_HNOTIFY_DIS	BIT(0)
#define SMB_HC2_I2C_EN		BIT(1)
#define SMB_HC2_AAPEC		BIT(2)
#define SMB_HC2_E32B		BIT(3)
#define SMB_HC2_SRESET		BIT(7)

#define SMB_REG_HPIN		0x0D
#define SMB_REG_HC3		0x0E
#define SMB_REG_HC4		0x0F
#define SMB_REG_NOTIFY_D0	0x11
#define SMB_REG_NOTIFY_D1	0x12
#define SMB_REG_HPRESCALE1	0x13
#define SMB_REG_HPRESCALE2	0x14
#define SMB_REG_HEXTRA		0x15

#define I2C_TIMEOUT		(10 * USEC_PER_MSEC)
#define USE_DEFAULT		-1

#define CHIP_CLK		50000
#define I2C_SCLH_HIGH		2500
#define I2C_SCLH_LOW		1000
#define I2C_SCL_FAST_MODE	0x80
#define I2C_THRESHOLD_SPEED	100
#define I2C_THRESHOLD_SCLH	30
#define I2C_FREQ_MAX		400
#define I2C_FREQ_MIN		8

enum eio_chan_id {
	EIO_I2C0 = 0,
	EIO_I2C1,
	EIO_SMB0,
	EIO_SMB1,
};

struct eio_i2c_dev {
	struct device *dev;
	struct device *mfd;
	struct regmap *regmap;
	struct mutex pnp_mutex; /* Mutex for PNP acces */
	struct eio_i2c_chan *chan[MAX_I2C_SMB];
};

struct eio_i2c_chan {
	u16 base;
	enum eio_chan_id id;
	struct eio_i2c_dev *parent;
	struct i2c_adapter adap;
	struct mutex lock; /* Mutex for regmap writes */
	int freq_override;                  /* kHz or USE_DEFAULT */
};

static int timeout = I2C_TIMEOUT;
module_param(timeout, int, 0444);
MODULE_PARM_DESC(timeout, "Set IO timeout value.\n");

static int i2c0_freq = USE_DEFAULT;
module_param(i2c0_freq, int, 0444);
MODULE_PARM_DESC(i2c0_freq, "Set EIO's I2C0 freq.\n");

static int i2c1_freq = USE_DEFAULT;
module_param(i2c1_freq, int, 0444);
MODULE_PARM_DESC(i2c1_freq, "Set EIO's I2C1 freq.\n");

static int smb0_freq = USE_DEFAULT;
module_param(smb0_freq, int, 0444);
MODULE_PARM_DESC(smb0_freq, "Set EIO's SMB0 freq.\n");

static int smb1_freq = USE_DEFAULT;
module_param(smb1_freq, int, 0444);
MODULE_PARM_DESC(smb1_freq, "Set EIO's SMB1 freq.\n");

static inline u16 eio_enc_7bit_addr(u16 x)
{
	return ((x & 0x07F) << 1);
}

static inline u16 eio_enc_10bit_addr(u16 x)
{
	return ((x & 0xFF) | ((x & 0x0300) << 1) | 0xF000);
}

static inline bool is_i2c(const struct eio_i2c_chan *i2c_chan)
{
	return i2c_chan->id == EIO_I2C0 || i2c_chan->id == EIO_I2C1;
}

static inline struct device *eio_dev(const struct eio_i2c_chan *i2c_chan)
{
	return i2c_chan->parent->dev;
}

static inline struct regmap *eio_map(const struct eio_i2c_chan *i2c_chan)
{
	return i2c_chan->parent->regmap;
}

static inline int eio_reg_write(struct eio_i2c_chan *i2c_chan,
				unsigned int reg_off, unsigned int val)
{
	return regmap_write(eio_map(i2c_chan), i2c_chan->base + reg_off, val);
}

static inline int eio_reg_read(const struct eio_i2c_chan *chan,
			       unsigned int reg, unsigned int *val)
{
	int ret;

	ret = regmap_read(chan->parent->regmap, chan->base + reg, val);
	return ret;
}

static inline int eio_reg_set_bits(const struct eio_i2c_chan *chan,
				   unsigned int reg, unsigned int mask)
{
	return regmap_update_bits(chan->parent->regmap, reg, mask, mask);
}

static inline int eio_reg_clear_bits(const struct eio_i2c_chan *chan,
				     unsigned int reg, unsigned int mask)
{
	return regmap_update_bits(chan->parent->regmap, reg, mask, 0);
}

static inline int eio_reg_or(struct eio_i2c_chan *chan,
			     unsigned int reg, unsigned int mask)
{
	return eio_reg_set_bits(chan, reg, mask);
}

static inline int eio_reg_and(struct eio_i2c_chan *chan,
			      unsigned int reg, unsigned int mask)
{
	return eio_reg_clear_bits(chan, reg, ~mask);
}

static inline unsigned int eio_chan_reg(const struct eio_i2c_chan *i2c_chan,
					unsigned int i2c_reg,
					unsigned int smb_reg)
{
	return is_i2c(i2c_chan) ? i2c_reg : smb_reg;
}

static inline int eio_trigger_read(struct eio_i2c_chan *i2c_chan, u32 *data)
{
	unsigned int reg = eio_chan_reg(i2c_chan, I2C_REG_DATA, SMB_REG_HD0);

	return eio_reg_read(i2c_chan, reg, data);
}

static int wait_busy(struct eio_i2c_chan *i2c_chan)
{
	ktime_t time_end = ktime_add_us(ktime_get(), timeout);
	unsigned int reg = eio_chan_reg(i2c_chan, I2C_REG_STAT, SMB_REG_HS);
	unsigned int target = eio_chan_reg(i2c_chan, I2C_STAT_BUSY, SMB_HS_BUSY);
	unsigned int val;
	int cnt = 0;

	do {
		fsleep(cnt++);

		if (ktime_after(ktime_get(), time_end)) {
			dev_err(eio_dev(i2c_chan), "Wait I2C bus busy timeout\n");
			return -ETIME;
		}

		if (eio_reg_read(i2c_chan, reg, &val))
			return -EIO;

	} while (val & target);

	return 0;
}

static void reset_bus(struct eio_i2c_chan *i2c_chan)
{
	ktime_t time_end = ktime_add_us(ktime_get(), timeout);
	unsigned int reg = eio_chan_reg(i2c_chan, I2C_REG_ECTRL, SMB_REG_HC2);
	unsigned int target = eio_chan_reg(i2c_chan, I2C_ECTRL_RST, SMB_HC2_SRESET);
	unsigned int val = 0;
	unsigned int cnt = 0;

	dev_dbg(eio_dev(i2c_chan), "i2c[%d] bus reset\n", i2c_chan->id);

	if (is_i2c(i2c_chan))
		eio_reg_write(i2c_chan, I2C_REG_ECTRL, I2C_ECTRL_RST);
	else
		eio_reg_or(i2c_chan, SMB_REG_HC2, SMB_HC2_SRESET);

	do {
		fsleep(cnt++);

		if (ktime_after(ktime_get(), time_end)) {
			dev_err(eio_dev(i2c_chan), "bus reset timeout\n");
			return;
		}

		if (eio_reg_read(i2c_chan, reg, &val))
			return;

	} while (val & target);

	wait_busy(i2c_chan);
}

static int wait_bus_free(struct eio_i2c_chan *i2c_chan)
{
	ktime_t time_end = ktime_add_us(ktime_get(), timeout);
	unsigned int val;
	int cnt = 1;

	/* Wait if channel is resetting */
	do {
		fsleep(cnt);

		if (ktime_after(ktime_get(), time_end)) {
			dev_err(eio_dev(i2c_chan), "Wait bus reset timeout\n");
			return -ETIME;
		}

		if (eio_reg_read(i2c_chan,
				 eio_chan_reg(i2c_chan, I2C_REG_ECTRL, SMB_REG_HC2),
				 &val))
			return -EIO;

	} while (val & eio_chan_reg(i2c_chan, I2C_ECTRL_RST, SMB_HC2_SRESET));

	/* Wait INUSE */
	time_end = ktime_add_us(ktime_get(), timeout);

	do {
		fsleep(cnt);

		if (ktime_after(ktime_get(), time_end)) {
			dev_err(eio_dev(i2c_chan), "Timeout: I2C bus in use\n");
			return -ETIME;
		}

		if (eio_reg_read(i2c_chan,
				 eio_chan_reg(i2c_chan, I2C_REG_SEM, SMB_REG_HS),
				 &val))
			return -EIO;

	} while (val & eio_chan_reg(i2c_chan, I2C_SEM_INUSE, SMB_HS_INUSE));

	return 0;
}

static int let_stop(struct eio_i2c_chan *i2c_chan)
{
	unsigned int reg = eio_chan_reg(i2c_chan, I2C_REG_CTRL, SMB_REG_HC);
	unsigned int target = eio_chan_reg(i2c_chan, I2C_CTRL_STOP, SMB_HC_LAST_BYTE);

	return eio_reg_or(i2c_chan, reg, target);
}

static int clr_inuse(struct eio_i2c_chan *i2c_chan)
{
	if (is_i2c(i2c_chan))
		return eio_reg_write(i2c_chan, I2C_REG_SEM, I2C_SEM_INUSE);

	return eio_reg_or(i2c_chan, SMB_REG_HS, SMB_HS_INUSE);
}

static int bus_stop(struct eio_i2c_chan *i2c_chan)
{
	ktime_t time_end = ktime_add_us(ktime_get(), timeout);
	unsigned int reg = eio_chan_reg(i2c_chan, I2C_REG_CTRL, SMB_REG_HC);
	unsigned int target = eio_chan_reg(i2c_chan, I2C_CTRL_STOP, SMB_HC_LAST_BYTE);
	unsigned int val = 0;
	int cnt = 0;

	/* Set STOP bit */
	eio_reg_or(i2c_chan, reg, target);

	/* Wait until STOP bit clears */
	do {
		fsleep(cnt++);

		if (ktime_after(ktime_get(), time_end))
			return -ETIME;

		if (eio_reg_read(i2c_chan, reg, &val))
			return -EIO;

	} while (val & target);

	return 0;
}

static void switch_i2c_mode(struct eio_i2c_chan *i2c_chan, bool on)
{
	u32 tmp;

	if (is_i2c(i2c_chan))
		return;

	if (eio_reg_read(i2c_chan, SMB_REG_HC2, &tmp))
		return;

	eio_reg_write(i2c_chan, SMB_REG_HC2,
		      on ? (tmp | SMB_HC2_I2C_EN | SMB_HC2_SRESET)
		      : (tmp & ~SMB_HC2_I2C_EN));
}

static void i2c_clear(struct eio_i2c_chan *i2c_chan)
{
	if (is_i2c(i2c_chan)) {
		eio_reg_write(i2c_chan, I2C_REG_STAT, 0xFF);
	} else {
		eio_reg_or(i2c_chan, SMB_REG_HS,  0xA9);
		eio_reg_or(i2c_chan, SMB_REG_HS2, 0x4C);
	}
}

static int wait_write_done(struct eio_i2c_chan *i2c_chan, bool no_ack)
{
	ktime_t time_end = ktime_add_us(ktime_get(), timeout);
	unsigned int val = 0;
	int cnt = 0;
	unsigned int reg = eio_chan_reg(i2c_chan, I2C_REG_STAT, SMB_REG_HS);
	unsigned int target = eio_chan_reg(i2c_chan, I2C_STAT_TXDONE, SMB_HS_TX_DONE);

	do {
		fsleep(cnt++);
		if (ktime_after(ktime_get(), time_end)) {
			if (is_i2c(i2c_chan)) {
				eio_reg_or(i2c_chan, I2C_REG_STAT, 0);
			} else {
				eio_reg_or(i2c_chan, SMB_REG_HS,  0);
				eio_reg_or(i2c_chan, SMB_REG_HS2, 0);
			}
			dev_err(eio_dev(i2c_chan), "wait write complete timeout %X %X\n",
				val, target);
			return -ETIME;
		}
		if (eio_reg_read(i2c_chan, reg, &val))
			return -EIO;

	} while ((val & target) == 0);

	if (no_ack)
		return 0;

	if (is_i2c(i2c_chan)) {
		eio_reg_or(i2c_chan, I2C_REG_STAT, 0);
		return (val & I2C_STAT_NAK_ERR) ? -EIO : 0;
	}

	eio_reg_or(i2c_chan, SMB_REG_HS, 0);
	if (eio_reg_read(i2c_chan, SMB_REG_HS2, &val))
		return -EIO;
	eio_reg_write(i2c_chan, SMB_REG_HS2, val);

	return (val & SMB_HS2_NACK_ERR) ? -EIO : 0;
}

static int wait_ready(struct eio_i2c_chan *i2c_chan)
{
	int ret;

	ret = wait_bus_free(i2c_chan);
	if (ret)
		return ret;

	if (wait_busy(i2c_chan) == 0)
		return 0;

	reset_bus(i2c_chan);

	return wait_busy(i2c_chan);
}

static int write_addr(struct eio_i2c_chan *i2c_chan, int addr, bool no_ack)
{
	eio_reg_write(i2c_chan, eio_chan_reg(i2c_chan, I2C_REG_ADDR, SMB_REG_HADDR),
		      addr);

	return wait_write_done(i2c_chan, no_ack);
}

static int write_data(struct eio_i2c_chan *i2c_chan, int data, bool no_ack)
{
	eio_reg_write(i2c_chan, eio_chan_reg(i2c_chan, I2C_REG_DATA, SMB_REG_HD0),
		      data);

	return wait_write_done(i2c_chan, no_ack);
}

static int read_data(struct eio_i2c_chan *i2c_chan, u8 *data)
{
	unsigned int val = 0, tmp;
	int cnt = 0;
	ktime_t time_end = ktime_add_us(ktime_get(), timeout);
	unsigned int stat = eio_chan_reg(i2c_chan, I2C_REG_STAT, SMB_REG_HS);
	unsigned int target = eio_chan_reg(i2c_chan, I2C_STAT_RXREADY, SMB_HS_RX_READY);
	unsigned int reg = eio_chan_reg(i2c_chan, I2C_REG_DATA, SMB_REG_HD0);

	do {
		fsleep(cnt++);

		if (ktime_after(ktime_get(), time_end)) {
			eio_reg_or(i2c_chan, stat, 0);
			dev_err(eio_dev(i2c_chan), "read data timeout\n");
			return -ETIME;
		}

		if (eio_reg_read(i2c_chan, stat, &val))
			return -EIO;

	} while ((val & target) != target);

	/* clear status */
	eio_reg_write(i2c_chan, stat, val);

	/* Must read data after clearing status */
	if (eio_reg_read(i2c_chan, reg, &tmp))
		return -EIO;
	*data = (u8)tmp;

	return 0;
}

static int set_freq(struct eio_i2c_chan *i2c_chan, int freq)
{
	u8 pre1, pre2;
	u16 speed;
	unsigned int reg1 = eio_chan_reg(i2c_chan, I2C_REG_PRESCALE1, SMB_REG_HPRESCALE1);
	unsigned int reg2 = eio_chan_reg(i2c_chan, I2C_REG_PRESCALE2, SMB_REG_HPRESCALE2);

	dev_dbg(eio_dev(i2c_chan), "set freq: %dkHz\n", freq);
	if (freq > I2C_FREQ_MAX || freq < I2C_FREQ_MIN) {
		dev_err(eio_dev(i2c_chan), "Invalid i2c freq: %d\n", freq);
		return -EINVAL;
	}

	speed = (freq < I2C_THRESHOLD_SCLH) ? I2C_SCLH_LOW : I2C_SCLH_HIGH;

	pre1 = (u8)(CHIP_CLK / speed);
	pre2 = (u8)((speed / freq) - 1);

	if (freq > I2C_THRESHOLD_SCLH)
		pre2 |= I2C_SCL_FAST_MODE;

	eio_reg_write(i2c_chan, reg1, pre1);
	eio_reg_write(i2c_chan, reg2, pre2);

	return 0;
}

static int get_freq(struct eio_i2c_chan *i2c_chan, int *freq)
{
	int clk;
	unsigned int pre1 = 0, pre2 = 0;
	unsigned int reg1 = eio_chan_reg(i2c_chan, I2C_REG_PRESCALE1, SMB_REG_HPRESCALE1);
	unsigned int reg2 = eio_chan_reg(i2c_chan, I2C_REG_PRESCALE2, SMB_REG_HPRESCALE2);

	if (eio_reg_read(i2c_chan, reg1, &pre1))
		return -EIO;
	if (eio_reg_read(i2c_chan, reg2, &pre2))
		return -EIO;

	clk = (pre2 & I2C_SCL_FAST_MODE) ? I2C_SCLH_HIGH : I2C_SCLH_LOW;
	pre2 &= ~I2C_SCL_FAST_MODE;

	*freq = clk / ((int)pre2 + 1);

	return 0;
}

static int smb_access(struct eio_i2c_chan *i2c_chan, u8 addr, bool is_read, u8 cmd,
		      int size, union i2c_smbus_data *data)
{
	int i, tmp, ret = 0;
	unsigned int st1, st2;
	int len = 0;

	mutex_lock(&i2c_chan->lock);

	ret = wait_ready(i2c_chan);
	if (ret)
		goto exit;

	/* Force SMBus mode */
	switch_i2c_mode(i2c_chan, false);

	addr = eio_enc_7bit_addr(addr) | (is_read ? 1 : 0);
	eio_reg_write(i2c_chan, SMB_REG_HADDR, addr);
	eio_reg_write(i2c_chan, SMB_REG_HCMD,  cmd);

	dev_dbg(eio_dev(i2c_chan), "SMB[%d], addr:0x%02X, cmd:0x%02X size=%d\n",
		i2c_chan->id, addr, cmd, size);

	switch (size) {
	case I2C_SMBUS_QUICK:
		dev_dbg(eio_dev(i2c_chan), "I2C_SMBUS_QUICK\n");
		break;

	case I2C_SMBUS_BYTE:
		if (!is_read) {
			dev_dbg(eio_dev(i2c_chan), "I2C_SMBUS_BYTE\n");
			eio_reg_write(i2c_chan, SMB_REG_HCMD, cmd);
		}
		break;

	case I2C_SMBUS_BYTE_DATA:
		dev_dbg(eio_dev(i2c_chan), "I2C_SMBUS_BYTE_DATA\n");
		if (!is_read) {
			eio_reg_write(i2c_chan, SMB_REG_HD0, data->byte);
			dev_dbg(eio_dev(i2c_chan), "write %X\n", data->byte);
		}
		break;

	case I2C_SMBUS_WORD_DATA:
		dev_dbg(eio_dev(i2c_chan), "I2C_SMBUS_WORD_DATA\n");
		if (!is_read) {
			eio_reg_write(i2c_chan, SMB_REG_HD0, data->block[0]);
			eio_reg_write(i2c_chan, SMB_REG_HD1, data->block[1]);
		}
		break;

	case I2C_SMBUS_PROC_CALL:
		dev_dbg(eio_dev(i2c_chan), "I2C_SMBUS_PROC_CALL\n");
		eio_reg_write(i2c_chan, SMB_REG_HD0, data->block[0]);
		eio_reg_write(i2c_chan, SMB_REG_HD1, data->block[1]);
		break;

	case I2C_SMBUS_BLOCK_DATA:
		dev_dbg(eio_dev(i2c_chan), "I2C_SMBUS_BLOCK_DATA\n");
		if (is_read)
			break;

		/* Program command type */
		eio_reg_read(i2c_chan, SMB_REG_HC, (unsigned int *)&tmp);
		tmp &= ~(0x07 << SMB_HC_CMD_SHIFT);
		tmp |=  (size << SMB_HC_CMD_SHIFT);
		eio_reg_write(i2c_chan, SMB_REG_HC, tmp);

		/* Force write for payload stage */
		eio_reg_write(i2c_chan, SMB_REG_HADDR, addr & ~0x01);

		/* Reset internal buffer index pointer */
		eio_reg_and(i2c_chan, SMB_REG_HC2, (int)~SMB_HC2_E32B);
		eio_reg_or(i2c_chan, SMB_REG_HC2, SMB_HC2_E32B);

		/* Write length + data */
		eio_reg_write(i2c_chan, SMB_REG_HD0, data->block[0]);
		for (i = 1; i <= data->block[0]; i++)
			eio_reg_write(i2c_chan, SMB_REG_HBLOCK, data->block[i]);
		break;

	case I2C_SMBUS_BLOCK_PROC_CALL:
		/* Set command type field */
		eio_reg_and(i2c_chan, SMB_REG_HC, (0x07 << SMB_HC_CMD_SHIFT));
		eio_reg_write(i2c_chan, SMB_REG_HD0, data->block[0]);

		/* Reset buffer index */
		eio_reg_and(i2c_chan, SMB_REG_HC2, (int)~SMB_HC2_E32B);
		eio_reg_or(i2c_chan, SMB_REG_HC2, SMB_HC2_E32B);

		for (i = 1; i <= data->block[0]; i++)
			eio_reg_write(i2c_chan, SMB_REG_HBLOCK, data->block[i]);
		break;

	default:
		ret = -EINVAL;
		goto exit;
	}

	/* Launch transaction */
	eio_reg_read(i2c_chan, SMB_REG_HC, (unsigned int *)&tmp);
	tmp &= ~(0x07 << SMB_HC_CMD_SHIFT);
	tmp |=  (size << SMB_HC_CMD_SHIFT) | SMB_HC_START;
	tmp &= ~(SMB_HC_I2C_NACKEN | SMB_HC_KILL | SMB_HC_PEC_EN);
	eio_reg_write(i2c_chan, SMB_REG_HC, tmp);

	ret = wait_busy(i2c_chan);
	if (ret)
		goto exit;

	eio_reg_read(i2c_chan, SMB_REG_HS,  &st1);
	eio_reg_read(i2c_chan, SMB_REG_HS2, &st2);

	if (st1 & SMB_HS_FAILED) {
		dev_err(eio_dev(i2c_chan), "HS FAILED\n");
		ret = -EIO;
	} else if (st1 & SMB_HS_ARL_ERR) {
		dev_err(eio_dev(i2c_chan), "ARL FAILED\n");
		ret = -EIO;
	} else if (st2 & SMB_HS2_TO_ERR) {
		dev_err(eio_dev(i2c_chan), "timeout\n");
		ret = -ETIME;
	} else if (st2 & SMB_HS2_NACK_ERR) {
		dev_err(eio_dev(i2c_chan), "NACK err\n");
		ret = -EIO;
	} else if (st2 & SMB_HS2_PEC_ERR) {
		dev_err(eio_dev(i2c_chan), "PEC err\n");
		ret = -EIO;
	}
	if (ret)
		goto exit;

	switch (size) {
	case I2C_SMBUS_QUICK:
		dev_dbg(eio_dev(i2c_chan), "I2C_SMBUS_QUICK\n");
		break;

	case I2C_SMBUS_BYTE:
	case I2C_SMBUS_BYTE_DATA:
		if (is_read) {
			unsigned int v;

			dev_dbg(eio_dev(i2c_chan), "I2C_SMBUS_BYTE/I2C_SMBUS_BYTE_DATA\n");
			eio_reg_read(i2c_chan, SMB_REG_HD0, &v);
			data->block[0] = (u8)v;
			dev_dbg(eio_dev(i2c_chan), "read %X\n", data->block[0]);
		}
		break;

	case I2C_SMBUS_WORD_DATA: {
		unsigned int v0, v1;

		if (is_read) {
			dev_dbg(eio_dev(i2c_chan), "I2C_SMBUS_WORD_DATA\n");
			eio_reg_read(i2c_chan, SMB_REG_HD0, &v0);
			eio_reg_read(i2c_chan, SMB_REG_HD1, &v1);
			data->block[0] = (u8)v0;
			data->block[1] = (u8)v1;
		}
		break;
	}

	case I2C_SMBUS_PROC_CALL: {
		unsigned int v0, v1;

		dev_dbg(eio_dev(i2c_chan), "I2C_SMBUS_PROC_CALL\n");
		eio_reg_read(i2c_chan, SMB_REG_HD0, &v0);
		eio_reg_read(i2c_chan, SMB_REG_HD1, &v1);
		data->block[0] = (u8)v0;
		data->block[1] = (u8)v1;
		break;
	}

	case I2C_SMBUS_BLOCK_DATA:
		if (!is_read)
			break;

		dev_dbg(eio_dev(i2c_chan), "I2C_SMBUS_BLOCK_DATA\n");
		eio_reg_read(i2c_chan, SMB_REG_HD0, (unsigned int *)&len);
		len = min(len, I2C_SMBUS_BLOCK_MAX);
		data->block[0] = len;

		for (i = 1; i <= len; i++)
			eio_reg_read(i2c_chan, SMB_REG_HBLOCK,
				     (unsigned int *)&data->block[i]);
		break;

	default:
		ret = -EINVAL;
		goto exit;
	}

exit:
	/* Clear latched status */
	eio_reg_write(i2c_chan, SMB_REG_HS,  0xFF);
	eio_reg_write(i2c_chan, SMB_REG_HS2, 0xFF);

	mutex_unlock(&i2c_chan->lock);
	return ret;
}

static int i2c_xfer(struct i2c_adapter *adap, struct i2c_msg *msgs, int nmsgs)
{
	int msg, data;
	int addr = 0;
	int dummy;
	int ret = 0;
	struct eio_i2c_chan *i2c_chan = i2c_get_adapdata(adap);

	mutex_lock(&i2c_chan->lock);

	ret = wait_ready(i2c_chan);
	if (ret)
		goto exit;

	switch_i2c_mode(i2c_chan, true);

	dev_dbg(eio_dev(i2c_chan), "Transmit %d I2C messages\n", nmsgs);
	for (msg = 0; msg < nmsgs; msg++) {
		int is_read = msgs[msg].flags & I2C_M_RD;
		bool no_ack = msgs[msg].flags & I2C_M_IGNORE_NAK;

		dev_dbg(eio_dev(i2c_chan), "message %d len=%d\n", msg, msgs[msg].len);

		if (!msgs[msg].len)
			let_stop(i2c_chan);

		if (msgs[msg].flags & I2C_M_TEN) {
			addr = eio_enc_10bit_addr(msgs[msg].addr);
			addr |= is_read;
			dev_dbg(eio_dev(i2c_chan), "10-bit addr: %X\n", addr);

			ret = write_addr(i2c_chan, addr >> 8, no_ack);
			if (!ret)
				ret = write_data(i2c_chan, addr & 0x7F, no_ack);
		} else {
			addr = eio_enc_7bit_addr(msgs[msg].addr);
			addr |= is_read;
			dev_dbg(eio_dev(i2c_chan), "7-bit addr: %X\n", addr);

			ret = write_addr(i2c_chan, addr, no_ack);
		}

		if (ret)
			goto exit;

		if (!msgs[msg].len)
			goto exit;

		if (is_read)
			ret = eio_trigger_read(i2c_chan, (u32 *)&dummy);

		/* Transmit all messages */
		for (data = 0; data < msgs[msg].len; data++) {
			if (msgs[msg].flags & I2C_M_RD) {
				bool last = (msgs[msg].len == data + 1);

				if (last)
					let_stop(i2c_chan);

				ret = read_data(i2c_chan, &msgs[msg].buf[data]);
				dev_dbg(eio_dev(i2c_chan), "I2C read[%d] = %x\n",
					data, msgs[msg].buf[data]);

				/* Don't stop twice */
				if (last && ret == 0)
					goto exit;
			} else {
				ret = write_data(i2c_chan, msgs[msg].buf[data], no_ack);
				dev_dbg(eio_dev(i2c_chan), "I2C write[%d] = %x\n",
					data, msgs[msg].buf[data]);
			}
			if (ret)
				goto exit;
		}
	}

	if (!ret)
		ret = bus_stop(i2c_chan);

	if (!ret)
		goto exit;

exit:
	if (ret)
		reset_bus(i2c_chan);

	i2c_clear(i2c_chan);
	clr_inuse(i2c_chan);

	mutex_unlock(&i2c_chan->lock);
	return ret ? ret : nmsgs;
}

static int smbus_xfer(struct i2c_adapter *adap, u16 addr,
		      u16 flags, char is_read, u8 cmd,
		      int size, union i2c_smbus_data *data)
{
	int ret;
	struct eio_i2c_chan *i2c_chan = i2c_get_adapdata(adap);
	int nmsgs = is_read ? 2 : 1;
	u8 buf[I2C_SMBUS_BLOCK_MAX + sizeof(u32)] = { cmd, };
	struct i2c_msg msgs[2] = {
		{ .addr = addr, .flags = flags & ~I2C_M_RD, .buf = buf + 0 },
		{ .addr = addr, .flags = flags |  I2C_M_RD, .buf = buf + 1 },
	};

	/* Non-I2C channels use the SMB engine, except I2C block variants we emulate */
	if (!is_i2c(i2c_chan) && size != I2C_SMBUS_I2C_BLOCK_DATA)
		return smb_access(i2c_chan, addr, is_read, cmd, size, data);

	if (data) {
		buf[0] = cmd;
		/* FIX: preserve other flags; only toggle I2C_M_RD */
		msgs[0].flags = is_read ? (flags | I2C_M_RD) : (flags & ~I2C_M_RD);
		msgs[1].buf   = data->block;
	}

	switch (size) {
	case I2C_SMBUS_QUICK:
		dev_dbg(eio_dev(i2c_chan), "I2C_SMBUS_QUICK on I2C\n");
		nmsgs = 1;
		break;

	case I2C_SMBUS_BYTE:
		dev_dbg(eio_dev(i2c_chan), "I2C_SMBUS_BYTE on I2C\n");
		nmsgs = 1;
		msgs[0].len   = 1;
		msgs[0].buf   = is_read ? data->block : buf;
		msgs[0].flags = is_read ? (flags | I2C_M_RD) : (flags & ~I2C_M_RD);
		break;

	case I2C_SMBUS_BYTE_DATA:
		dev_dbg(eio_dev(i2c_chan), "I2C_SMBUS_BYTE_DATA on I2C\n");
		if (!data)
			return -EINVAL;
		msgs[0].len = is_read ? 1 : 2;
		buf[1]      = data->block[0];
		msgs[1].len = 1;
		break;

	case I2C_SMBUS_WORD_DATA:
		dev_dbg(eio_dev(i2c_chan), "I2C_SMBUS_WORD_DATA on I2C\n");
		if (!data)
			return -EINVAL;
		msgs[0].len = is_read ? 1 : 3;
		msgs[1].len = 2;
		buf[1]      = data->block[0];
		buf[2]      = data->block[1];
		msgs[1].buf = data->block;
		break;

	case I2C_SMBUS_I2C_BLOCK_DATA:
	case I2C_SMBUS_I2C_BLOCK_BROKEN:
		dev_dbg(eio_dev(i2c_chan), "I2C_SMBUS_I2C_BLOCK_(DATA/BROKEN) on I2C len=%d\n",
			data->block[0]);
		if (!data)
			return -EINVAL;
		msgs[0].len = is_read ? 1 : data->block[0] + 1;
		msgs[1].len = data->block[0];
		msgs[1].buf = data->block + 1;
		if (msgs[0].len >= I2C_SMBUS_BLOCK_MAX ||
		    msgs[1].len >= I2C_SMBUS_BLOCK_MAX)
			return -EINVAL;
		if (!is_read)
			memcpy(buf + 1, data->block + 1, msgs[0].len);
		break;

	case I2C_SMBUS_PROC_CALL:
		dev_dbg(eio_dev(i2c_chan), "I2C_SMBUS_PROC_CALL on I2C\n");
		if (!data)
			return -EINVAL;
		nmsgs = 2;
		msgs[0].flags = flags & ~I2C_M_RD;
		msgs[0].len   = 3;
		buf[1]        = data->block[0];
		buf[2]        = data->block[1];
		msgs[1].len   = 2;
		break;

	case I2C_SMBUS_BLOCK_DATA:
		dev_dbg(eio_dev(i2c_chan), "I2C_SMBUS_BLOCK_DATA on I2C not supported\n");
		return -EINVAL;

	case I2C_SMBUS_BLOCK_PROC_CALL:
		dev_dbg(eio_dev(i2c_chan), "I2C_SMBUS_BLOCK_PROC_CALL on I2C not supported\n");
		return -EINVAL;

	default:
		return -EINVAL;
	}

	ret = i2c_xfer(adap, msgs, nmsgs);
	return ret < 0 ? ret : 0;
}

static int load_i2c(struct device *dev, enum eio_chan_id id,
		    struct eio_i2c_chan *i2c_chan)
{
	u32 base_lo, base_hi, base;
	int ldn = LDN_I2C0 + id;
	struct eio_i2c_dev *eio_i2c = i2c_chan->parent;
	struct regmap *map;

	if (!eio_i2c || !eio_i2c->regmap)
		return dev_err_probe(dev, -ENODEV, "missing parent/regmap\n");

	map = eio_i2c->regmap;

	/* Read channel I/O base via shared PNP window */
	mutex_lock(&eio_i2c->pnp_mutex);
	if (regmap_write(map, REG_PNP_INDEX, REG_EXT_MODE_ENTER) ||
	    regmap_write(map, REG_PNP_INDEX, REG_EXT_MODE_ENTER) ||
	    regmap_write(map, REG_PNP_INDEX, REG_LDN) ||
	    regmap_write(map, REG_PNP_DATA,  ldn) ||
	    regmap_write(map, REG_PNP_INDEX, REG_BASE_HI) ||
	    regmap_read(map,  REG_PNP_DATA,  &base_hi) ||
	    regmap_write(map, REG_PNP_INDEX, REG_BASE_LO) ||
	    regmap_read(map,  REG_PNP_DATA,  &base_lo) ||
	    regmap_write(map, REG_PNP_INDEX, REG_EXT_MODE_EXIT)) {
		mutex_unlock(&eio_i2c->pnp_mutex);
		dev_err(dev, "error read/write I2C[%d] IO port\n", id);
		return -EIO;
	}
	mutex_unlock(&eio_i2c->pnp_mutex);

	base = (base_hi << 8) | base_lo;
	if (base == 0xFFFF || base == 0) {
		dev_dbg(dev, "i2c[%d] base addr=%#x (not in-use)\n", id, base);
		return -ENODEV;
	}

	dev_dbg(dev, "i2c[%d] base addr=%#x\n", id, base);

	/* Bind channel (no per-chan dev) */
	i2c_chan->base = (u16)base;
	i2c_chan->id   = id;

	/* Per-channel frequency policy */
	if (i2c_chan->freq_override != USE_DEFAULT)
		set_freq(i2c_chan, i2c_chan->freq_override);

	get_freq(i2c_chan, &i2c_chan->freq_override);

	return 0;
}

static u32 functionality(struct i2c_adapter *adap)
{
	struct eio_i2c_chan *i2c_chan = i2c_get_adapdata(adap);

	return is_i2c(i2c_chan) ? SUPPORTED_I2C : SUPPORTED_SMB;
}

static const struct i2c_algorithm algo = {
	.smbus_xfer	= smbus_xfer,
	.master_xfer	= i2c_xfer,
	.functionality	= functionality,
};

static int eio_i2c_probe(struct platform_device *pdev)
{
	static const char * const names[] = { "i2c0", "i2c1", "smb0", "smb1" };
	struct device *dev = &pdev->dev;
	struct eio_i2c_dev *eio_i2c;
	struct eio_dev *eio_dev = dev_get_drvdata(dev->parent);
	int ret = 0;
	enum eio_chan_id ch;

	if (!eio_dev) {
		dev_err(dev, "Error contact eio_core\n");
		return -ENODEV;
	}

	timeout = clamp_t(int, timeout, I2C_TIMEOUT / 100, I2C_TIMEOUT * 100);
	dev_info(dev, "Timeout value %d\n", timeout);

	eio_i2c = devm_kzalloc(dev, sizeof(*eio_i2c), GFP_KERNEL);
	if (!eio_i2c)
		return -ENOMEM;

	eio_i2c->dev = dev;
	eio_i2c->mfd = dev->parent;
	eio_i2c->regmap = dev_get_regmap(dev->parent, NULL);
	if (!eio_i2c->regmap)
		return dev_err_probe(dev, -ENODEV, "parent regmap not found\n");

	mutex_init(&eio_i2c->pnp_mutex);
	platform_set_drvdata(pdev, eio_i2c);

	for (ch = EIO_I2C0; ch < MAX_I2C_SMB; ch++) {
		struct eio_i2c_chan *i2c_chan;

		i2c_chan = devm_kzalloc(dev, sizeof(*i2c_chan), GFP_KERNEL);
		if (!i2c_chan) {
			ret = -ENOMEM;
			break;
		}

		i2c_chan->parent = eio_i2c;
		i2c_chan->freq_override = USE_DEFAULT;
		mutex_init(&i2c_chan->lock);

		if (load_i2c(dev, ch, i2c_chan)) {
			dev_info(dev, "No %s%d!\n", (ch < 2) ? "I2C" : "SMBus", ch & 1);
			continue;
		}

		i2c_chan->adap.owner      = THIS_MODULE;
		i2c_chan->adap.class      = I2C_CLASS_HWMON;
		i2c_chan->adap.algo       = &algo;
		i2c_chan->adap.dev.parent = dev;
		snprintf(i2c_chan->adap.name, sizeof(i2c_chan->adap.name), "eio-%s",
			 names[ch]);

		i2c_set_adapdata(&i2c_chan->adap, i2c_chan);

		ret = i2c_add_adapter(&i2c_chan->adap);
		dev_info(dev, "Add %s%d %s. %d\n", (ch < 2) ? "I2C" : "SMBus",
			 ch, ret ? "Error" : "Success", ret);
		if (ret)
			break;

		eio_i2c->chan[ch] = i2c_chan;
	}

	if (ret) {
		for (ch = EIO_I2C0; ch < MAX_I2C_SMB; ch++) {
			if (eio_i2c->chan[ch]) {
				i2c_del_adapter(&eio_i2c->chan[ch]->adap);
				eio_i2c->chan[ch] = NULL;
			}
		}
	}

	return ret;
}

static void eio_i2c_remove(struct platform_device *pdev)
{
	struct eio_i2c_dev *eio_i2c = platform_get_drvdata(pdev);
	enum eio_chan_id ch;

	for (ch = EIO_I2C0; ch < MAX_I2C_SMB; ch++) {
		if (eio_i2c->chan[ch]) {
			i2c_del_adapter(&eio_i2c->chan[ch]->adap);
			eio_i2c->chan[ch] = NULL;
		}
	}
}

static struct platform_driver eio_i2c_driver = {
	.probe  = eio_i2c_probe,
	.remove = eio_i2c_remove,
	.driver = {
		.name = "i2c_eio",
	},
};

module_platform_driver(eio_i2c_driver);

MODULE_AUTHOR("Wenkai Chung <wenkai.chung@advantech.com.tw>");
MODULE_AUTHOR("Ramiro Oliveira <ramiro.oliveira@advantech.com>");
MODULE_DESCRIPTION("I2C driver for Advantech EIO embedded controller");
MODULE_LICENSE("GPL");
MODULE_SOFTDEP("pre: eio_core");

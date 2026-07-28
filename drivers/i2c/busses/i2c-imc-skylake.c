// SPDX-License-Identifier: GPL-2.0-only
/*
 * Intel Skylake-X iMC SMBus I2C adapter.
 *
 * The integrated memory controller (iMC) on Intel Skylake-X / Cascade Lake-X
 * processors contains an SMBus engine wired to the DDR4 DIMMs.  It reaches the
 * SPD EEPROMs (0x50-0x57) and the thermal sensors (0x18-0x1f) of the installed
 * modules.  The engine is driven through the PCI configuration space of the
 * Sky Lake-E PCU function (8086:2085).  This driver presents it as two standard
 * Linux I2C adapters - one per hardware SMBus channel - so that i2c-tools,
 * decode-dimms and lm-sensors can use it without bespoke sysfs hacks.
 *
 * Per-channel register triple within the config space of the function:
 *                  ch0     ch1
 *     CTRL (data)  0xB4    0xB8   write: data byte in bits[23:16]; read: low byte
 *     DATA (cmd)   0x9C    0xA0   command toggle | GO | address | register
 *     STATUS       0xA8    0xAC   bit0 BUSY, bit1 ERROR/NACK,
 *                                 bit2 READ_DONE, bit3 WRITE_DONE
 *   SMBus command byte (DATA bits[15:8]) = (rw << 7) | addr7: the 7-bit slave
 *   address with bit7 = direction (1 write / 0 read).  The register/offset goes
 *   in DATA[7:0].  So 0x50 reads SPD EEPROM 0x50.  This was decoded and
 *   confirmed on hardware, and cross-checked against an independent Windows
 *   implementation of the same engine.
 *
 * The two channels are independent SMBus buses (DIMMs 1,2 on ch0; DIMMs 3,4 on
 * ch1), hence two i2c_adapter instances exposed as separate /dev/i2c-* nodes.
 *
 * Bus arbitration / concurrency with firmware:
 *   The driver cannot coordinate with SMM, a BMC, or the iMC's own closed-loop
 *   thermal throttling (CLTT), and the PCI ID alone does not prove that none of
 *   them is active.  Binding is therefore disabled by default and requires
 *   allow_unsafe_access=1, following the same choice made by the earlier iMC
 *   drivers for Sandy Bridge-EP and Broadwell-E.  On top of that, transactions
 *   are serialized across both channels, wait for the complete engine to become
 *   idle, temporarily clear TSOD_ACTIVE, restore the previous command state,
 *   and verify afterwards that no other master modified the command registers
 *   while the transfer was in flight.
 *
 *   Not implemented: the Broadwell-E driver additionally stops the PCU's TSOD
 *   polling for the duration of a transfer (write 0 to the polling interval,
 *   wait 10 ms for in-flight transactions to drain, restore afterwards).  The
 *   equivalent register has not been identified on this part, so a system with
 *   CLTT enabled is expected to trip the interference check below rather than
 *   to be arbitrated against.
 */

#include <linux/bits.h>
#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/i2c-smbus.h>
#include <linux/iopoll.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/pci.h>
#include <linux/pci_ids.h>

/* PCI id of the Sky Lake-E PCU function carrying the SMBus engine */
#define PCU_DEVICE	0x2085

/*
 * The iMC channel function, device 10 and 12 function 0, carries the SMBus
 * control register documented in the Intel Xeon Processor Scalable Family
 * datasheet volume 2 (reference 614073) section 3.1.9.  Only SMBCNTL is read
 * here, and only to find out whether the hardware is polling the DIMM thermal
 * sensors on its own.
 */
#define IMC_CHANNEL_DEVICE	0x2040
#define IMC_SMBCNTL(ch)		(0xE88 + (ch) * 0x10)
#define SMBCNTL_TSOD_POLL_EN	BIT(8)
#define SMBCNTL_TSOD_PRESENT	GENMASK(7, 0)

/* per-channel register offsets in the function's config space */
#define CH0_CTRL	0xB4
#define CH0_DATA	0x9C
#define CH0_STAT	0xA8
#define CH1_CTRL	0xB8
#define CH1_DATA	0xA0
#define CH1_STAT	0xAC

/*
 * Command word written to the DATA register:
 *   bit29       = command toggle
 *   bit20       = TSOD active state, preserved across transfers
 *   bit19       = GO
 *   bit17       = word transfer
 *   bits[15:8]  = SMBus command byte = (rw << 7) | addr7
 *                   rw bit (0x80): 1 = write, 0 = read
 *                   addr7        : 7-bit SMBus slave address
 *   bits[7:0]   = register/offset within the addressed device
 * For a write the data byte is latched into CTRL[23:16] beforehand.  This
 * encoding was confirmed on hardware: command 0x50 reads SPD EEPROM 0x50
 * (DDR4 signature).
 */
#define COMMAND_TOGGLE	BIT(29)
#define GO_BIT		BIT(19)		/* start transaction */
#define TSOD_ACTIVE_BIT	BIT(20)
#define WORD_BIT	BIT(17)		/* 16-bit word transfer (vs 8-bit byte) */
/*
 * Address-only transfer: the register byte in bits[7:0] is not put on the bus.
 * A read then returns the byte at the device's own pointer (SMBus Receive
 * Byte) and a write sends the data byte alone (SMBus Send Byte).
 *
 * The bit is not documented for this function.  It was found by noting that
 * the interface Intel does document for the iMC channel functions (SMBCMD,
 * datasheet ref. 614073 section 3.1.8) places PNTR_SEL immediately above
 * WORD_ACCESS, and that word access here is bit 17.  Confirmed on hardware:
 * with the bit set, eight consecutive reads of an SPD EEPROM returned bytes
 * 0x01 through 0x08 in order, matching an ordinary dump of the same range,
 * which is the auto-increment behaviour of a Receive Byte and cannot be
 * produced by a transfer that names a register.
 */
#define PNTR_SEL_BIT	BIT(18)
#define WRITE_OPERATION	BIT(15)
#define COMMAND_PREFIX	(COMMAND_TOGGLE | GO_BIT)
#define COMMAND_KEEP_MASK	(~TSOD_ACTIVE_BIT)
/*
 * Command bits this driver produces itself.  Everything outside this mask
 * belongs to whoever else is driving the engine, so a change there during a
 * transfer means interference.
 */
#define COMMAND_OUR_BITS	(COMMAND_TOGGLE | GO_BIT | WORD_BIT | \
				 PNTR_SEL_BIT | GENMASK(15, 0))
#define STAT_BUSY	BIT(0)		/* low bit set while transaction in flight */
#define STAT_ERROR	BIT(1)
#define STAT_READ_DONE	BIT(2)
#define STAT_WRITE_DONE	BIT(3)
#define STAT_ANY_DONE	(STAT_ERROR | STAT_READ_DONE | STAT_WRITE_DONE)

/*
 * Polling bounds.  Transactions on the tested system complete in a few
 * milliseconds; these are upper bounds that avoid busy-spinning and cover
 * device clock stretching, not a measured multi-platform worst case.  The
 * mutex is held across a whole transfer, so the worst case a caller on the
 * other channel can wait for is the sum of all of them, roughly 750 ms.
 */
#define IMC_POLL_US		10
#define IMC_IDLE_TIMEOUT_US	50000
#define IMC_GO_TIMEOUT_US	200000
#define IMC_DONE_TIMEOUT_US	50000

static bool allow_unsafe_access;
module_param(allow_unsafe_access, bool, 0444);
MODULE_PARM_DESC(allow_unsafe_access,
		 "allow access without firmware/SMM arbitration (unsafe)");

struct imc_chan {
	unsigned int ctrl, data, stat;
	int idx;			/* channel index 0 or 1 */
};

static const struct imc_chan imc_chans[2] = {
	{ CH0_CTRL, CH0_DATA, CH0_STAT, 0 },
	{ CH1_CTRL, CH1_DATA, CH1_STAT, 1 },
};

/* one driver state object, shared by both per-channel adapters */
struct imc_smbus {
	struct pci_dev *pdev;		/* config space carrying the engine */
	struct device *dev;		/* &pdev->dev, for dev_*() logging */
	struct mutex lock;		/* serialises all SMBus transactions */
					/* needed: both channels share one engine */
	struct i2c_adapter adap[2];	/* one per hardware channel */
	int io_err;			/* sticky config-access error, under lock */
};

struct imc_xfer_state {
	u32 command[ARRAY_SIZE(imc_chans)];
	bool restore[ARRAY_SIZE(imc_chans)];
};

/*
 * Register accessors.  A config-space failure is recorded in s->io_err and
 * reported once, at the end of the transfer, so the transaction logic does not
 * have to check a return value on every register touch.  A failed read yields
 * all-ones, which no polling loop mistakes for a completed transaction.
 */
static u32 imc_reg(struct imc_smbus *s, unsigned int off)
{
	u32 val;
	int ret;

	ret = pci_read_config_dword(s->pdev, off, &val);
	if (ret) {
		s->io_err = pcibios_err_to_errno(ret);
		return ~0U;
	}

	return val;
}

static void imc_set(struct imc_smbus *s, unsigned int off, u32 val)
{
	int ret;

	ret = pci_write_config_dword(s->pdev, off, val);
	if (ret)
		s->io_err = pcibios_err_to_errno(ret);
}

static u32 imc_busy_mask(struct imc_smbus *s)
{
	u32 busy = 0;
	int i;

	for (i = 0; i < ARRAY_SIZE(imc_chans); i++)
		if (imc_reg(s, imc_chans[i].stat) & STAT_BUSY)
			busy |= BIT(i);

	return busy;
}

static int imc_wait_engine_idle(struct imc_smbus *s)
{
	u32 busy;
	int ret;

	ret = read_poll_timeout(imc_busy_mask, busy, !busy || s->io_err,
				IMC_POLL_US, IMC_IDLE_TIMEOUT_US, false, s);
	if (ret)
		dev_warn_ratelimited(s->dev,
				     "engine busy on channel mask 0x%x\n", busy);

	return ret;
}

static int imc_wait_go_clear(struct imc_smbus *s, const struct imc_chan *c)
{
	u32 val;

	return read_poll_timeout(imc_reg, val, !(val & GO_BIT) || s->io_err,
				 IMC_POLL_US, IMC_GO_TIMEOUT_US, false,
				 s, c->data);
}

static int imc_wait_done(struct imc_smbus *s, const struct imc_chan *c,
			 u32 *status)
{
	u32 val;
	int ret;

	ret = read_poll_timeout(imc_reg, val,
				(!(val & STAT_BUSY) && (val & STAT_ANY_DONE)) ||
				s->io_err,
				IMC_POLL_US, IMC_DONE_TIMEOUT_US, false,
				s, c->stat);
	*status = val;

	return ret;
}

static int imc_wait_transfer(struct imc_smbus *s, const struct imc_chan *c,
			     u32 *status)
{
	u32 command;
	int ret;

	ret = imc_wait_go_clear(s, c);
	if (ret)
		*status = imc_reg(s, c->stat);
	else
		ret = imc_wait_done(s, c, status);
	if (!ret)
		return 0;
	if (!(*status & STAT_BUSY) || (*status & STAT_ANY_DONE))
		return ret;

	command = imc_reg(s, c->data);
	imc_set(s, c->data, command ^ COMMAND_TOGGLE);

	ret = imc_wait_done(s, c, status);
	if (ret)
		dev_warn_ratelimited(s->dev,
				     "ch%d recovery timed out (stat 0x%08x)\n",
				     c->idx, *status);

	return ret;
}

static int imc_check_status(struct imc_smbus *s, const struct imc_chan *c,
			    u32 status, u32 expected, u8 addr, u8 reg)
{
	if (status & STAT_ERROR) {
		dev_dbg(s->dev, "addr 0x%02x reg 0x%02x NACK (stat 0x%08x)\n",
			addr, reg, status);
		return -ENXIO;
	}

	/* Done bits are latched; only the bit for this operation is required. */
	if (!(status & expected)) {
		dev_warn_ratelimited(s->dev,
				     "ch%d unexpected completion for addr 0x%02x reg 0x%02x (stat 0x%08x)\n",
				     c->idx, addr, reg, status);
		return -EIO;
	}

	return 0;
}

/*
 * The done bits are latched and no way to clear them has been found, so a
 * command that never reaches the register is indistinguishable from one that
 * completed: STATUS still shows the previous DONE, the GO bit of the stale
 * command word is already clear, and the caller would receive the previous
 * transaction's data as valid.
 *
 * Read the command word back and require it to be the one just written.  The
 * engine only ever clears GO, so that bit is the single permitted difference;
 * anything else means the write did not land.  Checking the word rather than
 * a status transition matters because two identical transfers in a row leave
 * STATUS bit for bit the same.
 */
static bool imc_command_landed(u32 command, u32 readback)
{
	return !((command ^ readback) & ~GO_BIT);
}

/*
 * Detect a second master.  Compare the command registers against what this
 * driver left in them: for the channel it drove, only the bits it does not own;
 * for the other channel, all of them.  A difference means firmware, a BMC or
 * CLTT touched the engine while the transfer was in flight, so the result
 * cannot be trusted.
 */
static int imc_check_interference(struct imc_smbus *s, const struct imc_chan *c,
				  const struct imc_xfer_state *state)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(imc_chans); i++) {
		u32 mask = i == c->idx ? ~COMMAND_OUR_BITS : ~0U;
		u32 expect = state->command[i] & COMMAND_KEEP_MASK;
		u32 now = imc_reg(s, imc_chans[i].data);

		if (!((now ^ expect) & mask))
			continue;

		dev_warn_ratelimited(s->dev,
				     "ch%d command changed under us (0x%08x -> 0x%08x): another master is using the engine\n",
				     i, expect, now);
		return -EAGAIN;
	}

	return 0;
}

static void imc_restore_xfer(struct imc_smbus *s,
			     const struct imc_xfer_state *state)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(imc_chans); i++) {
		if (!state->restore[i])
			continue;
		/*
		 * Mask GO: the saved word is only meant to put the previous
		 * address, register and TSOD state back, never to re-arm a
		 * transaction that was already in flight when we found it.
		 */
		imc_set(s, imc_chans[i].data, state->command[i] & ~GO_BIT);
	}
}

static int imc_prepare_xfer(struct imc_smbus *s, const struct imc_chan *c,
			    struct imc_xfer_state *state)
{
	int i, ret;

	ret = imc_wait_engine_idle(s);
	if (ret)
		return ret;

	for (i = 0; i < ARRAY_SIZE(imc_chans); i++) {
		state->command[i] = imc_reg(s, imc_chans[i].data);
		state->restore[i] = i == c->idx ||
				    (state->command[i] & TSOD_ACTIVE_BIT);
		if (!(state->command[i] & TSOD_ACTIVE_BIT))
			continue;

		imc_set(s, imc_chans[i].data,
			state->command[i] & COMMAND_KEEP_MASK);
		ret = imc_wait_go_clear(s, &imc_chans[i]);
		if (ret) {
			imc_restore_xfer(s, state);
			return ret;
		}
	}

	return 0;
}

static int imc_access(struct imc_smbus *s, const struct imc_chan *c, u8 addr,
		      u8 reg, char read_write, int size, u16 *value)
{
	struct imc_xfer_state state = {};
	u32 command, expected, raw, readback, status = 0;
	int ret;

	ret = imc_prepare_xfer(s, c, &state);
	if (ret)
		return ret;

	command = COMMAND_PREFIX | ((u32)addr << 8) | reg;
	if (size == I2C_SMBUS_WORD_DATA)
		command |= WORD_BIT;
	else if (size == I2C_SMBUS_BYTE)
		command |= PNTR_SEL_BIT;

	if (read_write == I2C_SMBUS_WRITE) {
		raw = size == I2C_SMBUS_WORD_DATA ? swab16(*value) : *value;
		imc_set(s, c->ctrl, raw << 16);
		command |= WRITE_OPERATION;
	}

	imc_set(s, c->data, command);
	readback = imc_reg(s, c->data);
	if (!imc_command_landed(command, readback)) {
		dev_warn_ratelimited(s->dev,
				     "ch%d command 0x%08x did not reach the register (read back 0x%08x)\n",
				     c->idx, command, readback);
		ret = -EIO;
		goto restore_command;
	}

	ret = imc_wait_transfer(s, c, &status);
	if (ret)
		goto restore_command;

	expected = read_write == I2C_SMBUS_WRITE ?
		   STAT_WRITE_DONE : STAT_READ_DONE;
	ret = imc_check_status(s, c, status, expected, addr, reg);
	if (ret)
		goto restore_command;

	ret = imc_check_interference(s, c, &state);
	if (ret)
		goto restore_command;

	if (read_write == I2C_SMBUS_READ) {
		raw = imc_reg(s, c->ctrl);
		*value = size == I2C_SMBUS_WORD_DATA ?
			 swab16(raw & 0xffff) : raw & 0xff;
	}

restore_command:
	imc_restore_xfer(s, &state);

	return ret;
}

/*
 * Standard SMBus transfer callback.  The per-channel imc_chan is stashed in the
 * adapter's algo_data.  The command word carries the 7-bit address, so
 * SPD EEPROMs (0x50-0x57) are reachable on each channel.  BYTE, BYTE_DATA and
 * WORD_DATA are supported; larger transfers would need the engine's block
 * primitives, which are not used by the devices on this bus.
 */
static s32 imc_smbus_xfer(struct i2c_adapter *adap, u16 addr,
			  unsigned short flags, char read_write, u8 command,
			  int size, union i2c_smbus_data *data)
{
	struct imc_smbus *s = i2c_get_adapdata(adap);
	const struct imc_chan *c = adap->algo_data;
	u16 val = 0;
	u8 reg;
	int ret;

	if (flags & I2C_M_TEN)
		return -EAFNOSUPPORT;
	if (addr > 0x7f)
		return -EINVAL;

	if (size != I2C_SMBUS_BYTE && size != I2C_SMBUS_BYTE_DATA &&
	    size != I2C_SMBUS_WORD_DATA)
		return -EOPNOTSUPP;
	if (read_write != I2C_SMBUS_READ && read_write != I2C_SMBUS_WRITE)
		return -EINVAL;

	/*
	 * BYTE puts no register on the bus.  For a write the command parameter
	 * carries the data byte instead, and data is NULL.
	 */
	reg = size == I2C_SMBUS_BYTE ? 0 : command;

	mutex_lock(&s->lock);
	s->io_err = 0;

	if (read_write == I2C_SMBUS_WRITE) {
		switch (size) {
		case I2C_SMBUS_BYTE:
			val = command;
			dev_dbg(s->dev, "ch%d W addr=%02x val=%02x (send byte)\n",
				c->idx, addr, val);
			break;
		case I2C_SMBUS_WORD_DATA:
			val = data->word;
			dev_dbg(s->dev, "ch%d W addr=%02x reg=%02x val=%04x\n",
				c->idx, addr, reg, val);
			break;
		default:
			val = data->byte;
			dev_dbg(s->dev, "ch%d W addr=%02x reg=%02x val=%02x\n",
				c->idx, addr, reg, val);
			break;
		}
		ret = imc_access(s, c, addr, reg, read_write, size, &val);
	} else {
		ret = imc_access(s, c, addr, reg, read_write, size, &val);
		switch (size) {
		case I2C_SMBUS_BYTE:
			if (!ret)
				data->byte = val;
			dev_dbg(s->dev, "ch%d R addr=%02x -> %02x (receive byte, ret %d)\n",
				c->idx, addr, val, ret);
			break;
		case I2C_SMBUS_WORD_DATA:
			if (!ret)
				data->word = val;
			dev_dbg(s->dev, "ch%d R addr=%02x reg=%02x -> %04x (ret %d)\n",
				c->idx, addr, reg, val, ret);
			break;
		default:
			if (!ret)
				data->byte = val;
			dev_dbg(s->dev, "ch%d R addr=%02x reg=%02x -> %02x (ret %d)\n",
				c->idx, addr, reg, val, ret);
			break;
		}
	}

	/*
	 * A config-space failure invalidates whatever the transaction logic
	 * concluded, so it wins over any other result.
	 */
	if (s->io_err) {
		dev_warn_ratelimited(s->dev, "config access failed: %d\n",
				     s->io_err);
		ret = s->io_err;
	}
	mutex_unlock(&s->lock);

	return ret;
}

static u32 imc_func(struct i2c_adapter *adap)
{
	return I2C_FUNC_SMBUS_BYTE | I2C_FUNC_SMBUS_BYTE_DATA |
	       I2C_FUNC_SMBUS_WORD_DATA;
}

static const struct i2c_algorithm imc_algo = {
	.smbus_xfer	= imc_smbus_xfer,
	.functionality	= imc_func,
};

static int imc_suspend(struct device *dev)
{
	struct imc_smbus *s = dev_get_drvdata(dev);
	int i;

	for (i = 0; i < ARRAY_SIZE(s->adap); i++)
		i2c_mark_adapter_suspended(&s->adap[i]);

	return 0;
}

static int imc_resume(struct device *dev)
{
	struct imc_smbus *s = dev_get_drvdata(dev);
	int i;

	for (i = 0; i < ARRAY_SIZE(s->adap); i++)
		i2c_mark_adapter_resumed(&s->adap[i]);

	return 0;
}

static DEFINE_SIMPLE_DEV_PM_OPS(imc_pm_ops, imc_suspend, imc_resume);

/*
 * Refuse to share the engine with the memory controller's own thermal
 * throttling.  SMBCNTL.TSOD_POLL_EN tells us directly whether the iMC is
 * issuing SMBus transactions of its own; Intel documents that SPD command
 * access and TSOD polling are mutually exclusive, so a set bit means the
 * engine is not ours to drive.  This does not cover SMM or a BMC, which
 * remain the reason for the allow_unsafe_access gate.
 */
static int imc_check_firmware_polling(struct pci_dev *pdev)
{
	struct pci_dev *imc = NULL;
	unsigned int found = 0;
	int i;

	while ((imc = pci_get_device(PCI_VENDOR_ID_INTEL, IMC_CHANNEL_DEVICE,
				     imc))) {
		found++;
		for (i = 0; i < ARRAY_SIZE(imc_chans); i++) {
			u32 cntl;

			if (pci_read_config_dword(imc, IMC_SMBCNTL(i), &cntl)) {
				dev_warn(&pdev->dev,
					 "%s ch%d SMBCNTL unreadable; cannot verify that TSOD polling is disabled\n",
					 pci_name(imc), i);
				continue;
			}

			if (cntl & SMBCNTL_TSOD_POLL_EN) {
				dev_err(&pdev->dev,
					"%s ch%d has TSOD polling enabled (SMBCNTL 0x%08x); the memory controller is using the engine\n",
					pci_name(imc), i, cntl);
				pci_dev_put(imc);
				return -EBUSY;
			}

			dev_dbg(&pdev->dev,
				"%s ch%d SMBCNTL 0x%08x, TSOD present mask 0x%02lx\n",
				pci_name(imc), i, cntl,
				cntl & SMBCNTL_TSOD_PRESENT);
		}
	}

	if (!found)
		dev_warn(&pdev->dev,
			 "no iMC channel function found; cannot verify that TSOD polling is disabled\n");

	return 0;
}

static int imc_pci_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
	struct imc_smbus *s;
	int ret, i;

	if (!allow_unsafe_access) {
		dev_info(&pdev->dev,
			 "firmware/SMM arbitration is unknown; set allow_unsafe_access=1 to bind\n");
		return -ENODEV;
	}

	ret = imc_check_firmware_polling(pdev);
	if (ret)
		return ret;

	s = devm_kzalloc(&pdev->dev, sizeof(*s), GFP_KERNEL);
	if (!s)
		return -ENOMEM;

	/*
	 * No pci_enable_device(): the driver uses no BAR, no interrupt and no
	 * DMA.  The engine is reached entirely through config space, which does
	 * not require the function's memory or I/O decoding to be enabled.
	 */
	s->pdev = pdev;
	s->dev = &pdev->dev;
	pci_set_drvdata(pdev, s);
	ret = devm_mutex_init(&pdev->dev, &s->lock);
	if (ret)
		return ret;

	/*
	 * Lifetime safety: the I2C core guarantees that smbus_xfer callbacks
	 * are not invoked after i2c_del_adapter() returns. Since we use
	 * devm_i2c_add_adapter(), the adapter is automatically removed on
	 * driver detach, and no concurrent xfer can be in flight at that point.
	 */
	for (i = 0; i < ARRAY_SIZE(s->adap); i++) {
		struct i2c_adapter *a = &s->adap[i];

		a->owner     = THIS_MODULE;
		a->algo      = &imc_algo;
		a->algo_data = (void *)&imc_chans[i];
		a->dev.parent = &pdev->dev;
		/*
		 * One retry: interference from another master is reported as
		 * -EAGAIN and is usually over by the next attempt.
		 */
		a->retries = 1;
		i2c_set_adapdata(a, s);
		snprintf(a->name, sizeof(a->name),
			 "iMC SMBus Skylake-X channel %d", i);

		ret = devm_i2c_add_adapter(&pdev->dev, a);
		if (ret) {
			dev_err(&pdev->dev,
				"i2c_add_adapter ch%d failed: %d\n", i, ret);
			return ret;
		}

		/*
		 * Instantiate the SPD EEPROMs the same way the other SMBus
		 * host drivers do.  This counts the populated slots from DMI
		 * and probes 0x50 upwards, so no client is created for an
		 * address that does not answer and userspace keeps the ones
		 * that stay empty.  Each channel carries half the DIMMs, and
		 * the scan is per adapter, so each finds its own.
		 *
		 * The write_disable variant: this part is DDR4 only, where the
		 * two differ in nothing (the distinction gates spd5118 on
		 * DDR5).  Where they are equivalent, the one that does not
		 * suggest this driver enables SPD writes is the better name to
		 * leave in a memory-bus driver.
		 */
		i2c_register_spd_write_disable(a);
	}

	dev_dbg(&pdev->dev, "registered 2 SMBus channels\n");
	return 0;
}

static const struct pci_device_id imc_pci_ids[] = {
	{ PCI_DEVICE(PCI_VENDOR_ID_INTEL, PCU_DEVICE) },
	{ }
};
MODULE_DEVICE_TABLE(pci, imc_pci_ids);

/*
 * All resources (mutex, i2c adapters) are devm-managed and are released
 * automatically after probe returns or during device unbinding, so no .remove
 * callback is needed.  The register state is deliberately left as-is on
 * unbind: the engine carries no driver-private latched state that needs
 * teardown, and the SMBus controls we drive are side-band registers the rest
 * of the kernel does not touch.
 */
static struct pci_driver imc_driver = {
	.name     = "i2c-imc-skylake",
	.id_table = imc_pci_ids,
	.probe    = imc_pci_probe,
	.driver.pm = pm_sleep_ptr(&imc_pm_ops),
};
module_pci_driver(imc_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Simone Chifari");
MODULE_DESCRIPTION("Intel Skylake-X iMC SMBus I2C adapter");

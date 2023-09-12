// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * NCT6686D Watchdog Driver
 */

#define dev_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/acpi.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/iommu.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/platform_device.h>
#include <linux/watchdog.h>

static bool force;
module_param(force, bool, 0);
MODULE_PARM_DESC(force, "Set to one to enable support for unknown vendors");

#define DRVNAME "nct6686_wdt"

/*
 * Super-I/O constants and functions
 */
#define NCT6686_LD_HWM          0x0b
#define SIO_REG_LDSEL           0x07    /* Logical device select */
#define SIO_REG_DEVID           0x20    /* Device ID (2 bytes) */
#define SIO_REG_ENABLE          0x30    /* Logical device enable */
#define SIO_REG_ADDR            0x60    /* Logical device address (2 bytes) */

#define SIO_NCT6686_ID          0xd440
#define SIO_ID_MASK             0xFFF0

#define WDT_CFG                 0x828    /* W/O Lock Watchdog Register */
#define WDT_CNT                 0x829    /* R/W Watchdog Timer Register */
#define WDT_STS                 0x82A    /* R/O Watchdog Status Register */
#define WDT_STS_EVT_POS         (0)
#define WDT_STS_EVT_MSK         (0x3 << WDT_STS_EVT_POS)
#define WDT_SOFT_EN             0x87    /* Enable soft watchdog timer */
#define WDT_SOFT_DIS            0xAA    /* Disable soft watchdog timer */

#define WATCHDOG_TIMEOUT        60      /* 1 minute default timeout */

/* The timeout range is 1-255 seconds */
#define MIN_TIMEOUT             1
#define MAX_TIMEOUT             255

static int timeout;
module_param(timeout, int, 0);
MODULE_PARM_DESC(timeout, "Watchdog timeout in seconds. 1 <= timeout <= 255, default="
		 __MODULE_STRING(WATCHDOG_TIMEOUT) ".");

static bool nowayout = WATCHDOG_NOWAYOUT;
module_param(nowayout, bool, 0);
MODULE_PARM_DESC(nowayout, "Watchdog cannot be stopped once started (default="
		 __MODULE_STRING(WATCHDOG_NOWAYOUT) ")");

static int early_disable;
module_param(early_disable, int, 0);
MODULE_PARM_DESC(early_disable, "Disable watchdog at boot time (default=0)");

#define NCT6686_PAGE_REG_OFFSET         0
#define NCT6686_ADDR_REG_OFFSET         1
#define NCT6686_DATA_REG_OFFSET         2

struct watchdog_device wdt;

struct nct6686_sio_data {
	int sioreg;
};

struct nct6686_data {
	int addr;                          /* IO base of EC space */
	struct nct6686_sio_data sio_data;  /* SIO register */

	struct watchdog_device wdt;
	struct mutex update_lock; /* used to protect sensor updates */
};

static inline void
superio_outb(int ioreg, int reg, int val)
{
	outb(reg, ioreg);
	outb(val, ioreg + 1);
}

static inline int
superio_inb(int ioreg, int reg)
{
	outb(reg, ioreg);
	return inb(ioreg + 1);
}

static inline void
superio_select(int ioreg, int ld)
{
	outb(SIO_REG_LDSEL, ioreg);
	outb(ld, ioreg + 1);
}

static inline int
superio_enter(int ioreg)
{
	/*
	 * Try to reserve <ioreg> and <ioreg + 1> for exclusive access.
	 */
	if (!request_muxed_region(ioreg, 2, DRVNAME))
		return -EBUSY;

	outb(0x87, ioreg);
	outb(0x87, ioreg);

	return 0;
}

static inline void
superio_exit(int ioreg)
{
	outb(0xaa, ioreg);
	outb(0x02, ioreg);
	outb(0x02, ioreg + 1);
	release_region(ioreg, 2);
}

/*
 * ISA constants
 */

#define IOREGION_ALIGNMENT      (~7)
#define IOREGION_OFFSET         4       /* Use EC port 1 */
#define IOREGION_LENGTH         4

#define EC_PAGE_REG             0
#define EC_INDEX_REG            1
#define EC_DATA_REG             2
#define EC_EVENT_REG            3

static inline void nct6686_wdt_set_bank(int base_addr, u16 reg)
{
	outb_p(0xFF, base_addr + NCT6686_PAGE_REG_OFFSET);
	outb_p(reg >> 8, base_addr + NCT6686_PAGE_REG_OFFSET);
}

/* Not strictly necessary, but play it safe for now */
static inline void nct6686_wdt_reset_bank(int base_addr, u16 reg)
{
	if (reg & 0xff00)
		outb_p(0xFF, base_addr + NCT6686_PAGE_REG_OFFSET);
}

static u8 nct6686_read(struct nct6686_data *data, u16 reg)
{
	u8 res;

	nct6686_wdt_set_bank(data->addr, reg);
	outb_p(reg & 0xff, data->addr + NCT6686_ADDR_REG_OFFSET);
	res = inb_p(data->addr + NCT6686_DATA_REG_OFFSET);

	nct6686_wdt_reset_bank(data->addr, reg);
	return res;
}

static void nct6686_write(struct nct6686_data *data, u16 reg, u8 value)
{
	nct6686_wdt_set_bank(data->addr, reg);
	outb_p(reg & 0xff, data->addr + NCT6686_ADDR_REG_OFFSET);
	outb_p(value & 0xff, data->addr + NCT6686_DATA_REG_OFFSET);
	nct6686_wdt_reset_bank(data->addr, reg);
}

/*
 * Watchdog Functions
 */
static int nct6686_wdt_enable(struct watchdog_device *wdog, bool enable)
{
	struct nct6686_data *data = watchdog_get_drvdata(wdog);

	u_char reg;

	mutex_lock(&data->update_lock);
	reg = nct6686_read(data, WDT_CFG);

	if (enable) {
		nct6686_write(data, WDT_CFG, reg | 0x3);
		mutex_unlock(&data->update_lock);
		return 0;
	}

	nct6686_write(data, WDT_CFG, reg & ~BIT(0));
	mutex_unlock(&data->update_lock);

	return 0;
}

static int nct6686_wdt_set_time(struct watchdog_device *wdog)
{
	struct nct6686_data *data = watchdog_get_drvdata(wdog);

	mutex_lock(&data->update_lock);
	nct6686_write(data, WDT_CNT, wdog->timeout);
	mutex_unlock(&data->update_lock);

	if (wdog->timeout) {
		nct6686_wdt_enable(wdog, true);
		return 0;
	}

	nct6686_wdt_enable(wdog, false);
	return 0;
}

static int nct6686_wdt_start(struct watchdog_device *wdt)
{
	struct nct6686_data *data = watchdog_get_drvdata(wdt);
	u_char reg;

	nct6686_wdt_set_time(wdt);

	/* Enable soft watchdog timer */
	mutex_lock(&data->update_lock);
	/* reset trigger status */
	reg = nct6686_read(data, WDT_STS);
	nct6686_write(data, WDT_STS, reg & ~WDT_STS_EVT_MSK);
	mutex_unlock(&data->update_lock);
	return 0;
}

static int nct6686_wdt_stop(struct watchdog_device *wdt)
{
	struct nct6686_data *data = watchdog_get_drvdata(wdt);

	mutex_lock(&data->update_lock);
	nct6686_write(data, WDT_CFG, WDT_SOFT_DIS);
	mutex_unlock(&data->update_lock);
	return 0;
}

static int nct6686_wdt_set_timeout(struct watchdog_device *wdt,
				   unsigned int timeout)
{
	struct nct6686_data *data = watchdog_get_drvdata(wdt);

	wdt->timeout = timeout;
	mutex_lock(&data->update_lock);
	nct6686_write(data, WDT_CNT, timeout);
	mutex_unlock(&data->update_lock);
	return 0;
}

static int nct6686_wdt_ping(struct watchdog_device *wdt)
{
	struct nct6686_data *data = watchdog_get_drvdata(wdt);
	int timeout;

	/*
	 * Note:
	 * NCT6686 does not support refreshing WDT_TIMER_REG register when
	 * the watchdog is active. Please disable watchdog before feeding
	 * the watchdog and enable it again.
	 */
	/* Disable soft watchdog timer */
	nct6686_wdt_enable(wdt, false);

	/* feed watchdog */
	timeout = wdt->timeout;
	mutex_lock(&data->update_lock);
	nct6686_write(data, WDT_CNT, timeout);
	mutex_unlock(&data->update_lock);

	/* Enable soft watchdog timer */
	nct6686_wdt_enable(wdt, true);
	return 0;
}

static unsigned int nct6686_wdt_get_timeleft(struct watchdog_device *wdt)
{
	struct nct6686_data *data = watchdog_get_drvdata(wdt);
	int ret;

	mutex_lock(&data->update_lock);
	ret = nct6686_read(data, WDT_CNT);
	mutex_unlock(&data->update_lock);
	if (ret < 0)
		return 0;

	return ret;
}

static const struct watchdog_info nct6686_wdt_info = {
	.options        = WDIOF_SETTIMEOUT | WDIOF_KEEPALIVEPING |
			  WDIOF_MAGICCLOSE,
	.identity       = "nct6686 watchdog",
};

static const struct watchdog_ops nct6686_wdt_ops = {
	.owner          = THIS_MODULE,
	.start          = nct6686_wdt_start,
	.stop           = nct6686_wdt_stop,
	.ping           = nct6686_wdt_ping,
	.set_timeout    = nct6686_wdt_set_timeout,
	.get_timeleft   = nct6686_wdt_get_timeleft,
};

static int nct6686_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct resource *res;
	struct nct6686_data *data;
	struct nct6686_sio_data *sio_data = dev->platform_data;
	int ret;
	u_char reg;

	dev_dbg(&pdev->dev, "Probe NCT6686 called\n");
	res = platform_get_resource(pdev, IORESOURCE_IO, 0);
	if (!devm_request_region(dev, res->start, IOREGION_LENGTH, DRVNAME))
		return -EBUSY;

	data = devm_kzalloc(dev, sizeof(struct nct6686_data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	data->sio_data.sioreg = sio_data->sioreg;
	data->addr = res->start;
	mutex_init(&data->update_lock);
	platform_set_drvdata(pdev, data);

	/* Watchdog initialization */
	data->wdt.ops = &nct6686_wdt_ops;
	data->wdt.info = &nct6686_wdt_info;

	data->wdt.timeout = WATCHDOG_TIMEOUT; /* Set default timeout */
	data->wdt.min_timeout = MIN_TIMEOUT;
	data->wdt.max_timeout = MAX_TIMEOUT;
	data->wdt.parent = &pdev->dev;

	watchdog_init_timeout(&data->wdt, timeout, &pdev->dev);
	watchdog_set_nowayout(&data->wdt, nowayout);
	watchdog_set_drvdata(&data->wdt, data);

	/* reset trigger status */
	reg = nct6686_read(data, WDT_STS);
	nct6686_write(data, WDT_STS, reg & ~WDT_STS_EVT_MSK);

	watchdog_stop_on_unregister(&data->wdt);

	ret = devm_watchdog_register_device(dev, &data->wdt);

	dev_dbg(&pdev->dev, "initialized. timeout=%d sec (nowayout=%d)\n",
		data->wdt.timeout, nowayout);

	return ret;
}

static struct platform_driver nct6686_driver = {
	.driver = {
		.name = DRVNAME,
	},
	.probe  = nct6686_probe,
};

static int __init nct6686_find(int sioaddr, u_long *base_phys)
{
	u16 val;
	int err = 0;
	u_long addr;

	err = superio_enter(sioaddr);
	if (err)
		return err;

	val = (superio_inb(sioaddr, SIO_REG_DEVID) << 8)
	       | superio_inb(sioaddr, SIO_REG_DEVID + 1);

	if ((val & SIO_ID_MASK) != SIO_NCT6686_ID)
		goto fail;

	/* We have a known chip, find the HWM I/O address */
	superio_select(sioaddr, NCT6686_LD_HWM);
	val = (superio_inb(sioaddr, SIO_REG_ADDR) << 8)
	      | superio_inb(sioaddr, SIO_REG_ADDR + 1);
	addr = val & IOREGION_ALIGNMENT;
	if (addr == 0) {
		pr_err("EC base I/O port unconfigured\n");
		goto fail;
	}

	/* Activate logical device if needed */
	val = superio_inb(sioaddr, SIO_REG_ENABLE);
	if (!(val & 0x01)) {
		pr_warn("Forcibly enabling EC access. Data may be unusable.\n");
		superio_outb(sioaddr, SIO_REG_ENABLE, val | 0x01);
	}

	superio_exit(sioaddr);
	*base_phys = addr;

	return 0;

fail:
	superio_exit(sioaddr);
	return -ENODEV;
}

static struct platform_device *pdev;

static int __init nct6686_init(void)
{
	struct nct6686_sio_data sio_data;
	int sioaddr[2] = { 0x2e, 0x4e };
	struct resource res;
	int err;
	u_long addr;

	/*
	 * when Super-I/O functions move to a separate file, the Super-I/O
	 * driver will probe 0x2e and 0x4e and auto-detect the presence of a
	 * nct6686 hardware monitor, and call probe()
	 */
	err = nct6686_find(sioaddr[0], &addr);
	if (err) {
		err = nct6686_find(sioaddr[1], &addr);
		if (err)
			return -ENODEV;
	}

	pdev = platform_device_alloc(DRVNAME, addr);
	if (!pdev) {
		err = -ENOMEM;
		goto exit_device_unregister;
	}

	err = platform_device_add_data(pdev, &sio_data,
				       sizeof(struct nct6686_sio_data));
	if (err)
		goto exit_device_put;

	memset(&res, 0, sizeof(res));
	res.name = DRVNAME;
	res.start = addr + IOREGION_OFFSET;
	res.end = addr + IOREGION_OFFSET + IOREGION_LENGTH - 1;
	res.flags = IORESOURCE_IO;

	err = acpi_check_resource_conflict(&res);
	if (err)
		return err;

	platform_driver_register(&nct6686_driver);

	pdev = platform_device_alloc(DRVNAME, addr);
	if (!pdev) {
		err = -ENOMEM;
		goto exit_device_unregister;
	}
	err = platform_device_add_data(pdev, &sio_data,
				       sizeof(struct nct6686_sio_data));
	if (err)
		goto exit_device_put;

	err = platform_device_add_resources(pdev, &res, 1);
	if (err)
		goto exit_device_put;

	dev_info(&pdev->dev, "NCT6686 device found\n");
	/* platform_device_add calls probe() */
	err = platform_device_add(pdev);
	if (err)
		goto exit_device_put;

	return 0;
exit_device_put:
	platform_device_put(pdev);
	platform_device_unregister(pdev);
exit_device_unregister:
	platform_driver_unregister(&nct6686_driver);
	return err;
}

static void __exit nct6686_exit(void)
{
	platform_device_unregister(pdev);
	platform_driver_unregister(&nct6686_driver);
}

MODULE_AUTHOR("David Ober <dober@lenovo.com>");
MODULE_DESCRIPTION("NCT6686D driver");
MODULE_LICENSE("GPL");

module_init(nct6686_init);
module_exit(nct6686_exit);

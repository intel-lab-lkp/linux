// SPDX-License-Identifier: GPL-2.0+
/*
 *	w83627hf/thf WDT driver
 *
 *	(c) Copyright 2013 Guenter Roeck
 *		converted to watchdog infrastructure
 *
 *	(c) Copyright 2007 Vlad Drukker <vlad@storewiz.com>
 *		added support for W83627THF.
 *
 *	(c) Copyright 2003,2007 Pádraig Brady <P@draigBrady.com>
 *
 *	Based on advantechwdt.c which is based on wdt.c.
 *	Original copyright messages:
 *
 *	(c) Copyright 2000-2001 Marek Michalkiewicz <marekm@linux.org.pl>
 *
 *	(c) Copyright 1996 Alan Cox <alan@lxorguk.ukuu.org.uk>,
 *						All Rights Reserved.
 *
 *	Neither Alan Cox nor CymruNet Ltd. admit liability nor provide
 *	warranty for any of this software. This material is provided
 *	"AS-IS" and at no charge.
 *
 *	(c) Copyright 1995    Alan Cox <alan@lxorguk.ukuu.org.uk>
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/bits.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/moduleparam.h>
#include <linux/types.h>
#include <linux/watchdog.h>
#include <linux/ioport.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/dmi.h>

#define WATCHDOG_NAME "w83627hf/thf/hg/dhg WDT"
#define WATCHDOG_TIMEOUT 60		/* 60 sec default timeout */
#define WATCHDOG_MAX_TIMEOUT (255 * 60)

enum chips { w83627hf, w83627s, w83697hf, w83697ug, w83637hf, w83627thf,
	     w83687thf, w83627ehf, w83627dhg, w83627uhg, w83667hg, w83627dhg_p,
	     w83667hg_b, nct6775, nct6776, nct6779, nct6791, nct6792, nct6793,
	     nct6795, nct6796, nct6102, nct6116, nct6126 };

static int timeout;			/* in seconds */
module_param(timeout, int, 0);
MODULE_PARM_DESC(timeout,
		"Watchdog timeout in seconds. 1 <= timeout <= "
				__MODULE_STRING(WATCHDOG_MAX_TIMEOUT) ", default="
				__MODULE_STRING(WATCHDOG_TIMEOUT) ".");

static bool nowayout = WATCHDOG_NOWAYOUT;
module_param(nowayout, bool, 0);
MODULE_PARM_DESC(nowayout,
		"Watchdog cannot be stopped once started (default="
				__MODULE_STRING(WATCHDOG_NOWAYOUT) ")");

static int early_disable;
module_param(early_disable, int, 0);
MODULE_PARM_DESC(early_disable, "Disable watchdog at boot time (default=0)");

/*
 *	Kernel methods.
 */

#define SIO_REG_LDSEL		0x07	/* Logical device select */
#define SIO_REG_DEVID		0x20	/* Device ID (1 or 2 bytes) */
#define SIO_REG_ENABLE		0x30	/* Logical device enable */
#define SIO_REG_CONF_ADDR0	0x2E
#define SIO_REG_CONF_ADDR1	0x4E

#define W83627HF_LD_WDT		0x08

#define W83627HF_ID		0x52
#define W83627S_ID		0x59
#define W83697HF_ID		0x60
#define W83697UG_ID		0x68
#define W83637HF_ID		0x70
#define W83627THF_ID		0x82
#define W83687THF_ID		0x85
#define W83627EHF_ID		0x88
#define W83627DHG_ID		0xa0
#define W83627UHG_ID		0xa2
#define W83667HG_ID		0xa5
#define W83627DHG_P_ID		0xb0
#define W83667HG_B_ID		0xb3
#define NCT6775_ID		0xb4
#define NCT6776_ID		0xc3
#define NCT6102_ID		0xc4
#define NCT6116_ID		0xd2	/* also NCT6126D */
#define NCT6126_VER_A_LOW_ID	0x83	/* ... version A */
#define NCT6126_VER_B_LOW_ID	0x84	/* ... version B */
#define NCT6779_ID		0xc5
#define NCT6791_ID		0xc8
#define NCT6792_ID		0xc9
#define NCT6793_ID		0xd1
#define NCT6795_ID		0xd3
#define NCT6796_ID		0xd4	/* also NCT9697D, NCT9698D */

#define W83627HF_WDT_TIMEOUT	0xf6
#define W83697HF_WDT_TIMEOUT	0xf4
#define NCT6102D_WDT_TIMEOUT	0xf1

#define W83627HF_WDT_CONTROL	0xf5
#define W83697HF_WDT_CONTROL	0xf3
#define NCT6102D_WDT_CONTROL	0xf0

#define W836X7HF_WDT_CSR	0xf7
#define NCT6102D_WDT_CSR	0xf2

#define WDT_CSR_STATUS		BIT(4)
#define WDT_CSR_KBD_INT_RESET	BIT(6)
#define WDT_CSR_MOUSE_INT_RESET	BIT(7)

#define WDT_CTRL_RISING_EDGE_KBD_RESET	BIT(2)
#define WDT_CTRL_MINUTE_MODE		BIT(3)

struct wdt_pdata {
	int siocfg_enter;
	int siocfg_leave;
};

struct w83627hf_data {
	struct watchdog_device wdd;
	struct watchdog_info info;
	struct {
		int control;
		int timeout;
		int csr;
	} reg;
	int sioaddr;
	int siocfg_enter;
	int siocfg_leave;
	bool minute_mode;
	u8 early_timer_val;
	u8 timer_val;
};

static void superio_outb(int base, int reg, int val)
{
	outb(reg, base);
	outb(val, base + 1);
}

static inline int superio_inb(int base, int reg)
{
	outb(reg, base);
	return inb(base + 1);
}

static int superio_enter(int base, int enter)
{
	if (!request_muxed_region(base, 2, WATCHDOG_NAME))
		return -EBUSY;

	outb_p(enter, base); /* Enter extended function mode */
	outb_p(enter, base); /* Again according to manual */

	return 0;
}

static void superio_select(int base, int ld)
{
	superio_outb(base, SIO_REG_LDSEL, ld);
}

static void superio_exit(int base, int leave)
{
	outb_p(leave, base); /* Leave extended function mode */
	release_region(base, 2);
}

static int w83627hf_init(struct watchdog_device *wdog, enum chips chip)
{
	struct w83627hf_data *data = watchdog_get_drvdata(wdog);
	int ret;
	unsigned char t;

	ret = superio_enter(data->sioaddr, data->siocfg_enter);
	if (ret)
		return ret;

	superio_select(data->sioaddr, W83627HF_LD_WDT);

	/* set CR30 bit 0 to activate GPIO2 */
	t = superio_inb(data->sioaddr, SIO_REG_ENABLE);
	if (!(t & 0x01))
		superio_outb(data->sioaddr, SIO_REG_ENABLE, t | 0x01);

	switch (chip) {
	case w83627hf:
	case w83627s:
		t = superio_inb(data->sioaddr, 0x2B) & ~0x10;
		superio_outb(data->sioaddr, 0x2B, t); /* set GPIO24 to WDT0 */
		break;
	case w83697hf:
		/* Set pin 119 to WDTO# mode (= CR29, WDT0) */
		t = superio_inb(data->sioaddr, 0x29) & ~0x60;
		t |= 0x20;
		superio_outb(data->sioaddr, 0x29, t);
		break;
	case w83697ug:
		/* Set pin 118 to WDTO# mode */
		t = superio_inb(data->sioaddr, 0x2b) & ~0x04;
		superio_outb(data->sioaddr, 0x2b, t);
		break;
	case w83627thf:
		t = (superio_inb(data->sioaddr, 0x2B) & ~0x08) | 0x04;
		superio_outb(data->sioaddr, 0x2B, t); /* set GPIO3 to WDT0 */
		break;
	case w83627dhg:
	case w83627dhg_p:
		t = superio_inb(data->sioaddr, 0x2D) & ~0x01; /* PIN77 -> WDT0# */
		superio_outb(data->sioaddr, 0x2D, t); /* set GPIO5 to WDT0 */
		t = superio_inb(data->sioaddr, data->reg.control);
		t |= 0x02;	/* enable the WDTO# output low pulse
				 * to the KBRST# pin */
		superio_outb(data->sioaddr, data->reg.control, t);
		break;
	case w83637hf:
		break;
	case w83687thf:
		t = superio_inb(data->sioaddr, 0x2C) & ~0x80; /* PIN47 -> WDT0# */
		superio_outb(data->sioaddr, 0x2C, t);
		break;
	case w83627ehf:
	case w83627uhg:
	case w83667hg:
	case w83667hg_b:
	case nct6775:
	case nct6776:
	case nct6779:
	case nct6791:
	case nct6792:
	case nct6793:
	case nct6795:
	case nct6796:
	case nct6102:
	case nct6116:
	case nct6126:
		/*
		 * These chips have a fixed WDTO# output pin (W83627UHG),
		 * or support more than one WDTO# output pin.
		 * Don't touch its configuration, and hope the BIOS
		 * does the right thing.
		 */
		t = superio_inb(data->sioaddr, data->reg.control);
		t |= 0x02;	/* enable the WDTO# output low pulse
				 * to the KBRST# pin */
		superio_outb(data->sioaddr, data->reg.control, t);
		break;
	default:
		break;
	}

	data->early_timer_val = superio_inb(data->sioaddr, data->reg.timeout);

	/* disable keyboard reset turning off watchdog */
	t = superio_inb(data->sioaddr, data->reg.control) &
	    ~WDT_CTRL_RISING_EDGE_KBD_RESET;
	superio_outb(data->sioaddr, data->reg.control, t);

	t = superio_inb(data->sioaddr, data->reg.csr);
	if (t & WDT_CSR_STATUS)
		wdog->bootstatus |= WDIOF_CARDRESET;

	/* reset status, disable keyboard & mouse interrupt turning off watchdog */
	t &= ~(WDT_CSR_STATUS | WDT_CSR_KBD_INT_RESET | WDT_CSR_MOUSE_INT_RESET);
	superio_outb(data->sioaddr, data->reg.csr, t);

	superio_exit(data->sioaddr, data->siocfg_leave);

	return 0;
}

static int wdt_set_time(struct watchdog_device *wdog, unsigned int timeout)
{
	struct w83627hf_data *data = watchdog_get_drvdata(wdog);
	unsigned char ctrl;
	int ret;

	ret = superio_enter(data->sioaddr, data->siocfg_enter);
	if (ret)
		return ret;

	superio_select(data->sioaddr, W83627HF_LD_WDT);

	ctrl = superio_inb(data->sioaddr, data->reg.control);

	if (data->minute_mode)
		ctrl |= WDT_CTRL_MINUTE_MODE;
	else
		ctrl &= ~WDT_CTRL_MINUTE_MODE;

	superio_outb(data->sioaddr, data->reg.control, ctrl);
	superio_outb(data->sioaddr, data->reg.timeout, timeout);
	superio_exit(data->sioaddr, data->siocfg_leave);

	return 0;
}

static int wdt_start(struct watchdog_device *wdog)
{
	struct w83627hf_data *data = watchdog_get_drvdata(wdog);

	return wdt_set_time(wdog, data->timer_val);
}

static int wdt_stop(struct watchdog_device *wdog)
{
	return wdt_set_time(wdog, 0);
}

static int wdt_set_timeout(struct watchdog_device *wdog, unsigned int timeout)
{
	struct w83627hf_data *data = watchdog_get_drvdata(wdog);

	if (timeout > 255) {
		data->minute_mode = true;
		data->timer_val = DIV_ROUND_UP(timeout, 60);
		timeout = data->timer_val * 60;
	} else {
		data->minute_mode = false;
		data->timer_val = timeout;
	}

	wdog->timeout = timeout;

	return 0;
}

static unsigned int wdt_get_time(struct watchdog_device *wdog)
{
	struct w83627hf_data *data = watchdog_get_drvdata(wdog);
	unsigned int timeleft;
	int ret;

	ret = superio_enter(data->sioaddr, data->siocfg_enter);
	if (ret)
		return 0;

	superio_select(data->sioaddr, W83627HF_LD_WDT);
	timeleft = superio_inb(data->sioaddr, data->reg.timeout);
	if (data->minute_mode)
		timeleft *= 60;
	superio_exit(data->sioaddr, data->siocfg_leave);

	return timeleft;
}

/*
 *	Kernel Interfaces
 */

static const struct watchdog_ops wdt_ops = {
	.owner = THIS_MODULE,
	.start = wdt_start,
	.stop = wdt_stop,
	.set_timeout = wdt_set_timeout,
	.get_timeleft = wdt_get_time,
};

/*
 *	The WDT needs to learn about soft shutdowns in order to
 *	turn the timebomb registers off.
 */

static int wdt_find(int addr, int enter, int leave)
{
	u8 val;
	int ret;

	ret = superio_enter(addr, enter);
	if (ret)
		return ret;
	superio_select(addr, W83627HF_LD_WDT);
	val = superio_inb(addr, SIO_REG_DEVID);
	switch (val) {
	case W83627HF_ID:
		ret = w83627hf;
		break;
	case W83627S_ID:
		ret = w83627s;
		break;
	case W83697HF_ID:
		ret = w83697hf;
		break;
	case W83697UG_ID:
		ret = w83697ug;
		break;
	case W83637HF_ID:
		ret = w83637hf;
		break;
	case W83627THF_ID:
		ret = w83627thf;
		break;
	case W83687THF_ID:
		ret = w83687thf;
		break;
	case W83627EHF_ID:
		ret = w83627ehf;
		break;
	case W83627DHG_ID:
		ret = w83627dhg;
		break;
	case W83627DHG_P_ID:
		ret = w83627dhg_p;
		break;
	case W83627UHG_ID:
		ret = w83627uhg;
		break;
	case W83667HG_ID:
		ret = w83667hg;
		break;
	case W83667HG_B_ID:
		ret = w83667hg_b;
		break;
	case NCT6775_ID:
		ret = nct6775;
		break;
	case NCT6776_ID:
		ret = nct6776;
		break;
	case NCT6779_ID:
		ret = nct6779;
		break;
	case NCT6791_ID:
		ret = nct6791;
		break;
	case NCT6792_ID:
		ret = nct6792;
		break;
	case NCT6793_ID:
		ret = nct6793;
		break;
	case NCT6795_ID:
		ret = nct6795;
		break;
	case NCT6796_ID:
		ret = nct6796;
		break;
	case NCT6102_ID:
		ret = nct6102;
		break;
	case NCT6116_ID:
		val = superio_inb(addr, SIO_REG_DEVID + 1);
		if (val == NCT6126_VER_A_LOW_ID || val == NCT6126_VER_B_LOW_ID)
			ret = nct6126;
		else
			ret = nct6116;
		break;
	case 0xff:
		ret = -ENODEV;
		break;
	default:
		ret = -ENODEV;
		pr_err("Unsupported chip ID: 0x%02x\n", val);
		break;
	}
	superio_exit(addr, leave);
	return ret;
}

static int wdt_probe(struct platform_device *pdev)
{
	const struct platform_device_id *id = platform_get_device_id(pdev);
	const struct wdt_pdata *pdata = pdev->dev.platform_data;
	enum chips chip = id->driver_data;
	struct watchdog_device *wdd;
	struct w83627hf_data *data;
	struct resource *res;
	int ret;

	pr_info("WDT driver for %s Super I/O chip initialising\n", id->name);

	res = platform_get_resource(pdev, IORESOURCE_IO, 0);
	if (!res)
		return -ENXIO;

	data = devm_kzalloc(&pdev->dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	data->info.options = WDIOF_SETTIMEOUT | WDIOF_KEEPALIVEPING | WDIOF_MAGICCLOSE;
	snprintf(data->info.identity, sizeof(data->info.identity),
		 "%s Watchdog", id->name);

	wdd = &data->wdd;

	wdd->info = &data->info;
	wdd->ops = &wdt_ops;
	wdd->timeout = WATCHDOG_TIMEOUT;
	wdd->min_timeout = 1;
	wdd->max_timeout = WATCHDOG_MAX_TIMEOUT;

	data->sioaddr = res->start;
	data->siocfg_enter = pdata->siocfg_enter;
	data->siocfg_leave = pdata->siocfg_leave;
	data->reg.timeout = W83627HF_WDT_TIMEOUT;
	data->reg.control = W83627HF_WDT_CONTROL;
	data->reg.csr = W836X7HF_WDT_CSR;

	if (chip == nct6102 || chip == nct6116 || chip == nct6126) {
		data->reg.timeout = NCT6102D_WDT_TIMEOUT;
		data->reg.control = NCT6102D_WDT_CONTROL;
		data->reg.csr = NCT6102D_WDT_CSR;
	}

	if (chip == w83697hf || chip == w83697ug) {
		data->reg.timeout = W83697HF_WDT_TIMEOUT;
		data->reg.control = W83697HF_WDT_CONTROL;
	}

	watchdog_set_drvdata(wdd, data);
	watchdog_init_timeout(wdd, timeout, NULL);
	watchdog_set_nowayout(wdd, nowayout);
	watchdog_stop_on_reboot(wdd);

	wdt_set_timeout(wdd, wdd->timeout);

	ret = w83627hf_init(wdd, chip);
	if (ret) {
		pr_err("failed to initialize watchdog (err=%d)\n", ret);
		return ret;
	}

	if (data->early_timer_val) {
		if (early_disable) {
			pr_warn("Stopping previously enabled watchdog until userland kicks in\n");
			ret = wdt_stop(wdd);
		} else {
			pr_info("Watchdog already running. Resetting timeout to %d sec\n",
				wdd->timeout);
			ret = wdt_start(wdd);
			set_bit(WDOG_HW_RUNNING, &wdd->status);
		}

		if (ret)
			return ret;
	}

	ret = devm_watchdog_register_device(&pdev->dev, wdd);
	if (ret)
		return ret;

	pr_info("initialized. timeout=%d sec (nowayout=%d)\n", wdd->timeout,
		nowayout);

	return ret;
}

/*
 * On some systems, the NCT6791D comes with a companion chip and the
 * watchdog function is in this companion chip. We must use a different
 * unlocking sequence to access the companion chip.
 */
static int __init wdt_use_alt_key(const struct dmi_system_id *d)
{
	struct wdt_pdata *pdata = d->driver_data;

	pdata->siocfg_enter = 0x88;
	pdata->siocfg_leave = 0xBB;

	return 0;
}

static struct wdt_pdata pdata;

static const struct dmi_system_id wdt_dmi_table[] __initconst = {
	{
		.matches = {
			DMI_EXACT_MATCH(DMI_SYS_VENDOR, "INVES"),
			DMI_EXACT_MATCH(DMI_PRODUCT_NAME, "CTS"),
			DMI_EXACT_MATCH(DMI_BOARD_VENDOR, "INVES"),
			DMI_EXACT_MATCH(DMI_BOARD_NAME, "SHARKBAY"),
		},
		.callback = wdt_use_alt_key,
		.driver_data = &pdata,
	},
	{}
};

static const struct platform_device_id wdt_ids[] = {
	{ .name = "W83627HF", .driver_data = w83627hf },
	{ .name = "W83627S", .driver_data = w83627s },
	{ .name = "W83697HF", .driver_data = w83697hf },
	{ .name = "W83697UG", .driver_data = w83697ug },
	{ .name = "W83637HF", .driver_data = w83637hf },
	{ .name = "W83627THF", .driver_data = w83627thf },
	{ .name = "W83687THF", .driver_data = w83687thf },
	{ .name = "W83627EHF", .driver_data = w83627ehf },
	{ .name = "W83627DHG", .driver_data = w83627dhg },
	{ .name = "W83627UHG", .driver_data = w83627uhg },
	{ .name = "W83667HG", .driver_data = w83667hg },
	{ .name = "W83667DHG-P", .driver_data = w83627dhg_p },
	{ .name = "W83667HG-B", .driver_data = w83667hg_b },
	{ .name = "NCT6775", .driver_data = nct6775 },
	{ .name = "NCT6776", .driver_data = nct6776 },
	{ .name = "NCT6779", .driver_data = nct6779 },
	{ .name = "NCT6791", .driver_data = nct6791 },
	{ .name = "NCT6792", .driver_data = nct6792 },
	{ .name = "NCT6793", .driver_data = nct6793 },
	{ .name = "NCT6795", .driver_data = nct6795 },
	{ .name = "NCT6796", .driver_data = nct6796 },
	{ .name = "NCT6102", .driver_data = nct6102 },
	{ .name = "NCT6116", .driver_data = nct6116 },
	{ .name = "NCT6126", .driver_data = nct6126 },
	{},
};

static struct platform_driver wdt_driver = {
	.probe          = wdt_probe,
	.id_table       = wdt_ids,
	.driver         = {
		.name   = KBUILD_MODNAME,
	},
};

static struct platform_device *wdt_pdev;

static int __init wdt_init(void)
{
	struct resource res;
	int sioaddr;
	int ret;
	int chip;

	pdata.siocfg_enter = 0x87;
	pdata.siocfg_leave = 0xAA;

	/* Apply system-specific quirks */
	dmi_check_system(wdt_dmi_table);

	sioaddr = SIO_REG_CONF_ADDR0;
	chip = wdt_find(sioaddr, pdata.siocfg_enter, pdata.siocfg_leave);
	if (chip < 0) {
		sioaddr = SIO_REG_CONF_ADDR1;
		chip = wdt_find(sioaddr, pdata.siocfg_enter, pdata.siocfg_leave);
		if (chip < 0)
			return chip;
	}

	ret = platform_driver_register(&wdt_driver);
	if (ret)
		return ret;

	res.name = "Super I/O port";
	res.flags = IORESOURCE_IO;
	res.start = sioaddr;
	res.end = sioaddr + 1;

	wdt_pdev = platform_device_register_resndata(NULL, wdt_ids[chip].name,
						     PLATFORM_DEVID_NONE, &res,
						     1, &pdata, sizeof(pdata));
	if (IS_ERR(wdt_pdev)) {
		platform_driver_unregister(&wdt_driver);
		return PTR_ERR(wdt_pdev);
	}

	return 0;
}

static void __exit wdt_exit(void)
{
	platform_device_unregister(wdt_pdev);
	platform_driver_unregister(&wdt_driver);
}

module_init(wdt_init);
module_exit(wdt_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Pádraig  Brady <P@draigBrady.com>");
MODULE_DESCRIPTION("w83627hf/thf WDT driver");

// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * NCT6692D Watchdog Driver
 */

#define dev_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/acpi.h>
#include <linux/delay.h>
#include <linux/iommu.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/platform_device.h>
#include <linux/watchdog.h>

/* ISA constants */

#define IOREGION_OFFSET		4       /* Use EC port 1 */
#define IOREGION_LENGTH		4

#define WATCHDOG_TIMEOUT	60 /* 60 sec default timeout */

/*The timeout range is 1-255 seconds*/
#define MIN_TIMEOUT		1
#define MAX_TIMEOUT		255

static int timeout; /* in seconds */
module_param(timeout, int, 0);
MODULE_PARM_DESC(timeout,
		 "Watchdog timeout in seconds. 1 <= timeout <= 255, default="
		 __MODULE_STRING(WATCHDOG_TIMEOUT) ".");

static bool nowayout = WATCHDOG_NOWAYOUT;
module_param(nowayout, bool, 0);
MODULE_PARM_DESC(nowayout,
		 "Watchdog cannot be stopped once started (default="
		 __MODULE_STRING(WATCHDOG_NOWAYOUT) ")");

#define WDT_EFER(X) (X)  /* Extended Function Enable Registers */
#define WDT_EFIR(X) (X)  /* Extended Function Index Register(same as EFER) */
#define WDT_EFDR(X) (WDT_EFIR(X) + 1) /* Extended Function Data Register */

#define DRVNAME				"nct6692"
#define NCT6692_ID			0x0110
#define CHIPID_MASK			0xFFF0

#define NCT6692_MAX_IO_RETRY_NUM	0x100

#define NCT6692_EC_NAME			"nct6692_ec"
#define NCT6692_HWM_CFG			0x180

#define NCT6692_SIO_UNLOCK_KEY		0x87
#define NCT6692_SIO_LOCK_KEY		0xAA
#define NCT6692_LD_NUM_SHM		0x0F
#define NCT6692_LD_WIN2_BASE_ADDR	0xF8

/* Host Interface WIN2 offset definition */
#define SHM_WIN_MOD_OFFSET		0x01
#define SHM_WIN_CMD_OFFSET		0x02
#define SHM_WIN_SEL_OFFSET		0x03
#define SHM_WIN_CTL_OFFSET		0x04
#define VAL_SHM_WIN_CTRL_WR		0x40
#define VAL_SHM_WIN_CTRL_RD		0x80
#define SHM_WIN_ID_OFFSET		0x08
#define SHM_WIN_DAT_OFFSET		0x10

struct nct6692_shm_t {
	u_char __iomem *base_addr;
	u_long base_phys;
};

/* REGs definitions */
struct nct6692_sio_data {
	u_long base_phys;
	int sioreg;
};

struct nct6692_reg_t {
	u_char mod;
	u_char cmd;
	u_char sel;
	u_int idx;
};

struct nct6692_data_t {
	struct nct6692_shm_t shm;
	struct nct6692_reg_t cfg;
	struct nct6692_reg_t cnt;
	struct watchdog_device wdt;
};

static inline void superio_outb(int ioreg, int reg, int val)
{
	outb(reg, WDT_EFER(ioreg));
	outb(val, WDT_EFDR(ioreg));
}

static inline int superio_inb(int ioreg, int reg)
{
	outb(reg, WDT_EFER(ioreg));
	return inb(WDT_EFDR(ioreg));
}

static inline int superio_enter(int key, int addr, const char *name)
{
	if (!request_muxed_region(addr, 2, name)) {
		pr_err("I/O address 0x%04x already in use\n", addr);
		return -EBUSY;
	}
	outb_p(key, WDT_EFER(addr)); /* Enter extended function mode */
	outb_p(key, WDT_EFER(addr)); /* Again according to manual */

	return 0;
}

static inline void superio_select(int ioreg, int ld)
{
	superio_outb(ioreg, 0x07, ld);
}

static inline void superio_exit(int key, int addr)
{
	outb_p(key, WDT_EFER(addr)); /* Leave extended function mode */
	release_region(addr, 2);
}

/*
 * The following several functions are used to access host interface according
 * to the definition of memory region, reg (as a base addr) and an index offset
 * It uses (shm.base_addr + shm.offset) format to locate the data area of the
 * host interface channel. Then access the address "reg.idx + idx_offset" that
 * are suitable for a loop accessing. Where the idx_offset is an extra offset
 * based on the definition of reg for accessing the address based on the reg.
 */

static int shm_get_ec_ready(const struct nct6692_shm_t *shm,
			    const struct nct6692_reg_t *reg)
{
	u_char pre_id, new_id;
	u_int count = 0;

	iowrite8(reg->mod, shm->base_addr + SHM_WIN_MOD_OFFSET);
	iowrite8(reg->cmd, shm->base_addr + SHM_WIN_CMD_OFFSET);
	iowrite8(reg->sel, shm->base_addr + SHM_WIN_SEL_OFFSET);
	pre_id = ioread8(shm->base_addr + SHM_WIN_ID_OFFSET);
	iowrite8(VAL_SHM_WIN_CTRL_RD, shm->base_addr + SHM_WIN_CTL_OFFSET);
	do {
		msleep(20);
		new_id = ioread8(shm->base_addr + SHM_WIN_ID_OFFSET);
		if (count == NCT6692_MAX_IO_RETRY_NUM)
			return -ETIMEDOUT;
		count++;
	} while (pre_id == new_id);

	return new_id;
}

static int read_shm_win(const struct nct6692_shm_t *shm,
			const struct nct6692_reg_t *reg,
			u_char idx_offset)
{
	int retval;

	if (shm_get_ec_ready(shm, reg) != -ETIMEDOUT) {
		retval = ioread8(shm->base_addr + SHM_WIN_DAT_OFFSET + reg->idx + idx_offset);
		return retval;
	}

	return -ETIMEDOUT;
}

static int write_shm_win(const struct nct6692_shm_t *shm,
			 const struct nct6692_reg_t *reg,
			 u_char idx_offset,
			 u_char val)
{
	int err = 0;

	err = shm_get_ec_ready(shm, reg);
	if (err != -ETIMEDOUT)
		iowrite8(val, shm->base_addr + SHM_WIN_DAT_OFFSET + reg->idx + idx_offset);

	iowrite8(VAL_SHM_WIN_CTRL_WR, shm->base_addr + SHM_WIN_CTL_OFFSET);
	err = shm_get_ec_ready(shm, reg);

	return err;
}

static int nct6692_wdt_enable(u_int timeout,
			      struct nct6692_data_t *data)
{
	int err = 0;

	if (timeout)
		err = write_shm_win(&data->shm, &data->cfg, 0, 0x02);

	err = write_shm_win(&data->shm, &data->cnt, 0, timeout);

	return err;
}

static int nct6692_wdt_set_time(struct watchdog_device *wdog, u_int timeout)
{
	struct nct6692_data_t *data = watchdog_get_drvdata(wdog);
	const struct nct6692_shm_t *shm = &data->shm;

	if (!request_mem_region(shm->base_phys, 256, NCT6692_EC_NAME)) {
		pr_err("nuv:request I/O channel busy (base_addr=%lX)\n",
		       shm->base_phys);
		return -EBUSY;
	}

	nct6692_wdt_enable(timeout, data);

	release_mem_region(shm->base_phys, 256);
	return 0;
}

static int nct6692_wdt_setup(struct watchdog_device *wdt)
{
	u_char timeout;
	struct nct6692_data_t *data = watchdog_get_drvdata(wdt);
	const struct nct6692_shm_t *shm = &data->shm;

	timeout = 0;

	if (!request_mem_region(shm->base_phys, 256, NCT6692_EC_NAME)) {
		pr_err("nuv:request I/O channel busy (base_addr=%lX)\n",
		       shm->base_phys);
		return -EBUSY;
	}

	timeout = read_shm_win(&data->shm, &data->cnt, 0);
	release_mem_region(shm->base_phys, 256);
	if (timeout) {
		dev_err(data->wdt.parent, "failed to initialize watchdog (timeout=%d)\n", timeout);
		return timeout;
	}
	return timeout;
}

static int nct6692_wdt_start(struct watchdog_device *wdog)
{
	nct6692_wdt_setup(wdog);
	return nct6692_wdt_set_time(wdog, wdog->timeout);
}

static int nct6692_wdt_stop(struct watchdog_device *wdog)
{
	return nct6692_wdt_set_time(wdog, 0);
}

static int nct6692_wdt_set_timeout(struct watchdog_device *wdog,
				   u_int timeout)
{
	wdog->timeout = timeout;
	return 0;
}

static u_int nct6692_wdt_get_timeleft(struct watchdog_device *wdog)
{
	struct nct6692_data_t *data = watchdog_get_drvdata(wdog);
	const struct nct6692_shm_t *shm = &data->shm;
	u_int timeleft;

	if (!request_mem_region(shm->base_phys, 256, NCT6692_EC_NAME)) {
		pr_err("nuv:request I/O channel busy (base_addr=%lX)\n",
		       shm->base_phys);
		return -EBUSY;
	}

	timeleft = read_shm_win(&data->shm, &data->cnt, 0);
	release_mem_region(shm->base_phys, 256);
	return timeleft;
}

static int nct6692_wdt_ping(struct watchdog_device *wdt)
{
	struct nct6692_data_t *data = watchdog_get_drvdata(wdt);
	const struct nct6692_shm_t *shm = &data->shm;
	int timeout;
	int err = 0;

	/*
	 * Note:
	 * NCT6692 does not support refreshing WDT_TIMER_REG register when
	 * the watchdog is active. Please disable watchdog before feeding
	 * the watchdog and enable it again.
	 */
	if (!request_mem_region(shm->base_phys, 256, NCT6692_EC_NAME)) {
		pr_err("nuv:request I/O channel busy (base_addr=%lX)\n",
		       shm->base_phys);
		return -EBUSY;
	}

	/* Disable soft watchdog timer */
	timeout = 0;
	nct6692_wdt_enable(timeout, data);

	/* feed watchdog */
	timeout = wdt->timeout;
	err = write_shm_win(&data->shm, &data->cnt, 0, timeout);

	/* Enable soft watchdog timer */
	nct6692_wdt_enable(timeout, data);
	release_mem_region(shm->base_phys, 256);
	return err;
}

static const struct watchdog_info nct6692_wdt_info = {
	.options	= WDIOF_SETTIMEOUT | WDIOF_KEEPALIVEPING |
			  WDIOF_MAGICCLOSE,
	.identity	= "nct6692 watchdog",
};

static const struct watchdog_ops nct6692_wdt_ops = {
	.owner		= THIS_MODULE,
	.start		= nct6692_wdt_start,
	.stop		= nct6692_wdt_stop,
	.ping		= nct6692_wdt_ping,
	.set_timeout	= nct6692_wdt_set_timeout,
	.get_timeleft	= nct6692_wdt_get_timeleft,
};

static int nct6692_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct nct6692_data_t *data = NULL;
	struct nct6692_sio_data *sio_data = dev->platform_data;
	struct resource *res;

	dev_dbg(&pdev->dev, "Probe NCT6692 called\n");
	res = platform_get_resource(pdev, IORESOURCE_IO, 0);

	data = devm_kzalloc(&pdev->dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	/* init value */
	data->shm.base_phys = sio_data->base_phys;
	data->shm.base_addr = ioremap_cache(data->shm.base_phys, 256);

	data->cfg.mod = 0x10;
	data->cfg.idx = 0x15;

	data->cnt.mod = 0x10;
	data->cnt.idx = 0x16;

	data->wdt.ops = &nct6692_wdt_ops;
	data->wdt.info = &nct6692_wdt_info;

	data->wdt.timeout = WATCHDOG_TIMEOUT; /* Set default timeout */
	data->wdt.min_timeout = MIN_TIMEOUT;
	data->wdt.max_timeout = MAX_TIMEOUT;
	data->wdt.parent = &pdev->dev;

	watchdog_init_timeout(&data->wdt, timeout, &pdev->dev);
	watchdog_set_drvdata(&data->wdt, data);

	watchdog_set_nowayout(&data->wdt, nowayout);
	watchdog_stop_on_reboot(&data->wdt);
	watchdog_stop_on_unregister(&data->wdt);

	return devm_watchdog_register_device(dev, &data->wdt);
}

static struct platform_driver nct6692_driver = {
	.driver = {
		.name = DRVNAME,
	},
	.probe	= nct6692_probe,
};

static int __init nct6692_find(int addr, u_long *base_phys)
{
	u16 val;
	int err = 0;
	u_long tmp_base_phys;

	err = superio_enter(NCT6692_SIO_UNLOCK_KEY, addr, DRVNAME);
	if (err)
		return err;

	val = superio_inb(addr, 0x20);
	val = ((val << 8) | (superio_inb(addr, 0x21))) & CHIPID_MASK;

	if (val != NCT6692_ID) {
		err = -ENODEV;
		goto fail;
	}

	superio_select(addr, NCT6692_LD_NUM_SHM);
	tmp_base_phys = (superio_inb(addr, NCT6692_LD_WIN2_BASE_ADDR) |
			 (superio_inb(addr, NCT6692_LD_WIN2_BASE_ADDR + 1) << 8) |
			 (superio_inb(addr, NCT6692_LD_WIN2_BASE_ADDR + 2) << 16) |
			 (superio_inb(addr, NCT6692_LD_WIN2_BASE_ADDR + 3) << 24)) &
			0xFFFFFFFF;

	if (tmp_base_phys == 0xFFFFFFFF || tmp_base_phys == 0) {
		err = -ENODEV;
		goto fail;
	}

fail:
	superio_exit(NCT6692_SIO_LOCK_KEY, addr);
	if (!err) {
		if (base_phys)
			*base_phys = tmp_base_phys;
	}
	return err;
}

static struct platform_device *pdev;

static int __init nct6692_init(void)
{
	struct nct6692_sio_data sio_data;
	int sioaddr[2] = { 0x2e, 0x4e };
	struct resource res;
	int err;
	int address;
	u_long base_phys;

	/*
	 * initialize sio_data->kind and sio_data->sioreg.
	 *
	 * when Super-I/O functions move to a separate file, the Super-I/O
	 * driver will probe 0x2e and 0x4e and auto-detect the presence of a
	 * nct6692 hardware monitor, and call probe()
	 */
	err = nct6692_find(sioaddr[0], &base_phys);
	if (err) {
		err = nct6692_find(sioaddr[1], &base_phys);
		if (err)
			return -ENODEV;
	}

	memset(&res, 0, sizeof(res));
	res.name = DRVNAME;
	res.start = address + IOREGION_OFFSET;
	res.end = address + IOREGION_OFFSET + IOREGION_LENGTH - 1;
	res.flags = IORESOURCE_IO;

	err = acpi_check_resource_conflict(&res);
	if (err)
		return err;

	platform_driver_register(&nct6692_driver);

	sio_data.base_phys = base_phys;

	pdev = platform_device_alloc(DRVNAME, base_phys);
	if (!pdev) {
		err = -ENOMEM;
		goto exit_device_unregister;
	}
	err = platform_device_add_data(pdev, &sio_data,
				       sizeof(struct nct6692_sio_data));
	if (err)
		goto exit_device_put;

	err = platform_device_add_resources(pdev, &res, 1);
	if (err)
		goto exit_device_put;

	dev_info(&pdev->dev, "NCT6692 device found\n");
	/* platform_device_add calls probe() */
	err = platform_device_add(pdev);
	if (err)
		goto exit_device_put;

	return 0;
exit_device_put:
	platform_device_put(pdev);
	platform_device_unregister(pdev);
exit_device_unregister:
	platform_driver_unregister(&nct6692_driver);
	return err;
}

static void __exit nct6692_exit(void)
{
	platform_device_unregister(pdev);
	platform_driver_unregister(&nct6692_driver);
}

MODULE_AUTHOR("David Ober <dober@lenovo.com>");
MODULE_DESCRIPTION("NCT6692D driver");
MODULE_LICENSE("GPL");

module_init(nct6692_init);
module_exit(nct6692_exit);

// SPDX-License-Identifier: GPL-2.0 OR BSD-2-Clause
/*
 * Copyright (C) 2020-2023 VMware, Inc., Palo Alto, CA., USA
 * Copyright (C) 2024-2025 Broadcom Ltd.
 *
 * PTP clock driver for VMware precision clock virtual device.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/acpi.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/ptp_clock_kernel.h>
#include <asm/hypervisor.h>
#include <asm/vmware.h>

#define VMWARE_CMD_PCLK(nr) ((nr << 16) | 97)
#define VMWARE_CMD_PCLK_GETTIME VMWARE_CMD_PCLK(0)
#define VMWARE_CMD_PCLK_SETTIME VMWARE_CMD_PCLK(1)
#define VMWARE_CMD_PCLK_ADJTIME VMWARE_CMD_PCLK(2)
#define VMWARE_CMD_PCLK_ADJFREQ VMWARE_CMD_PCLK(3)

static struct acpi_device *ptp_vmw_acpi_device;
static struct ptp_clock *ptp_vmw_clock;

/*
 * Helpers for reading and writing to precision clock device.
 */

static int ptp_vmw_pclk_read(int cmd, u64 *ns)
{
	u32 ret, nsec_hi, nsec_lo;

	ret = vmware_hypercall3(cmd, 0, &nsec_hi, &nsec_lo);
	if (ret == 0)
		*ns = ((u64)nsec_hi << 32) | nsec_lo;

	return ret != 0 ? -EIO : 0;
}

static int ptp_vmw_pclk_write(int cmd, u64 in)
{
	u32 ret, unused;

	ret = vmware_hypercall5(cmd, 0, 0, in >> 32, in & 0xffffffff,
				&unused);

	return ret != 0 ? -EIO : 0;
}

/*
 * PTP clock ops.
 */

static int ptp_vmw_adjtime(struct ptp_clock_info *info, s64 delta)
{
	return ptp_vmw_pclk_write(VMWARE_CMD_PCLK_ADJTIME, (u64)delta);
}

static int ptp_vmw_adjfine(struct ptp_clock_info *info, long delta)
{
	return ptp_vmw_pclk_write(VMWARE_CMD_PCLK_ADJFREQ, (u64)delta);
}

static int ptp_vmw_gettime(struct ptp_clock_info *info, struct timespec64 *ts)
{
	u64 ns;

	if (ptp_vmw_pclk_read(VMWARE_CMD_PCLK_GETTIME, &ns) != 0)
		return -EIO;
	*ts = ns_to_timespec64(ns);
	return 0;
}

static int ptp_vmw_settime(struct ptp_clock_info *info,
			  const struct timespec64 *ts)
{
	u64 ns = timespec64_to_ns(ts);

	return ptp_vmw_pclk_write(VMWARE_CMD_PCLK_SETTIME, ns);
}

static int ptp_vmw_enable(struct ptp_clock_info *info,
			 struct ptp_clock_request *request, int on)
{
	return -EOPNOTSUPP;
}

static struct ptp_clock_info ptp_vmw_clock_info = {
	.owner		= THIS_MODULE,
	.name		= "ptp_vmw",
	.max_adj	= 999999999,
	.adjtime	= ptp_vmw_adjtime,
	.adjfine	= ptp_vmw_adjfine,
	.gettime64	= ptp_vmw_gettime,
	.settime64	= ptp_vmw_settime,
	.enable		= ptp_vmw_enable,
};

static int ptp_vmw_clock_register(void)
{
	ptp_vmw_clock = ptp_clock_register(&ptp_vmw_clock_info, NULL);
	if (IS_ERR(ptp_vmw_clock)) {
		pr_err("ptp_vmw: Failed to register ptp clock\n");
		return PTR_ERR(ptp_vmw_clock);
	}
	pr_debug("ptp_vmw: ptp clock registered\n");
	return 0;
}

static void ptp_vmw_clock_unregister(void)
{
	ptp_clock_unregister(ptp_vmw_clock);
	ptp_vmw_clock = NULL;
	pr_debug("ptp_vmw: ptp clock unregistered\n");
}

/*
 * ACPI driver ops for VMware "precision clock" virtual device.
 */

static int ptp_vmw_acpi_add(struct acpi_device *device)
{
	int ret = ptp_vmw_clock_register();

	if (ret == 0)
		ptp_vmw_acpi_device = device;
	return ret;
}

static void ptp_vmw_acpi_remove(struct acpi_device *device)
{
	ptp_vmw_clock_unregister();
	ptp_vmw_acpi_device = NULL;
}

static const struct acpi_device_id ptp_vmw_acpi_device_ids[] = {
	{ "VMW0005", 0 },
	{ "", 0 },
};

MODULE_DEVICE_TABLE(acpi, ptp_vmw_acpi_device_ids);

static struct acpi_driver ptp_vmw_acpi_driver = {
	.name = "ptp_vmw",
	.ids = ptp_vmw_acpi_device_ids,
	.ops = {
		.add = ptp_vmw_acpi_add,
		.remove	= ptp_vmw_acpi_remove
	},
};

/*
 * Probe existence of device by poking at a command. If successful,
 * register as a PTP clock. This is a fallback option for when ACPI
 * is not available.
 */
static int ptp_vmw_probe(void)
{
	u64 ns;

	return ptp_vmw_pclk_read(VMWARE_CMD_PCLK_GETTIME, &ns);
}

static int __init ptp_vmw_init(void)
{

	int error = -ENODEV;

	if (x86_hyper_type != X86_HYPER_VMWARE) {
		error = -EINVAL;
		goto out;
	}

	if (!acpi_disabled) {
		error = acpi_bus_register_driver(&ptp_vmw_acpi_driver);
		if (!error)
			goto out;
	}

	if (!ptp_vmw_probe())
		error = ptp_vmw_clock_register();

out:
	return error;
}

static void __exit ptp_vmw_exit(void)
{
	if (!acpi_disabled && ptp_vmw_acpi_device)
		acpi_bus_unregister_driver(&ptp_vmw_acpi_driver);
	else if (ptp_vmw_clock)
		ptp_vmw_clock_unregister();
}

module_init(ptp_vmw_init);
module_exit(ptp_vmw_exit);

MODULE_DESCRIPTION("VMware virtual PTP clock driver");
MODULE_AUTHOR("VMware, Inc.");
MODULE_LICENSE("Dual BSD/GPL");

// SPDX-License-Identifier: GPL-2.0+
/* Copyright 2025-2026 NXP */

#include <linux/bitfield.h>
#include <linux/elf.h>
#include <linux/firmware.h>
#include <linux/iopoll.h>

#include "neutron_device.h"

void neutron_enable_irq(struct neutron_device *ndev)
{
	u32 val;

	val = readl_relaxed(NEUTRON_REG(ndev, INTENA));
	val |= INTENA_INFDONE;
	writel_relaxed(val, NEUTRON_REG(ndev, INTENA));
}

void neutron_disable_irq(struct neutron_device *ndev)
{
	writel_relaxed(INTENA_INFDONE, NEUTRON_REG(ndev, INTCLR));
}

void neutron_handle_irq(struct neutron_device *ndev)
{
	u32 appstatus;

	appstatus = readl_relaxed(NEUTRON_REG(ndev, APPSTATUS));

	/* Write 1 to clear */
	writel_relaxed(appstatus & APPSTATUS_CLEAR_MASK, NEUTRON_REG(ndev, APPSTATUS));

	if (appstatus & APPSTATUS_FAULTCAUSE_MASK)
		dev_err(ndev->dev, "Neutron halted due to fault: 0x%lx\n",
			FIELD_GET(APPSTATUS_FAULTCAUSE_MASK, appstatus));
}

#define neutron_boot_done(appctrl) \
	(FIELD_GET(APPCTRL_MBWR_MASK, (appctrl)) == APPCTRL_MBWR_MAGIC)

static int neutron_start(struct neutron_device *ndev)
{
	u32 resetctrl, appctrl;
	int ret;

	resetctrl = readl_relaxed(NEUTRON_REG(ndev, RESETCTRL));
	writel_relaxed(resetctrl | RESETCTRL_ZVRUN, NEUTRON_REG(ndev, RESETCTRL));

	ret = readl_poll_timeout(NEUTRON_REG(ndev, APPCTRL),
				 appctrl, neutron_boot_done(appctrl),
				 100, 1000 * USEC_PER_MSEC);
	if (ret) {
		dev_err(ndev->dev, "Neutron boot timed out\n");
		return -ETIMEDOUT;
	}

	return 0;
}

static void neutron_stop(struct neutron_device *ndev)
{
	u32 resetctrl;

	resetctrl = readl_relaxed(NEUTRON_REG(ndev, RESETCTRL));
	writel_relaxed(resetctrl & ~RESETCTRL_ZVRUN, NEUTRON_REG(ndev, RESETCTRL));

	readl_poll_timeout(NEUTRON_REG(ndev, RESETCTRL),
			   resetctrl, !(resetctrl & RESETCTRL_ZVRUN),
			   100, 100 * USEC_PER_MSEC);
}

static void __iomem *neutron_tcm_da_to_va(struct neutron_device *ndev, u64 da)
{
	struct neutron_mem_region *mem;
	int offset, i;

	for (i = 0; i < NEUTRON_MEM_MAX; i++) {
		if (i != NEUTRON_MEM_ITCM && i != NEUTRON_MEM_DTCM)
			continue;
		mem = &ndev->mem_regions[i];
		if (da >= mem->da && da < mem->da + mem->size) {
			offset = da - mem->da;
			return mem->va + offset;
		}
	}

	return NULL;
}

static int neutron_load_firmware(struct neutron_device *ndev)
{
	const struct firmware *fw;
	struct elf32_hdr *ehdr;
	struct elf32_phdr *phdr, *seg;
	void __iomem *dest;
	int i, ret;

	ret = request_firmware(&fw, NEUTRON_FIRMWARE_NAME, ndev->dev);
	if (ret) {
		dev_err(ndev->dev, "Failed to request firmware\n");
		return ret;
	}

	ehdr = (struct elf32_hdr *)fw->data;
	if (memcmp(ehdr->e_ident, ELFMAG, SELFMAG) != 0) {
		dev_err(ndev->dev, "Invalid firmware image\n");
		ret = -EINVAL;
		goto out_release_fw;
	}

	phdr = (struct elf32_phdr *)(fw->data + ehdr->e_phoff);
	for (i = 0; i < ehdr->e_phnum; i++) {
		seg = &phdr[i];
		if (seg->p_type != PT_LOAD || !seg->p_memsz)
			continue;

		dest = neutron_tcm_da_to_va(ndev, seg->p_paddr);
		if (!dest) {
			dev_err(ndev->dev, "Invalid firmware segment: 0x%x\n", seg->p_paddr);
			ret = -EINVAL;
			goto out_release_fw;
		}

		memcpy_toio(dest, fw->data + seg->p_offset, seg->p_filesz);
		if (seg->p_memsz > seg->p_filesz)
			memset_io(dest + seg->p_filesz, 0, seg->p_memsz - seg->p_filesz);
	}

out_release_fw:
	release_firmware(fw);

	return ret;
}

int neutron_boot(struct neutron_device *ndev)
{
	int ret;

	if (ndev->flags & NEUTRON_BOOTED)
		neutron_shutdown(ndev);

	ret = neutron_load_firmware(ndev);
	if (ret)
		return ret;

	ret = neutron_start(ndev);
	if (ret)
		return ret;

	ndev->flags |= NEUTRON_BOOTED;

	return 0;
}

void neutron_shutdown(struct neutron_device *ndev)
{
	neutron_stop(ndev);
	ndev->flags &= ~NEUTRON_BOOTED;
}

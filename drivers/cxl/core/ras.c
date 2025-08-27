// SPDX-License-Identifier: GPL-2.0-only
/* Copyright(c) 2025 AMD Corporation. All rights reserved. */

#include <linux/pci.h>
#include <linux/aer.h>
#include <cxl/event.h>
#include <cxlmem.h>
#include <cxlpci.h>
#include "trace.h"

static void cxl_cper_trace_corr_port_prot_err(struct pci_dev *pdev,
					      struct cxl_ras_capability_regs ras_cap)
{
	u32 status = ras_cap.cor_status & ~ras_cap.cor_mask;

	trace_cxl_port_aer_correctable_error(&pdev->dev, status);
}

static void cxl_cper_trace_uncorr_port_prot_err(struct pci_dev *pdev,
						struct cxl_ras_capability_regs ras_cap)
{
	u32 status = ras_cap.uncor_status & ~ras_cap.uncor_mask;
	u32 fe;

	if (hweight32(status) > 1)
		fe = BIT(FIELD_GET(CXL_RAS_CAP_CONTROL_FE_MASK,
				   ras_cap.cap_control));
	else
		fe = status;

	trace_cxl_port_aer_uncorrectable_error(&pdev->dev, status, fe,
					       ras_cap.header_log);
}

static void cxl_cper_trace_corr_prot_err(struct cxl_memdev *cxlmd,
					 struct cxl_ras_capability_regs ras_cap)
{
	u32 status = ras_cap.cor_status & ~ras_cap.cor_mask;

	trace_cxl_aer_correctable_error(cxlmd, status);
}

static void
cxl_cper_trace_uncorr_prot_err(struct cxl_memdev *cxlmd,
			       struct cxl_ras_capability_regs ras_cap)
{
	u32 status = ras_cap.uncor_status & ~ras_cap.uncor_mask;
	u32 fe;

	if (hweight32(status) > 1)
		fe = BIT(FIELD_GET(CXL_RAS_CAP_CONTROL_FE_MASK,
				   ras_cap.cap_control));
	else
		fe = status;

	trace_cxl_aer_uncorrectable_error(cxlmd, status, fe,
					  ras_cap.header_log);
}

static int match_memdev_by_parent(struct device *dev, const void *uport)
{
	if (is_cxl_memdev(dev) && dev->parent == uport)
		return 1;
	return 0;
}

static void cxl_cper_handle_prot_err(struct cxl_cper_prot_err_work_data *data)
{
	unsigned int devfn = PCI_DEVFN(data->prot_err.agent_addr.device,
				       data->prot_err.agent_addr.function);
	struct pci_dev *pdev __free(pci_dev_put) =
		pci_get_domain_bus_and_slot(data->prot_err.agent_addr.segment,
					    data->prot_err.agent_addr.bus,
					    devfn);
	struct cxl_memdev *cxlmd;
	int port_type;

	if (!pdev)
		return;

	port_type = pci_pcie_type(pdev);
	if (port_type == PCI_EXP_TYPE_ROOT_PORT ||
	    port_type == PCI_EXP_TYPE_DOWNSTREAM ||
	    port_type == PCI_EXP_TYPE_UPSTREAM) {
		if (data->severity == AER_CORRECTABLE)
			cxl_cper_trace_corr_port_prot_err(pdev, data->ras_cap);
		else
			cxl_cper_trace_uncorr_port_prot_err(pdev, data->ras_cap);

		return;
	}

	guard(device)(&pdev->dev);
	if (!pdev->dev.driver)
		return;

	struct device *mem_dev __free(put_device) = bus_find_device(
		&cxl_bus_type, NULL, pdev, match_memdev_by_parent);
	if (!mem_dev)
		return;

	cxlmd = to_cxl_memdev(mem_dev);
	if (data->severity == AER_CORRECTABLE)
		cxl_cper_trace_corr_prot_err(cxlmd, data->ras_cap);
	else
		cxl_cper_trace_uncorr_prot_err(cxlmd, data->ras_cap);
}

static void cxl_cper_prot_err_work_fn(struct work_struct *work)
{
	struct cxl_cper_prot_err_work_data wd;

	while (cxl_cper_prot_err_kfifo_get(&wd))
		cxl_cper_handle_prot_err(&wd);
}
static DECLARE_WORK(cxl_cper_prot_err_work, cxl_cper_prot_err_work_fn);

int cxl_ras_init(void)
{
	return cxl_cper_register_prot_err_work(&cxl_cper_prot_err_work);
}

void cxl_ras_exit(void)
{
	cxl_cper_unregister_prot_err_work(&cxl_cper_prot_err_work);
	cancel_work_sync(&cxl_cper_prot_err_work);
}

static bool cxl_handle_ras(struct device *dev, void __iomem *ras_base);
static void cxl_handle_cor_ras(struct device *dev, void __iomem *ras_base);

#ifdef CONFIG_CXL_RCH_RAS
static void cxl_dport_map_rch_aer(struct cxl_dport *dport)
{
	resource_size_t aer_phys;
	struct device *host;
	u16 aer_cap;

	aer_cap = cxl_rcrb_to_aer(dport->dport_dev, dport->rcrb.base);
	if (aer_cap) {
		host = dport->reg_map.host;
		aer_phys = aer_cap + dport->rcrb.base;
		dport->regs.dport_aer = devm_cxl_iomap_block(host, aer_phys,
							     sizeof(struct aer_capability_regs));
	}
}

static void cxl_disable_rch_root_ints(struct cxl_dport *dport)
{
	void __iomem *aer_base = dport->regs.dport_aer;
	u32 aer_cmd_mask, aer_cmd;

	if (!aer_base)
		return;

	/*
	 * Disable RCH root port command interrupts.
	 * CXL 3.0 12.2.1.1 - RCH Downstream Port-detected Errors
	 *
	 * This sequence may not be necessary. CXL spec states disabling
	 * the root cmd register's interrupts is required. But, PCI spec
	 * shows these are disabled by default on reset.
	 */
	aer_cmd_mask = (PCI_ERR_ROOT_CMD_COR_EN |
			PCI_ERR_ROOT_CMD_NONFATAL_EN |
			PCI_ERR_ROOT_CMD_FATAL_EN);
	aer_cmd = readl(aer_base + PCI_ERR_ROOT_COMMAND);
	aer_cmd &= ~aer_cmd_mask;
	writel(aer_cmd, aer_base + PCI_ERR_ROOT_COMMAND);
}

/*
 * Copy the AER capability registers using 32 bit read accesses.
 * This is necessary because RCRB AER capability is MMIO mapped. Clear the
 * status after copying.
 *
 * @aer_base: base address of AER capability block in RCRB
 * @aer_regs: destination for copying AER capability
 */
static bool cxl_rch_get_aer_info(void __iomem *aer_base,
				 struct aer_capability_regs *aer_regs)
{
	int read_cnt = sizeof(struct aer_capability_regs) / sizeof(u32);
	u32 *aer_regs_buf = (u32 *)aer_regs;
	int n;

	if (!aer_base)
		return false;

	/* Use readl() to guarantee 32-bit accesses */
	for (n = 0; n < read_cnt; n++)
		aer_regs_buf[n] = readl(aer_base + n * sizeof(u32));

	writel(aer_regs->uncor_status, aer_base + PCI_ERR_UNCOR_STATUS);
	writel(aer_regs->cor_status, aer_base + PCI_ERR_COR_STATUS);

	return true;
}

/* Get AER severity. Return false if there is no error. */
static bool cxl_rch_get_aer_severity(struct aer_capability_regs *aer_regs,
				     int *severity)
{
	if (aer_regs->uncor_status & ~aer_regs->uncor_mask) {
		if (aer_regs->uncor_status & PCI_ERR_ROOT_FATAL_RCV)
			*severity = AER_FATAL;
		else
			*severity = AER_NONFATAL;
		return true;
	}

	if (aer_regs->cor_status & ~aer_regs->cor_mask) {
		*severity = AER_CORRECTABLE;
		return true;
	}

	return false;
}

static void cxl_handle_rdport_errors(struct cxl_dev_state *cxlds)
{
	struct pci_dev *pdev = to_pci_dev(cxlds->dev);
	struct aer_capability_regs aer_regs;
	struct cxl_dport *dport;
	int severity;

	struct cxl_port *port __free(put_cxl_port) =
		cxl_pci_find_port(pdev, &dport);
	if (!port)
		return;

	if (!cxl_rch_get_aer_info(dport->regs.dport_aer, &aer_regs))
		return;

	if (!cxl_rch_get_aer_severity(&aer_regs, &severity))
		return;

	pci_print_aer(pdev, severity, &aer_regs);
	if (severity == AER_CORRECTABLE)
		cxl_handle_cor_ras(&cxlds->cxlmd->dev, dport->regs.ras);
	else
		cxl_handle_ras(&cxlds->cxlmd->dev, dport->regs.ras);
}
#else
static inline void cxl_dport_map_rch_aer(struct cxl_dport *dport) { }
static inline void cxl_disable_rch_root_ints(struct cxl_dport *dport) { }
static inline void cxl_handle_rdport_errors(struct cxl_dev_state *cxlds) { }
#endif

static void cxl_dport_map_ras(struct cxl_dport *dport)
{
	struct cxl_register_map *map = &dport->reg_map;
	struct device *dev = dport->dport_dev;

	if (!map->component_map.ras.valid)
		dev_dbg(dev, "RAS registers not found\n");
	else if (cxl_map_component_regs(map, &dport->regs.component,
					BIT(CXL_CM_CAP_CAP_ID_RAS)))
		dev_dbg(dev, "Failed to map RAS capability.\n");
}

/**
 * cxl_dport_init_ras_reporting - Setup CXL RAS report on this dport
 * @dport: the cxl_dport that needs to be initialized
 * @host: host device for devm operations
 */
void cxl_dport_init_ras_reporting(struct cxl_dport *dport, struct device *host)
{
	dport->reg_map.host = host;
	cxl_dport_map_ras(dport);

	if (dport->rch) {
		struct pci_host_bridge *host_bridge = to_pci_host_bridge(dport->dport_dev);

		if (!host_bridge->native_aer)
			return;

		cxl_dport_map_rch_aer(dport);
		cxl_disable_rch_root_ints(dport);
	}
}
EXPORT_SYMBOL_NS_GPL(cxl_dport_init_ras_reporting, "CXL");

static void cxl_handle_cor_ras(struct device *dev, void __iomem *ras_base)
{
	void __iomem *addr;
	u32 status;

	if (!ras_base) {
		dev_warn_once(dev, "CXL RAS register block is not mapped");
		return;
	}

	addr = ras_base + CXL_RAS_CORRECTABLE_STATUS_OFFSET;
	status = readl(addr);
	if (status & CXL_RAS_CORRECTABLE_STATUS_MASK) {
		writel(status & CXL_RAS_CORRECTABLE_STATUS_MASK, addr);
		trace_cxl_aer_correctable_error(to_cxl_memdev(dev), status);
	}
}

/* CXL spec rev3.0 8.2.4.16.1 */
static void header_log_copy(void __iomem *ras_base, u32 *log)
{
	void __iomem *addr;
	u32 *log_addr;
	int i, log_u32_size = CXL_HEADERLOG_SIZE / sizeof(u32);

	addr = ras_base + CXL_RAS_HEADER_LOG_OFFSET;
	log_addr = log;

	for (i = 0; i < log_u32_size; i++) {
		*log_addr = readl(addr);
		log_addr++;
		addr += sizeof(u32);
	}
}

/*
 * Log the state of the RAS status registers and prepare them to log the
 * next error status. Return 1 if reset needed.
 */
static bool cxl_handle_ras(struct device *dev, void __iomem *ras_base)
{
	u32 hl[CXL_HEADERLOG_SIZE_U32];
	void __iomem *addr;
	u32 status;
	u32 fe;

	if (!ras_base) {
		dev_warn_once(dev, "CXL RAS register block is not mapped");
		return false;
	}

	addr = ras_base + CXL_RAS_UNCORRECTABLE_STATUS_OFFSET;
	status = readl(addr);
	if (!(status & CXL_RAS_UNCORRECTABLE_STATUS_MASK))
		return false;

	/* If multiple errors, log header points to first error from ctrl reg */
	if (hweight32(status) > 1) {
		void __iomem *rcc_addr =
			ras_base + CXL_RAS_CAP_CONTROL_OFFSET;

		fe = BIT(FIELD_GET(CXL_RAS_CAP_CONTROL_FE_MASK,
				   readl(rcc_addr)));
	} else {
		fe = status;
	}

	header_log_copy(ras_base, hl);
	trace_cxl_aer_uncorrectable_error(to_cxl_memdev(dev), status, fe, hl);
	writel(status & CXL_RAS_UNCORRECTABLE_STATUS_MASK, addr);

	return true;
}

void cxl_cor_error_detected(struct pci_dev *pdev)
{
	struct cxl_dev_state *cxlds = pci_get_drvdata(pdev);
	struct device *dev = &cxlds->cxlmd->dev;

	scoped_guard(device, dev) {
		if (!dev->driver) {
			dev_warn(&pdev->dev,
				 "%s: memdev disabled, abort error handling\n",
				 dev_name(dev));
			return;
		}

		if (cxlds->rcd)
			cxl_handle_rdport_errors(cxlds);

		cxl_handle_cor_ras(&cxlds->cxlmd->dev, cxlds->regs.ras);
	}
}
EXPORT_SYMBOL_NS_GPL(cxl_cor_error_detected, "CXL");

pci_ers_result_t cxl_error_detected(struct pci_dev *pdev,
				    pci_channel_state_t state)
{
	struct cxl_dev_state *cxlds = pci_get_drvdata(pdev);
	struct cxl_memdev *cxlmd = cxlds->cxlmd;
	struct device *dev = &cxlmd->dev;
	bool ue;

	scoped_guard(device, dev) {
		if (!dev->driver) {
			dev_warn(&pdev->dev,
				 "%s: memdev disabled, abort error handling\n",
				 dev_name(dev));
			return PCI_ERS_RESULT_DISCONNECT;
		}

		if (cxlds->rcd)
			cxl_handle_rdport_errors(cxlds);
		/*
		 * A frozen channel indicates an impending reset which is fatal to
		 * CXL.mem operation, and will likely crash the system. On the off
		 * chance the situation is recoverable dump the status of the RAS
		 * capability registers and bounce the active state of the memdev.
		 */
		ue = cxl_handle_ras(&cxlds->cxlmd->dev, cxlds->regs.ras);
	}


	switch (state) {
	case pci_channel_io_normal:
		if (ue) {
			device_release_driver(dev);
			return PCI_ERS_RESULT_NEED_RESET;
		}
		return PCI_ERS_RESULT_CAN_RECOVER;
	case pci_channel_io_frozen:
		dev_warn(&pdev->dev,
			 "%s: frozen state error detected, disable CXL.mem\n",
			 dev_name(dev));
		device_release_driver(dev);
		return PCI_ERS_RESULT_NEED_RESET;
	case pci_channel_io_perm_failure:
		dev_warn(&pdev->dev,
			 "failure state error detected, request disconnect\n");
		return PCI_ERS_RESULT_DISCONNECT;
	}
	return PCI_ERS_RESULT_NEED_RESET;
}
EXPORT_SYMBOL_NS_GPL(cxl_error_detected, "CXL");

// SPDX-License-Identifier: GPL-2.0-only
/* Copyright(c) 2025 AMD Corporation. All rights reserved. */

#include <linux/pci.h>
#include <linux/aer.h>
#include <cxl/event.h>
#include <cxlmem.h>
#include <cxlpci.h>
#include <cxl.h>
#include "trace.h"

static void cxl_cper_trace_corr_port_prot_err(struct pci_dev *pdev,
					      struct cxl_ras_capability_regs ras_cap)
{
	u32 status = ras_cap.cor_status & ~ras_cap.cor_mask;

	trace_cxl_aer_correctable_error(&pdev->dev, status, 0);
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

	trace_cxl_aer_uncorrectable_error(&pdev->dev, status, fe,
					  ras_cap.header_log, 0);
}

static void cxl_cper_trace_corr_prot_err(struct cxl_memdev *cxlmd,
					 struct cxl_ras_capability_regs ras_cap)
{
	u32 status = ras_cap.cor_status & ~ras_cap.cor_mask;

	trace_cxl_aer_correctable_error(&cxlmd->dev, cxlmd->cxlds->serial,
					status);
}

static void
cxl_cper_trace_uncorr_prot_err(struct cxl_memdev *cxlmd,
			       struct cxl_ras_capability_regs ras_cap)
{
	u32 status = ras_cap.uncor_status & ~ras_cap.uncor_mask;
	struct cxl_dev_state *cxlds = cxlmd->cxlds;
	u32 fe;

	if (hweight32(status) > 1)
		fe = BIT(FIELD_GET(CXL_RAS_CAP_CONTROL_FE_MASK,
				   ras_cap.cap_control));
	else
		fe = status;

	trace_cxl_aer_uncorrectable_error(&cxlmd->dev, status, fe,
					  ras_cap.header_log,
					  cxlds->serial);
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

static pci_ers_result_t cxl_handle_ras(struct device *dev, u64 serial, void __iomem *ras_base);
static void cxl_handle_cor_ras(struct device *dev, u64 serial, void __iomem *ras_base);

static void cxl_unmask_proto_interrupts(struct device *dev)
{
	struct pci_dev *pdev __free(pci_dev_put) =
		pci_dev_get(to_pci_dev(dev));

	if (!pdev->aer_cap) {
		pdev->aer_cap = pci_find_ext_capability(pdev,
							PCI_EXT_CAP_ID_ERR);
		if (!pdev->aer_cap)
			return;
	}

	pci_aer_unmask_internal_errors(pdev);
}

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
		cxl_handle_cor_ras(&cxlds->cxlmd->dev, cxlds->serial, dport->regs.ras);
	else
		cxl_handle_ras(&cxlds->cxlmd->dev, cxlds->serial, dport->regs.ras);
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

static int match_uport(struct device *dev, const void *data)
{
	const struct device *uport_dev = data;
	struct cxl_port *port;

	if (!is_cxl_port(dev))
		return 0;

	port = to_cxl_port(dev);

	return port->uport_dev == uport_dev;
}

static void __iomem *cxl_get_ras_base(struct device *dev)
{
	struct pci_dev *pdev = to_pci_dev(dev);

	switch (pci_pcie_type(pdev)) {
	case PCI_EXP_TYPE_ROOT_PORT:
	case PCI_EXP_TYPE_DOWNSTREAM:
	{
		struct cxl_dport *dport = NULL;
		struct cxl_port *port __free(put_cxl_port) = find_cxl_port(&pdev->dev, &dport);

		if (!dport || !dport->dport_dev) {
			pci_err(pdev, "Failed to find the CXL device");
			return NULL;
		}

		if (!dport)
			return NULL;

		return dport->regs.ras;
	}
	case PCI_EXP_TYPE_UPSTREAM:
	{
		struct cxl_port *port;
		struct device *port_dev __free(put_device) =
			bus_find_device(&cxl_bus_type, NULL,
					&pdev->dev, match_uport);

		if (!port_dev || !is_cxl_port(port_dev)) {
			pci_err(pdev, "Failed to find the CXL device");
			return NULL;
		}

		port = to_cxl_port(port_dev);
		if (!port)
			return NULL;

		return port->uport_regs.ras;
	}
	}

	dev_warn_once(dev, "Error: Unsupported device type (%X)", pci_pcie_type(pdev));
	return NULL;
}

static struct device *pci_to_cxl_dev(struct pci_dev *pdev)
{
	switch (pci_pcie_type(pdev)) {
	case PCI_EXP_TYPE_ROOT_PORT:
	case PCI_EXP_TYPE_DOWNSTREAM:
	{
		struct cxl_dport *dport = NULL;
		struct cxl_port *port __free(put_cxl_port) =
			find_cxl_port(&pdev->dev, &dport);

		if (!dport) {
			pci_err(pdev, "Failed to find the CXL device");
			return NULL;
		}

		return dport->dport_dev;
	}
	case PCI_EXP_TYPE_UPSTREAM:
	{
		struct cxl_port *port;
		struct device *port_dev __free(put_device) =
			bus_find_device(&cxl_bus_type, NULL,
					&pdev->dev, match_uport);

		if (!port_dev || !is_cxl_port(port_dev)) {
			pci_err(pdev, "Failed to find the CXL device");
			return NULL;
		}

		port = to_cxl_port(port_dev);
		if (!port)
			return NULL;

		return port->uport_dev;
	}
	case PCI_EXP_TYPE_ENDPOINT:
	{
		struct cxl_dev_state *cxlds = pci_get_drvdata(pdev);

		return cxlds->dev;
	}
	}

	pci_warn_once(pdev, "Error: Unsupported device type (%X)", pci_pcie_type(pdev));
	return NULL;
}


/*
 * Return 'struct device *' responsible for freeing pdev's CXL resources.
 * Caller is responsible for reference count decrementing the return
 * 'struct device *'.
 *
 * dev: Find the host of this dev
 */
static struct device *get_cxl_host_dev(struct device *dev)
{
	struct pci_dev *pdev = to_pci_dev(dev);

	switch (pci_pcie_type(pdev)) {
	case PCI_EXP_TYPE_ROOT_PORT:
	case PCI_EXP_TYPE_DOWNSTREAM:
	{
		struct cxl_dport *dport = NULL;
		struct cxl_port *port = find_cxl_port(&pdev->dev, &dport);

		if (!port)
			return NULL;

		return &port->dev;
	}
	case PCI_EXP_TYPE_UPSTREAM:
	{
		struct device *port_dev = bus_find_device(&cxl_bus_type, NULL,
							  &pdev->dev, match_uport);

		if (!port_dev || !is_cxl_port(port_dev))
			return NULL;

		return port_dev;
	}
	/* Endpoint resources are managed by endpoint itself */
	case PCI_EXP_TYPE_ENDPOINT:
		return NULL;
	}

	dev_warn_once(dev, "Error: Unsupported device type (%X)", pci_pcie_type(pdev));
	return NULL;
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
		return;
	}

	cxl_unmask_proto_interrupts(dport->dport_dev);
}
EXPORT_SYMBOL_NS_GPL(cxl_dport_init_ras_reporting, "CXL");

static void cxl_uport_init_ras_reporting(struct cxl_port *port,
					 struct device *host)
{
	struct cxl_register_map *map = &port->reg_map;

	map->host = host;
	if (cxl_map_component_regs(map, &port->uport_regs,
				   BIT(CXL_CM_CAP_CAP_ID_RAS))) {
		dev_dbg(&port->dev, "Failed to map RAS capability\n");
		return;
	}

	cxl_unmask_proto_interrupts(port->uport_dev);
}

void cxl_switch_port_init_ras(struct cxl_port *port)
{
	struct cxl_dport *parent_dport = port->parent_dport;

	if (is_cxl_root(to_cxl_port(port->dev.parent)))
		return;

	/* May have parent DSP or RP */
	if (parent_dport && dev_is_pci(parent_dport->dport_dev)) {
		struct pci_dev *pdev = to_pci_dev(parent_dport->dport_dev);

		if ((pci_pcie_type(pdev) == PCI_EXP_TYPE_ROOT_PORT) ||
		    (pci_pcie_type(pdev) == PCI_EXP_TYPE_DOWNSTREAM))
			cxl_dport_init_ras_reporting(parent_dport, &port->dev);
	}

	cxl_uport_init_ras_reporting(port, &port->dev);
}
EXPORT_SYMBOL_NS_GPL(cxl_switch_port_init_ras, "CXL");

void cxl_endpoint_port_init_ras(struct cxl_port *ep)
{
	struct cxl_dport *parent_dport;
	struct cxl_memdev *cxlmd = to_cxl_memdev(ep->uport_dev);
	struct cxl_port *parent_port __free(put_cxl_port) =
		cxl_mem_find_port(cxlmd, &parent_dport);

	if (!parent_dport || !dev_is_pci(parent_dport->dport_dev)) {
		dev_err(&ep->dev, "CXL port topology not found\n");
		return;
	}

	cxl_dport_init_ras_reporting(parent_dport, cxlmd->cxlds->dev);

	cxl_unmask_proto_interrupts(cxlmd->cxlds->dev);
}
EXPORT_SYMBOL_NS_GPL(cxl_endpoint_port_init_ras, "CXL");

static int cxl_report_error_detected(struct device *dev, void *data)
{
	struct pci_dev *pdev = to_pci_dev(dev);
	pci_ers_result_t vote, *result = data;

	guard(device)(dev);

	if (pci_pcie_type(pdev) == PCI_EXP_TYPE_ENDPOINT)
		vote = cxl_error_detected(dev);
	else
		vote = cxl_port_error_detected(dev);

	vote = cxl_error_detected(dev);
	*result = pci_ers_merge_result(*result, vote);

	return 0;
}

static int match_port_by_parent_dport(struct device *dev, const void *dport_dev)
{
	struct cxl_port *port;

	if (!is_cxl_port(dev))
		return 0;

	port = to_cxl_port(dev);

	return port->parent_dport->dport_dev == dport_dev;
}

static void cxl_walk_port(struct device *port_dev,
			  int (*cb)(struct device *, void *),
			  void *userdata)
{
	struct cxl_dport *dport = NULL;
	struct cxl_port *port;
	unsigned long index;

	if (!port_dev)
		return;

	port = to_cxl_port(port_dev);
	if (port->uport_dev && dev_is_pci(port->uport_dev))
		cb(port->uport_dev, userdata);

	xa_for_each(&port->dports, index, dport)
	{
		struct device *child_port_dev __free(put_device) =
			bus_find_device(&cxl_bus_type, &port->dev, dport,
					match_port_by_parent_dport);

		cb(dport->dport_dev, userdata);

		cxl_walk_port(child_port_dev, cxl_report_error_detected, userdata);
	}

	if (is_cxl_endpoint(port))
		cb(port->uport_dev->parent, userdata);
}

static void cxl_do_recovery(struct device *dev)
{
	pci_ers_result_t status = PCI_ERS_RESULT_CAN_RECOVER;
	struct pci_dev *pdev = to_pci_dev(dev);
	struct cxl_dport *dport;
	struct cxl_port *port;

	if ((pci_pcie_type(pdev) == PCI_EXP_TYPE_ROOT_PORT) ||
	    (pci_pcie_type(pdev) == PCI_EXP_TYPE_DOWNSTREAM)) {
		port = find_cxl_port(&pdev->dev, &dport);
	} else	if (pci_pcie_type(pdev) == PCI_EXP_TYPE_UPSTREAM) {
		struct device *port_dev = bus_find_device(&cxl_bus_type, NULL,
							  &pdev->dev, match_uport);
		port = to_cxl_port(port_dev);
	}

	if (!port)
		return;

	cxl_walk_port(&port->dev, cxl_report_error_detected, &status);
	if (status == PCI_ERS_RESULT_PANIC)
		panic("CXL cachemem error.");

	/*
	 * If we have native control of AER, clear error status in the device
	 * that detected the error.  If the platform retained control of AER,
	 * it is responsible for clearing this status.  In that case, the
	 * signaling device may not even be visible to the OS.
	 */
	if (cxl_error_is_native(pdev)) {
		pcie_clear_device_status(pdev);
		pci_aer_clear_nonfatal_status(pdev);
		pci_aer_clear_fatal_status(pdev);
	}
	put_device(&port->dev);
}

static void cxl_handle_cor_ras(struct device *dev, u64 serial, void __iomem *ras_base)
{
	void __iomem *addr;
	u32 status;

	if (!ras_base) {
		dev_warn_once(dev, "CXL RAS register block is not mapped");
		return;
	}

	addr = ras_base + CXL_RAS_CORRECTABLE_STATUS_OFFSET;
	status = readl(addr);
	if (!(status & CXL_RAS_CORRECTABLE_STATUS_MASK))
		return;
	writel(status & CXL_RAS_CORRECTABLE_STATUS_MASK, addr);

	trace_cxl_aer_correctable_error(dev, status, serial);
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
static pci_ers_result_t cxl_handle_ras(struct device *dev, u64 serial, void __iomem *ras_base)
{
	u32 hl[CXL_HEADERLOG_SIZE_U32];
	void __iomem *addr;
	u32 status;
	u32 fe;

	if (!ras_base) {
		dev_warn_once(dev, "CXL RAS register block is not mapped");
		return PCI_ERS_RESULT_NONE;
	}

	addr = ras_base + CXL_RAS_UNCORRECTABLE_STATUS_OFFSET;
	status = readl(addr);
	if (!(status & CXL_RAS_UNCORRECTABLE_STATUS_MASK))
		return PCI_ERS_RESULT_NONE;

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
	trace_cxl_aer_uncorrectable_error(dev, status, fe, hl, serial);
	writel(status & CXL_RAS_UNCORRECTABLE_STATUS_MASK, addr);

	return PCI_ERS_RESULT_PANIC;
}

void cxl_port_cor_error_detected(struct device *dev)
{
	void __iomem *ras_base = cxl_get_ras_base(dev);

	cxl_handle_cor_ras(dev, 0, ras_base);
}
EXPORT_SYMBOL_NS_GPL(cxl_port_cor_error_detected, "CXL");

pci_ers_result_t cxl_port_error_detected(struct device *dev)
{
	void __iomem *ras_base = cxl_get_ras_base(dev);

	return cxl_handle_ras(dev, 0, ras_base);
}
EXPORT_SYMBOL_NS_GPL(cxl_port_error_detected, "CXL");

void cxl_cor_error_detected(struct device *dev)
{
	struct pci_dev *pdev = to_pci_dev(dev);
	struct cxl_dev_state *cxlds = pci_get_drvdata(pdev);
	struct device *cxlmd_dev = &cxlds->cxlmd->dev;

	guard(device)(cxlmd_dev);

	if (!cxlmd_dev->driver) {
		dev_warn(&pdev->dev, "%s: memdev disabled, abort error handling", dev_name(dev));
		return;
	}

	if (cxlds->rcd)
		cxl_handle_rdport_errors(cxlds);

	cxl_handle_cor_ras(&cxlds->cxlmd->dev, cxlds->serial, cxlds->regs.ras);
}
EXPORT_SYMBOL_NS_GPL(cxl_cor_error_detected, "CXL");

void pci_cor_error_detected(struct pci_dev *pdev)
{
	cxl_cor_error_detected(&pdev->dev);
}
EXPORT_SYMBOL_NS_GPL(pci_cor_error_detected, "CXL");

pci_ers_result_t cxl_error_detected(struct device *dev)
{
	struct pci_dev *pdev = to_pci_dev(dev);
	struct cxl_dev_state *cxlds = pci_get_drvdata(pdev);
	struct device *cxlmd_dev = &cxlds->cxlmd->dev;

	guard(device)(cxlmd_dev);

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
	return cxl_handle_ras(&cxlds->cxlmd->dev, cxlds->serial, cxlds->regs.ras);
}
EXPORT_SYMBOL_NS_GPL(cxl_error_detected, "CXL");

pci_ers_result_t pci_error_detected(struct pci_dev *pdev,
				    pci_channel_state_t error)
{
	pci_ers_result_t rc;

	rc = cxl_error_detected(&pdev->dev);
	if (rc == PCI_ERS_RESULT_PANIC)
		panic("CXL cachemem error.");

	return rc;
}
EXPORT_SYMBOL_NS_GPL(pci_error_detected, "CXL");

static void cxl_handle_proto_error(struct cxl_proto_err_work_data *err_info)
{
	struct pci_dev *pdev = err_info->pdev;
	struct device *dev = pci_to_cxl_dev(pdev);
	struct device *host_dev __free(put_device) = get_cxl_host_dev(&pdev->dev);

	if (err_info->severity == AER_CORRECTABLE) {
		int aer = pdev->aer_cap;

		if (aer)
			pci_clear_and_set_config_dword(pdev,
						       aer + PCI_ERR_COR_STATUS,
						       0, PCI_ERR_COR_INTERNAL);

		if (pci_pcie_type(pdev) == PCI_EXP_TYPE_ENDPOINT)
			cxl_error_detected(&pdev->dev);
		else
			cxl_port_cor_error_detected(dev);

		pcie_clear_device_status(pdev);
	} else {
		cxl_do_recovery(dev);
	}
}

static void cxl_proto_err_work_fn(struct work_struct *work)
{
	struct cxl_proto_err_work_data wd;

	while (cxl_proto_err_kfifo_get(&wd))
		cxl_handle_proto_error(&wd);
}

static struct work_struct cxl_proto_err_work;
static DECLARE_WORK(cxl_proto_err_work, cxl_proto_err_work_fn);

int cxl_ras_init(void)
{
	if (cxl_cper_register_prot_err_work(&cxl_cper_prot_err_work))
		pr_err("Failed to initialize CXL RAS CPER\n");

	cxl_register_proto_err_work(&cxl_proto_err_work);

	return 0;
}

void cxl_ras_exit(void)
{
	cxl_cper_unregister_prot_err_work(&cxl_cper_prot_err_work);
	cancel_work_sync(&cxl_cper_prot_err_work);

	cxl_unregister_proto_err_work();
	cancel_work_sync(&cxl_proto_err_work);
}

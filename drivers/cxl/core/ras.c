// SPDX-License-Identifier: GPL-2.0-only
/* Copyright(c) 2025 AMD Corporation. All rights reserved. */

#include <linux/pci.h>
#include <linux/aer.h>
#include <cxl/event.h>
#include <cxlmem.h>
#include <cxlpci.h>
#include "trace.h"

static void cxl_cper_trace_corr_prot_err(struct pci_dev *pdev, u64 serial,
					 struct cxl_ras_capability_regs *ras_cap)
{
	u32 status = ras_cap->cor_status & ~ras_cap->cor_mask;

	trace_cxl_aer_correctable_error(&pdev->dev, status, serial);
}

static void cxl_cper_trace_uncorr_prot_err(struct pci_dev *pdev, u64 serial,
					   struct cxl_ras_capability_regs *ras_cap)
{
	u32 status = ras_cap->uncor_status & ~ras_cap->uncor_mask;
	u32 fe;

	if (hweight32(status) > 1)
		fe = BIT(FIELD_GET(CXL_RAS_CAP_CONTROL_FE_MASK,
				   ras_cap->cap_control));
	else
		fe = status;

	trace_cxl_aer_uncorrectable_error(&pdev->dev, status, fe,
					  ras_cap->header_log, serial);
}

void cxl_cper_handle_prot_err(struct cxl_cper_prot_err_work_data *data)
{
	unsigned int devfn = PCI_DEVFN(data->prot_err.agent_addr.device,
				       data->prot_err.agent_addr.function);
	struct pci_dev *pdev __free(pci_dev_put) =
		pci_get_domain_bus_and_slot(data->prot_err.agent_addr.segment,
					    data->prot_err.agent_addr.bus,
					    devfn);

	if (!pdev)
		return;

	guard(device)(&pdev->dev);
	if (!pdev->dev.driver)
		return;

	if (data->severity == AER_CORRECTABLE)
		cxl_cper_trace_corr_prot_err(pdev, pci_get_dsn(pdev),
					     &data->ras_cap);
	else
		cxl_cper_trace_uncorr_prot_err(pdev, pci_get_dsn(pdev),
					       &data->ras_cap);
}
EXPORT_SYMBOL_GPL(cxl_cper_handle_prot_err);

static void cxl_cper_prot_err_work_fn(struct work_struct *work)
{
	struct cxl_cper_prot_err_work_data wd;

	while (cxl_cper_prot_err_kfifo_get(&wd))
		cxl_cper_handle_prot_err(&wd);
}
static DECLARE_WORK(cxl_cper_prot_err_work, cxl_cper_prot_err_work_fn);

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
 * devm_cxl_dport_ras_setup - Setup CXL RAS report on this dport
 * @dport: the cxl_dport that needs to be initialized
 */
void devm_cxl_dport_ras_setup(struct cxl_dport *dport)
{
	dport->reg_map.host = dport_to_host(dport);
	cxl_dport_map_ras(dport);
}

void devm_cxl_dport_rch_ras_setup(struct cxl_dport *dport)
{
	struct pci_host_bridge *host_bridge;

	if (!dev_is_pci(dport->dport_dev))
		return;

	devm_cxl_dport_ras_setup(dport);

	host_bridge = to_pci_host_bridge(dport->dport_dev);
	if (!host_bridge->native_aer)
		return;

	cxl_dport_map_rch_aer(dport);
	cxl_disable_rch_root_ints(dport);
}
EXPORT_SYMBOL_NS_GPL(devm_cxl_dport_rch_ras_setup, "CXL");

void devm_cxl_port_ras_setup(struct cxl_port *port)
{
	struct cxl_register_map *map = &port->reg_map;

	if (!map->component_map.ras.valid) {
		dev_dbg(&port->dev, "RAS registers not found\n");
		return;
	}

	map->host = &port->dev;
	if (cxl_map_component_regs(map, &port->regs,
				   BIT(CXL_CM_CAP_CAP_ID_RAS)))
		dev_dbg(&port->dev, "Failed to map RAS capability\n");
}
EXPORT_SYMBOL_NS_GPL(devm_cxl_port_ras_setup, "CXL");

/**
 * find_cxl_port_by_dev - Use @dev as hint to do a _by_dport or _by_uport lookup
 * @dev: generic device that may either be a companion of port or target dport
 * @dport: output parameter; set to the matched dport for dport-class
 * lookups (Root Port, Downstream Port), NULL otherwise.
 *
 * Return a 'struct cxl_port' with an elevated reference if found. Use
 * __free(put_cxl_port) to release.
 */
static struct cxl_port *find_cxl_port_by_dev(struct device *dev, struct cxl_dport **dport)
{
	struct pci_dev *pdev;

	*dport = NULL;
	if (!dev_is_pci(dev))
		return NULL;

	pdev = to_pci_dev(dev);

	switch (pci_pcie_type(pdev)) {
	case PCI_EXP_TYPE_ROOT_PORT:
	case PCI_EXP_TYPE_DOWNSTREAM:
		return find_cxl_port_by_dport(dev, dport);
	case PCI_EXP_TYPE_UPSTREAM:
	case PCI_EXP_TYPE_ENDPOINT:
	case PCI_EXP_TYPE_RC_END:
		return find_cxl_port_by_uport(dev);
	}

	return NULL;
}

static void __iomem *to_ras_base(struct cxl_port *port, struct cxl_dport *dport)
{
	if (!port)
		return NULL;

	if (dport)
		return dport->regs.ras;

	return port->regs.ras;
}

static void cxl_do_recovery(struct pci_dev *pdev, struct cxl_port *port, struct cxl_dport *dport)
{
	struct device *dev = &pdev->dev;
	bool ue;

	if (pci_dev_is_disconnected(pdev))
		panic("CXL cachemem error: device disconnected during UE recovery");

	ue = cxl_handle_ras(dev, pci_get_dsn(pdev),
			    to_ras_base(port, dport));
	if (ue)
		panic("CXL cachemem error.");

	pcie_clear_device_status(pdev);
	pci_aer_clear_nonfatal_status(pdev);
	pci_aer_clear_fatal_status(pdev);
}

void cxl_handle_cor_ras(struct device *dev, u64 serial, void __iomem *ras_base)
{
	void __iomem *addr;
	u32 status;

	if (!ras_base)
		return;

	addr = ras_base + CXL_RAS_CORRECTABLE_STATUS_OFFSET;
	status = readl(addr);
	if (status & CXL_RAS_CORRECTABLE_STATUS_MASK) {
		writel(status & CXL_RAS_CORRECTABLE_STATUS_MASK, addr);
		trace_cxl_aer_correctable_error(dev, status, serial);
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
bool cxl_handle_ras(struct device *dev, u64 serial, void __iomem *ras_base)
{
	u32 hl[CXL_HEADERLOG_SIZE_U32];
	void __iomem *addr;
	u32 status;
	u32 fe;

	if (!ras_base)
		return false;

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
	trace_cxl_aer_uncorrectable_error(dev, status, fe, hl, serial);
	writel(status & CXL_RAS_UNCORRECTABLE_STATUS_MASK, addr);

	return true;
}

void cxl_cor_error_detected(struct pci_dev *pdev)
{
	struct cxl_dev_state *cxlds = pci_get_drvdata(pdev);
	struct cxl_memdev *cxlmd = cxlds->cxlmd;
	struct device *dev = &cxlds->cxlmd->dev;

	scoped_guard(device, dev) {
		if (!dev->driver) {
			dev_warn(&pdev->dev,
				 "%s: memdev disabled, abort error handling\n",
				 dev_name(dev));
			return;
		}

		cxl_handle_cor_ras(&cxlds->cxlmd->dev, pci_get_dsn(pdev),
				   cxlmd->endpoint->regs.ras);
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

		/*
		 * A frozen channel indicates an impending reset which is fatal to
		 * CXL.mem operation, and will likely crash the system. On the off
		 * chance the situation is recoverable dump the status of the RAS
		 * capability registers and bounce the active state of the memdev.
		 */
		ue = cxl_handle_ras(&cxlds->cxlmd->dev, pci_get_dsn(pdev),
				    cxlmd->endpoint->regs.ras);
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

static void cxl_handle_proto_error(struct pci_dev *pdev, struct cxl_port *port,
				   struct cxl_dport *dport, int severity)
{
	/*
	 * An RC_END device is an RCD (Restricted CXL Device). Its AER
	 * interrupt is shared with the RCH Downstream Port, so handle RCH
	 * Downstream Port protocol errors first before processing the RCD's
	 * own errors. See CXL spec r3.1 s12.2.
	 */
	if (pci_pcie_type(pdev) == PCI_EXP_TYPE_RC_END)
		cxl_handle_rdport_errors(pdev);

	if (severity == AER_CORRECTABLE) {
		cxl_handle_cor_ras(&pdev->dev, pci_get_dsn(pdev),
				   to_ras_base(port, dport));
		pcie_clear_device_status(pdev);
	} else {
		cxl_do_recovery(pdev, port, dport);
	}
}

static int __cxl_proto_err_work_fn(struct cxl_proto_err_work_data *wd)
{
	struct cxl_dport *dport;
	struct cxl_port *port __free(put_cxl_port) =
		find_cxl_port_by_dev(&wd->pdev->dev, &dport);

	if (!port) {
		dev_err_ratelimited(&wd->pdev->dev,
				    "Failed to find parent port device in CXL topology\n");
		return 0;
	}

	/*
	 * Hold the port device lock and verify a driver is bound before
	 * handling errors. Protects against NULL deref if an error is
	 * dispatched before probe completion or after driver removal.
	 */
	guard(device)(&port->dev);
	if (!port->dev.driver) {
		dev_err_ratelimited(&port->dev,
				    "Port device is unbound, abort error handling\n");
		return 0;
	}

	cxl_handle_proto_error(wd->pdev, port, dport, wd->severity);

	return 0;
}

static void cxl_proto_err_work_fn(struct work_struct *work)
{
	struct cxl_proto_err_work_data wd;
	int rc;

	rc = for_each_cxl_proto_err(&wd, __cxl_proto_err_work_fn);
	if (rc)
		pr_err_ratelimited("Failed to handle the CXL error (%d)\n", rc);
}

static DECLARE_WORK(cxl_proto_err_work, cxl_proto_err_work_fn);

int cxl_ras_init(void)
{
	cxl_cper_register_prot_err_work(&cxl_cper_prot_err_work);
	cxl_register_proto_err_work(&cxl_proto_err_work);

	return 0;
}

void cxl_ras_exit(void)
{
	cxl_cper_unregister_prot_err_work();
	cxl_unregister_proto_err_work();
}

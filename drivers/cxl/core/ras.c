// SPDX-License-Identifier: GPL-2.0-only
/* Copyright(c) 2025 AMD Corporation. All rights reserved. */

#include <linux/pci.h>
#include <linux/aer.h>
#include <cxl/event.h>
#include <cxlmem.h>
#include <cxlpci.h>
#include "trace.h"

/* Check that UCE header definition is maintained to keep ABI intact  */
static_assert(CXL_HEADERLOG_TRACE_SIZE_U32 == 128,
	      "rasdaemon ABI requires exactly 128 u32s");

static void cxl_cper_trace_corr_port_prot_err(struct pci_dev *pdev,
					      struct cxl_ras_capability_regs ras_cap)
{
	u32 status = ras_cap.cor_status & ~ras_cap.cor_mask;

	trace_cxl_port_aer_correctable_error(&pdev->dev, status);
}

static void cxl_cper_trace_uncorr_port_prot_err(struct pci_dev *pdev,
						struct cxl_ras_capability_regs ras_cap)
{
	u32 hl[CXL_HEADERLOG_TRACE_SIZE_U32] = {};
	u32 status = ras_cap.uncor_status & ~ras_cap.uncor_mask;
	u32 fe;

	if (hweight32(status) > 1)
		fe = BIT(FIELD_GET(CXL_RAS_CAP_CONTROL_FE_MASK,
				   ras_cap.cap_control));
	else
		fe = status;

	memcpy(hl, ras_cap.header_log, CXL_HEADERLOG_SIZE);
	trace_cxl_port_aer_uncorrectable_error(&pdev->dev, status, fe, hl);
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
	u32 hl[CXL_HEADERLOG_TRACE_SIZE_U32] = {};
	u32 status = ras_cap.uncor_status & ~ras_cap.uncor_mask;
	u32 fe;

	if (hweight32(status) > 1)
		fe = BIT(FIELD_GET(CXL_RAS_CAP_CONTROL_FE_MASK,
				   ras_cap.cap_control));
	else
		fe = status;

	/*
	 * ras_cap.header_log[] holds CXL_HEADERLOG_SIZE_U32 (16) hardware
	 * dwords.  Copy them into the front of a zero-filled
	 * CXL_HEADERLOG_TRACE_SIZE_U32 (128) u32 staging buffer so the trace
	 * event memcpy sees a full 512-byte source and the userspace ABI
	 * (rasdaemon) is preserved.
	 */
	memcpy(hl, ras_cap.header_log, CXL_HEADERLOG_SIZE);
	trace_cxl_aer_uncorrectable_error(cxlmd, status, fe, hl);
}

static int match_memdev_by_parent(struct device *dev, const void *uport)
{
	if (is_cxl_memdev(dev) && dev->parent == uport)
		return 1;
	return 0;
}


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
	if (dport)
		*dport = NULL;
	if (!dev_is_pci(dev))
		return NULL;

	switch (pci_pcie_type(to_pci_dev(dev))) {
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

void cxl_cper_handle_prot_err(struct cxl_cper_prot_err_work_data *data)
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
	if (!pdev->dev.driver) {
		dev_warn_ratelimited(&pdev->dev,
				     "Device is unbound, abort CPER error handling\n");
		return;
	}

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


void __iomem *to_ras_base(struct cxl_port *port, struct cxl_dport *dport)
{
	if (!port)
		return NULL;

	if (dport)
		return dport->regs.ras;

	return port->regs.ras;
}

void cxl_do_recovery(struct pci_dev *pdev, struct cxl_port *port, struct cxl_dport *dport)
{
	void __iomem *ras_base = to_ras_base(port, dport);

	if (!ras_base) {
		dev_err(&pdev->dev,
			"CXL UCE signaled but RAS registers not mapped\n");
		return;
	}

	if (cxl_handle_ras(port, dport, ras_base))
		panic("CXL cachemem error");

	dev_dbg(&pdev->dev,
		"CXL UCE signaled but no CXL RAS status bits set\n");
}

void cxl_handle_cor_ras(struct cxl_port *port, struct cxl_dport *dport, void __iomem *ras_base)
{
	u32 status;
	void __iomem *addr;

	if (!ras_base)
		return;

	addr = ras_base + CXL_RAS_CORRECTABLE_STATUS_OFFSET;
	status = readl(addr);
	if (status & CXL_RAS_CORRECTABLE_STATUS_MASK) {
		writel(status & CXL_RAS_CORRECTABLE_STATUS_MASK, addr);
		if (is_cxl_endpoint(port))
			trace_cxl_aer_correctable_error(to_cxl_memdev(port->uport_dev), status);
		else if (dport)
			trace_cxl_port_aer_correctable_error(dport->dport_dev, status);
		else
			trace_cxl_port_aer_correctable_error(port->uport_dev, status);
	}
}

/* CXL spec rev3.0 8.2.4.16.1 */
static void header_log_copy(void __iomem *ras_base, u32 *log)
{
	void __iomem *addr;
	u32 *log_addr;
	int i;

	addr = ras_base + CXL_RAS_HEADER_LOG_OFFSET;
	log_addr = log;

	for (i = 0; i < CXL_HEADERLOG_SIZE_U32; i++) {
		*log_addr = readl(addr);
		log_addr++;
		addr += sizeof(u32);
	}
}

/*
 * Log the state of the RAS status registers and prepare them to log the
 * next error status. Return 1 if reset needed.
 */
bool cxl_handle_ras(struct cxl_port *port, struct cxl_dport *dport, void __iomem *ras_base)
{
	u32 hl[CXL_HEADERLOG_TRACE_SIZE_U32] = {};
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
	if (is_cxl_endpoint(port))
		trace_cxl_aer_uncorrectable_error(to_cxl_memdev(port->uport_dev), status, fe, hl);
	else if (dport)
		trace_cxl_port_aer_uncorrectable_error(dport->dport_dev, status, fe, hl);
	else
		trace_cxl_port_aer_uncorrectable_error(port->uport_dev, status, fe, hl);

	writel(status & CXL_RAS_UNCORRECTABLE_STATUS_MASK, addr);

	return true;
}

pci_ers_result_t cxl_pci_error_detected(struct pci_dev *pdev,
					pci_channel_state_t state)
{
	struct cxl_port *port __free(put_cxl_port) = find_cxl_port_by_uport(&pdev->dev);
	bool ue = false;

	if (!port)
		return PCI_ERS_RESULT_DISCONNECT;


	scoped_guard(device, &port->dev) {
		if (!port->dev.driver) {
			dev_warn(&pdev->dev,
				 "%s: port disabled, abort error handling\n",
				 dev_name(&port->dev));
			return PCI_ERS_RESULT_DISCONNECT;
		}

		/*
		 * The CXL RAS read is unconditional regardless of channel
		 * state.  Any uncorrectable error bit set in the CXL RAS
		 * status register triggers a panic because CXL.mem cache
		 * coherency is already lost; continuing risks silent data
		 * corruption across interleaved HDM regions.
		 *
		 * On a dead link readl() returns 0xFFFFFFFF which sets all
		 * UCE bits and also triggers the panic - this is intentional.
		 * If RAS registers are not mapped the read is skipped, the
		 * panic is not reached, and the frozen/perm_failure switch
		 * cases below handle AER recovery for devices without active
		 * CXL.mem traffic.
		 */
		ue = cxl_handle_ras(port, NULL, to_ras_base(port, NULL));
	}

	/*
	 * CXL.mem UCE means cache coherency is lost. Continuing risks
	 * silent data corruption across interleaved HDM regions.
	 */
	if (ue)
		panic("CXL cachemem error");

	switch (state) {
	case pci_channel_io_normal:
		return PCI_ERS_RESULT_CAN_RECOVER;
	case pci_channel_io_frozen:
		dev_warn(&pdev->dev,
			 "%s: frozen state error detected, disable CXL.mem\n",
			 dev_name(port->uport_dev));
		device_release_driver(port->uport_dev);
		return PCI_ERS_RESULT_NEED_RESET;
	case pci_channel_io_perm_failure:
		dev_warn(&pdev->dev,
			 "failure state error detected, request disconnect\n");
		return PCI_ERS_RESULT_DISCONNECT;
	}
	return PCI_ERS_RESULT_NEED_RESET;
}
EXPORT_SYMBOL_NS_GPL(cxl_pci_error_detected, "CXL");

static void cxl_handle_proto_error(struct pci_dev *pdev, struct cxl_port *port,
				   struct cxl_dport *dport, int severity)
{
	if (severity == AER_CORRECTABLE)
		cxl_handle_cor_ras(port, dport, to_ras_base(port, dport));
	else
		cxl_do_recovery(pdev, port, dport);
}

static void __cxl_proto_err_work_fn(struct cxl_proto_err_work_data *wd)
{
	/*
	 * For RC_END (RCD) devices, handle RCH Downstream Port errors
	 * first.  cxl_handle_rdport_errors() does its own port lookup
	 * and locking, keeping the Downstream Port lock separate from the
	 * Endpoint Port lock taken below.
	 */
	if (is_cxl_restricted(wd->pdev))
		cxl_handle_rdport_errors(wd->pdev);

	struct cxl_port *port __free(put_cxl_port) = find_cxl_port_by_dev(&wd->pdev->dev, NULL);
	if (!port) {
		dev_err_ratelimited(&wd->pdev->dev,
				    "Failed to find parent port device in CXL topology\n");
		return;
	}
	guard(device)(&port->dev);
	if (!port->dev.driver) {
		dev_err_ratelimited(&port->dev,
				    "Port device is unbound, abort error handling\n");
		return;
	}

	struct cxl_dport *dport = cxl_find_dport_by_dev(port, &wd->pdev->dev);
	if (!dport && (pci_pcie_type(wd->pdev) == PCI_EXP_TYPE_ROOT_PORT ||
		       pci_pcie_type(wd->pdev) == PCI_EXP_TYPE_DOWNSTREAM)) {
		dev_err_ratelimited(&wd->pdev->dev,
				    "Failed to find dport device in CXL topology\n");
		return;
	}

	cxl_handle_proto_error(wd->pdev, port, dport, wd->severity);
}

static void cxl_proto_err_work_fn(struct work_struct *work)
{
	struct cxl_proto_err_work_data wd;

	for_each_cxl_proto_err(&wd, __cxl_proto_err_work_fn);
}

static DECLARE_WORK(cxl_proto_err_work, cxl_proto_err_work_fn);

static void cxl_proto_err_do_flush(void)
{
	flush_work(&cxl_proto_err_work);
}

void cxl_ras_init(void)
{
	cxl_cper_register_prot_err_work(&cxl_cper_prot_err_work);
	cxl_register_proto_err_work(&cxl_proto_err_work,
				   cxl_proto_err_do_flush);
}

void cxl_ras_exit(void)
{
	cxl_unregister_proto_err_work();
	cxl_cper_unregister_prot_err_work();
}

// SPDX-License-Identifier: GPL-2.0-only
/* Copyright(c) 2026 Intel Corporation. */

#include <linux/device.h>
#include <linux/pci.h>
#include <cxl.h>
#include "core.h"

/*
 * The reset cleared the HDM Decoder registers of every CXL component below
 * @dport_pci, so restore them from the settings the driver holds and from
 * @hdm_state, the register fields the driver does not model, saved before the
 * reset. Takes cxl_rwsem.region for read, which cxl_port_recommit_decoders()
 * requires. The caller has already disabled the regions, so nothing reaches the
 * decoders being reprogrammed.
 */
void cxl_sbr_recommit_decoders(struct pci_dev *dport_pci,
			       struct xarray *hdm_state)
{
	struct cxl_dport *dport;
	int rc;

	struct cxl_port *port __free(put_cxl_port) =
		find_cxl_port(&dport_pci->dev, &dport);
	if (!port) {
		pci_dbg(dport_pci, "no CXL port owns this Downstream Port\n");
		return;
	}

	pci_dbg(dport_pci, "restoring HDM decode below %s\n", dev_name(&port->dev));

	guard(rwsem_read)(&cxl_rwsem.region);
	rc = cxl_port_recommit_decoders(port, hdm_state);
	if (rc)
		pci_warn(dport_pci, "HDM decode restore failed: %d\n", rc);
}

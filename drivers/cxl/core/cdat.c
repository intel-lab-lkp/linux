// SPDX-License-Identifier: GPL-2.0-only
/* Copyright(c) 2023 Intel Corporation. All rights reserved. */
#include <linux/acpi.h>
#include <linux/xarray.h>
#include <linux/fw_table.h>
#include <linux/node.h>
#include <linux/overflow.h>
#include "cxlpci.h"
#include "cxlmem.h"
#include "core.h"
#include "cxl.h"
#include "core.h"

struct dsmas_entry {
	struct range dpa_range;
	u8 handle;
	struct access_coordinate coord[ACCESS_COORDINATE_MAX];
	struct access_coordinate cdat_coord[ACCESS_COORDINATE_MAX];
	int entries;
	int qos_class;
};

static u32 cdat_normalize(u16 entry, u64 base, u8 type)
{
	u32 value;

	/*
	 * Check for invalid and overflow values
	 */
	if (entry == 0xffff || !entry)
		return 0;
	else if (base > (UINT_MAX / (entry)))
		return 0;

	/*
	 * CDAT fields follow the format of HMAT fields. See table 5 Device
	 * Scoped Latency and Bandwidth Information Structure in Coherent Device
	 * Attribute Table (CDAT) Specification v1.01.
	 */
	value = entry * base;
	switch (type) {
	case ACPI_HMAT_ACCESS_LATENCY:
	case ACPI_HMAT_READ_LATENCY:
	case ACPI_HMAT_WRITE_LATENCY:
		value = DIV_ROUND_UP(value, 1000);
		break;
	default:
		break;
	}
	return value;
}

static int cdat_dsmas_handler(union acpi_subtable_headers *header, void *arg,
			      const unsigned long end)
{
	struct acpi_cdat_header *hdr = &header->cdat;
	struct acpi_cdat_dsmas *dsmas;
	int size = sizeof(*hdr) + sizeof(*dsmas);
	struct xarray *dsmas_xa = arg;
	struct dsmas_entry *dent;
	u16 len;
	int rc;

	len = le16_to_cpu((__force __le16)hdr->length);
	if (len != size || (unsigned long)hdr + len > end) {
		pr_warn("Malformed DSMAS table length: (%u:%u)\n", size, len);
		return -EINVAL;
	}

	/* Skip common header */
	dsmas = (struct acpi_cdat_dsmas *)(hdr + 1);

	dent = kzalloc(sizeof(*dent), GFP_KERNEL);
	if (!dent)
		return -ENOMEM;

	dent->handle = dsmas->dsmad_handle;
	dent->dpa_range.start = le64_to_cpu((__force __le64)dsmas->dpa_base_address);
	dent->dpa_range.end = le64_to_cpu((__force __le64)dsmas->dpa_base_address) +
			      le64_to_cpu((__force __le64)dsmas->dpa_length) - 1;

	rc = xa_insert(dsmas_xa, dent->handle, dent, GFP_KERNEL);
	if (rc) {
		kfree(dent);
		return rc;
	}

	return 0;
}

static void __cxl_access_coordinate_set(struct access_coordinate *coord,
					int access, unsigned int val)
{
	switch (access) {
	case ACPI_HMAT_ACCESS_LATENCY:
		coord->read_latency = val;
		coord->write_latency = val;
		break;
	case ACPI_HMAT_READ_LATENCY:
		coord->read_latency = val;
		break;
	case ACPI_HMAT_WRITE_LATENCY:
		coord->write_latency = val;
		break;
	case ACPI_HMAT_ACCESS_BANDWIDTH:
		coord->read_bandwidth = val;
		coord->write_bandwidth = val;
		break;
	case ACPI_HMAT_READ_BANDWIDTH:
		coord->read_bandwidth = val;
		break;
	case ACPI_HMAT_WRITE_BANDWIDTH:
		coord->write_bandwidth = val;
		break;
	}
}

static void cxl_access_coordinate_set(struct access_coordinate *coord,
				      int access, unsigned int val)
{
	for (int i = 0; i < ACCESS_COORDINATE_MAX; i++)
		__cxl_access_coordinate_set(&coord[i], access, val);
}

static int cdat_dslbis_handler(union acpi_subtable_headers *header, void *arg,
			       const unsigned long end)
{
	struct acpi_cdat_header *hdr = &header->cdat;
	struct acpi_cdat_dslbis *dslbis;
	int size = sizeof(*hdr) + sizeof(*dslbis);
	struct xarray *dsmas_xa = arg;
	struct dsmas_entry *dent;
	__le64 le_base;
	__le16 le_val;
	u64 val;
	u16 len;

	len = le16_to_cpu((__force __le16)hdr->length);
	if (len != size || (unsigned long)hdr + len > end) {
		pr_warn("Malformed DSLBIS table length: (%u:%u)\n", size, len);
		return -EINVAL;
	}

	/* Skip common header */
	dslbis = (struct acpi_cdat_dslbis *)(hdr + 1);

	/* Skip unrecognized data type */
	if (dslbis->data_type > ACPI_HMAT_WRITE_BANDWIDTH)
		return 0;

	/* Not a memory type, skip */
	if ((dslbis->flags & ACPI_HMAT_MEMORY_HIERARCHY) != ACPI_HMAT_MEMORY)
		return 0;

	dent = xa_load(dsmas_xa, dslbis->handle);
	if (!dent) {
		pr_warn("No matching DSMAS entry for DSLBIS entry.\n");
		return 0;
	}

	le_base = (__force __le64)dslbis->entry_base_unit;
	le_val = (__force __le16)dslbis->entry[0];
	val = cdat_normalize(le16_to_cpu(le_val), le64_to_cpu(le_base),
			     dslbis->data_type);

	cxl_access_coordinate_set(dent->cdat_coord, dslbis->data_type, val);

	return 0;
}

static int cdat_table_parse_output(int rc)
{
	if (rc < 0)
		return rc;
	if (rc == 0)
		return -ENOENT;

	return 0;
}

static int cxl_cdat_endpoint_process(struct cxl_port *port,
				     struct xarray *dsmas_xa)
{
	int rc;

	rc = cdat_table_parse(ACPI_CDAT_TYPE_DSMAS, cdat_dsmas_handler,
			      dsmas_xa, port->cdat.table, port->cdat.length);
	rc = cdat_table_parse_output(rc);
	if (rc)
		return rc;

	rc = cdat_table_parse(ACPI_CDAT_TYPE_DSLBIS, cdat_dslbis_handler,
			      dsmas_xa, port->cdat.table, port->cdat.length);
	return cdat_table_parse_output(rc);
}

static int cxl_port_perf_data_calculate(struct cxl_port *port,
					struct xarray *dsmas_xa)
{
	struct access_coordinate ep_c[ACCESS_COORDINATE_MAX];
	struct dsmas_entry *dent;
	int valid_entries = 0;
	unsigned long index;
	int rc;

	rc = cxl_endpoint_get_perf_coordinates(port, ep_c);
	if (rc) {
		dev_dbg(&port->dev, "Failed to retrieve ep perf coordinates.\n");
		return rc;
	}

	struct cxl_root *cxl_root __free(put_cxl_root) = find_cxl_root(port);

	if (!cxl_root)
		return -ENODEV;

	if (!cxl_root->ops || !cxl_root->ops->qos_class)
		return -EOPNOTSUPP;

	xa_for_each(dsmas_xa, index, dent) {
		int qos_class;

		cxl_coordinates_combine(dent->coord, dent->cdat_coord, ep_c);
		dent->entries = 1;
		rc = cxl_root->ops->qos_class(cxl_root,
					      &dent->coord[ACCESS_COORDINATE_CPU],
					      1, &qos_class);
		if (rc != 1)
			continue;

		valid_entries++;
		dent->qos_class = qos_class;
	}

	if (!valid_entries)
		return -ENOENT;

	return 0;
}

static void update_perf_entry(struct device *dev, struct dsmas_entry *dent,
			      struct cxl_dpa_perf *dpa_perf)
{
	for (int i = 0; i < ACCESS_COORDINATE_MAX; i++) {
		dpa_perf->coord[i] = dent->coord[i];
		dpa_perf->cdat_coord[i] = dent->cdat_coord[i];
	}
	dpa_perf->dpa_range = dent->dpa_range;
	dpa_perf->qos_class = dent->qos_class;
	dev_dbg(dev,
		"DSMAS: dpa: %#llx qos: %d read_bw: %d write_bw %d read_lat: %d write_lat: %d\n",
		dent->dpa_range.start, dpa_perf->qos_class,
		dent->coord[ACCESS_COORDINATE_CPU].read_bandwidth,
		dent->coord[ACCESS_COORDINATE_CPU].write_bandwidth,
		dent->coord[ACCESS_COORDINATE_CPU].read_latency,
		dent->coord[ACCESS_COORDINATE_CPU].write_latency);
}

static void cxl_memdev_set_qos_class(struct cxl_dev_state *cxlds,
				     struct xarray *dsmas_xa)
{
	struct cxl_memdev_state *mds = to_cxl_memdev_state(cxlds);
	struct device *dev = cxlds->dev;
	struct range pmem_range = {
		.start = cxlds->pmem_res.start,
		.end = cxlds->pmem_res.end,
	};
	struct range ram_range = {
		.start = cxlds->ram_res.start,
		.end = cxlds->ram_res.end,
	};
	struct dsmas_entry *dent;
	unsigned long index;

	xa_for_each(dsmas_xa, index, dent) {
		if (resource_size(&cxlds->ram_res) &&
		    range_contains(&ram_range, &dent->dpa_range))
			update_perf_entry(dev, dent, &mds->ram_perf);
		else if (resource_size(&cxlds->pmem_res) &&
			 range_contains(&pmem_range, &dent->dpa_range))
			update_perf_entry(dev, dent, &mds->pmem_perf);
		else
			dev_dbg(dev, "no partition for dsmas dpa: %#llx\n",
				dent->dpa_range.start);
	}
}

static int match_cxlrd_qos_class(struct device *dev, void *data)
{
	int dev_qos_class = *(int *)data;
	struct cxl_root_decoder *cxlrd;

	if (!is_root_decoder(dev))
		return 0;

	cxlrd = to_cxl_root_decoder(dev);
	if (cxlrd->qos_class == CXL_QOS_CLASS_INVALID)
		return 0;

	if (cxlrd->qos_class == dev_qos_class)
		return 1;

	return 0;
}

static void reset_dpa_perf(struct cxl_dpa_perf *dpa_perf)
{
	*dpa_perf = (struct cxl_dpa_perf) {
		.qos_class = CXL_QOS_CLASS_INVALID,
	};
}

static bool cxl_qos_match(struct cxl_port *root_port,
			  struct cxl_dpa_perf *dpa_perf)
{
	if (dpa_perf->qos_class == CXL_QOS_CLASS_INVALID)
		return false;

	if (!device_for_each_child(&root_port->dev, &dpa_perf->qos_class,
				   match_cxlrd_qos_class))
		return false;

	return true;
}

static int match_cxlrd_hb(struct device *dev, void *data)
{
	struct device *host_bridge = data;
	struct cxl_switch_decoder *cxlsd;
	struct cxl_root_decoder *cxlrd;

	if (!is_root_decoder(dev))
		return 0;

	cxlrd = to_cxl_root_decoder(dev);
	cxlsd = &cxlrd->cxlsd;

	guard(rwsem_read)(&cxl_region_rwsem);
	for (int i = 0; i < cxlsd->nr_targets; i++) {
		if (host_bridge == cxlsd->target[i]->dport_dev)
			return 1;
	}

	return 0;
}

static int cxl_qos_class_verify(struct cxl_memdev *cxlmd)
{
	struct cxl_dev_state *cxlds = cxlmd->cxlds;
	struct cxl_memdev_state *mds = to_cxl_memdev_state(cxlds);
	struct cxl_port *root_port;
	int rc;

	struct cxl_root *cxl_root __free(put_cxl_root) =
		find_cxl_root(cxlmd->endpoint);

	if (!cxl_root)
		return -ENODEV;

	root_port = &cxl_root->port;

	/* Check that the QTG IDs are all sane between end device and root decoders */
	if (!cxl_qos_match(root_port, &mds->ram_perf))
		reset_dpa_perf(&mds->ram_perf);
	if (!cxl_qos_match(root_port, &mds->pmem_perf))
		reset_dpa_perf(&mds->pmem_perf);

	/* Check to make sure that the device's host bridge is under a root decoder */
	rc = device_for_each_child(&root_port->dev,
				   cxlmd->endpoint->host_bridge, match_cxlrd_hb);
	if (!rc) {
		reset_dpa_perf(&mds->ram_perf);
		reset_dpa_perf(&mds->pmem_perf);
	}

	return rc;
}

static void discard_dsmas(struct xarray *xa)
{
	unsigned long index;
	void *ent;

	xa_for_each(xa, index, ent) {
		xa_erase(xa, index);
		kfree(ent);
	}
	xa_destroy(xa);
}
DEFINE_FREE(dsmas, struct xarray *, if (_T) discard_dsmas(_T))

void cxl_endpoint_parse_cdat(struct cxl_port *port)
{
	struct cxl_memdev *cxlmd = to_cxl_memdev(port->uport_dev);
	struct cxl_dev_state *cxlds = cxlmd->cxlds;
	struct xarray __dsmas_xa;
	struct xarray *dsmas_xa __free(dsmas) = &__dsmas_xa;
	int rc;

	xa_init(&__dsmas_xa);
	if (!port->cdat.table)
		return;

	rc = cxl_cdat_endpoint_process(port, dsmas_xa);
	if (rc < 0) {
		dev_dbg(&port->dev, "Failed to parse CDAT: %d\n", rc);
		return;
	}

	rc = cxl_port_perf_data_calculate(port, dsmas_xa);
	if (rc) {
		dev_dbg(&port->dev, "Failed to do perf coord calculations.\n");
		return;
	}

	cxl_memdev_set_qos_class(cxlds, dsmas_xa);
	cxl_qos_class_verify(cxlmd);
	cxl_memdev_update_perf(cxlmd);
}
EXPORT_SYMBOL_NS_GPL(cxl_endpoint_parse_cdat, CXL);

static int cdat_sslbis_handler(union acpi_subtable_headers *header, void *arg,
			       const unsigned long end)
{
	struct acpi_cdat_sslbis_table {
		struct acpi_cdat_header header;
		struct acpi_cdat_sslbis sslbis_header;
		struct acpi_cdat_sslbe entries[];
	} *tbl = (struct acpi_cdat_sslbis_table *)header;
	int size = sizeof(header->cdat) + sizeof(tbl->sslbis_header);
	struct acpi_cdat_sslbis *sslbis;
	struct cxl_port *port = arg;
	struct device *dev = &port->dev;
	int remain, entries, i;
	u16 len;

	len = le16_to_cpu((__force __le16)header->cdat.length);
	remain = len - size;
	if (!remain || remain % sizeof(tbl->entries[0]) ||
	    (unsigned long)header + len > end) {
		dev_warn(dev, "Malformed SSLBIS table length: (%u)\n", len);
		return -EINVAL;
	}

	sslbis = &tbl->sslbis_header;
	/* Unrecognized data type, we can skip */
	if (sslbis->data_type > ACPI_HMAT_WRITE_BANDWIDTH)
		return 0;

	entries = remain / sizeof(tbl->entries[0]);
	if (struct_size(tbl, entries, entries) != len)
		return -EINVAL;

	for (i = 0; i < entries; i++) {
		u16 x = le16_to_cpu((__force __le16)tbl->entries[i].portx_id);
		u16 y = le16_to_cpu((__force __le16)tbl->entries[i].porty_id);
		__le64 le_base;
		__le16 le_val;
		struct cxl_dport *dport;
		unsigned long index;
		u16 dsp_id;
		u64 val;

		switch (x) {
		case ACPI_CDAT_SSLBIS_US_PORT:
			dsp_id = y;
			break;
		case ACPI_CDAT_SSLBIS_ANY_PORT:
			switch (y) {
			case ACPI_CDAT_SSLBIS_US_PORT:
				dsp_id = x;
				break;
			case ACPI_CDAT_SSLBIS_ANY_PORT:
				dsp_id = ACPI_CDAT_SSLBIS_ANY_PORT;
				break;
			default:
				dsp_id = y;
				break;
			}
			break;
		default:
			dsp_id = x;
			break;
		}

		le_base = (__force __le64)tbl->sslbis_header.entry_base_unit;
		le_val = (__force __le16)tbl->entries[i].latency_or_bandwidth;
		val = cdat_normalize(le16_to_cpu(le_val), le64_to_cpu(le_base),
				     sslbis->data_type);

		xa_for_each(&port->dports, index, dport) {
			if (dsp_id == ACPI_CDAT_SSLBIS_ANY_PORT ||
			    dsp_id == dport->port_id) {
				cxl_access_coordinate_set(dport->coord,
							  sslbis->data_type,
							  val);
			}
		}
	}

	return 0;
}

void cxl_switch_parse_cdat(struct cxl_port *port)
{
	int rc;

	if (!port->cdat.table)
		return;

	rc = cdat_table_parse(ACPI_CDAT_TYPE_SSLBIS, cdat_sslbis_handler,
			      port, port->cdat.table, port->cdat.length);
	rc = cdat_table_parse_output(rc);
	if (rc)
		dev_dbg(&port->dev, "Failed to parse SSLBIS: %d\n", rc);
}
EXPORT_SYMBOL_NS_GPL(cxl_switch_parse_cdat, CXL);

static void __cxl_coordinates_combine(struct access_coordinate *out,
				      struct access_coordinate *c1,
				      struct access_coordinate *c2)
{
		if (c1->write_bandwidth && c2->write_bandwidth)
			out->write_bandwidth = min(c1->write_bandwidth,
						   c2->write_bandwidth);
		out->write_latency = c1->write_latency + c2->write_latency;

		if (c1->read_bandwidth && c2->read_bandwidth)
			out->read_bandwidth = min(c1->read_bandwidth,
						  c2->read_bandwidth);
		out->read_latency = c1->read_latency + c2->read_latency;
}

/**
 * cxl_coordinates_combine - Combine the two input coordinates
 *
 * @out: Output coordinate of c1 and c2 combined
 * @c1: input coordinates
 * @c2: input coordinates
 */
void cxl_coordinates_combine(struct access_coordinate *out,
			     struct access_coordinate *c1,
			     struct access_coordinate *c2)
{
	for (int i = 0; i < ACCESS_COORDINATE_MAX; i++)
		__cxl_coordinates_combine(&out[i], &c1[i], &c2[i]);
}

MODULE_IMPORT_NS(CXL);

static void cxl_bandwidth_add(struct access_coordinate *coord,
			      struct access_coordinate *c1,
			      struct access_coordinate *c2)
{
	for (int i = 0; i < ACCESS_COORDINATE_MAX; i++) {
		coord[i].read_bandwidth = c1[i].read_bandwidth +
					  c2[i].read_bandwidth;
		coord[i].write_bandwidth = c1[i].write_bandwidth +
					   c2[i].write_bandwidth;
	}
}

struct cxl_perf_ctx {
	struct access_coordinate coord[ACCESS_COORDINATE_MAX];
	struct cxl_port *port;
	struct cxl_dport *dport;
	int active_rps;
};

static struct cxl_dpa_perf *cxl_memdev_get_dpa_perf(struct cxl_memdev_state *mds,
						    enum cxl_decoder_mode mode)
{
	switch (mode) {
	case CXL_DECODER_RAM:
		return &mds->ram_perf;
	case CXL_DECODER_PMEM:
		return &mds->pmem_perf;
	default:
		break;
	}

	return ERR_PTR(-EINVAL);
}

static bool dpa_perf_contains(struct cxl_dpa_perf *perf,
			      struct resource *dpa_res)
{
	struct range dpa = {
		.start = dpa_res->start,
		.end = dpa_res->end,
	};

	if (!range_contains(&perf->dpa_range, &dpa))
		return false;

	return true;
}

static int cxl_endpoint_gather_coordinates(struct cxl_region *cxlr,
					   struct cxl_endpoint_decoder *cxled,
					   struct xarray *usp_xa)
{
	struct cxl_port *endpoint = to_cxl_port(cxled->cxld.dev.parent);
	struct access_coordinate pci_coord[ACCESS_COORDINATE_MAX];
	struct access_coordinate sw_coord[ACCESS_COORDINATE_MAX];
	struct access_coordinate ep_coord[ACCESS_COORDINATE_MAX];
	struct cxl_memdev *cxlmd = cxled_to_memdev(cxled);
	struct cxl_dev_state *cxlds = cxlmd->cxlds;
	struct cxl_memdev_state *mds = to_cxl_memdev_state(cxlds);
	struct pci_dev *pdev = to_pci_dev(cxlds->dev);
	struct cxl_port *parent_port, *gp_port;
	struct cxl_perf_ctx *perf_ctx;
	struct cxl_dpa_perf *perf;
	bool gp_is_root;
	unsigned long index;
	void *ptr;
	int rc;

	if (cxlds->rcd)
		return -ENODEV;

	parent_port = to_cxl_port(endpoint->dev.parent);
	gp_port = to_cxl_port(parent_port->dev.parent);
	if (is_cxl_root(gp_port))
		gp_is_root = true;

	perf = cxl_memdev_get_dpa_perf(mds, cxlr->mode);
	if (IS_ERR(perf))
		return PTR_ERR(perf);

	if (!dpa_perf_contains(perf, cxled->dpa_res))
		return -EINVAL;

	/*
	 * The index for the xarray is the upstream port device of the upstream
	 * CXL switch.
	 */
	index = (unsigned long)parent_port->uport_dev;
	perf_ctx = xa_load(usp_xa, index);
	if (!perf_ctx) {
		struct cxl_perf_ctx *c __free(kfree) =
			kzalloc(sizeof(*perf_ctx), GFP_KERNEL);

		if (!c)
			return -ENOMEM;
		ptr = xa_store(usp_xa, index, c, GFP_KERNEL);
		if (xa_is_err(ptr))
			return xa_err(ptr);
		perf_ctx = no_free_ptr(c);
	}

	/* Direct upstream link from EP bandwidth */
	rc = cxl_pci_get_bandwidth(pdev, pci_coord);
	if (rc < 0)
		return rc;

	/*
	 * Min of upstream link bandwidth and Endpoint CDAT bandwidth from
	 * DSLBIS.
	 */
	cxl_coordinates_combine(ep_coord, pci_coord, perf->cdat_coord);

	/*
	 * If grandparent port is root, then there's no switch involved and
	 * the endpoint is connected to a root port.
	 */
	if (!gp_is_root) {
		/*
		 * Retrieve the switch SSLBIS for switch downstream port
		 * associated with the endpoint bandwidth.
		 */
		rc = cxl_port_get_switch_dport_bandwidth(endpoint, sw_coord);
		if (rc)
			return rc;

		/*
		 * Min of the earlier coordinates with the switch SSLBIS
		 * bandwidth
		 */
		cxl_coordinates_combine(ep_coord, ep_coord, sw_coord);
	}

	/*
	 * Aggregate the computed bandwidth with the current aggregated bandwidth
	 * of the endpoints with the same switch upstream port.
	 */
	cxl_bandwidth_add(perf_ctx->coord, perf_ctx->coord, ep_coord);
	perf_ctx->port = parent_port;
	perf_ctx->dport = parent_port->parent_dport;

	/* Count the active RPs */
	if (gp_is_root)
		perf_ctx->active_rps++;

	return 0;
}

static int count_rootport(struct device *dev, void *data)
{
	struct pci_dev *pdev;
	int *total = data;

	if (!dev_is_pci(dev))
		return 0;

	pdev = to_pci_dev(dev);
	if (pci_pcie_type(pdev) == PCI_EXP_TYPE_ROOT_PORT)
		(*total)++;

	return 0;
}

static struct xarray *cxl_switch_iterate_coordinates(struct xarray *input_xa,
						     bool *parent_is_root)
{
	struct xarray *res_xa __free(kfree) = kzalloc(sizeof(*res_xa), GFP_KERNEL);
	struct access_coordinate coords[ACCESS_COORDINATE_MAX];
	struct cxl_perf_ctx *ctx, *us_ctx;
	unsigned long index, us_index;
	void *ptr;
	int rc;

	if (!res_xa)
		return ERR_PTR(-ENOMEM);
	xa_init(res_xa);

	*parent_is_root = false;
	xa_for_each(input_xa, index, ctx) {
		struct cxl_port *parent_port, *port, *gp_port;
		struct device *dev = (struct device *)index;
		struct cxl_dport *dport;
		struct pci_dev *pdev;
		bool gp_is_root;

		gp_is_root = false;
		port = ctx->port;
		parent_port = to_cxl_port(port->dev.parent);
		if (is_cxl_root(parent_port)) {
			*parent_is_root = true;
		} else {
			gp_port = to_cxl_port(parent_port->dev.parent);
			gp_is_root = is_cxl_root(gp_port);
		}

		dport = port->parent_dport;

		/*
		 * Create an xarray entry with the key of the upstream
		 * port of the upstream switch.
		 */
		us_index = (unsigned long)parent_port->uport_dev;
		us_ctx = xa_load(res_xa, us_index);
		if (!us_ctx) {
			struct cxl_perf_ctx *n __free(kfree) =
				kzalloc(sizeof(*n), GFP_KERNEL);

			if (!n)
				return ERR_PTR(-ENOMEM);

			ptr = xa_store(res_xa, us_index, n, GFP_KERNEL);
			if (xa_is_err(ptr))
				return ERR_PTR(xa_err(ptr));
			us_ctx = no_free_ptr(n);
		}

		/* Count the active RPs */
		if (gp_is_root)
			us_ctx->active_rps++;

		us_ctx->port = parent_port;
		us_ctx->dport = parent_port->parent_dport;

		if (*parent_is_root) {
			int total_rps = 0;

			/* Figure out how many RPs are under the host bridge */
			device_for_each_child(dport->dport_dev, &total_rps,
					      count_rootport);

			if (!total_rps || total_rps < ctx->active_rps)
				return ERR_PTR(-EINVAL);

			/*
			 * Determine the actual upstream link bandwidth by the
			 * total RPs * active under the HB of the region.
			 */
			for (int i = 0; i < ACCESS_COORDINATE_MAX; i++) {
				us_ctx->coord[i].read_bandwidth =
					us_ctx->coord[i].read_bandwidth +
					min(dport->coord[i].read_bandwidth /
					    total_rps * ctx->active_rps,
					    ctx->coord[i].read_bandwidth);
				us_ctx->coord[i].write_bandwidth =
					us_ctx->coord[i].read_bandwidth +
					min(dport->coord[i].write_bandwidth /
					    total_rps * ctx->active_rps,
					    ctx->coord[i].write_bandwidth);
			}

			continue;
		}

		/* Below is the calculation for another switch upstream */

		/*
		 * If the device isn't an upstream PCIe port, there's something
		 * wrong with the topology.
		 */
		if (!dev_is_pci(dev))
			return ERR_PTR(-EINVAL);

		/* Retrieve the upstream link bandwidth */
		pdev = to_pci_dev(dev);
		rc = cxl_pci_get_bandwidth(pdev, coords);
		if (rc)
			return ERR_PTR(-ENXIO);

		/*
		 * Take the min of downstream bandwidth and the upstream link
		 * bandwidth.
		 */
		cxl_coordinates_combine(coords, coords, ctx->coord);
		/*
		 * Take the min of the calculated bandwdith and the upstream
		 * switch SSLBIS bandwidth.
		 */
		cxl_coordinates_combine(coords, coords, dport->coord);

		/*
		 * Aggregate the bandwidth based on the upstream switch.
		 */
		cxl_bandwidth_add(us_ctx->coord, us_ctx->coord, coords);
	}

	return_ptr(res_xa);
}

static void cxl_region_update_access_coordinate(struct cxl_region *cxlr,
						struct xarray *input_xa)
{
	struct access_coordinate coord[ACCESS_COORDINATE_MAX];
	struct cxl_perf_ctx *ctx;
	unsigned long index;

	memset(coord, 0, sizeof(coord));
	xa_for_each(input_xa, index, ctx)
		cxl_bandwidth_add(coord, coord, ctx->coord);

	for (int i = 0; i < ACCESS_COORDINATE_MAX; i++) {
		cxlr->coord[i].read_bandwidth = coord[i].read_bandwidth;
		cxlr->coord[i].write_bandwidth = coord[i].write_bandwidth;
	}
}

static void free_perf_xa(struct xarray *xa)
{
	struct cxl_perf_ctx *ctx;
	unsigned long index;

	if (!xa)
		return;

	xa_for_each(xa, index, ctx)
		kfree(ctx);
	xa_destroy(xa);
	kfree(xa);
}

/*
 * cxl_region_shared_upstream_perf_update - Recalculate the access coordinates
 * @cxl_region: the cxl region to recalculate
 *
 * For certain region construction with endpoints behind CXL switches,
 * there is the possibility of the total bandwdith for all the endpoints
 * behind a switch being less or more than the switch upstream link. The
 * algorithm assumes the configuration is a symmetric topology as that
 * maximizes performance.
 *
 * There can be multiple switches under a RP. There can be multiple RPs under
 * a HB.
 *
 * An example hierarchy:
 *
 *                 CFMWS 0
 *                   |
 *          _________|_________
 *         |                   |
 *     ACPI0017-0          ACPI0017-1
 *  GP0/HB0/ACPI0016-0   GP1/HB1/ACPI0016-1
 *     |          |        |           |
 *    RP0        RP1      RP2         RP3
 *     |          |        |           |
 *   SW 0       SW 1     SW 2        SW 3
 *   |   |      |   |    |   |       |   |
 *  EP0 EP1    EP2 EP3  EP4  EP5    EP6 EP7
 *
 * Computation for the example hierarchy:
 *
 * Min (GP0 to CPU BW / total RPs * active RPs,
 *      Min(SW 0 Upstream Link to RP0 BW,
 *          Min(SW0SSLBIS for SW0DSP0 (EP0), EP0 DSLBIS, EP0 Upstream Link) +
 *          Min(SW0SSLBIS for SW0DSP1 (EP1), EP1 DSLBIS, EP1 Upstream link)) +
 *      Min(SW 1 Upstream Link to RP1 BW,
 *          Min(SW1SSLBIS for SW1DSP0 (EP2), EP2 DSLBIS, EP2 Upstream Link) +
 *          Min(SW1SSLBIS for SW1DSP1 (EP3), EP3 DSLBIS, EP3 Upstream link))) +
 * Min (GP1 to CPU BW / total RPs * active RPs,
 *      Min(SW 2 Upstream Link to RP2 BW,
 *          Min(SW2SSLBIS for SW2DSP0 (EP4), EP4 DSLBIS, EP4 Upstream Link) +
 *          Min(SW2SSLBIS for SW2DSP1 (EP5), EP5 DSLBIS, EP5 Upstream link)) +
 *      Min(SW 3 Upstream Link to RP3 BW,
 *          Min(SW3SSLBIS for SW3DSP0 (EP6), EP6 DSLBIS, EP6 Upstream Link) +
 *          Min(SW3SSLBIS for SW3DSP1 (EP7), EP7 DSLBIS, EP7 Upstream link))))
 */
void cxl_region_shared_upstream_perf_update(struct cxl_region *cxlr)
{
	struct xarray *usp_xa, *iter_xa, *working_xa;
	bool is_root;
	int rc;

	lockdep_assert_held(&cxl_dpa_rwsem);

	usp_xa = kzalloc(sizeof(*usp_xa), GFP_KERNEL);
	if (!usp_xa)
		return;

	xa_init(usp_xa);

	/*
	 * Collect aggregated endpoint bandwidth and store the bandwidth in
	 * an xarray indexed by the upstream port of the switch or RP. The
	 * bandwidth is aggregated per switch. Each endpoint consists of the
	 * minimum of bandwidth from DSLBIS from the endpoint CDAT, the endpoint
	 * upstream link bandwidth, and the bandwidth from the SSLBIS of the
	 * switch CDAT for the switch upstream port to the downstream port that's
	 * associated with the endpoint. If the device is directly connected to
	 * a RP, then no SSLBIS is involved.
	 */
	for (int i = 0; i < cxlr->params.nr_targets; i++) {
		struct cxl_endpoint_decoder *cxled = cxlr->params.targets[i];

		rc = cxl_endpoint_gather_coordinates(cxlr, cxled, usp_xa);
		if (rc) {
			free_perf_xa(usp_xa);
			return;
		}
	}

	iter_xa = usp_xa;
	usp_xa = NULL;
	/*
	 * Iterate through the components in the xarray and aggregate any
	 * component that share the same upstream link from the switch.
	 * The iteration takes consideration of multi-level switch
	 * hierarchy.
	 *
	 * When cxl_switch_iterate_coordinates() detect the grandparent
	 * upstream is a root port, it updates the bandwidth in the
	 * xarray by taking the min of the provided bandwidth and
	 * the bandwidth from the generic port (divided by the total
	 * RPs and multiplied by the number of involved RPs). is_root
	 * is set if the parent port is the cxl root.
	 */
	do {
		working_xa = cxl_switch_iterate_coordinates(iter_xa, &is_root);
		if (IS_ERR(working_xa))
			goto out;
		free_perf_xa(iter_xa);
		iter_xa = working_xa;
	} while (!is_root);

	/*
	 * Aggregate the bandwidths in the xarray (for all the HBs) and update
	 * the region bandwidths with the newly calculated bandwidths.
	 */
	cxl_region_update_access_coordinate(cxlr, iter_xa);

out:
	free_perf_xa(iter_xa);
}

void cxl_region_perf_data_calculate(struct cxl_region *cxlr,
				    struct cxl_endpoint_decoder *cxled)
{
	struct cxl_memdev *cxlmd = cxled_to_memdev(cxled);
	struct cxl_memdev_state *mds = to_cxl_memdev_state(cxlmd->cxlds);
	struct cxl_dpa_perf *perf;

	perf = cxl_memdev_get_dpa_perf(mds, cxlr->mode);
	if (IS_ERR(perf))
		return;

	lockdep_assert_held(&cxl_dpa_rwsem);

	if (!dpa_perf_contains(perf, cxled->dpa_res))
		return;

	for (int i = 0; i < ACCESS_COORDINATE_MAX; i++) {
		/* Get total bandwidth and the worst latency for the cxl region */
		cxlr->coord[i].read_latency = max_t(unsigned int,
						    cxlr->coord[i].read_latency,
						    perf->coord[i].read_latency);
		cxlr->coord[i].write_latency = max_t(unsigned int,
						     cxlr->coord[i].write_latency,
						     perf->coord[i].write_latency);
		cxlr->coord[i].read_bandwidth += perf->coord[i].read_bandwidth;
		cxlr->coord[i].write_bandwidth += perf->coord[i].write_bandwidth;
	}
}

int cxl_update_hmat_access_coordinates(int nid, struct cxl_region *cxlr,
				       enum access_coordinate_class access)
{
	return hmat_update_target_coordinates(nid, &cxlr->coord[access], access);
}

bool cxl_need_node_perf_attrs_update(int nid)
{
	return !acpi_node_backed_by_real_pxm(nid);
}

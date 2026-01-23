// SPDX-License-Identifier: GPL-2.0-only
/* Copyright(c) 2020 Intel Corporation. */
#include <linux/device.h>
#include <linux/memregion.h>
#include <cxlmem.h>
#include <cxl.h>
#include "core.h"

/**
 * DOC: cxl pmem region
 *
 * The core CXL PMEM region infrastructure supports persistent memory
 * region creation using LIBNVDIMM subsystem. It has dependency on
 * LIBNVDIMM, pmem region needs to update the cxl region information
 * in the LSA.
 */

static void cxl_pmem_region_release(struct device *dev)
{
	struct cxl_pmem_region *cxlr_pmem = to_cxl_pmem_region(dev);
	int i;

	for (i = 0; i < cxlr_pmem->nr_mappings; i++) {
		struct cxl_memdev *cxlmd = cxlr_pmem->mapping[i].cxlmd;

		put_device(&cxlmd->dev);
	}

	kfree(cxlr_pmem);
}

static ssize_t region_label_update_store(struct device *dev,
					 struct device_attribute *attr,
					 const char *buf, size_t len)
{
	struct cxl_pmem_region *cxlr_pmem = to_cxl_pmem_region(dev);
	struct cxl_region *cxlr = cxlr_pmem->cxlr;
	ssize_t rc;
	bool update;

	rc = kstrtobool(buf, &update);
	if (rc)
		return rc;

	ACQUIRE(rwsem_write_kill, rwsem)(&cxl_rwsem.region);
	if ((rc = ACQUIRE_ERR(rwsem_write_kill, &rwsem)))
		return rc;

	/* Region not yet committed */
	if (update && cxlr && cxlr->params.state != CXL_CONFIG_COMMIT) {
		dev_dbg(dev, "region not committed, can't update into LSA\n");
		return -ENXIO;
	}

	if (!cxlr || !cxlr->cxlr_pmem || !cxlr->cxlr_pmem->nd_region)
		return 0;

	rc = nd_region_label_update(cxlr->cxlr_pmem->nd_region);
	if (rc)
		return rc;

	cxlr->params.state_region_label = CXL_REGION_LABEL_ACTIVE;

	return len;
}

static ssize_t region_label_update_show(struct device *dev,
					struct device_attribute *attr,
					char *buf)
{
	struct cxl_pmem_region *cxlr_pmem = to_cxl_pmem_region(dev);
	struct cxl_region *cxlr = cxlr_pmem->cxlr;
	struct cxl_region_params *p = &cxlr->params;
	ssize_t rc;

	ACQUIRE(rwsem_read_intr, rwsem)(&cxl_rwsem.region);
	if ((rc = ACQUIRE_ERR(rwsem_read_intr, &rwsem)))
		return rc;

	return sysfs_emit(buf, "%d\n", p->state_region_label);
}
static DEVICE_ATTR_RW(region_label_update);

static ssize_t region_label_delete_store(struct device *dev,
					 struct device_attribute *attr,
					 const char *buf, size_t len)
{
	struct cxl_pmem_region *cxlr_pmem = to_cxl_pmem_region(dev);
	struct cxl_region *cxlr = cxlr_pmem->cxlr;
	ssize_t rc;

	ACQUIRE(rwsem_write_kill, rwsem)(&cxl_rwsem.region);
	if ((rc = ACQUIRE_ERR(rwsem_write_kill, &rwsem)))
		return rc;

	if (!cxlr && !cxlr->cxlr_pmem && !cxlr->cxlr_pmem->nd_region)
		return 0;

	rc = nd_region_label_delete(cxlr->cxlr_pmem->nd_region);
	if (rc)
		return rc;

	cxlr->params.state_region_label = CXL_REGION_LABEL_INACTIVE;

	return len;
}
static DEVICE_ATTR_WO(region_label_delete);

static struct attribute *cxl_pmem_region_attrs[] = {
	&dev_attr_region_label_update.attr,
	&dev_attr_region_label_delete.attr,
	NULL
};

static struct attribute_group cxl_pmem_region_group = {
	.attrs = cxl_pmem_region_attrs,
};

static const struct attribute_group *cxl_pmem_region_attribute_groups[] = {
	&cxl_base_attribute_group,
	&cxl_pmem_region_group,
	NULL
};

const struct device_type cxl_pmem_region_type = {
	.name = "cxl_pmem_region",
	.release = cxl_pmem_region_release,
	.groups = cxl_pmem_region_attribute_groups,
};

bool is_cxl_pmem_region(struct device *dev)
{
	return dev->type == &cxl_pmem_region_type;
}
EXPORT_SYMBOL_NS_GPL(is_cxl_pmem_region, "CXL");

struct cxl_pmem_region *to_cxl_pmem_region(struct device *dev)
{
	if (dev_WARN_ONCE(dev, !is_cxl_pmem_region(dev),
			  "not a cxl_pmem_region device\n"))
		return NULL;
	return container_of(dev, struct cxl_pmem_region, dev);
}
EXPORT_SYMBOL_NS_GPL(to_cxl_pmem_region, "CXL");

static struct lock_class_key cxl_pmem_region_key;

static int cxl_pmem_region_alloc(struct cxl_region *cxlr)
{
	struct cxl_region_params *p = &cxlr->params;
	struct cxl_nvdimm_bridge *cxl_nvb;
	struct device *dev;
	int i;

	guard(rwsem_read)(&cxl_rwsem.region);
	if (p->state != CXL_CONFIG_COMMIT)
		return -ENXIO;

	struct cxl_pmem_region *cxlr_pmem __free(kfree) =
		kzalloc(struct_size(cxlr_pmem, mapping, p->nr_targets),
			GFP_KERNEL);
	if (!cxlr_pmem)
		return -ENOMEM;

	cxlr_pmem->hpa_range.start = p->res->start;
	cxlr_pmem->hpa_range.end = p->res->end;

	/* Snapshot the region configuration underneath the cxl_region_rwsem */
	cxlr_pmem->nr_mappings = p->nr_targets;
	for (i = 0; i < p->nr_targets; i++) {
		struct cxl_endpoint_decoder *cxled = p->targets[i];
		struct cxl_memdev *cxlmd = cxled_to_memdev(cxled);
		struct cxl_pmem_region_mapping *m = &cxlr_pmem->mapping[i];

		/*
		 * Regions never span CXL root devices, so by definition the
		 * bridge for one device is the same for all.
		 */
		if (i == 0) {
			cxl_nvb = cxl_find_nvdimm_bridge(cxlmd->endpoint);
			if (!cxl_nvb)
				return -ENODEV;
			cxlr->cxl_nvb = cxl_nvb;
		}
		m->cxlmd = cxlmd;
		get_device(&cxlmd->dev);
		m->start = cxled->dpa_res->start;
		m->size = resource_size(cxled->dpa_res);
		m->position = i;
	}

	dev = &cxlr_pmem->dev;
	device_initialize(dev);
	lockdep_set_class(&dev->mutex, &cxl_pmem_region_key);
	device_set_pm_not_required(dev);
	dev->parent = &cxlr->dev;
	dev->bus = &cxl_bus_type;
	dev->type = &cxl_pmem_region_type;
	cxlr_pmem->cxlr = cxlr;
	cxlr->cxlr_pmem = no_free_ptr(cxlr_pmem);

	return 0;
}

static void cxlr_pmem_unregister(void *_cxlr_pmem)
{
	struct cxl_pmem_region *cxlr_pmem = _cxlr_pmem;
	struct cxl_region *cxlr = cxlr_pmem->cxlr;
	struct cxl_nvdimm_bridge *cxl_nvb = cxlr->cxl_nvb;

	/*
	 * Either the bridge is in ->remove() context under the device_lock(),
	 * or cxlr_release_nvdimm() is cancelling the bridge's release action
	 * for @cxlr_pmem and doing it itself (while manually holding the bridge
	 * lock).
	 */
	device_lock_assert(&cxl_nvb->dev);
	cxlr->cxlr_pmem = NULL;
	cxlr_pmem->cxlr = NULL;
	device_unregister(&cxlr_pmem->dev);
}

static void cxlr_release_nvdimm(void *_cxlr)
{
	struct cxl_region *cxlr = _cxlr;
	struct cxl_nvdimm_bridge *cxl_nvb = cxlr->cxl_nvb;

	scoped_guard(device, &cxl_nvb->dev) {
		if (cxlr->cxlr_pmem)
			devm_release_action(&cxl_nvb->dev, cxlr_pmem_unregister,
					    cxlr->cxlr_pmem);
	}
	cxlr->cxl_nvb = NULL;
	put_device(&cxl_nvb->dev);
}

/**
 * devm_cxl_add_pmem_region() - add a cxl_region-to-nd_region bridge
 * @cxlr: parent CXL region for this pmem region bridge device
 *
 * Return: 0 on success negative error code on failure.
 */
int devm_cxl_add_pmem_region(struct cxl_region *cxlr)
{
	struct cxl_pmem_region *cxlr_pmem;
	struct cxl_nvdimm_bridge *cxl_nvb;
	struct device *dev;
	int rc;

	rc = cxl_pmem_region_alloc(cxlr);
	if (rc)
		return rc;
	cxlr_pmem = cxlr->cxlr_pmem;
	cxl_nvb = cxlr->cxl_nvb;

	dev = &cxlr_pmem->dev;
	rc = dev_set_name(dev, "pmem_region%d", cxlr->id);
	if (rc)
		goto err;

	rc = device_add(dev);
	if (rc)
		goto err;

	dev_dbg(&cxlr->dev, "%s: register %s\n", dev_name(dev->parent),
		dev_name(dev));

	scoped_guard(device, &cxl_nvb->dev) {
		if (cxl_nvb->dev.driver)
			rc = devm_add_action_or_reset(&cxl_nvb->dev,
						      cxlr_pmem_unregister,
						      cxlr_pmem);
		else
			rc = -ENXIO;
	}

	if (rc)
		goto err_bridge;

	/* @cxlr carries a reference on @cxl_nvb until cxlr_release_nvdimm */
	return devm_add_action_or_reset(&cxlr->dev, cxlr_release_nvdimm, cxlr);

err:
	put_device(dev);
err_bridge:
	put_device(&cxl_nvb->dev);
	cxlr->cxl_nvb = NULL;
	return rc;
}

static int match_root_decoder_by_dport(struct device *dev, const void *data)
{
	const struct cxl_port *ep_port = data;
	struct cxl_root_decoder *cxlrd;
	struct cxl_port *root_port;
	struct cxl_decoder *cxld;
	struct cxl_dport *dport;
	int i;

	if (!is_root_decoder(dev))
		return 0;

	cxld = to_cxl_decoder(dev);
	if (!(cxld->flags & CXL_DECODER_F_PMEM))
		return 0;

	cxlrd = to_cxl_root_decoder(dev);

	root_port = cxlrd_to_port(cxlrd);
	dport = cxl_find_dport_by_dev(root_port, ep_port->host_bridge);
	if (!dport)
		return 0;

	for (i = 0; i < cxlrd->cxlsd.nr_targets; i++)
		if (dport == cxlrd->cxlsd.target[i])
			break;

	if (i == cxlrd->cxlsd.nr_targets)
		return 0;

	return is_root_decoder(dev);
}

/**
 * cxl_find_root_decoder_by_port() - find a cxl root decoder on cxl bus
 * @port: any descendant port in CXL port topology
 * @cxled: endpoint decoder
 *
 * Caller of this function must call put_device() when done as a device ref
 * is taken via device_find_child()
 */
static struct cxl_root_decoder *
cxl_find_root_decoder_by_port(struct cxl_port *port,
			      struct cxl_endpoint_decoder *cxled)
{
	struct cxl_root *cxl_root __free(put_cxl_root) = find_cxl_root(port);
	struct cxl_port *ep_port = cxled_to_port(cxled);
	struct device *dev;

	if (!cxl_root)
		return NULL;

	dev = device_find_child(&cxl_root->port.dev, ep_port,
				match_root_decoder_by_dport);
	if (!dev)
		return NULL;

	return to_cxl_root_decoder(dev);
}

static int match_free_ep_decoder(struct device *dev, const void *data)
{
	if (!is_endpoint_decoder(dev))
		return 0;

	return is_free_decoder(dev);
}

/**
 * cxl_find_free_ep_decoder() - find a cxl endpoint decoder using cxl port
 * @port: any descendant port in CXL port topology
 *
 * Caller of this function must call put_device() when done as a device ref
 * is taken via device_find_child()
 */
static struct cxl_decoder *cxl_find_free_ep_decoder(struct cxl_port *port)
{
	struct device *dev;

	dev = device_find_child(&port->dev, NULL, match_free_ep_decoder);
	if (!dev)
		return NULL;

	return to_cxl_decoder(dev);
}

void create_pmem_region(struct nvdimm *nvdimm)
{
	struct cxl_pmem_region_params *params;
	struct cxl_endpoint_decoder *cxled;
	struct cxl_nvdimm *cxl_nvd;
	struct cxl_memdev *cxlmd;
	struct cxl_region *cxlr;

	if (!nvdimm_has_cxl_region(nvdimm))
		return;

	lockdep_assert_held(&cxl_rwsem.region);
	cxl_nvd = nvdimm_provider_data(nvdimm);
	params = nvdimm_get_cxl_region_param(nvdimm);
	cxlmd = cxl_nvd->cxlmd;

	/* TODO: Region creation support only for interleave way == 1 */
	if (!(params->nlabel == 1)) {
		dev_dbg(&cxlmd->dev,
				"Region Creation is not supported with iw > 1\n");
		return;
	}

	struct cxl_decoder *cxld __free(put_cxl_decoder) =
		cxl_find_free_ep_decoder(cxlmd->endpoint);
	if (!cxld) {
		dev_err(&cxlmd->dev, "CXL endpoint decoder not found\n");
		return;
	}

	cxled = to_cxl_endpoint_decoder(&cxld->dev);

	struct cxl_root_decoder *cxlrd __free(put_cxl_root_decoder) =
		cxl_find_root_decoder_by_port(cxlmd->endpoint, cxled);
	if (!cxlrd) {
		dev_err(&cxlmd->dev, "CXL root decoder not found\n");
		return;
	}

	cxlr = cxl_create_region(cxlrd, CXL_PARTMODE_PMEM,
				 atomic_read(&cxlrd->region_id),
				 params, cxled);
	if (IS_ERR(cxlr))
		dev_warn(&cxlmd->dev, "Region Creation failed\n");
}
EXPORT_SYMBOL_NS_GPL(create_pmem_region, "CXL");

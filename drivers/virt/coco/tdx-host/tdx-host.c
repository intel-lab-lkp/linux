// SPDX-License-Identifier: GPL-2.0
/*
 * TDX host user interface driver
 *
 * Copyright (C) 2025 Intel Corporation
 */

#include <linux/bitfield.h>
#include <linux/device/faux.h>
#include <linux/dmar.h>
#include <linux/module.h>
#include <linux/mod_devicetable.h>
#include <linux/pci.h>
#include <linux/pci-doe.h>
#include <linux/pci-tsm.h>
#include <linux/tsm.h>
#include <linux/vmalloc.h>

#include <asm/cpu_device_id.h>
#include <asm/tdx.h>
#include <asm/tdx_global_metadata.h>

static const struct x86_cpu_id tdx_host_ids[] = {
	X86_MATCH_FEATURE(X86_FEATURE_TDX_HOST_PLATFORM, NULL),
	{}
};
MODULE_DEVICE_TABLE(x86cpu, tdx_host_ids);

/*
 * The global pointer is for features which won't be affected by tdx_sysinfo
 * change after TDX Module update, e.g. TDX Connect, so could cache it. A
 * counterexample is the TDX Module version.
 */
static const struct tdx_sys_info *tdx_sysinfo;

#define TDISP_FUNC_ID		GENMASK(15, 0)
#define TDISP_FUNC_ID_SEGMENT		GENMASK(23, 16)
#define TDISP_FUNC_ID_SEG_VALID		BIT(24)

static inline u32 tdisp_func_id(struct pci_dev *pdev)
{
	u32 func_id;

	func_id = FIELD_PREP(TDISP_FUNC_ID_SEGMENT, pci_domain_nr(pdev->bus));
	if (func_id)
		func_id |= TDISP_FUNC_ID_SEG_VALID;
	func_id |= FIELD_PREP(TDISP_FUNC_ID,
			      PCI_DEVID(pdev->bus->number, pdev->devfn));

	return func_id;
}

struct spdm_config_info_t {
	u32 vmm_spdm_cap;
#define SPDM_CAP_HBEAT          BIT(13)
#define SPDM_CAP_KEY_UPD        BIT(14)
	u8 spdm_session_policy;
	u8 certificate_slot_mask;
	u8 raw_bitstream_requested;
} __packed;

struct tdx_tsm_link {
	struct pci_tsm_pf0 pci;
	u32 func_id;
	struct page *in_msg;
	struct page *out_msg;

	u64 spdm_id;
	struct page *spdm_conf;
	struct tdx_page_array *spdm_mt;
	unsigned int dev_info_size;
	void *dev_info_data;
};

static struct tdx_tsm_link *to_tdx_tsm_link(struct pci_tsm *tsm)
{
	return container_of(tsm, struct tdx_tsm_link, pci.base_tsm);
}

#define PCI_DOE_DATA_OBJECT_HEADER_1_OFFSET	0
#define PCI_DOE_DATA_OBJECT_HEADER_2_OFFSET	4
#define PCI_DOE_DATA_OBJECT_HEADER_SIZE		8
#define PCI_DOE_DATA_OBJECT_PAYLOAD_OFFSET	PCI_DOE_DATA_OBJECT_HEADER_SIZE

#define PCI_DOE_PROTOCOL_SECURE_SPDM		2

static int tdx_spdm_msg_exchange(struct tdx_tsm_link *tlink,
				 void *request, size_t request_sz,
				 void *response, size_t response_sz)
{
	struct pci_dev *pdev = tlink->pci.base_tsm.pdev;
	void *req_pl_addr, *resp_pl_addr;
	size_t req_pl_sz, resp_pl_sz;
	u32 data, len;
	u16 vendor;
	u8 type;
	int ret;

	/*
	 * pci_doe() accept DOE PAYLOAD only but request carries DOE HEADER so
	 * shift the buffers, skip DOE HEADER in request buffer, and fill DOE
	 * HEADER in response buffer manually.
	 */

	data = le32_to_cpu(*(__le32 *)(request + PCI_DOE_DATA_OBJECT_HEADER_1_OFFSET));
	vendor = FIELD_GET(PCI_DOE_DATA_OBJECT_HEADER_1_VID, data);
	type = FIELD_GET(PCI_DOE_DATA_OBJECT_HEADER_1_TYPE, data);

	data = le32_to_cpu(*(__le32 *)(request + PCI_DOE_DATA_OBJECT_HEADER_2_OFFSET));
	len = FIELD_GET(PCI_DOE_DATA_OBJECT_HEADER_2_LENGTH, data);

	req_pl_sz = len * sizeof(__le32) - PCI_DOE_DATA_OBJECT_HEADER_SIZE;
	resp_pl_sz = response_sz - PCI_DOE_DATA_OBJECT_HEADER_SIZE;
	req_pl_addr = request + PCI_DOE_DATA_OBJECT_HEADER_SIZE;
	resp_pl_addr = response + PCI_DOE_DATA_OBJECT_HEADER_SIZE;

	ret = pci_tsm_doe_transfer(pdev, type, req_pl_addr, req_pl_sz,
				   resp_pl_addr, resp_pl_sz);
	if (ret < 0) {
		pci_err(pdev, "spdm msg exchange fail %d\n", ret);
		return ret;
	}

	data = FIELD_PREP(PCI_DOE_DATA_OBJECT_HEADER_1_VID, vendor) |
	       FIELD_PREP(PCI_DOE_DATA_OBJECT_HEADER_1_TYPE, type);
	*(__le32 *)(response + PCI_DOE_DATA_OBJECT_HEADER_1_OFFSET) = cpu_to_le32(data);

	len = (ret + PCI_DOE_DATA_OBJECT_HEADER_SIZE) / sizeof(__le32);
	data = FIELD_PREP(PCI_DOE_DATA_OBJECT_HEADER_2_LENGTH, len);
	*(__le32 *)(response + PCI_DOE_DATA_OBJECT_HEADER_2_OFFSET) = cpu_to_le32(data);

	ret += PCI_DOE_DATA_OBJECT_HEADER_SIZE;

	pci_dbg(pdev, "%s complete: vendor 0x%x type 0x%x rsp_sz %d\n",
		__func__, vendor, type, ret);
	return ret;
}

static int tdx_spdm_session_keyupdate(struct tdx_tsm_link *tlink);

static int tdx_tsm_link_event_handler(struct tdx_tsm_link *tlink,
				      u64 tdx_ret, u64 out_msg_sz)
{
	int ret;

	if (tdx_ret == TDX_SUCCESS)
		return 0;

	if (tdx_ret == TDX_SPDM_REQUEST) {
		ret = tdx_spdm_msg_exchange(tlink,
					    page_address(tlink->out_msg),
					    out_msg_sz,
					    page_address(tlink->in_msg),
					    PAGE_SIZE);
		if (ret < 0)
			return ret;

		return -EAGAIN;
	}

	if (tdx_ret == TDX_SPDM_SESSION_KEY_REQUIRE_REFRESH) {
		/* keyupdate won't trigger this error again, no recursion risk */
		ret = tdx_spdm_session_keyupdate(tlink);
		if (ret)
			return ret;

		return -EAGAIN;
	}

	return -EFAULT;
}

/*
 * TDX Module extension introduced SEAMCALLs work like a request queue.
 * The caller is responsible for grabbing a queue slot before SEAMCALL,
 * otherwise will fail with TDX_OPERAND_BUSY. Currently the queue depth is 1.
 * So a mutex could work for simplicity.
 */
static DEFINE_MUTEX(tdx_ext_lock);

enum tdx_spdm_mng_op {
	TDX_SPDM_MNG_HEARTBEAT = 0,
	TDX_SPDM_MNG_KEY_UPDATE = 1,
	TDX_SPDM_MNG_RECOLLECT = 2,
};

static int tdx_spdm_session_mng(struct tdx_tsm_link *tlink,
				enum tdx_spdm_mng_op op)
{
	u64 r, out_msg_sz;
	int ret;

	guard(mutex)(&tdx_ext_lock);
	do {
		r = tdh_exec_spdm_mng(tlink->spdm_id, op, NULL, tlink->in_msg,
				      tlink->out_msg, NULL, &out_msg_sz);
		ret = tdx_tsm_link_event_handler(tlink, r, out_msg_sz);
	} while (ret == -EAGAIN);

	return ret;
}

static int tdx_spdm_session_keyupdate(struct tdx_tsm_link *tlink)
{
	return tdx_spdm_session_mng(tlink, TDX_SPDM_MNG_KEY_UPDATE);
}

static void *tdx_dup_array_data(struct tdx_page_array *array,
				unsigned int data_size)
{
	unsigned int npages = (data_size + PAGE_SIZE - 1) / PAGE_SIZE;
	void *data, *dup_data;

	if (npages > array->nr_pages)
		return NULL;

	data = vm_map_ram(array->pages, npages, -1);
	if (!data)
		return NULL;

	dup_data = kmemdup(data, data_size, GFP_KERNEL);
	vm_unmap_ram(data, npages);

	return dup_data;
}

static struct tdx_tsm_link *
tdx_spdm_session_connect(struct tdx_tsm_link *tlink,
			 struct tdx_page_array *dev_info)
{
	u64 r, out_msg_sz;
	int ret;

	guard(mutex)(&tdx_ext_lock);
	do {
		r = tdh_exec_spdm_connect(tlink->spdm_id, tlink->spdm_conf,
					  tlink->in_msg, tlink->out_msg,
					  dev_info, &out_msg_sz);
		ret = tdx_tsm_link_event_handler(tlink, r, out_msg_sz);
	} while (ret == -EAGAIN);

	if (ret)
		return ERR_PTR(ret);

	tlink->dev_info_size = out_msg_sz;
	return tlink;
}

static void tdx_spdm_session_disconnect(struct tdx_tsm_link *tlink)
{
	u64 r, out_msg_sz;
	int ret;

	guard(mutex)(&tdx_ext_lock);
	do {
		r = tdh_exec_spdm_disconnect(tlink->spdm_id, tlink->in_msg,
					     tlink->out_msg, &out_msg_sz);
		ret = tdx_tsm_link_event_handler(tlink, r, out_msg_sz);
	} while (ret == -EAGAIN);

	WARN_ON(ret);
}

DEFINE_FREE(tdx_spdm_session_disconnect, struct tdx_tsm_link *,
	    if (!IS_ERR_OR_NULL(_T)) tdx_spdm_session_disconnect(_T))

static struct tdx_tsm_link *tdx_spdm_create(struct tdx_tsm_link *tlink)
{
	unsigned int nr_pages = tdx_sysinfo->connect.spdm_mt_page_count;
	u64 spdm_id, r;

	struct tdx_page_array *spdm_mt __free(tdx_page_array_free) =
		tdx_page_array_create(nr_pages);
	if (!spdm_mt)
		return ERR_PTR(-ENOMEM);

	r = tdh_spdm_create(tlink->func_id, spdm_mt, &spdm_id);
	if (r)
		return ERR_PTR(-EFAULT);

	tlink->spdm_id = spdm_id;
	tlink->spdm_mt = no_free_ptr(spdm_mt);
	return tlink;
}

static void tdx_spdm_delete(struct tdx_tsm_link *tlink)
{
	struct pci_dev *pdev = tlink->pci.base_tsm.pdev;
	unsigned int nr_released;
	u64 released_hpa, r;

	r = tdh_spdm_delete(tlink->spdm_id, tlink->spdm_mt, &nr_released, &released_hpa);
	if (r) {
		pci_err(pdev, "fail to delete spdm 0x%llx\n", r);
		goto leak;
	}

	if (tdx_page_array_ctrl_release(tlink->spdm_mt, nr_released, released_hpa)) {
		pci_err(pdev, "fail to release spdm_mt pages\n");
		goto leak;
	}

	return;

leak:
	tdx_page_array_ctrl_leak(tlink->spdm_mt);
}

DEFINE_FREE(tdx_spdm_delete, struct tdx_tsm_link *, if (!IS_ERR_OR_NULL(_T)) tdx_spdm_delete(_T))

static struct tdx_tsm_link *tdx_spdm_session_setup(struct tdx_tsm_link *tlink)
{
	unsigned int nr_pages = tdx_sysinfo->connect.spdm_max_dev_info_pages;

	struct tdx_tsm_link *tlink_create __free(tdx_spdm_delete) =
		tdx_spdm_create(tlink);
	if (IS_ERR(tlink_create))
		return tlink_create;

	struct tdx_page_array *dev_info __free(tdx_page_array_free) =
		tdx_page_array_create(nr_pages);
	if (!dev_info)
		return ERR_PTR(-ENOMEM);

	struct tdx_tsm_link *tlink_connect __free(tdx_spdm_session_disconnect) =
		tdx_spdm_session_connect(tlink, dev_info);
	if (IS_ERR(tlink_connect))
		return tlink_connect;

	tlink->dev_info_data = tdx_dup_array_data(dev_info,
						  tlink->dev_info_size);
	if (!tlink->dev_info_data)
		return ERR_PTR(-ENOMEM);

	retain_and_null_ptr(tlink_create);
	retain_and_null_ptr(tlink_connect);

	return tlink;
}

static void tdx_spdm_session_teardown(struct tdx_tsm_link *tlink)
{
	kfree(tlink->dev_info_data);

	tdx_spdm_session_disconnect(tlink);
	tdx_spdm_delete(tlink);
}

DEFINE_FREE(tdx_spdm_session_teardown, struct tdx_tsm_link *,
	    if (!IS_ERR_OR_NULL(_T)) tdx_spdm_session_teardown(_T))

static int tdx_tsm_link_connect(struct pci_dev *pdev)
{
	struct tdx_tsm_link *tlink = to_tdx_tsm_link(pdev->tsm);

	struct tdx_tsm_link *tlink_spdm __free(tdx_spdm_session_teardown) =
		tdx_spdm_session_setup(tlink);
	if (IS_ERR(tlink_spdm)) {
		pci_err(pdev, "fail to setup spdm session\n");
		return PTR_ERR(tlink_spdm);
	}

	retain_and_null_ptr(tlink_spdm);

	return 0;
}

static void tdx_tsm_link_disconnect(struct pci_dev *pdev)
{
	struct tdx_tsm_link *tlink = to_tdx_tsm_link(pdev->tsm);

	tdx_spdm_session_teardown(tlink);
}

static struct pci_tsm *tdx_tsm_link_pf0_probe(struct tsm_dev *tsm_dev,
					      struct pci_dev *pdev)
{
	struct spdm_config_info_t *spdm_conf;
	int rc;

	struct tdx_tsm_link *tlink __free(kfree) = kzalloc_obj(*tlink);
	if (!tlink)
		return NULL;

	rc = pci_tsm_pf0_constructor(pdev, &tlink->pci, tsm_dev);
	if (rc)
		return NULL;

	tlink->func_id = tdisp_func_id(pdev);

	struct page *in_msg_page __free(__free_page) =
		alloc_page(GFP_KERNEL | __GFP_ZERO);
	if (!in_msg_page)
		return NULL;

	struct page *out_msg_page __free(__free_page) =
		alloc_page(GFP_KERNEL | __GFP_ZERO);
	if (!out_msg_page)
		return NULL;

	struct page *spdm_conf_page __free(kfree) =
		alloc_page(GFP_KERNEL | __GFP_ZERO);
	if (!spdm_conf_page)
		return NULL;

	/* use a default configuration, may require user input later */
	spdm_conf = page_address(spdm_conf_page);
	spdm_conf->vmm_spdm_cap = SPDM_CAP_KEY_UPD;
	spdm_conf->certificate_slot_mask = 0xff;

	tlink->in_msg = no_free_ptr(in_msg_page);
	tlink->out_msg = no_free_ptr(out_msg_page);
	tlink->spdm_conf = no_free_ptr(spdm_conf_page);

	return &no_free_ptr(tlink)->pci.base_tsm;
}

static void tdx_tsm_link_pf0_remove(struct pci_tsm *tsm)
{
	struct tdx_tsm_link *tlink = to_tdx_tsm_link(tsm);

	__free_page(tlink->spdm_conf);
	__free_page(tlink->out_msg);
	__free_page(tlink->in_msg);
	pci_tsm_pf0_destructor(&tlink->pci);
	kfree(tlink);
}

static struct pci_tsm *tdx_tsm_link_fn_probe(struct tsm_dev *tsm_dev,
					     struct pci_dev *pdev)
{
	int rc;

	struct pci_tsm *pci_tsm __free(kfree) = kzalloc_obj(*pci_tsm);
	if (!pci_tsm)
		return NULL;

	rc = pci_tsm_link_constructor(pdev, pci_tsm, tsm_dev);
	if (rc)
		return NULL;

	return no_free_ptr(pci_tsm);
}

static struct pci_tsm *tdx_tsm_link_probe(struct tsm_dev *tsm_dev,
					  struct pci_dev *pdev)
{
	if (is_pci_tsm_pf0(pdev))
		return tdx_tsm_link_pf0_probe(tsm_dev, pdev);

	return tdx_tsm_link_fn_probe(tsm_dev, pdev);
}

static void tdx_tsm_link_remove(struct pci_tsm *tsm)
{
	if (is_pci_tsm_pf0(tsm->pdev)) {
		tdx_tsm_link_pf0_remove(tsm);
		return;
	}

	/* for sub-functions */
	kfree(tsm);
}

static struct pci_tsm_ops tdx_tsm_link_ops = {
	.probe = tdx_tsm_link_probe,
	.remove = tdx_tsm_link_remove,
	.connect = tdx_tsm_link_connect,
	.disconnect = tdx_tsm_link_disconnect,
};

static void unregister_link_tsm(void *link)
{
	tsm_unregister(link);
}

static DEFINE_XARRAY(tlink_iommu_xa);

static void tdx_iommu_clear(u64 iommu_id, struct tdx_page_array *iommu_mt)
{
	u64 r;

	r = tdh_iommu_clear(iommu_id, iommu_mt);
	if (r) {
		pr_err("fail to clear tdx iommu 0x%llx\n", r);
		goto leak;
	}

	if (tdx_page_array_ctrl_release(iommu_mt, iommu_mt->nr_pages,
					virt_to_phys(iommu_mt->root))) {
		pr_err("fail to release iommu_mt pages\n");
		goto leak;
	}

	return;

leak:
	tdx_page_array_ctrl_leak(iommu_mt);
}

static int tdx_iommu_enable_one(struct dmar_drhd_unit *drhd)
{
	unsigned int nr_pages = tdx_sysinfo->connect.iommu_mt_page_count;
	u64 r, iommu_id;
	int ret;

	struct tdx_page_array *iommu_mt __free(tdx_page_array_free) =
		tdx_page_array_create_iommu_mt(1, nr_pages);
	if (!iommu_mt)
		return -ENOMEM;

	r = tdh_iommu_setup(drhd->reg_base_addr, iommu_mt, &iommu_id);
	/* This drhd doesn't support tdx mode, skip. */
	if ((r & TDX_SEAMCALL_STATUS_MASK)  == TDX_OPERAND_INVALID)
		return 0;

	if (r) {
		pr_err("fail to enable tdx mode for DRHD[0x%llx]\n",
		       drhd->reg_base_addr);
		return -EFAULT;
	}

	ret = xa_insert(&tlink_iommu_xa, (unsigned long)iommu_id,
			no_free_ptr(iommu_mt), GFP_KERNEL);
	if (ret) {
		tdx_iommu_clear(iommu_id, iommu_mt);
		return ret;
	}

	return 0;
}

static void tdx_iommu_disable_all(void *data)
{
	struct tdx_page_array *iommu_mt;
	unsigned long iommu_id;

	xa_for_each(&tlink_iommu_xa, iommu_id, iommu_mt)
		tdx_iommu_clear(iommu_id, iommu_mt);
}

static int tdx_iommu_enable_all(void)
{
	int ret;

	ret = do_for_each_drhd_unit(tdx_iommu_enable_one);
	if (ret)
		tdx_iommu_disable_all(NULL);

	return ret;
}

static int __maybe_unused tdx_connect_init(struct device *dev)
{
	struct tsm_dev *link;
	int ret;

	if (!IS_ENABLED(CONFIG_TDX_CONNECT))
		return 0;

	if (!(tdx_sysinfo->features.tdx_features0 & TDX_FEATURES0_TDXCONNECT))
		return 0;

	ret = tdx_iommu_enable_all();
	if (ret)
		return dev_err_probe(dev, ret, "Enable tdx iommu failed\n");

	ret = devm_add_action_or_reset(dev, tdx_iommu_disable_all, NULL);
	if (ret)
		return ret;

	link = tsm_register(dev, &tdx_tsm_link_ops);
	if (IS_ERR(link))
		return dev_err_probe(dev, PTR_ERR(link),
				     "failed to register TSM\n");

	return devm_add_action_or_reset(dev, unregister_link_tsm, link);
}

static int tdx_host_probe(struct faux_device *fdev)
{
	/* TODO: do tdx_connect_init() when it is fully implemented. */
	return 0;
}

static struct faux_device_ops tdx_host_ops = {
	.probe = tdx_host_probe,
};

static struct faux_device *fdev;

static int __init tdx_host_init(void)
{
	if (!x86_match_cpu(tdx_host_ids))
		return -ENODEV;

	tdx_sysinfo = tdx_get_sysinfo();
	if (!tdx_sysinfo)
		return -ENODEV;

	fdev = faux_device_create(KBUILD_MODNAME, NULL, &tdx_host_ops);
	if (!fdev)
		return -ENODEV;

	return 0;
}
module_init(tdx_host_init);

static void __exit tdx_host_exit(void)
{
	faux_device_destroy(fdev);
}
module_exit(tdx_host_exit);

MODULE_DESCRIPTION("TDX Host Services");
MODULE_LICENSE("GPL");

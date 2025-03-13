/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * Driver private data structures and helper macros
 * Copyright (c) 2019 - Maxim Levitsky
 */

#ifndef _MDEV_NVME_PRIV_H
#define _MDEV_NVME_PRIV_H

#include <linux/device.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/rbtree.h>
#include <linux/vfio.h>
#include <linux/mdev.h>
#include <linux/pci.h>
#include <linux/eventfd.h>
#include <linux/byteorder/generic.h>
#include "../../host/nvme.h"
#include "../nvmet.h"

#define NVME_MDEV_NVME_VER  NVME_VS(0x01, 0x03, 0x00)

#define NVME_MDEV_PCI_VENDOR_ID		PCI_VENDOR_ID_REDHAT_QUMRANET
#define NVME_MDEV_PCI_DEVICE_ID		0x1234
#define NVME_MDEV_PCI_SUBVENDOR_ID	PCI_SUBVENDOR_ID_REDHAT_QUMRANET
#define NVME_MDEV_PCI_SUBDEVICE_ID	0
#define NVME_MDEV_PCI_REVISION		0x0

#define DB_STRIDE_SHIFT 4 /*4 = 1 cacheline */
#define NVMET_MDEV_MAX_NR_QUEUES 16
#define MAX_VIRTUAL_IRQS 128

struct page_map {
	void *kmap;
	struct page *page;
	dma_addr_t iova;
};

struct user_prplist {
	/* used by user data iterator */
	struct page_map page;
	/* index of current entry */
	unsigned int index;
};

struct nvmet_ext_data_iter {
	/* private */
	struct nvmet_mdev_viommu *viommu;
	union {
		const union nvme_data_ptr *dptr;
	struct user_prplist uprp;
	};

	/* user interface */
	/* number of data pages, yet to be covered */
	u64		count;
	/* iterator physical address value */
	phys_addr_t	physical;

	struct list_head *mem_map_list;

	/* moves iterator to the next item */
	int (*next)(struct nvmet_ext_data_iter *data_iter);
	/*
	 * if != NULL, user should call this when it's done with data
	 * pointed by the iterator
	 */
	void (*release)(struct nvmet_ext_data_iter *data_iter);
};

/* virtual submission queue */
struct nvmet_mdev_vsq {
	struct nvmet_sq nvmet_sq;
	u16 qid;
	u16 size;
	/* next item to read */
	u16 head;

	/* the queue */
	struct nvme_command *data;
	unsigned int data_size;
	dma_addr_t iova;

	struct nvmet_mdev_vcq *vcq;
	struct nvmet_mdev_vctrl *vctrl;

	struct nvmet_mdev_req *reqs;
};

/* virtual completion queue */
struct nvmet_mdev_vcq {
	struct nvmet_cq nvmet_cq;
	/* basic queue settings */
	u16 qid;
	u16 size;
	u16 head;
	u16 tail;
	u16 phase;

	volatile struct nvme_completion *data;
	unsigned int data_size;
	dma_addr_t iova;

	struct llist_head mreq_list;
	struct nvmet_mdev_vctrl *vctrl;

	/* IRQ settings */
	int irq /* -1 if disabled */;
};

struct nvmet_mdev_req {
	struct nvmet_req req;
	struct nvme_completion cqe;
	struct sg_table sgt;
	struct nvmet_ext_data_iter data_iter;
	struct list_head mem_map_list;

	struct nvmet_mdev_vcq *vcq;
	struct llist_node cq_node;
};

/* Virtual IOMMU */
struct nvmet_mdev_viommu {
	struct vfio_device *vfio_dev;

	/* dma/prp bookkeeping */
	struct rb_root_cached maps_tree;
	struct list_head mem_map_list;
	struct nvmet_mdev_vctrl *vctrl;
};

struct doorbell {
	volatile __le32 sqt;
	u8 rsvd1[(4 << DB_STRIDE_SHIFT) - sizeof(__le32)];
	volatile __le32 cqh;
	u8 rsvd2[(4 << DB_STRIDE_SHIFT) - sizeof(__le32)];
};

/* MMIO state */
struct nvmet_mdev_user_ctrl_mmio {
	u32 cc;		/* controller configuration */
	u32 csts;	/* controller status */
	u64 cap		/* controller capabilities */;

	/* admin queue location & size */
	u32 aqa;
	u32 asql;
	u32 asqh;
	u32 acql;
	u32 acqh;

	bool shadow_db_supported;
	bool shadow_db_en;

	/* Regular doorbells */
	struct page *dbs_page;
	struct page *fake_eidx_page;
	void *db_page_kmap;
	void *fake_eidx_kmap;

	/* Shadow doorbells */
	dma_addr_t sdb_iova;
	struct page_map sdb_map;
	dma_addr_t eidx_iova;
	struct page_map seidx_map;

	/* Current doorbell mappings */
	volatile struct doorbell *dbs;
	volatile struct doorbell *eidxs;
};

/* pci configuration space of the device */
#define NVMET_MDEV_PCI_CFG_SIZE 4096
struct nvmet_mdev_pci_cfg_space {
	u8 *values;
	u8 *wmask;

	u8 pmcap;
	u8 pciecap;
	u8 msixcap;
	u8 end;
};

/* IRQ state of the controller */
struct nvmet_mdev_user_irq {
	struct eventfd_ctx *trigger;
	/* IRQ coalescing */
	bool irq_coalesc_en;
	ktime_t irq_time;
	unsigned int irq_pending_cnt;
};

enum nvmet_mdev_irq_mode {
	NVME_MDEV_IMODE_NONE,
	NVME_MDEV_IMODE_INTX,
	NVME_MDEV_IMODE_MSIX,
};

struct nvmet_mdev_user_irqs {
	/* one of VFIO_PCI_{INTX|MSI|MSIX}_IRQ_INDEX */
	enum nvmet_mdev_irq_mode mode;

	struct nvmet_mdev_user_irq vecs[MAX_VIRTUAL_IRQS];
	/* user interrupt coalescing settings */
	u8 irq_coalesc_max;
	unsigned int irq_coalesc_time_us;
	/* device removal trigger */
	struct eventfd_ctx *request_trigger;
};

/* IO region abstraction (BARs, the PCI config space */
struct nvmet_mdev_vctrl;
typedef int (*region_access_fn)(struct nvmet_mdev_vctrl *vctrl, u16 offset,
				char *buf, u32 size, bool is_write);

struct nvmet_mdev_io_region {
	unsigned int size;
	region_access_fn rw;

	/*
	 * IF != NULL, the mmap_area_start/size specify the mmaped window
	 * of this region
	 */
	const struct vm_operations_struct *mmap_ops;
	unsigned int mmap_area_start;
	unsigned int mmap_area_size;
};

struct nvmet_mdev_type {
	struct mdev_type type;
	char *name;
};

struct nvmet_mdev_port {
	struct mdev_parent parent;
	struct device device;
	struct nvmet_port *nvmet_port;
	struct mutex mutex;

	int ctrl_count;
	int type_count;
	struct nvmet_mdev_type *types;
	struct mdev_type **mdev_types;
};

#define vfio_dev_to_nvmet_mdev_vctrl(vfio_dev) \
	container_of(vfio_dev, struct nvmet_mdev_vctrl, vfio_dev)

/* Virtual NVME controller state */
struct nvmet_mdev_vctrl {
	struct vfio_device vfio_dev;
	struct kref ref;
	struct mutex lock;

	struct mdev_device *mdev;
	struct nvmet_ctrl *nvmet_ctrl;

	/* the IO thread */
	struct task_struct *iothread;
	bool iothread_parked;
	bool io_idle;
	ktime_t now;
	int expected_responses;
	int poll_timeout_ms;

	struct nvmet_mdev_io_region regions[VFIO_PCI_NUM_REGIONS];

	/* virtual controller state */
	struct nvmet_mdev_user_ctrl_mmio mmio;
	struct nvmet_mdev_pci_cfg_space pcicfg;
	struct nvmet_mdev_user_irqs irqs;

	/* emulated submission queues */
	struct nvmet_mdev_vsq vsqs[NVMET_MDEV_MAX_NR_QUEUES];
	unsigned long vsq_en[BITS_TO_LONGS(NVMET_MDEV_MAX_NR_QUEUES)];

	/* emulated completion queues */
	unsigned long vcq_en[BITS_TO_LONGS(NVMET_MDEV_MAX_NR_QUEUES)];
	struct nvmet_mdev_vcq vcqs[NVMET_MDEV_MAX_NR_QUEUES];

	/* Interface to access user memory */
	struct notifier_block vfio_map_notifier;
	struct notifier_block vfio_unmap_notifier;
	struct nvmet_mdev_viommu viommu;

	/* Settings */
	unsigned int arb_burst_shift;
	u8 worload_hint;
	unsigned int iothread_cpu;
};

/* vctrl.c */
int nvmet_mdev_vctrl_create(struct nvmet_mdev_vctrl *vctrl,
			    struct mdev_device *mdev);
void nvmet_mdev_vctrl_destroy(struct nvmet_mdev_vctrl *vctrl);
int nvmet_mdev_vctrl_open(struct vfio_device *vfio_dev);
void nvmet_mdev_vctrl_release(struct vfio_device *vfio_dev);
bool nvmet_mdev_vctrl_enable(struct nvmet_mdev_vctrl *vctrl, dma_addr_t cqiova,
			     dma_addr_t sqiova, u32 sizes);
void nvmet_mdev_vctrl_disable(struct nvmet_mdev_vctrl *vctrl);
void nvmet_mdev_vctrl_reset(struct nvmet_mdev_vctrl *vctrl);
void __nvmet_mdev_vctrl_reset(struct nvmet_mdev_vctrl *vctrl, bool pci_reset);
void nvmet_mdev_vctrl_add_region(struct nvmet_mdev_vctrl *vctrl,
				 unsigned int index, unsigned int size,
				 region_access_fn access_fn);
void nvmet_mdev_vctrl_region_set_mmap(struct nvmet_mdev_vctrl *vctrl,
				      unsigned int index, unsigned int offset,
				      unsigned int size,
				      const struct vm_operations_struct *ops);
void nvmet_mdev_vctrl_region_disable_mmap(struct nvmet_mdev_vctrl *vctrl,
					  unsigned int index);
bool nvmet_mdev_vctrl_is_dead(struct nvmet_mdev_vctrl *vctrl);
int nvmet_mdev_vctrl_viommu_map(struct nvmet_mdev_vctrl *vctrl, u32 flags,
				dma_addr_t iova, u64 size);
int nvmet_mdev_vctrl_viommu_unmap(struct nvmet_mdev_vctrl *vctrl,
				  dma_addr_t iova, u64 size);

/* io.c */
int nvmet_mdev_io_create(struct nvmet_mdev_vctrl *vctrl);
void nvmet_mdev_io_free(struct nvmet_mdev_vctrl *vctrl);
void nvmet_mdev_io_resume(struct nvmet_mdev_vctrl *vctrl);
bool nvmet_mdev_io_pause(struct nvmet_mdev_vctrl *vctrl);
void nvmet_mdev_assert_io_not_running(struct nvmet_mdev_vctrl *vctrl);
bool nvmet_mdev_process_responses(struct nvmet_mdev_vctrl *vctrl,
				  struct nvmet_mdev_vcq *vcq);

/* mmio.c */
int nvmet_mdev_mmio_create(struct nvmet_mdev_vctrl *vctrl);
void nvmet_mdev_mmio_open(struct nvmet_mdev_vctrl *vctrl);
void nvmet_mdev_mmio_reset(struct nvmet_mdev_vctrl *vctrl, bool pci_reset);
void nvmet_mdev_mmio_free(struct nvmet_mdev_vctrl *vctrl);

int nvmet_mdev_mmio_enable_dbs(struct nvmet_mdev_vctrl *vctrl);
int nvmet_mdev_mmio_enable_dbs_shadow(struct nvmet_mdev_vctrl *vctrl,
				      dma_addr_t sdb_iova,
				      dma_addr_t eidx_iova);

void nvmet_mdev_mmio_viommu_update(struct nvmet_mdev_vctrl *vctrl);
void nvmet_mdev_mmio_disable_dbs(struct nvmet_mdev_vctrl *vctrl);
bool nvmet_mdev_mmio_db_check(struct nvmet_mdev_vctrl *vctrl, u16 qid, u16 size,
			      u16 db);

/* pci.c */
int nvmet_mdev_pci_create(struct nvmet_mdev_vctrl *vctrl);
void nvmet_mdev_pci_free(struct nvmet_mdev_vctrl *vctrl);
void nvmet_mdev_pci_setup_bar(struct nvmet_mdev_vctrl *vctrl, u8 bar,
			      unsigned int size, region_access_fn access_fn);

/* irq.c */
void nvmet_mdev_irqs_setup(struct nvmet_mdev_vctrl *vctrl);
void nvmet_mdev_irqs_reset(struct nvmet_mdev_vctrl *vctrl);
int nvmet_mdev_irqs_enable(struct nvmet_mdev_vctrl *vctrl,
			   enum nvmet_mdev_irq_mode mode);
void nvmet_mdev_irqs_disable(struct nvmet_mdev_vctrl *vctrl,
			     enum nvmet_mdev_irq_mode mode);
int nvmet_mdev_irqs_set_triggers(struct nvmet_mdev_vctrl *vctrl,
				 int start, int count, int32_t *fds);
int nvmet_mdev_irqs_set_unplug_trigger(struct nvmet_mdev_vctrl *vctrl,
				       int32_t fd);
void nvmet_mdev_irq_raise_unplug_event(struct nvmet_mdev_vctrl *vctrl);
void nvmet_mdev_irq_raise(struct nvmet_mdev_vctrl *vctrl, unsigned int index);
void nvmet_mdev_irq_trigger(struct nvmet_mdev_vctrl *vctrl, unsigned int index);
void nvmet_mdev_irq_cond_trigger(struct nvmet_mdev_vctrl *vctrl,
				 unsigned int index, ktime_t now);
void nvmet_mdev_irq_clear(struct nvmet_mdev_vctrl *vctrl, unsigned int index,
			  ktime_t now);

/* vcq.c */
int nvmet_mdev_vcq_init(struct nvmet_mdev_vctrl *vctrl, u16 qid,
			dma_addr_t iova, u16 size, int irq);
int nvmet_mdev_vcq_viommu_update(struct nvmet_mdev_viommu *viommu,
				 struct nvmet_mdev_vcq *q);
void nvmet_mdev_vcq_delete(struct nvmet_mdev_vctrl *vctrl, u16 qid);
void nvmet_mdev_vcq_process(struct nvmet_mdev_vctrl *vctrl,
			    struct nvmet_mdev_vcq *q, bool trigger_irqs,
			    ktime_t now);
bool nvmet_mdev_vcq_flush(struct nvmet_mdev_vctrl *vctrl,
			  struct nvmet_mdev_vcq *q, ktime_t now);
void nvmet_mdev_vcq_write_cqe(struct nvmet_mdev_vctrl *vctrl,
			      struct nvmet_mdev_vcq *q,
			      struct nvme_completion *cqe);

/* vsq.c */
int nvmet_mdev_vsq_init(struct nvmet_mdev_vctrl *vctrl, u16 qid,
			dma_addr_t iova, u16 size, u16 cqid);
int nvmet_mdev_vsq_viommu_update(struct nvmet_mdev_viommu *viommu,
				 struct nvmet_mdev_vsq *q);
void nvmet_mdev_vsq_delete(struct nvmet_mdev_vctrl *vctrl, u16 qid);
struct nvme_command *nvmet_mdev_vsq_get_cmd(struct nvmet_mdev_vctrl *vctrl,
					    struct nvmet_mdev_vsq *q,
					    u16 *index);
bool nvmet_mdev_vsq_suspend_io(struct nvmet_mdev_vsq *q);

/* udata.c */
void nvmet_mdev_udata_iter_setup(struct nvmet_mdev_viommu *viommu,
				 struct nvmet_ext_data_iter *iter,
				 struct list_head *maps_list);
int nvmet_mdev_udata_iter_set_dptr(struct nvmet_ext_data_iter *it,
				   const union nvme_data_ptr *dptr, u64 size);
void *nvmet_mdev_udata_update_queue_vmap(struct nvmet_mdev_viommu *viommu,
					 dma_addr_t iova, void *data,
					 unsigned int data_size);
void nvmet_mdev_udata_queue_vunmap(struct nvmet_mdev_viommu *viommu,
				   dma_addr_t iova, void *data,
				   unsigned int data_size);

/* viommu.c */
void nvmet_mdev_viommu_init(struct nvmet_mdev_viommu *viommu,
			    struct vfio_device *vfio_dev);
int nvmet_mdev_viommu_add(struct nvmet_mdev_viommu *viommu, u32 flags,
			  dma_addr_t iova, u64 size,
			  struct list_head *maps_list);
int nvmet_mdev_viommu_remove(struct nvmet_mdev_viommu *viommu,
			     dma_addr_t iova, u64 size);
int nvmet_mdev_viommu_remove_list(struct nvmet_mdev_viommu *viommu,
				  struct list_head *remove_list);
int nvmet_mdev_viommu_translate(struct nvmet_mdev_viommu *viommu,
				dma_addr_t iova, dma_addr_t *physical);
int nvmet_mdev_viommu_create_kmap(struct nvmet_mdev_viommu *viommu,
				  dma_addr_t iova, struct page_map *page);
void nvmet_mdev_viommu_free_kmap(struct nvmet_mdev_viommu *viommu,
				 struct page_map *page);
void nvmet_mdev_viommu_update_kmap(struct nvmet_mdev_viommu *viommu,
				   struct page_map *page);
void nvmet_mdev_viommu_reset(struct nvmet_mdev_viommu *viommu);

/* instance.c */
int nvmet_mdev_register_port(struct nvmet_mdev_port *mport);
void nvmet_mdev_unregister_port(struct nvmet_mdev_port *mport);
void nvmet_mdev_remove_ctrl(struct nvmet_mdev_vctrl *vctrl);

/* some utilities*/

#define store_le32(address, value) (*((__le32 *)(address)) = cpu_to_le32(value))
#define store_le16(address, value) (*((__le16 *)(address)) = cpu_to_le16(value))
#define store_le8(address, value)  (*((u8 *)(address)) = (value))

#define load_le16(address) le16_to_cpu(*(__le16 *)(address))

#define DNR(e) ((e) | NVME_STATUS_DNR)

#define PAGE_ADDRESS(address) ((address) & PAGE_MASK)

#define _DBG(vctrl, fmt, ...) \
	dev_dbg(vctrl->vfio_dev.dev, fmt, ##__VA_ARGS__)

#define _INFO(vctrl, fmt, ...) \
	dev_info(vctrl->vfio_dev.dev, fmt, ##__VA_ARGS__)

#define _WARN(vctrl, fmt, ...) \
	dev_warn(vctrl->vfio_dev.dev, fmt, ##__VA_ARGS__)

/* Rough translation of internal errors to the NVME errors */
static inline int nvmet_mdev_translate_error(int error)
{
	/* nvme status, including no error (NVME_SC_SUCCESS) */
	if (error >= 0)
		return error;

	switch (error) {
	case -ENOMEM:
		/* no memory - truly an internal error */
		return NVME_SC_INTERNAL;
	case -ENOSPC:
		/*
		 * Happens when user sends to large PRP list User shoudn't do
		 * this since the maximum transfer size is specified in the
		 * controller caps
		 */
		return DNR(NVME_SC_DATA_XFER_ERROR);
	case -EFAULT:
		/* Bad memory pointers in the prp lists */
		return DNR(NVME_SC_DATA_XFER_ERROR);
	case -EINVAL:
		/* Bad prp offsets in the prp lists/command */
		return DNR(NVME_SC_PRP_INVALID_OFFSET);
	default:
		/* Shouldn't happen */
		WARN_ON_ONCE(true);
		return NVME_SC_INTERNAL;
	}
}

extern const struct nvmet_fabrics_ops nvmet_mdev_ops;

#endif // _MDEV_NVME_H

// SPDX-License-Identifier: GPL-2.0
/*
 * SCSI (virtio-scsi) PCI Endpoint Function target driver.
 *
 * Modeled on the nvme pci-epf.c target driver. This driver presents
 * a transitional virtio-scsi device to the host over the legacy virtio PCI
 * transport, fetches SCSI commands from the request virtqueue, and dispatches
 * them through the LIO/TCM target core.
 *
 * Copyright (c) 2026, Western Digital Corporation or its affiliates.
 */
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/atomic.h>
#include <linux/configfs.h>
#include <linux/delay.h>
#include <linux/dmaengine.h>
#include <linux/io.h>
#include <linux/kthread.h>
#include <linux/mempool.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/pci_ids.h>
#include <linux/pci-epc.h>
#include <linux/pci-epf.h>
#include <linux/pci_regs.h>
#include <linux/sched/types.h>
#include <linux/slab.h>
#include <linux/virtio_config.h>
#include <linux/virtio_ids.h>
#include <linux/virtio_pci.h>
#include <linux/virtio_ring.h>
#include <linux/virtio_scsi.h>
#include <linux/wait.h>

#include <scsi/scsi.h>
#include <scsi/scsi_proto.h>

#include <target/target_core_base.h>
#include <target/target_core_fabric.h>

/*
 * Upper bound on the data transfer size the device advertises.
 */
#define SCSIT_PCI_EPF_MAX_SEGS		128
#define SCSIT_PCI_EPF_MDTS		(SCSIT_PCI_EPF_MAX_SEGS * PAGE_SIZE)

/*
 * Number of virtqueues exposed by this device. virtio-scsi requires at least
 * a control queue (index 0), an event queue (index 1) and one or more request
 * queues (index 2..N). We default to 1 request queue but allow more.
 */
#define SCSIT_PCI_EPF_VQ_CTRL		0
#define SCSIT_PCI_EPF_VQ_EVENT		1
#define SCSIT_PCI_EPF_VQ_REQUEST_BASE	2
#define SCSIT_PCI_EPF_MAX_REQ_VQS	8
#define SCSIT_PCI_EPF_NR_VQS		\
	(SCSIT_PCI_EPF_VQ_REQUEST_BASE + SCSIT_PCI_EPF_MAX_REQ_VQS)

#define SCSIT_PCI_EPF_MAX_QUEUE_DEPTH		128

/*
 * IRQ vector coalescing threshold: post a few used ring entries before raising
 * an interrupt to the host. Mirrors the NVMe driver's defaults.
 */
#define SCSIT_PCI_EPF_IV_THRESHOLD	8

/*
 * Virtqueue polling interval.
 *
 * The config-space polling is handled by a dedicated busy-polling kthread
 * (scsit_pci_epf_poll_cfg_thread) because the window the host gives us to
 * react to QUEUE_PFN/QUEUE_SEL writes between consecutive setup_vq() calls
 * is in the low microsecond range.
 */
#define SCSIT_PCI_EPF_VQ_POLL_INTERVAL	msecs_to_jiffies(5)
#define SCSIT_PCI_EPF_VQ_POLL_IDLE	msecs_to_jiffies(5000)

/*
 * Request virtqueue arbitration burst: fetch at most 8 descriptors at a time.
 */
#define SCSIT_PCI_EPF_VQ_AB		8

/*
 * Handling of used rings is normally immediate, unless we fail to map the
 * used ring or the host has not consumed previous entries.
 */
#define SCSIT_PCI_EPF_USED_RETRY_INTERVAL	msecs_to_jiffies(1)

/*
 * Offset within BAR 0 of the virtio_scsi_config device-specific region.
 * When MSI-X is enabled (as is always the case for this driver), the
 * legacy spec places device config at offset 24 (VIRTIO_PCI_CONFIG_OFF(1)).
 */
#define SCSIT_PCI_EPF_BAR_DEVCFG_OFF	VIRTIO_PCI_CONFIG_OFF(1)
#define SCSIT_PCI_EPF_BAR_DEVCFG_SZ	\
	(sizeof(struct virtio_scsi_config))

/*
 * Bit 0 of the virtio ISR status register indicates a queue interrupt.
 */
#define VIRTIO_PCI_ISR_QUEUE		0x1

#define SCSIT_PCI_EPF_BAR_REGS_END	0x1000

/*
 * virtio-scsi defaults.
 */
#define SCSIT_PCI_EPF_SENSE_SIZE	VIRTIO_SCSI_SENSE_DEFAULT_SIZE
#define SCSIT_PCI_EPF_CDB_SIZE		VIRTIO_SCSI_CDB_DEFAULT_SIZE
#define SCSIT_PCI_EPF_EVENT_LEN		8

#define SCSIT_PCI_EPF_FABRIC_NAME	"virtio-scsi-pci-epf"

static LIST_HEAD(scsit_pci_epf_tpgs);
static DEFINE_MUTEX(scsit_pci_epf_tpgs_mutex);

enum scsit_pci_epf_vq_flags {
	SCSIT_PCI_EPF_VQ_LIVE = 0,	/* The virtqueue is live */
	SCSIT_PCI_EPF_VQ_IRQ_ENABLED,	/* IRQ is enabled for this vq */
};

/*
 * IRQ vector descriptor.
 */
struct scsit_pci_epf_irq_vector {
	unsigned int	vector;
	unsigned int	ref;
	bool		cd;
	int		nr_irqs;
};

/*
 * State for a single virtqueue (one of control/event/request).
 */
struct scsit_pci_epf_vq {
	struct scsit_pci_epf_ctrl	*ctrl;
	unsigned long			flags;

	u16				vqid;
	u16				depth;
	u16				vector;

	/* Host-provided ring addresses. */
	u64				desc_pci_addr;
	u64				avail_pci_addr;
	u64				used_pci_addr;

	/* Mapped vring components. */
	struct pci_epc_map		desc_map;
	struct pci_epc_map		avail_map;
	struct pci_epc_map		used_map;

	/* Cached ring positions. */
	u16				last_avail_idx;
	u16				next_used_idx;

	/* MSI-X coalescing state. */
	struct scsit_pci_epf_irq_vector	*iv;

	/* Workqueue for executing requests parsed off this queue. */
	struct workqueue_struct		*cmd_wq;
	struct delayed_work		used_work;
	spinlock_t			lock;
	struct list_head		complete_list;
};

/*
 * PCI Root Complex (RC) address data segment for mapping a request or
 * response buffer @buf of @length bytes to the PCI address @pci_addr.
 */
struct scsit_pci_epf_segment {
	void				*buf;
	u64				pci_addr;
	u32				length;
};

/*
 * SCSI command descriptor.
 *
 * Wraps an se_cmd (LIO target core command) with the bookkeeping needed to
 * fetch the request from the host, marshal data and post the response back to
 * the host via the used ring.
 */
struct scsit_pci_epf_cmd {
	struct list_head		link;

	struct se_cmd			se_cmd;

	/* virtio-scsi request and response, populated from the host. */
	struct virtio_scsi_cmd_req	req;
	struct virtio_scsi_cmd_resp	resp;

	unsigned char			sense_buf[TRANSPORT_SENSE_BUFFER];

	struct scsit_pci_epf_ctrl	*ctrl;
	struct scsit_pci_epf_vq		*vq;

	u16				head_idx;

	enum dma_data_direction		dma_dir;

	unsigned int			nr_data_segs;
	struct scsit_pci_epf_segment	data_seg;
	struct scsit_pci_epf_segment	*data_segs;
	struct scatterlist		data_sgl;
	struct sg_table			data_sgt;

	u64				resp_pci_addr;
	u32				resp_len;

	struct work_struct		work;
	struct completion		done;

	bool				is_tmr;
	struct virtio_scsi_ctrl_tmf_req	tmf_req;
	struct virtio_scsi_ctrl_tmf_resp tmf_resp;

	/*
	 * True once target_init_cmd() has succeeded for this cmd, i.e. TCM
	 * owns a reference and will eventually call release_cmd() once the
	 * command finishes processing.
	 *
	 * Used by used_work() to decide whether to free the cmd directly
	 * (failure paths before TCM took ownership) or drop our reference
	 * via target_put_sess_cmd() and let TCM tear it down.
	 */
	bool				tcm_owned;
};

#define to_scsit_pci_epf_cmd(_se)		\
	container_of(_se, struct scsit_pci_epf_cmd, se_cmd)

/*
 * I_T Nexus (per-EPF-function session). Allocated when the EPF function is
 * started and torn down when it is unbound.
 */
struct scsit_pci_epf_nexus {
	struct se_session		*se_sess;
};

/*
 * Target Portal Group (TPG) wrapper.
 *
 * Each TPG is registered through LIO's configfs and represents a SCSI target
 * port group with one or more LUNs. Each EPF function references one TPG by
 * "wwn:tpgt" name in its configfs attributes.
 */
struct scsit_pci_epf_tpg {
	struct se_portal_group		se_tpg;
	struct scsit_pci_epf_wwn	*wwn;
	struct scsit_pci_epf_nexus	*nexus;
	u16				tpgt;
	int				prot_type;
	struct list_head		list;

	/* The EPF controller currently bound to this TPG, if any. */
	struct scsit_pci_epf_ctrl	*ctrl;
};

#define to_scsit_pci_epf_tpg(_tpg)		\
	container_of(_tpg, struct scsit_pci_epf_tpg, se_tpg)

/*
 * WWN (target port). Holds an array of TPGs created beneath it via
 * configfs.
 */
struct scsit_pci_epf_wwn {
	struct se_wwn			se_wwn;
	char				name[256];
};

#define to_scsit_pci_epf_wwn(_w)		\
	container_of(_w, struct scsit_pci_epf_wwn, se_wwn)

struct scsit_pci_epf_ctrl {
	struct scsit_pci_epf		*scsi_epf;
	struct scsit_pci_epf_tpg	*tpg;
	struct device			*dev;

	/* Shadow of the virtio common configuration. */
	void				*bar;
	u64				device_features;
	u64				driver_features;
	u32				device_feature_select;
	u32				driver_feature_select;
	u8				device_status;
	u8				config_generation;
	u16				msix_config;
	u16				num_queues;
	u16				queue_select;

	/* Virtqueues. */
	struct scsit_pci_epf_vq		*vqs;
	unsigned int			nr_vqs;
	unsigned int			req_vq_ab;

	/* virtio-scsi device-specific configuration. */
	struct virtio_scsi_config	devcfg;

	/* Command mempool. */
	mempool_t			cmd_pool;

	/*
	 * Number of commands currently allocated from cmd_pool (incremented in
	 * alloc_cmd(), decremented in free_cmd()). Teardown waits for this to
	 * reach zero before freeing the virtqueues and the mempool.
	 */
	atomic_t			n_inflight;
	wait_queue_head_t		inflight_wq;

	size_t				mdts;
	u32				cmd_per_lun;

	/*
	 * Busy-polling kthread that watches the legacy virtio register block
	 * in BAR 0 and reacts to host writes (QUEUE_SEL, QUEUE_PFN, STATUS,
	 * MSI vectors).
	 */
	struct task_struct		*poll_cfg_task;
	struct delayed_work		poll_vqs;

	struct mutex			irq_lock;
	struct scsit_pci_epf_irq_vector	*irq_vectors;
	unsigned int			irq_vector_threshold;

	bool				link_up;
	bool				running;
};

/*
 * PCI EPF driver private data.
 */
struct scsit_pci_epf {
	struct pci_epf			*epf;

	const struct pci_epc_features	*epc_features;

	void				*reg_bar;
	size_t				msix_table_offset;

	unsigned int			irq_type;
	unsigned int			nr_vectors;

	struct scsit_pci_epf_ctrl	ctrl;

	bool				dma_enabled;
	struct dma_chan			*dma_tx_chan;
	struct mutex			dma_tx_lock;
	struct dma_chan			*dma_rx_chan;
	struct mutex			dma_rx_lock;

	struct mutex			mmio_lock;

	/* PCI endpoint function configfs attributes. */
	struct config_group		group;
	char				tpg_wwn[256];
	u16				tpgt;
	unsigned int			nr_req_queues;
};

static void scsit_pci_epf_exec_cmd_work(struct work_struct *work);
static void scsit_pci_epf_used_work(struct work_struct *work);
static int scsit_pci_epf_poll_cfg_thread(void *arg);
static void scsit_pci_epf_poll_vqs_work(struct work_struct *work);
static void scsit_pci_epf_init_bar_regs(struct scsit_pci_epf_ctrl *ctrl);
static const struct target_core_fabric_ops scsit_pci_epf_fabric_ops;

static inline u8 scsit_pci_epf_bar_read8(struct scsit_pci_epf_ctrl *ctrl,
					 u32 off)
{
	return READ_ONCE(*(u8 *)(ctrl->bar + off));
}

static inline void scsit_pci_epf_bar_write8(struct scsit_pci_epf_ctrl *ctrl,
					    u32 off, u8 val)
{
	WRITE_ONCE(*(u8 *)(ctrl->bar + off), val);
}

static inline u16 scsit_pci_epf_bar_read16(struct scsit_pci_epf_ctrl *ctrl,
					   u32 off)
{
	__le16 *bar_reg = ctrl->bar + off;

	return le16_to_cpu(READ_ONCE(*bar_reg));
}

static inline void scsit_pci_epf_bar_write16(struct scsit_pci_epf_ctrl *ctrl,
					     u32 off, u16 val)
{
	__le16 *bar_reg = ctrl->bar + off;

	WRITE_ONCE(*bar_reg, cpu_to_le16(val));
}

static inline u32 scsit_pci_epf_bar_read32(struct scsit_pci_epf_ctrl *ctrl,
					   u32 off)
{
	__le32 *bar_reg = ctrl->bar + off;

	return le32_to_cpu(READ_ONCE(*bar_reg));
}

static inline void scsit_pci_epf_bar_write32(struct scsit_pci_epf_ctrl *ctrl,
					     u32 off, u32 val)
{
	__le32 *bar_reg = ctrl->bar + off;

	WRITE_ONCE(*bar_reg, cpu_to_le32(val));
}

static inline u64 scsit_pci_epf_bar_read64(struct scsit_pci_epf_ctrl *ctrl,
					   u32 off)
{
	return (u64)scsit_pci_epf_bar_read32(ctrl, off) |
		((u64)scsit_pci_epf_bar_read32(ctrl, off + 4) << 32);
}

static inline void scsit_pci_epf_bar_write64(struct scsit_pci_epf_ctrl *ctrl,
					     u32 off, u64 val)
{
	scsit_pci_epf_bar_write32(ctrl, off, val & 0xFFFFFFFF);
	scsit_pci_epf_bar_write32(ctrl, off + 4, (val >> 32) & 0xFFFFFFFF);
}

static inline int scsit_pci_epf_mem_map(struct scsit_pci_epf *scsi_epf,
		u64 pci_addr, size_t size, struct pci_epc_map *map)
{
	struct pci_epf *epf = scsi_epf->epf;

	return pci_epc_mem_map(epf->epc, epf->func_no, epf->vfunc_no,
			       pci_addr, size, map);
}

static inline void scsit_pci_epf_mem_unmap(struct scsit_pci_epf *scsi_epf,
					   struct pci_epc_map *map)
{
	struct pci_epf *epf = scsi_epf->epf;

	pci_epc_mem_unmap(epf->epc, epf->func_no, epf->vfunc_no, map);
}

/*
 * Persistently map a virtqueue ring into an EPC outbound window so it can be
 * accessed with aligned readw()/writel() rather than per-access
 * pci_epc_mem_map() + memcpy_fromio().
 */
static int scsit_pci_epf_map_ring(struct scsit_pci_epf_ctrl *ctrl,
				  u64 pci_addr, size_t size,
				  struct pci_epc_map *map)
{
	int ret;

	ret = scsit_pci_epf_mem_map(ctrl->scsi_epf, pci_addr, size, map);
	if (ret)
		return ret;

	if (map->pci_size < size) {
		dev_err(ctrl->dev,
			"Partial ring mapping at 0x%llx (%zu < %zu) - EPC window too small\n",
			pci_addr, map->pci_size, size);
		scsit_pci_epf_mem_unmap(ctrl->scsi_epf, map);
		memset(map, 0, sizeof(*map));
		return -ENOSPC;
	}

	return 0;
}

static void scsit_pci_epf_unmap_vq_rings(struct scsit_pci_epf_vq *vq)
{
	struct scsit_pci_epf_ctrl *ctrl = vq->ctrl;

	if (vq->used_map.virt_addr) {
		scsit_pci_epf_mem_unmap(ctrl->scsi_epf, &vq->used_map);
		memset(&vq->used_map, 0, sizeof(vq->used_map));
	}
	if (vq->avail_map.virt_addr) {
		scsit_pci_epf_mem_unmap(ctrl->scsi_epf, &vq->avail_map);
		memset(&vq->avail_map, 0, sizeof(vq->avail_map));
	}
	if (vq->desc_map.virt_addr) {
		scsit_pci_epf_mem_unmap(ctrl->scsi_epf, &vq->desc_map);
		memset(&vq->desc_map, 0, sizeof(vq->desc_map));
	}
}

static int scsit_pci_epf_map_vq_rings(struct scsit_pci_epf_vq *vq)
{
	struct scsit_pci_epf_ctrl *ctrl = vq->ctrl;
	size_t desc_size, avail_size, used_size;
	int ret;

	if (vq->vqid == SCSIT_PCI_EPF_VQ_EVENT)
		return 0;

	desc_size = (size_t)vq->depth * sizeof(struct vring_desc);
	avail_size = offsetof(struct vring_avail, ring) +
		     (size_t)vq->depth * sizeof(__le16) + sizeof(__le16);
	used_size = offsetof(struct vring_used, ring) +
		    (size_t)vq->depth * sizeof(struct vring_used_elem) +
		    sizeof(__le16);

	ret = scsit_pci_epf_map_ring(ctrl, vq->desc_pci_addr, desc_size,
				     &vq->desc_map);
	if (ret)
		return ret;

	ret = scsit_pci_epf_map_ring(ctrl, vq->avail_pci_addr, avail_size,
				     &vq->avail_map);
	if (ret)
		goto err;

	ret = scsit_pci_epf_map_ring(ctrl, vq->used_pci_addr, used_size,
				     &vq->used_map);
	if (ret)
		goto err;

	return 0;

err:
	scsit_pci_epf_unmap_vq_rings(vq);
	return ret;
}

struct scsit_pci_epf_dma_filter {
	struct device *dev;
	u32 dma_mask;
};

static bool scsit_pci_epf_dma_filter(struct dma_chan *chan, void *arg)
{
	struct scsit_pci_epf_dma_filter *filter = arg;
	struct dma_slave_caps caps;

	memset(&caps, 0, sizeof(caps));
	dma_get_slave_caps(chan, &caps);

	return chan->device->dev == filter->dev &&
		(filter->dma_mask & caps.directions);
}

static void scsit_pci_epf_init_dma(struct scsit_pci_epf *scsi_epf)
{
	struct pci_epf *epf = scsi_epf->epf;
	struct device *dev = &epf->dev;
	struct scsit_pci_epf_dma_filter filter;
	struct dma_chan *chan;
	dma_cap_mask_t mask;

	mutex_init(&scsi_epf->dma_rx_lock);
	mutex_init(&scsi_epf->dma_tx_lock);

	dma_cap_zero(mask);
	dma_cap_set(DMA_SLAVE, mask);

	filter.dev = epf->epc->dev.parent;
	filter.dma_mask = BIT(DMA_DEV_TO_MEM);

	chan = dma_request_channel(mask, scsit_pci_epf_dma_filter, &filter);
	if (!chan)
		goto out_dma_no_rx;

	scsi_epf->dma_rx_chan = chan;

	filter.dma_mask = BIT(DMA_MEM_TO_DEV);
	chan = dma_request_channel(mask, scsit_pci_epf_dma_filter, &filter);
	if (!chan)
		goto out_dma_no_tx;

	scsi_epf->dma_tx_chan = chan;
	scsi_epf->dma_enabled = true;

	dev_info(dev, "Using DMA RX channel %s, maximum segment size %u B\n",
		dma_chan_name(scsi_epf->dma_rx_chan),
		dma_get_max_seg_size(dmaengine_get_dma_device(scsi_epf->
							      dma_rx_chan)));

	dev_info(dev, "Using DMA TX channel %s, maximum segment size %u B\n",
		dma_chan_name(scsi_epf->dma_tx_chan),
		dma_get_max_seg_size(dmaengine_get_dma_device(scsi_epf->
							      dma_tx_chan)));
	return;

out_dma_no_tx:
	dma_release_channel(scsi_epf->dma_rx_chan);
	scsi_epf->dma_rx_chan = NULL;

out_dma_no_rx:
	mutex_destroy(&scsi_epf->dma_rx_lock);
	mutex_destroy(&scsi_epf->dma_tx_lock);
	scsi_epf->dma_enabled = false;

	dev_info(dev, "DMA not supported, falling back to MMIO\n");
}

static void scsit_pci_epf_deinit_dma(struct scsit_pci_epf *scsi_epf)
{
	if (!scsi_epf->dma_enabled)
		return;

	dma_release_channel(scsi_epf->dma_tx_chan);
	scsi_epf->dma_tx_chan = NULL;
	dma_release_channel(scsi_epf->dma_rx_chan);
	scsi_epf->dma_rx_chan = NULL;
	mutex_destroy(&scsi_epf->dma_rx_lock);
	mutex_destroy(&scsi_epf->dma_tx_lock);
	scsi_epf->dma_enabled = false;
}

static int scsit_pci_epf_dma_transfer(struct scsit_pci_epf *scsi_epf,
		struct scsit_pci_epf_segment *seg, enum dma_data_direction dir)
{
	struct pci_epf *epf = scsi_epf->epf;
	struct dma_async_tx_descriptor *desc;
	struct dma_slave_config sconf = {};
	struct device *dev = &epf->dev;
	struct device *dma_dev;
	struct dma_chan *chan;
	dma_cookie_t cookie;
	dma_addr_t dma_addr;
	struct mutex *lock;
	int ret;

	switch (dir) {
	case DMA_FROM_DEVICE:
		lock = &scsi_epf->dma_rx_lock;
		chan = scsi_epf->dma_rx_chan;
		sconf.direction = DMA_DEV_TO_MEM;
		sconf.src_addr = seg->pci_addr;
		break;
	case DMA_TO_DEVICE:
		lock = &scsi_epf->dma_tx_lock;
		chan = scsi_epf->dma_tx_chan;
		sconf.direction = DMA_MEM_TO_DEV;
		sconf.dst_addr = seg->pci_addr;
		break;
	default:
		return -EINVAL;
	}

	mutex_lock(lock);

	dma_dev = dmaengine_get_dma_device(chan);
	dma_addr = dma_map_single(dma_dev, seg->buf, seg->length, dir);
	ret = dma_mapping_error(dma_dev, dma_addr);
	if (ret)
		goto unlock;

	ret = dmaengine_slave_config(chan, &sconf);
	if (ret) {
		dev_err(dev, "Failed to configure DMA channel\n");
		goto unmap;
	}

	desc = dmaengine_prep_slave_single(chan, dma_addr, seg->length,
					   sconf.direction, DMA_CTRL_ACK);
	if (!desc) {
		dev_err(dev, "Failed to prepare DMA\n");
		ret = -EIO;
		goto unmap;
	}

	cookie = dmaengine_submit(desc);
	ret = dma_submit_error(cookie);
	if (ret) {
		dev_err(dev, "Failed to do DMA submit (err=%d)\n", ret);
		goto unmap;
	}

	if (dma_sync_wait(chan, cookie) != DMA_COMPLETE) {
		dev_err(dev, "DMA transfer failed\n");
		ret = -EIO;
	}

	dmaengine_terminate_sync(chan);

unmap:
	dma_unmap_single(dma_dev, dma_addr, seg->length, dir);

unlock:
	mutex_unlock(lock);

	return ret;
}

static int scsit_pci_epf_mmio_transfer(struct scsit_pci_epf *scsi_epf,
		struct scsit_pci_epf_segment *seg, enum dma_data_direction dir)
{
	u64 pci_addr = seg->pci_addr;
	u32 length = seg->length;
	void *buf = seg->buf;
	struct pci_epc_map map;
	int ret = -EINVAL;

	/*
	 * MMIO transfers do not strictly need serialization but the locking
	 * keeps the number of in-flight mapping windows bounded.
	 */
	mutex_lock(&scsi_epf->mmio_lock);

	while (length) {
		ret = scsit_pci_epf_mem_map(scsi_epf, pci_addr, length, &map);
		if (ret)
			break;

		switch (dir) {
		case DMA_FROM_DEVICE:
			memcpy_fromio(buf, map.virt_addr, map.pci_size);
			break;
		case DMA_TO_DEVICE:
			memcpy_toio(map.virt_addr, buf, map.pci_size);
			break;
		default:
			ret = -EINVAL;
			goto unlock;
		}

		pci_addr += map.pci_size;
		buf += map.pci_size;
		length -= map.pci_size;

		scsit_pci_epf_mem_unmap(scsi_epf, &map);
	}

unlock:
	mutex_unlock(&scsi_epf->mmio_lock);

	return ret;
}

static inline int scsit_pci_epf_transfer_seg(struct scsit_pci_epf *scsi_epf,
		struct scsit_pci_epf_segment *seg, enum dma_data_direction dir)
{
	/*
	 * dma_map_single() refuses vmalloc-backed buffers because they are
	 * not physically contiguous. With VMAP_STACK (default on arm64),
	 * any caller that passes a kernel-stack address (e.g. the small
	 * inline transfer helpers used for descriptor/avail/used ring
	 * accesses) would hit that check. Fall back to MMIO for those: the
	 * transfers are typically a handful of bytes so DMA setup overhead
	 * would dominate anyway.
	 */
	if (scsi_epf->dma_enabled && !is_vmalloc_addr(seg->buf))
		return scsit_pci_epf_dma_transfer(scsi_epf, seg, dir);

	return scsit_pci_epf_mmio_transfer(scsi_epf, seg, dir);
}

static inline int scsit_pci_epf_transfer(struct scsit_pci_epf_ctrl *ctrl,
					 void *buf, u64 pci_addr, u32 length,
					 enum dma_data_direction dir)
{
	struct scsit_pci_epf_segment seg = {
		.buf = buf,
		.pci_addr = pci_addr,
		.length = length,
	};

	return scsit_pci_epf_transfer_seg(ctrl->scsi_epf, &seg, dir);
}

static int scsit_pci_epf_alloc_irq_vectors(struct scsit_pci_epf_ctrl *ctrl)
{
	ctrl->irq_vectors = kzalloc_objs(struct scsit_pci_epf_irq_vector,
					 ctrl->nr_vqs);
	if (!ctrl->irq_vectors)
		return -ENOMEM;

	mutex_init(&ctrl->irq_lock);

	return 0;
}

static void scsit_pci_epf_free_irq_vectors(struct scsit_pci_epf_ctrl *ctrl)
{
	if (ctrl->irq_vectors) {
		mutex_destroy(&ctrl->irq_lock);
		kfree(ctrl->irq_vectors);
		ctrl->irq_vectors = NULL;
	}
}

static struct scsit_pci_epf_irq_vector *
scsit_pci_epf_find_irq_vector(struct scsit_pci_epf_ctrl *ctrl, u16 vector)
{
	struct scsit_pci_epf_irq_vector *iv;
	int i;

	lockdep_assert_held(&ctrl->irq_lock);

	for (i = 0; i < ctrl->nr_vqs; i++) {
		iv = &ctrl->irq_vectors[i];
		if (iv->ref && iv->vector == vector)
			return iv;
	}

	return NULL;
}

static struct scsit_pci_epf_irq_vector *
scsit_pci_epf_add_irq_vector(struct scsit_pci_epf_ctrl *ctrl, u16 vector)
{
	struct scsit_pci_epf_irq_vector *iv;
	int i;

	mutex_lock(&ctrl->irq_lock);

	iv = scsit_pci_epf_find_irq_vector(ctrl, vector);
	if (iv) {
		iv->ref++;
		goto unlock;
	}

	for (i = 0; i < ctrl->nr_vqs; i++) {
		iv = &ctrl->irq_vectors[i];
		if (!iv->ref)
			break;
	}

	if (WARN_ON_ONCE(!iv))
		goto unlock;

	iv->ref = 1;
	iv->vector = vector;
	iv->nr_irqs = 0;

unlock:
	mutex_unlock(&ctrl->irq_lock);

	return iv;
}

static void scsit_pci_epf_remove_irq_vector(struct scsit_pci_epf_ctrl *ctrl,
					    u16 vector)
{
	struct scsit_pci_epf_irq_vector *iv;

	mutex_lock(&ctrl->irq_lock);

	iv = scsit_pci_epf_find_irq_vector(ctrl, vector);
	if (iv) {
		iv->ref--;
		if (!iv->ref) {
			iv->vector = 0;
			iv->nr_irqs = 0;
		}
	}

	mutex_unlock(&ctrl->irq_lock);
}

static bool scsit_pci_epf_should_raise_irq(struct scsit_pci_epf_ctrl *ctrl,
		struct scsit_pci_epf_vq *vq, bool force)
{
	struct scsit_pci_epf_irq_vector *iv = vq->iv;
	bool ret;

	/* IRQ coalescing for the control queue is not allowed. */
	if (vq->vqid == SCSIT_PCI_EPF_VQ_CTRL)
		return true;

	if (!iv || iv->cd)
		return true;

	if (force) {
		ret = iv->nr_irqs > 0;
	} else {
		iv->nr_irqs++;
		ret = iv->nr_irqs >= ctrl->irq_vector_threshold;
	}
	if (ret)
		iv->nr_irqs = 0;

	return ret;
}

static void scsit_pci_epf_raise_irq(struct scsit_pci_epf_ctrl *ctrl,
		struct scsit_pci_epf_vq *vq, bool force)
{
	struct scsit_pci_epf *scsi_epf = ctrl->scsi_epf;
	struct pci_epf *epf = scsi_epf->epf;
	int ret = 0;

	if (!test_bit(SCSIT_PCI_EPF_VQ_LIVE, &vq->flags) ||
	    !test_bit(SCSIT_PCI_EPF_VQ_IRQ_ENABLED, &vq->flags))
		return;

	mutex_lock(&ctrl->irq_lock);

	if (!scsit_pci_epf_should_raise_irq(ctrl, vq, force))
		goto unlock;

	/*
	 * Set the ISR bit corresponding to a queue interrupt. The host will
	 * clear it when reading the ISR register. We also raise the actual
	 * MSI / MSI-X / INTx so the host sees an event.
	 */
	scsit_pci_epf_bar_write8(ctrl, VIRTIO_PCI_ISR,
		scsit_pci_epf_bar_read8(ctrl, VIRTIO_PCI_ISR) |
		VIRTIO_PCI_ISR_QUEUE);

	switch (scsi_epf->irq_type) {
	case PCI_IRQ_MSIX:
	case PCI_IRQ_MSI:
		ret = pci_epc_raise_irq(epf->epc, epf->func_no, epf->vfunc_no,
					scsi_epf->irq_type, vq->vector + 1);
		if (!ret || !scsi_epf->epc_features->intx_capable)
			break;
		fallthrough;
	case PCI_IRQ_INTX:
		ret = pci_epc_raise_irq(epf->epc, epf->func_no, epf->vfunc_no,
					PCI_IRQ_INTX, 0);
		break;
	default:
		WARN_ON_ONCE(1);
		ret = -EINVAL;
		break;
	}

	if (ret)
		dev_err_ratelimited(ctrl->dev,
				    "VQ[%u]: Failed to raise IRQ (err=%d)\n",
				    vq->vqid, ret);

unlock:
	mutex_unlock(&ctrl->irq_lock);
}

static struct scsit_pci_epf_cmd *
scsit_pci_epf_alloc_cmd(struct scsit_pci_epf_vq *vq)
{
	struct scsit_pci_epf_ctrl *ctrl = vq->ctrl;
	struct scsit_pci_epf_cmd *cmd;

	cmd = mempool_alloc(&ctrl->cmd_pool, GFP_KERNEL);
	if (unlikely(!cmd))
		return NULL;

	memset(cmd, 0, sizeof(*cmd));
	cmd->ctrl = ctrl;
	cmd->vq = vq;
	INIT_LIST_HEAD(&cmd->link);
	cmd->dma_dir = DMA_NONE;
	INIT_WORK(&cmd->work, scsit_pci_epf_exec_cmd_work);
	init_completion(&cmd->done);

	atomic_inc(&ctrl->n_inflight);

	return cmd;
}

static int scsit_pci_epf_alloc_cmd_data_segs(struct scsit_pci_epf_cmd *cmd,
					     int nsegs)
{
	struct scsit_pci_epf_segment *segs;
	int nr_segs = cmd->nr_data_segs + nsegs;

	segs = krealloc(cmd->data_segs,
			nr_segs * sizeof(struct scsit_pci_epf_segment),
			GFP_KERNEL | __GFP_ZERO);
	if (!segs)
		return -ENOMEM;

	cmd->nr_data_segs = nr_segs;
	cmd->data_segs = segs;

	return 0;
}

static void scsit_pci_epf_free_cmd(struct scsit_pci_epf_cmd *cmd)
{
	struct scsit_pci_epf_ctrl *ctrl = cmd->ctrl;
	int i;

	if (cmd->data_segs) {
		for (i = 0; i < cmd->nr_data_segs; i++)
			kfree(cmd->data_segs[i].buf);
		if (cmd->data_segs != &cmd->data_seg)
			kfree(cmd->data_segs);
	}
	if (cmd->data_sgt.nents > 1)
		sg_free_table(&cmd->data_sgt);
	mempool_free(cmd, &ctrl->cmd_pool);

	if (atomic_dec_and_test(&ctrl->n_inflight))
		wake_up(&ctrl->inflight_wq);
}

static void scsit_pci_epf_release_completed_cmd(struct scsit_pci_epf_cmd *cmd)
{
	if (cmd->tcm_owned)
		target_put_sess_cmd(&cmd->se_cmd);
	else
		scsit_pci_epf_free_cmd(cmd);
}

static int scsit_pci_epf_transfer_cmd_data(struct scsit_pci_epf_cmd *cmd)
{
	struct scsit_pci_epf *scsi_epf = cmd->ctrl->scsi_epf;
	struct se_cmd *se_cmd = &cmd->se_cmd;
	struct scsit_pci_epf_segment *seg = &cmd->data_segs[0];
	enum dma_data_direction xfer_dir;
	u32 xfer_len;
	int i, ret;

	switch (cmd->dma_dir) {
	case DMA_FROM_DEVICE:
		xfer_dir = DMA_TO_DEVICE;
		break;
	case DMA_TO_DEVICE:
		xfer_dir = DMA_FROM_DEVICE;
		break;
	default:
		xfer_dir = DMA_NONE;
		break;
	}

	xfer_len = se_cmd->data_length;
	if (cmd->dma_dir == DMA_FROM_DEVICE &&
	    (se_cmd->se_cmd_flags & SCF_UNDERFLOW_BIT))
		xfer_len -= se_cmd->residual_count;

	for (i = 0; i < cmd->nr_data_segs; i++, seg++) {
		u32 seg_len = seg->length;

		if (xfer_dir == DMA_TO_DEVICE) {
			/* READ: never push more than the produced bytes. */
			if (!xfer_len)
				break;
			seg_len = min(seg_len, xfer_len);
			xfer_len -= seg_len;
		}

		ret = scsit_pci_epf_transfer(cmd->ctrl, seg->buf,
					     seg->pci_addr, seg_len, xfer_dir);
		if (ret) {
			dev_err(scsi_epf->ctrl.dev,
				"transfer_seg %d/%u failed (err=%d, len=%u)\n",
				i, cmd->nr_data_segs, ret, seg_len);
			return ret;
		}
	}

	return 0;
}

static int scsit_pci_epf_read_desc(struct scsit_pci_epf_vq *vq,
				   u16 idx, struct vring_desc *desc)
{
	void __iomem *p;

	if (idx >= vq->depth)
		return -EINVAL;

	p = vq->desc_map.virt_addr + (size_t)idx * sizeof(struct vring_desc);
	desc->addr = cpu_to_le64(readq(p + offsetof(struct vring_desc, addr)));
	desc->len = cpu_to_le32(readl(p + offsetof(struct vring_desc, len)));
	desc->flags = cpu_to_le16(readw(p + offsetof(struct vring_desc, flags)));
	desc->next = cpu_to_le16(readw(p + offsetof(struct vring_desc, next)));

	return 0;
}

static int scsit_pci_epf_read_indirect(struct scsit_pci_epf_ctrl *ctrl,
				       const struct vring_desc *parent,
				       struct vring_desc **out_descs,
				       unsigned int *out_n)
{
	struct vring_desc *descs;
	u32 len = le32_to_cpu(parent->len);
	unsigned int n;
	int ret;

	if (len % sizeof(struct vring_desc) || !len)
		return -EINVAL;

	n = len / sizeof(struct vring_desc);
	descs = kmalloc_array(n, sizeof(*descs), GFP_KERNEL);
	if (!descs)
		return -ENOMEM;

	ret = scsit_pci_epf_transfer(ctrl, descs, le64_to_cpu(parent->addr),
				     len, DMA_FROM_DEVICE);
	if (ret) {
		kfree(descs);
		return ret;
	}

	*out_descs = descs;
	*out_n = n;
	return 0;
}

struct scsit_pci_epf_chain {
	struct vring_desc	*ro;
	unsigned int		nr_ro;
	u32			ro_len;
	struct vring_desc	*wo;
	unsigned int		nr_wo;
	u32			wo_len;

	struct vring_desc	*indirect;
};

static void scsit_pci_epf_chain_free(struct scsit_pci_epf_chain *c)
{
	kfree(c->ro);
	c->ro = NULL;
	kfree(c->wo);
	c->wo = NULL;
	kfree(c->indirect);
	c->indirect = NULL;
}

static int scsit_pci_epf_walk_chain(struct scsit_pci_epf_ctrl *ctrl,
				    struct scsit_pci_epf_vq *vq,
				    u16 head_idx,
				    struct scsit_pci_epf_chain *out)
{
	struct vring_desc desc, *descs = NULL;
	unsigned int n = 0, i = 0, cap_ro = 8, cap_wo = 8;
	bool in_indirect = false;
	int ret;

	memset(out, 0, sizeof(*out));

	out->ro = kmalloc_array(cap_ro, sizeof(*out->ro), GFP_KERNEL);
	out->wo = kmalloc_array(cap_wo, sizeof(*out->wo), GFP_KERNEL);
	if (!out->ro || !out->wo) {
		ret = -ENOMEM;
		goto err;
	}

	ret = scsit_pci_epf_read_desc(vq, head_idx, &desc);
	if (ret)
		goto err;

	/* Reject obviously-invalid head descriptors */
	if (!le64_to_cpu(desc.addr) && !le32_to_cpu(desc.len) &&
	    !le16_to_cpu(desc.flags)) {
		ret = -EINVAL;
		goto err;
	}

	for (;;) {
		u16 flags = le16_to_cpu(desc.flags);

		if (!in_indirect && (flags & VRING_DESC_F_INDIRECT)) {
			ret = scsit_pci_epf_read_indirect(ctrl, &desc,
							  &descs, &n);
			if (ret)
				goto err;
			out->indirect = descs;
			in_indirect = true;
			i = 0;
			desc = descs[0];
			continue;
		}

		if (flags & VRING_DESC_F_WRITE) {
			if (out->nr_wo == cap_wo) {
				cap_wo *= 2;
				out->wo = krealloc_array(out->wo, cap_wo,
						sizeof(*out->wo), GFP_KERNEL);
				if (!out->wo) {
					ret = -ENOMEM;
					goto err;
				}
			}
			out->wo[out->nr_wo++] = desc;
			out->wo_len += le32_to_cpu(desc.len);
		} else {
			/* Read-only descriptors must precede write-only. */
			if (out->nr_wo) {
				ret = -EINVAL;
				goto err;
			}
			if (out->nr_ro == cap_ro) {
				cap_ro *= 2;
				out->ro = krealloc_array(out->ro, cap_ro,
						sizeof(*out->ro), GFP_KERNEL);
				if (!out->ro) {
					ret = -ENOMEM;
					goto err;
				}
			}
			out->ro[out->nr_ro++] = desc;
			out->ro_len += le32_to_cpu(desc.len);
		}

		if (!(flags & VRING_DESC_F_NEXT))
			break;

		if (in_indirect) {
			i = le16_to_cpu(desc.next);
			if (i >= n) {
				ret = -EINVAL;
				goto err;
			}
			desc = descs[i];
		} else {
			u16 next = le16_to_cpu(desc.next);

			ret = scsit_pci_epf_read_desc(vq, next, &desc);
			if (ret)
				goto err;
		}
	}

	return 0;

err:
	scsit_pci_epf_chain_free(out);
	return ret;
}

static int scsit_pci_epf_chain_to_segs(struct scsit_pci_epf_cmd *cmd,
				       const struct vring_desc *descs,
				       unsigned int n)
{
	struct scsit_pci_epf_segment *seg;
	unsigned int i;
	int ret;

	if (n == 0)
		return 0;

	if (n == 1) {
		cmd->nr_data_segs = 1;
		cmd->data_segs = &cmd->data_seg;
		seg = &cmd->data_segs[0];
		seg->pci_addr = le64_to_cpu(descs[0].addr);
		seg->length = le32_to_cpu(descs[0].len);
		return 0;
	}

	ret = scsit_pci_epf_alloc_cmd_data_segs(cmd, n);
	if (ret)
		return ret;

	for (i = 0; i < n; i++) {
		seg = &cmd->data_segs[i];
		seg->pci_addr = le64_to_cpu(descs[i].addr);
		seg->length = le32_to_cpu(descs[i].len);
	}

	return 0;
}

static int scsit_pci_epf_alloc_cmd_data_buf(struct scsit_pci_epf_cmd *cmd)
{
	struct scsit_pci_epf_ctrl *ctrl = cmd->ctrl;
	struct scsit_pci_epf_segment *seg;
	struct scatterlist *sg;
	int ret, i;

	if (cmd->se_cmd.data_length > ctrl->mdts)
		return -EINVAL;

	if (cmd->nr_data_segs == 1) {
		sg_init_table(&cmd->data_sgl, 1);
		cmd->data_sgt.sgl = &cmd->data_sgl;
		cmd->data_sgt.nents = 1;
		cmd->data_sgt.orig_nents = 1;
	} else {
		ret = sg_alloc_table(&cmd->data_sgt, cmd->nr_data_segs,
				     GFP_KERNEL);
		if (ret)
			return ret;
	}

	for_each_sgtable_sg(&cmd->data_sgt, sg, i) {
		seg = &cmd->data_segs[i];
		seg->buf = kmalloc(seg->length, GFP_KERNEL);
		if (!seg->buf)
			return -ENOMEM;
		sg_set_buf(sg, seg->buf, seg->length);
	}

	return 0;
}

static void scsit_pci_epf_post_used(struct scsit_pci_epf_vq *vq,
				    u16 head_idx, u32 len)
{
	void __iomem *ring = vq->used_map.virt_addr +
			     offsetof(struct vring_used, ring) +
			     (size_t)(vq->next_used_idx & (vq->depth - 1)) *
			     sizeof(struct vring_used_elem);

	writel(head_idx, ring + offsetof(struct vring_used_elem, id));
	writel(len, ring + offsetof(struct vring_used_elem, len));

	vq->next_used_idx++;
	writew(vq->next_used_idx,
	       vq->used_map.virt_addr + offsetof(struct vring_used, idx));
}

static void scsit_pci_epf_complete_cmd(struct scsit_pci_epf_cmd *cmd)
{
	struct scsit_pci_epf_vq *vq = cmd->vq;
	unsigned long flags;

	spin_lock_irqsave(&vq->lock, flags);
	/*
	 * Guard against a double completion queuing the same cmd twice, which
	 * would corrupt complete_list and publish the head_idx to the used
	 * ring more than once.
	 */
	if (WARN_ON_ONCE(!list_empty(&cmd->link))) {
		spin_unlock_irqrestore(&vq->lock, flags);
		return;
	}
	list_add_tail(&cmd->link, &vq->complete_list);
	queue_delayed_work(system_highpri_wq, &vq->used_work, 0);
	spin_unlock_irqrestore(&vq->lock, flags);
}

static void scsit_pci_epf_drain_vq(struct scsit_pci_epf_vq *vq)
{
	struct scsit_pci_epf_cmd *cmd, *tmp;
	unsigned long flags;
	LIST_HEAD(drain);

	spin_lock_irqsave(&vq->lock, flags);
	list_splice_init(&vq->complete_list, &drain);
	spin_unlock_irqrestore(&vq->lock, flags);

	list_for_each_entry_safe(cmd, tmp, &drain, link) {
		list_del_init(&cmd->link);
		scsit_pci_epf_release_completed_cmd(cmd);
	}
}

static char *scsit_pci_epf_get_fabric_wwn(struct se_portal_group *se_tpg)
{
	struct scsit_pci_epf_tpg *tpg = to_scsit_pci_epf_tpg(se_tpg);

	return &tpg->wwn->name[0];
}

static u16 scsit_pci_epf_get_tag(struct se_portal_group *se_tpg)
{
	struct scsit_pci_epf_tpg *tpg = to_scsit_pci_epf_tpg(se_tpg);

	return tpg->tpgt;
}

static int scsit_pci_epf_check_demo_mode(struct se_portal_group *se_tpg)
{
	return 1;
}

static int scsit_pci_epf_check_prot_fabric_only(struct se_portal_group *se_tpg)
{
	struct scsit_pci_epf_tpg *tpg = to_scsit_pci_epf_tpg(se_tpg);

	return tpg->prot_type;
}

static u32 scsit_pci_epf_sess_get_index(struct se_session *se_sess)
{
	return 0;
}

static int scsit_pci_epf_check_stop_free(struct se_cmd *se_cmd)
{
	return transport_generic_free_cmd(se_cmd, 0);
}

static void scsit_pci_epf_release_cmd(struct se_cmd *se_cmd)
{
	struct scsit_pci_epf_cmd *cmd = to_scsit_pci_epf_cmd(se_cmd);

	scsit_pci_epf_free_cmd(cmd);
}

static int scsit_pci_epf_get_cmd_state(struct se_cmd *se_cmd)
{
	return 0;
}

static int scsit_pci_epf_write_pending(struct se_cmd *se_cmd)
{
	struct scsit_pci_epf_cmd *cmd = to_scsit_pci_epf_cmd(se_cmd);
	int ret;

	/*
	 * Pull the WRITE data from the host into our local SG list before the
	 * target backend executes the CDB. Once the data is staged, kick the
	 * backend.
	 */
	ret = scsit_pci_epf_transfer_cmd_data(cmd);
	if (ret) {
		dev_err(cmd->ctrl->dev,
			"WRITE: data transfer failed (err=%d)\n", ret);
		cmd->resp.response = VIRTIO_SCSI_S_FAILURE;
		complete(&cmd->done);
		return ret;
	}

	target_execute_cmd(se_cmd);
	return 0;
}

static void scsit_pci_epf_fill_resp(struct scsit_pci_epf_cmd *cmd, u8 scsi_status)
{
	struct se_cmd *se_cmd = &cmd->se_cmd;
	struct virtio_scsi_cmd_resp *resp = &cmd->resp;

	memset(resp, 0, sizeof(*resp));
	resp->response = VIRTIO_SCSI_S_OK;
	resp->status = scsi_status;
	resp->status_qualifier = 0;

	if ((se_cmd->se_cmd_flags & SCF_OVERFLOW_BIT) ||
	    (se_cmd->se_cmd_flags & SCF_UNDERFLOW_BIT))
		resp->resid = cpu_to_le32(se_cmd->residual_count);

	if (se_cmd->sense_buffer &&
	    ((se_cmd->se_cmd_flags & SCF_TRANSPORT_TASK_SENSE) ||
	     (se_cmd->se_cmd_flags & SCF_EMULATED_TASK_SENSE))) {
		u32 sense_len = min_t(u32, SCSIT_PCI_EPF_SENSE_SIZE,
				      TRANSPORT_SENSE_BUFFER);

		memcpy(resp->sense, se_cmd->sense_buffer, sense_len);
		resp->sense_len = cpu_to_le32(sense_len);
		resp->status = SAM_STAT_CHECK_CONDITION;
	}
}

static int scsit_pci_epf_queue_data_in(struct se_cmd *se_cmd)
{
	struct scsit_pci_epf_cmd *cmd = to_scsit_pci_epf_cmd(se_cmd);
	int ret;

	/* Push READ data to the host before the response header. */
	ret = scsit_pci_epf_transfer_cmd_data(cmd);
	if (ret) {
		dev_err(cmd->ctrl->dev,
			"READ: data transfer failed (err=%d)\n", ret);
		cmd->resp.response = VIRTIO_SCSI_S_FAILURE;
		goto queue;
	}

	scsit_pci_epf_fill_resp(cmd, SAM_STAT_GOOD);

queue:
	scsit_pci_epf_complete_cmd(cmd);
	return 0;
}

static int scsit_pci_epf_queue_status(struct se_cmd *se_cmd)
{
	struct scsit_pci_epf_cmd *cmd = to_scsit_pci_epf_cmd(se_cmd);

	scsit_pci_epf_fill_resp(cmd, se_cmd->scsi_status);
	scsit_pci_epf_complete_cmd(cmd);
	return 0;
}

static void scsit_pci_epf_queue_tm_rsp(struct se_cmd *se_cmd)
{
	struct scsit_pci_epf_cmd *cmd = to_scsit_pci_epf_cmd(se_cmd);

	cmd->tmf_resp.response =
		(se_cmd->se_tmr_req->response == TMR_FUNCTION_COMPLETE) ?
			VIRTIO_SCSI_S_FUNCTION_SUCCEEDED :
			VIRTIO_SCSI_S_FUNCTION_REJECTED;
	complete(&cmd->done);
}

static void scsit_pci_epf_aborted_task(struct se_cmd *se_cmd)
{
}

/*
 * Decode a virtio-scsi 8-byte LUN field into a 64-bit "unpacked" LUN as
 * understood by LIO.
 */
static u64 scsit_pci_epf_parse_lun(const u8 lun[8])
{
	if (lun[0] != 1)
		return ~0ULL;
	return ((u64)(lun[2] & 0x3f) << 8) | lun[3];
}

static void scsit_pci_epf_exec_cmd_work(struct work_struct *work)
{
	struct scsit_pci_epf_cmd *cmd =
		container_of(work, struct scsit_pci_epf_cmd, work);
	struct scsit_pci_epf_ctrl *ctrl = cmd->ctrl;
	struct se_cmd *se_cmd = &cmd->se_cmd;
	struct scsit_pci_epf_nexus *nexus;
	u64 lun;
	int ret;

	if (!ctrl->link_up || !ctrl->tpg) {
		cmd->resp.response = VIRTIO_SCSI_S_TRANSPORT_FAILURE;
		goto fail;
	}

	nexus = ctrl->tpg->nexus;
	if (!nexus || !nexus->se_sess) {
		cmd->resp.response = VIRTIO_SCSI_S_NEXUS_FAILURE;
		goto fail;
	}

	lun = scsit_pci_epf_parse_lun(cmd->req.lun);
	if (lun == ~0ULL) {
		dev_warn_ratelimited(ctrl->dev,
			"VQ[%u]: bad LUN (cdb[0]=0x%02x tag=0x%llx)\n",
			cmd->vq->vqid, cmd->req.cdb[0],
			le64_to_cpu(cmd->req.tag));
		cmd->resp.response = VIRTIO_SCSI_S_INCORRECT_LUN;
		goto fail;
	}

	se_cmd->tag = le64_to_cpu(cmd->req.tag);

	if (cmd->dma_dir != DMA_NONE) {
		ret = scsit_pci_epf_alloc_cmd_data_buf(cmd);
		if (ret) {
			cmd->resp.response = VIRTIO_SCSI_S_FAILURE;
			goto fail;
		}
	}

	ret = target_init_cmd(se_cmd, nexus->se_sess, cmd->sense_buf, lun,
			      se_cmd->data_length, TCM_SIMPLE_TAG,
			      cmd->dma_dir, TARGET_SCF_ACK_KREF);
	if (ret) {
		if (ret == TCM_NON_EXISTENT_LUN)
			cmd->resp.response = VIRTIO_SCSI_S_BAD_TARGET;
		else
			cmd->resp.response = VIRTIO_SCSI_S_FAILURE;
		goto fail;
	}

	cmd->tcm_owned = true;

	ret = target_submit_prep(se_cmd, cmd->req.cdb,
				 cmd->dma_dir != DMA_NONE ?
					cmd->data_sgt.sgl : NULL,
				 cmd->dma_dir != DMA_NONE ?
					cmd->data_sgt.nents : 0,
				 NULL, 0, NULL, 0, GFP_KERNEL);
	if (ret)
		return;

	target_submit(se_cmd);
	return;

fail:
	scsit_pci_epf_complete_cmd(cmd);
}

static int scsit_pci_epf_post_response(struct scsit_pci_epf_cmd *cmd)
{
	struct scsit_pci_epf_ctrl *ctrl = cmd->ctrl;
	struct scsit_pci_epf_vq *vq = cmd->vq;
	u32 written;
	int ret;

	if (!cmd->resp_pci_addr || !cmd->resp_len)
		goto done;

	if (cmd->is_tmr) {
		ret = scsit_pci_epf_transfer(ctrl, &cmd->tmf_resp,
				cmd->resp_pci_addr,
				min_t(u32, cmd->resp_len,
				      sizeof(cmd->tmf_resp)),
				DMA_TO_DEVICE);
	} else {
		ret = scsit_pci_epf_transfer(ctrl, &cmd->resp,
				cmd->resp_pci_addr,
				min_t(u32, cmd->resp_len, sizeof(cmd->resp)),
				DMA_TO_DEVICE);
	}
	if (ret) {
		dev_err(ctrl->dev,
			"VQ[%u]: failed to write response (err=%d)\n",
			vq->vqid, ret);
		return ret;
	}

done:
	if (cmd->dma_dir == DMA_FROM_DEVICE) {
		struct se_cmd *se_cmd = &cmd->se_cmd;
		u32 data_len = se_cmd->data_length;

		if (se_cmd->se_cmd_flags & SCF_UNDERFLOW_BIT)
			data_len -= se_cmd->residual_count;
		written = cmd->resp_len + data_len;
	} else
		written = cmd->resp_len;

	scsit_pci_epf_post_used(vq, cmd->head_idx, written);
	return 0;
}

static void scsit_pci_epf_used_work(struct work_struct *work)
{
	struct scsit_pci_epf_vq *vq =
		container_of(work, struct scsit_pci_epf_vq, used_work.work);
	struct scsit_pci_epf_ctrl *ctrl = vq->ctrl;
	struct scsit_pci_epf_cmd *cmd;
	unsigned long flags;
	bool live;
	int ret, n = 0;

	for (;;) {
		spin_lock_irqsave(&vq->lock, flags);
		cmd = list_first_entry_or_null(&vq->complete_list,
					       struct scsit_pci_epf_cmd, link);
		if (cmd)
			list_del_init(&cmd->link);
		spin_unlock_irqrestore(&vq->lock, flags);

		if (!cmd)
			break;

		live = test_bit(SCSIT_PCI_EPF_VQ_LIVE, &vq->flags) &&
		       ctrl->link_up;

		if (live) {
			ret = scsit_pci_epf_post_response(cmd);
			if (ret) {
				/* Re-queue and retry shortly. */
				spin_lock_irqsave(&vq->lock, flags);
				list_add(&cmd->link, &vq->complete_list);
				spin_unlock_irqrestore(&vq->lock, flags);
				queue_delayed_work(system_highpri_wq,
					&vq->used_work,
					SCSIT_PCI_EPF_USED_RETRY_INTERVAL);
				return;
			}
		}

		scsit_pci_epf_release_completed_cmd(cmd);

		if (live) {
			scsit_pci_epf_raise_irq(ctrl, vq, false);
			n++;
		}
	}

	if (n)
		scsit_pci_epf_raise_irq(ctrl, vq, true);
}

static u16 scsit_pci_epf_read_avail_idx(struct scsit_pci_epf_vq *vq)
{
	return readw(vq->avail_map.virt_addr +
		     offsetof(struct vring_avail, idx));
}

static u16 scsit_pci_epf_read_avail_head(struct scsit_pci_epf_vq *vq,
					 u16 ring_idx)
{
	unsigned int slot = ring_idx & (vq->depth - 1);

	return readw(vq->avail_map.virt_addr +
		     offsetof(struct vring_avail, ring) +
		     slot * sizeof(__le16));
}

static int scsit_pci_epf_build_request(struct scsit_pci_epf_cmd *cmd,
				       struct scsit_pci_epf_chain *chain)
{
	struct scsit_pci_epf_ctrl *ctrl = cmd->ctrl;
	struct vring_desc *desc;
	struct vring_desc *data_descs;
	u32 req_len;
	int ret;

	if (chain->nr_ro < 1 || chain->nr_wo < 1)
		return -EINVAL;

	/* Read the request header from the first read-only descriptor. */
	desc = &chain->ro[0];
	req_len = le32_to_cpu(desc->len);
	if (req_len < sizeof(struct virtio_scsi_cmd_req))
		return -EINVAL;

	ret = scsit_pci_epf_transfer(ctrl, &cmd->req,
				     le64_to_cpu(desc->addr),
				     sizeof(cmd->req), DMA_FROM_DEVICE);
	if (ret)
		return ret;

	/*
	 * The response header lives in the first write-only descriptor.
	 * Capture its address for later writeback.
	 */
	desc = &chain->wo[0];
	cmd->resp_pci_addr = le64_to_cpu(desc->addr);
	cmd->resp_len = le32_to_cpu(desc->len);

	/*
	 * Determine the data direction. If the chain contains additional RO
	 * descriptors after the request header, this is a host-to-device
	 * write. If it contains additional WO descriptors after the response
	 * header, it's a device-to-host read. Both can be present for a
	 * bidirectional command which we treat as DMA_NONE for now.
	 */
	if (chain->nr_ro > 1 && chain->nr_wo > 1) {
		/* Bidirectional commands are not supported. */
		return -EINVAL;
	} else if (chain->nr_ro > 1) {
		cmd->dma_dir = DMA_TO_DEVICE;
		data_descs = &chain->ro[1];
		ret = scsit_pci_epf_chain_to_segs(cmd, data_descs,
						  chain->nr_ro - 1);
		if (ret)
			return ret;
		cmd->se_cmd.data_length = chain->ro_len - req_len;
	} else if (chain->nr_wo > 1) {
		cmd->dma_dir = DMA_FROM_DEVICE;
		data_descs = &chain->wo[1];
		ret = scsit_pci_epf_chain_to_segs(cmd, data_descs,
						  chain->nr_wo - 1);
		if (ret)
			return ret;
		cmd->se_cmd.data_length = chain->wo_len - cmd->resp_len;
	} else {
		cmd->dma_dir = DMA_NONE;
		cmd->se_cmd.data_length = 0;
	}

	return 0;
}

static int scsit_pci_epf_build_tmr(struct scsit_pci_epf_cmd *cmd,
				   struct scsit_pci_epf_chain *chain)
{
	struct scsit_pci_epf_ctrl *ctrl = cmd->ctrl;
	struct vring_desc *desc;
	int ret;

	if (chain->nr_ro < 1 || chain->nr_wo < 1)
		return -EINVAL;

	desc = &chain->ro[0];
	if (le32_to_cpu(desc->len) < sizeof(cmd->tmf_req))
		return -EINVAL;

	ret = scsit_pci_epf_transfer(ctrl, &cmd->tmf_req,
				     le64_to_cpu(desc->addr),
				     sizeof(cmd->tmf_req), DMA_FROM_DEVICE);
	if (ret)
		return ret;

	desc = &chain->wo[0];
	cmd->resp_pci_addr = le64_to_cpu(desc->addr);
	cmd->resp_len = le32_to_cpu(desc->len);
	cmd->is_tmr = true;
	cmd->dma_dir = DMA_NONE;

	return 0;
}

static void scsit_pci_epf_dispatch_tmr(struct scsit_pci_epf_cmd *cmd)
{
	struct scsit_pci_epf_ctrl *ctrl = cmd->ctrl;
	struct scsit_pci_epf_nexus *nexus;
	struct se_cmd *se_cmd = &cmd->se_cmd;
	u8 tm_type;
	u64 lun;
	int rc;

	nexus = ctrl->tpg ? ctrl->tpg->nexus : NULL;
	if (!nexus || !nexus->se_sess) {
		cmd->tmf_resp.response = VIRTIO_SCSI_S_FUNCTION_REJECTED;
		goto fail;
	}

	switch (le32_to_cpu(cmd->tmf_req.subtype)) {
	case VIRTIO_SCSI_T_TMF_ABORT_TASK:
		tm_type = TMR_ABORT_TASK;
		break;
	case VIRTIO_SCSI_T_TMF_ABORT_TASK_SET:
		tm_type = TMR_ABORT_TASK_SET;
		break;
	case VIRTIO_SCSI_T_TMF_CLEAR_TASK_SET:
		tm_type = TMR_CLEAR_TASK_SET;
		break;
	case VIRTIO_SCSI_T_TMF_I_T_NEXUS_RESET:
		tm_type = TMR_TARGET_WARM_RESET;
		break;
	case VIRTIO_SCSI_T_TMF_LOGICAL_UNIT_RESET:
		tm_type = TMR_LUN_RESET;
		break;
	default:
		cmd->tmf_resp.response = VIRTIO_SCSI_S_FUNCTION_REJECTED;
		goto fail;
	}

	lun = scsit_pci_epf_parse_lun(cmd->tmf_req.lun);
	if (lun == ~0ULL) {
		cmd->tmf_resp.response = VIRTIO_SCSI_S_INCORRECT_LUN;
		goto fail;
	}

	rc = target_submit_tmr(se_cmd, nexus->se_sess, cmd->sense_buf,
			       lun, NULL, tm_type, GFP_KERNEL,
			       le64_to_cpu(cmd->tmf_req.tag),
			       TARGET_SCF_ACK_KREF);
	if (rc < 0) {
		cmd->tmf_resp.response = VIRTIO_SCSI_S_FUNCTION_REJECTED;
		goto fail;
	}

	wait_for_completion(&cmd->done);
	target_put_sess_cmd(se_cmd);

	scsit_pci_epf_complete_cmd(cmd);
	return;

fail:
	scsit_pci_epf_complete_cmd(cmd);
}

static int scsit_pci_epf_process_vq(struct scsit_pci_epf_ctrl *ctrl,
				    struct scsit_pci_epf_vq *vq)
{
	struct scsit_pci_epf_chain chain;
	struct scsit_pci_epf_cmd *cmd;
	u16 avail_idx, head_idx;
	int ret, n = 0;

	if (!test_bit(SCSIT_PCI_EPF_VQ_LIVE, &vq->flags))
		return 0;

	avail_idx = scsit_pci_epf_read_avail_idx(vq);

	if (vq->vqid == SCSIT_PCI_EPF_VQ_CTRL && avail_idx != vq->last_avail_idx)
		dev_warn_ratelimited(ctrl->dev,
			"VQ[CTRL]: unexpected activity (avail_idx=%u last_avail_idx=%u)\n",
			avail_idx, vq->last_avail_idx);

	if ((u16)(avail_idx - vq->last_avail_idx) > vq->depth) {
		dev_warn_ratelimited(ctrl->dev,
			"VQ[%u]: inconsistent ring, avail_idx=%u last_avail_idx=%u depth=%u\n",
			vq->vqid, avail_idx, vq->last_avail_idx, vq->depth);
		return -EIO;
	}

	while (vq->last_avail_idx != avail_idx &&
	       (!ctrl->req_vq_ab || n < ctrl->req_vq_ab)) {
		head_idx = scsit_pci_epf_read_avail_head(vq,
							 vq->last_avail_idx);

		if (head_idx >= vq->depth) {
			dev_warn_ratelimited(ctrl->dev,
				"VQ[%u]: head_idx %u out of range (depth %u)\n",
				vq->vqid, head_idx, vq->depth);
			return -EIO;
		}

		ret = scsit_pci_epf_walk_chain(ctrl, vq, head_idx, &chain);
		if (ret) {
			dev_err_ratelimited(ctrl->dev,
				"VQ[%u]: invalid descriptor chain at head_idx %u (err=%d)\n",
				vq->vqid, head_idx, ret);
			vq->last_avail_idx++;
			continue;
		}

		cmd = scsit_pci_epf_alloc_cmd(vq);
		if (!cmd) {
			scsit_pci_epf_chain_free(&chain);
			break;
		}

		cmd->head_idx = head_idx;

		if (vq->vqid == SCSIT_PCI_EPF_VQ_CTRL)
			ret = scsit_pci_epf_build_tmr(cmd, &chain);
		else
			ret = scsit_pci_epf_build_request(cmd, &chain);

		scsit_pci_epf_chain_free(&chain);

		if (ret) {
			cmd->resp.response = VIRTIO_SCSI_S_FAILURE;
			scsit_pci_epf_complete_cmd(cmd);
		} else if (vq->vqid == SCSIT_PCI_EPF_VQ_CTRL) {
			scsit_pci_epf_dispatch_tmr(cmd);
		} else if (vq->vqid == SCSIT_PCI_EPF_VQ_EVENT) {
			/* Event queue buffers are not actively polled. */
			scsit_pci_epf_free_cmd(cmd);
		} else {
			queue_work_on(WORK_CPU_UNBOUND, vq->cmd_wq, &cmd->work);
		}

		vq->last_avail_idx++;
		n++;
	}

	return n;
}

static void scsit_pci_epf_poll_vqs_work(struct work_struct *work)
{
	struct scsit_pci_epf_ctrl *ctrl =
		container_of(work, struct scsit_pci_epf_ctrl, poll_vqs.work);
	struct scsit_pci_epf_vq *vq;
	unsigned long limit = jiffies;
	unsigned long last = 0;
	int i, nr_active;

	while (ctrl->link_up && ctrl->running) {
		nr_active = 0;
		for (i = 0; i < ctrl->nr_vqs; i++) {
			vq = &ctrl->vqs[i];
			if (i == SCSIT_PCI_EPF_VQ_EVENT)
				continue;
			if (scsit_pci_epf_process_vq(ctrl, vq) > 0)
				nr_active++;
		}

		if (time_is_before_jiffies(limit + secs_to_jiffies(1))) {
			cond_resched();
			limit = jiffies;
			continue;
		}

		if (nr_active) {
			last = jiffies;
			continue;
		}

		if (time_is_before_jiffies(last + SCSIT_PCI_EPF_VQ_POLL_IDLE))
			break;

		cpu_relax();
	}

	schedule_delayed_work(&ctrl->poll_vqs, SCSIT_PCI_EPF_VQ_POLL_INTERVAL);
}

static void scsit_pci_epf_init_vq(struct scsit_pci_epf_ctrl *ctrl,
				  unsigned int qid)
{
	struct scsit_pci_epf_vq *vq = &ctrl->vqs[qid];

	memset(vq, 0, sizeof(*vq));
	vq->ctrl = ctrl;
	vq->vqid = qid;
	spin_lock_init(&vq->lock);
	INIT_LIST_HEAD(&vq->complete_list);
	INIT_DELAYED_WORK(&vq->used_work, scsit_pci_epf_used_work);
}

static int scsit_pci_epf_alloc_vqs(struct scsit_pci_epf_ctrl *ctrl)
{
	unsigned int qid;

	ctrl->vqs = kzalloc_objs(struct scsit_pci_epf_vq, ctrl->nr_vqs);
	if (!ctrl->vqs)
		return -ENOMEM;

	for (qid = 0; qid < ctrl->nr_vqs; qid++)
		scsit_pci_epf_init_vq(ctrl, qid);

	return 0;
}

static void scsit_pci_epf_free_vqs(struct scsit_pci_epf_ctrl *ctrl)
{
	kfree(ctrl->vqs);
	ctrl->vqs = NULL;
}

static int scsit_pci_epf_enable_vq(struct scsit_pci_epf_ctrl *ctrl, u16 qid)
{
	struct scsit_pci_epf_vq *vq;
	int ret;

	if (qid >= ctrl->nr_vqs)
		return -EINVAL;

	vq = &ctrl->vqs[qid];
	if (test_bit(SCSIT_PCI_EPF_VQ_LIVE, &vq->flags))
		return -EBUSY;

	if (!vq->desc_pci_addr || !vq->depth ||
	    vq->depth > SCSIT_PCI_EPF_MAX_QUEUE_DEPTH ||
	    !is_power_of_2(vq->depth))
		return -EINVAL;

	vq->last_avail_idx = 0;
	vq->next_used_idx = 0;

	ret = scsit_pci_epf_map_vq_rings(vq);
	if (ret) {
		dev_err(ctrl->dev, "VQ[%u]: failed to map rings (err=%d)\n",
			qid, ret);
		return ret;
	}

	if (vq->vector != VIRTIO_MSI_NO_VECTOR) {
		vq->iv = scsit_pci_epf_add_irq_vector(ctrl, vq->vector);
		if (!vq->iv) {
			ret = -ENOMEM;
			goto err_rings;
		}
		set_bit(SCSIT_PCI_EPF_VQ_IRQ_ENABLED, &vq->flags);
	}

	vq->cmd_wq = alloc_workqueue("scsi_epf_vq%u", WQ_UNBOUND,
				     min_t(int, vq->depth, WQ_MAX_ACTIVE), qid);
	if (!vq->cmd_wq) {
		ret = -ENOMEM;
		goto err_vec;
	}

	set_bit(SCSIT_PCI_EPF_VQ_LIVE, &vq->flags);

	dev_info(ctrl->dev,
		 "VQ[%u]: depth %u, desc@%llx avail@%llx used@%llx vector %u\n",
		 qid, vq->depth, vq->desc_pci_addr, vq->avail_pci_addr,
		 vq->used_pci_addr, vq->vector);

	return 0;

err_vec:
	if (test_and_clear_bit(SCSIT_PCI_EPF_VQ_IRQ_ENABLED, &vq->flags))
		scsit_pci_epf_remove_irq_vector(ctrl, vq->vector);
err_rings:
	scsit_pci_epf_unmap_vq_rings(vq);
	return ret;
}

static void scsit_pci_epf_set_queue_pfn(struct scsit_pci_epf_ctrl *ctrl,
					u16 qid, u32 pfn)
{
	struct scsit_pci_epf_vq *vq;
	size_t qsize;
	u64 desc_addr, avail_addr, used_addr;

	if (qid >= ctrl->nr_vqs)
		return;

	vq = &ctrl->vqs[qid];

	if (!pfn) {
		/* Driver is tearing down this queue. */
		vq->desc_pci_addr = 0;
		vq->avail_pci_addr = 0;
		vq->used_pci_addr = 0;
		vq->depth = 0;
		return;
	}

	qsize = SCSIT_PCI_EPF_MAX_QUEUE_DEPTH;
	desc_addr  = (u64)pfn << VIRTIO_PCI_QUEUE_ADDR_SHIFT;
	avail_addr = desc_addr + qsize * sizeof(struct vring_desc);
	used_addr  = ALIGN(avail_addr + offsetof(struct vring_avail,
						 ring[qsize]) +
			   sizeof(__le16), VIRTIO_PCI_VRING_ALIGN);

	vq->desc_pci_addr  = desc_addr;
	vq->avail_pci_addr = avail_addr;
	vq->used_pci_addr  = used_addr;
	vq->depth          = qsize;
}

static void scsit_pci_epf_disable_vq(struct scsit_pci_epf_ctrl *ctrl, u16 qid)
{
	struct scsit_pci_epf_vq *vq;

	if (qid >= ctrl->nr_vqs)
		return;

	vq = &ctrl->vqs[qid];
	if (!test_and_clear_bit(SCSIT_PCI_EPF_VQ_LIVE, &vq->flags))
		return;

	cancel_delayed_work_sync(&vq->used_work);
	scsit_pci_epf_drain_vq(vq);

	if (vq->cmd_wq) {
		destroy_workqueue(vq->cmd_wq);
		vq->cmd_wq = NULL;
	}

	if (test_and_clear_bit(SCSIT_PCI_EPF_VQ_IRQ_ENABLED, &vq->flags))
		scsit_pci_epf_remove_irq_vector(ctrl, vq->vector);

	scsit_pci_epf_unmap_vq_rings(vq);
}

static u64 scsit_pci_epf_device_features(void)
{
	u64 features = 0;

	/*
	 * Legacy virtio only negotiates the low 32 bits of the feature
	 * bitmap.
	 */
	features |= 1ULL << VIRTIO_SCSI_F_HOTPLUG;
	features |= 1ULL << VIRTIO_SCSI_F_CHANGE;

	return features;
}

static void scsit_pci_epf_init_devcfg(struct scsit_pci_epf_ctrl *ctrl)
{
	struct virtio_scsi_config *cfg = &ctrl->devcfg;

	memset(cfg, 0, sizeof(*cfg));
	cfg->num_queues = cpu_to_le32(ctrl->nr_vqs - SCSIT_PCI_EPF_VQ_REQUEST_BASE);
	cfg->seg_max = cpu_to_le32(SCSIT_PCI_EPF_MAX_SEGS);
	cfg->max_sectors = cpu_to_le32(ctrl->mdts / 512);
	cfg->cmd_per_lun = cpu_to_le32(ctrl->cmd_per_lun);
	cfg->event_info_size = cpu_to_le32(sizeof(struct virtio_scsi_event));
	cfg->sense_size = cpu_to_le32(SCSIT_PCI_EPF_SENSE_SIZE);
	cfg->cdb_size = cpu_to_le32(SCSIT_PCI_EPF_CDB_SIZE);
	cfg->max_channel = cpu_to_le16(0);
	cfg->max_target = cpu_to_le16(0);
	cfg->max_lun = cpu_to_le32(255);

	/*
	 * Mirror the device-specific config at both legacy offsets: 20 (used
	 * by the host before MSI-X is enabled) and 24 (used afterwards).
	 */
	memcpy(ctrl->bar + VIRTIO_PCI_CONFIG_OFF(0), cfg, sizeof(*cfg));
	memcpy(ctrl->bar + VIRTIO_PCI_CONFIG_OFF(1), cfg, sizeof(*cfg));
}

static int scsit_pci_epf_driver_ok(struct scsit_pci_epf_ctrl *ctrl)
{
	unsigned int i;
	int ret = 0;

	for (i = 0; i < ctrl->nr_vqs; i++) {
		struct scsit_pci_epf_vq *vq = &ctrl->vqs[i];

		if (!vq->desc_pci_addr)
			continue;

		ret = scsit_pci_epf_enable_vq(ctrl, i);
		if (ret) {
			dev_err(ctrl->dev,
				"Failed to enable virtqueue %u (err=%d)\n",
				i, ret);
			goto err;
		}
	}

	ctrl->req_vq_ab = SCSIT_PCI_EPF_VQ_AB;
	ctrl->irq_vector_threshold = SCSIT_PCI_EPF_IV_THRESHOLD;
	ctrl->running = true;

	/* Start polling the request virtqueues. */
	schedule_delayed_work(&ctrl->poll_vqs, 0);

	return 0;

err:
	for (i = 0; i < ctrl->nr_vqs; i++)
		scsit_pci_epf_disable_vq(ctrl, i);
	return ret;
}

static void scsit_pci_epf_driver_stop(struct scsit_pci_epf_ctrl *ctrl)
{
	unsigned int i;

	if (!ctrl->running)
		return;

	ctrl->running = false;
	cancel_delayed_work_sync(&ctrl->poll_vqs);

	for (i = ctrl->nr_vqs; i-- > 0;)
		scsit_pci_epf_disable_vq(ctrl, i);
}

static void scsit_pci_epf_handle_status(struct scsit_pci_epf_ctrl *ctrl,
					u8 new_status)
{
	u8 old_status = ctrl->device_status;

	if (new_status == old_status)
		return;

	ctrl->device_status = new_status;

	if (new_status == 0) {
		scsit_pci_epf_driver_stop(ctrl);
		ctrl->driver_features = 0;
		ctrl->driver_feature_select = 0;
		scsit_pci_epf_init_bar_regs(ctrl);
		return;
	}

	if ((new_status & VIRTIO_CONFIG_S_DRIVER_OK) &&
	    !(old_status & VIRTIO_CONFIG_S_DRIVER_OK)) {
		if (scsit_pci_epf_driver_ok(ctrl)) {
			scsit_pci_epf_bar_write8(ctrl, VIRTIO_PCI_STATUS,
				new_status | VIRTIO_CONFIG_S_FAILED);
			ctrl->device_status |= VIRTIO_CONFIG_S_FAILED;
		}
	}
}

static void scsit_pci_epf_poll_cfg_iter(struct scsit_pci_epf_ctrl *ctrl)
{
	u32 guest_features, queue_pfn;
	u16 queue_sel, queue_vec;
	u8 status;

	if (!ctrl->bar)
		return;

	guest_features = scsit_pci_epf_bar_read32(ctrl,
						  VIRTIO_PCI_GUEST_FEATURES);
	if (guest_features != (u32)ctrl->driver_features) {
		ctrl->driver_features =
			(ctrl->driver_features & ~0xffffffffULL) |
			guest_features;
		ctrl->driver_feature_select = 0;
	}

	queue_sel = scsit_pci_epf_bar_read16(ctrl, VIRTIO_PCI_QUEUE_SEL);
	if (queue_sel != ctrl->queue_select) {
		ctrl->queue_select = queue_sel;

		if (queue_sel < ctrl->nr_vqs)
			scsit_pci_epf_bar_write16(ctrl, VIRTIO_PCI_QUEUE_NUM,
				SCSIT_PCI_EPF_MAX_QUEUE_DEPTH);
		else
			scsit_pci_epf_bar_write16(ctrl, VIRTIO_PCI_QUEUE_NUM,
						  0);

		/*
		 * Reset QUEUE_PFN to zero so a subsequent host
		 * get_queue_enable() check (read QUEUE_PFN after writing
		 * QUEUE_SEL) sees "queue not yet enabled".
		 */
		scsit_pci_epf_bar_write32(ctrl, VIRTIO_PCI_QUEUE_PFN, 0);
	}

	/*
	 * Capture host-written QUEUE_PFN for the currently-selected queue.
	 * Zero the BAR register immediately after capture so the next
	 * QUEUE_SEL+QUEUE_PFN read sequence by the host doesn't see a
	 * stale value.
	 */
	queue_pfn = scsit_pci_epf_bar_read32(ctrl, VIRTIO_PCI_QUEUE_PFN);
	if (queue_pfn) {
		if (ctrl->queue_select < ctrl->nr_vqs) {
			scsit_pci_epf_set_queue_pfn(ctrl, ctrl->queue_select,
						    queue_pfn);
		}
		scsit_pci_epf_bar_write32(ctrl, VIRTIO_PCI_QUEUE_PFN, 0);
	}

	queue_vec = scsit_pci_epf_bar_read16(ctrl, VIRTIO_MSI_QUEUE_VECTOR);
	if (ctrl->queue_select < ctrl->nr_vqs &&
	    (queue_vec == VIRTIO_MSI_NO_VECTOR ||
	     queue_vec < ctrl->scsi_epf->nr_vectors))
		ctrl->vqs[ctrl->queue_select].vector = queue_vec;

	/* And the config-change MSI-X vector. */
	ctrl->msix_config = scsit_pci_epf_bar_read16(ctrl,
						     VIRTIO_MSI_CONFIG_VECTOR);

	status = scsit_pci_epf_bar_read8(ctrl, VIRTIO_PCI_STATUS);
	scsit_pci_epf_handle_status(ctrl, status);
}

/*
 * Busy-poll loop watching the legacy virtio register block in BAR 0.
 *
 * Legacy virtio multiplexes per-queue state (QUEUE_PFN, QUEUE_NUM, MSI vector)
 * through a single QUEUE_SEL register, and the host's setup_vq() expects to
 * see a "zero PFN" between consecutive queue installations within a window of
 * just a few microseconds. A delayed_work (jiffy granularity) is far too slow
 * to react inside that window, so this dedicated kthread spins reading the
 * BAR.
 */
static int scsit_pci_epf_poll_cfg_thread(void *arg)
{
	struct scsit_pci_epf_ctrl *ctrl = arg;

	while (!kthread_should_stop()) {
		scsit_pci_epf_poll_cfg_iter(ctrl);
		cpu_relax();
		if (need_resched())
			cond_resched();
	}

	return 0;
}

static struct scsit_pci_epf_tpg *
scsit_pci_epf_find_tpg(const char *wwn, u16 tpgt)
{
	struct scsit_pci_epf_tpg *tpg, *found = NULL;

	mutex_lock(&scsit_pci_epf_tpgs_mutex);
	list_for_each_entry(tpg, &scsit_pci_epf_tpgs, list) {
		if (tpg->tpgt == tpgt &&
		    !strcmp(tpg->wwn->name, wwn)) {
			found = tpg;
			break;
		}
	}
	mutex_unlock(&scsit_pci_epf_tpgs_mutex);

	return found;
}

/*
 * Determine the maximum data transfer size to advertise. Start from the
 * driver's own ceiling (SCSIT_PCI_EPF_MDTS) and clamp it down to the smallest
 * hw_max_sectors reported by any backend device mapped into the TPG, so the
 * host never issues a transfer larger than a backend can handle.
 */
static size_t scsit_pci_epf_backend_mdts(struct scsit_pci_epf_tpg *tpg)
{
	struct se_portal_group *se_tpg = &tpg->se_tpg;
	size_t mdts = SCSIT_PCI_EPF_MDTS;
	struct se_lun *lun;

	rcu_read_lock();
	hlist_for_each_entry_rcu(lun, &se_tpg->tpg_lun_hlist, link) {
		struct se_device *dev = rcu_dereference_raw(lun->lun_se_dev);
		size_t dev_max;

		if (!dev)
			continue;

		dev_max = (size_t)dev->dev_attrib.hw_max_sectors *
			  dev->dev_attrib.block_size;
		if (dev_max)
			mdts = min(mdts, dev_max);
	}
	rcu_read_unlock();

	return mdts;
}

/*
 * Number of commands to advertise as queueable per LUN. Start from the
 * virtqueue depth (an in-flight command occupies at least one descriptor, so
 * we can never have more outstanding than the ring holds) and clamp it down
 * to the smallest backend queue depth.
 */
static u32 scsit_pci_epf_backend_cmd_per_lun(struct scsit_pci_epf_tpg *tpg)
{
	struct se_portal_group *se_tpg = &tpg->se_tpg;
	u32 depth = SCSIT_PCI_EPF_MAX_QUEUE_DEPTH;
	struct se_lun *lun;

	rcu_read_lock();
	hlist_for_each_entry_rcu(lun, &se_tpg->tpg_lun_hlist, link) {
		struct se_device *dev = rcu_dereference_raw(lun->lun_se_dev);

		if (dev && dev->dev_attrib.queue_depth)
			depth = min(depth, dev->dev_attrib.queue_depth);
	}
	rcu_read_unlock();

	return depth;
}

static void scsit_pci_epf_init_bar_regs(struct scsit_pci_epf_ctrl *ctrl)
{
	memset(ctrl->bar, 0, SCSIT_PCI_EPF_BAR_REGS_END);

	scsit_pci_epf_bar_write32(ctrl, VIRTIO_PCI_HOST_FEATURES,
				  ctrl->device_features & 0xffffffff);

	scsit_pci_epf_bar_write16(ctrl, VIRTIO_PCI_QUEUE_NUM,
				  SCSIT_PCI_EPF_MAX_QUEUE_DEPTH);

	scsit_pci_epf_bar_write16(ctrl, VIRTIO_MSI_CONFIG_VECTOR,
				  VIRTIO_MSI_NO_VECTOR);
	scsit_pci_epf_bar_write16(ctrl, VIRTIO_MSI_QUEUE_VECTOR,
				  VIRTIO_MSI_NO_VECTOR);

	scsit_pci_epf_init_devcfg(ctrl);
}

static int scsit_pci_epf_create_ctrl(struct scsit_pci_epf *scsi_epf)
{
	struct scsit_pci_epf_ctrl *ctrl = &scsi_epf->ctrl;
	unsigned int nr_req_queues = scsi_epf->nr_req_queues;
	int ret;

	memset(ctrl, 0, sizeof(*ctrl));
	ctrl->dev = &scsi_epf->epf->dev;
	mutex_init(&ctrl->irq_lock);
	ctrl->scsi_epf = scsi_epf;
	INIT_DELAYED_WORK(&ctrl->poll_vqs, scsit_pci_epf_poll_vqs_work);
	atomic_set(&ctrl->n_inflight, 0);
	init_waitqueue_head(&ctrl->inflight_wq);

	if (!nr_req_queues)
		nr_req_queues = 1;
	if (nr_req_queues > SCSIT_PCI_EPF_MAX_REQ_VQS)
		nr_req_queues = SCSIT_PCI_EPF_MAX_REQ_VQS;
	ctrl->nr_vqs = SCSIT_PCI_EPF_VQ_REQUEST_BASE + nr_req_queues;

	ret = mempool_init_kmalloc_pool(&ctrl->cmd_pool,
					SCSIT_PCI_EPF_MAX_QUEUE_DEPTH *
						ctrl->nr_vqs,
					sizeof(struct scsit_pci_epf_cmd));
	if (ret) {
		dev_err(ctrl->dev, "Failed to initialize command mempool\n");
		return ret;
	}

	ctrl->tpg = scsit_pci_epf_find_tpg(scsi_epf->tpg_wwn, scsi_epf->tpgt);
	if (!ctrl->tpg) {
		dev_err(ctrl->dev, "Target portal group %s/tpgt_%u not found\n",
			scsi_epf->tpg_wwn, scsi_epf->tpgt);
		ret = -ENOENT;
		goto err_mempool;
	}

	/* Advertise limits the backend device(s) can satisfy. */
	ctrl->mdts = scsit_pci_epf_backend_mdts(ctrl->tpg);
	ctrl->cmd_per_lun = scsit_pci_epf_backend_cmd_per_lun(ctrl->tpg);

	ret = scsit_pci_epf_alloc_vqs(ctrl);
	if (ret)
		goto err_mempool;

	ret = scsit_pci_epf_alloc_irq_vectors(ctrl);
	if (ret)
		goto err_vqs;

	ctrl->device_features = scsit_pci_epf_device_features();
	ctrl->bar = scsi_epf->reg_bar;

	scsit_pci_epf_init_bar_regs(ctrl);
	ctrl->tpg->ctrl = ctrl;

	dev_info(ctrl->dev,
		 "New PCI ctrl bound to TPG %s/tpgt_%u, %u virtqueues, mdts %zu B\n",
		 scsi_epf->tpg_wwn, scsi_epf->tpgt,
		 ctrl->nr_vqs, ctrl->mdts);

	return 0;

err_vqs:
	scsit_pci_epf_free_vqs(ctrl);
err_mempool:
	mempool_exit(&ctrl->cmd_pool);
	return ret;
}

static void scsit_pci_epf_start_ctrl(struct scsit_pci_epf_ctrl *ctrl)
{
	struct task_struct *task;

	ctrl->link_up = true;

	if (ctrl->poll_cfg_task)
		return;

	task = kthread_run(scsit_pci_epf_poll_cfg_thread, ctrl,
			   "scsit_pci_epf/cfg");
	if (IS_ERR(task)) {
		dev_err(ctrl->dev,
			"Failed to start config-poll kthread (err=%ld)\n",
			PTR_ERR(task));
		return;
	}
	ctrl->poll_cfg_task = task;
}

static void scsit_pci_epf_stop_ctrl(struct scsit_pci_epf_ctrl *ctrl)
{
	ctrl->link_up = false;

	if (ctrl->poll_cfg_task) {
		kthread_stop(ctrl->poll_cfg_task);
		ctrl->poll_cfg_task = NULL;
	}

	scsit_pci_epf_driver_stop(ctrl);
}

static void scsit_pci_epf_destroy_ctrl(struct scsit_pci_epf_ctrl *ctrl)
{
	if (!ctrl->scsi_epf)
		return;

	scsit_pci_epf_stop_ctrl(ctrl);

	if (ctrl->tpg && ctrl->tpg->ctrl == ctrl)
		ctrl->tpg->ctrl = NULL;

	/*
	 * Wait for existing async commands already handed to TCM to drain.
	 */
	while (!wait_event_timeout(ctrl->inflight_wq,
				   !atomic_read(&ctrl->n_inflight),
				   msecs_to_jiffies(5000)))
		dev_warn(ctrl->dev,
			 "waiting for %d in-flight command(s) to drain on teardown\n",
			 atomic_read(&ctrl->n_inflight));

	if (ctrl->vqs) {
		unsigned int i;

		for (i = 0; i < ctrl->nr_vqs; i++)
			cancel_delayed_work_sync(&ctrl->vqs[i].used_work);
	}

	scsit_pci_epf_free_vqs(ctrl);
	scsit_pci_epf_free_irq_vectors(ctrl);

	mempool_exit(&ctrl->cmd_pool);
	ctrl->scsi_epf = NULL;
}

static int scsit_pci_epf_configure_bar(struct scsit_pci_epf *scsi_epf)
{
	struct pci_epf *epf = scsi_epf->epf;
	const struct pci_epc_features *epc_features = scsi_epf->epc_features;
	size_t reg_size, reg_bar_size;
	size_t msix_table_size = 0;

	if (pci_epc_get_first_free_bar(epc_features) != BAR_0) {
		dev_err(&epf->dev, "BAR 0 is not free\n");
		return -ENODEV;
	}

	epf->bar[BAR_0].flags |= PCI_BASE_ADDRESS_MEM_TYPE_64;

	reg_size = SCSIT_PCI_EPF_BAR_REGS_END;
	scsi_epf->msix_table_offset = reg_size;

	if (epc_features->msix_capable) {
		size_t pba_size;

		msix_table_size = PCI_MSIX_ENTRY_SIZE * epf->msix_interrupts;
		pba_size = ALIGN(DIV_ROUND_UP(epf->msix_interrupts, 8), 8);

		reg_size += msix_table_size + pba_size;
	}

	if (epc_features->bar[BAR_0].type == BAR_FIXED) {
		if (reg_size > epc_features->bar[BAR_0].fixed_size) {
			dev_err(&epf->dev,
				"BAR 0 size %llu B too small, need %zu B\n",
				epc_features->bar[BAR_0].fixed_size,
				reg_size);
			return -ENOMEM;
		}
		reg_bar_size = epc_features->bar[BAR_0].fixed_size;
	} else {
		reg_bar_size = ALIGN(reg_size, max(epc_features->align, 4096));
	}

	scsi_epf->reg_bar = pci_epf_alloc_space(epf, reg_bar_size, BAR_0,
						epc_features, PRIMARY_INTERFACE);
	if (!scsi_epf->reg_bar) {
		dev_err(&epf->dev, "Failed to allocate BAR 0\n");
		return -ENOMEM;
	}
	memset(scsi_epf->reg_bar, 0, reg_bar_size);

	return 0;
}

static void scsit_pci_epf_free_bar(struct scsit_pci_epf *scsi_epf)
{
	struct pci_epf *epf = scsi_epf->epf;

	if (!scsi_epf->reg_bar)
		return;

	pci_epf_free_space(epf, scsi_epf->reg_bar, BAR_0, PRIMARY_INTERFACE);
	scsi_epf->reg_bar = NULL;
}

static void scsit_pci_epf_clear_bar(struct scsit_pci_epf *scsi_epf)
{
	struct pci_epf *epf = scsi_epf->epf;

	pci_epc_clear_bar(epf->epc, epf->func_no, epf->vfunc_no,
			  &epf->bar[BAR_0]);
}

static int scsit_pci_epf_init_irq(struct scsit_pci_epf *scsi_epf)
{
	const struct pci_epc_features *epc_features = scsi_epf->epc_features;
	struct pci_epf *epf = scsi_epf->epf;
	int ret;

	if (epc_features->msix_capable && epf->msix_interrupts) {
		ret = pci_epc_set_msix(epf->epc, epf->func_no, epf->vfunc_no,
				       epf->msix_interrupts, BAR_0,
				       scsi_epf->msix_table_offset);
		if (ret) {
			dev_err(&epf->dev, "Failed to configure MSI-X\n");
			return ret;
		}

		scsi_epf->nr_vectors = epf->msix_interrupts;
		scsi_epf->irq_type = PCI_IRQ_MSIX;

		return 0;
	}

	if (epc_features->msi_capable && epf->msi_interrupts) {
		ret = pci_epc_set_msi(epf->epc, epf->func_no, epf->vfunc_no,
				      epf->msi_interrupts);
		if (ret) {
			dev_err(&epf->dev, "Failed to configure MSI\n");
			return ret;
		}

		scsi_epf->nr_vectors = epf->msi_interrupts;
		scsi_epf->irq_type = PCI_IRQ_MSI;

		return 0;
	}

	scsi_epf->nr_vectors = 1;
	scsi_epf->irq_type = PCI_IRQ_INTX;

	return 0;
}

static struct pci_epf_header scsit_pci_epf_pci_header = {
	.vendorid	= PCI_VENDOR_ID_REDHAT_QUMRANET,
	.deviceid	= VIRTIO_TRANS_ID_SCSI,
	.revid		= VIRTIO_PCI_ABI_VERSION,
	.baseclass_code = PCI_BASE_CLASS_STORAGE,
	.subclass_code	= PCI_CLASS_STORAGE_SCSI,
	/* This is an invaild vendor ID, but we set it here
	 * to activate the `force_use_map_api` quirk in the
	 * hosts virtio_pci_legacy_probe() function.
	 * This is because we can't set the VIRTIO_F_ACCESS_PLATFORM feature
	 * with a legacy virtio device.
	 */
	.subsys_vendor_id = 0xFFFF,
	.subsys_id	= VIRTIO_ID_SCSI,
	.interrupt_pin	= PCI_INTERRUPT_INTA,
};

static int scsit_pci_epf_epc_init(struct pci_epf *epf)
{
	struct scsit_pci_epf *scsi_epf = epf_get_drvdata(epf);
	struct scsit_pci_epf_ctrl *ctrl = &scsi_epf->ctrl;
	int ret;

	if (epf->vfunc_no > 0) {
		dev_err(&epf->dev, "Virtual functions are not supported\n");
		return -EINVAL;
	}

	ret = scsit_pci_epf_create_ctrl(scsi_epf);
	if (ret) {
		dev_err(&epf->dev,
			"Failed to create SCSI PCI target controller (err=%d)\n",
			ret);
		return ret;
	}

	scsit_pci_epf_init_dma(scsi_epf);

	ret = pci_epc_write_header(epf->epc, epf->func_no, epf->vfunc_no,
				   epf->header);
	if (ret) {
		dev_err(&epf->dev,
			"Failed to write configuration header (err=%d)\n", ret);
		goto out_destroy_ctrl;
	}

	ret = pci_epc_set_bar(epf->epc, epf->func_no, epf->vfunc_no,
			      &epf->bar[BAR_0]);
	if (ret) {
		dev_err(&epf->dev, "Failed to set BAR 0 (err=%d)\n", ret);
		goto out_destroy_ctrl;
	}

	ret = scsit_pci_epf_init_irq(scsi_epf);
	if (ret)
		goto out_clear_bar;

	if (!scsi_epf->epc_features->linkup_notifier)
		scsit_pci_epf_start_ctrl(ctrl);

	return 0;

out_clear_bar:
	scsit_pci_epf_clear_bar(scsi_epf);
out_destroy_ctrl:
	scsit_pci_epf_destroy_ctrl(&scsi_epf->ctrl);
	return ret;
}

static void scsit_pci_epf_epc_deinit(struct pci_epf *epf)
{
	struct scsit_pci_epf *scsi_epf = epf_get_drvdata(epf);
	struct scsit_pci_epf_ctrl *ctrl = &scsi_epf->ctrl;

	scsit_pci_epf_destroy_ctrl(ctrl);

	scsit_pci_epf_deinit_dma(scsi_epf);
	scsit_pci_epf_clear_bar(scsi_epf);
}

static int scsit_pci_epf_link_up(struct pci_epf *epf)
{
	struct scsit_pci_epf *scsi_epf = epf_get_drvdata(epf);

	scsit_pci_epf_start_ctrl(&scsi_epf->ctrl);
	return 0;
}

static int scsit_pci_epf_link_down(struct pci_epf *epf)
{
	struct scsit_pci_epf *scsi_epf = epf_get_drvdata(epf);

	scsit_pci_epf_stop_ctrl(&scsi_epf->ctrl);
	return 0;
}

static const struct pci_epc_event_ops scsit_pci_epf_event_ops = {
	.epc_init = scsit_pci_epf_epc_init,
	.epc_deinit = scsit_pci_epf_epc_deinit,
	.link_up = scsit_pci_epf_link_up,
	.link_down = scsit_pci_epf_link_down,
};

static int scsit_pci_epf_bind(struct pci_epf *epf)
{
	struct scsit_pci_epf *scsi_epf = epf_get_drvdata(epf);
	const struct pci_epc_features *epc_features;
	struct pci_epc *epc = epf->epc;
	int ret;

	if (WARN_ON_ONCE(!epc))
		return -EINVAL;

	epc_features = pci_epc_get_features(epc, epf->func_no, epf->vfunc_no);
	if (!epc_features) {
		dev_err(&epf->dev, "epc_features not implemented\n");
		return -EOPNOTSUPP;
	}
	scsi_epf->epc_features = epc_features;

	ret = scsit_pci_epf_configure_bar(scsi_epf);
	if (ret)
		return ret;

	return 0;
}

static void scsit_pci_epf_unbind(struct pci_epf *epf)
{
	struct scsit_pci_epf *scsi_epf = epf_get_drvdata(epf);
	struct pci_epc *epc = epf->epc;

	scsit_pci_epf_destroy_ctrl(&scsi_epf->ctrl);

	if (epc->init_complete) {
		scsit_pci_epf_deinit_dma(scsi_epf);
		scsit_pci_epf_clear_bar(scsi_epf);
	}

	scsit_pci_epf_free_bar(scsi_epf);
}

static int scsit_pci_epf_probe(struct pci_epf *epf,
			       const struct pci_epf_device_id *id)
{
	struct scsit_pci_epf *scsi_epf;
	int ret;

	scsi_epf = devm_kzalloc(&epf->dev, sizeof(*scsi_epf), GFP_KERNEL);
	if (!scsi_epf)
		return -ENOMEM;

	ret = devm_mutex_init(&epf->dev, &scsi_epf->mmio_lock);
	if (ret)
		return ret;

	scsi_epf->epf = epf;
	scsi_epf->nr_req_queues = 1;

	epf->event_ops = &scsit_pci_epf_event_ops;
	epf->header = &scsit_pci_epf_pci_header;
	epf_set_drvdata(epf, scsi_epf);

	return 0;
}

#define to_scsi_epf(_group)	\
	container_of(_group, struct scsit_pci_epf, group)

static ssize_t scsit_pci_epf_tpg_wwn_show(struct config_item *item, char *page)
{
	struct config_group *group = to_config_group(item);
	struct scsit_pci_epf *scsi_epf = to_scsi_epf(group);

	return sysfs_emit(page, "%s\n", scsi_epf->tpg_wwn);
}

static ssize_t scsit_pci_epf_tpg_wwn_store(struct config_item *item,
					   const char *page, size_t len)
{
	struct config_group *group = to_config_group(item);
	struct scsit_pci_epf *scsi_epf = to_scsi_epf(group);

	if (scsi_epf->ctrl.scsi_epf)
		return -EBUSY;
	if (!len || len >= sizeof(scsi_epf->tpg_wwn))
		return -EINVAL;

	strscpy(scsi_epf->tpg_wwn, page, sizeof(scsi_epf->tpg_wwn));
	strim(scsi_epf->tpg_wwn);

	return len;
}

CONFIGFS_ATTR(scsit_pci_epf_, tpg_wwn);

static ssize_t scsit_pci_epf_tpgt_show(struct config_item *item, char *page)
{
	struct config_group *group = to_config_group(item);
	struct scsit_pci_epf *scsi_epf = to_scsi_epf(group);

	return sysfs_emit(page, "%u\n", scsi_epf->tpgt);
}

static ssize_t scsit_pci_epf_tpgt_store(struct config_item *item,
					const char *page, size_t len)
{
	struct config_group *group = to_config_group(item);
	struct scsit_pci_epf *scsi_epf = to_scsi_epf(group);
	u16 tpgt;

	if (scsi_epf->ctrl.scsi_epf)
		return -EBUSY;
	if (kstrtou16(page, 0, &tpgt))
		return -EINVAL;

	scsi_epf->tpgt = tpgt;

	return len;
}

CONFIGFS_ATTR(scsit_pci_epf_, tpgt);

static ssize_t scsit_pci_epf_nr_req_queues_show(struct config_item *item,
						char *page)
{
	struct config_group *group = to_config_group(item);
	struct scsit_pci_epf *scsi_epf = to_scsi_epf(group);

	return sysfs_emit(page, "%u\n", scsi_epf->nr_req_queues);
}

static ssize_t scsit_pci_epf_nr_req_queues_store(struct config_item *item,
						 const char *page, size_t len)
{
	struct config_group *group = to_config_group(item);
	struct scsit_pci_epf *scsi_epf = to_scsi_epf(group);
	unsigned long nr;
	int ret;

	if (scsi_epf->ctrl.scsi_epf)
		return -EBUSY;

	ret = kstrtoul(page, 0, &nr);
	if (ret)
		return ret;
	if (nr == 0 || nr > SCSIT_PCI_EPF_MAX_REQ_VQS)
		return -EINVAL;

	scsi_epf->nr_req_queues = nr;
	return len;
}

CONFIGFS_ATTR(scsit_pci_epf_, nr_req_queues);

static struct configfs_attribute *scsit_pci_epf_attrs[] = {
	&scsit_pci_epf_attr_tpg_wwn,
	&scsit_pci_epf_attr_tpgt,
	&scsit_pci_epf_attr_nr_req_queues,
	NULL,
};

static const struct config_item_type scsit_pci_epf_group_type = {
	.ct_attrs	= scsit_pci_epf_attrs,
	.ct_owner	= THIS_MODULE,
};

static struct config_group *scsit_pci_epf_add_cfs(struct pci_epf *epf,
						  struct config_group *group)
{
	struct scsit_pci_epf *scsi_epf = epf_get_drvdata(epf);

	config_group_init_type_name(&scsi_epf->group, "scsi",
				    &scsit_pci_epf_group_type);

	return &scsi_epf->group;
}

static const struct pci_epf_device_id scsit_pci_epf_ids[] = {
	{ .name = "scsit_pci_epf" },
	{},
};

static struct pci_epf_ops scsit_pci_epf_ops = {
	.bind	 = scsit_pci_epf_bind,
	.unbind	 = scsit_pci_epf_unbind,
	.add_cfs = scsit_pci_epf_add_cfs,
};

static struct pci_epf_driver scsit_pci_epf_driver = {
	.driver.name	= "scsit_pci_epf",
	.probe		= scsit_pci_epf_probe,
	.id_table	= scsit_pci_epf_ids,
	.ops		= &scsit_pci_epf_ops,
	.owner		= THIS_MODULE,
};

static int scsit_pci_epf_make_nexus(struct scsit_pci_epf_tpg *tpg,
				    const char *name)
{
	struct scsit_pci_epf_nexus *nexus;

	if (tpg->nexus)
		return -EEXIST;

	nexus = kzalloc_obj(*nexus);
	if (!nexus)
		return -ENOMEM;

	nexus->se_sess = target_setup_session(&tpg->se_tpg, 0, 0,
				TARGET_PROT_NORMAL, name, nexus, NULL);
	if (IS_ERR(nexus->se_sess)) {
		int ret = PTR_ERR(nexus->se_sess);

		kfree(nexus);
		return ret;
	}

	tpg->nexus = nexus;
	return 0;
}

static void scsit_pci_epf_drop_nexus(struct scsit_pci_epf_tpg *tpg)
{
	struct scsit_pci_epf_nexus *nexus = tpg->nexus;

	if (!nexus)
		return;

	target_remove_session(nexus->se_sess);
	kfree(nexus);
	tpg->nexus = NULL;
}

static struct se_portal_group *
scsit_pci_epf_make_tpg(struct se_wwn *wwn, const char *name)
{
	struct scsit_pci_epf_wwn *epf_wwn = to_scsit_pci_epf_wwn(wwn);
	struct scsit_pci_epf_tpg *tpg;
	unsigned long tpgt;
	int ret;

	if (strstr(name, "tpgt_") != name)
		return ERR_PTR(-EINVAL);
	if (kstrtoul(name + 5, 0, &tpgt) || tpgt > USHRT_MAX)
		return ERR_PTR(-EINVAL);

	tpg = kzalloc_obj(*tpg);
	if (!tpg)
		return ERR_PTR(-ENOMEM);

	tpg->wwn = epf_wwn;
	tpg->tpgt = tpgt;
	INIT_LIST_HEAD(&tpg->list);

	ret = core_tpg_register(wwn, &tpg->se_tpg, SCSI_PROTOCOL_SAS);
	if (ret < 0) {
		kfree(tpg);
		return ERR_PTR(ret);
	}

	ret = scsit_pci_epf_make_nexus(tpg, epf_wwn->name);
	if (ret) {
		core_tpg_deregister(&tpg->se_tpg);
		kfree(tpg);
		return ERR_PTR(ret);
	}

	mutex_lock(&scsit_pci_epf_tpgs_mutex);
	list_add_tail(&tpg->list, &scsit_pci_epf_tpgs);
	mutex_unlock(&scsit_pci_epf_tpgs_mutex);

	return &tpg->se_tpg;
}

static void scsit_pci_epf_drop_tpg(struct se_portal_group *se_tpg)
{
	struct scsit_pci_epf_tpg *tpg = to_scsit_pci_epf_tpg(se_tpg);

	mutex_lock(&scsit_pci_epf_tpgs_mutex);
	list_del_init(&tpg->list);
	mutex_unlock(&scsit_pci_epf_tpgs_mutex);

	scsit_pci_epf_drop_nexus(tpg);
	core_tpg_deregister(&tpg->se_tpg);
	kfree(tpg);
}

static struct se_wwn *
scsit_pci_epf_make_wwn(struct target_fabric_configfs *tf,
		       struct config_group *group, const char *name)
{
	struct scsit_pci_epf_wwn *wwn;

	wwn = kzalloc_obj(*wwn);
	if (!wwn)
		return ERR_PTR(-ENOMEM);

	strscpy(wwn->name, name, sizeof(wwn->name));

	return &wwn->se_wwn;
}

static void scsit_pci_epf_drop_wwn(struct se_wwn *se_wwn)
{
	struct scsit_pci_epf_wwn *wwn = to_scsit_pci_epf_wwn(se_wwn);

	kfree(wwn);
}

static const struct target_core_fabric_ops scsit_pci_epf_fabric_ops = {
	.module				= THIS_MODULE,
	.fabric_name			= SCSIT_PCI_EPF_FABRIC_NAME,
	.tpg_get_wwn			= scsit_pci_epf_get_fabric_wwn,
	.tpg_get_tag			= scsit_pci_epf_get_tag,
	.tpg_check_demo_mode		= scsit_pci_epf_check_demo_mode,
	.tpg_check_prot_fabric_only	= scsit_pci_epf_check_prot_fabric_only,
	.check_stop_free		= scsit_pci_epf_check_stop_free,
	.release_cmd			= scsit_pci_epf_release_cmd,
	.sess_get_index			= scsit_pci_epf_sess_get_index,
	.write_pending			= scsit_pci_epf_write_pending,
	.get_cmd_state			= scsit_pci_epf_get_cmd_state,
	.queue_data_in			= scsit_pci_epf_queue_data_in,
	.queue_status			= scsit_pci_epf_queue_status,
	.queue_tm_rsp			= scsit_pci_epf_queue_tm_rsp,
	.aborted_task			= scsit_pci_epf_aborted_task,
	.fabric_make_wwn		= scsit_pci_epf_make_wwn,
	.fabric_drop_wwn		= scsit_pci_epf_drop_wwn,
	.fabric_make_tpg		= scsit_pci_epf_make_tpg,
	.fabric_drop_tpg		= scsit_pci_epf_drop_tpg,
	.default_submit_type		= TARGET_QUEUE_SUBMIT,
	.default_compl_type		= TARGET_QUEUE_COMPL,
};

static int __init scsit_pci_epf_init_module(void)
{
	int ret;

	ret = pci_epf_register_driver(&scsit_pci_epf_driver);
	if (ret)
		return ret;

	ret = target_register_template(&scsit_pci_epf_fabric_ops);
	if (ret) {
		pci_epf_unregister_driver(&scsit_pci_epf_driver);
		return ret;
	}

	return 0;
}

static void __exit scsit_pci_epf_cleanup_module(void)
{
	target_unregister_template(&scsit_pci_epf_fabric_ops);
	pci_epf_unregister_driver(&scsit_pci_epf_driver);
}

module_init(scsit_pci_epf_init_module);
module_exit(scsit_pci_epf_cleanup_module);

MODULE_DESCRIPTION("virtio-scsi PCI Endpoint Function target driver");
MODULE_AUTHOR("Alistair Francis <alistair.francis@wdc.com>");
MODULE_LICENSE("GPL");

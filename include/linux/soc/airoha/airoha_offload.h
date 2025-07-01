/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2025 AIROHA Inc
 * Author: Lorenzo Bianconi <lorenzo@kernel.org>
 */
#ifndef AIROHA_OFFLOAD_H
#define AIROHA_OFFLOAD_H

#define NPU_NUM_CORES		8
#define NPU_NUM_IRQ		6
#define NPU_RX0_DESC_NUM	512
#define NPU_RX1_DESC_NUM	512

/* CTRL */
#define NPU_RX_DMA_DESC_LAST_MASK	BIT(29)
#define NPU_RX_DMA_DESC_LEN_MASK	GENMASK(28, 15)
#define NPU_RX_DMA_DESC_CUR_LEN_MASK	GENMASK(14, 1)
#define NPU_RX_DMA_DESC_DONE_MASK	BIT(0)
/* INFO */
#define NPU_RX_DMA_PKT_COUNT_MASK	GENMASK(31, 28)
#define NPU_RX_DMA_PKT_ID_MASK		GENMASK(28, 26)
#define NPU_RX_DMA_SRC_PORT_MASK	GENMASK(25, 21)
#define NPU_RX_DMA_CRSN_MASK		GENMASK(20, 16)
#define NPU_RX_DMA_FOE_ID_MASK		GENMASK(15, 0)
/* DATA */
#define NPU_RX_DMA_SID_MASK		GENMASK(31, 16)
#define NPU_RX_DMA_FRAG_TYPE_MASK	GENMASK(15, 14)
#define NPU_RX_DMA_PRIORITY_MASK	GENMASK(13, 10)
#define NPU_RX_DMA_RADIO_ID_MASK	GENMASK(9, 6)
#define NPU_RX_DMA_VAP_ID_MASK		GENMASK(5, 2)
#define NPU_RX_DMA_FRAME_TYPE_MASK	GENMASK(1, 0)

struct airoha_npu_rx_dma_desc {
	u32 ctrl;
	u32 info;
	u32 data;
	u32 addr;
	u64 rsv;
} __packed;

/* CTRL */
#define NPU_TX_DMA_DESC_SCHED_MASK	BIT(31)
#define NPU_TX_DMA_DESC_LEN_MASK	GENMASK(30, 18)
#define NPU_TX_DMA_DESC_VEND_LEN_MASK	GENMASK(17, 1)
#define NPU_TX_DMA_DESC_DONE_MASK	BIT(0)

#define NPU_TXWI_LEN	192

struct airoha_npu_tx_dma_desc {
	u32 ctrl;
	u32 addr;
	u64 rsv;
	u8 txwi[NPU_TXWI_LEN];
} __packed;

struct airoha_npu {
#if (IS_BUILTIN(CONFIG_NET_AIROHA_NPU) || IS_MODULE(CONFIG_NET_AIROHA_NPU))
	struct device *dev;
	struct regmap *regmap;

	struct airoha_npu_core {
		struct airoha_npu *npu;
		/* protect concurrent npu memory accesses */
		spinlock_t lock;
		struct work_struct wdt_work;
	} cores[NPU_NUM_CORES];

	int irqs[NPU_NUM_IRQ];

	struct airoha_foe_stats __iomem *stats;

	struct {
		int (*ppe_init)(struct airoha_npu *npu);
		int (*ppe_deinit)(struct airoha_npu *npu);
		int (*ppe_flush_sram_entries)(struct airoha_npu *npu,
					      dma_addr_t foe_addr,
					      int sram_num_entries);
		int (*ppe_foe_commit_entry)(struct airoha_npu *npu,
					    dma_addr_t foe_addr,
					    u32 entry_size, u32 hash,
					    bool ppe2);
		int (*wlan_init_reserved_memory)(struct airoha_npu *npu);
		int (*wlan_set_txrx_reg_addr)(struct airoha_npu *npu,
					      int ifindex, u32 dir,
					      u32 in_counter_addr,
					      u32 out_status_addr,
					      u32 out_counter_addr);
		int (*wlan_set_pcie_port_type)(struct airoha_npu *npu,
					       int ifindex, u32 port_type);
		int (*wlan_set_pcie_addr)(struct airoha_npu *npu, int ifindex,
					  u32 addr);
		int (*wlan_set_desc)(struct airoha_npu *npu, int ifindex,
				     u32 desc);
		int (*wlan_set_tx_ring_pcie_addr)(struct airoha_npu *npu,
						  int ifindex, u32 addr);
		int (*wlan_get_rx_desc_base)(struct airoha_npu *npu,
					     int ifindex, u32 *data);
		int (*wlan_set_tx_buf_space_base)(struct airoha_npu *npu,
						  int ifindex, u32 addr);
		int (*wlan_set_rx_ring_for_txdone)(struct airoha_npu *npu,
						   int ifindex, u32 addr);
		u32 (*wlan_get_queue_addr)(struct airoha_npu *npu, int qid,
					   bool xmit);
		void (*wlan_set_irq_status)(struct airoha_npu *npu, u32 val);
		u32 (*wlan_get_irq_status)(struct airoha_npu *npu, int q);
		void (*wlan_enable_irq)(struct airoha_npu *npu, int q);
		void (*wlan_disable_irq)(struct airoha_npu *npu, int q);
	} ops;
#endif
};

#if (IS_BUILTIN(CONFIG_NET_AIROHA_NPU) || IS_MODULE(CONFIG_NET_AIROHA_NPU))
struct airoha_npu *airoha_npu_get(struct device *dev, dma_addr_t *stats_addr);
void airoha_npu_put(struct airoha_npu *npu);

static inline int airoha_npu_wlan_init_reserved_memory(struct airoha_npu *npu)
{
	return npu->ops.wlan_init_reserved_memory(npu);
}

static inline int airoha_npu_wlan_set_txrx_reg_addr(struct airoha_npu *npu,
						    int ifindex, u32 dir,
						    u32 in_counter_addr,
						    u32 out_status_addr,
						    u32 out_counter_addr)
{
	return npu->ops.wlan_set_txrx_reg_addr(npu, ifindex, dir,
					       in_counter_addr,
					       out_status_addr,
					       out_counter_addr);
}

static inline int airoha_npu_wlan_set_pcie_port_type(struct airoha_npu *npu,
						     int ifindex,
						     u32 port_type)
{
	return npu->ops.wlan_set_pcie_port_type(npu, ifindex, port_type);
}

static inline int airoha_npu_wlan_set_pcie_addr(struct airoha_npu *npu,
						int ifindex, u32 addr)
{
	return npu->ops.wlan_set_pcie_addr(npu, ifindex, addr);
}

static inline int airoha_npu_wlan_set_desc(struct airoha_npu *npu, int ifindex,
					   u32 desc)
{
	return npu->ops.wlan_set_desc(npu, ifindex, desc);
}

static inline int airoha_npu_wlan_set_tx_ring_pcie_addr(struct airoha_npu *npu,
							int ifindex, u32 addr)
{
	return npu->ops.wlan_set_tx_ring_pcie_addr(npu, ifindex, addr);
}

static inline int airoha_npu_wlan_get_rx_desc_base(struct airoha_npu *npu, int ifindex,
						   u32 *data)
{
	return npu->ops.wlan_get_rx_desc_base(npu, ifindex, data);
}

static inline int airoha_npu_wlan_set_tx_buf_space_base(struct airoha_npu *npu,
							int ifindex, u32 addr)
{
	return npu->ops.wlan_set_tx_buf_space_base(npu, ifindex, addr);
}

static inline int airoha_npu_wlan_set_rx_ring_for_txdone(struct airoha_npu *npu,
							 int ifindex, u32 addr)
{
	return npu->ops.wlan_set_rx_ring_for_txdone(npu, ifindex, addr);
}

static inline u32 airoha_npu_wlan_get_queue_addr(struct airoha_npu *npu, int qid,
						 bool xmit)
{
	return npu->ops.wlan_get_queue_addr(npu, qid, xmit);
}

static inline void airoha_npu_wlan_set_irq_status(struct airoha_npu *npu,
						  u32 val)
{
	npu->ops.wlan_set_irq_status(npu, val);
}

static inline u32 airoha_npu_wlan_get_irq_status(struct airoha_npu *npu, int q)
{
	return npu->ops.wlan_get_irq_status(npu, q);
}

static inline void airoha_npu_wlan_enable_irq(struct airoha_npu *npu, int q)
{
	npu->ops.wlan_enable_irq(npu, q);
}

static inline void airoha_npu_wlan_disable_irq(struct airoha_npu *npu, int q)
{
	npu->ops.wlan_disable_irq(npu, q);
}
#else
static inline struct airoha_npu *airoha_npu_get(struct device *dev,
						dma_addr_t *foe_stats_addr)
{
	return NULL;
}

static inline void airoha_npu_put(struct airoha_npu *npu)
{
}

static inline int airoha_npu_wlan_init_reserved_memory(struct airoha_npu *npu)
{
	return -EOPNOTSUPP;
}

static inline int airoha_npu_wlan_set_txrx_reg_addr(struct airoha_npu *npu,
						    int ifindex, u32 dir,
						    u32 in_counter_addr,
						    u32 out_status_addr,
						    u32 out_counter_addr)
{
	return -EOPNOTSUPP;
}

static inline int airoha_npu_wlan_set_pcie_port_type(struct airoha_npu *npu,
						     int ifindex, u32 port_type)
{
	return -EOPNOTSUPP;
}

static inline int airoha_npu_wlan_set_pcie_addr(struct airoha_npu *npu,
						int ifindex, u32 addr)
{
	return -EOPNOTSUPP;
}

static inline int airoha_npu_wlan_set_desc(struct airoha_npu *npu, int ifindex,
					   u32 desc)
{
	return -EOPNOTSUPP;
}

static inline int airoha_npu_wlan_set_tx_ring_pcie_addr(struct airoha_npu *npu,
							int ifindex, u32 addr)
{
	return -EOPNOTSUPP;
}

static inline int airoha_npu_wlan_get_rx_desc_base(struct airoha_npu *npu,
						   int ifindex, u32 *data)
{
	return -EOPNOTSUPP;
}

static inline int airoha_npu_wlan_set_tx_buf_space_base(struct airoha_npu *npu,
							int ifindex, u32 addr)
{
	return -EOPNOTSUPP;
}

static inline int airoha_npu_wlan_set_rx_ring_for_txdone(struct airoha_npu *npu,
							 int ifindex, u32 addr)
{
	return -EOPNOTSUPP;
}

static inline u32 airoha_npu_wlan_get_queue_addr(struct airoha_npu *npu,
						 int qid, bool xmit)
{
	return 0;
}

static inline void airoha_npu_wlan_set_irq_status(struct airoha_npu *npu,
						  u32 val)
{
}

static inline u32 airoha_npu_wlan_get_irq_status(struct airoha_npu *npu,
						 int q)
{
	return 0;
}

static inline void airoha_npu_wlan_enable_irq(struct airoha_npu *npu, int q)
{
}

static inline void airoha_npu_wlan_disable_irq(struct airoha_npu *npu, int q)
{
}
#endif

#endif /* AIROHA_OFFLOAD_H */

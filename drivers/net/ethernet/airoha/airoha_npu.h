/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2025 AIROHA Inc
 * Author: Lorenzo Bianconi <lorenzo@kernel.org>
 */

#define NPU_NUM_CORES		8
#define NPU_NUM_IRQ		6

struct airoha_npu {
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
};

struct airoha_npu *airoha_npu_get(struct device *dev, dma_addr_t *stats_addr);
void airoha_npu_put(struct airoha_npu *npu);

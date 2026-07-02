// SPDX-License-Identifier: GPL-2.0-only
/*
 * ZTE DingHai Rdma driver
 * Copyright (c) 2022-2026, ZTE Corporation
 */

#include "zrdma_hw.h"
#include "zrdma_ctrl.h"
#include "zrdma_mem.h"

static void zxdh_config_tx_regs(struct zxdh_sc_dev *dev)
{
	u32 temp;

	temp = FIELD_PREP(ZXDH_TX_CACHE_ID, 0) |
	       FIELD_PREP(ZXDH_TX_INDICATE_ID, ZXDH_INDICATE_HOST_NOSMMU) |
	       FIELD_PREP(ZXDH_TX_AXI_ID, (ZXDH_AXID_HOST_EP0 + dev->ep_id)) |
	       FIELD_PREP(ZXDH_TX_WAY_PARTITION, 0);

	writel(temp, dev->hw->hw_addr + RDMATX_ACK_SQWQE_PARA_CFG);
	writel(temp, dev->hw->hw_addr + RDMATX_ACK_DDR_PARA_CFG);
	writel(temp, dev->hw->hw_addr + RDMATX_DB_SQWQE_ID_CFG);
	writel(temp, dev->hw->hw_addr + RDMATX_SQWQE_PARA_CFG);
	writel(temp, dev->hw->hw_addr + RDMATX_PAYLOAD_PARA_CFG);

	if (dev->hmc_use_dpu_ddr) {
		temp = FIELD_PREP(ZXDH_TX_CACHE_ID, dev->cache_id) |
		       FIELD_PREP(ZXDH_TX_INDICATE_ID, ZXDH_INDICATE_DPU_DDR) |
		       FIELD_PREP(ZXDH_TX_AXI_ID,
				  (ZXDH_AXID_HOST_EP0 + dev->ep_id)) |
		       FIELD_PREP(ZXDH_TX_WAY_PARTITION, 0);
	} else {
		temp = FIELD_PREP(ZXDH_TX_CACHE_ID, dev->cache_id) |
		       FIELD_PREP(ZXDH_TX_INDICATE_ID,
				  ZXDH_INDICATE_HOST_SMMU) |
		       FIELD_PREP(ZXDH_TX_AXI_ID,
				  (ZXDH_AXID_HOST_EP0 + dev->ep_id)) |
		       FIELD_PREP(ZXDH_TX_WAY_PARTITION, 0);
	}
	writel(temp, dev->hw->hw_addr + C_HMC_MRTE_TX2);
	writel(temp, dev->hw->hw_addr + C_HMC_PBLEMR_TX2);
	writel((ZXDH_AXID_HOST_EP0 + dev->ep_id),
	       dev->hw->hw_addr + RDMATX_HOSTID_CFG);
}

static void zxdh_config_rx_regs(struct zxdh_sc_dev *dev)
{
	u32 temp;

	temp = FIELD_PREP(ZXDH_RX_CACHE_ID, 0) |
	       FIELD_PREP(ZXDH_RX_INDICATE_ID, ZXDH_INDICATE_HOST_NOSMMU) |
	       FIELD_PREP(ZXDH_RX_AXI_ID, (ZXDH_AXID_HOST_EP0 + dev->ep_id)) |
	       FIELD_PREP(ZXDH_RX_WAY_PARTITION, 0);

	writel(temp, dev->hw->hw_addr + RDMARX_PLD_WR_AXIID_RAM);
	writel(temp, dev->hw->hw_addr + RDMARX_RQ_AXI_RAM);
	writel(temp, dev->hw->hw_addr + RDMARX_SRQ_AXI_RAM);
	writel(temp, dev->hw->hw_addr + RDMARX_ACK_RQDB_AXI_RAM);
	writel(temp, dev->hw->hw_addr + RDMARX_CQ_CQE_AXI_INFO_RAM);
	writel(temp, dev->hw->hw_addr + RDMARX_CQ_DBSA_AXI_INFO_RAM);
	writel(dev->hmc_fn_id,
	       dev->hw->hw_addr + RDMARX_MUL_CACHE_CFG_SIDN_RAM);
	writel((ZXDH_AXID_HOST_EP0 + dev->ep_id),
	       dev->hw->hw_addr + RDMARX_MUL_COPY_QPN_INDICATE);
	writel(RDMARX_MAX_MSG_SIZE,
	       dev->hw->hw_addr + RDMARX_VHCA_MAX_SIZE_RAM);
}

static void zxdh_config_io_regs(struct zxdh_sc_dev *dev)
{
	u32 temp0, temp1, temp2;
	struct zxdh_pci_f *rf = container_of(dev, struct zxdh_pci_f, sc_dev);

	temp0 = FIELD_PREP(ZXDH_IOTABLE2_SID, dev->hmc_fn_id);
	writel(temp0, dev->hw->hw_addr + C_RDMAIO_TABLE2);

	temp1 = FIELD_PREP(ZXDH_IOTABLE4_EPID,
			   (ZXDH_HOST_EP0_ID + dev->ep_id)) |
		FIELD_PREP(ZXDH_IOTABLE4_VFID, dev->vf_id) |
		FIELD_PREP(ZXDH_IOTABLE4_PFID, rf->pf_id);
	writel(temp1, dev->hw->hw_addr + C_RDMAIO_TABLE4);

	temp0 = 0x10000;
	writel(temp0, dev->hw->hw_addr + C_RDMAIO_TABLE3);
	for (temp0 = 0; temp0 < 32; temp0++) {
		if (temp0 < ZXDH_RW_PAYLOAD || temp0 == ZXDH_QPC_OBJ_ID) {
			writel(0, dev->hw->hw_addr +
					  (C_RDMAIO_TABLE5_0 + (temp0 * 4)));
		} else {
			temp2 = (rf->ftype == 0) ? 0 : ZXDH_TABLE5_VF_EN;
			writel(temp2, dev->hw->hw_addr + (C_RDMAIO_TABLE5_0 +
							  (temp0 * 4)));
		}
	}

	if (rf->ftype == 0) {
		writel(0, dev->hw->hw_addr + C_RDMAIO_TABLE6_0);
		writel(0, dev->hw->hw_addr + C_RDMAIO_TABLE6_1);
		writel(0, dev->hw->hw_addr + C_RDMAIO_TABLE6_2);
		writel(0, dev->hw->hw_addr + C_RDMAIO_TABLE6_3);
		writel(0, dev->hw->hw_addr + C_RDMAIO_TABLE6_4);
		writel(0, dev->hw->hw_addr + C_RDMAIO_TABLE6_5);
		writel(0, dev->hw->hw_addr + C_RDMAIO_TABLE6_6);
		writel(0, dev->hw->hw_addr + C_RDMAIO_TABLE6_7);
		writel(0, dev->hw->hw_addr + C_RDMAIO_TABLE6_8);
		writel(0, dev->hw->hw_addr + C_RDMAIO_TABLE6_9);
		writel(0, dev->hw->hw_addr + C_RDMAIO_TABLE6_10);
		writel(0, dev->hw->hw_addr + C_RDMAIO_TABLE6_11);
		writel(0, dev->hw->hw_addr + C_RDMAIO_TABLE6_12);
		writel(0, dev->hw->hw_addr + C_RDMAIO_TABLE6_13);
		writel(0, dev->hw->hw_addr + C_RDMAIO_TABLE6_14);
		writel(0, dev->hw->hw_addr + C_RDMAIO_TABLE6_15);

		temp2 = FIELD_PREP(ZXDH_IOTABLE7_PFID, rf->pf_id) |
			FIELD_PREP(ZXDH_IOTABLE7_EPID,
				   (ZXDH_HOST_EP0_ID + rf->ep_id));
		writel(temp2, dev->hw->hw_addr + C_RDMAIO_TABLE7);
	}
}

static void zxdh_config_hw_regs(struct zxdh_sc_dev *dev)
{
	zxdh_config_tx_regs(dev);
	zxdh_config_rx_regs(dev);
	zxdh_config_io_regs(dev);
}

int zxdh_ctrl_init_hw(struct zxdh_pci_f *rf)
{
	struct zxdh_sc_dev *dev = &rf->sc_dev;

	zxdh_config_hw_regs(dev);

	return 0;
}

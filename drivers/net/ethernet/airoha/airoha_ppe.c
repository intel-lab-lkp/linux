// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2025 AIROHA Inc
 * Author: Lorenzo Bianconi <lorenzo@kernel.org>
 */

#include "airoha_regs.h"
#include "airoha_eth.h"

bool airoha_ppe2_is_enabled(struct airoha_eth *eth)
{
	return airoha_fe_rr(eth, REG_PPE_GLO_CFG(1)) & PPE_GLO_CFG_EN_MASK;
}

static void airoha_ppe_hw_init(struct airoha_ppe *ppe)
{
	u32 sram_tb_size, sram_num_entries, dram_num_entries;
	struct airoha_eth *eth = ppe->eth;
	int i;

	sram_tb_size = PPE_SRAM_NUM_ENTRIES * sizeof(struct airoha_foe_entry);
	dram_num_entries = PPE_RAM_NUM_ENTRIES_SHIFT(PPE_DRAM_NUM_ENTRIES);

	for (i = 0; i < PPE_NUM; i++) {
		airoha_fe_wr(eth, REG_PPE_TB_BASE(i),
			     ppe->foe_dma + sram_tb_size);

		airoha_fe_rmw(eth, REG_PPE_BND_AGE0(i),
			      PPE_BIND_AGE0_DELTA_NON_L4 |
			      PPE_BIND_AGE0_DELTA_UDP,
			      FIELD_PREP(PPE_BIND_AGE0_DELTA_NON_L4, 1) |
			      FIELD_PREP(PPE_BIND_AGE0_DELTA_UDP, 12));
		airoha_fe_rmw(eth, REG_PPE_BND_AGE1(i),
			      PPE_BIND_AGE1_DELTA_TCP_FIN |
			      PPE_BIND_AGE1_DELTA_TCP,
			      FIELD_PREP(PPE_BIND_AGE1_DELTA_TCP_FIN, 1) |
			      FIELD_PREP(PPE_BIND_AGE1_DELTA_TCP, 7));

		airoha_fe_rmw(eth, REG_PPE_TB_HASH_CFG(i),
			      PPE_SRAM_TABLE_EN_MASK |
			      PPE_SRAM_HASH1_EN_MASK |
			      PPE_DRAM_TABLE_EN_MASK |
			      PPE_SRAM_HASH0_MODE_MASK |
			      PPE_SRAM_HASH1_MODE_MASK |
			      PPE_DRAM_HASH0_MODE_MASK |
			      PPE_DRAM_HASH1_MODE_MASK,
			      FIELD_PREP(PPE_SRAM_TABLE_EN_MASK, 1) |
			      FIELD_PREP(PPE_SRAM_HASH1_EN_MASK, 1) |
			      FIELD_PREP(PPE_SRAM_HASH1_MODE_MASK, 1) |
			      FIELD_PREP(PPE_DRAM_HASH1_MODE_MASK, 3));

		airoha_fe_rmw(eth, REG_PPE_TB_CFG(i),
			      PPE_TB_CFG_SEARCH_MISS_MASK |
			      PPE_TB_ENTRY_SIZE_MASK,
			      FIELD_PREP(PPE_TB_CFG_SEARCH_MISS_MASK, 3) |
			      FIELD_PREP(PPE_TB_ENTRY_SIZE_MASK, 0));

		airoha_fe_wr(eth, REG_PPE_HASH_SEED(i), PPE_HASH_SEED);
	}

	if (airoha_ppe2_is_enabled(eth)) {
		sram_num_entries =
			PPE_RAM_NUM_ENTRIES_SHIFT(PPE1_SRAM_NUM_ENTRIES);
		airoha_fe_rmw(eth, REG_PPE_TB_CFG(0),
			      PPE_SRAM_TB_NUM_ENTRY_MASK |
			      PPE_DRAM_TB_NUM_ENTRY_MASK,
			      FIELD_PREP(PPE_SRAM_TB_NUM_ENTRY_MASK,
					 sram_num_entries) |
			      FIELD_PREP(PPE_DRAM_TB_NUM_ENTRY_MASK,
					 dram_num_entries));
		airoha_fe_rmw(eth, REG_PPE_TB_CFG(1),
			      PPE_SRAM_TB_NUM_ENTRY_MASK |
			      PPE_DRAM_TB_NUM_ENTRY_MASK,
			      FIELD_PREP(PPE_SRAM_TB_NUM_ENTRY_MASK,
					 sram_num_entries) |
			      FIELD_PREP(PPE_DRAM_TB_NUM_ENTRY_MASK,
					 dram_num_entries));
	} else {
		sram_num_entries =
			PPE_RAM_NUM_ENTRIES_SHIFT(PPE_SRAM_NUM_ENTRIES);
		airoha_fe_rmw(eth, REG_PPE_TB_CFG(0),
			      PPE_SRAM_TB_NUM_ENTRY_MASK |
			      PPE_DRAM_TB_NUM_ENTRY_MASK,
			      FIELD_PREP(PPE_SRAM_TB_NUM_ENTRY_MASK,
					 sram_num_entries) |
			      FIELD_PREP(PPE_DRAM_TB_NUM_ENTRY_MASK,
					 dram_num_entries));
	}
}

int airoha_ppe_init(struct airoha_eth *eth)
{
	struct airoha_npu *npu;
	struct airoha_ppe *ppe;
	int foe_size, err;

	ppe = devm_kzalloc(eth->dev, sizeof(*ppe), GFP_KERNEL);
	if (!ppe)
		return -ENOMEM;

	foe_size = PPE_NUM_ENTRIES * sizeof(struct airoha_foe_entry);
	ppe->foe = dmam_alloc_coherent(eth->dev, foe_size, &ppe->foe_dma,
				       GFP_KERNEL | __GFP_ZERO);
	if (!ppe->foe)
		return -ENOMEM;

	ppe->eth = eth;
	eth->ppe = ppe;

	npu = airoha_npu_init(eth);
	if (IS_ERR(npu))
		return PTR_ERR(npu);

	eth->npu = npu;
	err = airoha_npu_ppe_init(npu);
	if (err)
		goto error;

	airoha_ppe_hw_init(ppe);
	err = airoha_npu_flush_ppe_sram_entries(npu, ppe);
	if (err)
		goto error;

	return 0;

error:
	airoha_npu_deinit(npu);
	eth->npu = NULL;

	return err;
}

void airoha_ppe_deinit(struct airoha_eth *eth)
{
	if (eth->npu) {
		airoha_npu_ppe_deinit(eth->npu);
		airoha_npu_deinit(eth->npu);
	}
}

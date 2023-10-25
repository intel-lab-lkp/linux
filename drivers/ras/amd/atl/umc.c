// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * AMD Address Translation Library
 *
 * umc.c : Unified Memory Controller (UMC) topology helpers
 *
 * Copyright (c) 2023, Advanced Micro Devices, Inc.
 * All Rights Reserved.
 *
 * Author: Yazen Ghannam <Yazen.Ghannam@amd.com>
 */

#include "internal.h"

static bool internal_bit_wise_xor(u32 inp)
{
	bool tmp = 0;
	int i;

	for (i = 0; i < 32; i++)
		tmp = tmp ^ ((inp >> i) & 0x1);

	return tmp;
}

/*
 * Mapping of MCA decoded error address bit location to
 * normalized address on MI300A systems.
 */
static const u8 umc_mca2na_mapping[] = {
	0,  5,  6,  8,  9,  14, 12, 13,
	10, 11, 15, 16, 17, 18, 19, 20,
	21, 22, 23, 24, 25, 26, 27, 28,
	7,  29, 30,
};

/* AddrHashBank and AddrHashPC/PC2 umc register bit fields */
static struct {
	u32 xor_enable	:1;
	u32 col_xor	:13;
	u32 row_xor	:18;
} addr_hash_pc, addr_hash_bank[4];

static struct {
	u32 bank_xor	:6;
} addr_hash_pc2;

#define COLUMN_LOCATION		GENMASK(5, 1)
#define ROW_LOCATION		GENMASK(23, 10)
/*
 * The location of bank, column and row are fixed.
 * location of column bit must be NA[5].
 * Row bits are always placed in a contiguous stretch of NA above the
 * column and bank bits.
 * Bits below the row bits can be either column or bank in any order,
 * with the exception that NA[5] must be a column bit.
 * Stack ID(SID) bits are placed in the MSB position of the NA.
 */
static int umc_ondie_addr_to_normaddr(u64 mca_addr, u16 nid)
{
	u32 bank[4], bank_hash[4], pc_hash;
	u32 col, row, rawbank = 0, pc;
	int i, temp = 0, err;
	u64 mca2na;

	/* Default umc base address on MI300A systems */
	u32 gpu_umc_base = 0x90000;

	/*
	 * Error address logged on MI300A systems is ondie MCA address
	 * in the format MCA_Addr[27:0] =
	 *	{SID[1:0],PC[0],row[14:0],bank[3:0],col[4:0],1'b0}
	 * The bit locations are calculated as per umc_mca2na_mapping[]
	 * to find normalized address.
	 * Refer F19 M90h BKDG Section 20.3.1.3 for clarifications
	 *
	 * XORs need to be applied based on the hash settings below.
	 */

	/* Calculate column and row */
	col = FIELD_GET(COLUMN_LOCATION, mca_addr);
	row = FIELD_GET(ROW_LOCATION, mca_addr);

	/* Apply hashing on below banks for bank calculation */
	for (i = 0; i < 4; i++)
		bank_hash[i] = (mca_addr >> (6 + i)) & 0x1;

	/* bank hash algorithm */
	for (i = 0; i < 4; i++) {
		/* Read AMD PPR UMC::AddrHashBank register */
		err = amd_smn_read(nid, gpu_umc_base + 0xC8 + (i * 4), &temp);
		if (err)
			return err;

		addr_hash_bank[i].xor_enable = temp & 1;
		addr_hash_bank[i].col_xor = FIELD_GET(GENMASK(13, 1), temp);
		addr_hash_bank[i].row_xor = FIELD_GET(GENMASK(31, 14), temp);
		/* bank hash selection */
		bank[i] = bank_hash[i] ^ (addr_hash_bank[i].xor_enable &
			  (internal_bit_wise_xor(col & addr_hash_bank[i].col_xor) ^
			  internal_bit_wise_xor(row & addr_hash_bank[i].row_xor)));
	}

	/* To apply hash on pc bit */
	pc_hash = (mca_addr >> 25) & 0x1;

	/* Read AMD PPR UMC::CH::AddrHashPC register */
	err = amd_smn_read(nid, gpu_umc_base + 0xE0, &temp);
	if (err)
		return err;

	addr_hash_pc.xor_enable = temp & 1;
	addr_hash_pc.col_xor = FIELD_GET(GENMASK(13, 1), temp);
	addr_hash_pc.row_xor = FIELD_GET(GENMASK(31, 14), temp);

	/* Read AMD PPR UMC::CH::AddrHashPC2 register*/
	err = amd_smn_read(nid, gpu_umc_base + 0xE4, &temp);
	if (err)
		return err;

	addr_hash_pc2.bank_xor = FIELD_GET(GENMASK(5, 0), temp);

	/* Calculate bank value from bank[0..3], bank[4] and bank[5] */
	for (i = 0; i < 4; i++)
		rawbank |= (bank[i] & 1) << i;

	rawbank |= (mca_addr >> 22) & 0x30;

	/* pseudochannel(pc) hash selection */
	pc = pc_hash ^ (addr_hash_pc.xor_enable &
		(internal_bit_wise_xor(col & addr_hash_pc.col_xor) ^
		internal_bit_wise_xor(row & addr_hash_pc.row_xor) ^
		internal_bit_wise_xor(rawbank & addr_hash_pc2.bank_xor)));

	/* Mask b'25(pc_bit) and b'[9:6](bank) */
	mca_addr &= ~0x20003c0ULL;

	for (i = 0; i < 4; i++)
		mca_addr |= (bank[i] << (6 + i));

	 mca_addr |= (pc << 25);

	/* NA[4..0] is fixed */
	mca2na = 0x0;
	/* convert mca error address to normalized address */
	for (i = 1; i < ARRAY_SIZE(umc_mca2na_mapping); i++)
		mca2na |= ((mca_addr >> i) & 0x1) << umc_mca2na_mapping[i];

	mca_addr = mca2na;
	pr_info("Error Addr: 0x%016llx\n", mca_addr);
	pr_info("Error hit on: Bank %d Row %d Column %d\n", rawbank, row, col);

	return mca_addr;
}

static u8 get_socket_id(struct mce *m)
{
	return m->socketid;
}

#define MCA_IPID_INST_ID_HI	GENMASK_ULL(47, 44)
static u8 get_die_id(struct mce *m)
{
	/* The "AMD Node ID" is provided in MCA_IPID[InstanceIdHi] */
	if (df_cfg.rev == DF4p5 && df_cfg.flags.heterogeneous) {
		u8 node_id = FIELD_GET(MCA_IPID_INST_ID_HI, m->ipid);

		return node_id / 4;
	}

	/*
	 * For CPUs, this is the AMD Node ID modulo the number
	 * of AMD Nodes per socket.
	 */
	return topology_die_id(m->extcpu) % amd_get_nodes_per_socket();
}

static u64 get_norm_addr(struct mce *m)
{
	/* MI300: DRAM->Normalized translation */
	if (df_cfg.rev == DF4p5 && df_cfg.flags.heterogeneous)
		return umc_ondie_addr_to_normaddr(m->addr, get_socket_id(m));

	return m->addr;
}

#define UMC_CHANNEL_NUM	GENMASK(31, 20)
static u8 get_cs_inst_id(struct mce *m)
{
	return FIELD_GET(UMC_CHANNEL_NUM, m->ipid);
}

/*
 * Use CPU's AMD Node ID for all cases.
 *
 * This is needed to read DF registers which can only be
 * done on CPU-attached DFs even in heterogeneous cases.
 *
 * Future systems may report MCA errors across AMD Nodes.
 * For example, errors from CPU socket 1 are reported to a
 * CPU on socket 0. When this happens, the assumption below
 * will break. But the AMD Node ID will be reported in
 * MCA_IPID[InstanceIdHi] at that time.
 */
static u16 get_df_acc_id(struct mce *m)
{
	return topology_die_id(m->extcpu);
}

int umc_mca_addr_to_sys_addr(struct mce *m, u64 *sys_addr)
{
	u8 cs_inst_id = get_cs_inst_id(m);
	u8 socket_id = get_socket_id(m);
	u64 addr = get_norm_addr(m);
	u8 die_id = get_die_id(m);
	u16 df_acc_id = get_df_acc_id(m);

	if (norm_to_sys_addr(df_acc_id, socket_id, die_id, cs_inst_id, &addr))
		return -EINVAL;

	*sys_addr = addr;
	return 0;
}
EXPORT_SYMBOL_GPL(umc_mca_addr_to_sys_addr);

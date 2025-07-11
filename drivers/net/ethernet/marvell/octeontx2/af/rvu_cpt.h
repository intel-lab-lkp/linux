/* SPDX-License-Identifier: GPL-2.0 */
/* Marvell AF CPT driver
 *
 * Copyright (C) 2022 Marvell.
 */

#ifndef RVU_CPT_H
#define RVU_CPT_H

#include <linux/types.h>

/* CPT instruction size in bytes */
#define RVU_CPT_INST_SIZE	64

/* CPT instruction (CPT_INST_S) queue length */
#define RVU_CPT_INST_QLEN	8200

/* CPT instruction queue size passed to HW is in units of
 * 40*CPT_INST_S messages.
 */
#define RVU_CPT_SIZE_DIV40 (RVU_CPT_INST_QLEN / 40)

/* CPT instruction and pending queues length in CPT_INST_S messages */
#define RVU_CPT_INST_QLEN_MSGS	((RVU_CPT_SIZE_DIV40 - 1) * 40)

/* CPT needs 320 free entries */
#define RVU_CPT_INST_QLEN_EXTRA_BYTES	(320 * RVU_CPT_INST_SIZE)
#define RVU_CPT_EXTRA_SIZE_DIV40	(320 / 40)

/* CPT instruction queue length in bytes */
#define RVU_CPT_INST_QLEN_BYTES                                               \
		((RVU_CPT_SIZE_DIV40 * 40 * RVU_CPT_INST_SIZE) +             \
		RVU_CPT_INST_QLEN_EXTRA_BYTES)

/* CPT instruction group queue length in bytes */
#define RVU_CPT_INST_GRP_QLEN_BYTES                                           \
		((RVU_CPT_SIZE_DIV40 + RVU_CPT_EXTRA_SIZE_DIV40) * 16)

/* CPT FC length in bytes */
#define RVU_CPT_Q_FC_LEN 128

/* CPT LF_Q_SIZE Register */
#define CPT_LF_Q_SIZE_DIV40 GENMASK_ULL(14, 0)

/* CPT invalid engine group num */
#define OTX2_CPT_INVALID_CRYPTO_ENG_GRP 0xFF

/* Fastpath ipsec opcode with inplace processing */
#define OTX2_CPT_INLINE_RX_OPCODE (0x26 | (1 << 6))
#define CN10K_CPT_INLINE_RX_OPCODE (0x29 | (1 << 6))

/* Calculate CPT register offset */
#define CPT_RVU_FUNC_ADDR_S(blk, slot, offs) \
		(((blk) << 20) | ((slot) << 12) | (offs))

static inline void otx2_cpt_write64(void __iomem *reg_base, u64 blk, u64 slot,
				    u64 offs, u64 val)
{
	writeq_relaxed(val, reg_base + CPT_RVU_FUNC_ADDR_S(blk, slot, offs));
}

static inline u64 otx2_cpt_read64(void __iomem *reg_base, u64 blk, u64 slot,
				  u64 offs)
{
	return readq_relaxed(reg_base + CPT_RVU_FUNC_ADDR_S(blk, slot, offs));
}
#endif // RVU_CPT_H

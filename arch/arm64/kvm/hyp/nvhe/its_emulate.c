// SPDX-License-Identifier: GPL-2.0-only

#include <asm/kvm_pkvm.h>
#include <nvhe/mem_protect.h>
#include <nvhe/its_emulate.h>

#include <linux/irqchip/arm-gic-v3.h>

void its_emulate_forward_req(struct pkvm_protected_reg *region, u64 offset, bool write, u64 *reg,
			     u8 reg_size)
{
	void __iomem *addr = __hyp_va(PFN_PHYS(region->pfn) + offset);

	switch (reg_size) {
	case 1:
		if (!write)
			*reg = readb_relaxed(addr);
		else
			writeb_relaxed(*reg, addr);
		break;
	case 2:
		if (!write)
			*reg = readw_relaxed(addr);
		else
			writew_relaxed(*reg, addr);
		break;
	case 4:
		if (!write)
			*reg = readl_relaxed(addr);
		else
			writel_relaxed(*reg, addr);
		break;
	case 8:
		if (!write)
			*reg = readq_relaxed(addr);
		else
			writeq_relaxed(*reg, addr);
		break;
	}
}

struct its_handler {
	u64	offset;
	u8	access_size;
	void	(*write)(struct pkvm_protected_reg *region, u64 offset, u64 value);
	void	(*read)(struct pkvm_protected_reg *region, u64 offset, u64 *read);
};

#define ITS_HANDLER(off, sz, write_cb, read_cb)		\
{							\
	.offset = (off),				\
	.access_size = (sz),				\
	.write = (write_cb),				\
	.read = (read_cb),				\
}

struct its_priv_state {
	/* The location of the ITS in the hypervisor VA */
	void __iomem	*base;

	/* ITS command queue use by the hardware */
	void		*cmd_original;
	void		*cmd_host_copy;
	u64		cmd_offset;
	bool		needs_flush;
	hyp_spinlock_t	its_lock;

	struct its_host_state	*host_state;
};

#define GITS_CWRITER_RETRY	BIT_ULL(0)
#define GITS_CWRITER_OFFSET	GENMASK_ULL(19, 5)

#define GITS_CREADR_STALLED	BIT_ULL(0)
#define GITS_CREADR_OFFSET	GENMASK_ULL(19, 5)

static int submit_single_cmd(struct its_priv_state *its, bool retry)
{
	size_t cmdq_sz = its->host_state->cmdq_len;
	u64 timeout = 1000;
	u64 offset, cwriter, creadr;

	offset = (its->cmd_offset + sizeof(struct its_cmd_block)) % cmdq_sz;

	cwriter = offset & GITS_CWRITER_OFFSET;
	cwriter |= FIELD_PREP(GITS_CWRITER_RETRY, retry);
	writeq_relaxed(cwriter, its->base + GITS_CWRITER);

	while (its->cmd_offset != offset) {
		creadr = readq_relaxed(its->base + GITS_CREADR);

		/* Command failed. */
		if (FIELD_GET(GITS_CREADR_STALLED, creadr))
			return -EIO;

		its->cmd_offset = creadr & GITS_CREADR_OFFSET;
		if (its->cmd_offset == offset)
			return 0;

		/*
		 * We can't spin here forever and we can't roll back
		 * the cmd queue pointer. Let's revert the cmd effects in the
		 * emulation layer and then go back to the driver to let it
		 * decide what to do next.
		 */
		if (!timeout--)
			return -EBUSY;
	}

	return 0;
}

static int process_cmd(struct its_priv_state *its, struct its_cmd_block *cmd,
		       bool rollback)
{
	/* Passthrough everything for now */
	return 0;
}

static void cwriter_write(struct pkvm_protected_reg *region, u64 offset, u64 value)
{
	struct its_priv_state *its = region->priv;
	struct its_cmd_block cmd, raw;
	u64 new_offset;
	bool retry;
	int i;

	new_offset = value & GITS_CWRITER_OFFSET;
	if (new_offset >= its->host_state->cmdq_len)
		return;

	retry = FIELD_GET(GITS_CWRITER_RETRY, value);
	while (its->cmd_offset != new_offset) {
		memcpy(&raw, its->cmd_host_copy + its->cmd_offset, sizeof(raw));

		for (i = 0; i < ARRAY_SIZE(cmd.raw_cmd); i++)
			cmd.raw_cmd[i] = le64_to_cpu(raw.raw_cmd_le[i]);

		if (process_cmd(its, &cmd, /* rollback */ false))
			return;

		memcpy(its->cmd_original + its->cmd_offset, &raw, sizeof(struct its_cmd_block));

		if (its->needs_flush)
			gic_flush_dcache_to_poc(its->cmd_original + its->cmd_offset, sizeof(cmd));
		else
			dsb(ishst);

		if (submit_single_cmd(its, retry)) {
			WARN_ON(process_cmd(its, &cmd, /* rollback */ true));
			return;
		}
	}
}

static void cwriter_read(struct pkvm_protected_reg *region, u64 offset, u64 *read)
{
	struct its_priv_state *its = region->priv;
	*read = readq_relaxed(its->base + GITS_CWRITER);
}

static struct its_handler its_handlers[] = {
	ITS_HANDLER(GITS_CWRITER, sizeof(u64), cwriter_write, cwriter_read),
	{},
};

void pkvm_its_emulate_handler(struct pkvm_protected_reg *region, u64 offset, bool write, u64 *reg,
			      u8 reg_size)
{
	struct its_priv_state *priv = region->priv;
	struct its_handler *reg_handler;

	if (!priv || !IS_ALIGNED(offset, reg_size))
		return;

	for (reg_handler = its_handlers; reg_handler->access_size; reg_handler++) {
		if (reg_handler->offset > offset ||
		    reg_handler->offset + reg_handler->access_size <= offset)
			continue;

		if (reg_handler->access_size < reg_size)
			return;

		if (write && reg_handler->write) {
			hyp_spin_lock(&priv->its_lock);
			reg_handler->write(region, offset, *reg);
			hyp_spin_unlock(&priv->its_lock);
			return;
		}

		if (!write && reg_handler->read) {
			hyp_spin_lock(&priv->its_lock);
			reg_handler->read(region, offset, reg);
			hyp_spin_unlock(&priv->its_lock);
			return;
		}

		return;
	}

	its_emulate_forward_req(region, offset, write, reg, reg_size);
}

static int pkvm_setup_its_shadow_cmdq(struct its_host_state *host_state)
{
	u64 start_pfn, num_pages, i;
	int ret;

	start_pfn = hyp_virt_to_pfn(host_state->cmd_host_copy);
	num_pages = host_state->cmdq_len >> PAGE_SHIFT;

	for (i = 0; i < num_pages; i++) {
		ret = __pkvm_host_share_hyp(start_pfn + i);
		if (ret)
			goto unshare_cmd_host;
	}

	ret = hyp_pin_shared_mem(host_state->cmd_host_copy,
				 host_state->cmd_host_copy + host_state->cmdq_len);
	if (ret)
		goto unshare_cmd_host;

	ret = __pkvm_host_donate_hyp(hyp_virt_to_pfn(host_state->cmd_original), num_pages);
	if (ret) {
		hyp_unpin_shared_mem(host_state->cmd_host_copy,
				     host_state->cmd_host_copy + host_state->cmdq_len);
		goto unshare_cmd_host;
	}

	return ret;
unshare_cmd_host:
	if (i == 0)
		return ret;

	for (i = i - 1; i >= 0; i--)
		__pkvm_host_unshare_hyp(start_pfn + i);
	return ret;
}

static struct pkvm_protected_reg *get_region(phys_addr_t dev_addr)
{
	int i;

	for (i = 0; i < num_protected_reg; i++) {
		if (PFN_PHYS(pkvm_protected_regs[i].pfn) == dev_addr)
			return &pkvm_protected_regs[i];
	}

	return NULL;
}

DEFINE_HYP_SPINLOCK(its_setup_lock);

int pkvm_its_emulate_setup(phys_addr_t dev_addr, struct its_host_state *host_state, void *priv,
			   size_t priv_num_pages)
{
	struct pkvm_protected_reg *its_reg;
	struct its_priv_state *priv_state;
	int ret;

	if (!PAGE_ALIGNED(host_state) || !PAGE_ALIGNED(priv) || !priv_num_pages)
		return -EINVAL;

	host_state = kern_hyp_va(host_state);
	priv = kern_hyp_va(priv);

	hyp_spin_lock(&its_setup_lock);
	its_reg = get_region(dev_addr);
	if (!its_reg) {
		ret = -ENODEV;
		goto err_unlock;
	}

	if (its_reg->priv) {
		ret = -EOPNOTSUPP;
		goto err_unlock;
	}

	ret = __pkvm_host_donate_hyp(hyp_virt_to_pfn(priv), priv_num_pages);
	if (ret)
		goto err_unlock;

	priv_state = priv;
	memset(priv_state, 0, priv_num_pages << PAGE_SHIFT);

	ret = __pkvm_host_donate_hyp(hyp_virt_to_pfn(host_state), 1);
	if (ret)
		goto err_with_priv;

	host_state->cmd_original = kern_hyp_va(host_state->cmd_original);
	host_state->cmd_host_copy = kern_hyp_va(host_state->cmd_host_copy);

	ret = pkvm_setup_its_shadow_cmdq(host_state);
	if (ret)
		goto err_with_host_state;

	hyp_spin_lock_init(&priv_state->its_lock);

	priv_state->host_state = host_state;
	priv_state->base = (void __iomem *)__hyp_va(dev_addr);
	priv_state->cmd_original = host_state->cmd_original;
	priv_state->cmd_host_copy = host_state->cmd_host_copy;

	priv_state->cmd_offset = readq_relaxed(priv_state->base + GITS_CREADR) &
		GITS_CREADR_OFFSET;
	priv_state->needs_flush =
		(readq_relaxed(priv_state->base + GITS_CBASER) & GITS_CBASER_SHAREABILITY_MASK) !=
		GITS_CBASER_InnerShareable;

	its_reg->priv = priv_state;

	hyp_spin_unlock(&its_setup_lock);

	return 0;
err_with_host_state:
	WARN_ON(__pkvm_hyp_donate_host(hyp_virt_to_pfn(host_state), 1));
err_with_priv:
	WARN_ON(__pkvm_hyp_donate_host(hyp_virt_to_pfn(priv_state), 1));
err_unlock:
	hyp_spin_unlock(&its_setup_lock);
	return ret;
}

// SPDX-License-Identifier: GPL-2.0-only

#include <asm/kvm_pkvm.h>
#include <linux/irqchip/arm-gic-v3.h>
#include <nvhe/its_emulate.h>
#include <nvhe/mem_protect.h>

struct its_priv_state {
	void *base;
	void *cmd_hyp_base;
	void *cmd_host_base;
	void *cmd_host_cwriter;
	struct its_shadow_tables *shadow;
	hyp_spinlock_t its_lock;
};

struct its_handler {
	u64 offset;
	u8 access_size;
	void (*write)(struct its_priv_state *its, u64 offset, u64 value);
	void (*read)(struct its_priv_state *its, u64 offset, u64 *read);
};

DEFINE_HYP_SPINLOCK(its_setup_lock);

static void cwriter_write(struct its_priv_state *its, u64 offset, u64 value)
{
	u64 cwriter_offset = value & GENMASK(19, 5);
	int cmd_len, cmd_offset;
	size_t cmdq_sz = its->shadow->cmdq_len;

	if (cwriter_offset > cmdq_sz)
		return;

	cmd_offset = its->cmd_host_cwriter - its->cmd_host_base;
	cmd_len = cwriter_offset - cmd_offset;
	if (cmd_len < 0)
		cmd_len = cmdq_sz - cmd_offset;

	if (cmd_offset + cmd_len > cmdq_sz)
		return;

	memcpy(its->cmd_hyp_base + cmd_offset, its->cmd_host_cwriter, cmd_len);

	its->cmd_host_cwriter = its->cmd_host_base +
		(cmd_offset + cmd_len) % cmdq_sz;
	if (its->cmd_host_cwriter == its->cmd_host_base) {
		memcpy(its->cmd_hyp_base, its->cmd_host_base, cwriter_offset);

		its->cmd_host_cwriter = its->cmd_host_base + cwriter_offset;
	}

	writeq_relaxed(value, its->base + GITS_CWRITER);
}

static void cwriter_read(struct its_priv_state *its, u64 offset, u64 *read)
{
	*read = readq_relaxed(its->base + GITS_CWRITER);
}

#define ITS_HANDLER(off, sz, write_cb, read_cb)	\
{							\
	.offset = (off),				\
	.access_size = (sz),				\
	.write = (write_cb),				\
	.read = (read_cb),				\
}

static struct its_handler its_handlers[] = {
	ITS_HANDLER(GITS_CWRITER, sizeof(u64), cwriter_write, cwriter_read),
	{},
};

void pkvm_handle_forward_req(struct pkvm_protected_reg *region, u64 offset, bool write,
			     u64 *reg, u8 reg_size)
{
	void __iomem *addr = __hyp_va((region->start_pfn << PAGE_SHIFT) + offset);

	if (reg_size == sizeof(u32)) {
		if (!write)
			*reg = readl_relaxed(addr);
		else
			writel_relaxed(*reg, addr);
	} else if (reg_size == sizeof(u64)) {
		if (!write)
			*reg = readq_relaxed(addr);
		else
			writeq_relaxed(*reg, addr);
	}
}

void pkvm_handle_gic_emulation(struct pkvm_protected_reg *region, u64 offset, bool write,
			       u64 *reg, u8 reg_size)
{
	struct its_priv_state *its_priv = region->priv;
	void __iomem *addr;
	struct its_handler *reg_handler;

	if (!its_priv)
		return;

	addr = its_priv->base + offset;
	for (reg_handler = its_handlers; reg_handler->access_size; reg_handler++) {
		if (reg_handler->offset > offset ||
		    reg_handler->offset + reg_handler->access_size <= offset)
			continue;

		if (reg_handler->access_size & (reg_size - 1))
			continue;

		if (write && reg_handler->write) {
			hyp_spin_lock(&its_priv->its_lock);
			reg_handler->write(its_priv, offset, *reg);
			hyp_spin_unlock(&its_priv->its_lock);
			return;
		}

		if (!write && reg_handler->read) {
			hyp_spin_lock(&its_priv->its_lock);
			reg_handler->read(its_priv, offset, reg);
			hyp_spin_unlock(&its_priv->its_lock);
			return;
		}

		return;
	}

	pkvm_handle_forward_req(region, offset, write, reg, reg_size);
}

static struct pkvm_protected_reg *get_region(phys_addr_t dev_addr)
{
	int i;
	u64 dev_pfn = dev_addr >> PAGE_SHIFT;

	for (i = 0; i < PKVM_PROTECTED_REGS_NUM; i++) {
		if (pkvm_protected_regs[i].start_pfn == dev_pfn)
			return &pkvm_protected_regs[i];
	}

	return NULL;
}

static int pkvm_setup_its_shadow_cmdq(struct its_shadow_tables *shadow)
{
	int ret, i, num_pages;
	u64 shadow_start_pfn, original_start_pfn;
	void *cmd_shadow_va = kern_hyp_va(shadow->cmd_shadow);

	shadow_start_pfn = hyp_virt_to_pfn(cmd_shadow_va);
	original_start_pfn = hyp_virt_to_pfn(kern_hyp_va(shadow->cmd_original));
	num_pages = shadow->cmdq_len >> PAGE_SHIFT;

	for (i = 0; i < num_pages; i++) {
		ret = __pkvm_host_share_hyp(shadow_start_pfn + i);
		if (ret)
			goto unshare_shadow;
	}

	ret = hyp_pin_shared_mem(cmd_shadow_va, cmd_shadow_va + shadow->cmdq_len);
	if (ret)
		goto unshare_shadow;

	ret = __pkvm_host_donate_hyp(original_start_pfn, num_pages);
	if (ret)
		goto unpin_shadow;

	return ret;

unpin_shadow:
	hyp_unpin_shared_mem(cmd_shadow_va, cmd_shadow_va + shadow->cmdq_len);

unshare_shadow:
	for (i = i - 1; i >= 0; i--)
		__pkvm_host_unshare_hyp(shadow_start_pfn + i);

	return ret;
}

int pkvm_init_gic_its_emulation(phys_addr_t dev_addr, void *host_priv_state,
				struct its_shadow_tables *host_shadow)
{
	int ret;
	struct its_priv_state *priv_state = kern_hyp_va(host_priv_state);
	struct its_shadow_tables *shadow = kern_hyp_va(host_shadow);
	struct pkvm_protected_reg *its_reg;

	hyp_spin_lock(&its_setup_lock);
	its_reg = get_region(dev_addr);
	if (!its_reg)
		return -ENODEV;

	if (its_reg->priv)
		return -EOPNOTSUPP;

	ret = __pkvm_host_donate_hyp(hyp_virt_to_pfn(priv_state), 1);
	if (ret)
		return ret;

	ret = __pkvm_host_donate_hyp(hyp_virt_to_pfn(shadow), 1);
	if (ret)
		goto err_with_state;

	ret = pkvm_setup_its_shadow_cmdq(shadow);
	if (ret)
		goto err_with_shadow;

	its_reg->priv = priv_state;

	hyp_spin_lock_init(&priv_state->its_lock);
	priv_state->shadow = shadow;
	priv_state->base = __hyp_va(dev_addr);

	priv_state->cmd_hyp_base = kern_hyp_va(shadow->cmd_original);
	priv_state->cmd_host_base = kern_hyp_va(shadow->cmd_shadow);
	priv_state->cmd_host_cwriter = priv_state->cmd_host_base;

	hyp_spin_unlock(&its_setup_lock);

	return 0;
err_with_shadow:
	__pkvm_hyp_donate_host(hyp_virt_to_pfn(shadow), 1);
err_with_state:
	__pkvm_hyp_donate_host(hyp_virt_to_pfn(priv_state), 1);
	return ret;
}

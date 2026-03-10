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
	u16 empty_idx;
	u64 tracked_pfns[];
};

#define MAX_TRACKED_PFNS	((PAGE_SIZE - offsetof(struct its_priv_state, \
				  tracked_pfns)) / sizeof(u64))

struct its_handler {
	u64 offset;
	u8 access_size;
	u8 num_registers;
	void (*write)(struct its_priv_state *its, u64 offset, u64 value);
	void (*read)(struct its_priv_state *its, u64 offset, u64 *read);
};

DEFINE_HYP_SPINLOCK(its_setup_lock);

static int track_pfn_add(struct its_priv_state *its, u64 pfn)
{
	int ret, i;

	if (its->empty_idx + 1 >= MAX_TRACKED_PFNS)
		return -ENOSPC;

	ret = __pkvm_host_share_hyp(pfn);
	if (ret)
		return ret;

	its->tracked_pfns[its->empty_idx] = pfn;
	for (i = 0; i < MAX_TRACKED_PFNS; i++) {
		if (!its->tracked_pfns[i])
			break;
	}

	its->empty_idx = i;
	return 0;
}

static int track_pfn_remove(struct its_priv_state *its, u64 pfn)
{
	int i, ret;

	for (i = 0; i < MAX_TRACKED_PFNS; i++) {
		if (its->tracked_pfns[i] != pfn)
			continue;

		ret = __pkvm_host_unshare_hyp(pfn);
		if (ret)
			return ret;

		its->tracked_pfns[i] = 0;
		its->empty_idx = i;
	}

	return 0;
}

static int get_num_itt_pages(struct its_priv_state *its, u8 num_bits)
{
	int nr_ites = 1 << (num_bits + 1);
	u64 size, gits_typer = readq_relaxed(its->base + GITS_TYPER);

	size = nr_ites * (FIELD_GET(GITS_TYPER_ITT_ENTRY_SIZE, gits_typer) + 1);
	size = max(size, ITS_ITT_ALIGN) + ITS_ITT_ALIGN - 1;

	return PAGE_ALIGN(size) >> PAGE_SHIFT;
}

static int track_pfn(struct its_priv_state *its, u64 start_pfn, int num_pages, bool remove)
{
	int i, ret;

	for (i = 0; i < num_pages; i++) {
		if (remove)
			ret = track_pfn_remove(its, start_pfn + i);
		else
			ret = track_pfn_add(its, start_pfn + i);

		if (ret)
			goto err_track;
	}

	return 0;
err_track:
	for (i = i - 1; i >= 0; i--) {
		if (remove)
			track_pfn_add(its, start_pfn + i);
		else
			track_pfn_remove(its, start_pfn + i);
	}

	return ret;
}

static struct its_baser *get_table(struct its_priv_state *its, u64 type)
{
	int i;
	struct its_shadow_tables *shadow = its->shadow;

	for (i = 0; i < GITS_BASER_NR_REGS; i++) {
		if (GITS_BASER_TYPE(shadow->tables[i].val) == type)
			return &shadow->tables[i];
	}

	return NULL;
}

static int check_table_update(struct its_priv_state *its, u32 id, u64 type)
{
	u32 lvl1_idx;
	u64 esz, *host_table, *hyp_table, new_entry, update;
	struct its_baser *table = get_table(its, type);
	int ret;
	phys_addr_t new_lvl2_table, lvl2_table;

	if (!table)
		return -EINVAL;

	if (!(table->val & GITS_BASER_INDIRECT))
		return 0;

	esz = GITS_BASER_ENTRY_SIZE(table->val);
	lvl1_idx = id / (table->psz / esz);

	host_table = kern_hyp_va(table->shadow);
	hyp_table = kern_hyp_va(table->base);

	new_entry = host_table[id];
	update = new_entry ^ hyp_table[id];
	if (!update || !(update & GITS_BASER_VALID))
		return 0;

	new_lvl2_table = hyp_phys_to_pfn(new_entry & PHYS_MASK_SHIFT);
	lvl2_table = hyp_phys_to_pfn(hyp_table[id] & PHYS_MASK_SHIFT);
	if (new_entry & GITS_BASER_VALID)
		ret = __pkvm_host_donate_hyp(new_lvl2_table, table->psz >> PAGE_SHIFT);
	else
		ret = __pkvm_hyp_donate_host(lvl2_table, table->psz >> PAGE_SHIFT);
	if (ret)
		return ret;

	hyp_table[id] = new_entry;
	return 0;
}

static int process_its_mapd(struct its_priv_state *its, struct its_cmd_block *cmd)
{
	phys_addr_t itt_addr = cmd->raw_cmd[2] & GENMASK(51, 8);
	u8 size = cmd->raw_cmd[1] & GENMASK(4, 0);
	bool remove = !(cmd->raw_cmd[2] & BIT(63));
	u32 device_id = cmd->raw_cmd[0] >> 32;
	int num_pages, ret;
	u64 base_pfn;

	if (PAGE_ALIGNED(itt_addr))
		return -EINVAL;

	base_pfn = hyp_phys_to_pfn(itt_addr);
	num_pages = get_num_itt_pages(its, size);

	ret = check_table_update(its, device_id, GITS_BASER_TYPE_DEVICE);
	if (ret)
		return ret;

	return track_pfn(its, base_pfn, num_pages, remove);
}

static int process_its_vmapp(struct its_priv_state *its, struct its_cmd_block *cmd)
{
	bool remove = !(cmd->raw_cmd[2] & BIT(63));
	phys_addr_t vpt_addr = cmd->raw_cmd[3] & GENMASK(51, 16);
	u8 vpt_size = cmd->raw_cmd[3] & GENMASK(4, 0);
	u32 vpe_id = (cmd->raw_cmd[1] & GENMASK(47, 32)) >> 32;
	int num_pages;
	u64 base_pfn;
	int ret;

	base_pfn = hyp_phys_to_pfn(vpt_addr);
	num_pages = ALIGN(BIT((vpt_size + 1) >> 3), SZ_64K);

	ret = check_table_update(its, vpe_id, GITS_BASER_TYPE_VCPU);
	if (ret)
		return ret;

	return track_pfn(its, base_pfn, num_pages, remove);
}

static int process_its_mapc(struct its_priv_state *its, struct its_cmd_block *cmd)
{
	u32 icid = cmd->raw_cmd[2] & GENMASK(15, 0);

	return check_table_update(its, icid, GITS_BASER_TYPE_COLLECTION);
}

static int parse_its_cmdq(struct its_priv_state *its, int offset, ssize_t len)
{
	struct its_cmd_block *cmd = its->cmd_hyp_base + offset;
	u8 req_type;
	int ret = 0;

	while (len > 0 && !ret) {
		req_type = cmd->raw_cmd[0] & GENMASK(7, 0);

		switch (req_type) {
		case GITS_CMD_MAPD:
			ret = process_its_mapd(its, cmd);
			break;

		case GITS_CMD_VMAPP:
			ret = process_its_vmapp(its, cmd);
			break;

		case GITS_CMD_MAPC:
			ret = process_its_mapc(its, cmd);
			break;
		}

		cmd++;
		len -= sizeof(struct its_cmd_block);
	}

	return ret;
}

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
	if (parse_its_cmdq(its, cmd_offset, cmd_len))
		return;

	its->cmd_host_cwriter = its->cmd_host_base +
		(cmd_offset + cmd_len) % cmdq_sz;
	if (its->cmd_host_cwriter == its->cmd_host_base) {
		memcpy(its->cmd_hyp_base, its->cmd_host_base, cwriter_offset);
		if (parse_its_cmdq(its, cmd_offset, cmd_len))
			return;

		its->cmd_host_cwriter = its->cmd_host_base + cwriter_offset;
	}

	writeq_relaxed(value, its->base + GITS_CWRITER);
}

static void cwriter_read(struct its_priv_state *its, u64 offset, u64 *read)
{
	*read = readq_relaxed(its->base + GITS_CWRITER);
}

static void ctlr_read(struct its_priv_state *its, u64 offset, u64 *read)
{
	*read = readq_relaxed(its->base + GITS_CTLR);
}

static void ctlr_write(struct its_priv_state *its, u64 offset, u64 value)
{
	u64 ctlr = readq_relaxed(its->base + GITS_CTLR);
	bool is_quiescent = !!(ctlr & GITS_CTLR_QUIESCENT);
	bool is_enabled = !!(ctlr & GITS_CTLR_ENABLE);

	if (!is_enabled && (value & GITS_CTLR_ENABLE) && !is_quiescent)
		return;

	writeq_relaxed(value, its->base + GITS_CTLR);
}

static void cbaser_write(struct its_priv_state *its, u64 offset, u64 value)
{
	u64 ctlr = readq_relaxed(its->base + GITS_CTLR);
	int num_pages;

	if ((ctlr & GITS_CTLR_ENABLE) ||
	    !(ctlr & GITS_CTLR_QUIESCENT))
		return;

	num_pages = its->shadow->cmdq_len / SZ_4K;
	value &= ~GENMASK(7, 0) | ~GENMASK_ULL(51, 12);

	value |= (num_pages - 1) & GENMASK(7, 0);
	value |= __hyp_pa(its->cmd_hyp_base) & GENMASK_ULL(51, 12);

	its->cmd_host_cwriter = its->cmd_host_base;
	writeq_relaxed(value, its->base + GITS_CBASER);
}

static void cbaser_read(struct its_priv_state *its, u64 offset, u64 *read)
{
	*read = readq_relaxed(its->base + GITS_CBASER);
}

static void baser_write(struct its_priv_state *its, u64 offset, u64 value)
{
	u64 baser, ctlr = readq_relaxed(its->base + GITS_CTLR);
	int baser_idx;

	if ((ctlr & GITS_CTLR_ENABLE) ||
	    !(ctlr & GITS_CTLR_QUIESCENT))
		return;

	baser_idx = (offset - GITS_BASER) >> 3;
	baser = its->shadow->tables[baser_idx].val;
	if ((value & GITS_BASER_INDIRECT) != (baser & GITS_BASER_INDIRECT))
		return;

	value &= ~GENMASK(47, 12) | ~GENMASK(9, 0);
	value |= (baser & GENMASK(47, 12)) | (baser & GENMASK(9, 0));

	writeq_relaxed(value, its->base + offset);
}

static void baser_read(struct its_priv_state *its, u64 offset, u64 *read)
{
	*read = readq_relaxed(its->base + offset);
}

#define ITS_HANDLER(off, sz, num, write_cb, read_cb)	\
{							\
	.offset = (off),				\
	.access_size = (sz),				\
	.num_registers = (num),				\
	.write = (write_cb),				\
	.read = (read_cb),				\
}

#define ITS_REG(off, sz, write_cb, read_cb)	\
	ITS_HANDLER(off, sz, 1, write_cb, read_cb)

static struct its_handler its_handlers[] = {
	ITS_REG(GITS_CWRITER, sizeof(u64), cwriter_write, cwriter_read),
	ITS_REG(GITS_CTLR, sizeof(u64), ctlr_write, ctlr_read),
	ITS_REG(GITS_CBASER, sizeof(u64), cbaser_write, cbaser_read),
	ITS_HANDLER(GITS_BASER, sizeof(u64), 8, baser_write, baser_read),
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
	u64 end;

	if (!its_priv)
		return;

	addr = its_priv->base + offset;
	for (reg_handler = its_handlers; reg_handler->access_size; reg_handler++) {
		end = reg_handler->offset + reg_handler->access_size * reg_handler->num_registers;

		if (reg_handler->offset > offset || end <= offset)
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

static int pkvm_host_unmap_last_level(void *shadow, size_t num_pages, u32 psz)
{
	u64 *table = shadow;
	int ret, i, end = (num_pages << PAGE_SHIFT) / sizeof(table);
	phys_addr_t table_addr;

	for (i = 0; i < end; i++) {
		if (!(table[i] & GITS_BASER_VALID))
			continue;

		table_addr = table[i] & PHYS_MASK;
		ret = __pkvm_host_donate_hyp(hyp_phys_to_pfn(table_addr), psz >> PAGE_SHIFT);
		if (ret)
			goto err_donate;
	}

	return 0;
err_donate:
	for (i = i - 1; i >= 0; i--) {
		if (!(table[i] & GITS_BASER_VALID))
			continue;

		table_addr = table[i] & PHYS_MASK;
		__pkvm_hyp_donate_host(hyp_phys_to_pfn(table_addr), psz >> PAGE_SHIFT);
	}
	return ret;
}

static int pkvm_share_shadow_table(void *shadow, u64 nr_pages)
{
	u64 i, ret, start_pfn = hyp_virt_to_pfn(shadow);

	for (i = 0; i < nr_pages; i++) {
		ret = __pkvm_host_share_hyp(start_pfn + i);
		if (ret)
			goto unshare;
	}

	ret = hyp_pin_shared_mem(shadow, shadow + (nr_pages << PAGE_SHIFT));
	if (ret)
		goto unshare;

	return ret;
unshare:
	for (i = i - 1; i >= 0; i--)
		__pkvm_host_unshare_hyp(start_pfn + i);
	return ret;
}

static void pkvm_unshare_shadow_table(void *shadow, u64 nr_pages)
{
	u64 i, start_pfn = hyp_virt_to_pfn(shadow);

	hyp_unpin_shared_mem(shadow, shadow + (nr_pages << PAGE_SHIFT));

	for (i = 0; i < nr_pages; i++)
		WARN_ON(__pkvm_host_unshare_hyp(start_pfn + i));
}

static void pkvm_host_map_last_level(void *shadow, size_t num_pages, u32 psz)
{
	u64 *table;
	int i, end = (num_pages << PAGE_SHIFT) / sizeof(table);
	phys_addr_t table_addr;

	for (i = 0; i < end; i++) {
		if (!(table[i] & GITS_BASER_VALID))
			continue;

		table_addr = table[i] & ~GITS_BASER_VALID;
		WARN_ON(__pkvm_hyp_donate_host(hyp_phys_to_pfn(table_addr), psz >> PAGE_SHIFT));
	}
}

static int pkvm_setup_its_shadow_baser(struct its_shadow_tables *shadow)
{
	int i, ret;
	u64 baser_val, num_pages, type;
	void *base, *host_base;

	for (i = 0; i < GITS_BASER_NR_REGS; i++) {
		baser_val = shadow->tables[i].val;
		if (!(baser_val & GITS_BASER_VALID))
			continue;

		base = kern_hyp_va(shadow->tables[i].base);
		num_pages = (1 << shadow->tables[i].order);

		ret = __pkvm_host_donate_hyp(hyp_virt_to_pfn(base), num_pages);
		if (ret)
			goto err_donate;

		if (baser_val & GITS_BASER_INDIRECT) {
			host_base = kern_hyp_va(shadow->tables[i].shadow);
			ret = pkvm_share_shadow_table(host_base, num_pages);
			if (ret)
				goto err_with_donation;

			type = GITS_BASER_TYPE(baser_val);
			if (type == GITS_BASER_TYPE_COLLECTION)
				continue;

			ret = pkvm_host_unmap_last_level(base, num_pages,
							 shadow->tables[i].psz);
			if (ret)
				goto err_with_share;
		}
	}

	return 0;
err_with_share:
	pkvm_unshare_shadow_table(host_base, num_pages);
err_with_donation:
	__pkvm_hyp_donate_host(hyp_virt_to_pfn(base), num_pages);
err_donate:
	for (i = i - 1; i >= 0; i--) {
		baser_val = shadow->tables[i].val;
		if (!(baser_val & GITS_BASER_VALID))
			continue;

		base = kern_hyp_va(shadow->tables[i].base);
		num_pages = (1 << shadow->tables[i].order);

		WARN_ON(__pkvm_hyp_donate_host(hyp_virt_to_pfn(base), num_pages));
		if (baser_val & GITS_BASER_INDIRECT) {
			host_base = kern_hyp_va(shadow->tables[i].shadow);
			pkvm_unshare_shadow_table(host_base, num_pages);

			type = GITS_BASER_TYPE(baser_val);
			if (type == GITS_BASER_TYPE_COLLECTION)
				continue;

			pkvm_host_map_last_level(base, num_pages, shadow->tables[i].psz);
		}
	}

	return ret;
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

	ret = pkvm_setup_its_shadow_baser(shadow);
	if (ret)
		goto err_with_shadow;

	its_reg->priv = priv_state;

	hyp_spin_lock_init(&priv_state->its_lock);
	priv_state->shadow = shadow;
	priv_state->base = __hyp_va(dev_addr);

	priv_state->cmd_hyp_base = kern_hyp_va(shadow->cmd_original);
	priv_state->cmd_host_base = kern_hyp_va(shadow->cmd_shadow);
	priv_state->cmd_host_cwriter = priv_state->cmd_host_base;
	priv_state->empty_idx = 0;

	hyp_spin_unlock(&its_setup_lock);

	return 0;
err_with_shadow:
	__pkvm_hyp_donate_host(hyp_virt_to_pfn(shadow), 1);
err_with_state:
	__pkvm_hyp_donate_host(hyp_virt_to_pfn(priv_state), 1);
	return ret;
}

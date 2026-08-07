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
	u8	num_registers;
	void	(*write)(struct pkvm_protected_reg *region, u64 offset, u64 value);
	void	(*read)(struct pkvm_protected_reg *region, u64 offset, u64 *read);
};

#define ITS_HANDLER_REG_PAIR(off, sz, registers, write_cb, read_cb)	\
{							\
	.offset = (off),				\
	.access_size = (sz),				\
	.num_registers = (registers),			\
	.write = (write_cb),				\
	.read = (read_cb),				\
}

#define ITS_HANDLER(off, sz, write_cb, read_cb)		\
	ITS_HANDLER_REG_PAIR(off, sz, 1, write_cb, read_cb)

struct dte_entry {
	u32	device_id;
	u64	itt_pfn;
};

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
	u16			empty_entry;
	u16			num_tracked_entries;
	struct dte_entry	tracked_entries[];
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

static int get_num_itt_pages(struct its_priv_state *its, u8 num_bits)
{
	u64 gits_typer, nr_ites;
	size_t sz;

	gits_typer = readq_relaxed(its->base + GITS_TYPER);
	if (num_bits > FIELD_GET(GITS_TYPER_IDBITS, gits_typer))
		return -EINVAL;

	nr_ites = BIT_ULL(num_bits + 1);
	sz = nr_ites * (FIELD_GET(GITS_TYPER_ITT_ENTRY_SIZE, gits_typer) + 1);
	sz = max(sz, ITS_ITT_ALIGN) + ITS_ITT_ALIGN - 1;

	return PAGE_ALIGN(sz) >> PAGE_SHIFT;
}

static struct its_baser *get_table_from_snapshot(struct its_host_state *host, u64 baser_type)
{
	int i;

	for (i = 0; i < GITS_BASER_NR_REGS; i++) {
		if (GITS_BASER_TYPE(host->tables[i].val) == baser_type)
			return &host->tables[i];
	}

	return NULL;
}

static int check_table_update(struct its_priv_state *its, u32 device_id, u64 type, bool rollback)
{
	struct its_baser *table = get_table_from_snapshot(its->host_state, type);
	size_t lvl2_entry_sz, lvl1_table_sz, num_lvl2_entries, num_lvl1_entries;
	u64 *snapshot_table, *original_table;
	u64 prev_entry, new_entry;
	u32 new_entry_index;
	int ret;

	if (!table)
		return -EINVAL;

	/* We only do shadow udates for the first level of indirect tables */
	if (!(table->val & GITS_BASER_INDIRECT))
		return 0;

	lvl2_entry_sz = GITS_BASER_ENTRY_SIZE(table->val);
	num_lvl2_entries = table->psz / lvl2_entry_sz;

	lvl1_table_sz = (1 << table->order) << PAGE_SHIFT;
	num_lvl1_entries = lvl1_table_sz / sizeof(u64);

	new_entry_index = device_id / num_lvl2_entries;
	if (new_entry_index >= num_lvl1_entries)
		return -ENOSPC;

	snapshot_table = kern_hyp_va(table->base_snapshot);
	original_table = kern_hyp_va(table->base);

	/*
	 * Look at the host table copy and if the entry hasn't changed the valid
	 * bit compared to the original table used by the hardwre, don't update anything.
	 */
	new_entry = snapshot_table[new_entry_index];
	prev_entry = original_table[new_entry_index];
	if (!((new_entry ^ prev_entry) & GITS_BASER_VALID))
		return 0;

	/*
	 * The host can play nasty tricks with read-modify-write after a
	 * rollback is triggered but we still hold on to the original tables
	 * which are hyp managed and we don't give back any other page to the
	 * host.
	 */
	if (rollback)
		new_entry = new_entry ^ GITS_BASER_VALID;

	if (new_entry & GITS_BASER_VALID)
		ret = __pkvm_host_donate_hyp(hyp_phys_to_pfn(new_entry & PHYS_MASK),
					     table->psz >> PAGE_SHIFT);
	else
		ret = __pkvm_hyp_donate_host(hyp_phys_to_pfn(prev_entry & PHYS_MASK),
					     table->psz >> PAGE_SHIFT);
	if (ret)
		return ret;

	original_table[new_entry_index] = new_entry;
	return 0;
}

static int track_pfn_add(struct its_priv_state *its, u32 device_id, u64 pfn)
{
	void *virt = hyp_phys_to_virt(hyp_pfn_to_phys(pfn));
	struct dte_entry *entries = &its->tracked_entries[0];
	bool pfn_shared = false;
	int ret;
	int i;

	for (i = 0; i < its->num_tracked_entries; i++) {
		if (entries[i].itt_pfn == pfn) {
			if (entries[i].device_id != device_id) {
				pfn_shared = true;
				break;
			} else {
				return hyp_pin_shared_mem(virt, virt + PAGE_SIZE);
			}
		}
	}

	if (its->empty_entry >= its->num_tracked_entries)
		return -ENOSPC;

	if (!pfn_shared) {
		ret = __pkvm_host_share_hyp(pfn);
		if (ret)
			return ret;
	}

	ret = hyp_pin_shared_mem(virt, virt + PAGE_SIZE);
	if (ret) {
		__pkvm_host_unshare_hyp(pfn);
		return ret;
	}

	entries[its->empty_entry].itt_pfn = pfn;
	entries[its->empty_entry].device_id = device_id;

	for (i = 0; i < its->num_tracked_entries; i++) {
		if (!entries[i].itt_pfn && !entries[i].device_id)
			break;
	}
	its->empty_entry = i;
	return 0;
}

static int track_pfn_remove(struct its_priv_state *its, u32 device_id, u64 pfn)
{
	void *virt = hyp_phys_to_virt(hyp_pfn_to_phys(pfn));
	struct dte_entry *entries = &its->tracked_entries[0];
	int ret;
	int i;

	for (i = 0; i < its->num_tracked_entries; i++) {
		if (entries[i].itt_pfn != pfn || entries[i].device_id != device_id)
			continue;

		/* To decrement the refcount, first try to unshare it */
		ret = __pkvm_host_unshare_hyp(pfn);
		if (ret == -EBUSY) {
			hyp_unpin_shared_mem(virt, virt + PAGE_SIZE);
			ret = __pkvm_host_unshare_hyp(pfn);
			if (ret == -EBUSY)
				return 0;

			WARN_ON(ret);
		}

		memset(&entries[i], 0, sizeof(struct dte_entry));
		its->empty_entry = i;
		return 0;
	}

	return -EINVAL;
}

static int track_pfn(struct its_priv_state *its, u32 device_id, u64 pfn, int num_pages,
		     bool remove)
{
	int ret;
	int i;

	for (i = 0; i < num_pages; i++) {
		if (remove)
			ret = track_pfn_remove(its, device_id, pfn + i);
		else
			ret = track_pfn_add(its, device_id, pfn + i);

		if (ret)
			goto err_track_pfn;
	}

	return 0;
err_track_pfn:
	for (i = i - 1; i >= 0; i--) {
		if (remove)
			WARN_ON(track_pfn_add(its, device_id, pfn + i));
		else
			WARN_ON(track_pfn_remove(its, device_id, pfn + i));
	}
	return ret;
}

static int process_its_mapd(struct its_priv_state *its, struct its_cmd_block *cmd, bool rollback)
{
	phys_addr_t itt_addr = cmd->raw_cmd[2] & GENMASK(51, 8);
	bool remove = !(cmd->raw_cmd[2] & BIT(63));
	u8 size = cmd->raw_cmd[1] & GENMASK(4, 0);
	u32 device_id = cmd->raw_cmd[0] >> 32;
	int num_pages, ret;
	u64 itt_pfn;

	if (rollback)
		remove = !remove;

	itt_pfn = hyp_phys_to_pfn(itt_addr);
	num_pages = get_num_itt_pages(its, size);
	if (num_pages < 0)
		return num_pages;

	ret = check_table_update(its, device_id, GITS_BASER_TYPE_DEVICE, rollback);
	if (ret)
		return ret;

	return track_pfn(its, device_id, itt_pfn, num_pages, remove);
}

static int process_its_mapc(struct its_priv_state *its, struct its_cmd_block *cmd, bool rollback)
{
	u32 icid = cmd->raw_cmd[2] & GENMASK(15, 0);

	return check_table_update(its, icid, GITS_BASER_TYPE_COLLECTION, rollback);
}

static int process_cmd(struct its_priv_state *its, struct its_cmd_block *cmd,
		       bool rollback)
{
	u8 req_type = cmd->raw_cmd[0] & GENMASK_ULL(7, 0);
	int ret = 0;

	switch (req_type) {
	case GITS_CMD_MAPD:
		ret = process_its_mapd(its, cmd, rollback);
		break;

	case GITS_CMD_MAPC:
		ret = process_its_mapc(its, cmd, rollback);
		break;
	default:
		/* Passthrough everything for now */
		break;
	}

	return ret;
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

static void ctlr_read(struct pkvm_protected_reg *region, u64 offset, u64 *read)
{
	struct its_priv_state *its = region->priv;
	*read = readl_relaxed(its->base + GITS_CTLR);
}

static void ctlr_write(struct pkvm_protected_reg *region, u64 offset, u64 value)
{
	struct its_priv_state *its = region->priv;
	bool is_quiescent, is_enabled;
	u32 ctlr;

	ctlr = readl_relaxed(its->base + GITS_CTLR);
	is_quiescent = !!(ctlr & GITS_CTLR_QUIESCENT);
	is_enabled = !!(ctlr & GITS_CTLR_ENABLE);

	/*
	 * If it's disabled and not in quiescent state and it tries to enable
	 * it, bail out.
	 */
	if (!is_enabled && (value & GITS_CTLR_ENABLE) && !is_quiescent)
		return;

	writel_relaxed(value, its->base + GITS_CTLR);
}

static void cbaser_write(struct pkvm_protected_reg *region, u64 offset, u64 value)
{
	struct its_priv_state *its = region->priv;
	int num_pages;
	u64 ctlr;

	ctlr = readl_relaxed(its->base + GITS_CTLR);
	if ((ctlr & GITS_CTLR_ENABLE) || !(ctlr & GITS_CTLR_QUIESCENT))
		return;

	num_pages = its->host_state->cmdq_len / SZ_4K;

	/* Don't let the host program a different command queue */
	value &= ~(GENMASK(7, 0) | GENMASK_ULL(51, 12));
	value |= (num_pages - 1) & GENMASK(7, 0);
	value |= __hyp_pa(its->cmd_original) & GENMASK_ULL(51, 12);
	its->needs_flush = (value & GITS_CBASER_SHAREABILITY_MASK) != GITS_CBASER_InnerShareable;

	writeq_relaxed(value, its->base + GITS_CBASER);

	/* Restart the CMDQ to read from 0 */
	its->cmd_offset = 0;
	writeq_relaxed(0, its->base + GITS_CWRITER);
}

static void cbaser_read(struct pkvm_protected_reg *region, u64 offset, u64 *read)
{
	struct its_priv_state *its = region->priv;
	*read = readq_relaxed(its->base + GITS_CBASER);
}

static void baser_write(struct pkvm_protected_reg *region, u64 offset, u64 value)
{
	struct its_priv_state *its = region->priv;
	u32 ctlr = readl_relaxed(its->base + GITS_CTLR);
	int baser_idx;
	u64 baser;

	if ((ctlr & GITS_CTLR_ENABLE) || !(ctlr & GITS_CTLR_QUIESCENT))
		return;

	baser_idx = (offset - GITS_BASER) >> 3;
	baser = its->host_state->tables[baser_idx].val;

	/* Prevent if it tries to change from direct layout to indirect layout */
	if ((value & GITS_BASER_INDIRECT) != (baser & GITS_BASER_INDIRECT))
		return;

	/* Don't allow the host to point to new tables or new attributes */
	value &= ~(GENMASK_ULL(47, 12) | GENMASK_ULL(9, 0));
	value |= (baser & GENMASK_ULL(47, 12)) | (baser & GENMASK_ULL(9, 0));

	writeq_relaxed(value, its->base + offset);
}

static void baser_read(struct pkvm_protected_reg *region, u64 offset, u64 *read)
{
	struct its_priv_state *its = region->priv;
	*read = readq_relaxed(its->base + offset);
}

static struct its_handler its_handlers[] = {
	ITS_HANDLER(GITS_CWRITER, sizeof(u64), cwriter_write, cwriter_read),
	ITS_HANDLER(GITS_CTLR, sizeof(u32), ctlr_write, ctlr_read),
	ITS_HANDLER(GITS_CBASER, sizeof(u64), cbaser_write, cbaser_read),

	ITS_HANDLER_REG_PAIR(GITS_BASER, sizeof(u64), 8, baser_write, baser_read),
	{},
};

void pkvm_its_emulate_handler(struct pkvm_protected_reg *region, u64 offset, bool write, u64 *reg,
			      u8 reg_size)
{
	struct its_priv_state *priv = region->priv;
	struct its_handler *reg_handler;
	u64 end;

	if (!priv || !IS_ALIGNED(offset, reg_size))
		return;

	for (reg_handler = its_handlers; reg_handler->access_size; reg_handler++) {
		end = reg_handler->offset + reg_handler->access_size * reg_handler->num_registers;
		if (reg_handler->offset > offset || end <= offset)
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

static void pkvm_teardown_its_shadow_cmdq(struct its_host_state *host_state)
{
	u64 i, start_pfn, num_pages = host_state->cmdq_len >> PAGE_SHIFT;

	start_pfn = hyp_virt_to_pfn(host_state->cmd_host_copy);
	hyp_unpin_shared_mem(host_state->cmd_host_copy,
			     host_state->cmd_host_copy + host_state->cmdq_len);

	for (i = 0; i < num_pages; i++)
		WARN_ON(__pkvm_host_unshare_hyp(start_pfn + i));

	WARN_ON(__pkvm_hyp_donate_host(hyp_virt_to_pfn(host_state->cmd_original), num_pages));
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

static void pkvm_unshare_shadow_table(void *shadow, u64 nr_pages)
{
	u64 i, start_pfn = hyp_virt_to_pfn(shadow);

	hyp_unpin_shared_mem(shadow, shadow + (nr_pages << PAGE_SHIFT));

	for (i = 0; i < nr_pages; i++)
		WARN_ON(__pkvm_host_unshare_hyp(start_pfn + i));
}

static int pkvm_host_unmap_last_level(void *shadow, size_t num_pages, u32 psz)
{
	phys_addr_t table_addr;
	u64 *table = shadow;
	int i, end;
	int ret;

	end = (num_pages << PAGE_SHIFT) / sizeof(*table);
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
	while (i--)
		__pkvm_host_unshare_hyp(start_pfn + i);
	return ret;
}

static void pkvm_host_map_last_level(void *shadow, size_t num_pages, u32 psz)
{
	u64 *table = shadow;
	int i, end = (num_pages << PAGE_SHIFT) / sizeof(*table);
	phys_addr_t table_addr;

	for (i = 0; i < end; i++) {
		if (!(table[i] & GITS_BASER_VALID))
			continue;

		table_addr = table[i] & PHYS_MASK;
		WARN_ON(__pkvm_hyp_donate_host(hyp_phys_to_pfn(table_addr), psz >> PAGE_SHIFT));
	}
}

static int pkvm_setup_its_shadow_baser(struct its_host_state *host_state)
{
	u64 baser_val, num_pages;
	void *original_table, *snapshot_table;
	int ret;
	int i;

	for (i = 0; i < GITS_BASER_NR_REGS; i++) {
		baser_val = host_state->tables[i].val;
		if (!(baser_val & GITS_BASER_VALID))
			continue;

		original_table = kern_hyp_va(host_state->tables[i].base);
		num_pages = (1 << host_state->tables[i].order);

		ret = __pkvm_host_donate_hyp(hyp_virt_to_pfn(original_table), num_pages);
		if (ret)
			goto err_donate;

		if (baser_val & GITS_BASER_INDIRECT) {
			if (!host_state->tables[i].base_snapshot) {
				ret = -EINVAL;
				goto err_with_donation;
			}

			snapshot_table = kern_hyp_va(host_state->tables[i].base_snapshot);
			ret = pkvm_share_shadow_table(snapshot_table, num_pages);
			if (ret)
				goto err_with_donation;

			ret = pkvm_host_unmap_last_level(original_table, num_pages,
							 host_state->tables[i].psz);
			if (ret)
				goto err_with_share;
		}
	}

	return 0;
err_with_share:
	pkvm_unshare_shadow_table(snapshot_table, num_pages);
err_with_donation:
	__pkvm_hyp_donate_host(hyp_virt_to_pfn(original_table), num_pages);
err_donate:
	for (i = i - 1; i >= 0; i--) {
		baser_val = host_state->tables[i].val;
		if (!(baser_val & GITS_BASER_VALID))
			continue;

		original_table = kern_hyp_va(host_state->tables[i].base);
		num_pages = (1 << host_state->tables[i].order);

		if (baser_val & GITS_BASER_INDIRECT) {
			snapshot_table = kern_hyp_va(host_state->tables[i].base_snapshot);
			pkvm_unshare_shadow_table(snapshot_table, num_pages);

			pkvm_host_map_last_level(original_table, num_pages,
						 host_state->tables[i].psz);
		}

		WARN_ON(__pkvm_hyp_donate_host(hyp_virt_to_pfn(original_table), num_pages));
	}

	return ret;
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

	ret = pkvm_setup_its_shadow_baser(host_state);
	if (ret)
		goto err_with_shadow_cmdq;

	hyp_spin_lock_init(&priv_state->its_lock);

	priv_state->host_state = host_state;
	priv_state->base = (void __iomem *)__hyp_va(dev_addr);
	priv_state->cmd_original = host_state->cmd_original;
	priv_state->cmd_host_copy = host_state->cmd_host_copy;
	priv_state->empty_entry = 0;
	priv_state->num_tracked_entries = ((priv_num_pages << PAGE_SHIFT) -
		offsetof(struct its_priv_state, tracked_entries)) / sizeof(struct dte_entry);

	priv_state->cmd_offset = readq_relaxed(priv_state->base + GITS_CREADR) &
		GITS_CREADR_OFFSET;
	priv_state->needs_flush =
		(readq_relaxed(priv_state->base + GITS_CBASER) & GITS_CBASER_SHAREABILITY_MASK) !=
		GITS_CBASER_InnerShareable;

	its_reg->priv = priv_state;

	hyp_spin_unlock(&its_setup_lock);

	return 0;
err_with_shadow_cmdq:
	pkvm_teardown_its_shadow_cmdq(host_state);
err_with_host_state:
	WARN_ON(__pkvm_hyp_donate_host(hyp_virt_to_pfn(host_state), 1));
err_with_priv:
	WARN_ON(__pkvm_hyp_donate_host(hyp_virt_to_pfn(priv_state), 1));
err_unlock:
	hyp_spin_unlock(&its_setup_lock);
	return ret;
}

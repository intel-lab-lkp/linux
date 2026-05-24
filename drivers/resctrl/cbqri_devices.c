// SPDX-License-Identifier: GPL-2.0-only

#define pr_fmt(fmt) "%s:%s: " fmt, KBUILD_MODNAME, __func__

#include <linux/acpi.h>
#include <linux/bitfield.h>
#include <linux/cacheinfo.h>
#include <linux/riscv_cbqri.h>
#include <linux/cpu.h>
#include <linux/cpumask.h>
#include <linux/err.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/ioport.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/printk.h>
#include <linux/slab.h>
#include <linux/types.h>

#include <asm/csr.h>

#include "cbqri_internal.h"

LIST_HEAD(cbqri_controllers);

/* Set capacity block mask (cc_block_mask) */
static void cbqri_set_cbm(struct cbqri_controller *ctrl, u64 cbm)
{
	iowrite64(cbm, ctrl->base + CBQRI_CC_BLOCK_MASK_OFF);
}

static int cbqri_wait_busy_flag(struct cbqri_controller *ctrl, int reg_offset,
				u64 *regp)
{
	u64 reg;
	int ret;

	/*
	 * Sleeping poll: caller holds ctrl->lock as a sleeping mutex, so
	 * 10us/1ms is safe under PREEMPT_RT.
	 */
	ret = readq_poll_timeout(ctrl->base + reg_offset, reg,
				 !FIELD_GET(CBQRI_CONTROL_REGISTERS_BUSY_MASK, reg),
				 10, 1000);
	if (ret) {
		ctrl->faulted = true;
		return ret;
	}
	ctrl->faulted = false;
	if (regp)
		*regp = reg;
	return 0;
}

/*
 * Perform capacity allocation control operation on capacity controller.
 * Caller must hold ctrl->lock.
 */
static int cbqri_cc_alloc_op(struct cbqri_controller *ctrl, int operation,
			     int rcid, enum cbqri_at at)
{
	int reg_offset = CBQRI_CC_ALLOC_CTL_OFF;
	int status;
	u64 reg;

	lockdep_assert_held(&ctrl->lock);

	if (cbqri_wait_busy_flag(ctrl, reg_offset, &reg) < 0) {
		pr_err_ratelimited("BUSY timeout before starting operation\n");
		return -EIO;
	}
	FIELD_MODIFY(CBQRI_CONTROL_REGISTERS_OP_MASK, &reg, operation);
	FIELD_MODIFY(CBQRI_CONTROL_REGISTERS_RCID_MASK, &reg, rcid);

	/*
	 * CBQRI Table 1: AT 0=Data, 1=Code. Program AT on controllers
	 * that report supports_alloc_at_code. On controllers that don't,
	 * AT is reserved-zero and the op acts on both halves.
	 */
	reg &= ~CBQRI_CONTROL_REGISTERS_AT_MASK;
	if (ctrl->cc.supports_alloc_at_code)
		reg |= FIELD_PREP(CBQRI_CONTROL_REGISTERS_AT_MASK, at);

	iowrite64(reg, ctrl->base + reg_offset);

	if (cbqri_wait_busy_flag(ctrl, reg_offset, &reg) < 0) {
		pr_err_ratelimited("BUSY timeout during operation\n");
		return -EIO;
	}

	status = FIELD_GET(CBQRI_CONTROL_REGISTERS_STATUS_MASK, reg);
	if (status != CBQRI_CC_ALLOC_CTL_STATUS_SUCCESS) {
		pr_err_ratelimited("operation %d failed: status=%d\n", operation, status);
		return -EIO;
	}

	return 0;
}

/*
 * Issue a monitoring op on a CC or BC controller's mon_ctl register at
 * reg_offset (CBQRI_CC_MON_CTL_OFF or CBQRI_BC_MON_CTL_OFF). The CC and
 * BC mon_ctl registers share an identical OP/MCID/EVT_ID/STATUS layout, so
 * one helper covers both. Caller must hold ctrl->lock.
 */
int cbqri_mon_op(struct cbqri_controller *ctrl, int reg_offset,
		 int operation, int mcid, int evt_id, u64 *out_reg)
{
	u64 reg;

	lockdep_assert_held(&ctrl->lock);

	if (cbqri_wait_busy_flag(ctrl, reg_offset, &reg) < 0) {
		pr_err_ratelimited("BUSY timeout before starting operation\n");
		return -EIO;
	}
	FIELD_MODIFY(CBQRI_MON_CTL_OP_MASK, &reg, operation);
	FIELD_MODIFY(CBQRI_MON_CTL_MCID_MASK, &reg, mcid);
	FIELD_MODIFY(CBQRI_MON_CTL_EVT_ID_MASK, &reg, evt_id);
	iowrite64(reg, ctrl->base + reg_offset);

	if (cbqri_wait_busy_flag(ctrl, reg_offset, &reg) < 0) {
		pr_err_ratelimited("BUSY timeout\n");
		return -EIO;
	}

	if (FIELD_GET(CBQRI_MON_CTL_STATUS_MASK, reg) !=
	    CBQRI_MON_CTL_STATUS_SUCCESS)
		return -EIO;

	if (out_reg)
		*out_reg = reg;

	return 0;
}

/*
 * Apply a capacity block mask and verify via CONFIG_LIMIT + READ_LIMIT.
 *
 * AT-capable controllers with CDP off need a second CONFIG_LIMIT on the
 * other AT half (the spec encodes AT only as 0=Data / 1=Code, there is
 * no "both halves" value). CDP-on issues separate per-type writes from
 * resctrl, so a single CONFIG_LIMIT per call is correct.
 */
int cbqri_apply_cache_config(struct cbqri_controller *ctrl, u32 closid,
			     const struct cbqri_cc_config *cfg)
{
	bool need_at_mirror;
	u64 saved_cbm = 0;
	int err = 0;
	u64 reg;

	mutex_lock(&ctrl->lock);

	need_at_mirror = ctrl->cc.supports_alloc_at_code && !cfg->cdp_enabled;

	/*
	 * Capture the cfg->at half CBM before any write so a partial
	 * AT-mirror failure can revert and keep the two halves consistent.
	 * Pre-clear cc_block_mask so a silent firmware no-op (status
	 * SUCCESS but staging not updated) shows as a zero readback
	 * rather than carrying stale data from a prior op. Mirrors the
	 * defensive pattern in cbqri_read_cache_config().
	 */
	if (need_at_mirror) {
		cbqri_set_cbm(ctrl, 0);
		err = cbqri_cc_alloc_op(ctrl, CBQRI_CC_ALLOC_CTL_OP_READ_LIMIT,
					closid, cfg->at);
		if (err < 0)
			goto out;
		saved_cbm = ioread64(ctrl->base + CBQRI_CC_BLOCK_MASK_OFF);
	}

	/* Set capacity block mask (cc_block_mask) */
	cbqri_set_cbm(ctrl, cfg->cbm);

	/* Capacity config limit operation for the AT half implied by cfg->at */
	err = cbqri_cc_alloc_op(ctrl, CBQRI_CC_ALLOC_CTL_OP_CONFIG_LIMIT,
				closid, cfg->at);
	if (err < 0)
		goto out;

	/*
	 * CDP-off mirror: on AT-capable controllers, also program the
	 * other AT half with the same mask so the two halves stay in sync.
	 */
	if (need_at_mirror) {
		enum cbqri_at other = (cfg->at == CBQRI_AT_CODE) ?
				      CBQRI_AT_DATA : CBQRI_AT_CODE;

		cbqri_set_cbm(ctrl, cfg->cbm);
		err = cbqri_cc_alloc_op(ctrl,
					CBQRI_CC_ALLOC_CTL_OP_CONFIG_LIMIT,
					closid, other);
		if (err < 0) {
			int rerr;

			/*
			 * Best-effort revert of the cfg->at half so the two
			 * halves stay in sync. A schemata read sees only one
			 * half, so silent divergence would otherwise report
			 * the new value as if the write had succeeded.
			 */
			cbqri_set_cbm(ctrl, saved_cbm);
			rerr = cbqri_cc_alloc_op(ctrl,
						 CBQRI_CC_ALLOC_CTL_OP_CONFIG_LIMIT,
						 closid, cfg->at);
			if (rerr < 0)
				pr_err_ratelimited("AT-mirror revert failed (err=%d), AT halves diverged\n",
						   rerr);
			goto out;
		}
	}

	/* Clear cc_block_mask before read limit to verify op works */
	cbqri_set_cbm(ctrl, 0);

	/* Perform a capacity read limit operation to verify blockmask */
	err = cbqri_cc_alloc_op(ctrl, CBQRI_CC_ALLOC_CTL_OP_READ_LIMIT,
				closid, cfg->at);
	if (err < 0)
		goto out;

	/*
	 * Read capacity blockmask and narrow to u32 to match resctrl's CBM
	 * width. cbqri_probe_cc() rejects ncblks > 32 so the upper bits are
	 * reserved zero.
	 */
	reg = ioread64(ctrl->base + CBQRI_CC_BLOCK_MASK_OFF);
	if (lower_32_bits(reg) != cfg->cbm) {
		pr_err_ratelimited("CBM verify mismatch (reg=%llx != cbm=%llx)\n",
				   reg, cfg->cbm);
		err = -EIO;
	}

out:
	mutex_unlock(&ctrl->lock);
	return err;
}

/*
 * Read the configured CBM for closid on the at half via READ_LIMIT.
 * Pre-clears cc_block_mask before the op so a silent firmware no-op
 * (status SUCCESS but staging not updated) is detectable in cbm_out.
 */
int cbqri_read_cache_config(struct cbqri_controller *ctrl, u32 closid,
			    enum cbqri_at at, u32 *cbm_out)
{
	int err;

	mutex_lock(&ctrl->lock);
	cbqri_set_cbm(ctrl, 0);
	err = cbqri_cc_alloc_op(ctrl, CBQRI_CC_ALLOC_CTL_OP_READ_LIMIT, closid, at);
	if (err == 0) {
		/*
		 * cc_block_mask is a 64-bit MMIO register. resctrl exposes the
		 * CBM as a u32. cbqri_probe_cc() rejects ncblks > 32 so the
		 * upper 32 bits are reserved zero by the spec. Narrow
		 * explicitly via lower_32_bits() so the assumption is visible
		 * at the read site.
		 */
		*cbm_out = lower_32_bits(ioread64(ctrl->base + CBQRI_CC_BLOCK_MASK_OFF));
	}
	mutex_unlock(&ctrl->lock);
	return err;
}

static int cbqri_probe_feature(struct cbqri_controller *ctrl, int reg_offset,
			       int operation, int *status, bool *access_type_supported)
{
	const u64 active_mask = CBQRI_CONTROL_REGISTERS_OP_MASK |
				CBQRI_CONTROL_REGISTERS_AT_MASK |
				CBQRI_CONTROL_REGISTERS_RCID_MASK |
				CBQRI_MON_CTL_EVT_ID_MASK;
	u64 reg, saved_reg;
	int at;

	/*
	 * Default the output to false so the status==0 (feature not
	 * implemented) path returns a deterministic value to the caller
	 * rather than leaving an uninitialized bool.
	 */
	*access_type_supported = false;

	/* Keep the initial register value to preserve the WPRI fields */
	reg = ioread64(ctrl->base + reg_offset);
	saved_reg = reg;

	/* Drain any in-flight firmware op before issuing our own write. */
	if (cbqri_wait_busy_flag(ctrl, reg_offset, &saved_reg) < 0) {
		pr_err("BUSY timeout before probe operation\n");
		return -EIO;
	}

	/*
	 * Execute the requested operation with all active fields
	 * (OP/AT/RCID/EVT_ID) zeroed except OP itself. The same builder
	 * works for ALLOC_CTL and MON_CTL because every bit not in
	 * active_mask is WPRI and gets carried over from saved_reg. The
	 * AT and EVT_ID positions are reserved for the other register
	 * type, where writing zero is harmless.
	 */
	reg = (saved_reg & ~active_mask) |
	      FIELD_PREP(CBQRI_CONTROL_REGISTERS_OP_MASK, operation);
	iowrite64(reg, ctrl->base + reg_offset);
	if (cbqri_wait_busy_flag(ctrl, reg_offset, &reg) < 0) {
		pr_err_ratelimited("BUSY timeout during operation\n");
		return -EIO;
	}

	/* Get the operation status */
	*status = FIELD_GET(CBQRI_CONTROL_REGISTERS_STATUS_MASK, reg);

	/*
	 * Check for the AT support if the register is implemented
	 * (if not, the status value will remain 0)
	 */
	if (*status != 0) {
		/*
		 * Re-issue operation with AT=CODE so the controller
		 * latches AT=CODE on supported hardware (or resets it to 0
		 * on hardware that doesn't). OP must be a defined CBQRI op
		 * here. OP=0 is a no-op and would silently disable CDP.
		 */
		reg = (saved_reg & ~active_mask) |
		      FIELD_PREP(CBQRI_CONTROL_REGISTERS_OP_MASK, operation) |
		      FIELD_PREP(CBQRI_CONTROL_REGISTERS_AT_MASK,
				 CBQRI_CONTROL_REGISTERS_AT_CODE);
		iowrite64(reg, ctrl->base + reg_offset);
		if (cbqri_wait_busy_flag(ctrl, reg_offset, &reg) < 0) {
			pr_err("BUSY timeout setting AT field\n");
			return -EIO;
		}

		/*
		 * If the AT field value has been reset to zero,
		 * then the AT support is not present
		 */
		at = FIELD_GET(CBQRI_CONTROL_REGISTERS_AT_MASK, reg);
		if (at == CBQRI_CONTROL_REGISTERS_AT_CODE)
			*access_type_supported = true;
	}

	/* Restore the original register value. Clear OP to avoid re-triggering the probe op. */
	saved_reg &= ~CBQRI_CONTROL_REGISTERS_OP_MASK;
	iowrite64(saved_reg, ctrl->base + reg_offset);
	if (cbqri_wait_busy_flag(ctrl, reg_offset, NULL) < 0) {
		pr_err("BUSY timeout restoring register value\n");
		return -EIO;
	}

	return 0;
}

static int cbqri_probe_cc(struct cbqri_controller *ctrl)
{
	bool has_mon_at_code = false;
	int err, status;
	u64 reg;

	reg = ioread64(ctrl->base + CBQRI_CC_CAPABILITIES_OFF);
	if (reg == 0)
		return -ENODEV;

	ctrl->ver_minor = FIELD_GET(CBQRI_CC_CAPABILITIES_VER_MINOR_MASK, reg);
	ctrl->ver_major = FIELD_GET(CBQRI_CC_CAPABILITIES_VER_MAJOR_MASK, reg);
	ctrl->cc.ncblks = FIELD_GET(CBQRI_CC_CAPABILITIES_NCBLKS_MASK, reg);

	pr_debug("version=%d.%d ncblks=%d cache_level=%d\n",
		 ctrl->ver_major, ctrl->ver_minor,
		 ctrl->cc.ncblks, ctrl->cache.cache_level);

	/*
	 * NCBLKS == 0 would divide-by-zero in the schemata math while
	 * ctrl->lock is held.
	 */
	if (!ctrl->cc.ncblks) {
		pr_warn("CC at %pa has 0 capacity blocks, skipping\n",
			&ctrl->addr);
		return -ENODEV;
	}

	if (ctrl->cc.ncblks > 32) {
		pr_warn("CC at %pa has ncblks=%u > 32 (resctrl CBM is u32), skipping\n",
			&ctrl->addr, ctrl->cc.ncblks);
		return -ENODEV;
	}

	/*
	 * Resolve cache_size via cacheinfo. cpus_read_lock satisfies
	 * lockdep_assert_cpus_held() inside get_cpu_cacheinfo_level(). If
	 * every cpu_mask member is offline, cache_size stays 0 and the
	 * controller cannot back occupancy monitoring.
	 */
	cpus_read_lock();
	if (!ctrl->cache.cache_size) {
		int cpu = cpumask_first_and(&ctrl->cache.cpu_mask, cpu_online_mask);

		if (cpu < nr_cpu_ids) {
			struct cacheinfo *ci;

			ci = get_cpu_cacheinfo_level(cpu, ctrl->cache.cache_level);
			if (ci)
				ctrl->cache.cache_size = ci->size;
		}
	}
	cpus_read_unlock();

	/* Probe monitoring features */
	err = cbqri_probe_feature(ctrl, CBQRI_CC_MON_CTL_OFF,
				  CBQRI_CC_MON_CTL_OP_READ_COUNTER, &status,
				  &has_mon_at_code);
	if (err)
		return err;

	if (status == CBQRI_MON_CTL_STATUS_SUCCESS) {
		/*
		 * Occupancy is reported to userspace in bytes, computed as
		 * cache_size * counter / ncblks by the resctrl glue. If
		 * cacheinfo has no cache_size, leave mon_capable false so
		 * the file is not exposed at all rather than silently
		 * returning 0.
		 */
		if (!ctrl->cache.cache_size)
			pr_debug("CC @%pa: cache_size unknown, occupancy monitoring disabled\n",
				 &ctrl->addr);
		else
			ctrl->mon_capable = true;
	}

	/* Probe allocation features */
	err = cbqri_probe_feature(ctrl, CBQRI_CC_ALLOC_CTL_OFF,
				  CBQRI_CC_ALLOC_CTL_OP_READ_LIMIT,
				  &status, &ctrl->cc.supports_alloc_at_code);
	if (err)
		return err;

	if (status == CBQRI_CC_ALLOC_CTL_STATUS_SUCCESS)
		ctrl->alloc_capable = true;

	return 0;
}

static int cbqri_probe_controller(struct cbqri_controller *ctrl)
{
	int err;

	pr_debug("controller info: type=%d addr=%pa size=%pa max-rcid=%u max-mcid=%u\n",
		 ctrl->type, &ctrl->addr, &ctrl->size,
		 ctrl->rcid_count, ctrl->mcid_count);

	if (!ctrl->addr) {
		pr_warn("controller has invalid addr=0x0, skipping\n");
		return -EINVAL;
	}

	if (ctrl->size < CBQRI_CTRL_MIN_REG_SPAN) {
		pr_warn("controller at %pa: size %pa < minimum 0x%x, skipping\n",
			&ctrl->addr, &ctrl->size, CBQRI_CTRL_MIN_REG_SPAN);
		return -EINVAL;
	}

	if (!request_mem_region(ctrl->addr, ctrl->size, "cbqri_controller")) {
		pr_err("request_mem_region failed for %pa\n", &ctrl->addr);
		return -EBUSY;
	}

	ctrl->base = ioremap(ctrl->addr, ctrl->size);
	if (!ctrl->base) {
		pr_err("ioremap failed for %pa\n", &ctrl->addr);
		err = -ENOMEM;
		goto err_release;
	}

	switch (ctrl->type) {
	case CBQRI_CONTROLLER_TYPE_CAPACITY:
		err = cbqri_probe_cc(ctrl);
		break;
	default:
		pr_err("unknown controller type %d\n", ctrl->type);
		err = -ENODEV;
		break;
	}

	if (err)
		goto err_iounmap;

	return 0;

err_iounmap:
	iounmap(ctrl->base);
	ctrl->base = NULL;
err_release:
	release_mem_region(ctrl->addr, ctrl->size);
	return err;
}

/*
 * Pre-arm every MCID with the Occupancy event so a subsequent READ_COUNTER
 * just snapshots the live counter rather than re-configuring the slot.
 * Called once per CC during resctrl-side cpuhp online for the L3 monitoring
 * domain.
 */
int cbqri_init_mon_counters(struct cbqri_controller *ctrl)
{
	int i, err;

	for (i = 0; i < ctrl->mcid_count; i++) {
		mutex_lock(&ctrl->lock);
		err = cbqri_mon_op(ctrl, CBQRI_CC_MON_CTL_OFF,
				   CBQRI_CC_MON_CTL_OP_CONFIG_EVENT,
				   i, CBQRI_CC_EVT_ID_OCCUPANCY, NULL);
		mutex_unlock(&ctrl->lock);
		if (err)
			return err;
	}
	return 0;
}

void cbqri_controller_destroy(struct cbqri_controller *ctrl)
{
	/*
	 * cbqri_probe_controller() clears ctrl->base on its error paths and
	 * releases the mem region itself, so reach into both only when
	 * destroy is rolling back a successful probe.
	 */
	if (ctrl->base) {
		iounmap(ctrl->base);
		release_mem_region(ctrl->addr, ctrl->size);
	}
	kfree(ctrl);
}

/*
 * Roll back the most recent n successful riscv_cbqri_register_controller()
 * calls. Discovery layers use this to undo partial registrations when a
 * subsequent table entry turns out to be malformed and the whole parse must
 * abort.
 *
 * Caller serialization: this is intended for boot-time discovery (ACPI
 * acpi_arch_init, future DT) which run single-threaded before late_initcall.
 * No lock is taken.
 */
void riscv_cbqri_unregister_last(unsigned int n)
{
	while (n--) {
		struct cbqri_controller *ctrl;

		if (list_empty(&cbqri_controllers))
			return;
		ctrl = list_last_entry(&cbqri_controllers,
				       struct cbqri_controller, list);
		list_del(&ctrl->list);
		cbqri_controller_destroy(ctrl);
	}
}

/*
 * Allocate, populate, and add to cbqri_controllers a fresh controller
 * descriptor based on info supplied by a discovery layer (ACPI RQSC,
 * future DT). Resolves the cpumask via PPTT (capacity) so callers do
 * not need to know about cacheinfo topology.
 */
int riscv_cbqri_register_controller(const struct cbqri_controller_info *info)
{
	struct cbqri_controller *ctrl;
	int err;

	if (!info->addr) {
		pr_warn("skipping controller with invalid addr=0x0\n");
		return -EINVAL;
	}

	ctrl = kzalloc(sizeof(*ctrl), GFP_KERNEL);
	if (!ctrl)
		return -ENOMEM;

	mutex_init(&ctrl->lock);

	ctrl->addr = info->addr;
	ctrl->size = info->size;
	ctrl->type = info->type;
	ctrl->rcid_count = info->rcid_count;
	ctrl->mcid_count = info->mcid_count;

	/*
	 * SRMCFG encodes RCID in 12 bits.  ACPI's acpi_parse_rqsc() already
	 * caps info->rcid_count at CBQRI_MAX_RCID (1024) so this is unreachable
	 * today, but a future DT discovery path or a malformed firmware table
	 * routed through a different validator could bypass that ceiling.
	 * Catch the violation here rather than silently truncating in every
	 * FIELD_PREP(SRMCFG_RCID_MASK, closid) on the schedule-in fast path.
	 */
	if (WARN_ON_ONCE(ctrl->rcid_count > SRMCFG_RCID_MASK)) {
		cbqri_controller_destroy(ctrl);
		return -EINVAL;
	}

	switch (info->type) {
	case CBQRI_CONTROLLER_TYPE_CAPACITY: {
		int level;

		ctrl->cache.cache_id = info->cache_id;

		level = find_acpi_cache_level_from_id(info->cache_id);
		if (level < 0) {
			pr_warn("Failed to resolve cache level for cache id 0x%x (%d), skipping\n",
				info->cache_id, level);
			cbqri_controller_destroy(ctrl);
			return level;
		}
		ctrl->cache.cache_level = level;

		/*
		 * cache_size stays at 0 here. cacheinfo is not populated
		 * yet at acpi_arch_init time. Filled lazily during probe
		 * via get_cpu_cacheinfo_level().
		 */

		err = acpi_pptt_get_cpumask_from_cache_id(info->cache_id,
							  &ctrl->cache.cpu_mask);
		if (err) {
			pr_warn("Failed to get cpumask for cache id 0x%x (%d), skipping\n",
				info->cache_id, err);
			cbqri_controller_destroy(ctrl);
			return err;
		}
		break;
	}
	default:
		pr_warn("controller at %pa: unknown type %u, skipping\n",
			&ctrl->addr, info->type);
		cbqri_controller_destroy(ctrl);
		return -EINVAL;
	}

	err = cbqri_probe_controller(ctrl);
	if (err) {
		cbqri_controller_destroy(ctrl);
		return err;
	}

	list_add_tail(&ctrl->list, &cbqri_controllers);
	return 0;
}

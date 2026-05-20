// SPDX-License-Identifier: GPL-2.0
/*
 * SiTime SiT9531x DPLL core driver
 *
 * Copyright (C) 2026 SiTime Corp.
 * Author: Ali Rouhi <arouhi@sitime.com>
 * Author: Oleg Zadorozhnyi <Oleg.Zadorozhnyi@devoxsoftware.com>
 *
 * Core I2C probe, regmap configuration, hardware state management,
 * and periodic work thread.
 */

#include <linux/bitfield.h>
#include <linux/bits.h>
#include <linux/clk.h>
#include <linux/dev_printk.h>
#include <linux/device.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/kthread.h>
#include <linux/module.h>
#include <linux/regmap.h>
#include <linux/slab.h>

#include "core.h"
#include "dpll.h"
#include "regs.h"

/* ====================================================================
 * Chip variant table
 * ====================================================================
 */

#define SIT9531X_CHIP(_id, _nin, _nout, _name, _map) \
	{ .id = (_id), .num_inputs = (_nin), .num_outputs = (_nout), \
	  .name = (_name), .clkout_map = (_map) }

/* Per-variant output index -> physical slot mapping */
static const u8 clkout_map_95317[] = {0, 3, 4, 5, 7, 8, 9, 11};
static const u8 clkout_map_95316[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};

static const struct sit9531x_chip_info sit9531x_chip_ids[] = {
	SIT9531X_CHIP(SIT9531X_VARIANT_ID_95317,  4,  8, "SiT95317", clkout_map_95317),
	SIT9531X_CHIP(SIT9531X_VARIANT_ID_95316,  4, 12, "SiT95316", clkout_map_95316),
};

/* ====================================================================
 * Regmap configuration
 * ====================================================================
 */

#define SIT9531X_RANGE_OFFSET	SIT9531X_PAGE_SIZE

static const struct regmap_range_cfg sit9531x_regmap_range = {
	.range_min	= SIT9531X_RANGE_OFFSET,
	.range_max	= SIT9531X_RANGE_OFFSET +
			  (SIT9531X_NUM_PAGES * SIT9531X_PAGE_SIZE) - 1,
	.selector_reg	= SIT9531X_PAGE_SEL,
	.selector_mask	= GENMASK(7, 0),
	.selector_shift	= 0,
	.window_start	= 0,
	.window_len	= SIT9531X_PAGE_SIZE,
};

const struct regmap_config sit9531x_regmap_config = {
	.reg_bits	= 8,
	.val_bits	= 8,
	.max_register	= SIT9531X_RANGE_OFFSET +
			  (SIT9531X_NUM_PAGES * SIT9531X_PAGE_SIZE) - 1,
	.ranges		= &sit9531x_regmap_range,
	.num_ranges	= 1,
	.cache_type	= REGCACHE_NONE,
};

/* ====================================================================
 * Register access helpers
 * ====================================================================
 */

/**
 * sit9531x_read_u8 - read an 8-bit register
 * @sitdev:	device pointer
 * @reg:	register in SIT9531X_REG(page, offset) form
 * @val:	output value
 *
 * Return: 0 on success, <0 on error
 */
int sit9531x_read_u8(struct sit9531x_dev *sitdev, unsigned int reg,
		     u8 *val)
{
	unsigned int tmp;
	int rc;

	reg = (SIT9531X_REG_PAGE(reg) * SIT9531X_PAGE_SIZE) +
	      SIT9531X_REG_OFFSET(reg) + SIT9531X_RANGE_OFFSET;

	rc = regmap_read(sitdev->regmap, reg, &tmp);
	if (rc)
		dev_err(sitdev->dev, "Failed to read reg 0x%04x: %d\n",
			reg, rc);
	else
		*val = (u8)tmp;

	return rc;
}

/**
 * sit9531x_write_u8 - write an 8-bit register
 * @sitdev:	device pointer
 * @reg:	register in SIT9531X_REG(page, offset) form
 * @val:	value to write
 *
 * Return: 0 on success, <0 on error
 */
int sit9531x_write_u8(struct sit9531x_dev *sitdev, unsigned int reg,
		      u8 val)
{
	int rc;

	reg = (SIT9531X_REG_PAGE(reg) * SIT9531X_PAGE_SIZE) +
	      SIT9531X_REG_OFFSET(reg) + SIT9531X_RANGE_OFFSET;

	rc = regmap_write(sitdev->regmap, reg, val);
	if (rc)
		dev_err(sitdev->dev, "Failed to write reg 0x%04x: %d\n",
			reg, rc);

	return rc;
}

/**
 * sit9531x_read_pll_u8 - read a register on a PLL page
 * @sitdev:	device pointer
 * @pll_idx:	PLL index (0-3)
 * @offset:	register offset within the PLL page
 * @val:	output value
 *
 * Return: 0 on success, <0 on error
 */
int sit9531x_read_pll_u8(struct sit9531x_dev *sitdev, u8 pll_idx,
			 u8 offset, u8 *val)
{
	return sit9531x_read_u8(sitdev,
				SIT9531X_REG(sit9531x_pll_page(pll_idx), offset),
				val);
}

/**
 * sit9531x_write_pll_u8 - write a register on a PLL page
 * @sitdev:	device pointer
 * @pll_idx:	PLL index (0-3)
 * @offset:	register offset within the PLL page
 * @val:	value to write
 *
 * Return: 0 on success, <0 on error
 */
int sit9531x_write_pll_u8(struct sit9531x_dev *sitdev, u8 pll_idx,
			  u8 offset, u8 val)
{
	return sit9531x_write_u8(sitdev,
				 SIT9531X_REG(sit9531x_pll_page(pll_idx), offset),
				 val);
}

/**
 * sit9531x_update_pll_u8 - read-modify-write a register on a PLL page
 * @sitdev:	device pointer
 * @pll_idx:	PLL index (0-3)
 * @offset:	register offset within the PLL page
 * @mask:	bits to modify
 * @val:	new value for masked bits
 *
 * Return: 0 on success, <0 on error
 */
int sit9531x_update_pll_u8(struct sit9531x_dev *sitdev, u8 pll_idx,
			   u8 offset, u8 mask, u8 val)
{
	unsigned int reg;

	reg = (sit9531x_pll_page(pll_idx) * SIT9531X_PAGE_SIZE) +
	      offset + SIT9531X_RANGE_OFFSET;

	return regmap_update_bits(sitdev->regmap, reg, mask, val);
}

/* ====================================================================
 * Input enable / disable
 * ====================================================================
 */

/*
 * sit9531x_input_get_regs - get force mask and state register addresses
 * @ref:	input reference state (contains sig_mode and polarity)
 * @force_reg:	output force mask register address
 * @state_reg:	output state register address
 *
 * Selects the correct Page 0x02 register pair based on signal mode
 * and polarity.
 */
static void sit9531x_input_get_regs(const struct sit9531x_ref *ref,
				    unsigned int *force_reg,
				    unsigned int *state_reg)
{
	if (ref->sig_mode == SIT9531X_MODE_DE) {
		*force_reg = SIT9531X_REG_IN_DE_FORCE;
		*state_reg = SIT9531X_REG_IN_DE_STATE;
	} else if (ref->polarity == SIT9531X_POL_N) {
		*force_reg = SIT9531X_REG_IN_SEN_FORCE;
		*state_reg = SIT9531X_REG_IN_SEN_STATE;
	} else {
		*force_reg = SIT9531X_REG_IN_SEP_FORCE;
		*state_reg = SIT9531X_REG_IN_SEP_STATE;
	}
}

/**
 * sit9531x_input_disable - disable an input reference
 * @sitdev:	device pointer
 * @index:	input index (0-N)
 *
 * Sets the force mask bit and clears the state bit for the given
 * input, effectively disabling it.  Register selection depends on
 * the input's signal mode (SE/DE) and polarity (P/N).
 *
 * Return: 0 on success, <0 on error
 */
int sit9531x_input_disable(struct sit9531x_dev *sitdev, u8 index)
{
	struct sit9531x_ref *ref = &sitdev->ref[index];
	unsigned int force_reg, state_reg;
	u8 val;
	int rc;

	sit9531x_input_get_regs(ref, &force_reg, &state_reg);

	/* Set force mask bit: enable override */
	rc = sit9531x_read_u8(sitdev, force_reg, &val);
	if (rc)
		return rc;
	rc = sit9531x_write_u8(sitdev, force_reg, val | BIT(index));
	if (rc)
		return rc;

	/* Clear state bit: 0 = disabled */
	rc = sit9531x_read_u8(sitdev, state_reg, &val);
	if (rc)
		return rc;
	rc = sit9531x_write_u8(sitdev, state_reg, val & ~BIT(index));
	if (rc)
		return rc;

	ref->enabled = false;

	return 0;
}

/**
 * sit9531x_input_enable - enable an input reference
 * @sitdev:	device pointer
 * @index:	input index (0-N)
 *
 * Clears the force mask bit for the given input, returning it to
 * hardware default (enabled).
 *
 * Return: 0 on success, <0 on error
 */
int sit9531x_input_enable(struct sit9531x_dev *sitdev, u8 index)
{
	struct sit9531x_ref *ref = &sitdev->ref[index];
	unsigned int force_reg, state_reg;
	u8 val;
	int rc;

	sit9531x_input_get_regs(ref, &force_reg, &state_reg);

	/* Clear force mask bit: return to hardware default (enabled) */
	rc = sit9531x_read_u8(sitdev, force_reg, &val);
	if (rc)
		return rc;
	rc = sit9531x_write_u8(sitdev, force_reg, val & ~BIT(index));
	if (rc)
		return rc;

	ref->enabled = true;

	return 0;
}

/* ====================================================================
 * Output enable / disable (Hi-Z control)
 *
 * SiT9531x outputs can be configured as differential (DIFF) or
 * single-ended (SE) depending on the factory blob.  Each output slot
 * has TWO Hi-Z force/state register pairs on Page 0x03 -- one for the
 * DIFF path, one for the SE path.
 *
 * We write to BOTH pairs so the function mutes the output regardless
 * of whether it's been configured DIFF or SE on this board.
 *
 *   slot 0-7 :
 *     DIFF mask=0xF2 state=0xF3   SE mask=0xF8 state=0xF9
 *   slot 8-11:
 *     DIFF mask=0xF4 state=0xF5   SE mask=0xFA state=0xFB
 *
 * MASK bit = 1  -> driver takes control of that output's Hi-Z state
 * STATE bit = 0 -> output is forced to Hi-Z (muted)
 * STATE bit = 1 -> output is driven (active)
 *
 * The output "index" in the driver is logical; the physical slot comes
 * from info->clkout_map[].
 * ====================================================================
 */

struct sit9531x_hiz_regs {
	unsigned int diff_mask;
	unsigned int diff_state;
	unsigned int se_mask;
	unsigned int se_state;
	u8 bit;
};

static void sit9531x_output_get_hiz_regs(u8 slot,
					 struct sit9531x_hiz_regs *r)
{
	if (slot <= 7) {
		r->diff_mask  = SIT9531X_REG_HIZ_DIFF_07_MASK;
		r->diff_state = SIT9531X_REG_HIZ_DIFF_07_STATE;
		r->se_mask    = SIT9531X_REG_HIZ_SE_07_MASK;
		r->se_state   = SIT9531X_REG_HIZ_SE_07_STATE;
		r->bit = slot;
	} else {
		r->diff_mask  = SIT9531X_REG_HIZ_DIFF_811_MASK;
		r->diff_state = SIT9531X_REG_HIZ_DIFF_811_STATE;
		r->se_mask    = SIT9531X_REG_HIZ_SE_811_MASK;
		r->se_state   = SIT9531X_REG_HIZ_SE_811_STATE;
		r->bit = slot - 8;
	}
}

static int sit9531x_hiz_set_bit(struct sit9531x_dev *sitdev,
				unsigned int reg, u8 bit, bool set)
{
	u8 cur, new_val;
	int rc;

	rc = sit9531x_read_u8(sitdev, reg, &cur);
	if (rc)
		return rc;

	new_val = set ? (cur | BIT(bit)) : (cur & ~BIT(bit));

	return sit9531x_write_u8(sitdev, reg, new_val);
}

/**
 * sit9531x_output_disable - mute an output (force Hi-Z)
 * @sitdev:	device pointer
 * @index:	logical output index (0..info->num_outputs-1)
 *
 * Sets MASK+STATE on BOTH the DIFF and SE register pairs so that the
 * output is muted regardless of its electrical configuration.
 *
 * Caller must hold sitdev->multiop_lock.
 *
 * Return: 0 on success, <0 on error
 */
int sit9531x_output_disable(struct sit9531x_dev *sitdev, u8 index)
{
	const struct sit9531x_chip_info *info = sitdev->info;
	struct sit9531x_hiz_regs r;
	u8 slot;
	int rc;

	lockdep_assert_held(&sitdev->multiop_lock);

	if (index >= info->num_outputs)
		return -EINVAL;

	slot = info->clkout_map[index];
	sit9531x_output_get_hiz_regs(slot, &r);

	/* Take control (MASK=1) and mute (STATE=0) on both DIFF and SE */
	rc = sit9531x_hiz_set_bit(sitdev, r.diff_mask, r.bit, true);
	if (rc)
		return rc;
	rc = sit9531x_hiz_set_bit(sitdev, r.diff_state, r.bit, false);
	if (rc)
		return rc;
	rc = sit9531x_hiz_set_bit(sitdev, r.se_mask, r.bit, true);
	if (rc)
		return rc;
	rc = sit9531x_hiz_set_bit(sitdev, r.se_state, r.bit, false);
	if (rc)
		return rc;

	sitdev->out[index].enabled = false;
	return 0;
}

/**
 * sit9531x_output_enable - un-mute an output (active state)
 * @sitdev:	device pointer
 * @index:	logical output index (0..info->num_outputs-1)
 *
 * Releases MASK on BOTH register pairs so the output returns to
 * whatever the initial_config blob programmed.
 *
 * Caller must hold sitdev->multiop_lock.
 *
 * Return: 0 on success, <0 on error
 */
int sit9531x_output_enable(struct sit9531x_dev *sitdev, u8 index)
{
	const struct sit9531x_chip_info *info = sitdev->info;
	struct sit9531x_hiz_regs r;
	u8 slot;
	int rc;

	lockdep_assert_held(&sitdev->multiop_lock);

	if (index >= info->num_outputs)
		return -EINVAL;

	slot = info->clkout_map[index];
	sit9531x_output_get_hiz_regs(slot, &r);

	/* Release control (MASK=0) on both DIFF and SE */
	rc = sit9531x_hiz_set_bit(sitdev, r.diff_mask, r.bit, false);
	if (rc)
		return rc;
	rc = sit9531x_hiz_set_bit(sitdev, r.se_mask, r.bit, false);
	if (rc)
		return rc;

	sitdev->out[index].enabled = true;
	return 0;
}

/* ====================================================================
 * Input priority selection
 *
 * The SiT9531x has a 12-slot priority table per PLL on Page 1.  Each
 * register holds two slots nibble-packed (even slot in [3:0], odd slot
 * in [7:4]).
 *
 * The procedure:
 *   1. Force PLL into holdover (PLL page reg 0x6F bit 4)
 *   2. Enter PRG_CMD state (Page 0 reg 0x0F <- 0x01)
 *   3. Write priority slots on Page 1
 *   4. NVM update + loop lock (Page 0 reg 0x0F)
 *   5. Release holdover
 *
 * Caller must hold sitdev->multiop_lock.
 * ====================================================================
 */

/**
 * sit9531x_input_prio_set - set input priority for a PLL
 * @sitdev:	device pointer
 * @pll_idx:	PLL index (0-3)
 * @input_idx:	input source index (0-11, using the p_dic encoding)
 * @prio:	priority slot position (0 = highest)
 *
 * Writes a single priority slot assignment.  The input source is
 * placed at the requested slot, and all lower-priority (higher-numbered)
 * slots are filled with the same source to avoid stale entries.
 *
 * Return: 0 on success, <0 on error
 */
int sit9531x_input_prio_set(struct sit9531x_dev *sitdev, u8 pll_idx,
			    u8 input_idx, u8 prio)
{
	u8 pll_offset, reg_addr, val, slot;
	int rc;

	lockdep_assert_held(&sitdev->multiop_lock);

	if (pll_idx >= SIT9531X_NUM_PLLS)
		return -EINVAL;
	if (input_idx >= SIT9531X_PRIO_MAX_SLOTS)
		return -EINVAL;
	if (prio >= SIT9531X_PRIO_MAX_SLOTS)
		return -EINVAL;

	pll_offset = SIT9531X_PRIO_REGS_PER_PLL * pll_idx;

	/* Step 1: Force PLL into holdover */
	rc = sit9531x_update_pll_u8(sitdev, pll_idx,
				    SIT9531X_PLL_REG_HO_CTRL,
				    BIT(SIT9531X_PLL_HO_FORCE_BIT),
				    BIT(SIT9531X_PLL_HO_FORCE_BIT));
	if (rc)
		return rc;

	usleep_range(10000, 12000);

	/* Step 2: Enter PRG_CMD state (Page 0, reg 0x0F) */
	rc = sit9531x_write_u8(sitdev, SIT9531X_REG_GLOBAL_UPDATE,
			       SIT9531X_PRG_CMD_STATE);
	if (rc)
		goto release_ho;

	/* Step 3: Write requested priority slot on Page 1 */
	reg_addr = SIT9531X_PRIO_BASE_REG + pll_offset + (prio / 2);

	rc = sit9531x_read_u8(sitdev,
			      SIT9531X_REG(SIT9531X_PAGE_PRIOSYS, reg_addr), &val);
	if (rc)
		goto release_ho;

	if (prio & 1) {
		/* Odd slot: bits [7:4] */
		val = (val & SIT9531X_PRIO_NIBBLE_MASK) |
		      (input_idx << SIT9531X_PRIO_HI_SHIFT);
	} else {
		/* Even slot: bits [3:0] */
		val = (val & (SIT9531X_PRIO_NIBBLE_MASK << SIT9531X_PRIO_HI_SHIFT)) |
		      (input_idx & SIT9531X_PRIO_NIBBLE_MASK);
	}

	rc = sit9531x_write_u8(sitdev,
			       SIT9531X_REG(SIT9531X_PAGE_PRIOSYS, reg_addr), val);
	if (rc)
		goto release_ho;

	/*
	 * Fill remaining slots (prio+1 .. 11) with the same source,
	 * matching the procedure script's backfill behaviour.
	 */
	for (slot = prio + 1; slot < SIT9531X_PRIO_MAX_SLOTS; slot++) {
		reg_addr = SIT9531X_PRIO_BASE_REG + pll_offset + (slot / 2);

		rc = sit9531x_read_u8(sitdev,
				      SIT9531X_REG(SIT9531X_PAGE_PRIOSYS, reg_addr),
				      &val);
		if (rc)
			goto release_ho;

		if (slot & 1)
			val = (val & SIT9531X_PRIO_NIBBLE_MASK) |
			      (input_idx << SIT9531X_PRIO_HI_SHIFT);
		else
			val = (val & (SIT9531X_PRIO_NIBBLE_MASK <<
				      SIT9531X_PRIO_HI_SHIFT)) |
			      (input_idx & SIT9531X_PRIO_NIBBLE_MASK);

		rc = sit9531x_write_u8(sitdev,
				       SIT9531X_REG(SIT9531X_PAGE_PRIOSYS, reg_addr),
				       val);
		if (rc)
			goto release_ho;
	}

	/* Step 4: NVM update */
	rc = sit9531x_write_u8(sitdev, SIT9531X_REG_GLOBAL_UPDATE,
			       SIT9531X_UPDATE_NVM);
	if (rc)
		goto release_ho;

	/* Step 5: Loop lock */
	rc = sit9531x_write_u8(sitdev, SIT9531X_REG_GLOBAL_UPDATE,
			       SIT9531X_LOOP_LOCK);
	if (rc)
		goto release_ho;

	usleep_range(1000, 2000);

release_ho:
	/* Step 6: Release holdover */
	sit9531x_update_pll_u8(sitdev, pll_idx,
			       SIT9531X_PLL_REG_HO_CTRL,
			       BIT(SIT9531X_PLL_HO_FORCE_BIT), 0);

	return rc;
}

/* ====================================================================
 * DCO tuning (inner and outer loop)
 *
 * The SiT9531x DCO mechanism adjusts PLL frequency in ppb via
 * fractional divider manipulation.
 *
 * Inner loop (free-run mode):
 *   - Reads DIVN integer/fraction from PLL page 0x30-0x3B
 *   - Computes 48-bit DCO fractional word
 *   - Writes to PLL page regs 0x51-0x56
 *
 * Outer loop (sync mode):
 *   - Reads DIVN2 integer/fraction from PLL page 0x3E-0x4C
 *   - Computes 24-bit integer + 32-bit fractional DCO word
 *   - Writes to PLL page regs 0x5E-0x60 (int), 0x63-0x66 (frac)
 *
 * After writing DCO codes, a trigger pulse on Page 0 reg 0x64
 * applies the increment (bit 6) or decrement (bit 4).
 * ====================================================================
 */

/**
 * struct sit9531x_dco_code - computed DCO register values
 * @dco_frac:	48-bit inner loop fractional word
 * @dco_int:	24-bit outer loop integer word (signed)
 * @outer_frac:	32-bit outer loop fractional word
 */
struct sit9531x_dco_code {
	u64	dco_frac;
	s32	dco_int;
	u64	outer_frac;
};

/*
 * sit9531x_dco_read_divn - read DIVN values for free-run DCO calculation
 * @sitdev:	device pointer
 * @pll_idx:	PLL index
 * @int_part:	output integer part
 * @fracn:	output fractional numerator
 * @fracd:	output fractional denominator
 *
 * Return: 0 on success, <0 on error
 */
static int sit9531x_dco_read_divn(struct sit9531x_dev *sitdev, u8 pll_idx,
				  u32 *int_part, s32 *fracn, u32 *fracd)
{
	u8 v, pbyq;
	int rc, i;

	/* Integer part */
	rc = sit9531x_read_pll_u8(sitdev, pll_idx, SIT9531X_PLL_REG_DIVN_INT, &v);
	if (rc)
		return rc;
	*int_part = v;

	/* Numerator (4 bytes, little-endian) */
	*fracn = 0;
	for (i = 3; i >= 0; i--) {
		rc = sit9531x_read_pll_u8(sitdev, pll_idx,
					  SIT9531X_PLL_REG_DIVN_NUM + i, &v);
		if (rc)
			return rc;
		*fracn = (*fracn << 8) | v;
	}

	/* Check P/Q enable bit (reg 0x31 bit 6) */
	rc = sit9531x_read_pll_u8(sitdev, pll_idx, SIT9531X_PLL_REG_STATUS, &v);
	if (rc)
		return rc;
	pbyq = (v >> 6) & 1;

	if (pbyq) {
		/* Denominator (4 bytes, little-endian) */
		*fracd = 0;
		for (i = 3; i >= 0; i--) {
			rc = sit9531x_read_pll_u8(sitdev, pll_idx,
						  SIT9531X_PLL_REG_DIVN_DEN + i,
						  &v);
			if (rc)
				return rc;
			*fracd = (*fracd << 8) | v;
		}
	} else {
		/*
		 * P/Q disabled: integer-only mode.
		 * Set fracd = 1 to avoid division by zero.
		 */
		*fracn = 0;
		*fracd = 1;
	}

	return 0;
}

/*
 * sit9531x_dco_read_divn2 - read DIVN2 values for sync DCO calculation
 * @sitdev:	device pointer
 * @pll_idx:	PLL index
 * @n2_int:	output integer part
 * @n2_fracn:	output fractional numerator
 * @n2_fracd:	output fractional denominator
 *
 * Return: 0 on success, <0 on error
 */
static int sit9531x_dco_read_divn2(struct sit9531x_dev *sitdev, u8 pll_idx,
				   s64 *n2_int, s32 *n2_fracn, u32 *n2_fracd)
{
	int rc, i;
	u8 v;

	/* Integer part (5 bytes, big-endian: 0x42 is MSB) */
	*n2_int = 0;
	for (i = 4; i >= 0; i--) {
		rc = sit9531x_read_pll_u8(sitdev, pll_idx,
					  SIT9531X_PLL_REG_DIVN2_INT + i, &v);
		if (rc)
			return rc;
		if (i == 4)
			v &= 0x07;  /* only 3 bits in MSB */
		*n2_int = (*n2_int << 8) | v;
	}

	/* Fractional numerator (4 bytes, big-endian: 0x46 is MSB) */
	*n2_fracn = 0;
	for (i = 3; i >= 0; i--) {
		rc = sit9531x_read_pll_u8(sitdev, pll_idx,
					  SIT9531X_PLL_REG_DIVN2_FRAC_NUM + i,
					  &v);
		if (rc)
			return rc;
		*n2_fracn = (*n2_fracn << 8) | v;
	}

	/* Fractional denominator (4 bytes, big-endian: 0x4C is MSB) */
	*n2_fracd = 0;
	for (i = 3; i >= 0; i--) {
		rc = sit9531x_read_pll_u8(sitdev, pll_idx,
					  SIT9531X_PLL_REG_DIVN2_FRAC_DEN + i,
					  &v);
		if (rc)
			return rc;
		*n2_fracd = (*n2_fracd << 8) | v;
	}

	return 0;
}

/*
 * sit9531x_dco_calc_inner - compute inner loop DCO code (free-run)
 *
 * Formula:
 *   divn_code = int_part * 65536 + fracn * 65536 / fracd
 *   dco_frac  = divn_code * ppb * fracd / 1e9
 *
 * Caller guarantees ppb >= 0; sign handled by trigger pulse.
 */
static void sit9531x_dco_calc_inner(u32 int_part, s32 fracn, u32 fracd,
				    s64 ppb, struct sit9531x_dco_code *code)
{
	s32 frac_scaled;
	u64 divn_code;

	if (!fracd)
		fracd = 1;

	frac_scaled = (s32)((s64)fracn * 65536 / fracd);
	divn_code = (u64)int_part * 65536 + frac_scaled;

	/*
	 * The triple multiply (divn_code * ppb * fracd) overflows u64 in
	 * realistic ranges (divn_code ~ 2^21, ppb ~ 2^30, fracd ~ 2^32 =
	 * ~2^83).  Use mul_u64_u64_div_u64() to carry out the multiply in
	 * 128 bits internally.
	 */
	code->dco_frac = mul_u64_u64_div_u64(divn_code,
					     (u64)ppb * fracd,
					     1000000000ULL);
}

/*
 * sit9531x_dco_calc_outer - compute outer loop DCO code (sync)
 *
 * The previous formulation computed divn2 with truncating integer
 * division (n2_int + n2_fracn / n2_fracd), silently dropping the
 * fractional part of DIVN2.  Mirror the inner-loop scale-then-divide
 * pattern by keeping the un-divided numerator:
 *   divn2_num = n2_int * n2_fracd + n2_fracn       (exact)
 *   divn2     = divn2_num / n2_fracd               (conceptually)
 *   dco_int   = divn2 * ppb / 1e9
 *             = divn2_num * ppb / (n2_fracd * 1e9)
 *   outer_frac = (divn2 * ppb / 1e9 - dco_int) * n2_fracd * 65536
 *              = divn2_num * (ppb * 65536) / 1e9 - dco_int * n2_fracd * 65536
 *
 * Caller guarantees ppb >= 0; sign handled by trigger pulse.
 */
static void sit9531x_dco_calc_outer(s64 n2_int, s32 n2_fracn, u32 n2_fracd,
				    s64 ppb, struct sit9531x_dco_code *code)
{
	u64 divn2_num, dco_int_u, adjustment_frac;
	u32 fracd_eff;

	fracd_eff = n2_fracd ? n2_fracd : 1;
	divn2_num = (u64)n2_int * fracd_eff + (u32)n2_fracn;

	/*
	 * dco_int = divn2_num * ppb / (fracd_eff * 1e9).  The (divn2_num
	 * * ppb) product can exceed u64 in the worst case, so use
	 * mul_u64_u64_div_u64() which carries it out in 128 bits.
	 */
	dco_int_u = mul_u64_u64_div_u64(divn2_num, (u64)ppb,
					(u64)fracd_eff * 1000000000ULL);
	code->dco_int = (s32)dco_int_u;

	/*
	 * outer_frac = divn2_num * (ppb << 16) / 1e9
	 *              - dco_int_u * fracd_eff * 65536
	 *
	 * The first term overflows u64 if computed as a flat multiply
	 * (divn2_num ~ 2^67 in the worst case); the 128-bit helper takes
	 * care of it.
	 */
	adjustment_frac = mul_u64_u64_div_u64(divn2_num,
					      (u64)ppb << 16,
					      1000000000ULL);
	code->outer_frac = adjustment_frac -
			   dco_int_u * fracd_eff * 65536;
}

/*
 * sit9531x_dco_write_inner - write inner loop DCO registers
 */
static int sit9531x_dco_write_inner(struct sit9531x_dev *sitdev,
				    u8 pll_idx,
				    const struct sit9531x_dco_code *code)
{
	u64 frac = code->dco_frac;
	int rc, i;

	/* Enable DCO with dither mode */
	rc = sit9531x_write_pll_u8(sitdev, pll_idx,
				   SIT9531X_PLL_REG_DCO_FUNC,
				   SIT9531X_DCO_DITHER_MODE | SIT9531X_DCO_EN);
	if (rc)
		return rc;

	/* Write 48-bit fractional word (LSB first: 0x51->0x56) */
	for (i = 0; i < 6; i++) {
		rc = sit9531x_write_pll_u8(sitdev, pll_idx,
					   SIT9531X_PLL_REG_DCO_FRAC1 + i,
					   frac & 0xFF);
		if (rc)
			return rc;
		frac >>= 8;
	}

	return 0;
}

/*
 * sit9531x_dco_write_outer - write outer loop DCO registers
 */
static int sit9531x_dco_write_outer(struct sit9531x_dev *sitdev,
				    u8 pll_idx,
				    const struct sit9531x_dco_code *code)
{
	s32 dco_int;
	u64 frac;
	int rc;

	/* Enable DCO with dither mode + outer loop */
	rc = sit9531x_write_pll_u8(sitdev, pll_idx,
				   SIT9531X_PLL_REG_DCO_FUNC,
				   SIT9531X_DCO_DITHER_MODE | SIT9531X_DCO_OUTER_EN |
				   SIT9531X_DCO_EN);
	if (rc)
		return rc;

	/* Write 32-bit fractional (shifted >>16, LSB first: 0x63->0x66) */
	frac = code->outer_frac >> 16;
	rc = sit9531x_write_pll_u8(sitdev, pll_idx,
				   SIT9531X_PLL_REG_DCO_OFRAC_7, frac & 0xFF);
	if (rc)
		return rc;
	rc = sit9531x_write_pll_u8(sitdev, pll_idx,
				   SIT9531X_PLL_REG_DCO_OFRAC_15,
				   (frac >> 8) & 0xFF);
	if (rc)
		return rc;
	rc = sit9531x_write_pll_u8(sitdev, pll_idx,
				   SIT9531X_PLL_REG_DCO_OFRAC_23,
				   (frac >> 16) & 0xFF);
	if (rc)
		return rc;
	rc = sit9531x_write_pll_u8(sitdev, pll_idx,
				   SIT9531X_PLL_REG_DCO_OFRAC_31,
				   (frac >> 24) & 0xFF);
	if (rc)
		return rc;

	/* Write 24-bit integer (LSB first: 0x5E->0x60) */
	dco_int = code->dco_int;
	rc = sit9531x_write_pll_u8(sitdev, pll_idx,
				   SIT9531X_PLL_REG_DCO_INT_7, dco_int & 0xFF);
	if (rc)
		return rc;
	rc = sit9531x_write_pll_u8(sitdev, pll_idx,
				   SIT9531X_PLL_REG_DCO_INT_15,
				   (dco_int >> 8) & 0xFF);
	if (rc)
		return rc;
	rc = sit9531x_write_pll_u8(sitdev, pll_idx,
				   SIT9531X_PLL_REG_DCO_INT_23,
				   (dco_int >> 16) & 0xFF);
	if (rc)
		return rc;

	return 0;
}

/*
 * sit9531x_dco_mask - mask a PLL's DCO (prevent updates)
 */
static int sit9531x_dco_mask(struct sit9531x_dev *sitdev, u8 pll_idx)
{
	u8 val;
	int rc;

	rc = sit9531x_read_pll_u8(sitdev, pll_idx,
				  SIT9531X_PLL_REG_DCO_FUNC, &val);
	if (rc)
		return rc;

	return sit9531x_write_pll_u8(sitdev, pll_idx,
				     SIT9531X_PLL_REG_DCO_FUNC,
				     val | SIT9531X_DCO_MASK);
}

/*
 * sit9531x_dco_unmask - unmask a PLL's DCO (allow updates)
 */
static int sit9531x_dco_unmask(struct sit9531x_dev *sitdev, u8 pll_idx)
{
	u8 val;
	int rc;

	rc = sit9531x_read_pll_u8(sitdev, pll_idx,
				  SIT9531X_PLL_REG_DCO_FUNC, &val);
	if (rc)
		return rc;

	return sit9531x_write_pll_u8(sitdev, pll_idx,
				     SIT9531X_PLL_REG_DCO_FUNC,
				     val & ~SIT9531X_DCO_MASK);
}

/*
 * sit9531x_dco_trigger - trigger DCO increment or decrement
 * @sitdev:	device pointer
 * @neg_adj:	true for decrement, false for increment
 *
 * Pulses the appropriate bit in Page 0 reg 0x64 (1->0 edge).
 */
static int sit9531x_dco_trigger(struct sit9531x_dev *sitdev, bool neg_adj)
{
	u8 val, bit;
	int rc;

	bit = neg_adj ? SIT9531X_DCO_TRIGGER_DECR : SIT9531X_DCO_TRIGGER_INCR;

	/* Set trigger bit */
	rc = sit9531x_read_u8(sitdev, SIT9531X_REG_DCO_TRIGGER, &val);
	if (rc)
		return rc;
	rc = sit9531x_write_u8(sitdev, SIT9531X_REG_DCO_TRIGGER, val | bit);
	if (rc)
		return rc;

	ndelay(SIT9531X_DCO_TRIGGER_PULSE_NS);

	/* Clear trigger bit */
	rc = sit9531x_read_u8(sitdev, SIT9531X_REG_DCO_TRIGGER, &val);
	if (rc)
		return rc;
	rc = sit9531x_write_u8(sitdev, SIT9531X_REG_DCO_TRIGGER, val & ~bit);
	if (rc)
		return rc;

	return 0;
}

/**
 * sit9531x_dco_adjust - adjust PLL frequency via DCO
 * @sitdev:	device pointer
 * @pll_idx:	PLL index (0-3)
 * @ppb:	frequency adjustment in parts per billion
 *
 * In free-run mode, adjusts the inner loop only.
 * In sync mode, adjusts both inner and outer loops.
 * Masks all other PLLs' DCOs during the operation.
 *
 * Caller must hold sitdev->multiop_lock.
 *
 * Return: 0 on success, <0 on error
 */
int sit9531x_dco_adjust(struct sit9531x_dev *sitdev, u8 pll_idx, s64 ppb)
{
	struct sit9531x_dco_code code = {};
	bool neg_adj = false;
	int rc, i;

	lockdep_assert_held(&sitdev->multiop_lock);

	if (ppb < 0) {
		neg_adj = true;
		ppb = -ppb;
	}

	/* Mask all other PLLs' DCOs */
	for (i = 0; i < SIT9531X_NUM_PLLS; i++) {
		if (i == pll_idx)
			continue;
		rc = sit9531x_dco_mask(sitdev, i);
		if (rc)
			return rc;
	}

	/* Unmask target PLL */
	rc = sit9531x_dco_unmask(sitdev, pll_idx);
	if (rc)
		return rc;

	/* Unlock debug registers */
	rc = sit9531x_write_pll_u8(sitdev, pll_idx,
				   SIT9531X_PLL_REG_DEBUG, SIT9531X_PLL_DEBUG_UNLOCK);
	if (rc)
		return rc;

	/* Determine mode and compute/write DCO codes */
	if (sitdev->chan[pll_idx].mode) {
		/* Free-run: inner loop only */
		u32 div_int;
		s32 div_fracn;
		u32 div_fracd;

		rc = sit9531x_dco_read_divn(sitdev, pll_idx,
					    &div_int, &div_fracn, &div_fracd);
		if (rc)
			return rc;

		sit9531x_dco_calc_inner(div_int, div_fracn, div_fracd,
					ppb, &code);

		rc = sit9531x_dco_write_inner(sitdev, pll_idx, &code);
		if (rc)
			return rc;
	} else {
		/* Sync: inner + outer loops */
		s64 n2_int;
		s32 n2_fracn;
		u32 n2_fracd;
		u32 div_int;
		s32 div_fracn;
		u32 div_fracd;

		/* Inner loop */
		rc = sit9531x_dco_read_divn(sitdev, pll_idx,
					    &div_int, &div_fracn, &div_fracd);
		if (rc)
			return rc;

		sit9531x_dco_calc_inner(div_int, div_fracn, div_fracd,
					ppb, &code);

		rc = sit9531x_dco_write_inner(sitdev, pll_idx, &code);
		if (rc)
			return rc;

		/* Outer loop */
		rc = sit9531x_dco_read_divn2(sitdev, pll_idx,
					     &n2_int, &n2_fracn, &n2_fracd);
		if (rc)
			return rc;

		sit9531x_dco_calc_outer(n2_int, n2_fracn, n2_fracd,
					ppb, &code);

		rc = sit9531x_dco_write_outer(sitdev, pll_idx, &code);
		if (rc)
			return rc;
	}

	/* Set trigger register base value, then pulse increment/decrement */
	rc = sit9531x_write_u8(sitdev, SIT9531X_REG_DCO_TRIGGER,
			       SIT9531X_DCO_TRIGGER_BASE);
	if (rc)
		return rc;

	return sit9531x_dco_trigger(sitdev, neg_adj);
}

/* ====================================================================
 * Output frequency set
 * ====================================================================
 */

/* Per-slot DIVO base register offsets (6 slots per page) */
static const u8 clkout_odr_divn_base[] = {
	0x14, 0x24, 0x34, 0x44, 0x54, 0x64
};

/* XO doubler register */
#define SIT9531X_REG_XO2_GENERIC		SIT9531X_REG(0x00, 0x2D)
#define SIT9531X_XO_DOUBLER_ENB_BIT		7   /* inverted: 0 = enabled */

/* VCO frequency bands (Hz) */
#define SIT9531X_FVCO_LOWBAND_MIN		4915200000ULL
#define SIT9531X_FVCO_LOWBAND_MAX		5898240000ULL
#define SIT9531X_FVCO_HIGHBAND_MIN		6875000000ULL
#define SIT9531X_FVCO_HIGHBAND_MAX		7812500000ULL

/*
 * sit9531x_is_xo_doubler_enabled - check if Fref doubler is active
 * @sitdev:	device pointer
 *
 * Register 0x2D bit 7 is active-low: 0 = doubler enabled, 1 = disabled.
 *
 * Return: 1 if enabled, 0 if disabled, <0 on error
 */
static int sit9531x_is_xo_doubler_enabled(struct sit9531x_dev *sitdev)
{
	u8 val;
	int rc;

	rc = sit9531x_read_u8(sitdev, SIT9531X_REG_XO2_GENERIC, &val);
	if (rc)
		return rc;

	return (~val >> SIT9531X_XO_DOUBLER_ENB_BIT) & 1u;
}

/*
 * sit9531x_get_fvco - read VCO frequency from chip's DIVN registers
 * @sitdev:	device pointer
 * @pll_idx:	PLL index (0-3)
 *
 * Fvco = Fref * DIVN, where DIVN = int_part + fracn/fracd is read from
 * PLL page regs 0x30 (int), 0x32-0x35 (numerator), 0x38-0x3B
 * (denominator), and Fref = xtal_freq << doubler.  DIVN is the
 * steady-state Fvco/Fref target programmed by the NVM blob and is
 * authoritative in both free-run and sync modes; the previous split
 * between free-run and sync formulas returned 0 on chips that didn't
 * have a sync input populated, which broke the TDC phase readback.
 *
 * The numerator and denominator are unsigned 32-bit values.  When the
 * denominator reads as zero (the chip's "implicit denominator" mode for
 * pure-fractional DIVN), the convention is fracd = 2^32, so fracn is
 * interpreted as a binary fraction over 2^32.
 *
 * Return: Fvco in Hz, or 0 on error
 */
static u64 sit9531x_get_fvco(struct sit9531x_dev *sitdev, u8 pll_idx)
{
	u64 fracd = 0, fref, divn_temp;
	u32 int_part, fracn = 0;
	int doubler, rc, i;
	u8 v;

	rc = sit9531x_read_pll_u8(sitdev, pll_idx,
				  SIT9531X_PLL_REG_DIVN_INT, &v);
	if (rc)
		return 0;
	int_part = v;

	for (i = 3; i >= 0; i--) {
		rc = sit9531x_read_pll_u8(sitdev, pll_idx,
					  SIT9531X_PLL_REG_DIVN_NUM + i, &v);
		if (rc)
			return 0;
		fracn = (fracn << 8) | v;
	}

	for (i = 3; i >= 0; i--) {
		rc = sit9531x_read_pll_u8(sitdev, pll_idx,
					  SIT9531X_PLL_REG_DIVN_DEN + i, &v);
		if (rc)
			return 0;
		fracd = (fracd << 8) | v;
	}

	/*
	 * Implicit denominator: fracd=0 means fracn is a binary fraction
	 * over 2^32 (the chip uses this when no explicit denominator is
	 * programmed).
	 */
	if (!fracd)
		fracd = 1ULL << 32;

	doubler = sit9531x_is_xo_doubler_enabled(sitdev);
	if (doubler < 0)
		return 0;

	fref = (u64)sitdev->xtal_freq << doubler;

	/*
	 * Fvco = fref * (int_part + fracn/fracd).  Split the multiplication
	 * to avoid 64-bit overflow on the realistic fref/DIVN range.
	 */
	divn_temp = fref * int_part + div64_u64(fref * fracn, fracd);

	return divn_temp;
}

/**
 * sit9531x_output_freq_set - set output clock frequency
 * @sitdev:	device pointer
 * @out_idx:	output index (0-N for this chip variant)
 * @pll_idx:	PLL driving this output (0-3)
 * @frequency:	desired output frequency in Hz
 *
 * Computes DIVO = Fvco / frequency and writes the 34-bit output divider
 * to the output system registers on Pages 3/4.  The write sequence is:
 *   1. Unlock debug registers (Page 3)
 *   2. Enter PRG_CMD state
 *   3. Write 5-byte DIVO to the correct page/slot
 *   4. NVM update
 *   5. Loop lock
 *   6. Wait for lock to settle
 *
 * Caller must hold sitdev->multiop_lock.
 *
 * Return: 0 on success, <0 on error.  Actual frequency may differ
 *         due to integer division; the output state is updated with
 *         the effective frequency (Fvco / DIVO).
 */
int sit9531x_output_freq_set(struct sit9531x_dev *sitdev, u8 out_idx,
			     u8 pll_idx, u64 frequency)
{
	const struct sit9531x_chip_info *info = sitdev->info;
	u8 slot, page, base_reg, divo_bytes[5], msb_old;
	u64 fvco, divo, fvco_min, fvco_max;
	int rc, j;

	lockdep_assert_held(&sitdev->multiop_lock);

	if (out_idx >= info->num_outputs || pll_idx >= SIT9531X_NUM_PLLS)
		return -EINVAL;

	if (!frequency)
		return -EINVAL;

	/* Determine VCO frequency band limits */
	if (pll_idx == 1 || pll_idx == 3) {
		/* PLLB, PLLD: high band */
		fvco_min = SIT9531X_FVCO_HIGHBAND_MIN;
		fvco_max = SIT9531X_FVCO_HIGHBAND_MAX;
	} else {
		/* PLLA, PLLC: low band */
		fvco_min = SIT9531X_FVCO_LOWBAND_MIN;
		fvco_max = SIT9531X_FVCO_LOWBAND_MAX;
	}

	/* Read current VCO frequency */
	fvco = sit9531x_get_fvco(sitdev, pll_idx);
	if (!fvco)
		fvco = fvco_min;
	else if (fvco < fvco_min)
		fvco = fvco_min;
	else if (fvco > fvco_max)
		fvco = fvco_max;

	/* Compute output divider: DIVO = Fvco / freq */
	divo = div64_u64(fvco, frequency);
	if (!divo)
		return -EINVAL;

	dev_dbg(sitdev->dev,
		"out%u: Fvco=%llu freq=%llu DIVO=%llu (effective %llu Hz)\n",
		out_idx, fvco, frequency, divo, div64_u64(fvco, divo));

	/* Map output index to physical slot */
	slot = info->clkout_map[out_idx];

	/* Determine page and per-page slot register */
	if (slot > SIT9531X_PAGE_OUTSYS0_SLOT_MAX)
		page = SIT9531X_PAGE_OUTSYS1;
	else
		page = SIT9531X_PAGE_OUTSYS0;
	base_reg = clkout_odr_divn_base[slot % 6];

	/* Step 1: Switch to Page 3 and unlock debug */
	rc = sit9531x_write_u8(sitdev, SIT9531X_REG_OUTSYS_DEBUG,
			       SIT9531X_DEBUG_UNLOCK_VAL);
	if (rc)
		return rc;

	/* Step 2: Enter PRG_CMD state */
	rc = sit9531x_write_u8(sitdev, SIT9531X_REG_PRG_DIR_GEN,
			       SIT9531X_PRG_CMD_STATE);
	if (rc)
		return rc;

	/* Step 3: Prepare DIVO bytes (34-bit, LSB at base reg) */
	divo_bytes[0] = (divo >>  0) & 0xFF;
	divo_bytes[1] = (divo >>  8) & 0xFF;
	divo_bytes[2] = (divo >> 16) & 0xFF;
	divo_bytes[3] = (divo >> 24) & 0xFF;
	divo_bytes[4] = (divo >> 32) & 0x03;  /* only bits [1:0] */

	/* Read existing MSB register and preserve upper 6 bits */
	rc = sit9531x_read_u8(sitdev,
			      SIT9531X_REG(page, base_reg - 4), &msb_old);
	if (rc)
		return rc;
	divo_bytes[4] |= msb_old & 0xFC;

	/* Write 5 DIVO bytes (base, base-1, base-2, base-3, base-4) */
	for (j = 0; j < 5; j++) {
		rc = sit9531x_write_u8(sitdev,
				       SIT9531X_REG(page, base_reg - j),
				       divo_bytes[j]);
		if (rc)
			return rc;
	}

	/* Step 4: Switch back to Page 3 for control registers */
	/* (SIT9531X_REG_PRG_DIR_GEN is always on Page 3) */

	/* Step 5: NVM update */
	rc = sit9531x_write_u8(sitdev, SIT9531X_REG_PRG_DIR_GEN, SIT9531X_UPDATE_NVM);
	if (rc)
		return rc;

	/* Step 6: Loop lock */
	rc = sit9531x_write_u8(sitdev, SIT9531X_REG_PRG_DIR_GEN, SIT9531X_LOOP_LOCK);
	if (rc)
		return rc;

	/*
	 * Hardware requires settling time after loop-lock command.
	 * This sleep is intentional despite holding multiop_lock;
	 * the NVM + lock sequence must be atomic.
	 */
	msleep(100);

	/* Update cached output frequency */
	sitdev->out[out_idx].freq = (u32)div64_u64(fvco, divo);

	return 0;
}

/* ====================================================================
 * Phase adjust (PRG_RST_DELAY register-based)
 * ====================================================================
 *
 * The chip exposes a per-output 34-bit coarse delay measured in VCO
 * clock periods plus a 3-bit fine delay in fixed 30 ps steps.  The
 * five bytes PROG6..PROG2 hold the field across registers:
 *   base + 0  PROG6  [7:5] OPSTG_VCASC_BUMP (preserved via RMW)
 *                    [4:2] PRG_RST_FINE_DELAY
 *                    [1:0] PRG_RST_DELAY[33:32]
 *   base + 1  PROG5  PRG_RST_DELAY[31:24]
 *   base + 2  PROG4  PRG_RST_DELAY[23:16]
 *   base + 3  PROG3  PRG_RST_DELAY[15:8]
 *   base + 4  PROG2  PRG_RST_DELAY[7:0]
 *
 * Outputs 0-5 live on Page 3, outputs 6-11 on Page 4, with each
 * output's block at base = 0x15 + 16 * (out_idx % 6).
 *
 * The chip only supports unsigned positive delay.  A negative phase
 * adjustment (advance) is wrapped to (T_out - |phase|) modulo one
 * output period, which is identical for a periodic signal.
 */

int sit9531x_output_phase_adjust_set(struct sit9531x_dev *sitdev,
				     u8 out_idx, s32 phase_ps)
{
	const struct sit9531x_chip_info *info = sitdev->info;
	u64 abs_ps, fvco, coarse, coarse_ps, rem_ps;
	u8 page, base, prog6_val, fine = 0;
	u8 pll_idx, slot;
	u32 freq;
	int rc;

	if (out_idx >= info->num_outputs)
		return -EINVAL;

	pll_idx = sitdev->out[out_idx].pll_idx;
	if (pll_idx >= SIT9531X_NUM_PLLS)
		return -EINVAL;

	freq = sitdev->out[out_idx].freq;
	if (!freq)
		return -EINVAL;

	fvco = sit9531x_get_fvco(sitdev, pll_idx);
	if (!fvco)
		return -EIO;

	/*
	 * Convert to unsigned absolute delay.  Negative phase (advance)
	 * is rendered as T_out - |phase|, modulo the output period.
	 */
	if (phase_ps == 0) {
		abs_ps = 0;
	} else if (phase_ps > 0) {
		abs_ps = (u64)phase_ps;
	} else {
		u64 t_out_ps = div64_u64(1000000000000ULL, freq);
		u64 advance = (u64)(-(s64)phase_ps);

		if (t_out_ps == 0)
			return -EINVAL;
		advance %= t_out_ps;
		abs_ps = (advance == 0) ? 0 : (t_out_ps - advance);
	}

	/*
	 * coarse_cycles = abs_ps * Fvco / 1e12 ps/s.
	 * mul_u64_u64_div_u64() avoids overflow when abs_ps approaches
	 * one second of 1 PPS wrap-around.
	 */
	coarse = mul_u64_u64_div_u64(abs_ps, fvco, 1000000000000ULL);
	if (coarse >= (1ULL << SIT9531X_OUT_PRG_COARSE_BITS))
		return -ERANGE;

	/* Fine delay = round((abs_ps - coarse * vco_period_ps) / 30 ps) */
	coarse_ps = mul_u64_u64_div_u64(coarse, 1000000000000ULL, fvco);
	rem_ps = (abs_ps > coarse_ps) ? (abs_ps - coarse_ps) : 0;
	if (rem_ps) {
		u64 steps;

		steps = div64_u64(rem_ps + SIT9531X_OUT_PRG_FINE_STEP_PS / 2,
				  SIT9531X_OUT_PRG_FINE_STEP_PS);
		if (steps > SIT9531X_OUT_PRG_FINE_MAX)
			steps = SIT9531X_OUT_PRG_FINE_MAX;
		fine = (u8)steps;
	}

	/*
	 * Map logical output index to the chip's physical output slot.
	 * On SiT95317 the eight logical outputs land on chip slots
	 * {0, 3, 4, 5, 7, 8, 9, 11}; on SiT95316 the map is identity.
	 * Page/base must address the slot, not the logical index.
	 */
	slot = info->clkout_map[out_idx];
	page = (slot > SIT9531X_PAGE_OUTSYS0_SLOT_MAX) ?
	       SIT9531X_PAGE_OUTSYS1 : SIT9531X_PAGE_OUTSYS0;
	base = SIT9531X_OUT_PRG_DELAY_BASE +
	       SIT9531X_OUT_PRG_SLOT_STRIDE * (slot % 6);

	/* Caller (dpll.c) holds multiop_lock around the whole sequence. */
	lockdep_assert_held(&sitdev->multiop_lock);

	/* PROG6 RMW: preserve OPSTG_VCASC_BUMP in [7:5] */
	rc = sit9531x_read_u8(sitdev, SIT9531X_REG(page, base),
			      &prog6_val);
	if (rc)
		return rc;

	prog6_val &= SIT9531X_OUT_PRG_OPSTG_MASK;
	prog6_val |= (fine << SIT9531X_OUT_PRG_FINE_SHIFT) &
		     SIT9531X_OUT_PRG_FINE_MASK;
	prog6_val |= (u8)((coarse >> 32) & SIT9531X_OUT_PRG_COARSE_HI_MASK);

	rc = sit9531x_write_u8(sitdev, SIT9531X_REG(page, base),
			       prog6_val);
	if (rc)
		return rc;
	rc = sit9531x_write_u8(sitdev, SIT9531X_REG(page, base + 1),
			       (u8)((coarse >> 24) & 0xFF));
	if (rc)
		return rc;
	rc = sit9531x_write_u8(sitdev, SIT9531X_REG(page, base + 2),
			       (u8)((coarse >> 16) & 0xFF));
	if (rc)
		return rc;
	rc = sit9531x_write_u8(sitdev, SIT9531X_REG(page, base + 3),
			       (u8)((coarse >> 8) & 0xFF));
	if (rc)
		return rc;
	rc = sit9531x_write_u8(sitdev, SIT9531X_REG(page, base + 4),
			       (u8)(coarse & 0xFF));
	if (rc)
		return rc;

	return 0;
}

/* ====================================================================
 * PLL lock-detection thresholds (LL_REG2_PLL, reg 0x2A)
 * ====================================================================
 *
 * Per SiT95316 register map p.80 (PAGE_PLL):
 *   bits [7:4] LL_SET_VALUE_PLL[3:0]  -- threshold for declaring the
 *                                        outer loop as "out of lock"
 *   bits [3:0] LL_CLR_VALUE_PLL[3:0]  -- threshold for re-acquiring
 *                                        lock
 *
 * Each 4-bit field selects from a 16-step ladder spanning 0.05 PPB to
 * 4000 PPM.  Loop-filter coefficients on regs 0x10-0x15 are
 * GUI/NVM-managed by the timing configurator and must not be reprogrammed at runtime.
 */

/**
 * sit9531x_pll_lock_threshold_set - program lock-loss / lock-acquire thresholds
 * @sitdev:	device pointer
 * @pll_idx:	PLL index (0=PLLA ... 3=PLLD)
 * @set_val:	LL_SET_VALUE 4-bit code (0..15) -- outer-loop unlock threshold
 * @clr_val:	LL_CLR_VALUE 4-bit code (0..15) -- outer-loop relock threshold
 *
 * Writes PLL_PAGE reg 0x2A (LL_REG2_PLL).
 *
 * Caller must hold sitdev->multiop_lock.
 *
 * Return: 0 on success, <0 on error.
 */
int sit9531x_pll_lock_threshold_set(struct sit9531x_dev *sitdev, u8 pll_idx,
				    u8 set_val, u8 clr_val)
{
	u8 reg_val;

	lockdep_assert_held(&sitdev->multiop_lock);

	if (pll_idx >= SIT9531X_NUM_PLLS || set_val > 0x0F || clr_val > 0x0F)
		return -EINVAL;

	reg_val = ((set_val & 0x0F) << 4) | (clr_val & 0x0F);

	return sit9531x_write_pll_u8(sitdev, pll_idx,
				     SIT9531X_PLL_REG_LL_THRESH, reg_val);
}

/* ====================================================================
 * Notification clear
 * ====================================================================
 */

/**
 * sit9531x_clear_notifications - clear all notification registers
 * @sitdev:	device pointer
 *
 * Clears all write-1-to-clear notification registers:
 *   - PLL outer LOL notification (Page 0, reg 0x07)
 *   - PLL holdover freeze notification (Page 0, reg 0x0B)
 *   - PLL inner LOL notification (Page 0, reg 0x93)
 *   - Clock monitor XO/PLL notification (Page 0, reg 0x9E)
 *   - Clock input notifications (Page 6, regs 0x03/0x07/0x93/0x97)
 *
 * Caller must hold sitdev->multiop_lock.
 *
 * Return: 0 on success, <0 on error
 */
int sit9531x_clear_notifications(struct sit9531x_dev *sitdev)
{
	int rc;

	lockdep_assert_held(&sitdev->multiop_lock);

	/* Page 0x00 W1C notification registers */
	rc = sit9531x_write_u8(sitdev, SIT9531X_REG_OUTER_LOL_NOTIF, 0xFF);
	if (rc)
		return rc;
	rc = sit9531x_write_u8(sitdev, SIT9531X_REG_HO_FREEZE_NOTIF, 0xFF);
	if (rc)
		return rc;
	rc = sit9531x_write_u8(sitdev, SIT9531X_REG_PLL_INNER_LOL_NOTIF, 0xFF);
	if (rc)
		return rc;
	rc = sit9531x_write_u8(sitdev, SIT9531X_REG_CMON_NOTIF, 0xFF);
	if (rc)
		return rc;

	/* Page 0x06 clock input monitor notifications */
	rc = sit9531x_write_u8(sitdev, SIT9531X_CLKMON_P_NOTIF_01, 0xFF);
	if (rc)
		return rc;
	rc = sit9531x_write_u8(sitdev, SIT9531X_CLKMON_P_NOTIF_23, 0xFF);
	if (rc)
		return rc;
	rc = sit9531x_write_u8(sitdev, SIT9531X_CLKMON_N_NOTIF_01, 0xFF);
	if (rc)
		return rc;
	rc = sit9531x_write_u8(sitdev, SIT9531X_CLKMON_N_NOTIF_23, 0xFF);
	if (rc)
		return rc;

	dev_dbg(sitdev->dev, "All notification registers cleared\n");
	return 0;
}

/* ====================================================================
 * INTSYNC (inter-PLL synchronization)
 * ====================================================================
 */

/*
 * INTSYNC configuration register values.
 * These are written to the source PLL's EXT page to enable/disable
 * inter-PLL synchronization (lock frequency PLL to phase PLL).
 */
struct sit9531x_intsync_reg {
	u8 offset;
	u8 en_val;
	u8 dis_val;
};

static const struct sit9531x_intsync_reg intsync_config[] = {
	{ 0x2D, 0x02, 0x00 },
	{ 0x50, 0x08, 0x00 },
	{ 0x51, 0x04, 0x00 },
	{ 0x54, 0x02, 0x00 },
	{ 0x55, 0x28, 0x20 },
	{ 0x5C, 0x0F, 0x00 },
	{ 0x5D, 0xFF, 0x00 },
	{ 0x6C, 0xDD, 0x00 },
};

/**
 * sit9531x_intsync_enable - enable inter-PLL synchronization
 * @sitdev:	device pointer
 * @src_pll_idx: source (frequency) PLL index (0-3)
 *
 * Enables INTSYNC global bit, unlocks the source PLL's EXT page
 * debug registers, writes configuration, and triggers a small
 * update on the source PLL.
 *
 * Caller must hold sitdev->multiop_lock.
 *
 * Return: 0 on success, <0 on error
 */
int sit9531x_intsync_enable(struct sit9531x_dev *sitdev, u8 src_pll_idx)
{
	u8 ext_page, val;
	int rc, i;

	lockdep_assert_held(&sitdev->multiop_lock);

	if (src_pll_idx >= SIT9531X_NUM_PLLS)
		return -EINVAL;

	ext_page = SIT9531X_PLL_EXT_PAGE(src_pll_idx);

	/* Set INTSYNC global enable bit (Page 0, reg 0x40 bit 6) */
	rc = sit9531x_read_u8(sitdev, SIT9531X_REG_INTSYNC_GLOBAL, &val);
	if (rc)
		return rc;
	rc = sit9531x_write_u8(sitdev, SIT9531X_REG_INTSYNC_GLOBAL,
			       val | BIT(SIT9531X_INTSYNC_EN_BIT));
	if (rc)
		return rc;

	/* Small update on Page 0 */
	rc = sit9531x_write_u8(sitdev, SIT9531X_REG_GLOBAL_UPDATE,
			       SIT9531X_PLL_SMALL_UPDATE_CMD);
	if (rc)
		return rc;

	/* Unlock debug on EXT page */
	rc = sit9531x_write_u8(sitdev, SIT9531X_REG(ext_page, SIT9531X_PLL_REG_DEBUG),
			       SIT9531X_PLL_DEBUG_UNLOCK);
	if (rc)
		return rc;

	/* Write INTSYNC configuration */
	for (i = 0; i < ARRAY_SIZE(intsync_config); i++) {
		rc = sit9531x_write_u8(sitdev,
				       SIT9531X_REG(ext_page,
						    intsync_config[i].offset),
				       intsync_config[i].en_val);
		if (rc)
			return rc;
	}

	/* Small update on source PLL */
	rc = sit9531x_write_pll_u8(sitdev, src_pll_idx,
				   SIT9531X_PLL_REG_SMALL_UPDATE,
				   SIT9531X_PLL_SMALL_UPDATE_CMD);
	if (rc)
		return rc;

	dev_info(sitdev->dev, "INTSYNC enabled on PLL%c\n",
		 'A' + src_pll_idx);
	return 0;
}

/**
 * sit9531x_intsync_disable - disable inter-PLL synchronization
 * @sitdev:	device pointer
 * @src_pll_idx: source (frequency) PLL index (0-3)
 *
 * Clears INTSYNC global bit, writes disable values to the source
 * PLL's EXT page, and triggers a small update.
 *
 * Caller must hold sitdev->multiop_lock.
 *
 * Return: 0 on success, <0 on error
 */
int sit9531x_intsync_disable(struct sit9531x_dev *sitdev, u8 src_pll_idx)
{
	u8 ext_page, val;
	int rc, i;

	lockdep_assert_held(&sitdev->multiop_lock);

	if (src_pll_idx >= SIT9531X_NUM_PLLS)
		return -EINVAL;

	ext_page = SIT9531X_PLL_EXT_PAGE(src_pll_idx);

	/* Clear INTSYNC global enable bit */
	rc = sit9531x_read_u8(sitdev, SIT9531X_REG_INTSYNC_GLOBAL, &val);
	if (rc)
		return rc;
	rc = sit9531x_write_u8(sitdev, SIT9531X_REG_INTSYNC_GLOBAL,
			       val & ~BIT(SIT9531X_INTSYNC_EN_BIT));
	if (rc)
		return rc;

	/* Small update on Page 0 */
	rc = sit9531x_write_u8(sitdev, SIT9531X_REG_GLOBAL_UPDATE,
			       SIT9531X_PLL_SMALL_UPDATE_CMD);
	if (rc)
		return rc;

	/* Unlock debug on EXT page */
	rc = sit9531x_write_u8(sitdev, SIT9531X_REG(ext_page, SIT9531X_PLL_REG_DEBUG),
			       SIT9531X_PLL_DEBUG_UNLOCK);
	if (rc)
		return rc;

	/* Write INTSYNC configuration */
	for (i = 0; i < ARRAY_SIZE(intsync_config); i++) {
		rc = sit9531x_write_u8(sitdev,
				       SIT9531X_REG(ext_page,
						    intsync_config[i].offset),
				       intsync_config[i].dis_val);
		if (rc)
			return rc;
	}

	/* Small update on source PLL */
	rc = sit9531x_write_pll_u8(sitdev, src_pll_idx,
				   SIT9531X_PLL_REG_SMALL_UPDATE,
				   SIT9531X_PLL_SMALL_UPDATE_CMD);
	if (rc)
		return rc;

	dev_info(sitdev->dev, "INTSYNC disabled on PLL%c\n",
		 'A' + src_pll_idx);
	return 0;
}

/**
 * sit9531x_pll_sysref_mode_set - configure SYSREF / SYNCB / Pulser output mode
 * @sitdev:		device pointer
 * @pll_idx:		PLL index (0=PLLA ... 3=PLLD)
 * @mode:		one of sit9531x_sysref_mode (DISABLE, SYSREF, SYNCB, PULSER)
 * @target_outputs:	12-bit one-hot mask selecting which physical chip
 *			slots (0..11) are driven by this PLL's SYSREF/SYNCB
 *
 * Programs PLL_CONFIG47_PLL bits 6/5/4 (DIVO_PULSER_MODE / DIVO_SYSREF_MODE
 * / DIVO_SYNCB_MODE) and bits 3:0 (DIVO_SYS_REF[11:8]) plus
 * Sysref_sel_PLL (DIVO_SYS_REF[7:0]).  The per-PLL small-change trigger
 * enable on Page 0 reg 0x19 is set when mode != DISABLE and cleared when
 * mode == DISABLE.
 *
 * The mask is one-hot per physical slot.  Callers needing to convert
 * from logical output index must do so via info->clkout_map[].
 *
 * Caller must hold sitdev->multiop_lock.
 *
 * Return: 0 on success, <0 on error.
 */
int sit9531x_pll_sysref_mode_set(struct sit9531x_dev *sitdev, u8 pll_idx,
				 enum sit9531x_sysref_mode mode,
				 u16 target_outputs)
{
	u8 mode_bits = 0;
	int rc;

	if (pll_idx >= SIT9531X_NUM_PLLS)
		return -EINVAL;
	if (target_outputs & ~0x0FFFU)
		return -EINVAL;

	switch (mode) {
	case SIT9531X_SYSREF_DISABLE:
		break;
	case SIT9531X_SYSREF_MODE_SYSREF:
		mode_bits = SIT9531X_PLL_SYSREF_MODE_BIT;
		break;
	case SIT9531X_SYSREF_MODE_SYNCB:
		mode_bits = SIT9531X_PLL_SYSREF_SYNCB_BIT;
		break;
	case SIT9531X_SYSREF_MODE_PULSER:
		mode_bits = SIT9531X_PLL_SYSREF_PULSER_BIT |
			    SIT9531X_PLL_SYSREF_MODE_BIT;
		break;
	default:
		return -EINVAL;
	}

	/* PLL reg 0x47: mode bits [6:4] + DIVO_SYS_REF[11:8] in [3:0] */
	rc = sit9531x_update_pll_u8(sitdev, pll_idx,
				    SIT9531X_PLL_REG_SYSREF_MODE,
				    SIT9531X_PLL_SYSREF_MODE_MASK |
				    SIT9531X_PLL_SYSREF_TARGET_HI_MASK,
				    mode_bits |
				    ((target_outputs >> 8) &
				     SIT9531X_PLL_SYSREF_TARGET_HI_MASK));
	if (rc)
		return rc;

	/* PLL reg 0x48: DIVO_SYS_REF[7:0] */
	rc = sit9531x_write_pll_u8(sitdev, pll_idx,
				   SIT9531X_PLL_REG_SYSREF_SEL,
				   target_outputs & 0xFF);
	if (rc)
		return rc;

	/* Page 0 reg 0x19: enable small-change (SYSREF) trigger for this PLL */
	{
		u8 trig_bit = BIT(SIT9531X_DIVO_SYSREF_TRIG_BIT(pll_idx));
		u8 val;

		rc = sit9531x_read_u8(sitdev, SIT9531X_REG_DIVO_TRIGGER_EN,
				      &val);
		if (rc)
			return rc;
		if (mode == SIT9531X_SYSREF_DISABLE)
			val &= ~trig_bit;
		else
			val |= trig_bit;
		rc = sit9531x_write_u8(sitdev, SIT9531X_REG_DIVO_TRIGGER_EN,
				       val);
		if (rc)
			return rc;
	}

	dev_info(sitdev->dev,
		 "PLL%c sysref mode=%d target_outputs=0x%03x\n",
		 'A' + pll_idx, mode, target_outputs);
	return 0;
}

/**
 * sit9531x_output_pulse_ctrl_set - program per-output PULSE_CTRL byte
 * @sitdev:	device pointer
 * @out_idx:	logical output index (translated to chip slot internally)
 * @pulse_ctrl:	8-bit PULSE_CTRL value (PROG0)
 *
 * Writes ODRn_PROG0 on the output page (Page 3 for slots 0..5,
 * Page 4 for slots 6..11) at offset 0x1B + 16 * (slot % 6).
 *
 * Caller must hold sitdev->multiop_lock.
 *
 * Return: 0 on success, <0 on error.
 */
int sit9531x_output_pulse_ctrl_set(struct sit9531x_dev *sitdev,
				   u8 out_idx, u8 pulse_ctrl)
{
	const struct sit9531x_chip_info *info = sitdev->info;
	u8 slot, page, reg;

	if (out_idx >= info->num_outputs)
		return -EINVAL;

	slot = info->clkout_map[out_idx];
	page = (slot > SIT9531X_PAGE_OUTSYS0_SLOT_MAX) ?
		SIT9531X_PAGE_OUTSYS1 : SIT9531X_PAGE_OUTSYS0;
	reg = SIT9531X_OUT_PROG0_BASE +
	      SIT9531X_OUT_PRG_SLOT_STRIDE * (slot % 6);

	return sit9531x_write_u8(sitdev, SIT9531X_REG(page, reg),
				 pulse_ctrl);
}

/* ====================================================================
 * Phase offset readback (TDC)
 * ====================================================================
 */

/**
 * sit9531x_phase_offset_read - read phase difference via TDC
 * @sitdev:	device pointer
 * @pll_idx:	PLL index (0-3)
 * @phase_ps:	output phase difference in picoseconds
 *
 * Reads the Time-to-Digital Converter (TDC) 40-bit code from the
 * PLL page registers, then converts to picoseconds using the VCO
 * frequency: phase_diff = tdc_code / fvco.
 *
 * Caller must hold sitdev->multiop_lock.
 *
 * Return: 0 on success, <0 on error
 */
int sit9531x_phase_offset_read(struct sit9531x_dev *sitdev, u8 pll_idx,
			       s64 *phase_ps)
{
	u64 fvco, fvco_mhz;
	s64 tdc_signed;
	u64 tdc_raw;
	int rc, i;
	bool sign;
	u8 v;

	lockdep_assert_held(&sitdev->multiop_lock);

	if (pll_idx >= SIT9531X_NUM_PLLS)
		return -EINVAL;

	/* Configure TDC */
	rc = sit9531x_write_pll_u8(sitdev, pll_idx,
				   SIT9531X_PLL_REG_TDC_MODE,
				   SIT9531X_TDC_MODE_ENABLE);
	if (rc)
		return rc;
	rc = sit9531x_write_pll_u8(sitdev, pll_idx,
				   SIT9531X_PLL_REG_TDC_CFG,
				   SIT9531X_TDC_CFG_DEFAULT);
	if (rc)
		return rc;

	/* Trigger TDC sampling by reading the trigger register 3 times */
	for (i = 0; i < 3; i++) {
		rc = sit9531x_read_pll_u8(sitdev, pll_idx,
					  SIT9531X_PLL_REG_TDC_TRIGGER, &v);
		if (rc)
			return rc;
	}

	/* Read 40-bit TDC code (5 bytes: 0xB5-0xB9) */
	tdc_raw = 0;

	rc = sit9531x_read_pll_u8(sitdev, pll_idx,
				  SIT9531X_PLL_REG_TDC_DATA_4, &v);
	if (rc)
		return rc;
	sign = !!(v & BIT(SIT9531X_TDC_SIGN_BIT));
	tdc_raw = (u64)(v & 0x07) << 32;

	rc = sit9531x_read_pll_u8(sitdev, pll_idx,
				  SIT9531X_PLL_REG_TDC_DATA_3, &v);
	if (rc)
		return rc;
	tdc_raw |= (u64)v << 24;

	rc = sit9531x_read_pll_u8(sitdev, pll_idx,
				  SIT9531X_PLL_REG_TDC_DATA_2, &v);
	if (rc)
		return rc;
	tdc_raw |= (u64)v << 16;

	rc = sit9531x_read_pll_u8(sitdev, pll_idx,
				  SIT9531X_PLL_REG_TDC_DATA_1, &v);
	if (rc)
		return rc;
	tdc_raw |= (u64)v << 8;

	rc = sit9531x_read_pll_u8(sitdev, pll_idx,
				  SIT9531X_PLL_REG_TDC_DATA_0, &v);
	if (rc)
		return rc;
	tdc_raw |= v;

	/* Apply sign */
	tdc_signed = sign ? -(s64)tdc_raw : (s64)tdc_raw;

	/*
	 * Get VCO frequency for conversion.  Fvco==0 means DIVN is not
	 * programmed (PLL unused on this board) -- skip silently rather
	 * than spamming the log on every poll cycle.
	 */
	fvco = sit9531x_get_fvco(sitdev, pll_idx);
	if (!fvco) {
		dev_dbg(sitdev->dev, "PLL%c: Fvco unknown, skip TDC\n",
			'A' + pll_idx);
		return -ENODEV;
	}

	/*
	 * phase_diff (seconds) = tdc_code / fvco
	 * phase_diff (ps) = tdc_code * 1e12 / fvco
	 *
	 * To avoid 64-bit overflow:
	 *   phase_ps = tdc_code * 1e6 / (fvco / 1e6)
	 */
	fvco_mhz = div64_u64(fvco, 1000000ULL);
	if (!fvco_mhz)
		return -EIO;

	*phase_ps = div64_s64(tdc_signed * 1000000LL, (s64)fvco_mhz);

	return 0;
}

/* ====================================================================
 * Hardware state fetch
 * ====================================================================
 */

/*
 * sit9531x_ref_state_fetch - read input reference status from hardware
 * @sitdev:	device pointer
 * @index:	input reference index
 *
 * Reads LOS and OOF status bits for the given input reference.
 *
 * Return: 0 on success, <0 on error
 */
static int sit9531x_ref_state_fetch(struct sit9531x_dev *sitdev, u8 index)
{
	struct sit9531x_ref *ref = &sitdev->ref[index];
	u8 los_status, oof_status;
	int rc;

	rc = sit9531x_read_u8(sitdev, SIT9531X_REG_LOS_STATUS, &los_status);
	if (rc)
		return rc;

	rc = sit9531x_read_u8(sitdev, SIT9531X_REG_OOF_STATUS, &oof_status);
	if (rc)
		return rc;

	ref->los = !!(los_status & BIT(index));
	ref->oof = !!(oof_status & BIT(index));

	return 0;
}

/*
 * sit9531x_chan_state_fetch - read PLL channel status from hardware
 * @sitdev:	device pointer
 * @pll_idx:	PLL index (0-3)
 *
 * Reads lock status and mode from the PLL status register.
 *
 * Return: 0 on success, <0 on error
 */
static int sit9531x_chan_state_fetch(struct sit9531x_dev *sitdev, u8 pll_idx)
{
	u8 status, holdover, input_sel, inner_lol, ho_freeze;
	struct sit9531x_chan *chan = &sitdev->chan[pll_idx];
	int rc;

	/* Read PLL lock/mode from PLL page */
	rc = sit9531x_read_pll_u8(sitdev, pll_idx,
				  SIT9531X_PLL_REG_STATUS, &status);
	if (rc)
		return rc;

	/* Read holdover status from Page 0 */
	rc = sit9531x_read_u8(sitdev, SIT9531X_REG_HOLDOVER_STATUS, &holdover);
	if (rc)
		return rc;

	/* Read selected input reference */
	rc = sit9531x_read_pll_u8(sitdev, pll_idx,
				  SIT9531X_PLL_REG_INPUT_SEL, &input_sel);
	if (rc)
		return rc;

	/* Read PLL inner loop loss-of-lock */
	rc = sit9531x_read_u8(sitdev, SIT9531X_REG_PLL_INNER_LOL_STATUS,
			      &inner_lol);
	if (rc)
		return rc;

	/* Read holdover freeze status */
	rc = sit9531x_read_u8(sitdev, SIT9531X_REG_HO_FREEZE_STATUS, &ho_freeze);
	if (rc)
		return rc;

	/*
	 * Holdover bit set means the PLL is in holdover -- i.e. NOT locked
	 * to its input.  Invert the polarity so chan->locked reflects the
	 * intuitive sense (true == locked, false == holdover / free-run).
	 */
	chan->locked = !(holdover & BIT(pll_idx));
	chan->mode = !!(status & SIT9531X_PLL_STATUS_OUTER_DIS);
	chan->selected_ref = input_sel;
	chan->inner_lol = !!(inner_lol & BIT(pll_idx));
	chan->ho_freeze = !!(ho_freeze & BIT(pll_idx));

	return 0;
}

/*
 * sit9531x_out_state_fetch - read output status from hardware
 * @sitdev:	device pointer
 * @index:	output index
 *
 * Reads the output PLL association from the PLL page output map
 * registers.
 *
 * Return: 0 on success, <0 on error
 */
static int sit9531x_out_state_fetch(struct sit9531x_dev *sitdev, u8 index)
{
	struct sit9531x_out *out = &sitdev->out[index];
	u8 map_lo, map_hi;
	int pll_idx;

	/*
	 * Determine which PLL drives this output by checking each PLL's
	 * output map registers (0x27 = outputs 8-11, 0x28 = outputs 0-7).
	 */
	for (pll_idx = 0; pll_idx < SIT9531X_NUM_PLLS; pll_idx++) {
		int rc;

		rc = sit9531x_read_pll_u8(sitdev, pll_idx,
					  SIT9531X_PLL_REG_OUT_MAP_LO, &map_lo);
		if (rc)
			return rc;

		rc = sit9531x_read_pll_u8(sitdev, pll_idx,
					  SIT9531X_PLL_REG_OUT_MAP_HI, &map_hi);
		if (rc)
			return rc;

		if (index < 8) {
			if (map_lo & BIT(index)) {
				out->pll_idx = pll_idx;
				out->enabled = true;
				return 0;
			}
		} else {
			if (map_hi & BIT(index - 8)) {
				out->pll_idx = pll_idx;
				out->enabled = true;
				return 0;
			}
		}
	}

	/* Output not mapped to any PLL */
	out->pll_idx = 0;
	out->enabled = false;

	return 0;
}

/*
 * sit9531x_dev_state_fetch - read all hardware state at startup
 * @sitdev:	device pointer
 *
 * Called once during probe to populate the initial state cache.
 *
 * Return: 0 on success, <0 on error
 */
static int sit9531x_dev_state_fetch(struct sit9531x_dev *sitdev)
{
	int rc;
	u8 i;

	for (i = 0; i < sitdev->info->num_inputs; i++) {
		rc = sit9531x_ref_state_fetch(sitdev, i);
		if (rc) {
			dev_err(sitdev->dev,
				"Failed to fetch input %u state: %d\n", i, rc);
			return rc;
		}
	}

	for (i = 0; i < sitdev->info->num_outputs; i++) {
		rc = sit9531x_out_state_fetch(sitdev, i);
		if (rc) {
			dev_err(sitdev->dev,
				"Failed to fetch output %u state: %d\n", i, rc);
			return rc;
		}
	}

	for (i = 0; i < SIT9531X_NUM_PLLS; i++) {
		rc = sit9531x_chan_state_fetch(sitdev, i);
		if (rc) {
			dev_err(sitdev->dev,
				"Failed to fetch PLL%c state: %d\n",
				'A' + i, rc);
			return rc;
		}
	}

	return 0;
}

/* ====================================================================
 * Periodic work thread
 * ====================================================================
 */

/*
 * sit9531x_dev_ref_states_update - update all input reference states
 * @sitdev:	device pointer
 */
static void sit9531x_dev_ref_states_update(struct sit9531x_dev *sitdev)
{
	int i, rc;

	for (i = 0; i < sitdev->info->num_inputs; i++) {
		rc = sit9531x_ref_state_fetch(sitdev, i);
		if (rc)
			dev_warn(sitdev->dev,
				 "Failed to get REF%u status: %d\n", i, rc);
	}
}

/*
 * sit9531x_dev_chan_states_update - update all PLL channel states
 * @sitdev:	device pointer
 */
static void sit9531x_dev_chan_states_update(struct sit9531x_dev *sitdev)
{
	int i, rc;

	for (i = 0; i < SIT9531X_NUM_PLLS; i++) {
		rc = sit9531x_chan_state_fetch(sitdev, i);
		if (rc)
			dev_warn(sitdev->dev,
				 "Failed to get PLL%c state: %d\n",
				 'A' + i, rc);
	}
}

/*
 * sit9531x_dev_periodic_work - periodic hardware state polling
 * @work:	kthread_work pointer
 *
 * Polls hardware state at SIT9531X_STATUS_POLL_MS intervals.
 * Updates reference and channel states, then delegates change
 * detection to sit9531x_dpll_changes_check() for each registered DPLL.
 */
static void sit9531x_dev_periodic_work(struct kthread_work *work)
{
	struct sit9531x_dev *sitdev = container_of(work, struct sit9531x_dev,
						   work.work);
	struct sit9531x_dpll *sitdpll;
	int rc;

	/* Update input references' states */
	sit9531x_dev_ref_states_update(sitdev);

	/* Update PLL channels' states */
	sit9531x_dev_chan_states_update(sitdev);

	/* Check for state changes on each registered DPLL */
	list_for_each_entry(sitdpll, &sitdev->dplls, list)
		sit9531x_dpll_changes_check(sitdpll);

	/*
	 * Acknowledge the chip's notification latches after the tick has
	 * read and acted on them.  Without this, the W1C bits remain set
	 * and -- on boards that wire INTRB -- the line stays asserted,
	 * re-firing the threaded handler back to back.  The helper writes
	 * W1C bits across page 0 and page 6 and must run under
	 * multiop_lock to serialise the page selector against userspace
	 * dpll ops.  Failure is non-fatal: status was already consumed
	 * for this tick and the next tick re-processes whatever stayed
	 * latched.
	 */
	mutex_lock(&sitdev->multiop_lock);
	rc = sit9531x_clear_notifications(sitdev);
	mutex_unlock(&sitdev->multiop_lock);
	if (rc)
		dev_warn_ratelimited(sitdev->dev,
				     "Failed to clear notifications: %d\n",
				     rc);

	/* Run twice a second */
	kthread_queue_delayed_work(sitdev->kworker, &sitdev->work,
				   msecs_to_jiffies(SIT9531X_STATUS_POLL_MS));
}

/*
 * sit9531x_irq_thread_fn - threaded IRQ handler for the chip's INTRB line
 *
 * Triggered when the chip asserts INTRB (and only when DT wires up the
 * client interrupt; absent property == handler never installed).  The
 * action mirrors a periodic-work tick: queue an immediate run so status
 * registers are read and DPLL changes_check fires without waiting for
 * the next poll deadline.  Polling continues to run as a fallback.
 */
static irqreturn_t sit9531x_irq_thread_fn(int irq, void *data)
{
	struct sit9531x_dev *sitdev = data;

	kthread_mod_delayed_work(sitdev->kworker, &sitdev->work, 0);
	return IRQ_HANDLED;
}

/* ====================================================================
 * Device lifecycle -- start / stop / dpll_init / dpll_fini
 * ====================================================================
 */

/**
 * sit9531x_dev_start - start normal operation
 * @sitdev:	device pointer
 *
 * Fetches initial hardware state, registers all DPLL devices and
 * their pins, and starts the periodic monitoring thread.
 *
 * Return: 0 on success, <0 on error
 */
int sit9531x_dev_start(struct sit9531x_dev *sitdev)
{
	struct sit9531x_dpll *sitdpll;
	int rc;

	/* Fetch device state */
	rc = sit9531x_dev_state_fetch(sitdev);
	if (rc)
		return rc;

	/* Register all DPLLs */
	list_for_each_entry(sitdpll, &sitdev->dplls, list) {
		rc = sit9531x_dpll_register(sitdpll);
		if (rc) {
			dev_err_probe(sitdev->dev, rc,
				      "Failed to register DPLL%u\n",
				      sitdpll->id);
			return rc;
		}
	}

	/* Start monitoring */
	kthread_queue_delayed_work(sitdev->kworker, &sitdev->work, 0);

	return 0;
}

/**
 * sit9531x_dev_stop - stop normal operation
 * @sitdev:	device pointer
 *
 * Cancels the monitoring thread and unregisters all DPLL devices
 * and their pins.
 */
void sit9531x_dev_stop(struct sit9531x_dev *sitdev)
{
	struct sit9531x_dpll *sitdpll;

	/* Stop monitoring */
	kthread_cancel_delayed_work_sync(&sitdev->work);

	/* Unregister all DPLLs */
	list_for_each_entry(sitdpll, &sitdev->dplls, list) {
		if (sitdpll->dpll_dev)
			sit9531x_dpll_unregister(sitdpll);
	}
}

static void sit9531x_dev_dpll_fini(void *ptr)
{
	struct sit9531x_dpll *sitdpll, *next;
	struct sit9531x_dev *sitdev = ptr;

	/* Stop monitoring and unregister DPLLs */
	sit9531x_dev_stop(sitdev);

	/* Destroy monitoring thread */
	if (sitdev->kworker) {
		kthread_destroy_worker(sitdev->kworker);
		sitdev->kworker = NULL;
	}

	/* Free all DPLLs */
	list_for_each_entry_safe(sitdpll, next, &sitdev->dplls, list) {
		list_del(&sitdpll->list);
		sit9531x_dpll_free(sitdpll);
	}
}

/*
 * sit9531x_devm_dpll_init - allocate DPLLs and start the device
 * @sitdev:	device pointer
 *
 * Allocates one DPLL per PLL channel, creates the monitoring thread,
 * starts normal operation, and registers a devres cleanup action.
 *
 * Return: 0 on success, <0 on error
 */
static int sit9531x_devm_dpll_init(struct sit9531x_dev *sitdev)
{
	struct kthread_worker *kworker;
	struct sit9531x_dpll *sitdpll;
	unsigned int i;
	int rc;

	INIT_LIST_HEAD(&sitdev->dplls);

	/* Allocate all DPLLs */
	for (i = 0; i < SIT9531X_NUM_PLLS; i++) {
		sitdpll = sit9531x_dpll_alloc(sitdev, i);
		if (IS_ERR(sitdpll)) {
			dev_err_probe(sitdev->dev, PTR_ERR(sitdpll),
				      "Failed to alloc DPLL%u\n", i);
			rc = PTR_ERR(sitdpll);
			goto error;
		}

		list_add_tail(&sitdpll->list, &sitdev->dplls);
	}

	/* Initialize monitoring thread */
	kthread_init_delayed_work(&sitdev->work, sit9531x_dev_periodic_work);
	kworker = kthread_run_worker(0, "sit9531x-%s",
				     dev_name(sitdev->dev));
	if (IS_ERR(kworker)) {
		rc = PTR_ERR(kworker);
		goto error;
	}
	sitdev->kworker = kworker;

	/* Start normal operation */
	rc = sit9531x_dev_start(sitdev);
	if (rc) {
		dev_err_probe(sitdev->dev, rc, "Failed to start device\n");
		goto error;
	}

	/* Add devres action to release DPLL related resources */
	return devm_add_action_or_reset(sitdev->dev, sit9531x_dev_dpll_fini,
					sitdev);

error:
	sit9531x_dev_dpll_fini(sitdev);

	return rc;
}

/* ====================================================================
 * Chip identification
 * ====================================================================
 */

/*
 * sit9531x_read_variant_id - read chip variant ID byte from hardware
 * @sitdev:	device pointer
 * @id:		output variant ID byte
 *
 * Reads the single-byte variant identification register from Page 0
 * reg 0x02 (95317 = 0x17, 95316 = 0x31).  Reg 0x03 holds a separate
 * revision byte and is intentionally not consumed here.
 *
 * Return: 0 on success, <0 on error
 */
static int sit9531x_read_variant_id(struct sit9531x_dev *sitdev, u8 *id)
{
	return sit9531x_read_u8(sitdev, SIT9531X_REG_VARIANT_ID, id);
}

/*
 * sit9531x_match_variant - match variant ID against known variants
 * @id:	variant ID byte
 *
 * Return: pointer to chip_info on match, NULL on unknown ID
 */
static const struct sit9531x_chip_info *sit9531x_match_variant(u8 id)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(sit9531x_chip_ids); i++) {
		if (sit9531x_chip_ids[i].id == id)
			return &sit9531x_chip_ids[i];
	}

	return NULL;
}

/*
 * sit9531x_derive_clock_id - build EUI-64 clock identifier
 * @sitdev:	device pointer
 *
 * Generates a deterministic 64-bit identifier from the SiTime OUI,
 * the chip ID, and the I2C address.  This provides a stable clock_id
 * across reboots.
 *
 * Return: 64-bit clock identifier
 */
static u64 sit9531x_derive_clock_id(struct sit9531x_dev *sitdev)
{
	u64 clkid;

	clkid  = SIT9531X_OUI << 24;
	clkid |= (u64)sitdev->info->id << 8;
	clkid |= (u64)sitdev->client->addr;

	return clkid;
}

/* ====================================================================
 * Probe entry point
 * ====================================================================
 */

/**
 * sit9531x_dev_probe - initialize SiT9531x device
 * @sitdev:	pointer to device structure (caller-allocated)
 *
 * Common initialization: read chip ID, match variant, generate
 * clock_id, initialize synchronization mutex, and register DPLL
 * channels.  Called from the I2C probe function.
 *
 * Return: 0 on success, <0 on error
 */
int sit9531x_dev_probe(struct sit9531x_dev *sitdev)
{
	struct clk *xtal_clk;
	u8 variant_id;
	int rc;

	/*
	 * Read the external reference (XO) feeding the chip's XIN/XO_CLK
	 * input.  Required: Fvco computation does
	 * Fvco = Fref * (DIVN + frac/2^32) with Fref = xtal_freq << doubler,
	 * so without a populated xtal_freq every freq_set/phase_adjust path
	 * dividing by Fvco fails with -EIO.  DT property "clocks" pointing
	 * to a fixed-clock node with clock-names = "xtal" is mandatory.
	 */
	xtal_clk = devm_clk_get_optional_enabled(sitdev->dev, "xtal");
	if (IS_ERR(xtal_clk))
		return dev_err_probe(sitdev->dev, PTR_ERR(xtal_clk),
				     "Failed to get xtal clock\n");
	sitdev->xtal_freq = xtal_clk ? clk_get_rate(xtal_clk) : 0;
	if (!sitdev->xtal_freq)
		return dev_err_probe(sitdev->dev, -EINVAL,
				     "xtal clock rate is 0; DT must provide clocks=<&xo> and clock-names=\"xtal\"\n");
	dev_info(sitdev->dev, "xtal_freq=%u Hz\n", sitdev->xtal_freq);

	/* Read variant ID byte */
	rc = sit9531x_read_variant_id(sitdev, &variant_id);
	if (rc)
		return rc;

	/* Detect chip variant */
	sitdev->info = sit9531x_match_variant(variant_id);
	if (!sitdev->info)
		return dev_err_probe(sitdev->dev, -ENODEV,
				     "Unknown variant ID: 0x%02x\n", variant_id);

	dev_info(sitdev->dev, "VariantID(0x%02X), %s (%u in, %u out)\n",
		 variant_id, sitdev->info->name,
		 sitdev->info->num_inputs, sitdev->info->num_outputs);

	/* Generate deterministic clock ID */
	sitdev->clock_id = sit9531x_derive_clock_id(sitdev);

	/* Initialize mutex for multi-register atomic operations */
	rc = devm_mutex_init(sitdev->dev, &sitdev->multiop_lock);
	if (rc)
		return dev_err_probe(sitdev->dev, rc,
				     "Failed to initialize mutex\n");

	/*
	 * Optional DT-described reset line.  Requested in the deasserted
	 * state so any prior chip programming is not torn down by the
	 * request itself; the descriptor is held for an explicit reset
	 * path.  Absent DT property == descriptor stays NULL, no
	 * behaviour change.
	 */
	sitdev->reset_gpio = devm_gpiod_get_optional(sitdev->dev, "reset",
						     GPIOD_OUT_LOW);
	if (IS_ERR(sitdev->reset_gpio))
		return dev_err_probe(sitdev->dev, PTR_ERR(sitdev->reset_gpio),
				     "Failed to request reset gpio\n");
	if (sitdev->reset_gpio)
		dev_info(sitdev->dev, "reset-gpios: present (held deasserted)\n");

	/*
	 * Register DPLL channels and create the kworker first.  The IRQ
	 * handler dereferences sitdev->kworker via
	 * kthread_mod_delayed_work(), so it must be live before any
	 * INTRB assertion can land on the request_threaded_irq path.
	 */
	rc = sit9531x_devm_dpll_init(sitdev);
	if (rc)
		return rc;

	/*
	 * Optional INTRB IRQ from DT.  The I2C subsystem populates
	 * client->irq from the node's "interrupts"/"interrupts-extended"
	 * property; if no IRQ is wired client->irq is 0 and we keep
	 * relying on the periodic poll.
	 */
	sitdev->irq = sitdev->client ? sitdev->client->irq : 0;
	if (sitdev->irq > 0) {
		rc = devm_request_threaded_irq(sitdev->dev, sitdev->irq,
					       NULL, sit9531x_irq_thread_fn,
					       IRQF_ONESHOT,
					       dev_name(sitdev->dev), sitdev);
		if (rc)
			return dev_err_probe(sitdev->dev, rc,
					     "Failed to request IRQ %d\n",
					     sitdev->irq);
		dev_info(sitdev->dev,
			 "INTRB IRQ %d wired (threaded handler kicks periodic poll)\n",
			 sitdev->irq);
	}

	return 0;
}

/* ====================================================================
 * I2C driver
 * ====================================================================
 */

static int sit9531x_i2c_probe(struct i2c_client *client)
{
	struct sit9531x_dev *sitdev;
	struct regmap *regmap;

	regmap = devm_regmap_init_i2c(client, &sit9531x_regmap_config);
	if (IS_ERR(regmap))
		return dev_err_probe(&client->dev, PTR_ERR(regmap),
				     "Failed to initialize regmap\n");

	sitdev = devm_kzalloc(&client->dev, sizeof(*sitdev), GFP_KERNEL);
	if (!sitdev)
		return -ENOMEM;

	sitdev->dev = &client->dev;
	sitdev->client = client;
	sitdev->regmap = regmap;
	i2c_set_clientdata(client, sitdev);

	return sit9531x_dev_probe(sitdev);
}

static const struct i2c_device_id sit9531x_i2c_id[] = {
	{ "sit95317" },
	{ "sit95316" },
	{ }
};
MODULE_DEVICE_TABLE(i2c, sit9531x_i2c_id);

static const struct of_device_id sit9531x_of_match[] = {
	{ .compatible = "sitime,sit95317" },
	{ .compatible = "sitime,sit95316" },
	{ }
};
MODULE_DEVICE_TABLE(of, sit9531x_of_match);

static struct i2c_driver sit9531x_i2c_driver = {
	.driver = {
		.name		= "sit9531x",
		.of_match_table	= sit9531x_of_match,
	},
	.probe		= sit9531x_i2c_probe,
	.id_table	= sit9531x_i2c_id,
};
module_i2c_driver(sit9531x_i2c_driver);

MODULE_AUTHOR("Ali Rouhi <arouhi@sitime.com>");
MODULE_AUTHOR("Oleg Zadorozhnyi <Oleg.Zadorozhnyi@devoxsoftware.com>");
MODULE_DESCRIPTION("SiTime SiT9531x DPLL subsystem driver");
MODULE_LICENSE("GPL");

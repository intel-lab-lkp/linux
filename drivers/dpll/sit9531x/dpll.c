// SPDX-License-Identifier: GPL-2.0
/*
 * SiTime SiT9531x DPLL subsystem callbacks and registration
 *
 * Copyright (C) 2026 SiTime Corp.
 * Author: Ali Rouhi <arouhi@sitime.com>
 * Author: Oleg Zadorozhnyi <Oleg.Zadorozhnyi@devoxsoftware.com>
 *
 * DPLL device ops, pin ops (separate input/output), pin registration,
 * and periodic change detection.
 *
 */

#include <linux/dpll.h>
#include <linux/err.h>
#include <linux/kthread.h>
#include <linux/list.h>
#include <linux/netlink.h>
#include <linux/slab.h>

#include "core.h"
#include "dpll.h"
#include "prop.h"
#include "regs.h"

/* Number of input + output pin positions for pin index allocation */
#define SIT9531X_NUM_INPUT_PINS		(SIT9531X_MAX_INPUTS + 1) /* +xtal */
#define SIT9531X_NUM_OUTPUT_PINS	SIT9531X_MAX_OUTPUTS
#define SIT9531X_NUM_PINS_TOTAL		(SIT9531X_NUM_INPUT_PINS + SIT9531X_NUM_OUTPUT_PINS)

#define SIT9531X_ESYNC_FREQ_10MHZ	10000000ULL
#define SIT9531X_ESYNC_PULSE_DEFAULT	50

static const struct dpll_pin_frequency sit9531x_esync_ranges[] = {
	DPLL_PIN_FREQUENCY(0),
	DPLL_PIN_FREQUENCY(SIT9531X_ESYNC_FREQ_10MHZ),
};

static inline bool
sit9531x_dpll_esync_pin_supported(const struct sit9531x_dpll_pin *dpin)
{
	return dpin->esync_control;
}

/* ====================================================================
 * Pin direction helpers
 * ====================================================================
 */

static inline bool sit9531x_dpll_is_input_pin(const struct sit9531x_dpll_pin *pin)
{
	return pin->dir == DPLL_PIN_DIRECTION_INPUT;
}

/* ====================================================================
 * dpll_device_ops callbacks
 * ====================================================================
 */

/*
 * sit9531x_dpll_lock_status_get - report PLL lock/holdover state
 *
 * reads holdover register (Page 0 reg 0x06), PLL
 * status register (PLL page reg 0x31), inner LOL (reg 0x92), and
 * holdover freeze (reg 0x0A) to determine lock status and error.
 */
static int
sit9531x_dpll_lock_status_get(const struct dpll_device *dpll, void *dpll_priv,
			      enum dpll_lock_status *status,
			      enum dpll_lock_status_error *status_error,
			      struct netlink_ext_ack *extack)
{
	struct sit9531x_dpll *sitdpll = dpll_priv;
	const struct sit9531x_chan *chan;

	if (status_error)
		*status_error = DPLL_LOCK_STATUS_ERROR_NONE;

	chan = sit9531x_chan_state_get(sitdpll->dev, sitdpll->id);

	if (chan->locked) {
		if (chan->mode)
			*status = DPLL_LOCK_STATUS_LOCKED;       /* free-run */
		else
			*status = DPLL_LOCK_STATUS_LOCKED_HO_ACQ; /* sync */
	} else if (chan->ho_freeze) {
		*status = DPLL_LOCK_STATUS_HOLDOVER;
	} else {
		*status = DPLL_LOCK_STATUS_UNLOCKED;
	}

	/* Report inner LOL as an error condition */
	if (status_error && chan->inner_lol)
		*status_error = DPLL_LOCK_STATUS_ERROR_UNDEFINED;

	return 0;
}

/*
 * sit9531x_dpll_mode_get - report current PLL operating mode
 *
 * reads outer loop disable bit (PLL page reg 0x31[5]).
 * Free-run -> MANUAL, sync -> AUTOMATIC.
 */
static int
sit9531x_dpll_mode_get(const struct dpll_device *dpll, void *dpll_priv,
		       enum dpll_mode *mode, struct netlink_ext_ack *extack)
{
	struct sit9531x_dpll *sitdpll = dpll_priv;
	const struct sit9531x_chan *chan;

	chan = sit9531x_chan_state_get(sitdpll->dev, sitdpll->id);

	*mode = chan->mode ? DPLL_MODE_MANUAL : DPLL_MODE_AUTOMATIC;

	return 0;
}

/*
 * sit9531x_dpll_mode_set - switch PLL between free-run and sync mode
 *
 * writes PLL page reg 0x31[5] to enable/disable the
 * outer loop, then triggers a small update via reg 0x0F.
 */
static int
sit9531x_dpll_mode_set(const struct dpll_device *dpll, void *dpll_priv,
		       enum dpll_mode mode, struct netlink_ext_ack *extack)
{
	struct sit9531x_dpll *sitdpll = dpll_priv;
	struct sit9531x_dev *sitdev = sitdpll->dev;
	u8 val;
	int rc;

	/*
	 * Outer loop disable bit:
	 *   0 = sync mode (outer loop enabled)  -> AUTOMATIC
	 *   1 = free-run (outer loop disabled)  -> MANUAL
	 */
	val = (mode == DPLL_MODE_MANUAL) ? SIT9531X_PLL_STATUS_OUTER_DIS : 0;

	mutex_lock(&sitdev->multiop_lock);

	rc = sit9531x_update_pll_u8(sitdev, sitdpll->id,
				    SIT9531X_PLL_REG_STATUS,
				    SIT9531X_PLL_STATUS_OUTER_DIS, val);
	if (rc) {
		NL_SET_ERR_MSG(extack, "Failed to write PLL mode register");
		goto unlock;
	}

	/* Trigger small update to apply without full NVM cycle */
	rc = sit9531x_write_pll_u8(sitdev, sitdpll->id,
				   SIT9531X_PLL_REG_SMALL_UPDATE,
				   SIT9531X_PLL_SMALL_UPDATE_CMD);
	if (rc)
		NL_SET_ERR_MSG(extack, "Failed to trigger small update");

unlock:
	mutex_unlock(&sitdev->multiop_lock);

	return rc;
}

/*
 * sit9531x_dpll_supported_modes_get - report which modes the PLL supports
 */
static int
sit9531x_dpll_supported_modes_get(const struct dpll_device *dpll,
				  void *dpll_priv, unsigned long *modes,
				  struct netlink_ext_ack *extack)
{
	__set_bit(DPLL_MODE_AUTOMATIC, modes);
	__set_bit(DPLL_MODE_MANUAL, modes);

	return 0;
}

static const struct dpll_device_ops sit9531x_dpll_device_ops = {
	.lock_status_get	= sit9531x_dpll_lock_status_get,
	.mode_get		= sit9531x_dpll_mode_get,
	.mode_set		= sit9531x_dpll_mode_set,
	.supported_modes_get	= sit9531x_dpll_supported_modes_get,
	/* temp_get not available -- SiT9531x has no on-die temp sensor */
};

/* ====================================================================
 * Input pin ops
 * ====================================================================
 */

static int
sit9531x_dpll_input_pin_direction_get(const struct dpll_pin *pin,
				      void *pin_priv,
				      const struct dpll_device *dpll,
				      void *dpll_priv,
				      enum dpll_pin_direction *direction,
				      struct netlink_ext_ack *extack)
{
	*direction = DPLL_PIN_DIRECTION_INPUT;
	return 0;
}

/*
 * sit9531x_dpll_input_pin_frequency_get - read input pin frequency
 *
 * returns cached frequency from DT or last set.
 */
static int
sit9531x_dpll_input_pin_frequency_get(const struct dpll_pin *pin,
				      void *pin_priv,
				      const struct dpll_device *dpll,
				      void *dpll_priv, u64 *frequency,
				      struct netlink_ext_ack *extack)
{
	struct sit9531x_dpll_pin *dpin = pin_priv;
	struct sit9531x_dpll *sitdpll = dpll_priv;
	const struct sit9531x_ref *ref;

	ref = sit9531x_ref_state_get(sitdpll->dev, dpin->id);
	*frequency = ref->freq;

	return 0;
}

/*
 * sit9531x_dpll_input_pin_state_on_dpll_get - get input pin DPLL state
 *
 * determines pin state from channel state: connected
 * if this input is the selected reference on a locked PLL, selectable
 * if enabled in automatic mode, disconnected otherwise.
 */
static int
sit9531x_dpll_input_pin_state_on_dpll_get(const struct dpll_pin *pin,
					  void *pin_priv,
					  const struct dpll_device *dpll,
					  void *dpll_priv,
					  enum dpll_pin_state *state,
					  struct netlink_ext_ack *extack)
{
	struct sit9531x_dpll_pin *dpin = pin_priv;
	struct sit9531x_dpll *sitdpll = dpll_priv;
	const struct sit9531x_chan *chan;
	const struct sit9531x_ref *ref;

	chan = sit9531x_chan_state_get(sitdpll->dev, sitdpll->id);
	ref = sit9531x_ref_state_get(sitdpll->dev, dpin->id);

	/* Check if this input is the connected reference */
	if (chan->locked && chan->selected_ref == dpin->id) {
		*state = DPLL_PIN_STATE_CONNECTED;
		return 0;
	}

	/* In auto mode, enabled inputs with good signal are selectable */
	if (!chan->mode && ref->enabled && !ref->los && !ref->oof) {
		*state = DPLL_PIN_STATE_SELECTABLE;
		return 0;
	}

	*state = DPLL_PIN_STATE_DISCONNECTED;
	return 0;
}

/*
 * sit9531x_dpll_input_pin_state_on_dpll_set - set input pin DPLL state
 *
 * disables or enables the input by writing Page 0x02
 * force/state registers via sit9531x_input_disable/enable().
 *   DISCONNECTED -> disable input (force override, clear state)
 *   SELECTABLE   -> enable input (release force override)
 *   CONNECTED    -> enable input (same as SELECTABLE; actual selection
 *                  is done by the PLL auto-switching logic)
 */
static int
sit9531x_dpll_input_pin_state_on_dpll_set(const struct dpll_pin *pin,
					  void *pin_priv,
					  const struct dpll_device *dpll,
					  void *dpll_priv,
					  enum dpll_pin_state state,
					  struct netlink_ext_ack *extack)
{
	struct sit9531x_dpll_pin *dpin = pin_priv;
	struct sit9531x_dpll *sitdpll = dpll_priv;
	struct sit9531x_dev *sitdev = sitdpll->dev;
	int rc;

	mutex_lock(&sitdev->multiop_lock);

	switch (state) {
	case DPLL_PIN_STATE_DISCONNECTED:
		rc = sit9531x_input_disable(sitdev, dpin->id);
		break;
	case DPLL_PIN_STATE_SELECTABLE:
	case DPLL_PIN_STATE_CONNECTED:
		rc = sit9531x_input_enable(sitdev, dpin->id);
		break;
	default:
		rc = -EINVAL;
		break;
	}

	mutex_unlock(&sitdev->multiop_lock);

	if (rc)
		NL_SET_ERR_MSG(extack, "Failed to set input pin state");

	return rc;
}

/*
 * sit9531x_dpll_input_pin_prio_get - read input pin priority
 *
 * returns cached priority (populated from clock monitor
 * registers during pin registration).
 */
static int
sit9531x_dpll_input_pin_prio_get(const struct dpll_pin *pin, void *pin_priv,
				 const struct dpll_device *dpll, void *dpll_priv,
				 u32 *prio, struct netlink_ext_ack *extack)
{
	struct sit9531x_dpll_pin *dpin = pin_priv;

	*prio = dpin->prio;
	return 0;
}

/*
 * sit9531x_dpll_input_pin_prio_set - set input pin priority
 *
 * writes input priority table on Page 1 via
 * core.c sit9531x_input_prio_set().  Forces holdover during update.
 */
static int
sit9531x_dpll_input_pin_prio_set(const struct dpll_pin *pin, void *pin_priv,
				 const struct dpll_device *dpll, void *dpll_priv,
				 u32 prio, struct netlink_ext_ack *extack)
{
	struct sit9531x_dpll_pin *dpin = pin_priv;
	struct sit9531x_dpll *sitdpll = dpll_priv;
	struct sit9531x_dev *sitdev = sitdpll->dev;
	int rc;

	if (dpin->dir != DPLL_PIN_DIRECTION_INPUT) {
		NL_SET_ERR_MSG(extack, "Priority applies only to input pins");
		return -EINVAL;
	}

	if (prio >= SIT9531X_PRIO_MAX_SLOTS) {
		NL_SET_ERR_MSG(extack, "Priority out of range (0-11)");
		return -EINVAL;
	}

	mutex_lock(&sitdev->multiop_lock);
	rc = sit9531x_input_prio_set(sitdev, sitdpll->id, dpin->id,
				     (u8)prio);
	mutex_unlock(&sitdev->multiop_lock);

	if (rc) {
		NL_SET_ERR_MSG(extack, "Failed to set input priority");
		return rc;
	}

	dpin->prio = (u8)prio;

	return 0;
}

/*
 * sit9531x_dpll_input_pin_phase_adjust_get - read phase adjustment
 *
 * returns cached phase adjustment value (in ps).
 */
static int
sit9531x_dpll_input_pin_phase_adjust_get(const struct dpll_pin *pin,
					 void *pin_priv,
					 const struct dpll_device *dpll,
					 void *dpll_priv, s32 *phase_adjust,
					 struct netlink_ext_ack *extack)
{
	struct sit9531x_dpll_pin *dpin = pin_priv;

	*phase_adjust = dpin->phase_adjust;
	return 0;
}

/*
 * sit9531x_dpll_input_pin_phase_offset_get - read phase offset
 *
 * reads the TDC (Time-to-Digital Converter) hardware
 * to measure the phase difference in picoseconds via
 * sit9531x_phase_offset_read().
 */
static int
sit9531x_dpll_input_pin_phase_offset_get(const struct dpll_pin *pin,
					 void *pin_priv,
					 const struct dpll_device *dpll,
					 void *dpll_priv, s64 *phase_offset,
					 struct netlink_ext_ack *extack)
{
	struct sit9531x_dpll_pin *dpin = pin_priv;
	struct sit9531x_dpll *sitdpll = dpll_priv;
	struct sit9531x_dev *sitdev = sitdpll->dev;
	s64 offset;
	int rc;

	mutex_lock(&sitdev->multiop_lock);
	rc = sit9531x_phase_offset_read(sitdev, sitdpll->id, &offset);
	mutex_unlock(&sitdev->multiop_lock);

	/*
	 * -ENODEV means the PLL has no programmed DIVN (unused on this
	 * board); report phase_offset = 0 so a full pin-get dump does not
	 * fail just because one DPLL is dormant.
	 */
	if (rc == -ENODEV) {
		dpin->phase_offset = 0;
		*phase_offset = 0;
		return 0;
	}
	if (rc) {
		NL_SET_ERR_MSG(extack, "TDC phase readback failed");
		return rc;
	}

	dpin->phase_offset = offset;
	*phase_offset = offset;
	return 0;
}

static const struct dpll_pin_ops sit9531x_dpll_input_pin_ops = {
	.direction_get		= sit9531x_dpll_input_pin_direction_get,
	.frequency_get		= sit9531x_dpll_input_pin_frequency_get,
	.state_on_dpll_get	= sit9531x_dpll_input_pin_state_on_dpll_get,
	.state_on_dpll_set	= sit9531x_dpll_input_pin_state_on_dpll_set,
	.prio_get		= sit9531x_dpll_input_pin_prio_get,
	.prio_set		= sit9531x_dpll_input_pin_prio_set,
	.phase_adjust_get	= sit9531x_dpll_input_pin_phase_adjust_get,
	.phase_offset_get	= sit9531x_dpll_input_pin_phase_offset_get,
};

/* ====================================================================
 * Output pin ops
 * ====================================================================
 */

static int
sit9531x_dpll_output_pin_direction_get(const struct dpll_pin *pin,
				       void *pin_priv,
				       const struct dpll_device *dpll,
				       void *dpll_priv,
				       enum dpll_pin_direction *direction,
				       struct netlink_ext_ack *extack)
{
	*direction = DPLL_PIN_DIRECTION_OUTPUT;
	return 0;
}

/*
 * sit9531x_dpll_output_pin_frequency_get - read output pin frequency
 *
 * returns cached frequency from state fetch.
 * TODO: compute from VCO / divider chain for live readback.
 */
static int
sit9531x_dpll_output_pin_frequency_get(const struct dpll_pin *pin,
				       void *pin_priv,
				       const struct dpll_device *dpll,
				       void *dpll_priv, u64 *frequency,
				       struct netlink_ext_ack *extack)
{
	struct sit9531x_dpll_pin *dpin = pin_priv;
	struct sit9531x_dpll *sitdpll = dpll_priv;
	const struct sit9531x_out *out;

	out = sit9531x_out_state_get(sitdpll->dev, dpin->id);
	*frequency = out->freq;

	return 0;
}

/*
 * sit9531x_dpll_output_pin_frequency_set - set output pin frequency
 *
 * computes DIVO = Fvco / frequency and writes the
 * 34-bit output divider to the output system registers via
 * sit9531x_output_freq_set().
 */
static int
sit9531x_dpll_output_pin_frequency_set(const struct dpll_pin *pin,
				       void *pin_priv,
				       const struct dpll_device *dpll,
				       void *dpll_priv, u64 frequency,
				       struct netlink_ext_ack *extack)
{
	struct sit9531x_dpll_pin *dpin = pin_priv;
	struct sit9531x_dpll *sitdpll = dpll_priv;
	struct sit9531x_dev *sitdev = sitdpll->dev;
	u8 actual_pll;
	int rc;

	/* Use the actual PLL that drives this output (populated by
	 * out_state_fetch from the chip's OUT_MAP registers), not the
	 * DPLL device id -- in our current registration all outputs are
	 * bound to DPLL 0 for convenience, but physically they may be
	 * driven by PLL A/B/C/D with different Fvco.
	 */
	actual_pll = sitdev->out[dpin->id].pll_idx;

	mutex_lock(&sitdev->multiop_lock);
	rc = sit9531x_output_freq_set(sitdev, dpin->id, actual_pll,
				      frequency);
	mutex_unlock(&sitdev->multiop_lock);

	if (rc)
		NL_SET_ERR_MSG(extack, "Output frequency set failed");

	return rc;
}

/*
 * sit9531x_dpll_output_pin_state_on_dpll_get - get output pin state
 *
 * reports CONNECTED when the output is driven and
 * DISCONNECTED when it has been muted via sit9531x_output_disable().
 */
static int
sit9531x_dpll_output_pin_state_on_dpll_get(const struct dpll_pin *pin,
					   void *pin_priv,
					   const struct dpll_device *dpll,
					   void *dpll_priv,
					   enum dpll_pin_state *state,
					   struct netlink_ext_ack *extack)
{
	struct sit9531x_dpll_pin *dpin = pin_priv;
	struct sit9531x_dpll *sitdpll = dpll_priv;
	const struct sit9531x_out *out;

	out = sit9531x_out_state_get(sitdpll->dev, dpin->id);
	*state = out->enabled ? DPLL_PIN_STATE_CONNECTED
			      : DPLL_PIN_STATE_DISCONNECTED;
	return 0;
}

/*
 * sit9531x_dpll_output_pin_state_on_dpll_set - mute/un-mute an output
 *
 * forces Hi-Z on the output pin via the Page 0x03
 * force/state register pair.
 *   CONNECTED    -> enable (release force, back to factory default)
 *   DISCONNECTED -> disable (force Hi-Z)
 */
static int
sit9531x_dpll_output_pin_state_on_dpll_set(const struct dpll_pin *pin,
					   void *pin_priv,
					   const struct dpll_device *dpll,
					   void *dpll_priv,
					   enum dpll_pin_state state,
					   struct netlink_ext_ack *extack)
{
	struct sit9531x_dpll_pin *dpin = pin_priv;
	struct sit9531x_dpll *sitdpll = dpll_priv;
	struct sit9531x_dev *sitdev = sitdpll->dev;
	int rc;

	mutex_lock(&sitdev->multiop_lock);

	switch (state) {
	case DPLL_PIN_STATE_CONNECTED:
		rc = sit9531x_output_enable(sitdev, dpin->id);
		break;
	case DPLL_PIN_STATE_DISCONNECTED:
		rc = sit9531x_output_disable(sitdev, dpin->id);
		break;
	default:
		rc = -EINVAL;
		break;
	}

	mutex_unlock(&sitdev->multiop_lock);

	if (rc)
		NL_SET_ERR_MSG(extack, "Failed to set output pin state");

	return rc;
}

/*
 * sit9531x_dpll_output_pin_phase_adjust_get - read output phase adjustment
 *
 * returns cached value.
 */
static int
sit9531x_dpll_output_pin_phase_adjust_get(const struct dpll_pin *pin,
					  void *pin_priv,
					  const struct dpll_device *dpll,
					  void *dpll_priv, s32 *phase_adjust,
					  struct netlink_ext_ack *extack)
{
	struct sit9531x_dpll_pin *dpin = pin_priv;

	*phase_adjust = dpin->phase_adjust;
	return 0;
}

/*
 * sit9531x_dpll_output_pin_phase_adjust_set - set output phase adjustment
 *
 * Programs the per-output PRG_RST_DELAY registers for deterministic
 * phase offset; see sit9531x_output_phase_adjust_set() in core.c.
 */
static int
sit9531x_dpll_output_pin_phase_adjust_set(const struct dpll_pin *pin,
					  void *pin_priv,
					  const struct dpll_device *dpll,
					  void *dpll_priv, s32 phase_adjust,
					  struct netlink_ext_ack *extack)
{
	struct sit9531x_dpll_pin *dpin = pin_priv;
	struct sit9531x_dpll *sitdpll = dpll_priv;
	struct sit9531x_dev *sitdev = sitdpll->dev;
	int rc;

	mutex_lock(&sitdev->multiop_lock);
	rc = sit9531x_output_phase_adjust_set(sitdev, dpin->id, phase_adjust);
	mutex_unlock(&sitdev->multiop_lock);

	if (rc) {
		NL_SET_ERR_MSG(extack, "Phase adjust failed");
		return rc;
	}

	dpin->phase_adjust = phase_adjust;
	return 0;
}

static int
sit9531x_dpll_output_pin_esync_get(const struct dpll_pin *pin,
				   void *pin_priv,
				   const struct dpll_device *dpll,
				   void *dpll_priv,
				   struct dpll_pin_esync *esync,
				   struct netlink_ext_ack *extack)
{
	struct sit9531x_dpll_pin *dpin = pin_priv;

	if (!sit9531x_dpll_esync_pin_supported(dpin))
		return -EOPNOTSUPP;

	esync->range = sit9531x_esync_ranges;
	esync->range_num = ARRAY_SIZE(sit9531x_esync_ranges);
	esync->pulse = SIT9531X_ESYNC_PULSE_DEFAULT;
	esync->freq = dpin->esync_freq;

	return 0;
}

static int
sit9531x_dpll_output_pin_esync_set(const struct dpll_pin *pin,
				   void *pin_priv,
				   const struct dpll_device *dpll,
				   void *dpll_priv,
				   u64 freq,
				   struct netlink_ext_ack *extack)
{
	struct sit9531x_dpll_pin *dpin = pin_priv;
	struct sit9531x_dpll *sitdpll = dpll_priv;
	struct sit9531x_dev *sitdev = sitdpll->dev;
	u8 actual_pll;
	int rc;

	if (!sit9531x_dpll_esync_pin_supported(dpin)) {
		NL_SET_ERR_MSG(extack,
			       "Embedded sync not enabled for this pin");
		return -EOPNOTSUPP;
	}

	actual_pll = sitdev->out[dpin->id].pll_idx;

	mutex_lock(&sitdev->multiop_lock);

	/*
	 * This output is a dedicated embedded-sync pin.
	 * Treat freq=0 as a request to disable the entire output.
	 */
	if (!freq) {
		rc = sit9531x_output_disable(sitdev, dpin->id);
		if (!rc)
			dpin->esync_freq = 0;
		mutex_unlock(&sitdev->multiop_lock);
		return rc;
	}

	if (freq != SIT9531X_ESYNC_FREQ_10MHZ) {
		mutex_unlock(&sitdev->multiop_lock);
		NL_SET_ERR_MSG(extack,
			       "Only 10 MHz esync frequency is supported");
		return -EINVAL;
	}

	rc = sit9531x_output_freq_set(sitdev, dpin->id, actual_pll,
				      SIT9531X_ESYNC_FREQ_10MHZ);
	if (!rc)
		rc = sit9531x_output_enable(sitdev, dpin->id);

	mutex_unlock(&sitdev->multiop_lock);

	if (!rc)
		dpin->esync_freq = SIT9531X_ESYNC_FREQ_10MHZ;

	return rc;
}

static const struct dpll_pin_ops sit9531x_dpll_output_pin_ops = {
	.direction_get		= sit9531x_dpll_output_pin_direction_get,
	.frequency_get		= sit9531x_dpll_output_pin_frequency_get,
	.frequency_set		= sit9531x_dpll_output_pin_frequency_set,
	.state_on_dpll_get	= sit9531x_dpll_output_pin_state_on_dpll_get,
	.state_on_dpll_set	= sit9531x_dpll_output_pin_state_on_dpll_set,
	.phase_adjust_get	= sit9531x_dpll_output_pin_phase_adjust_get,
	.phase_adjust_set	= sit9531x_dpll_output_pin_phase_adjust_set,
	.esync_get		= sit9531x_dpll_output_pin_esync_get,
	.esync_set		= sit9531x_dpll_output_pin_esync_set,
};

/* ====================================================================
 * Pin allocation, registration, and cleanup
 * ====================================================================
 */

/*
 * sit9531x_dpll_pin_alloc - allocate a DPLL pin
 * @sitdpll:	DPLL device this pin belongs to
 * @dir:	pin direction
 * @id:		hardware pin index
 *
 * Return: pointer to allocated pin on success, error pointer on error
 */
static struct sit9531x_dpll_pin *
sit9531x_dpll_pin_alloc(struct sit9531x_dpll *sitdpll,
			enum dpll_pin_direction dir, u8 id)
{
	struct sit9531x_dpll_pin *pin;

	pin = kzalloc_obj(*pin, GFP_KERNEL);
	if (!pin)
		return ERR_PTR(-ENOMEM);

	pin->dpll = sitdpll;
	pin->dir = dir;
	pin->id = id;

	return pin;
}

/*
 * sit9531x_dpll_pin_free - deallocate a DPLL pin
 * @pin:	pin to free
 */
static void sit9531x_dpll_pin_free(struct sit9531x_dpll_pin *pin)
{
	WARN(pin->dpll_pin, "DPLL pin is still registered\n");
	kfree(pin);
}

/*
 * sit9531x_dpll_pin_register - register a DPLL pin with the subsystem
 * @pin:	pin to register
 * @index:	absolute pin index for clock_id namespace
 *
 * Gets pin properties from firmware, creates or gets a dpll_pin,
 * and registers it with the parent DPLL device.
 *
 * Return: 0 on success, <0 on error
 */
static int sit9531x_dpll_pin_register(struct sit9531x_dpll_pin *pin,
				      u32 index)
{
	struct sit9531x_dpll *sitdpll = pin->dpll;
	struct sit9531x_pin_props *props;
	const struct dpll_pin_ops *ops;
	int rc;

	/* Get pin properties from firmware nodes */
	props = sit9531x_pin_props_get(sitdpll->dev, pin->dir, pin->id);
	if (IS_ERR(props))
		return PTR_ERR(props);

	/* Save package label and firmware node */
	strscpy(pin->label, props->package_label, sizeof(pin->label));
	pin->fwnode = fwnode_handle_get(props->fwnode);
	pin->esync_control = props->esync_control;

	/* Create or get existing DPLL pin */
	pin->dpll_pin = dpll_pin_get(sitdpll->dev->clock_id, index,
				     THIS_MODULE, &props->dpll_props,
				     &pin->tracker);
	if (IS_ERR(pin->dpll_pin)) {
		rc = PTR_ERR(pin->dpll_pin);
		goto err_pin_get;
	}
	dpll_pin_fwnode_set(pin->dpll_pin, props->fwnode);

	if (sit9531x_dpll_is_input_pin(pin))
		ops = &sit9531x_dpll_input_pin_ops;
	else
		ops = &sit9531x_dpll_output_pin_ops;

	/* Register the pin */
	rc = dpll_pin_register(sitdpll->dpll_dev, pin->dpll_pin, ops, pin);
	if (rc)
		goto err_register;

	/* Free pin properties */
	sit9531x_pin_props_put(props);

	return 0;

err_register:
	dpll_pin_put(pin->dpll_pin, &pin->tracker);
	pin->dpll_pin = NULL;
err_pin_get:
	fwnode_handle_put(pin->fwnode);
	pin->fwnode = NULL;
	sit9531x_pin_props_put(props);

	return rc;
}

/*
 * sit9531x_dpll_pin_unregister - unregister a DPLL pin
 * @pin:	pin to unregister
 */
static void sit9531x_dpll_pin_unregister(struct sit9531x_dpll_pin *pin)
{
	struct sit9531x_dpll *sitdpll = pin->dpll;
	const struct dpll_pin_ops *ops;

	WARN(!pin->dpll_pin, "DPLL pin is not registered\n");

	if (sit9531x_dpll_is_input_pin(pin))
		ops = &sit9531x_dpll_input_pin_ops;
	else
		ops = &sit9531x_dpll_output_pin_ops;

	dpll_pin_unregister(sitdpll->dpll_dev, pin->dpll_pin, ops, pin);
	dpll_pin_put(pin->dpll_pin, &pin->tracker);
	pin->dpll_pin = NULL;

	fwnode_handle_put(pin->fwnode);
	pin->fwnode = NULL;
}

/*
 * sit9531x_dpll_pins_unregister - unregister all pins on a DPLL
 * @sitdpll:	DPLL device
 */
static void sit9531x_dpll_pins_unregister(struct sit9531x_dpll *sitdpll)
{
	struct sit9531x_dpll_pin *pin, *next;

	list_for_each_entry_safe(pin, next, &sitdpll->pins, list) {
		sit9531x_dpll_pin_unregister(pin);
		list_del(&pin->list);
		sit9531x_dpll_pin_free(pin);
	}
}

/*
 * sit9531x_dpll_pin_is_registrable - check if a pin should be registered
 * @sitdpll:	DPLL device
 * @dir:	pin direction
 * @index:	pin hardware index
 *
 * For input pins: the pin is registrable if it is enabled.
 * For output pins: the pin is registrable if it is driven by this DPLL.
 *
 * Return: true if pin should be registered, false otherwise
 */
static bool sit9531x_dpll_pin_is_registrable(struct sit9531x_dpll *sitdpll,
					     enum dpll_pin_direction dir,
					     u8 index)
{
	struct sit9531x_dev *sitdev = sitdpll->dev;

	if (dir == DPLL_PIN_DIRECTION_INPUT) {
		/* All configured inputs are registrable */
		return index < sitdev->info->num_inputs;
	}

	/* Output -- check if driven by this DPLL */
	if (index >= sitdev->info->num_outputs)
		return false;

	return sitdev->out[index].pll_idx == sitdpll->id &&
	       sitdev->out[index].enabled;
}

/*
 * sit9531x_dpll_pins_register - register all registrable pins
 * @sitdpll:	DPLL device
 *
 * Enumerates all possible input and output pins, checks registrability,
 * and registers each one.  Input pins come first, then output pins,
 * with input pins first, then output pins.
 *
 * Return: 0 on success, <0 on error
 */
static int sit9531x_dpll_pins_register(struct sit9531x_dpll *sitdpll)
{
	struct sit9531x_dpll_pin *pin;
	enum dpll_pin_direction dir;
	u8 id, index;
	int rc;

	for (index = 0; index < SIT9531X_NUM_PINS_TOTAL; index++) {
		if (index < SIT9531X_NUM_INPUT_PINS) {
			id = index;
			dir = DPLL_PIN_DIRECTION_INPUT;
		} else {
			id = index - SIT9531X_NUM_INPUT_PINS;
			dir = DPLL_PIN_DIRECTION_OUTPUT;
		}

		if (!sit9531x_dpll_pin_is_registrable(sitdpll, dir, id))
			continue;

		pin = sit9531x_dpll_pin_alloc(sitdpll, dir, id);
		if (IS_ERR(pin)) {
			rc = PTR_ERR(pin);
			goto error;
		}

		rc = sit9531x_dpll_pin_register(pin, index);
		if (rc) {
			sit9531x_dpll_pin_free(pin);
			goto error;
		}

		list_add(&pin->list, &sitdpll->pins);
	}

	return 0;

error:
	sit9531x_dpll_pins_unregister(sitdpll);
	return rc;
}

/* ====================================================================
 * DPLL device registration
 * ====================================================================
 */

static void sit9531x_dpll_change_work(struct work_struct *work)
{
	struct sit9531x_dpll *sitdpll;

	sitdpll = container_of(work, struct sit9531x_dpll, change_work);
	dpll_device_change_ntf(sitdpll->dpll_dev);
}

/*
 * sit9531x_dpll_device_register - register a DPLL device
 * @sitdpll:	DPLL to register
 *
 * Return: 0 on success, <0 on error
 */
static int sit9531x_dpll_device_register(struct sit9531x_dpll *sitdpll)
{
	struct sit9531x_dev *sitdev = sitdpll->dev;
	int rc;

	sitdpll->ops = sit9531x_dpll_device_ops;

	sitdpll->dpll_dev = dpll_device_get(sitdev->clock_id, sitdpll->id,
					    THIS_MODULE, &sitdpll->tracker);
	if (IS_ERR(sitdpll->dpll_dev)) {
		rc = PTR_ERR(sitdpll->dpll_dev);
		sitdpll->dpll_dev = NULL;
		return rc;
	}

	rc = dpll_device_register(sitdpll->dpll_dev,
				  sit9531x_prop_dpll_type_get(sitdev,
							      sitdpll->id),
				  &sitdpll->ops, sitdpll);
	if (rc) {
		dpll_device_put(sitdpll->dpll_dev, &sitdpll->tracker);
		sitdpll->dpll_dev = NULL;
	}

	return rc;
}

/*
 * sit9531x_dpll_device_unregister - unregister a DPLL device
 * @sitdpll:	DPLL to unregister
 */
static void sit9531x_dpll_device_unregister(struct sit9531x_dpll *sitdpll)
{
	WARN(!sitdpll->dpll_dev, "DPLL device is not registered\n");

	cancel_work_sync(&sitdpll->change_work);

	dpll_device_unregister(sitdpll->dpll_dev, &sitdpll->ops, sitdpll);
	dpll_device_put(sitdpll->dpll_dev, &sitdpll->tracker);
	sitdpll->dpll_dev = NULL;
}

/* ====================================================================
 * DPLL allocation and top-level register/unregister
 * ====================================================================
 */

/**
 * sit9531x_dpll_alloc - allocate a DPLL device structure
 * @sitdev:	parent device
 * @ch:		PLL channel number (0-3)
 *
 * Return: pointer to allocated DPLL on success, error pointer on error
 */
struct sit9531x_dpll *sit9531x_dpll_alloc(struct sit9531x_dev *sitdev, u8 ch)
{
	struct sit9531x_dpll *sitdpll;

	sitdpll = kzalloc_obj(*sitdpll, GFP_KERNEL);
	if (!sitdpll)
		return ERR_PTR(-ENOMEM);

	sitdpll->dev = sitdev;
	sitdpll->id = ch;
	sitdpll->lock_status = DPLL_LOCK_STATUS_UNLOCKED;
	INIT_LIST_HEAD(&sitdpll->pins);
	INIT_WORK(&sitdpll->change_work, sit9531x_dpll_change_work);

	return sitdpll;
}

/**
 * sit9531x_dpll_free - deallocate a DPLL device structure
 * @sitdpll:	DPLL to free
 */
void sit9531x_dpll_free(struct sit9531x_dpll *sitdpll)
{
	WARN(sitdpll->dpll_dev, "DPLL device is still registered\n");
	kfree(sitdpll);
}

/**
 * sit9531x_dpll_register - register DPLL device and all its pins
 * @sitdpll:	DPLL device
 *
 * Registers the DPLL device with the subsystem and then registers
 * all input and output pins that are connected to this PLL.
 *
 * Return: 0 on success, <0 on error
 */
int sit9531x_dpll_register(struct sit9531x_dpll *sitdpll)
{
	int rc;

	rc = sit9531x_dpll_device_register(sitdpll);
	if (rc)
		return rc;

	rc = sit9531x_dpll_pins_register(sitdpll);
	if (rc) {
		sit9531x_dpll_device_unregister(sitdpll);
		return rc;
	}

	return 0;
}

/**
 * sit9531x_dpll_unregister - unregister DPLL device and its pins
 * @sitdpll:	DPLL device
 */
void sit9531x_dpll_unregister(struct sit9531x_dpll *sitdpll)
{
	sit9531x_dpll_pins_unregister(sitdpll);
	sit9531x_dpll_device_unregister(sitdpll);
}

/* ====================================================================
 * Periodic change detection
 * ====================================================================
 */

/**
 * sit9531x_dpll_changes_check - check for state changes and notify
 * @sitdpll:	DPLL device
 *
 * Called from sit9531x_dev_periodic_work().  Compares current hardware
 * state against cached values and sends netlink notifications on changes.
 */
void sit9531x_dpll_changes_check(struct sit9531x_dpll *sitdpll)
{
	struct sit9531x_dev *sitdev = sitdpll->dev;
	enum dpll_lock_status lock_status;
	struct sit9531x_dpll_pin *pin;
	int rc;

	/* Get current lock status */
	rc = sit9531x_dpll_lock_status_get(sitdpll->dpll_dev, sitdpll,
					   &lock_status, NULL, NULL);
	if (rc) {
		dev_err(sitdev->dev, "Failed to get DPLL%u lock status: %d\n",
			sitdpll->id, rc);
		return;
	}

	/* If lock status changed, notify DPLL core */
	if (sitdpll->lock_status != lock_status) {
		sitdpll->lock_status = lock_status;
		dpll_device_change_ntf(sitdpll->dpll_dev);
	}

	/* Check input pins for state changes */
	list_for_each_entry(pin, &sitdpll->pins, list) {
		enum dpll_pin_state state;
		bool changed = false;

		/* Output pin states are constant -- skip */
		if (!sit9531x_dpll_is_input_pin(pin))
			continue;

		rc = sit9531x_dpll_input_pin_state_on_dpll_get(pin->dpll_pin,
							       pin,
							       sitdpll->dpll_dev,
							       sitdpll,
							       &state,
							       NULL);
		if (rc)
			continue;

		if (state != pin->pin_state) {
			dev_dbg(sitdev->dev, "%s state changed: %u->%u\n",
				pin->label, pin->pin_state, state);
			pin->pin_state = state;
			changed = true;
		}

		if (changed)
			dpll_pin_change_ntf(pin->dpll_pin);
	}
}

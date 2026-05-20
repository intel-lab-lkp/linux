/* SPDX-License-Identifier: GPL-2.0 */
/*
 * SiTime SiT9531x DPLL core driver
 *
 * Copyright (C) 2026 SiTime Corp.
 * Author: Ali Rouhi <arouhi@sitime.com>
 * Author: Oleg Zadorozhnyi <Oleg.Zadorozhnyi@devoxsoftware.com>
 *
 * Device structure, register access helpers, and core function
 * declarations.
 */

#ifndef _SIT9531X_CORE_H
#define _SIT9531X_CORE_H

#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/kthread.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/regmap.h>
#include <linux/types.h>

#include "regs.h"

#define SIT9531X_NUM_PLLS		4
#define SIT9531X_MAX_INPUTS		8
#define SIT9531X_MAX_OUTPUTS		12
#define SIT9531X_NUM_PINS		(SIT9531X_MAX_INPUTS + 1 + SIT9531X_MAX_OUTPUTS)
#define SIT9531X_STATUS_POLL_MS		500

/* SiTime IEEE OUI for EUI-64 generation */
#define SIT9531X_OUI			0x0090C2FFFEULL

struct sit9531x_dpll;

/**
 * struct sit9531x_chip_info - chip variant identification
 * @id:		variant ID byte read from register
 * @num_inputs:	number of input clock pins
 * @num_outputs: number of output clock pins
 * @name:	human-readable variant name
 * @clkout_map:	per-output slot mapping (output index -> physical slot)
 */
struct sit9531x_chip_info {
	u8		id;
	u8		num_inputs;
	u8		num_outputs;
	const char	*name;
	const u8	*clkout_map;
};

/**
 * enum sit9531x_signal_mode - input signal electrical mode
 * @SIT9531X_MODE_SE: single-ended
 * @SIT9531X_MODE_DE: differential
 */
enum sit9531x_signal_mode {
	SIT9531X_MODE_SE = 0,
	SIT9531X_MODE_DE,
};

/**
 * enum sit9531x_polarity - input signal polarity (for single-ended)
 * @SIT9531X_POL_P: positive / non-inverted
 * @SIT9531X_POL_N: negative / inverted
 */
enum sit9531x_polarity {
	SIT9531X_POL_P = 0,
	SIT9531X_POL_N,
};

/**
 * struct sit9531x_ref - input reference state
 * @freq:		configured frequency in Hz
 * @enabled:		reference is enabled for monitoring
 * @los:		loss-of-signal detected
 * @oof:		out-of-frequency detected
 * @pll_mask:		bitmask of PLLs this input feeds (bit 0 = PLLA)
 * @label:		board label from DT or default
 * @sig_mode:		signal mode (SE or DE)
 * @polarity:		polarity (P or N, for SE only)
 */
struct sit9531x_ref {
	u32		freq;
	bool		enabled;
	bool		los;
	bool		oof;
	u8		pll_mask;
	const char	*label;
	enum sit9531x_signal_mode	sig_mode;
	enum sit9531x_polarity		polarity;
};

/**
 * struct sit9531x_out - output state
 * @freq:		current output frequency in Hz
 * @enabled:		output is enabled
 * @pll_idx:		PLL driving this output (0-3)
 * @label:		board label from DT or default
 */
struct sit9531x_out {
	u32		freq;
	bool		enabled;
	u8		pll_idx;
	const char	*label;
};

/**
 * struct sit9531x_chan - per-PLL channel state
 * @lock_status:	hardware lock status (raw register bit)
 * @mode:		0 = sync (outer loop enabled), 1 = free-run
 * @selected_ref:	currently selected input reference index
 * @inner_lol:		PLL inner loop loss-of-lock detected
 * @ho_freeze:		holdover freeze active
 */
struct sit9531x_chan {
	bool		locked;
	u8		mode;
	u8		selected_ref;
	bool		inner_lol;
	bool		ho_freeze;
};

/**
 * struct sit9531x_dev - SiT9531x device instance
 * @dev:		pointer to device
 * @client:		I2C client
 * @regmap:		regmap for register access
 * @info:		detected chip variant info
 * @multiop_lock:	mutex for multi-register atomic operations
 * @ref:		array of input reference states
 * @out:		array of output states
 * @chan:		array of per-PLL channel states
 * @xtal_freq:		crystal oscillator frequency in Hz
 * @dplls:		list of registered DPLL devices
 * @kworker:		kthread worker for periodic polling
 * @work:		delayed work for periodic state checks
 * @clock_id:		IEEE 1588 EUI-64 clock identifier
 * @reset_gpio:		optional reset line (DT "reset-gpios"), NULL if absent
 * @irq:		optional INTRB IRQ number (from DT "interrupts" via the
 *			I2C client), 0 if no IRQ is wired
 */
struct sit9531x_dev {
	struct device			*dev;
	struct i2c_client		*client;
	struct regmap			*regmap;
	const struct sit9531x_chip_info	*info;
	/* Serialises multi-step register sequences */
	struct mutex			multiop_lock;

	/* Hardware state */
	struct sit9531x_ref	ref[SIT9531X_MAX_INPUTS + 1]; /* +1 for xtal */
	struct sit9531x_out	out[SIT9531X_MAX_OUTPUTS];
	struct sit9531x_chan	chan[SIT9531X_NUM_PLLS];
	u32			xtal_freq;

	/* DPLL channels */
	struct list_head	dplls;

	/* Monitor */
	struct kthread_worker		*kworker;
	struct kthread_delayed_work	work;

	/* Device identity */
	u64			clock_id;

	/* Optional DT-described GPIO / IRQ lines */
	struct gpio_desc	*reset_gpio;
	int			irq;
};

extern const struct regmap_config sit9531x_regmap_config;

/* ---- Core lifecycle ---- */
int  sit9531x_dev_probe(struct sit9531x_dev *sitdev);
int  sit9531x_dev_start(struct sit9531x_dev *sitdev);
void sit9531x_dev_stop(struct sit9531x_dev *sitdev);

/* ---- Register access ---- */
int sit9531x_read_u8(struct sit9531x_dev *sitdev, unsigned int reg,
		     u8 *val);
int sit9531x_write_u8(struct sit9531x_dev *sitdev, unsigned int reg,
		      u8 val);
int sit9531x_read_pll_u8(struct sit9531x_dev *sitdev, u8 pll_idx,
			 u8 offset, u8 *val);
int sit9531x_write_pll_u8(struct sit9531x_dev *sitdev, u8 pll_idx,
			  u8 offset, u8 val);
int sit9531x_update_pll_u8(struct sit9531x_dev *sitdev, u8 pll_idx,
			   u8 offset, u8 mask, u8 val);

/* ---- Input enable/disable ---- */
int sit9531x_input_disable(struct sit9531x_dev *sitdev, u8 index);
int sit9531x_input_enable(struct sit9531x_dev *sitdev, u8 index);

/* ---- Input priority ---- */
int sit9531x_input_prio_set(struct sit9531x_dev *sitdev, u8 pll_idx,
			    u8 input_idx, u8 prio);

/* ---- Output enable/disable (Hi-Z control) ---- */
int sit9531x_output_disable(struct sit9531x_dev *sitdev, u8 index);
int sit9531x_output_enable(struct sit9531x_dev *sitdev, u8 index);

/* ---- DCO tuning ---- */
int sit9531x_dco_adjust(struct sit9531x_dev *sitdev, u8 pll_idx, s64 ppb);

/* ---- Output frequency ---- */
int sit9531x_output_freq_set(struct sit9531x_dev *sitdev, u8 out_idx,
			     u8 pll_idx, u64 frequency);

/* ---- Output phase adjust (PRG_RST_DELAY register-based) ---- */
int sit9531x_output_phase_adjust_set(struct sit9531x_dev *sitdev,
				     u8 out_idx, s32 phase_ps);

/* ---- PLL lock-detection thresholds (LL_REG2_PLL, reg 0x2A) ---- */
int sit9531x_pll_lock_threshold_set(struct sit9531x_dev *sitdev,
				    u8 pll_idx, u8 set_val, u8 clr_val);

/* ---- Notification clear ---- */
int sit9531x_clear_notifications(struct sit9531x_dev *sitdev);

/* ---- INTSYNC (inter-PLL synchronization) ---- */
int sit9531x_intsync_enable(struct sit9531x_dev *sitdev, u8 src_pll_idx);
int sit9531x_intsync_disable(struct sit9531x_dev *sitdev, u8 src_pll_idx);

/* ---- Per-PLL SYSREF / SYNCB / Pulser output mode ---- */
enum sit9531x_sysref_mode {
	SIT9531X_SYSREF_DISABLE,
	SIT9531X_SYSREF_MODE_SYSREF,
	SIT9531X_SYSREF_MODE_SYNCB,
	SIT9531X_SYSREF_MODE_PULSER,
};

int sit9531x_pll_sysref_mode_set(struct sit9531x_dev *sitdev, u8 pll_idx,
				 enum sit9531x_sysref_mode mode,
				 u16 target_outputs);
int sit9531x_output_pulse_ctrl_set(struct sit9531x_dev *sitdev,
				   u8 out_idx, u8 pulse_ctrl);

/* ---- Phase offset (TDC readback) ---- */
int sit9531x_phase_offset_read(struct sit9531x_dev *sitdev, u8 pll_idx,
			       s64 *phase_ps);

/* ---- State helpers ---- */

/**
 * sit9531x_pll_page - get register page for PLL index
 * @pll_idx: PLL index (0 = PLLA, 3 = PLLD)
 */
static inline u8 sit9531x_pll_page(u8 pll_idx)
{
	return SIT9531X_PAGE_PLLA + pll_idx;
}

/**
 * sit9531x_ref_state_get - get reference state by index
 */
static inline const struct sit9531x_ref *
sit9531x_ref_state_get(const struct sit9531x_dev *sitdev, u8 index)
{
	return &sitdev->ref[index];
}

/**
 * sit9531x_out_state_get - get output state by index
 */
static inline const struct sit9531x_out *
sit9531x_out_state_get(const struct sit9531x_dev *sitdev, u8 index)
{
	return &sitdev->out[index];
}

/**
 * sit9531x_chan_state_get - get channel state by PLL index
 */
static inline const struct sit9531x_chan *
sit9531x_chan_state_get(const struct sit9531x_dev *sitdev, u8 pll_idx)
{
	return &sitdev->chan[pll_idx];
}

#endif /* _SIT9531X_CORE_H */

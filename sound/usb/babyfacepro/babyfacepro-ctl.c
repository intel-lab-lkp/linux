// SPDX-License-Identifier: GPL-2.0-only
/*
 * RME Babyface Pro FS — proprietary-mode USB audio driver
 *
 * ALSA control surface (mixer, front panel, DSP EQ).  This is the
 * initial slice of the series: the card's PCM stream + lifecycle come
 * from babyfacepro.c, while the control surface is stubbed out here so
 * the module links.  The real mixer, front-panel and DSP EQ land in the
 * follow-up patches (mixer, panel, eq) — each replaces its stubs.
 *
 * See babyfacepro.h for the shared device state and register map.
 */
#include <linux/log2.h>
#include <linux/math64.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/unaligned.h>
#include <linux/usb.h>
#include <linux/workqueue.h>
#include <sound/control.h>
#include <sound/tlv.h>
#include <sound/core.h>
#include <sound/initval.h>
#include <sound/pcm.h>

#include "babyfacepro.h"

const struct bf_source bf_sources[14] = {
	{ "AN1",     0,  0 },
	{ "AN2",     1,  1 },
	{ "AN3",     2,  2 },
	{ "AN4",     3,  3 },
	{ "AS1/2",   4,  5 },
	{ "ADAT3/4", 6,  7 },
	{ "ADAT5/6", 8,  9 },
	{ "ADAT7/8", 10, 11 },
	{ "PB1",    12, 13 },
	{ "PB2",    14, 15 },
	{ "PB3",    16, 17 },
	{ "PB4",    18, 19 },
	{ "PB5",    20, 21 },
	{ "PB6",    22, 23 },
};

/* Crosspoint-map output order vs the master-map order — HARDWARE-
 * VERIFIED 2026-08-24: the block that feeds the Phones is the FIRST
 * crosspoint block (0x34), while the Phones master is the SECOND
 * (0x03E2/0x0006).  The crosspoint map lists the Phones first (the
 * monitor output); the master map lists AN1/2 first.  Control index =
 * the canonical order (AN1/2=0, PH3/4=1, ...) so the crosspoint and
 * master controls line up; this table maps to the register block.
 */
const u8 bf_xpoint_block[6] = { 1, 0, 2, 3, 4, 5 };

/* ── control-surface stubs ──────────────────────────────────────
 * Filled in by the mixer / panel / eq patches.  The core driver
 * (babyfacepro.c) calls these from probe() and the stream/state
 * paths, so they must exist for the module to link.  Until then the
 * card exposes the PCM stream only.
 */

int babyface_write_default_mixer(struct snd_usb_babyface *chip)
{
	return 0;
}

int bf_apply_masters(struct snd_usb_babyface *chip)
{
	return 0;
}

int bf_loopback_write_map(struct snd_usb_babyface *chip, int out, bool on)
{
	return 0;
}

int bf_preamp_state_write(struct snd_usb_babyface *chip)
{
	return 0;
}

int babyface_create_controls(struct snd_usb_babyface *chip)
{
	return 0;
}

int babyface_create_xpoints(struct snd_usb_babyface *chip)
{
	return 0;
}

int babyface_create_flags(struct snd_usb_babyface *chip)
{
	return 0;
}

int babyface_create_panel(struct snd_usb_babyface *chip)
{
	return 0;
}

int babyface_create_eq(struct snd_usb_babyface *chip)
{
	return 0;
}

void babyface_panel_start(struct snd_usb_babyface *chip)
{
}

void babyface_panel_stop(struct snd_usb_babyface *chip)
{
}

void babyface_panel_work(struct work_struct *work)
{
}

void bf_eq_reupload(struct snd_usb_babyface *chip)
{
}

u8 bf_gain_raw(int mic, int db)
{
	return 0;
}

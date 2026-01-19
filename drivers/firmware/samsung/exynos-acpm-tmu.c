// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright 2020 Samsung Electronics Co., Ltd.
 * Copyright 2020 Google LLC.
 * Copyright 2026 Linaro Ltd.
 */

#include <linux/bitfield.h>
#include <linux/bits.h>
#include <linux/firmware/samsung/exynos-acpm-protocol.h>
#include <linux/ktime.h>
#include <linux/types.h>
#include <linux/units.h>

#include "exynos-acpm.h"
#include "exynos-acpm-tmu.h"

/* IPC Request Types */
#define ACPM_TMU_INIT		0x01
#define ACPM_TMU_READ_TEMP	0x02
#define ACPM_TMU_SUSPEND	0x04
#define ACPM_TMU_RESUME		0x10
#define ACPM_TMU_THRESHOLD	0x11
#define ACPM_TMU_INTEN		0x12
#define ACPM_TMU_CONTROL	0x13
#define ACPM_TMU_IRQ_CLEAR	0x14
#define ACPM_TMU_HYSTERESIS	0x16

#define ACPM_TMU_TX_DATA_LEN	8
#define ACPM_TMU_RX_DATA_LEN	7

struct acpm_tmu_tx {
	u16 ctx;
	u16 fw_use;
	u8 type;
	u8 rsvd0;
	u8 tzid;
	u8 rsvd1;
	u8 data[ACPM_TMU_TX_DATA_LEN];
} __packed;

struct acpm_tmu_rx {
	u16 ctx;
	u16 fw_use;
	u8 type;
	s8 ret;
	u8 tzid;
	s8 temp;
	u8 rsvd;
	u8 data[ACPM_TMU_RX_DATA_LEN];
} __packed;

union acpm_tmu_msg {
	u32 data[4];
	struct acpm_tmu_tx tx;
	struct acpm_tmu_rx rx;
} __packed;

static void acpm_tmu_set_xfer(struct acpm_xfer *xfer, u32 *cmd, size_t cmdlen,
			      unsigned int acpm_chan_id)
{
	xfer->acpm_chan_id = acpm_chan_id;
	xfer->txd = cmd;
	xfer->txlen = cmdlen;
	xfer->rxd = cmd;
	xfer->rxlen = cmdlen;
}

int acpm_tmu_init(const struct acpm_handle *handle, unsigned int acpm_chan_id)
{
	union acpm_tmu_msg msg = {0};
	struct acpm_xfer xfer;

	msg.tx.type = ACPM_TMU_INIT;
	acpm_tmu_set_xfer(&xfer, msg.data, sizeof(msg.data), acpm_chan_id);

	return acpm_do_xfer(handle, &xfer);
}

int acpm_tmu_read_temp(const struct acpm_handle *handle,
		       unsigned int acpm_chan_id, u8 tz, int *temp)
{
	union acpm_tmu_msg msg = {0};
	struct acpm_xfer xfer;
	int ret;

	msg.tx.type = ACPM_TMU_READ_TEMP;
	msg.tx.tzid = tz;

	acpm_tmu_set_xfer(&xfer, msg.data, sizeof(msg.data), acpm_chan_id);

	ret = acpm_do_xfer(handle, &xfer);
	if (ret)
		return ret;

	*temp = msg.rx.temp;

	return 0;
}

int acpm_tmu_set_threshold(const struct acpm_handle *handle,
			   unsigned int acpm_chan_id, u8 tz,
			   const u8 temperature[8], size_t tlen)
{
	union acpm_tmu_msg msg = {0};
	struct acpm_xfer xfer;
	int i;

	if (tlen > ACPM_TMU_TX_DATA_LEN)
		return -EINVAL;

	msg.tx.type = ACPM_TMU_THRESHOLD;
	msg.tx.tzid = tz;

	for (i = 0; i < tlen; i++)
		msg.tx.data[i] = temperature[i];

	acpm_tmu_set_xfer(&xfer, msg.data, sizeof(msg.data), acpm_chan_id);

	return acpm_do_xfer(handle, &xfer);
}

int acpm_tmu_set_hysteresis(const struct acpm_handle *handle,
			    unsigned int acpm_chan_id, u8 tz,
			    const u8 hysteresis[8], size_t hlen)
{
	union acpm_tmu_msg msg = {0};
	struct acpm_xfer xfer;
	int i;

	if (hlen > ACPM_TMU_TX_DATA_LEN)
		return -EINVAL;

	msg.tx.type = ACPM_TMU_HYSTERESIS;
	msg.tx.tzid = tz;

	for (i = 0; i < hlen; i++)
		msg.tx.data[i] = hysteresis[i];

	acpm_tmu_set_xfer(&xfer, msg.data, sizeof(msg.data), acpm_chan_id);

	return acpm_do_xfer(handle, &xfer);
}

int acpm_tmu_set_interrupt_enable(const struct acpm_handle *handle,
				  unsigned int acpm_chan_id, u8 tz, u8 inten)
{
	union acpm_tmu_msg msg = {0};
	struct acpm_xfer xfer;

	msg.tx.type = ACPM_TMU_INTEN;
	msg.tx.tzid = tz;
	msg.tx.data[0] = inten;

	acpm_tmu_set_xfer(&xfer, msg.data, sizeof(msg.data), acpm_chan_id);

	return acpm_do_xfer(handle, &xfer);
}

int acpm_tmu_tz_control(const struct acpm_handle *handle,
			unsigned int acpm_chan_id, u8 tz, bool enable)
{
	union acpm_tmu_msg msg = {0};
	struct acpm_xfer xfer;

	msg.tx.type = ACPM_TMU_CONTROL;
	msg.tx.tzid = tz;
	msg.tx.data[0] = enable ? 1 : 0;

	acpm_tmu_set_xfer(&xfer, msg.data, sizeof(msg.data), acpm_chan_id);

	return acpm_do_xfer(handle, &xfer);
}

int acpm_tmu_clear_tz_irq(const struct acpm_handle *handle,
			  unsigned int acpm_chan_id, u8 tz)
{
	union acpm_tmu_msg msg = {0};
	struct acpm_xfer xfer;

	msg.tx.type = ACPM_TMU_IRQ_CLEAR;
	msg.tx.tzid = tz;

	acpm_tmu_set_xfer(&xfer, msg.data, sizeof(msg.data), acpm_chan_id);

	return acpm_do_xfer(handle, &xfer);
}

int acpm_tmu_suspend(const struct acpm_handle *handle,
		     unsigned int acpm_chan_id)
{
	union acpm_tmu_msg msg = {0};
	struct acpm_xfer xfer;

	msg.tx.type = ACPM_TMU_SUSPEND;

	acpm_tmu_set_xfer(&xfer, msg.data, sizeof(msg.data), acpm_chan_id);

	return acpm_do_xfer(handle, &xfer);
}

int acpm_tmu_resume(const struct acpm_handle *handle, unsigned int acpm_chan_id)
{
	union acpm_tmu_msg msg = {0};
	struct acpm_xfer xfer;

	msg.tx.type = ACPM_TMU_RESUME;

	acpm_tmu_set_xfer(&xfer, msg.data, sizeof(msg.data), acpm_chan_id);

	return acpm_do_xfer(handle, &xfer);
}

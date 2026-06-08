/* SPDX-License-Identifier: GPL-2.0-or-later */

#ifndef _REALTEK_PSE_H
#define _REALTEK_PSE_H

#include <linux/mutex.h>
#include <linux/pse-pd/pse.h>
#include <linux/types.h>

/*
 * Time the MCU itself needs between accepting a request and having a
 * response ready. These are properties of the MCU firmware, not of the
 * underlying transport: the core paces transactions by RTPSE_MCU_RESPONSE_MS
 * and both transports size their per-transaction recv ceiling from
 * RTPSE_MCU_RESPONSE_MAX_MS, since some commands are documented as
 * needing up to ~1s to produce a reply.
 */
#define RTPSE_MCU_RESPONSE_MS			25
#define RTPSE_MCU_RESPONSE_MAX_MS		1000

/*
 * Total time to keep retrying the first MCU read at probe, and the pause
 * between attempts. Right after enable-gpios is asserted the MCU may not
 * answer on the bus yet; give it a bounded window to come up before
 * declaring the probe failed.
 */
#define RTPSE_MCU_BOOT_TIMEOUT_MS		3000
#define RTPSE_MCU_BOOT_RETRY_MS			100

#define RTPSE_MCU_MSG_SIZE			12

struct rtpse_mcu_msg {
	u8 opcode;
	u8 seq_num;
	u8 payload[9];
	u8 checksum;
} __packed;

/* Opaque to transports; defined in realtek-pse-core.c. */
struct rtpse_mcu_dialect;
struct rtpse_match_data;
struct rtpse_chip_info;
struct rtpse_ctrl;

struct rtpse_transport_ops {
	int (*send)(struct rtpse_ctrl *pse, const struct rtpse_mcu_msg *req);
	int (*recv)(struct rtpse_ctrl *pse, const struct rtpse_mcu_msg *req,
		    struct rtpse_mcu_msg *resp);
};

struct rtpse_ctrl {
	struct device *dev;
	struct pse_controller_dev pcdev;
	struct mutex mutex; /* serializes MCU request/response transactions */
	const struct rtpse_mcu_dialect *dialect;
	const struct rtpse_chip_info *chip;
	const struct rtpse_transport_ops *transport;

	struct regulator *poe_supply;
};

int rtpse_register(struct rtpse_ctrl *pse);

/* Whether the I2C transport must read "realtek,i2c-protocol" from DT. */
bool rtpse_needs_i2c_proto(const struct rtpse_match_data *match);

extern const struct rtpse_match_data rtpse_rtk_data;
extern const struct rtpse_match_data rtpse_bcm_data;

#endif

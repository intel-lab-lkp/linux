/* SPDX-License-Identifier: GPL-2.0 */

#ifndef __DRIVER_USB_TYPEC_CROS_EC_UCSI_NL_H
#define __DRIVER_USB_TYPEC_CROS_EC_UCSI_NL_H

#define NL_CROS_EC_NAME "cros_ec_ucsi"
#define NL_CROS_EC_VER 1
#define NL_CROS_EC_MC_GRP_NAME "cros_ec_ucsi_mc"

/* attributes */
enum nl_cros_ec_attrs {
	NL_CROS_EC_A_SRC,
	NL_CROS_EC_A_DIR,
	NL_CROS_EC_A_OFFSET,
	NL_CROS_EC_A_CMD_TYPE,
	NL_CROS_EC_A_TSTAMP_SEC,
	NL_CROS_EC_A_TSTAMP_USEC,
	NL_CROS_EC_A_PAYLOAD,
	NL_CROS_EC_A_MAX
};

enum nl_cros_ec_cmds {
	NL_CROS_EC_C_UCSI,
	NL_CROS_EC_C_MAX
};

/* where message was captured - EC or AP */
enum nl_cros_ec_src {
	NL_CROS_EC_AP,
	NL_CROS_EC_EC
};

/* message destination */
enum nl_cros_ec_msg_dir {
	NL_CROS_EC_TO_PPM,
	NL_CROS_EC_TO_OPM,
	NL_CROS_EC_TO_LPM
};

/* command type - read or write */
enum nl_cros_ec_cmd_type {
	NL_CROS_EC_RD,
	NL_CROS_EC_WR
};

int nl_cros_ec_register(void);
void nl_cros_ec_unregister(void);
int nl_cros_ec_bcast_msg(enum nl_cros_ec_msg_dir dir,
			 enum nl_cros_ec_cmd_type cmd_type,
			 u16 offset, const u8 *payload, size_t msg_size);

#endif /* __DRIVER_USB_TYPEC_CROS_EC_UCSI_NL_H */

/* SPDX-License-Identifier: GPL-2.0 */

#include <linux/usb/typec_dp.h>
#include <linux/usb/typec_tbt.h>

static inline int typec_svid_to_altmode(const u16 svid)
{
	switch (svid) {
	case USB_TYPEC_DP_SID:
		return TYPEC_DP_ALTMODE;
	case USB_TYPEC_TBT_SID:
		return TYPEC_TBT_ALTMODE;
	}
	return TYPEC_MODE_MAX;
}

int typec_mode_priorities_set(struct typec_port *port,
		const char *user_priorities);
int typec_mode_priorities_get(struct typec_port *port, char *buf);

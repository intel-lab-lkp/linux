/* SPDX-License-Identifier: GPL-2.0 */

#include <linux/usb/typec_dp.h>
#include <linux/usb/typec_tbt.h>

static inline enum typec_mode_type typec_svid_to_altmode(const u16 svid)
{
	switch (svid) {
	case USB_TYPEC_DP_SID:
		return TYPEC_DP_ALTMODE;
	case USB_TYPEC_TBT_SID:
		return TYPEC_TBT_ALTMODE;
	}
	return TYPEC_MODE_MAX;
}

void typec_mode_selection_init(struct typec_port *port);
void typec_mode_selection_destroy(struct typec_port *port);
int typec_mode_set_priority(struct typec_port *port,
		const enum typec_mode_type mode, const int priority);
int typec_mode_get_priority(struct typec_port *port,
		const enum typec_mode_type mode, int *priority);
ssize_t typec_mode_get_priority_list(struct typec_port *port, char *buf);

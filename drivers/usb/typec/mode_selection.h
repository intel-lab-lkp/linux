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

/**
 * The mode selection process follows a lifecycle tied to the USB-C partner
 * device. The API is designed to first build a set of desired modes and then
 * trigger the selection process. The expected sequence of calls is as follows:
 *
 * Creation and Configuration:
 * call typec_mode_selection_create() when the partner device is being set
 * up. This allocates resources for the mode selection.
 * After creation, call typec_mode_selection_add_mode() and
 * typec_mode_selection_add_cable() to define the parameters for the
 * selection process.
 *
 * Execution:
 * Call typec_mode_selection_start() to trigger the mode selection.
 * Call typec_mode_selection_reset() to prematurely stop the selection
 * process and clear any stored results.
 *
 * Destruction:
 * Before destroying a partner, call typec_mode_selection_destroy()
 */
int typec_mode_selection_create(struct typec_partner *partner);
void typec_mode_selection_destroy(struct typec_partner *partner);
int typec_mode_selection_start(struct typec_partner *partner);
int typec_mode_selection_reset(struct typec_partner *partner);
void typec_mode_selection_add_mode(struct typec_partner *partner,
		const int mode);
void typec_mode_selection_add_cable(struct typec_partner *partner,
		struct typec_cable *cable);
int typec_mode_selection_get(struct typec_partner *partner, char *buf);

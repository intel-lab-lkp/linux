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

/**
 * The mode selection process follows a lifecycle tied to the USB-C partner
 * device. The API is designed to first build a set of desired modes and then
 * trigger the selection process. The expected sequence of calls is as follows:
 *
 * Creation and Configuration:
 * call typec_mode_selection_add_partner() when the partner device is being set
 * up. After creation, call typec_mode_selection_add_mode() and
 * typec_mode_selection_add_cable() to define the parameters for the
 * selection process.
 *
 * Execution:
 * Call typec_mode_selection_start() to trigger the mode selection.
 * Call typec_mode_selection_reset() to prematurely stop the selection
 * process and clear any stored results.
 *
 * Destruction:
 * Before destroying a partner, call typec_mode_selection_remove_partner()
 */
void typec_mode_selection_add_partner(struct typec_partner *partner);
void typec_mode_selection_remove_partner(struct typec_partner *partner);
int typec_mode_selection_start(struct typec_partner *partner);
int typec_mode_selection_reset(struct typec_partner *partner);
void typec_mode_selection_add_mode(struct typec_partner *partner,
		const enum typec_mode_type mode);
void typec_mode_selection_add_cable(struct typec_partner *partner,
		struct typec_cable *cable);
int typec_mode_selection_get_active(struct typec_partner *partner, char *buf);
int typec_mode_selection_get_result(struct typec_partner *partner,
		const enum typec_mode_type mode, char *buf);

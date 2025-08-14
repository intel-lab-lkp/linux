/* SPDX-License-Identifier: GPL-2.0 */

#include <linux/usb/typec_dp.h>
#include <linux/usb/typec_tbt.h>

int typec_mode_set_priority(struct typec_altmode *adev, const int priority);
int typec_mode_get_priority(struct typec_altmode *adev, int *priority);
void typec_mode_selection_destroy(struct typec_port *port);

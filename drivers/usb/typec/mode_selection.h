/* SPDX-License-Identifier: GPL-2.0 */

int typec_mode_priorities_set(struct typec_port *port,
		const char *user_priorities);
int typec_mode_priorities_get(struct typec_port *port, char *buf);

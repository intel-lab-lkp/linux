/* SPDX-License-Identifier: GPL-2.0 */

#ifndef __USB_TYPEC_ALTMODE_H__
#define __USB_TYPEC_ALTMODE_H__

#include <linux/usb/typec_altmode.h>

struct typec_mux;
struct typec_retimer;

#define TYPEC_ALTMODE_MAX_PARTNERS 2

struct altmode {
	unsigned int			id;
	struct typec_altmode		adev;
	struct typec_mux		*mux;
	struct typec_retimer		*retimer;

	enum typec_port_data		roles;

	struct attribute		*attrs[5];
	char				group_name[8];
	struct attribute_group		group;
	const struct attribute_group	*groups[2];

	struct altmode			*partners[TYPEC_ALTMODE_MAX_PARTNERS];
	struct altmode			*plug[2];
};

#define to_altmode(d) container_of(d, struct altmode, adev)

void typec_altmode_dump(char *name, struct altmode *alt);
int typec_altmode_get_partner_idx_by_name(struct altmode *altmode, const char *name);
bool typec_altmode_partners_is_empty(struct altmode *altmode);

#endif /* __USB_TYPEC_ALTMODE_H__ */

// SPDX-License-Identifier: GPL-2.0 or BSD-3-Clause
/*
 * Copyright(c) 2025-2026 Cornelis Networks, Inc.
 */

#include <linux/types.h>
#include <linux/string.h>

#include "pinning.h"
#include "trace.h"

struct pinning_interface hfi2_pinning_interfaces[PINNING_MAX_INTERFACES];

void hfi2_register_pinning_interface(unsigned int type,
				struct pinning_interface *interface)
{
	hfi2_pinning_interfaces[type] = *interface;
}

void hfi2_deregister_pinning_interface(unsigned int type)
{
	memset(&hfi2_pinning_interfaces[type], 0, sizeof(hfi2_pinning_interfaces[type]));
}

int hfi2_init_pinning_interfaces(struct hfi2_user_sdma_pkt_q *pq)
{
	int i;
	int ret;

	for (i = 0; i < PINNING_MAX_INTERFACES; i++) {
		if (hfi2_pinning_interfaces[i].init) {
			ret = hfi2_pinning_interfaces[i].init(pq);
			if (ret)
				goto fail;
		}
	}

	return 0;

fail:
	while (--i >= 0) {
		if (hfi2_pinning_interfaces[i].free)
			hfi2_pinning_interfaces[i].free(pq);
	}
	return ret;
}

void hfi2_free_pinning_interfaces(struct hfi2_user_sdma_pkt_q *pq)
{
	unsigned int i;

	for (i = 0; i < PINNING_MAX_INTERFACES; i++) {
		if (trace_pin_stats_enabled() &&
		    hfi2_pinning_interfaces[i].get_stats) {
			struct hfi2_pin_stats s = {0};
			int ret;

			ret = hfi2_pinning_interfaces[i].get_stats(pq, 0, &s);
			if (!WARN_ON_ONCE(ret))
				trace_pin_stats(pq, &s);
		}

		if (hfi2_pinning_interfaces[i].free)
			hfi2_pinning_interfaces[i].free(pq);
	}
}

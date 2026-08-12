/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __ACPI_NUMA_HMAT_TEST_H
#define __ACPI_NUMA_HMAT_TEST_H

#include <linux/acpi.h>
#include <linux/list.h>

#if IS_ENABLED(CONFIG_KUNIT)
int hmat_parse_p2p(struct acpi_hmat_p2p_latency *p2p, u8 revision,
		   struct list_head *localities);
int hmat_get_p2p_coordinates(struct list_head *localities, int initiator_pxm,
			     int target_pxm, enum hmat_p2p_class class,
			     struct access_coordinate *coord);
void hmat_free_p2p_localities(struct list_head *localities);
#endif

#endif /* __ACPI_NUMA_HMAT_TEST_H */

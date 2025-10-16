// SPDX-License-Identifier: GPL-2.0

#include "resctrl.h"
#include <stdio.h>

int resctrl_find_pmu_type(const char *pmu_name)
{
	char path[256];
	FILE *file;
	int type;

	if (!pmu_name)
		return -1;

	snprintf(path, sizeof(path), "/sys/bus/event_source/devices/%s/type",
		 pmu_name);

	file = fopen(path, "r");
	if (!file) {
		ksft_print_msg("Failed to open %s: %s\n", path, strerror(errno));
		return -1;
	}

	if (fscanf(file, "%d", &type) != 1) {
		ksft_print_msg("Failed to read PMU type from %s\n", path);
		fclose(file);
		return -1;
	}

	fclose(file);
	return type;
}

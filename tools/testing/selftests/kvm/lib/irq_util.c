// SPDX-License-Identifier: GPL-2.0
#include "kvm_util.h"
#include "test_util.h"
#include "irq_util.h"

#include <ctype.h>

static FILE *open_proc_interrupts(void)
{
	FILE *fp;

	fp = fopen("/proc/interrupts", "r");
	TEST_ASSERT(fp, "fopen(/proc/interrupts) failed");

	return fp;
}

int get_irq_number(const char *device_bdf, int msi)
{
	char search_string[64];
	char line[4096];
	int irq = -1;
	FILE *fp;

	fp = open_proc_interrupts();

	snprintf(search_string, sizeof(search_string), "vfio-msix[%d]", msi);

	while (fgets(line, sizeof(line), fp)) {
		if (strstr(line, device_bdf) && strstr(line, search_string)) {
			TEST_ASSERT_EQ(1, sscanf(line, "%d:", &irq));
			break;
		}
	}

	fclose(fp);

	TEST_ASSERT(irq != -1, "Failed to locate IRQ for %s %s", device_bdf, search_string);
	return irq;
}

static int parse_interrupt_count(char *token)
{
	char *c;

	for (c = token; *c; c++) {
		if (!isdigit(*c))
			return 0;
	}

	return atoi_non_negative("interrupt count", token);
}

uint64_t get_irq_count_by_name(const char *name)
{
	uint64_t total_count = 0;
	char line[4096];
	FILE *fp;

	fp = open_proc_interrupts();

	while (fgets(line, sizeof(line), fp)) {
		char *token = strtok(line, " ");

		if (strcmp(token, name))
			continue;

		while ((token = strtok(NULL, " ")))
			total_count += parse_interrupt_count(token);

		break;
	}

	fclose(fp);
	return total_count;
}

uint64_t get_irq_count(int irq)
{
	char search_string[32];

	snprintf(search_string, sizeof(search_string), "%d:", irq);
	return get_irq_count_by_name(search_string);
}

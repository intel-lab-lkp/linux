// SPDX-License-Identifier: GPL-2.0
#include "kvm_util.h"
#include "test_util.h"
#include "proc_util.h"

static FILE *open_proc_interrupts(void)
{
	FILE *fp;

	fp = fopen("/proc/interrupts", "r");
	TEST_ASSERT(fp, "fopen(/proc/interrupts) failed");

	return fp;
}

unsigned int vfio_msix_to_host_irq(const char *device_bdf, int msix)
{
	char search_string[64];
	char line[4096];
	int irq = -1;
	FILE *fp;

	fp = open_proc_interrupts();

	snprintf(search_string, sizeof(search_string), "vfio-msix[%d]", msix);

	while (fgets(line, sizeof(line), fp)) {
		if (strstr(line, device_bdf) && strstr(line, search_string)) {
			TEST_ASSERT_EQ(1, sscanf(line, "%d:", &irq));
			break;
		}
	}

	fclose(fp);

	TEST_ASSERT(irq != -1, "Failed to locate IRQ for %s %s", device_bdf,
		    search_string);
	return (unsigned int)irq;
}


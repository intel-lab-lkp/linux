// SPDX-License-Identifier: GPL-2.0
/* Copyright (c) 2026 Tao Cui */
#include <test_progs.h>
#include "iocost_model.skel.h"

/*
 * Switch a device to ctrl=bpf through io.cost.model and verify the
 * readback, restoring ctrl=auto afterwards.  Returns 0 on success.
 * Shared by the example-model tests below.
 */
static int switch_to_bpf(const char *dev)
{
	char path[] = "/sys/fs/cgroup/io.cost.model";
	char word[256], line[256], buf[300];
	FILE *fp;
	int err, found = 0;

	fp = fopen(path, "r+");
	if (!fp) {
		TH_LOG("open %s: %s", path, strerror(errno));
		return -1;
	}

	while (fgets(line, sizeof(line), fp)) {
		if (sscanf(line, "%255s", word) == 1 && !strcmp(word, dev)) {
			found = 1;
			break;
		}
	}
	if (!found) {
		TH_LOG("%s has no iocost line", dev);
		fclose(fp);
		return -1;
	}

	snprintf(buf, sizeof(buf), "%s ctrl=bpf", dev);
	err = fprintf(fp, "%s\n", buf);
	if (err <= 0) {
		TH_LOG("write ctrl=bpf failed");
		fclose(fp);
		return -1;
	}
	fflush(fp);

	rewind(fp);
	found = 0;
	while (fgets(line, sizeof(line), fp)) {
		if (sscanf(line, "%255s", word) == 1 && !strcmp(word, dev)) {
			found = strstr(line, "ctrl=bpf") != NULL;
			break;
		}
	}
	if (!found)
		TH_LOG("readback does not show ctrl=bpf");

	fprintf(fp, "%s ctrl=auto\n", dev);
	fclose(fp);
	return found ? 0 : -1;
}

static int dev_has_iocost(void)
{
	FILE *fp = fopen("/sys/fs/cgroup/io.cost.qos", "r");

	if (fp)
		fclose(fp);
	return fp != NULL;
}

/*
 * Register the example cost model and switch a device to ctrl=bpf.
 * IO accounting itself is not checked here; it needs a device doing
 * real IO under iocost and is covered by the kernel-side validation
 * described in the cover letter.
 *
 * Requires root, cgroup v2 and a device with iocost support.  The
 * device must be given as major:minor in $IOCOST_TEST_DEV, otherwise
 * the test is skipped.
 */
void serial_test_iocost_model(void)
{
	struct iocost_model *skel;
	char word[256], *dev;
	int err;

	dev = getenv("IOCOST_TEST_DEV");
	if (!dev || geteuid() != 0) {
		test__skip();
		return;
	}
	if (!ASSERT_TRUE(dev_has_iocost(), "iocost_mounted"))
		return;

	/* negative: ctrl=bpf must be rejected while no model is
	 * registered, so a typo cannot silently disable cost model
	 * updates */
	if (sscanf(dev, "%255s", word) != 1)
		return;
	{
		char path[] = "/sys/fs/cgroup/io.cost.model";
		char buf[300];
		FILE *fp = fopen(path, "w");

		snprintf(buf, sizeof(buf), "%s ctrl=bpf\n", word);
		err = 0;
		if (fp) {
			err = fprintf(fp, "%s", buf) <= 0;
			fclose(fp);
		}
		ASSERT_TRUE(err, "ctrl_bpf_without_model_rejected");
	}

	skel = iocost_model__open_and_load();
	if (!ASSERT_OK_PTR(skel, "skel_open_load"))
		return;

	/* attaching the struct_ops registers the model; a second
	 * registration of the same ops would fail with EBUSY */
	err = iocost_model__attach(skel);
	if (ASSERT_OK(err, "attach"))
		ASSERT_OK(switch_to_bpf(dev), "switch_and_readback");

	iocost_model__destroy(skel);
}

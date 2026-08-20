// SPDX-License-Identifier: GPL-2.0
#include <linux/kernel.h>
#include <linux/string.h>
#include "tlob_kunit.h"

#if IS_REACHABLE(CONFIG_RV_MON_TLOB)

/* Valid "p PATH:START STOP threshold=NS" lines. */
static const char * const tlob_parse_valid[] = {
	"p /usr/bin/myapp:4768 4848 threshold=5000000",
	"p /usr/bin/myapp:0x12a0 0x12f0 threshold=10000000",
	"p /opt/my:app/bin:0x100 0x200 threshold=1000000",
};

/* Malformed "p ..." lines that must be rejected with -EINVAL. */
static const char * const tlob_parse_invalid[] = {
	"p :0x100 0x200 threshold=5000",
	"p /usr/bin/myapp:0x100 threshold=5000",
	"p /usr/bin/myapp:-1 0x200 threshold=5000",
	"p /usr/bin/myapp:0x100 -1 threshold=5000000",	/* negative stop offset */
	"p /usr/bin/myapp:0x100 0x200",
	"p /usr/bin/myapp:0x100 0x100 threshold=5000",
};

/* threshold_ns out of valid range => -ERANGE. */
static const char * const tlob_parse_out_of_range[] = {
	"p /usr/bin/myapp:0x100 0x200 threshold=0",
	"p /usr/bin/myapp:0x100 0x200 threshold=999",
	"p /usr/bin/myapp:0x100 0x200 threshold=3600000000001",
};

/* Valid "-PATH:OFFSET_START" remove lines. */
static const char * const tlob_remove_valid[] = {
	"-/usr/bin/myapp:0x100",
	"-/opt/my:app/bin:0x200",
};

/* Malformed remove lines that must be rejected with -EINVAL. */
static const char * const tlob_remove_invalid[] = {
	"-usr/bin/myapp:0x100",
	"-/usr/bin/myapp",
	"-/:0x100",
	"-/usr/bin/myapp:-1",	/* negative offset */
	"-/usr/bin/myapp:abc",
};

static void rv_test_tlob(struct kunit *test)
{
	u64 thr;
	char *path;
	loff_t start, stop;
	char buf[128];
	int i;

	for (i = 0; i < ARRAY_SIZE(tlob_parse_valid); i++) {
		strscpy(buf, tlob_parse_valid[i], sizeof(buf));
		KUNIT_EXPECT_EQ(test, rv_tlob_kunit_ops.parse_uprobe_line(buf, &thr, &path,
									  &start, &stop), 0);
	}

	for (i = 0; i < ARRAY_SIZE(tlob_parse_invalid); i++) {
		strscpy(buf, tlob_parse_invalid[i], sizeof(buf));
		KUNIT_EXPECT_EQ(test, rv_tlob_kunit_ops.parse_uprobe_line(buf, &thr, &path,
									  &start, &stop), -EINVAL);
	}

	for (i = 0; i < ARRAY_SIZE(tlob_parse_out_of_range); i++) {
		strscpy(buf, tlob_parse_out_of_range[i], sizeof(buf));
		KUNIT_EXPECT_EQ(test, rv_tlob_kunit_ops.parse_uprobe_line(buf, &thr, &path,
									  &start, &stop), -ERANGE);
	}

	for (i = 0; i < ARRAY_SIZE(tlob_remove_valid); i++) {
		strscpy(buf, tlob_remove_valid[i], sizeof(buf));
		KUNIT_EXPECT_EQ(test, rv_tlob_kunit_ops.parse_remove_line(buf, &path, &start), 0);
	}

	for (i = 0; i < ARRAY_SIZE(tlob_remove_invalid); i++) {
		strscpy(buf, tlob_remove_invalid[i], sizeof(buf));
		KUNIT_EXPECT_EQ(test, rv_tlob_kunit_ops.parse_remove_line(buf, &path, &start),
				-EINVAL);
	}
}

#else
#define rv_test_tlob rv_test_stub
#endif

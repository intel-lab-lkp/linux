// SPDX-License-Identifier: GPL-2.0
#include <linux/kernel.h>
#include <linux/string.h>
#include "tlob_kunit.h"

#if IS_REACHABLE(CONFIG_RV_MON_TLOB)

/* Valid "p:NAME PATH:START STOP threshold=NS" lines and what they decode to. */
static const struct {
	const char	*line;
	const char	*name;
	const char	*path;
	loff_t		start;
	loff_t		stop;
	u64		thr;
} tlob_parse_valid[] = {
	{ "p:region /usr/bin/myapp:4768 4848 threshold=5000000",
	  "region", "/usr/bin/myapp", 4768, 4848, 5000000 },
	/* Base 0: hex offsets must not decode as decimal. */
	{ "p:my_region_2 /usr/bin/myapp:0x12a0 0x12f0 threshold=10000000",
	  "my_region_2", "/usr/bin/myapp", 0x12a0, 0x12f0, 10000000 },
	/* The last ':' separates the offset, so a ':' in PATH is fine. */
	{ "p:_r3 /opt/my:app/bin:0x100 0x200 threshold=1000000",
	  "_r3", "/opt/my:app/bin", 0x100, 0x200, 1000000 },
	/* TLOB_MAX_NAME - 1 characters: the longest name accepted. */
	{ "p:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa /bin/a:0x100 0x200 threshold=5000",
	  "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", "/bin/a", 0x100, 0x200, 5000 },
	/* The accepted threshold bounds themselves. */
	{ "p:lo /bin/a:0x100 0x200 threshold=1000",
	  "lo", "/bin/a", 0x100, 0x200, 1000 },
	{ "p:hi /bin/a:0x100 0x200 threshold=3600000000000",
	  "hi", "/bin/a", 0x100, 0x200, 3600000000000ULL },
};

/* Malformed "p:..." lines that must be rejected with -EINVAL. */
static const char * const tlob_parse_invalid[] = {
	"p /usr/bin/myapp:0x100 0x200 threshold=5000",		/* no name */
	"p:",							/* empty name */
	"p: /usr/bin/myapp:0x100 0x200 threshold=5000",		/* name is a path */
	"p:r /bin/a:0x100junk 0x200 threshold=5000",		/* trailing junk */
	"p:1region /usr/bin/myapp:0x100 0x200 threshold=5000",	/* digit lead */
	"p:my-win /usr/bin/myapp:0x100 0x200 threshold=5000",	/* bad char */
	"p:my.win /usr/bin/myapp:0x100 0x200 threshold=5000",	/* bad char */
	/* TLOB_MAX_NAME characters: one over the limit. */
	"p:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa /bin/a:0x100 0x200 threshold=5000",
	"p:region :0x100 0x200 threshold=5000",			/* empty path */
	"p:region /usr/bin/myapp:0x100 threshold=5000",		/* missing stop */
	"p:region /usr/bin/myapp:-1 0x200 threshold=5000",		/* negative start */
	"p:region /usr/bin/myapp:0x100 -1 threshold=5000000",	/* negative stop */
	"p:region /usr/bin/myapp:0x100 0x200",			/* missing threshold */
	"p:region /usr/bin/myapp:0x100 0x100 threshold=5000",	/* start == stop */
	/* Duplicate threshold=, like a duplicate offset, is an error. */
	"p:region /bin/a:0x100 0x200 threshold=5000 threshold=9000",
};

/* threshold_ns out of valid range => -ERANGE. */
static const char * const tlob_parse_out_of_range[] = {
	"p:region /usr/bin/myapp:0x100 0x200 threshold=0",
	"p:region /usr/bin/myapp:0x100 0x200 threshold=999",
	"p:region /usr/bin/myapp:0x100 0x200 threshold=3600000000001",
};

/* Valid "-:NAME" remove lines. */
static const char * const tlob_remove_valid[] = {
	"-:region",
	"-:my_region",
};

/* Malformed remove lines that must be rejected with -EINVAL. */
static const char * const tlob_remove_invalid[] = {
	"-",	/* no name */
	"-:",	/* empty name */
	"-region",	/* missing ':' */
	"-:my region",	/* bad char */
	"-:1region",	/* digit lead */
	"-:my-region",	/* bad char */
	"-:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",	/* TLOB_MAX_NAME, one too many */
};

static void rv_test_tlob(struct kunit *test)
{
	u64 thr;
	char *name, *path;
	loff_t start, stop;
	char buf[128];
	int i, ret;

	/* NAME and PATH come back NUL-terminated inside buf. */
	strscpy(buf, "p:region /usr/bin/myapp:0x100 0x200 threshold=5000",
		sizeof(buf));
	ret = rv_tlob_kunit_ops.parse_uprobe_line(buf, &name, &path,
						  &start, &stop, &thr);
	KUNIT_EXPECT_EQ(test, ret, 0);
	KUNIT_EXPECT_STREQ(test, name, "region");
	KUNIT_EXPECT_STREQ(test, path, "/usr/bin/myapp");

	strscpy(buf, "-:region", sizeof(buf));
	ret = rv_tlob_kunit_ops.parse_remove_line(buf, &name);
	KUNIT_EXPECT_EQ(test, ret, 0);
	KUNIT_EXPECT_STREQ(test, name, "region");

	for (i = 0; i < ARRAY_SIZE(tlob_parse_valid); i++) {
		strscpy(buf, tlob_parse_valid[i].line, sizeof(buf));
		ret = rv_tlob_kunit_ops.parse_uprobe_line(buf, &name, &path,
							  &start, &stop,
							  &thr);
		KUNIT_EXPECT_EQ(test, ret, 0);
		if (ret)
			continue;
		KUNIT_EXPECT_STREQ(test, name, tlob_parse_valid[i].name);
		KUNIT_EXPECT_STREQ(test, path, tlob_parse_valid[i].path);
		KUNIT_EXPECT_EQ(test, start, tlob_parse_valid[i].start);
		KUNIT_EXPECT_EQ(test, stop, tlob_parse_valid[i].stop);
		KUNIT_EXPECT_EQ(test, thr, tlob_parse_valid[i].thr);
	}

	for (i = 0; i < ARRAY_SIZE(tlob_parse_invalid); i++) {
		strscpy(buf, tlob_parse_invalid[i], sizeof(buf));
		ret = rv_tlob_kunit_ops.parse_uprobe_line(buf, &name, &path,
							  &start, &stop,
							  &thr);
		KUNIT_EXPECT_EQ(test, ret, -EINVAL);
	}

	for (i = 0; i < ARRAY_SIZE(tlob_parse_out_of_range); i++) {
		strscpy(buf, tlob_parse_out_of_range[i], sizeof(buf));
		ret = rv_tlob_kunit_ops.parse_uprobe_line(buf, &name, &path,
							  &start, &stop,
							  &thr);
		KUNIT_EXPECT_EQ(test, ret, -ERANGE);
	}

	for (i = 0; i < ARRAY_SIZE(tlob_remove_valid); i++) {
		strscpy(buf, tlob_remove_valid[i], sizeof(buf));
		ret = rv_tlob_kunit_ops.parse_remove_line(buf, &name);
		KUNIT_EXPECT_EQ(test, ret, 0);
		if (!ret)
			KUNIT_EXPECT_STREQ(test, name,
					   tlob_remove_valid[i] + 2);
	}

	for (i = 0; i < ARRAY_SIZE(tlob_remove_invalid); i++) {
		strscpy(buf, tlob_remove_invalid[i], sizeof(buf));
		ret = rv_tlob_kunit_ops.parse_remove_line(buf, &name);
		KUNIT_EXPECT_EQ(test, ret, -EINVAL);
	}
}

#else
#define rv_test_tlob rv_test_stub
#endif

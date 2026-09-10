/* SPDX-License-Identifier: GPL-2.0 */

#include <stdio.h>
#include <stdbool.h>

struct enum_kv {
	const char *name;
	const long long val;
	const bool is_undef:1;
};

struct enum_entry {
	const struct enum_kv ynl;
	const struct enum_kv c;
	const bool is_sentinal:1;
};

#define YNL_ENUM_ENTRY(YNL_NAME, C_NAME, YNL_VALUE) \
	{ \
		.ynl = { .name = YNL_NAME, .val = YNL_VALUE }, \
		.c = { .name = #C_NAME, .val = C_NAME }, \
	}

#define YNL_ENUM_BAD_ENTRY(YNL_NAME, C_NAME, YNL_VALUE) \
	{ \
		.ynl = { .name = YNL_NAME, .val = YNL_VALUE }, \
		.c = { .name = #C_NAME, .is_undef = true }, \
	}

#define YNL_ENUM_SENTINAL(C_NAME, YNL_VALUE) \
	{ \
		.ynl = { .name = "MAX", .val = YNL_VALUE }, \
		.c = { .name = #C_NAME, .val = C_NAME }, \
		.is_sentinal = true, \
	}

enum ynl_enum_type {
	YNL_ENUM,
	YNL_FLAGS,
};

struct enum_set {
	const char *name;
	const struct enum_entry *entry;
	enum ynl_enum_type type;
};

struct linter_ctx {
	int errors;
	int warnings;
	const char *name;
};

#define errf(fmt, ...) \
	do { \
		fprintf(stderr, "%s: ERROR: " fmt, ctx->name, __VA_ARGS__); \
		ctx->errors++; \
	} while (0)

#define warnf(fmt, ...) \
	do { \
		fprintf(stderr, "%s: WARN: " fmt, ctx->name, __VA_ARGS__); \
		ctx->warnings++; \
	} while (0)

static inline long long find_next_value(const struct enum_set *es,
					const long long last_val)
{
	switch (es->type) {
	case YNL_ENUM:
		return last_val + 1;
	case YNL_FLAGS:
		return last_val << 1;
	default:
		/* unreachable */
		abort();
	}
}

static inline void lint_enum_entry(struct linter_ctx *ctx,
				   const struct enum_set *es,
				   const struct enum_entry *entry,
				   long long *last_val, const int i,
				   const int cnt)
{
	const long long val = entry->c.val;

	if (i > 0 && val != *last_val && val != find_next_value(es, *last_val))
		warnf("%s: Possible missing member before %s (%lld -> %lld)\n",
		      es->name, entry->c.name, *last_val, val);
	*last_val = entry->c.val;

	if (i == cnt - 1 && strcmp(entry->ynl.name, "max") == 0)
		errf("%s: Sentinal used in YNL spec\n", es->name);

	if (entry->c.is_undef) {
		errf("%s: %s: %s not found\n", es->name, entry->ynl.name,
		     entry->c.name);
		return;
	}

	if (entry->ynl.val == entry->c.val)
		return;

	if (entry->is_sentinal) {
		if (es->type == YNL_ENUM && entry->ynl.val + 1 == entry->c.val)
			return; /* eg. DEVCONF_MAX is the storage size */
		warnf("%s: Sentinal mismatch: %lld != %lld (Last YNL != %s)\n",
		      es->name, entry->ynl.val, entry->c.val, entry->c.name);
	} else {
		errf("%s: Value mismatch: %lld != %lld (%s != %s)\n", es->name,
		     entry->ynl.val, entry->c.val, entry->ynl.name,
		     entry->c.name);
	}
}

static inline void lint_enum(struct linter_ctx *ctx, const struct enum_set *es)
{
	const struct enum_entry *entry = es->entry;
	long long last_val;
	int cnt = 0;
	int i = 0;

	while (entry->ynl.name) {
		if (!entry->is_sentinal)
			cnt++;
		entry++;
	}
	entry = es->entry;

	while (entry->ynl.name)
		lint_enum_entry(ctx, es, entry++, &last_val, i++, cnt);
}

static inline int linter_run(const int argc, const char **argv,
			     const struct enum_set *es)
{
	struct linter_ctx ctx = {
		.name = argv[argc > 1 ? 1 : 0],
	};

	while (es->name) {
		lint_enum(&ctx, es);
		es++;
	}

	if (ctx.errors || ctx.warnings)
		fprintf(stderr, "%s: Linter summary: %d errors, %d warnings\n",
			ctx.name, ctx.errors, ctx.warnings);

	return EXIT_SUCCESS;
}

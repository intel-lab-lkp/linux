// SPDX-License-Identifier: GPL-2.0
/*
 * KUnit tests for the E820 memory map code.
 *
 * e820__update_table() and e820__range_update_table() are __init functions
 * that work on a table passed by the caller. The tests therefore run during
 * boot, before the init sections are freed, and never touch the real map.
 *
 * The layouts are written in units of one 4K page.
 */
#include <kunit/test.h>
#include <linux/init.h>
#include <linux/limits.h>
#include <linux/sizes.h>
#include <linux/string.h>

#include <asm/e820/api.h>
#include <asm/e820/types.h>

#define PG(n)        ((u64)(n) * SZ_4K)
#define E(a, s, t)   { .addr = PG(a), .size = PG(s), .type = E820_TYPE_##t }
#define CHECK_UPDATE(test, in, exp) \
	check_update(test, in, ARRAY_SIZE(in), exp, ARRAY_SIZE(exp))

static struct e820_table *__init new_table(struct kunit *test, const struct e820_entry *e, u32 n)
{
	struct e820_table *t = kunit_kzalloc(test, sizeof(*t), GFP_KERNEL);

	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, t);
	KUNIT_ASSERT_LE(test, n, (u32)E820_MAX_ENTRIES);
	if (n)
		memcpy(t->entries, e, n * sizeof(*e));
	t->nr_entries = n;
	return t;
}

static void __init expect_table(struct kunit *test, const struct e820_table *t,
		const struct e820_entry *e, u32 n)
{
	u32 i;

	KUNIT_ASSERT_EQ(test, t->nr_entries, n);
	for (i = 0; i < n; i++) {
		KUNIT_EXPECT_EQ_MSG(test, t->entries[i].addr, e[i].addr, "entry %u addr", i);
		KUNIT_EXPECT_EQ_MSG(test, t->entries[i].size, e[i].size, "entry %u size", i);
		KUNIT_EXPECT_EQ_MSG(test, (u32)t->entries[i].type, (u32)e[i].type,
				"entry %u type", i);
	}
}

static void __init check_update(struct kunit *test,
		const struct e820_entry *in, u32 n_in,
		const struct e820_entry *exp, u32 n_exp)
{
	struct e820_table *t = new_table(test, in, n_in);

	KUNIT_EXPECT_EQ(test, e820__update_table(t), 0);
	expect_table(test, t, exp, n_exp);
}

static void __init check_range_update(struct kunit *test,
		const struct e820_entry *in, u32 n_in, u64 start, u64 size,
		enum e820_type old_type, enum e820_type new_type, u64 ret,
		const struct e820_entry *exp, u32 n_exp)
{
	struct e820_table *t = new_table(test, in, n_in);

	KUNIT_EXPECT_EQ(test, e820__range_update_table(t, start, size, old_type, new_type), ret);
	expect_table(test, t, exp, n_exp);
}

/* One entry, or none: nothing to sanitize. The table must stay as it was. */
static void __init update_table_too_few(struct kunit *test)
{
	static const struct e820_entry in[] = { E(0, 4, RAM) };
	struct e820_table *t = new_table(test, in, ARRAY_SIZE(in));

	KUNIT_EXPECT_EQ(test, e820__update_table(t), -1);
	expect_table(test, t, in, ARRAY_SIZE(in));

	t = new_table(test, NULL, 0);
	KUNIT_EXPECT_EQ(test, e820__update_table(t), -1);
	KUNIT_EXPECT_EQ(test, t->nr_entries, 0);
}

/* An entry that wraps around through zero makes the whole table invalid. */
static void __init update_table_overflow(struct kunit *test)
{
	static const struct e820_entry in[] = {
		E(0, 1, RAM),
		{ .addr = ULLONG_MAX - 0xfff, .size = SZ_8K, .type = E820_TYPE_RESERVED },
	};
	struct e820_table *t = new_table(test, in, ARRAY_SIZE(in));

	KUNIT_EXPECT_EQ(test, e820__update_table(t), -1);
	expect_table(test, t, in, ARRAY_SIZE(in));
}

static void __init update_table_unsorted_disjoint(struct kunit *test)
{
	static const struct e820_entry in[] = { E(4, 1, ACPI), E(0, 1, RESERVED), E(2, 1, RAM) };
	static const struct e820_entry exp[] = { E(0, 1, RESERVED), E(2, 1, RAM), E(4, 1, ACPI) };

	CHECK_UPDATE(test, in, exp);
}

/* A higher type inside a lower one splits it in three. */
static void __init update_table_nested_higher_type(struct kunit *test)
{
	static const struct e820_entry in[] = { E(0, 4, RAM), E(1, 1, RESERVED) };
	static const struct e820_entry exp[] = { E(0, 1, RAM), E(1, 1, RESERVED), E(2, 2, RAM) };

	CHECK_UPDATE(test, in, exp);
}

static void __init update_table_partial_overlap(struct kunit *test)
{
	static const struct e820_entry in[] = { E(0, 2, RAM), E(1, 2, ACPI) };
	static const struct e820_entry exp[] = { E(0, 1, RAM), E(1, 2, ACPI) };

	CHECK_UPDATE(test, in, exp);
}

/* A lower type inside a higher one disappears. */
static void __init update_table_lower_type_absorbed(struct kunit *test)
{
	static const struct e820_entry in[] = { E(0, 4, ACPI), E(1, 1, RAM) };
	static const struct e820_entry exp[] = { E(0, 4, ACPI) };

	CHECK_UPDATE(test, in, exp);
}

static void __init update_table_merges_same_type(struct kunit *test)
{
	static const struct e820_entry in[] = { E(0, 1, RAM), E(1, 1, RAM), E(2, 1, RAM) };
	static const struct e820_entry exp[] = { E(0, 3, RAM) };

	CHECK_UPDATE(test, in, exp);
}

static void __init update_table_duplicates(struct kunit *test)
{
	static const struct e820_entry in[] = { E(0, 2, RAM), E(0, 2, RAM) };
	static const struct e820_entry exp[] = { E(0, 2, RAM) };

	CHECK_UPDATE(test, in, exp);
}

/* Adjacent entries of different types stay separate, in any input order. */
static void __init update_table_adjacent_types(struct kunit *test)
{
	static const struct e820_entry a[] = { E(0, 1, RAM), E(1, 1, RESERVED) };
	static const struct e820_entry b[] = { E(1, 1, RESERVED), E(0, 1, RAM) };
	static const struct e820_entry c[] = { E(0, 1, RESERVED), E(1, 1, RAM) };

	CHECK_UPDATE(test, a, a);
	CHECK_UPDATE(test, b, a);
	CHECK_UPDATE(test, c, c);
}

/* These types describe distinct ranges, so adjacent ones are not merged. */
static void __init update_table_no_merge_pram_soft_reserved(struct kunit *test)
{
	static const struct e820_entry pram[] = { E(0, 1, PRAM), E(1, 1, PRAM) };
	static const struct e820_entry soft[] = { E(0, 1, SOFT_RESERVED), E(1, 1, SOFT_RESERVED) };

	CHECK_UPDATE(test, pram, pram);
	CHECK_UPDATE(test, soft, soft);
}

static void __init update_table_drops_empty_entries(struct kunit *test)
{
	static const struct e820_entry in[] = { E(0, 1, RAM), E(5, 0, RAM), E(2, 1, RESERVED) };
	static const struct e820_entry exp[] = { E(0, 1, RAM), E(2, 1, RESERVED) };

	CHECK_UPDATE(test, in, exp);
}

/* Converts rows like "__22__" to entries: column * 4K, run length * 4K, digit as type. */
static void __init art_to_entries(const char * const *rows, u32 nrows, struct e820_entry *out)
{
	u32 i, start, end;

	for (i = 0; i < nrows; i++) {
		start = 0;
		while (rows[i][start] == '_')
			start++;
		end = start;
		while (rows[i][end] && rows[i][end] == rows[i][start])
			end++;
		out[i].addr = (u64)start * SZ_4K;
		out[i].size = (u64)(end - start) * SZ_4K;
		out[i].type = rows[i][start] - '0';
	}
}

/* The example from the comment above e820__update_table(). */
static void __init update_table_documented_example(struct kunit *test)
{
	static const char * const in_rows[] = {
		"____22__________________",
		"______________________4_",
		"____1111________________",
		"_44_____________________",
		"11111111________________",
		"____________________33__",
		"___________44___________",
		"__________33333_________",
		"______________22________",
		"___________________2222_",
		"_________111111111______",
		"_____________________11_",
		"_________________4______",
	};
	static const char * const out_rows[] = {
		"1_______________________",
		"_44_____________________",
		"___1____________________",
		"____22__________________",
		"______11________________",
		"_________1______________",
		"__________3_____________",
		"___________44___________",
		"_____________33_________",
		"_______________2________",
		"________________1_______",
		"_________________4______",
		"___________________2____",
		"____________________33__",
		"______________________4_",
	};
	struct e820_entry in[ARRAY_SIZE(in_rows)], exp[ARRAY_SIZE(out_rows)];

	art_to_entries(in_rows, ARRAY_SIZE(in_rows), in);
	art_to_entries(out_rows, ARRAY_SIZE(out_rows), exp);
	CHECK_UPDATE(test, in, exp);
}

/*
 * If the result does not fit, the table is cut at its capacity. This
 * documents the current behaviour.
 */
static void __init update_table_full_table_truncates(struct kunit *test)
{
	struct e820_table *t = kunit_kzalloc(test, sizeof(*t), GFP_KERNEL);
	u32 i;

	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, t);

	t->entries[0] = (struct e820_entry)E(0, 2 * E820_MAX_ENTRIES, RAM);
	for (i = 1; i < E820_MAX_ENTRIES; i++) {
		t->entries[i].addr = PG(2 * i - 1);
		t->entries[i].size = PG(1);
		t->entries[i].type = E820_TYPE_RESERVED;
	}
	t->nr_entries = E820_MAX_ENTRIES;

	KUNIT_EXPECT_EQ(test, e820__update_table(t), 0);
	KUNIT_ASSERT_EQ(test, t->nr_entries, (u32)E820_MAX_ENTRIES);
	for (i = 0; i < E820_MAX_ENTRIES; i++) {
		KUNIT_EXPECT_EQ_MSG(test, t->entries[i].addr, PG(i), "entry %u addr", i);
		KUNIT_EXPECT_EQ_MSG(test, t->entries[i].size, PG(1), "entry %u size", i);
		KUNIT_EXPECT_EQ_MSG(test, (u32)t->entries[i].type,
				(u32)(i % 2 ? E820_TYPE_RESERVED : E820_TYPE_RAM),
				"entry %u type", i);
	}
}

static void __init range_update_covered(struct kunit *test)
{
	static const struct e820_entry in[] = { E(1, 4, RAM) };
	static const struct e820_entry exp[] = { E(1, 4, RESERVED) };

	check_range_update(test, in, ARRAY_SIZE(in), PG(0), PG(16),
			E820_TYPE_RAM, E820_TYPE_RESERVED, PG(4), exp, ARRAY_SIZE(exp));
}

/* The new range is inside the entry: it is split, new pieces go to the end. */
static void __init range_update_split(struct kunit *test)
{
	static const struct e820_entry in[] = { E(1, 4, RAM) };
	static const struct e820_entry exp[] = { E(1, 1, RAM), E(2, 1, RESERVED), E(3, 2, RAM) };

	check_range_update(test, in, ARRAY_SIZE(in), PG(2), PG(1),
			E820_TYPE_RAM, E820_TYPE_RESERVED, PG(1), exp, ARRAY_SIZE(exp));
}

static void __init range_update_head(struct kunit *test)
{
	static const struct e820_entry in[] = { E(1, 4, RAM) };
	static const struct e820_entry exp[] = { E(3, 2, RAM), E(1, 2, RESERVED) };

	check_range_update(test, in, ARRAY_SIZE(in), PG(0), PG(3),
			E820_TYPE_RAM, E820_TYPE_RESERVED, PG(2), exp, ARRAY_SIZE(exp));
}

static void __init range_update_tail(struct kunit *test)
{
	static const struct e820_entry in[] = { E(1, 4, RAM) };
	static const struct e820_entry exp[] = { E(1, 3, RAM), E(4, 1, RESERVED) };

	check_range_update(test, in, ARRAY_SIZE(in), PG(4), PG(4),
			E820_TYPE_RAM, E820_TYPE_RESERVED, PG(1), exp, ARRAY_SIZE(exp));
}

/* No overlap, or a different old type: nothing changes. */
static void __init range_update_no_match(struct kunit *test)
{
	static const struct e820_entry in[] = { E(1, 4, RAM) };

	check_range_update(test, in, ARRAY_SIZE(in), PG(8), PG(1),
			E820_TYPE_RAM, E820_TYPE_RESERVED, 0, in, ARRAY_SIZE(in));
	check_range_update(test, in, ARRAY_SIZE(in), PG(0), PG(16),
			E820_TYPE_ACPI, E820_TYPE_RESERVED, 0, in, ARRAY_SIZE(in));
}

/* A size that would wrap around is cut at the end of the address space. */
static void __init range_update_clamps_size(struct kunit *test)
{
	static const struct e820_entry in[] = {
		{ .addr = ULLONG_MAX - 0xffff, .size = 0xffff, .type = E820_TYPE_RAM },
	};
	static const struct e820_entry exp[] = {
		{ .addr = ULLONG_MAX - 0xffff, .size = 0xffff, .type = E820_TYPE_RESERVED },
	};

	check_range_update(test, in, ARRAY_SIZE(in), ULLONG_MAX - 0xffff, ULLONG_MAX,
			E820_TYPE_RAM, E820_TYPE_RESERVED, 0xffff, exp, ARRAY_SIZE(exp));
}

static void __init range_update_several_entries(struct kunit *test)
{
	static const struct e820_entry in[] = { E(0, 1, RAM), E(2, 1, RAM), E(4, 1, ACPI) };
	static const struct e820_entry exp[] = { E(0, 1, NVS), E(2, 1, NVS), E(4, 1, ACPI) };

	check_range_update(test, in, ARRAY_SIZE(in), PG(0), PG(8),
			E820_TYPE_RAM, E820_TYPE_NVS, PG(2), exp, ARRAY_SIZE(exp));
}

static struct kunit_case __refdata e820_test_cases[] = {
	KUNIT_CASE(update_table_too_few),
	KUNIT_CASE(update_table_overflow),
	KUNIT_CASE(update_table_unsorted_disjoint),
	KUNIT_CASE(update_table_nested_higher_type),
	KUNIT_CASE(update_table_partial_overlap),
	KUNIT_CASE(update_table_lower_type_absorbed),
	KUNIT_CASE(update_table_merges_same_type),
	KUNIT_CASE(update_table_duplicates),
	KUNIT_CASE(update_table_adjacent_types),
	KUNIT_CASE(update_table_no_merge_pram_soft_reserved),
	KUNIT_CASE(update_table_drops_empty_entries),
	KUNIT_CASE(update_table_documented_example),
	KUNIT_CASE(update_table_full_table_truncates),
	KUNIT_CASE(range_update_covered),
	KUNIT_CASE(range_update_split),
	KUNIT_CASE(range_update_head),
	KUNIT_CASE(range_update_tail),
	KUNIT_CASE(range_update_no_match),
	KUNIT_CASE(range_update_clamps_size),
	KUNIT_CASE(range_update_several_entries),
	{}
};

static struct kunit_suite e820_test_suite = {
	.name = "e820",
	.test_cases = e820_test_cases,
};
kunit_test_init_section_suite(e820_test_suite);

// SPDX-License-Identifier: GPL-2.0
#include <linux/kernel.h>
#include <linux/types.h>
#include <linux/init.h>
#include <linux/memblock.h>
#include <linux/seq_file.h>
#include <linux/bitops.h>

static bool early_memtest_done;
static phys_addr_t early_memtest_bad_size;

static u64 addr_bus_failed_bits;
static u64 addr_bus_skipped_bits;
static bool addr_bus_test_done;

static u64 patterns[] __initdata = {
	/* The first entry has to be 0 to leave memtest with zeroed memory */
	0,
	0xffffffffffffffffULL,
	0x5555555555555555ULL,
	0xaaaaaaaaaaaaaaaaULL,
	0x1111111111111111ULL,
	0x2222222222222222ULL,
	0x4444444444444444ULL,
	0x8888888888888888ULL,
	0x3333333333333333ULL,
	0x6666666666666666ULL,
	0x9999999999999999ULL,
	0xccccccccccccccccULL,
	0x7777777777777777ULL,
	0xbbbbbbbbbbbbbbbbULL,
	0xddddddddddddddddULL,
	0xeeeeeeeeeeeeeeeeULL,
	0x7a6c7258554e494cULL, /* yeah ;-) */
};

static void __init reserve_bad_mem(u64 pattern, phys_addr_t start_bad, phys_addr_t end_bad)
{
	pr_info("  %016llx bad mem addr %pa - %pa reserved\n",
		cpu_to_be64(pattern), &start_bad, &end_bad);
	memblock_reserve(start_bad, end_bad - start_bad);
	early_memtest_bad_size += (end_bad - start_bad);
}

static void __init memtest(u64 pattern, phys_addr_t start_phys, phys_addr_t size)
{
	u64 *p, *start, *end;
	phys_addr_t start_bad, last_bad;
	phys_addr_t start_phys_aligned;
	const size_t incr = sizeof(pattern);

	start_phys_aligned = ALIGN(start_phys, incr);
	start = __va(start_phys_aligned);
	end = start + (size - (start_phys_aligned - start_phys)) / incr;
	start_bad = 0;
	last_bad = 0;

	VM_WARN_ON_ONCE(size < start_phys_aligned - start_phys);

	for (p = start; p < end; p++)
		WRITE_ONCE(*p, pattern);

	for (p = start; p < end; p++, start_phys_aligned += incr) {
		if (READ_ONCE(*p) == pattern)
			continue;
		if (start_phys_aligned == last_bad + incr) {
			last_bad += incr;
			continue;
		}
		if (start_bad)
			reserve_bad_mem(pattern, start_bad, last_bad + incr);
		start_bad = last_bad = start_phys_aligned;
	}
	if (start_bad)
		reserve_bad_mem(pattern, start_bad, last_bad + incr);

	early_memtest_done = true;
}

static void __init do_one_pass(u64 pattern, phys_addr_t start, phys_addr_t end)
{
	u64 i;
	phys_addr_t this_start, this_end;

	for_each_free_mem_range(i, NUMA_NO_NODE, MEMBLOCK_NONE, &this_start,
				&this_end, NULL) {
		this_start = clamp(this_start, start, end);
		this_end = clamp(this_end, start, end);
		if (this_start < this_end) {
			pr_info("  %pa - %pa pattern %016llx\n",
				&this_start, &this_end, cpu_to_be64(pattern));
			memtest(pattern, this_start, this_end - this_start);
		}
	}
}

/* default is disabled */
static unsigned int memtest_pattern __initdata;

static int __init parse_memtest(char *arg)
{
	int ret = 0;

	if (arg)
		ret = kstrtouint(arg, 0, &memtest_pattern);
	else
		memtest_pattern = ARRAY_SIZE(patterns);

	return ret;
}

early_param("memtest", parse_memtest);

static bool __init is_address_free(phys_addr_t addr)
{
	return memblock_is_memory(addr) && !memblock_is_reserved(addr);
}

static bool __init find_test_pair(unsigned int bit, phys_addr_t *addr1, phys_addr_t *addr2)
{
	u64 i;
	phys_addr_t this_start, this_end;
	phys_addr_t candidate;
	const phys_addr_t step = PAGE_SIZE;

	for_each_free_mem_range(i, NUMA_NO_NODE, MEMBLOCK_NONE, &this_start,
				&this_end, NULL) {
		candidate = this_start;
		while (candidate < this_end) {
			phys_addr_t test_addr2;

			/* Calculate address differing only in the specified bit */
			test_addr2 = candidate ^ BIT_ULL(bit);

			/* Check if both addresses are free */
			if (is_address_free(candidate) && is_address_free(test_addr2)) {
				*addr1 = candidate;
				*addr2 = test_addr2;
				return true;
			}

			/* Step to next candidate */
			candidate += step;
		}
	}

	return false;
}

static void __init test_address_bit(unsigned int bit)
{
	phys_addr_t addr1, addr2;
	u8 *vaddr1, *vaddr2;
	u8 val1, val2;

	if (!find_test_pair(bit, &addr1, &addr2)) {
		addr_bus_skipped_bits |= BIT_ULL(bit);
		return;
	}

	vaddr1 = (u8 *)__va(addr1);
	vaddr2 = (u8 *)__va(addr2);

	/* Write different patterns to both addresses (1 byte to avoid overlap) */
	WRITE_ONCE(*vaddr1, 0xAA);
	WRITE_ONCE(*vaddr2, 0x55);

	/* Read back and verify we got what we wrote */
	val1 = READ_ONCE(*vaddr1);
	val2 = READ_ONCE(*vaddr2);

	/* Check for mirroring: if either address doesn't read its expected value, bit is stuck */
	if (val1 != 0xAA || val2 != 0x55)
		addr_bus_failed_bits |= BIT_ULL(bit);

	/* Restore memory to zero */
	WRITE_ONCE(*vaddr1, 0);
	WRITE_ONCE(*vaddr2, 0);
}

static void __init test_address_bus(phys_addr_t start, phys_addr_t end)
{
	unsigned int addr_bits = sizeof(phys_addr_t) * 8;
	unsigned int bit;
	unsigned long failed_count, skipped_count, ok_count;
	char result_str[128];
	int pos = 0;

	addr_bus_failed_bits = 0;
	addr_bus_skipped_bits = 0;

	for (bit = 0; bit < addr_bits; bit++) {
		test_address_bit(bit);

		/* Build compact result string */
		if (bit > 0 && (bit % 8) == 0)
			result_str[pos++] = ' ';
		if (addr_bus_failed_bits & BIT_ULL(bit))
			result_str[pos++] = 'F';
		else if (addr_bus_skipped_bits & BIT_ULL(bit))
			result_str[pos++] = '_';
		else
			result_str[pos++] = 'O';
	}
	result_str[pos] = '\0';

	failed_count = hweight64(addr_bus_failed_bits);
	skipped_count = hweight64(addr_bus_skipped_bits);
	ok_count = addr_bits - failed_count - skipped_count;

	pr_info("Address bus test: %s\n", result_str);
	pr_info("Address bus: %lu OK, %lu FAIL, %lu UNKNOWN\n",
		ok_count, failed_count, skipped_count);
	if (addr_bus_failed_bits)
		pr_info("Address bus failed bits: 0x%016llx\n", addr_bus_failed_bits);
	if (addr_bus_skipped_bits)
		pr_info("Address bus skipped bits: 0x%016llx\n", addr_bus_skipped_bits);

	addr_bus_test_done = true;
}

void __init early_memtest(phys_addr_t start, phys_addr_t end)
{
	unsigned int i;
	unsigned int idx = 0;

	if (!memtest_pattern)
		return;

	pr_info("early_memtest: # of tests: %u\n", memtest_pattern);
	for (i = memtest_pattern-1; i < UINT_MAX; --i) {
		idx = i % ARRAY_SIZE(patterns);
		do_one_pass(patterns[idx], start, end);
	}

	test_address_bus(start, end);
}

void memtest_report_meminfo(struct seq_file *m)
{
	unsigned long early_memtest_bad_size_kb;

	if (!IS_ENABLED(CONFIG_PROC_FS))
		return;

	if (!early_memtest_done)
		return;

	early_memtest_bad_size_kb = early_memtest_bad_size >> 10;
	if (early_memtest_bad_size && !early_memtest_bad_size_kb)
		early_memtest_bad_size_kb = 1;
	/* If address bus test found failures, ensure we don't report 0 */
	if (addr_bus_test_done && addr_bus_failed_bits && !early_memtest_bad_size_kb)
		early_memtest_bad_size_kb = 1;
	/* When 0 is reported, it means there actually was a successful test */
	seq_printf(m, "EarlyMemtestBad:   %5lu kB\n", early_memtest_bad_size_kb);
}

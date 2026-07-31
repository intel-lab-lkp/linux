// SPDX-License-Identifier: GPL-2.0
/*
 * hwpoison-panic: verify vm.panic_on_unrecoverable_memory_failure by
 * injecting a hwpoison error on a kernel-owned page and confirming the
 * kernel panics.
 *
 * Three kinds of kernel-owned page can be targeted, selected with -k
 * (default: rodata):
 *
 *   rodata  - a PG_reserved page in the kernel rodata range, from
 *             /proc/iomem "Kernel rodata".  x86 and riscv only.
 *   slab    - a slab page found via /proc/kpageflags (KPF_SLAB).
 *   pgtable - a page-table page found via /proc/kpageflags (KPF_PGTABLE).
 *
 * slab and pgtable work everywhere; rodata needs an architecture that
 * publishes the rodata resource in /proc/iomem.
 *
 * The slab and pgtable variants exercise memory_failure() -> get_any_page()
 * on a non PG_reserved kernel-owned page, catching regressions where
 * get_any_page() collapses such pages into a transient -EIO instead of
 * -ENOTRECOVERABLE.
 *
 * A successful run crashes the kernel, so -f is required and the tool is
 * meant for a disposable VM (e.g. virtme-ng) with the serial console
 * captured.  The result is observed externally: the kernel must panic with
 *   "Memory failure: <pfn>: unrecoverable page"
 * Returning at all means no panic fired, so every exit status is non-zero:
 * either a failure, or an inconclusive run where the target PFN raced to
 * another page type before injection.
 *
 * Author: Breno Leitao <leitao@debian.org>
 */
#define _GNU_SOURCE
#include <fcntl.h>
#include <getopt.h>
#include <limits.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../../../include/uapi/linux/kernel-page-flags.h"
#include <api/fs/fs.h>

#define SYSCTL_PATH	"/proc/sys/vm/panic_on_unrecoverable_memory_failure"
#define INJECT_PATH	"/sys/devices/system/memory/hard_offline_page"
#define PROC_KPAGEFLAGS	"/proc/kpageflags"

/* Not exported by the uapi header, see tools/mm/page-types.c. */
#define KPF_RESERVED	32

#define BIT(name)	(1ULL << KPF_##name)
#define ARRAY_SIZE(x)	(sizeof(x) / sizeof((x)[0]))

/* The kpageflags bit identifying each kernel-owned page kind we target. */
static const struct page_kind {
	const char *name;
	uint64_t bit;
} page_kinds[] = {
	{ "rodata",  BIT(RESERVED) },
	{ "slab",    BIT(SLAB)     },
	{ "pgtable", BIT(PGTABLE)  },
};

static unsigned long page_size;

static void fatal(const char *x, ...)
{
	va_list ap;

	va_start(ap, x);
	vfprintf(stderr, x, ap);
	va_end(ap);
	exit(EXIT_FAILURE);
}

static int kpageflags_read(int fd, unsigned long pfn, uint64_t *flags)
{
	ssize_t bytes;

	bytes = pread(fd, flags, sizeof(*flags), (off_t)pfn * sizeof(*flags));

	return bytes == sizeof(*flags) ? 0 : -1;
}

static long pick_rodata_phys_addr(void)
{
	char line[256], label[64];
	unsigned long start, end;
	long addr = -1;
	FILE *f;

	f = fopen("/proc/iomem", "r");
	if (!f)
		return -1;

	/* Sub-resources are indented: "  02500000-02ffffff : Kernel rodata" */
	while (fgets(line, sizeof(line), f)) {
		if (sscanf(line, " %lx-%lx : %63[^\n]", &start, &end, label) != 3)
			continue;
		if (strcmp(label, "Kernel rodata") || end <= start)
			continue;
		/* Page-align up and return the first byte of that page. */
		addr = ((start + page_size - 1) / page_size) * page_size;
		break;
	}

	fclose(f);
	return addr;
}

static long pick_kpageflags_phys_addr(uint64_t want)
{
	unsigned long pfn = (16UL << 20) / page_size;
	long addr = -1;
	uint64_t flags;
	int fd;

	fd = open(PROC_KPAGEFLAGS, O_RDONLY);
	if (fd < 0)
		return -1;

	for (; kpageflags_read(fd, pfn, &flags) == 0; pfn++) {
		if ((flags & want) && !(flags & BIT(HWPOISON)) &&
		    !(flags & BIT(NOPAGE)) && !(flags & BIT(COMPOUND_TAIL))) {
			addr = pfn * page_size;
			break;
		}
	}

	close(fd);
	return addr;
}

static int read_sysctl(unsigned long *val)
{
	FILE *f = fopen(SYSCTL_PATH, "r");
	int ret;

	if (!f)
		return -1;
	ret = fscanf(f, "%lu", val) == 1 ? 0 : -1;
	fclose(f);

	return ret;
}

static int write_sysctl(unsigned long val)
{
	FILE *f = fopen(SYSCTL_PATH, "w");
	int ret;

	if (!f)
		return -1;
	ret = fprintf(f, "%lu", val) < 0 ? -1 : 0;
	fclose(f);

	return ret;
}

/* hard_offline_page() injects with MF_SW_SIMULATED, so unpoison is allowed. */
static void unpoison_pfn(unsigned long pfn)
{
	char path[PATH_MAX], buf[32];
	const char *debugfs;
	int fd, len;

	debugfs = debugfs__mount();
	if (!debugfs)
		return;

	snprintf(path, sizeof(path), "%s/hwpoison/unpoison-pfn", debugfs);
	fd = open(path, O_WRONLY);
	if (fd < 0)
		return;

	len = snprintf(buf, sizeof(buf), "0x%lx\n", pfn);
	if (write(fd, buf, len) < 0)
		perror("unpoison-pfn");
	close(fd);
}

static const char *inject_hwpoison(const struct page_kind *kind,
				   unsigned long phys_addr, unsigned long pfn)
{
	char buf[32];
	int fd, len;
	ssize_t ret;

	printf("injecting hwpoison at phys 0x%lx (pfn 0x%lx, kind=%s)\n",
	       phys_addr, pfn, kind->name);
	printf("expecting kernel panic: 'Memory failure: <pfn>: unrecoverable page'\n");
	fflush(stdout);

	fd = open(INJECT_PATH, O_WRONLY);
	if (fd < 0)
		return "cannot open " INJECT_PATH;

	len = snprintf(buf, sizeof(buf), "0x%lx", phys_addr);
	ret = write(fd, buf, len);
	close(fd);

	if (ret != len)
		return "inject failed before reaching the panic path";

	return "inject returned without panic; sysctl ineffective";
}

static const struct page_kind *lookup_kind(const char *name)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(page_kinds); i++)
		if (!strcmp(page_kinds[i].name, name))
			return &page_kinds[i];

	return NULL;
}

static void usage(void)
{
	printf("hwpoison-panic [options]\n"
	       "            -k|--kind <kind>  rodata (default, x86 and riscv only),\n"
	       "                              slab or pgtable\n"
	       "            -f|--force        really run it: this panics the kernel\n"
	       "            -h|--help         show this\n");
}

static const struct option opts[] = {
	{ "kind",  1, NULL, 'k' },
	{ "force", 0, NULL, 'f' },
	{ "help",  0, NULL, 'h' },
	{ NULL,    0, NULL, 0 },
};

static const struct page_kind *parse_args(int argc, char **argv, int *force)
{
	const struct page_kind *kind;
	const char *name = "rodata";
	int c;

	while ((c = getopt_long(argc, argv, "k:fh", opts, NULL)) != -1) {
		switch (c) {
		case 'k':
			name = optarg;
			break;
		case 'f':
			*force = 1;
			break;
		case 'h':
			usage();
			exit(EXIT_SUCCESS);
		default:
			usage();
			exit(EXIT_FAILURE);
		}
	}

	kind = lookup_kind(name);
	if (!kind)
		fatal("unknown kind '%s' (expected: rodata|slab|pgtable)\n",
		      name);

	return kind;
}

static void check_prereqs(int force)
{
	if (geteuid())
		fatal("must run as root\n");
	if (access(SYSCTL_PATH, W_OK))
		fatal("%s not present (kernel without the sysctl?)\n",
		      SYSCTL_PATH);
	if (access(INJECT_PATH, W_OK))
		fatal("%s not present (no MEMORY_HOTPLUG?)\n", INJECT_PATH);
	if (!force)
		fatal("this panics the kernel; pass -f to run it in a disposable VM\n");
}

static unsigned long pick_phys_addr(const struct page_kind *kind)
{
	long addr;

	if (kind->bit == BIT(RESERVED)) {
		addr = pick_rodata_phys_addr();
		if (addr < 0)
			fatal("no \"Kernel rodata\" in /proc/iomem; try -k slab or -k pgtable\n");
	} else {
		addr = pick_kpageflags_phys_addr(kind->bit);
		if (addr < 0)
			fatal("no usable %s PFN in %s\n", kind->name,
			      PROC_KPAGEFLAGS);
	}

	return addr;
}

static void arm_sysctl(unsigned long *prior)
{
	if (read_sysctl(prior))
		fatal("failed to read %s\n", SYSCTL_PATH);
	if (write_sysctl(1))
		fatal("failed to enable %s\n", SYSCTL_PATH);
}

static void recheck_kind(const struct page_kind *kind, unsigned long pfn,
			 const char *verdict)
{
	uint64_t flags;
	int fd, ret;

	fd = open(PROC_KPAGEFLAGS, O_RDONLY);
	if (fd < 0)
		fatal("%s (could not open %s)\n", verdict, PROC_KPAGEFLAGS);
	ret = kpageflags_read(fd, pfn, &flags);
	close(fd);

	if (ret)
		fatal("%s (could not reconfirm page type via %s)\n", verdict,
		      PROC_KPAGEFLAGS);
	if (flags & kind->bit)
		fatal("%s (page still %s)\n", verdict, kind->name);

	fprintf(stderr, "target PFN no longer %s; raced before inject, inconclusive\n",
		kind->name);
}

int main(int argc, char **argv)
{
	unsigned long phys_addr, pfn, prior;
	const struct page_kind *kind;
	const char *verdict;
	int force = 0;

	kind = parse_args(argc, argv, &force);
	page_size = getpagesize();
	check_prereqs(force);

	phys_addr = pick_phys_addr(kind);
	pfn = phys_addr / page_size;
	arm_sysctl(&prior);

	verdict = inject_hwpoison(kind, phys_addr, pfn);

	/* No panic fired.  Put the machine back, then report. */
	write_sysctl(prior);
	unpoison_pfn(pfn);
	recheck_kind(kind, pfn, verdict);

	return EXIT_FAILURE;
}

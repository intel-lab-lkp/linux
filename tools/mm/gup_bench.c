#define __SANE_USERSPACE_TYPES__ // Use ll64
#include <fcntl.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stdint.h>
#include <limits.h>
#include <string.h>
#include <mm/gup_test.h>
#include <mm/hugepage_settings.h>

#define MB (1UL << 20)

/* Just the flags we need, copied from the kernel internals. */
#define FOLL_WRITE	0x01	/* check pte is writable */

#define GUP_TEST_FILE "/sys/kernel/debug/gup_test"

static unsigned long cmd = GUP_FAST_BENCHMARK;
static int gup_fd, repeats = 1;
static unsigned long size = 128 * MB;
static atomic_int bench_error;
/* Serialize prints */
static pthread_mutex_t print_mutex = PTHREAD_MUTEX_INITIALIZER;

static char *cmd_to_str(unsigned long cmd)
{
	switch (cmd) {
	case GUP_FAST_BENCHMARK:
		return "GUP_FAST_BENCHMARK";
	case PIN_FAST_BENCHMARK:
		return "PIN_FAST_BENCHMARK";
	case PIN_LONGTERM_BENCHMARK:
		return "PIN_LONGTERM_BENCHMARK";
	}
	return "Unknown command";
}

static long parse_long_arg_base(const char *arg, const char *name, int base)
{
	char *end;
	long val;

	errno = 0;
	val = strtol(arg, &end, base);
	if (errno || end == arg || *end != '\0') {
		fprintf(stderr, "Invalid %s '%s'\n", name, arg);
		exit(1);
	}
	return val;
}

static long parse_long_arg(const char *arg, const char *name)
{
	return parse_long_arg_base(arg, name, 10);
}

static long parse_positive_long_arg(const char *arg, const char *name)
{
	long val = parse_long_arg(arg, name);

	if (val < 1) {
		fprintf(stderr, "Invalid %s '%s'\n", name, arg);
		exit(1);
	}

	return val;
}

void *gup_thread(void *data)
{
	struct gup_test gup = *(struct gup_test *)data;
	int i, status;

	for (i = 0; i < repeats; i++) {
		gup.size = size;
		status = ioctl(gup_fd, cmd, &gup);
		if (status) {
			int err = errno;

			bench_error = 1;
			pthread_mutex_lock(&print_mutex);
			fprintf(stderr, "%s ioctl failed: %s\n", cmd_to_str(cmd),
				strerror(err));
			pthread_mutex_unlock(&print_mutex);
			break;
		}

		pthread_mutex_lock(&print_mutex);
		if (gup.size == size)
			printf("%s time: get:%lld us put:%lld us\n",
			       cmd_to_str(cmd), gup.get_delta_usec,
			       gup.put_delta_usec);
		else
			printf("%s time: get:%lld put:%lld us, truncated (size: %lld)\n",
			       cmd_to_str(cmd), gup.get_delta_usec,
			       gup.put_delta_usec, gup.size);
		pthread_mutex_unlock(&print_mutex);
	}

	return NULL;
}

int main(int argc, char **argv)
{
	struct gup_test gup = { 0 };
	int filed, i, opt, nr_pages = 1, thp = -1, write = 1;
	int nthreads = 1, ret, started_threads = 0;
	int flags = MAP_PRIVATE;
	const char *file = "/dev/zero";
	bool hugetlb = false, restore_hugetlb = false;
	unsigned long nr_pages_per_call;
	pthread_t *tid;
	char *p;

	while ((opt = getopt(argc, argv, "m:r:n:F:f:aj:tTLuwWSH")) != -1) {
		switch (opt) {
		case 'a':
			cmd = PIN_FAST_BENCHMARK;
			break;
		case 'L':
			cmd = PIN_LONGTERM_BENCHMARK;
			break;
		case 'F': {
			long val;

			val = parse_long_arg_base(optarg, "GUP flags", 0);
			if (val < 0 || val > UINT_MAX) {
				fprintf(stderr, "Invalid GUP flags '%s'\n", optarg);
				exit(1);
			}

			gup.gup_flags = val;
			break;
		}
		case 'j': {
			long val;

			val = parse_positive_long_arg(optarg, "thread count");
			if (val > INT_MAX ||
			    (size_t)val > SIZE_MAX / sizeof(pthread_t)) {
				fprintf(stderr, "Invalid thread count '%s'\n", optarg);
				exit(1);
			}
			nthreads = val;
			break;
		}
		case 'm':
			size = parse_positive_long_arg(optarg, "size");
			if (size > ULONG_MAX / MB) {
				fprintf(stderr, "Invalid size '%s'\n", optarg);
				exit(1);
			}
			size *= MB;
			break;
		case 'r': {
			long val;

			val = parse_positive_long_arg(optarg, "repeat count");
			if (val > INT_MAX) {
				fprintf(stderr, "Invalid repeat count '%s'\n", optarg);
				exit(1);
			}
			repeats = val;
			break;
		}
		case 'n': {
			long val;

			val = parse_long_arg(optarg, "page count");
			if (val != -1 && (val < 1 || val > INT_MAX)) {
				fprintf(stderr, "Invalid page count '%s'\n", optarg);
				exit(1);
			}
			nr_pages = val;
			break;
		}
		case 't':
			thp = 1;
			break;
		case 'T':
			thp = 0;
			break;
		case 'u':
			cmd = GUP_FAST_BENCHMARK;
			break;
		case 'w':
			write = 1;
			break;
		case 'W':
			write = 0;
			break;
		case 'f':
			file = optarg;
			break;
		case 'S':
			flags &= ~MAP_PRIVATE;
			flags |= MAP_SHARED;
			break;
		case 'H':
			flags |= (MAP_HUGETLB | MAP_ANONYMOUS);
			hugetlb = true;
			break;
		default:
			fprintf(stderr, "Wrong argument\n");
			exit(1);
		}
	}

	if (optind != argc) {
		fprintf(stderr, "Unexpected argument '%s'\n", argv[optind]);
		exit(1);
	}

	if (hugetlb) {
		unsigned long hp_size = default_huge_page_size();

		if (!hp_size) {
			fprintf(stderr, "Could not determine huge page size\n");
			return 1;
		}

		if (size > ULONG_MAX - (hp_size - 1)) {
			fprintf(stderr, "HugeTLB mapping size is too large\n");
			return 1;
		}

		size = (size + hp_size - 1) & ~(hp_size - 1);
		if (!hugetlb_setup_default(size / hp_size)) {
			fprintf(stderr, "Not enough huge pages\n");
			hugetlb_restore_settings();
			return 1;
		}
		restore_hugetlb = true;
	}

	nr_pages_per_call = nr_pages < 0 ? size / getpagesize() :
		(unsigned long)nr_pages;
	if (nr_pages_per_call > UINT_MAX) {
		fprintf(stderr, "Page count is too large\n");
		if (restore_hugetlb)
			hugetlb_restore_settings();
		return 1;
	}
	gup.nr_pages_per_call = nr_pages_per_call;
	if (write)
		gup.gup_flags |= FOLL_WRITE;

	filed = open(file, O_RDWR | O_CREAT, 0664);
	if (filed < 0) {
		fprintf(stderr, "Unable to open %s: %s\n", file, strerror(errno));
		if (restore_hugetlb)
			hugetlb_restore_settings();
		return 1;
	}

	gup_fd = open(GUP_TEST_FILE, O_RDWR);
	if (gup_fd == -1) {
		int err = errno;

		close(filed);
		if (err == EACCES)
			fprintf(stderr, "Please run as root\n");
		else if (err == ENOENT) {
			DIR *debugfs = opendir("/sys/kernel/debug");

			if (!debugfs)
				fprintf(stderr, "Mount debugfs at /sys/kernel/debug\n");
			else {
				closedir(debugfs);
				fprintf(stderr, "Check CONFIG_GUP_TEST in kernel config\n");
			}
		} else
			fprintf(stderr, "Failed to open %s: %s\n", GUP_TEST_FILE,
				strerror(err));
		if (restore_hugetlb)
			hugetlb_restore_settings();
		return 1;
	}

	p = mmap(NULL, size, PROT_READ | PROT_WRITE, flags, filed, 0);
	if (p == MAP_FAILED) {
		fprintf(stderr, "mmap: %s\n", strerror(errno));
		close(filed);
		close(gup_fd);

		if (restore_hugetlb)
			hugetlb_restore_settings();
		return 1;
	}
	close(filed);
	gup.addr = (unsigned long)p;

	if (thp == 1)
		madvise(p, size, MADV_HUGEPAGE);
	else if (thp == 0)
		madvise(p, size, MADV_NOHUGEPAGE);

	/* Fault them in here, from user space. */
	for (; (unsigned long)p < gup.addr + size; p += getpagesize())
		p[0] = 0;

	tid = malloc(sizeof(pthread_t) * nthreads);
	if (!tid) {
		fprintf(stderr, "Failed to allocate %d threads: %s\n",
			nthreads, strerror(errno));
		munmap((void *)gup.addr, size);
		close(gup_fd);
		if (restore_hugetlb)
			hugetlb_restore_settings();
		return 1;
	}

	for (i = 0; i < nthreads; i++) {
		ret = pthread_create(&tid[i], NULL, gup_thread, &gup);
		if (ret) {
			fprintf(stderr, "pthread_create failed: %s\n", strerror(ret));
			bench_error = 1;
			break;
		}
		started_threads++;
	}
	for (i = 0; i < started_threads; i++) {
		ret = pthread_join(tid[i], NULL);
		if (ret) {
			fprintf(stderr, "pthread_join failed: %s\n", strerror(ret));
			bench_error = 1;
		}
	}

	free(tid);
	munmap((void *)gup.addr, size);
	close(gup_fd);
	if (restore_hugetlb)
		hugetlb_restore_settings();

	return bench_error ? 1 : 0;
}

// SPDX-License-Identifier: GPL-2.0

#define _GNU_SOURCE
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <getopt.h>
#include <limits.h>
#include <net/if.h>
#include <netinet/in.h>
#include <poll.h>
#include <sched.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#include "kselftest.h"

#define SYSCTL_ROOT		"/proc/sys/net"
#define SYSCTL_TEXT_SIZE	4096
#define SYSCTL_MAX_VALUES	256
#define KMSG_TEXT_SIZE		4096
#define FIRST_TEST_PORT		20000
#define LAST_TEST_PORT		59998

struct sysctl_number {
	uint64_t magnitude;
	bool negative;
};

struct sysctl_values {
	struct sysctl_number value[SYSCTL_MAX_VALUES];
	size_t count;
};

static int kmsg_fd = -1;
static unsigned int next_tcp_port = FIRST_TEST_PORT;
static unsigned int next_udp_port = FIRST_TEST_PORT;
static size_t tested_files;
static size_t skipped_files;
static size_t tested_values;
static size_t tcp_attempts;
static size_t tcp_successes;
static size_t udp_attempts;
static size_t udp_successes;
static char failure[256];
static char tcp_delivery_failure[256];
static char udp_delivery_failure[256];

static const char * const representative_sysctls[] = {
	"ipv4/tcp_rmem",
	"ipv4/tcp_wmem",
	"ipv4/udp_rmem_min",
	"ipv4/udp_wmem_min",
};

static bool selected_sysctls[ARRAY_SIZE(representative_sysctls)];

static void set_failure(const char *fmt, ...)
{
	va_list args;

	if (failure[0])
		return;

	va_start(args, fmt);
	vsnprintf(failure, sizeof(failure), fmt, args);
	va_end(args);
}

static struct sysctl_number number_from_signed(int64_t value)
{
	struct sysctl_number number = {};

	if (value < 0) {
		number.negative = true;
		number.magnitude = (uint64_t)(-(value + 1)) + 1;
	} else {
		number.magnitude = value;
	}
	return number;
}

static struct sysctl_number number_from_unsigned(uint64_t value)
{
	struct sysctl_number number = {
		.magnitude = value,
	};

	return number;
}

static int64_t number_to_signed(struct sysctl_number number)
{
	if (!number.negative)
		return (int64_t)number.magnitude;
	if (number.magnitude == (UINT64_C(1) << 63))
		return INT64_MIN;
	return -(int64_t)number.magnitude;
}

static bool numbers_equal(struct sysctl_number a, struct sysctl_number b)
{
	return a.magnitude == b.magnitude &&
	       (a.magnitude == 0 || a.negative == b.negative);
}

static bool format_number(struct sysctl_number number, char *text,
			  size_t text_size)
{
	int n = snprintf(text, text_size, number.negative ? "-%llu" : "%llu",
			 (unsigned long long)number.magnitude);

	return n >= 0 && (size_t)n < text_size;
}

static bool parse_values(char *text, struct sysctl_values *values)
{
	char *pos = text;

	values->count = 0;
	while (*pos) {
		char *end;
		struct sysctl_number value;

		while (isspace((unsigned char)*pos))
			pos++;
		if (!*pos)
			break;
		if (values->count == SYSCTL_MAX_VALUES)
			return false;

		errno = 0;
		if (*pos == '-') {
			long long signed_value = strtoll(pos, &end, 10);

			if (end == pos || errno == ERANGE)
				return false;
			value = number_from_signed(signed_value);
		} else {
			unsigned long long unsigned_value = strtoull(pos, &end, 10);

			if (end == pos || errno == ERANGE)
				return false;
			value = number_from_unsigned(unsigned_value);
		}
		if (*end && !isspace((unsigned char)*end))
			return false;

		values->value[values->count++] = value;
		pos = end;
	}

	return values->count > 0;
}

static bool read_values(const char *path, struct sysctl_values *values)
{
	char text[SYSCTL_TEXT_SIZE];
	size_t len = 0;
	int fd;
	bool ok = false;

	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return false;

	while (len < sizeof(text) - 1) {
		ssize_t n = read(fd, text + len, sizeof(text) - 1 - len);

		if (n < 0 && errno == EINTR)
			continue;
		if (n < 0)
			goto out;
		if (!n)
			break;
		len += n;
	}
	if (len == sizeof(text) - 1)
		goto out;

	text[len] = '\0';
	ok = parse_values(text, values);
out:
	close(fd);
	return ok;
}

static bool values_equal(const struct sysctl_values *a,
			 const struct sysctl_values *b)
{
	size_t i;

	if (a->count != b->count)
		return false;
	for (i = 0; i < a->count; i++) {
		if (!numbers_equal(a->value[i], b->value[i]))
			return false;
	}
	return true;
}

static bool format_values(const struct sysctl_values *values, char *text,
			  size_t text_size)
{
	size_t i, len = 0;

	for (i = 0; i < values->count; i++) {
		char number[32];
		int n;

		if (!format_number(values->value[i], number, sizeof(number)))
			return false;
		n = snprintf(text + len, text_size - len, "%s%c", number,
			     i + 1 == values->count ? '\n' : ' ');
		if (n < 0 || (size_t)n >= text_size - len)
			return false;
		len += n;
	}
	return len > 0;
}

static bool write_values(const char *path,
			 const struct sysctl_values *values)
{
	char text[SYSCTL_TEXT_SIZE];
	size_t len;
	ssize_t n;
	int fd;
	bool ok;

	if (!format_values(values, text, sizeof(text)))
		return false;
	len = strlen(text);

	fd = open(path, O_WRONLY | O_CLOEXEC);
	if (fd < 0)
		return false;
	do {
		n = write(fd, text, len);
	} while (n < 0 && errno == EINTR);
	ok = n == (ssize_t)len;
	if (close(fd) < 0)
		ok = false;
	return ok;
}

static bool restore_values(const char *path,
			   const struct sysctl_values *original)
{
	struct sysctl_values current;

	if (read_values(path, &current) && values_equal(original, &current))
		return true;
	return write_values(path, original) &&
	       read_values(path, &current) &&
	       values_equal(original, &current);
}

static bool check_kernel_log(void)
{
	static const char * const splats[] = {
		"WARNING:",
		"BUG:",
		"Oops:",
		"divide error",
		"general protection fault",
		"invalid opcode",
		"kernel BUG at",
		"Kernel panic",
		"KASAN:",
		"UBSAN:",
		"soft lockup",
		"hard LOCKUP",
		"blocked for more than",
		"possible circular locking dependency",
	};
	char record[KMSG_TEXT_SIZE + 1];

	for (;;) {
		ssize_t n = read(kmsg_fd, record, sizeof(record) - 1);
		char *message;
		size_t i;

		if (n < 0 && errno == EINTR)
			continue;
		if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
			return true;
		if (n < 0) {
			set_failure("cannot read /dev/kmsg: %s", strerror(errno));
			return false;
		}
		if (!n)
			return true;

		record[n] = '\0';
		message = strchr(record, ';');
		message = message ? message + 1 : record;
		for (i = 0; i < ARRAY_SIZE(splats); i++) {
			if (!strstr(message, splats[i]))
				continue;
			set_failure("kernel splat detected: %.180s", message);
			return false;
		}
	}
}

static uint64_t ordered_value(int64_t value)
{
	/* Map signed integers to unsigned keys in numeric order. */
	return (uint64_t)value ^ (UINT64_C(1) << 63);
}

static int64_t unordered_value(uint64_t key)
{
	uint64_t raw = key ^ (UINT64_C(1) << 63);

	if (raw <= INT64_MAX)
		return (int64_t)raw;
	return -1 - (int64_t)(UINT64_MAX - raw);
}

static int probe_value(const char *path, const struct sysctl_values *original,
		       size_t index, struct sysctl_number candidate,
		       bool *accepted)
{
	struct sysctl_values expected = *original;
	struct sysctl_values current;
	char text[32];

	expected.value[index] = candidate;
	*accepted = false;

	if (write_values(path, &expected) &&
	    read_values(path, &current) &&
	    values_equal(&expected, &current))
		*accepted = true;

	if (!restore_values(path, original)) {
		format_number(candidate, text, sizeof(text));
		set_failure("failed to restore %s after probing %s", path, text);
		return -1;
	}
	if (!check_kernel_log())
		return -1;
	return 0;
}

static uint64_t next_step(uint64_t step)
{
	if (step > UINT64_MAX / 2)
		return UINT64_MAX;
	return step * 2;
}

static int find_bound(const char *path,
		      const struct sysctl_values *original, size_t index,
		      bool lower, bool unsigned_domain,
		      struct sysctl_number *bound)
{
	struct sysctl_number start = original->value[index];
	uint64_t valid = unsigned_domain ? start.magnitude :
			 ordered_value(number_to_signed(start));
	uint64_t step = 1;

	for (;;) {
		uint64_t candidate, invalid;
		bool accepted;

		if (lower) {
			if (!valid)
				break;
			candidate = valid > step ? valid - step : 0;
		} else {
			if (valid == UINT64_MAX)
				break;
			candidate = UINT64_MAX - valid < step ?
				    UINT64_MAX : valid + step;
		}
		if (candidate == valid)
			break;

		if (probe_value(path, original, index,
				unsigned_domain ? number_from_unsigned(candidate) :
				number_from_signed(unordered_value(candidate)),
				&accepted))
			return -1;
		if (accepted) {
			valid = candidate;
			step = next_step(step);
			continue;
		}

		invalid = candidate;
		while (lower ? valid - invalid > 1 : invalid - valid > 1) {
			uint64_t middle = lower ?
					  invalid + (valid - invalid) / 2 :
					  valid + (invalid - valid) / 2;

			if (probe_value(path, original, index,
					unsigned_domain ?
					number_from_unsigned(middle) :
					number_from_signed(unordered_value(middle)),
					&accepted))
				return -1;
			if (accepted)
				valid = middle;
			else
				invalid = middle;
		}
		break;
	}

	*bound = unsigned_domain ? number_from_unsigned(valid) :
		 number_from_signed(unordered_value(valid));
	return 0;
}

static int find_lower_bound(const char *path,
			    const struct sysctl_values *original,
			    size_t index, struct sysctl_number *bound)
{
	struct sysctl_number start = original->value[index];
	bool unsigned_domain = !start.negative && start.magnitude > INT64_MAX;

	return find_bound(path, original, index, true, unsigned_domain, bound);
}

static int find_upper_bound(const char *path,
			    const struct sysctl_values *original,
			    size_t index, struct sysctl_number *bound)
{
	bool unsigned_domain = !original->value[index].negative;

	return find_bound(path, original, index, false, unsigned_domain, bound);
}

static int get_port_pair(bool tcp, uint16_t *server, uint16_t *client)
{
	unsigned int *next = tcp ? &next_tcp_port : &next_udp_port;

	if (*next > LAST_TEST_PORT) {
		errno = EADDRNOTAVAIL;
		return -1;
	}
	*server = *next;
	*client = *next + 1;
	*next += 2;
	return 0;
}

static int bind_loopback(int fd, uint16_t port)
{
	struct sockaddr_in addr = {
		.sin_family = AF_INET,
		.sin_port = htons(port),
		.sin_addr.s_addr = htonl(INADDR_LOOPBACK),
	};

	return bind(fd, (struct sockaddr *)&addr, sizeof(addr));
}

static int wait_readable(int fd)
{
	struct pollfd pfd = {
		.fd = fd,
		.events = POLLIN,
	};
	int ret;

	ret = poll(&pfd, 1, 1000);
	if (ret < 0)
		return -1;
	if (!ret) {
		errno = ETIMEDOUT;
		return -1;
	}
	if (!(pfd.revents & POLLIN)) {
		errno = EIO;
		return -1;
	}
	return 0;
}

static int tcp_loopback(void)
{
	struct timeval timeout = { .tv_sec = 1 };
	uint16_t server_port, client_port;
	char data[64] = { 1 };
	char received[sizeof(data)];
	int listener = -1, client = -1, server = -1;
	int ret = -1;
	ssize_t n;

	if (get_port_pair(true, &server_port, &client_port))
		return -1;
	listener = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
	client = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (listener < 0 || client < 0 ||
	    setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &timeout,
		       sizeof(timeout)) ||
	    bind_loopback(listener, server_port) ||
	    listen(listener, 1) || bind_loopback(client, client_port))
		goto out;

	{
		struct sockaddr_in addr = {
			.sin_family = AF_INET,
			.sin_port = htons(server_port),
			.sin_addr.s_addr = htonl(INADDR_LOOPBACK),
		};

		if (connect(client, (struct sockaddr *)&addr, sizeof(addr)))
			goto out;
	}

	if (wait_readable(listener))
		goto out;
	server = accept4(listener, NULL, NULL, SOCK_CLOEXEC);
	if (server < 0)
		goto out;
	n = send(client, data, sizeof(data), MSG_NOSIGNAL);
	if (n <= 0) {
		if (!n)
			errno = EPIPE;
		goto out;
	}
	if (wait_readable(server))
		goto out;

	n = recv(server, received, sizeof(received), 0);
	if (n <= 0) {
		if (!n)
			errno = ECONNRESET;
		goto out;
	}
	ret = 0;
out:
	if (server >= 0)
		close(server);
	if (client >= 0)
		close(client);
	if (listener >= 0)
		close(listener);
	return ret;
}

static int udp_loopback(void)
{
	uint16_t server_port, client_port;
	struct sockaddr_in addr = {
		.sin_family = AF_INET,
		.sin_addr.s_addr = htonl(INADDR_LOOPBACK),
	};
	char data = 1, received;
	int server = -1, client = -1;
	int ret = -1;
	ssize_t n;

	if (get_port_pair(false, &server_port, &client_port))
		return -1;
	server = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
	client = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
	if (server < 0 || client < 0 || bind_loopback(server, server_port) ||
	    bind_loopback(client, client_port))
		goto out;

	addr.sin_port = htons(server_port);
	n = sendto(client, &data, sizeof(data), 0, (struct sockaddr *)&addr,
		   sizeof(addr));
	if (n != sizeof(data)) {
		if (n >= 0)
			errno = EIO;
		goto out;
	}
	if (wait_readable(server))
		goto out;

	n = recv(server, &received, sizeof(received), 0);
	if (n != sizeof(received)) {
		if (n >= 0)
			errno = EIO;
		goto out;
	}
	ret = 0;
out:
	if (client >= 0)
		close(client);
	if (server >= 0)
		close(server);
	return ret;
}

static void run_loopback_traffic(const char *path, size_t index,
				 const char *value)
{
	int err;

	tcp_attempts++;
	if (!tcp_loopback()) {
		tcp_successes++;
	} else {
		err = errno;
		if (!tcp_delivery_failure[0])
			snprintf(tcp_delivery_failure,
				 sizeof(tcp_delivery_failure),
				 "%s[%zu]=%s: %s", path, index, value,
				 strerror(err));
	}

	udp_attempts++;
	if (!udp_loopback()) {
		udp_successes++;
	} else {
		err = errno;
		if (!udp_delivery_failure[0])
			snprintf(udp_delivery_failure,
				 sizeof(udp_delivery_failure),
				 "%s[%zu]=%s: %s", path, index, value,
				 strerror(err));
	}
}

static int exercise_value(const char *path,
			  const struct sysctl_values *original,
			  size_t index, struct sysctl_number value)
{
	struct sysctl_values expected = *original;
	char text[32];
	bool restore = !numbers_equal(value, original->value[index]);
	bool applied = false;

	if (!format_number(value, text, sizeof(text))) {
		set_failure("cannot format boundary for %s", path);
		return -1;
	}
	if (restore) {
		struct sysctl_values current;

		expected.value[index] = value;
		applied = write_values(path, &expected) &&
			  read_values(path, &current) &&
			  values_equal(&expected, &current);
		if (!applied) {
			if (!restore_values(path, original)) {
				set_failure("failed to restore %s after setting %s",
					    path, text);
				return -1;
			}
			set_failure("%s no longer accepts boundary %s", path, text);
			return -1;
		}
	}

	check_kernel_log();
	if (!failure[0])
		run_loopback_traffic(path, index, text);

	if (restore && !restore_values(path, original)) {
		format_number(value, text, sizeof(text));
		set_failure("failed to restore %s after traffic at %s", path,
			    text);
	}
	check_kernel_log();
	return failure[0] ? -1 : 0;
}

static bool prepare_sysctl(const char *path,
			   const struct sysctl_values *original)
{
	struct sysctl_values current;
	bool writable;

	writable = write_values(path, original) &&
		   read_values(path, &current) &&
		   values_equal(original, &current);
	if (!writable && !restore_values(path, original)) {
		set_failure("failed to restore %s after initial write", path);
		return false;
	}
	if (!check_kernel_log())
		return false;
	return writable;
}

static int test_sysctl(const char *path, const struct sysctl_values *original)
{
	size_t i;

	if (!prepare_sysctl(path, original)) {
		if (!failure[0]) {
			skipped_files++;
			printf("# SKIP %s: sysctl is not writable\n", path);
		}
		return failure[0] ? -1 : 0;
	}

	tested_files++;
	for (i = 0; i < original->count; i++) {
		struct sysctl_number lower, upper;

		if (find_lower_bound(path, original, i, &lower) ||
		    find_upper_bound(path, original, i, &upper))
			return -1;
		if (exercise_value(path, original, i, lower) ||
		    (!numbers_equal(upper, lower) &&
		     exercise_value(path, original, i, upper)))
			return -1;
		tested_values++;
	}
	return 0;
}

static int scan_sysctl(const char *path, const struct stat *statbuf,
		       int type, struct FTW *ftwbuf)
{
	struct sysctl_values values;

	(void)statbuf;
	(void)ftwbuf;
	if (type != FTW_F || access(path, W_OK))
		return 0;
	if (!read_values(path, &values))
		return 0;
	return test_sysctl(path, &values) ? 1 : 0;
}

static void print_usage(FILE *stream, const char *program)
{
	size_t i;

	fprintf(stream, "Usage: %s [-a | -s SYSCTL ...]\n", program);
	fprintf(stream, "  -s, --sysctl SYSCTL  Test a representative sysctl (repeatable)\n");
	fprintf(stream, "  -a, --all            Test all writable numeric sysctls under %s\n",
		SYSCTL_ROOT);
	fprintf(stream, "  -h, --help           Show this help\n");
	fprintf(stream, "Default sysctls:\n");
	for (i = 0; i < ARRAY_SIZE(representative_sysctls); i++)
		fprintf(stream, "  %s\n", representative_sysctls[i]);
}

static int parse_options(int argc, char **argv, bool *all_sysctls)
{
	static const struct option options[] = {
		{ "all", no_argument, NULL, 'a' },
		{ "help", no_argument, NULL, 'h' },
		{ "sysctl", required_argument, NULL, 's' },
		{}
	};
	bool selection_given = false;
	int option;

	while ((option = getopt_long(argc, argv, "ahs:", options, NULL)) != -1) {
		switch (option) {
		case 'a':
			if (selection_given)
				goto invalid_options;
			selection_given = true;
			*all_sysctls = true;
			break;
		case 's': {
			size_t i;

			if (*all_sysctls)
				goto invalid_options;
			selection_given = true;
			for (i = 0; i < ARRAY_SIZE(representative_sysctls); i++) {
				if (strcmp(optarg, representative_sysctls[i]))
					continue;
				selected_sysctls[i] = true;
				break;
			}
			if (i == ARRAY_SIZE(representative_sysctls)) {
				fprintf(stderr, "Unknown sysctl: %s\n", optarg);
				goto invalid_options;
			}
			break;
		}
		case 'h':
			print_usage(stdout, argv[0]);
			return 1;
		default:
			goto invalid_options;
		}
	}
	if (optind != argc)
		goto invalid_options;

	if (!selection_given) {
		size_t i;

		for (i = 0; i < ARRAY_SIZE(representative_sysctls); i++)
			selected_sysctls[i] = true;
	}
	return 0;

invalid_options:
	print_usage(stderr, argv[0]);
	return -1;
}

static int test_selected_sysctl(const char *name)
{
	char path[PATH_MAX];
	struct sysctl_values values;
	int n;

	n = snprintf(path, sizeof(path), "%s/%s", SYSCTL_ROOT, name);
	if (n < 0 || (size_t)n >= sizeof(path)) {
		set_failure("sysctl path is too long: %s", name);
		return -1;
	}

	if (access(path, F_OK)) {
		if (errno == ENOENT || errno == ENOTDIR) {
			skipped_files++;
			printf("# SKIP %s: not available\n", name);
			return 0;
		}
		set_failure("cannot inspect %s: %s", path, strerror(errno));
		return -1;
	}
	if (access(path, R_OK)) {
		int err = errno;

		skipped_files++;
		printf("# SKIP %s: not readable (%s)\n", name, strerror(err));
		return 0;
	}
	if (!read_values(path, &values)) {
		set_failure("%s is not a readable numeric sysctl", path);
		return -1;
	}
	if (access(path, W_OK)) {
		int err = errno;

		skipped_files++;
		printf("# SKIP %s: not writable (%s)\n", name, strerror(err));
		return 0;
	}

	printf("# Testing %s\n", name);
	return test_sysctl(path, &values);
}

static int set_loopback_up(void)
{
	struct ifreq ifr = {};
	int fd, ret = -1;

	fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
	if (fd < 0)
		return -1;
	strncpy(ifr.ifr_name, "lo", IFNAMSIZ - 1);
	if (ioctl(fd, SIOCGIFFLAGS, &ifr))
		goto out;
	ifr.ifr_flags |= IFF_UP;
	ret = ioctl(fd, SIOCSIFFLAGS, &ifr);
out:
	close(fd);
	return ret;
}

int main(int argc, char **argv)
{
	bool all_sysctls = false;
	int option_status, ret;
	size_t i;

	option_status = parse_options(argc, argv, &all_sysctls);
	if (option_status > 0)
		return KSFT_PASS;
	if (option_status < 0)
		return KSFT_FAIL;

	ksft_print_header();
	ksft_set_plan(1);

	if (unshare(CLONE_NEWNET))
		ksft_exit_skip("cannot create network namespace: %s\n",
			       strerror(errno));

	kmsg_fd = open("/dev/kmsg", O_RDONLY | O_NONBLOCK | O_CLOEXEC);
	if (kmsg_fd < 0)
		ksft_exit_skip("cannot read /dev/kmsg: %s\n", strerror(errno));
	if (lseek(kmsg_fd, 0, SEEK_END) < 0)
		ksft_exit_skip("cannot seek /dev/kmsg: %s\n", strerror(errno));
	if (set_loopback_up())
		ksft_exit_skip("cannot bring loopback up: %s\n", strerror(errno));
	if (!check_kernel_log())
		goto fail;

	if (all_sysctls) {
		ret = nftw(SYSCTL_ROOT, scan_sysctl, 32, FTW_PHYS);
		if (ret < 0 && !failure[0])
			set_failure("cannot scan %s: %s", SYSCTL_ROOT,
				    strerror(errno));
	} else {
		for (i = 0; i < ARRAY_SIZE(representative_sysctls); i++) {
			if (!selected_sysctls[i])
				continue;
			if (test_selected_sysctl(representative_sysctls[i]))
				break;
		}
	}
	if (!failure[0] && !tested_files)
		ksft_exit_skip("no selected writable numeric network sysctls found\n");
	if (!failure[0] && (!tcp_successes || !udp_successes))
		set_failure("loopback delivery incomplete: TCP %zu/%zu, UDP %zu/%zu",
			    tcp_successes, tcp_attempts,
			    udp_successes, udp_attempts);
	if (!failure[0] && !check_kernel_log())
		goto fail;
	if (tcp_delivery_failure[0])
		printf("# first TCP delivery failure: %s\n",
		       tcp_delivery_failure);
	if (udp_delivery_failure[0])
		printf("# first UDP delivery failure: %s\n",
		       udp_delivery_failure);
	if (failure[0])
		goto fail;

	printf("# tested %zu sysctls, %zu values; skipped %zu\n",
	       tested_files, tested_values, skipped_files);
	ksft_test_result_pass("TCP %zu/%zu, UDP %zu/%zu deliveries succeeded\n",
			      tcp_successes, tcp_attempts,
			      udp_successes, udp_attempts);
	ksft_finished();
	close(kmsg_fd);
	return KSFT_PASS;

fail:
	ksft_test_result_fail("%s\n", failure);
	ksft_finished();
	close(kmsg_fd);
	return KSFT_FAIL;
}

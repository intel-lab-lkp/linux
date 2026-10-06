// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <linux/keyctl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>

#include "../kselftest.h"

#define EXPIRY_PATH "/proc/sys/kernel/keys/persistent_keyring_expiry"
#define TEST_EXPIRY 300

static int read_expiry(unsigned int *expiry)
{
	char buf[32];
	char *end;
	unsigned long value;
	ssize_t len;
	int fd;

	fd = open(EXPIRY_PATH, O_RDONLY);
	if (fd < 0)
		return -1;

	len = read(fd, buf, sizeof(buf) - 1);
	close(fd);
	if (len <= 0)
		return -1;

	buf[len] = '\0';
	errno = 0;
	value = strtoul(buf, &end, 10);
	if (errno || end == buf || (*end != '\n' && *end != '\0') ||
	    value > UINT_MAX) {
		errno = EINVAL;
		return -1;
	}

	*expiry = value;
	return 0;
}

static int write_expiry(unsigned int expiry)
{
	char buf[32];
	size_t len;
	ssize_t written;
	int fd;

	len = snprintf(buf, sizeof(buf), "%u\n", expiry);
	fd = open(EXPIRY_PATH, O_WRONLY);
	if (fd < 0)
		return -1;

	written = write(fd, buf, len);
	if (written != len) {
		int saved_errno = errno;

		close(fd);
		errno = written >= 0 ? EIO : saved_errno;
		return -1;
	}
	if (close(fd) < 0)
		return -1;

	return 0;
}

static long add_keyring(const char *description)
{
	return syscall(SYS_add_key, "keyring", description, NULL, 0,
		       KEY_SPEC_SESSION_KEYRING);
}

static int unlink_key(int key, int keyring)
{
	return syscall(SYS_keyctl, KEYCTL_UNLINK, key, keyring, 0, 0);
}

static int find_persistent_expiry(uid_t uid, char *expiry, size_t expiry_len)
{
	char description[32];
	char *line = NULL;
	size_t line_len = 0;
	ssize_t len;
	FILE *keys;
	int found = 0;

	snprintf(description, sizeof(description), "_persistent.%u", uid);
	keys = fopen("/proc/keys", "r");
	if (!keys)
		return -1;

	while ((len = getline(&line, &line_len, keys)) >= 0) {
		char *fields[9];
		char *saveptr;
		char *field;
		unsigned int n = 0;
		size_t description_len = strlen(description);

		for (field = strtok_r(line, " \t\n", &saveptr);
		     field && n < ARRAY_SIZE(fields);
		     field = strtok_r(NULL, " \t\n", &saveptr))
			fields[n++] = field;

		if (n == ARRAY_SIZE(fields) &&
		    strncmp(fields[8], description, description_len) == 0 &&
		    (fields[8][description_len] == '\0' ||
		     fields[8][description_len] == ':')) {
			snprintf(expiry, expiry_len, "%s", fields[3]);
			found = 1;
			break;
		}
	}

	free(line);
	fclose(keys);
	return found;
}

static void dump_persistent_keys(void)
{
	char *line = NULL;
	size_t line_len = 0;
	FILE *keys = fopen("/proc/keys", "r");

	if (!keys)
		return;

	while (getline(&line, &line_len, keys) >= 0) {
		if (strstr(line, "_persistent."))
			ksft_print_msg("visible key: %s", line);
	}

	free(line);
	fclose(keys);
}

static void set_failure(char *reason, size_t reason_len, const char *fmt, ...)
{
	va_list args;

	va_start(args, fmt);
	vsnprintf(reason, reason_len, fmt, args);
	va_end(args);
}

static void report_skip(const char *reason)
{
	ksft_test_result_skip("%s\n", reason);
	ksft_finished();
}

int main(void)
{
	char expiry[32] = {};
	char reason[256] = {};
	unsigned int saved_expiry;
	uid_t test_uid = getuid();
	int destination = -1;
	int cleanup_destination = -1;
	int linked_persistent = -1;
	int cleanup_persistent = -1;
	long ret;
	bool restore_expiry = false;
	bool passed = false;
	bool skipped = false;
	int get_persistent_errno;
	int attempt;
	int found;

	ksft_print_header();
	ksft_set_plan(1);

	if (geteuid() != 0)
		report_skip("requires root to set persistent_keyring_expiry");

	if (read_expiry(&saved_expiry) < 0)
		report_skip("CONFIG_PERSISTENT_KEYRINGS or procfs is unavailable");

	found = find_persistent_expiry(test_uid, expiry, sizeof(expiry));
	if (found < 0)
		report_skip("/proc/keys is unavailable");
	if (found) {
		test_uid = 50000 + (getpid() % 10000);
		for (attempt = 0; attempt < 128; attempt++) {
			found = find_persistent_expiry(test_uid, expiry,
						       sizeof(expiry));
			if (found < 0)
				report_skip("cannot read /proc/keys");
			if (!found)
				break;
			test_uid++;
		}
		if (attempt == 128)
			report_skip("could not find an unused persistent keyring UID");
	}

	/* The inherited session keyring may have been revoked. */
	ret = syscall(SYS_keyctl, KEYCTL_JOIN_SESSION_KEYRING, NULL, 0, 0, 0);
	if (ret < 0) {
		set_failure(reason, sizeof(reason),
			    "KEYCTL_JOIN_SESSION_KEYRING failed: %s",
			    strerror(errno));
		goto out;
	}

	if (write_expiry(TEST_EXPIRY) < 0) {
		if (write_expiry(saved_expiry) < 0)
			ksft_exit_fail_msg("failed to restore persistent keyring expiry: %s\n",
					   strerror(errno));
		report_skip("cannot change persistent_keyring_expiry");
	}
	restore_expiry = true;

	{
		char description[64];

		snprintf(description, sizeof(description), "keyring-expiry-test-%d",
			 getpid());
		ret = add_keyring(description);
	}
	if (ret < 0) {
		set_failure(reason, sizeof(reason), "add_key(keyring) failed: %s",
			    strerror(errno));
		goto out;
	}
	destination = ret;

	ret = syscall(SYS_keyctl, KEYCTL_RESTRICT_KEYRING, destination,
		      0, 0, 0);
	if (ret < 0) {
		set_failure(reason, sizeof(reason),
			    "KEYCTL_RESTRICT_KEYRING failed: %s", strerror(errno));
		goto out;
	}

	ret = syscall(SYS_keyctl, KEYCTL_GET_PERSISTENT, test_uid,
		      destination, 0, 0);
	get_persistent_errno = errno;
	ksft_print_msg("GET_PERSISTENT returned %ld (%s)\n", ret,
		       ret < 0 ? strerror(get_persistent_errno) : "success");
	if (ret >= 0) {
		linked_persistent = ret;
		set_failure(reason, sizeof(reason),
			    "GET_PERSISTENT unexpectedly linked key %ld", ret);
		goto out;
	}
	if (get_persistent_errno != EPERM) {
		set_failure(reason, sizeof(reason),
			    "GET_PERSISTENT failed with %s instead of EPERM",
			    strerror(get_persistent_errno));
		goto out;
	}

	ret = find_persistent_expiry(test_uid, expiry, sizeof(expiry));
	if (ret < 0) {
		set_failure(reason, sizeof(reason), "cannot read /proc/keys");
		goto out;
	}
	if (!ret) {
		dump_persistent_keys();
		set_failure(reason, sizeof(reason),
			    "GET_PERSISTENT did not create the requested keyring");
		skipped = true;
		goto out;
	}
	if (!strcmp(expiry, "perm") || !strcmp(expiry, "expd")) {
		set_failure(reason, sizeof(reason),
			    "failed link left _persistent.%u with expiry %s",
			    test_uid, expiry);
		{
			char description[64];

			snprintf(description, sizeof(description),
				 "keyring-expiry-cleanup-%d", getpid());
			ret = add_keyring(description);
		}
		if (ret >= 0) {
			cleanup_destination = ret;
			ret = syscall(SYS_keyctl, KEYCTL_GET_PERSISTENT, test_uid,
				      cleanup_destination, 0, 0);
			if (ret >= 0) {
				cleanup_persistent = ret;
				if (unlink_key(cleanup_persistent,
					       cleanup_destination) < 0) {
					ksft_print_msg("warning: unlink temporary key: %s\n",
						       strerror(errno));
				} else {
					cleanup_persistent = -1;
				}
			} else {
				ksft_print_msg("warning: expiry cleanup failed: %s\n",
					       strerror(errno));
			}
		} else {
			ksft_print_msg("warning: unable to create cleanup keyring: %s\n",
				       strerror(errno));
		}
		goto out;
	}

	passed = true;

out:
	if (linked_persistent > 0 &&
	    unlink_key(linked_persistent, destination) < 0) {
		ksft_print_msg("warning: unable to unlink persistent key from test keyring: %s\n",
			       strerror(errno));
		if (passed)
			set_failure(reason, sizeof(reason),
				    "unable to clean up linked persistent key: %s",
				    strerror(errno));
		passed = false;
	}
	if (cleanup_persistent > 0 && cleanup_destination > 0 &&
	    unlink_key(cleanup_persistent, cleanup_destination) < 0) {
		ksft_print_msg("warning: unable to unlink temporary persistent key: %s\n",
			       strerror(errno));
	}
	if (cleanup_destination > 0 &&
	    unlink_key(cleanup_destination, KEY_SPEC_SESSION_KEYRING) < 0) {
		ksft_print_msg("warning: unable to unlink cleanup keyring: %s\n",
			       strerror(errno));
		if (passed)
			set_failure(reason, sizeof(reason),
				    "unable to clean up temporary keyring: %s",
				    strerror(errno));
		passed = false;
	}
	if (destination > 0 &&
	    unlink_key(destination, KEY_SPEC_SESSION_KEYRING) < 0) {
		ksft_print_msg("warning: unable to unlink test keyring: %s\n",
			       strerror(errno));
		if (passed)
			set_failure(reason, sizeof(reason),
				    "unable to clean up test keyring: %s",
				    strerror(errno));
		passed = false;
	}
	if (restore_expiry && write_expiry(saved_expiry) < 0) {
		set_failure(reason, sizeof(reason),
			    "failed to restore persistent_keyring_expiry: %s",
			    strerror(errno));
		passed = false;
	}

	if (passed) {
		ksft_print_msg("restricted link returned EPERM; _persistent.%u expires in %s\n",
			       test_uid, expiry);
		ksft_test_result_pass("failed link still applies persistent keyring expiry\n");
	} else if (skipped) {
		ksft_test_result_skip("%s\n", reason);
	} else {
		ksft_test_result_fail("%s\n", reason);
	}
	ksft_finished();
}

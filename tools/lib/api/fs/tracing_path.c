// SPDX-License-Identifier: GPL-2.0
#ifndef _GNU_SOURCE
# define _GNU_SOURCE
#endif

#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <linux/string.h>
#include <errno.h>
#include <unistd.h>
#include "fs.h"

#include "tracing_path.h"

static char tracing_path[PATH_MAX]        = "/sys/kernel/tracing";

static void __tracing_path_set(const char *tracing, const char *mountpoint)
{
	snprintf(tracing_path, sizeof(tracing_path), "%s/%s",
		 mountpoint, tracing);
}

static const char *tracing_path_tracefs_mount(void)
{
	const char *mnt;

	mnt = tracefs__mount();
	if (!mnt)
		return NULL;

	__tracing_path_set("", mnt);

	return tracing_path;
}

static const char *tracing_path_debugfs_mount(void)
{
	const char *mnt;

	mnt = debugfs__mount();
	if (!mnt)
		return NULL;

	__tracing_path_set("tracing/", mnt);

	return tracing_path;
}

const char *tracing_path_mount(void)
{
	const char *mnt;

	mnt = tracing_path_tracefs_mount();
	if (mnt)
		return mnt;

	mnt = tracing_path_debugfs_mount();

	return mnt;
}

void tracing_path_set(const char *mntpt)
{
	__tracing_path_set("tracing/", mntpt);
}

char *get_tracing_file(const char *name)
{
	char *file;

	if (asprintf(&file, "%s%s", tracing_path_mount(), name) < 0)
		return NULL;

	return file;
}

void put_tracing_file(char *file)
{
	free(file);
}

char *get_events_dir(void)
{
	const char *eventfs;
	char *path;
	int saved_errno;

	path = get_tracing_file("events");
	if (path && faccessat(AT_FDCWD, path, R_OK, AT_EACCESS) == 0)
		return path;

	saved_errno = errno;
	put_tracing_file(path);

	eventfs = eventfs__mount();
	if (eventfs && faccessat(AT_FDCWD, eventfs, R_OK, AT_EACCESS) == 0 &&
	    eventfs__configured())
		return strdup(eventfs);

	/*
	 * Prefer EACCES over other errors: it tells the user that events
	 * exist but are not accessible, which is more actionable than
	 * ENOENT from a missing filesystem.
	 */
	if (errno != EACCES)
		errno = saved_errno;
	return NULL;
}

char *get_events_file(const char *name)
{
	char *dir, *file;

	dir = get_events_dir();
	if (!dir)
		return NULL;

	if (asprintf(&file, "%s/%s", dir, name) < 0)
		file = NULL;

	free(dir);
	return file;
}

void put_events_file(char *file)
{
	free(file);
}

DIR *tracing_events__opendir(void)
{
	char *path = get_events_dir();
	DIR *dir = NULL;

	if (path) {
		dir = opendir(path);
		put_events_file(path);
	}

	return dir;
}

int tracing_events__scandir_alphasort(struct dirent ***namelist)
{
	char *path = get_events_dir();
	int ret;

	if (!path) {
		*namelist = NULL;
		return 0;
	}

	ret = scandir(path, namelist, NULL, alphasort);
	put_events_file(path);

	if (ret < 0)
		*namelist = NULL;

	return ret;
}

int tracing_path__strerror_open_tp(int err, char *buf, size_t size,
				   const char *sys, const char *name)
{
	char sbuf[128];
	char filename[PATH_MAX];

	snprintf(filename, PATH_MAX, "%s/%s", sys, name ?: "*");

	switch (err) {
	case ENOENT:
		/*
		 * We will get here if we can't find the tracepoint, but one of
		 * debugfs or tracefs is configured, which means you probably
		 * want some tracepoint which wasn't compiled in your kernel.
		 * - jirka
		 */
		if (debugfs__configured() || tracefs__configured()) {
			/* sdt markers */
			if (!strncmp(filename, "sdt_", 4)) {
				snprintf(buf, size,
					"Error:\tFile %s/events/%s not found.\n"
					"Hint:\tSDT event cannot be directly recorded on.\n"
					"\tPlease first use 'perf probe %s:%s' before recording it.\n",
					tracing_path, filename, sys, name);
			} else {
				snprintf(buf, size,
					 "Error:\tFile %s/events/%s not found.\n"
					 "Hint:\tPerhaps this kernel misses some CONFIG_ setting to enable this feature?.\n",
					 tracing_path, filename);
			}
			break;
		}
		snprintf(buf, size, "%s",
			 "Error:\tUnable to find debugfs/tracefs\n"
			 "Hint:\tWas your kernel compiled with debugfs/tracefs support?\n"
			 "Hint:\tIs the debugfs/tracefs filesystem mounted?\n"
			 "Hint:\tTry 'sudo mount -t debugfs nodev /sys/kernel/debug'");
		break;
	case EACCES:
		snprintf(buf, size,
			 "Error:\tNo permissions to read %s/events/%s\n"
			 "Hint:\tTry 'sudo mount -o remount,mode=755 %s'\n"
			 "Hint:\tOr check if /sys/kernel/events is available\n",
			 tracing_path, filename, tracing_path_mount());
		break;
	default:
		snprintf(buf, size, "%s", str_error_r(err, sbuf, sizeof(sbuf)));
		break;
	}

	return 0;
}

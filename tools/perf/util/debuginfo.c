// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * DWARF debug information handling code.  Copied from probe-finder.c.
 *
 * Written by Masami Hiramatsu <mhiramat@redhat.com>
 */

#include <errno.h>
#include <fcntl.h>
#include <dirent.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>
#include <linux/list.h>
#include <linux/zalloc.h>
#include <api/fs/fs.h>

#include "build-id.h"
#include "dso.h"
#include "debug.h"
#include "debuginfo.h"
#include "mutex.h"
#include "symbol.h"
#include "term.h"

#ifdef HAVE_DEBUGINFOD_SUPPORT
#include <elfutils/debuginfod.h>
#endif

/* Dwarf FL wrappers */
static char *debuginfo_path;	/* Currently dummy */

static const Dwfl_Callbacks offline_callbacks = {
	.find_debuginfo = dwfl_standard_find_debuginfo,
	.debuginfo_path = &debuginfo_path,

	.section_address = dwfl_offline_section_address,

	/* We use this table for core files too.  */
	.find_elf = dwfl_build_id_find_elf,
};

/* Get a Dwarf from offline image */
static int debuginfo__init_offline_dwarf(struct debuginfo *dbg,
					 const char *path)
{
	GElf_Addr dummy;
	int fd;
	bool fd_consumed = false;

	fd = open(path, O_RDONLY);
	if (fd < 0)
		return fd;

	dbg->dwfl = dwfl_begin(&offline_callbacks);
	if (!dbg->dwfl)
		goto error;

	dwfl_report_begin(dbg->dwfl);
	dbg->mod = dwfl_report_offline(dbg->dwfl, "", "", fd);
	if (!dbg->mod)
		goto error;
	fd_consumed = true;

	dbg->dbg = dwfl_module_getdwarf(dbg->mod, &dbg->bias);
	if (!dbg->dbg)
		goto error;

	dwfl_module_build_id(dbg->mod, &dbg->build_id, &dummy);

	if (dwfl_report_end(dbg->dwfl, NULL, NULL) != 0)
		goto error;

	return 0;
error:
	if (dbg->dwfl)
		dwfl_end(dbg->dwfl);
	if (!fd_consumed)
		close(fd);
	memset(dbg, 0, sizeof(*dbg));

	return -ENOENT;
}

static struct debuginfo *__debuginfo__new(const char *path)
{
	struct debuginfo *dbg = zalloc(sizeof(*dbg));
	if (!dbg)
		return NULL;

	if (debuginfo__init_offline_dwarf(dbg, path) < 0)
		zfree(&dbg);
	if (dbg)
		pr_debug("Open Debuginfo file: %s\n", path);
	return dbg;
}

struct debuginfo *debuginfo__new(const char *path)
{
	static const enum dso_binary_type distro_dwarf_types[] = {
		DSO_BINARY_TYPE__FEDORA_DEBUGINFO,
		DSO_BINARY_TYPE__UBUNTU_DEBUGINFO,
		DSO_BINARY_TYPE__OPENEMBEDDED_DEBUGINFO,
		DSO_BINARY_TYPE__BUILDID_DEBUGINFO,
		DSO_BINARY_TYPE__MIXEDUP_UBUNTU_DEBUGINFO,
		DSO_BINARY_TYPE__NOT_FOUND,
	};
	const enum dso_binary_type *type;
	char buf[PATH_MAX], nil = '\0';
	struct dso *dso;
	struct debuginfo *dinfo = NULL;
	struct build_id bid = { .size = 0};

	/* Try to open distro debuginfo files */
	dso = dso__new(path);
	if (!dso)
		goto out;

	/*
	 * Set the build id for DSO_BINARY_TYPE__BUILDID_DEBUGINFO. Don't block
	 * incase the path isn't for a regular file.
	 */
	assert(!dso__has_build_id(dso));
	if (filename__read_build_id(path, &bid) > 0)
		dso__set_build_id(dso, &bid);

	for (type = distro_dwarf_types;
	     !dinfo && *type != DSO_BINARY_TYPE__NOT_FOUND;
	     type++) {
		if (dso__read_binary_type_filename(dso, *type, &nil,
						   buf, PATH_MAX) < 0)
			continue;
		dinfo = __debuginfo__new(buf);
	}
	dso__put(dso);

out:
	if (dinfo)
		return dinfo;

	/* if failed to open all distro debuginfo, open given binary */
	symbol__join_symfs(buf, path);
	return __debuginfo__new(buf);
}

#ifdef HAVE_DEBUGINFOD_SUPPORT
/*
 * Set with the use_browser variable in ui/ui.h, not included here to
 * avoid pulling in the UI headers: when the TUI is in use, printing to
 * stderr would garble its display.
 */
extern int use_browser;

static bool debuginfod_progress_started;
static bool debuginfod_fetch_cancelled;

/*
 * A fetch can be interrupted with Ctrl-C/SIGTERM while stdin is in raw
 * mode: the handler only records the signal, the progress callback
 * aborts the query, and debuginfo__find_build_id() restores the
 * terminal and raises the signal again, so that the terminal is never
 * left in raw mode when perf dies mid-fetch.
 */
static volatile sig_atomic_t debuginfod_signal;

static void debuginfod_signal_handler(int sig)
{
	debuginfod_signal = sig;
}

/*
 * 's': skip this fetch, and remember the build ID so that the rest of
 * the session doesn't ask for it again, the query is aborted by
 * returning a non-zero value from the progress callback, as the
 * debuginfod client docs prescribe.  'd': also disable debuginfod for
 * the rest of the session, telling how to make that permanent:
 * rewriting the user's ~/.perfconfig from here would drop its comments,
 * so point at 'perf config' instead.
 */
static void debuginfod__poll_cancel_keys(void)
{
	char ch;

	while (read(STDIN_FILENO, &ch, 1) == 1) {
		if (ch == 's' || ch == 'S') {
			debuginfod_fetch_cancelled = true;
			fputs("\nSkipping this debuginfod fetch, this build ID will not be fetched again in this session, press 'd' to also disable it for the other ones\n", stderr);
		} else if (ch == 'd' || ch == 'D') {
			debuginfod_fetch_cancelled = true;
			symbol_conf.debuginfod = false;
			fputs("\nSkipping this debuginfod fetch and disabling debuginfod for this session, run 'perf config core.debuginfod=false' to also disable it permanently\n", stderr);
		}
	}
}

/*
 * Print a warning and a progress indicator when the debuginfod client
 * ends up fetching a file, which can be big, such as the vmlinux for a
 * kernel profiled on another machine or before it got upgraded, so that
 * users know perf is not stuck, and let them bail out: 's' skips this
 * fetch and remembers the build ID, so that the rest of the session
 * doesn't ask for it again, 'd' also disables debuginfod for the rest of
 * the session.  The client only invokes this once it committed to a
 * server, so 'a' is the number of bytes fetched so far, 'b' the total
 * size when the server tells it, -1 otherwise.
 */
static int debuginfod_progress_fn(debuginfod_client *c __maybe_unused,
				  long a, long b)
{
	if (!isatty(STDERR_FILENO) || use_browser)
		return 0;

	if (debuginfod_signal)
		return 1;

	if (isatty(STDIN_FILENO)) {
		debuginfod__poll_cancel_keys();
		if (debuginfod_fetch_cancelled)
			return 1;
	}

	if (!debuginfod_progress_started) {
		fprintf(stderr, "Fetching debuginfo by build ID from the debuginfod servers, this may take a while for large files such as the vmlinux, press 's' to skip, 'd' to skip and disable\n");
		debuginfod_progress_started = true;
	}

	if (a >= 0) {
		if (b > 0)
			fprintf(stderr, "  %ld/%ld MiB fetched\r", a >> 20, b >> 20);
		else
			fprintf(stderr, "  %ld MiB fetched\r", a >> 20);
	}

	return 0;
}

/*
 * The debuginfod client checks its local cache only as part of the
 * server query flow, so with no servers configured it fails even when
 * the artifact is in the client cache.  Distro setup scripts, e.g.
 * /etc/profile.d/99-debuginfod.sh, export DEBUGINFOD_URLS from the
 * .urls files in /etc/debuginfod, but that doesn't reach environments
 * that don't source the profile scripts, such as cron jobs, systemd
 * services and CI, so do it here when the variable isn't set.  An
 * explicitly empty DEBUGINFOD_URLS is an opt-out, matching the
 * perf_debuginfod_setup() handling, and is left alone.
 *
 * setenv() is not thread safe and this is on the fetch path, that
 * dso__debuginfo() takes outside dso__lock, so do it just once, from
 * whichever fetch gets here first: the value is the same for all of them.
 */
static void debuginfod__urls_env_setup(void)
{
	char *urls = NULL;
	DIR *dir;
	struct dirent *dent;

	if (getenv("DEBUGINFOD_URLS") != NULL)
		return;

	dir = opendir("/etc/debuginfod");
	if (dir == NULL)
		return;

	while ((dent = readdir(dir)) != NULL) {
		char *content = NULL;
		char *new_urls;
		char path[PATH_MAX];
		size_t len = strlen(dent->d_name), i, size;
		int n;

		if (len < 5 || strcmp(dent->d_name + len - 5, ".urls"))
			continue;

		snprintf(path, sizeof(path), "/etc/debuginfod/%s", dent->d_name);
		if (filename__read_str(path, &content, &size) < 0)
			continue;

		for (i = 0; i < size; i++)
			if (content[i] == '\n' || content[i] == '\r')
				content[i] = ' ';

		if (urls == NULL) {
			urls = strdup(content);
		} else {
			n = asprintf(&new_urls, "%s %s", urls, content);
			if (n < 0) {
				free(content);
				continue;
			}
			free(urls);
			urls = new_urls;
		}
		free(content);
	}
	closedir(dir);

	if (urls != NULL) {
		setenv("DEBUGINFOD_URLS", urls, 1);
		pr_debug("Set DEBUGINFOD_URLS from /etc/debuginfod: %s\n", urls);
	}
	free(urls);
}

static void debuginfod__setup_urls_env(void)
{
	static pthread_once_t once = PTHREAD_ONCE_INIT;

	pthread_once(&once, debuginfod__urls_env_setup);
}

/*
 * Users can disable the local build-id/.debug cache by setting
 * buildid.dir to /dev/null, meaning they don't want fetched
 * binaries/debuginfo stored on the box; the debuginfod client keeps
 * its own cache in ~/.cache/debuginfod_client, so honour that intent
 * and don't fetch at all in that case.
 */
static bool debuginfod__cache_disabled(void)
{
	return !strcmp(buildid_dir, "/dev/null");
}

/*
 * Build IDs that shouldn't be searched for again in this session: the
 * ones already searched for on the debuginfod servers without success,
 * so that callers that see the same DSO over and over, such as the data
 * type profiler switching between DSOs on every hist entry, don't pay a
 * server round trip again for each miss, and the ones whose search the
 * user cancelled, maybe because what was being downloaded is too big,
 * so that the next request for the same build ID doesn't restart a
 * download that was refused.  The cache of successes is the debuginfod
 * client's own, in the local filesystem.
 */
struct debuginfod_miss {
	struct list_head	node;
	struct build_id		bid;
	bool			cancelled;
};

static LIST_HEAD(debuginfod__misses);
static struct mutex debuginfod__missed_lock;

static void debuginfod__missed_lock_setup(void)
{
	mutex_init(&debuginfod__missed_lock);
}

static void debuginfod__missed_lock_init(void)
{
	static pthread_once_t once = PTHREAD_ONCE_INIT;

	pthread_once(&once, debuginfod__missed_lock_setup);
}

/*
 * Was the search for this build ID already settled, by the servers
 * having nothing or by the user cancelling it?  When it was, @cancelled
 * tells the two apart, so that the debug message can say which one it
 * was.
 */
static bool debuginfod__missed(const struct build_id *bid, bool *cancelled)
{
	struct debuginfod_miss *miss;
	bool found = false;

	*cancelled = false;

	debuginfod__missed_lock_init();
	mutex_lock(&debuginfod__missed_lock);
	list_for_each_entry(miss, &debuginfod__misses, node) {
		if (miss->bid.size == bid->size &&
		    !memcmp(miss->bid.data, bid->data, bid->size)) {
			found = true;
			*cancelled = miss->cancelled;
			break;
		}
	}
	mutex_unlock(&debuginfod__missed_lock);

	return found;
}

static void debuginfod__miss_add(const struct build_id *bid, bool cancelled)
{
	struct debuginfod_miss *miss = zalloc(sizeof(*miss));

	if (miss == NULL)
		return;

	miss->bid = *bid;
	miss->cancelled = cancelled;

	debuginfod__missed_lock_init();
	mutex_lock(&debuginfod__missed_lock);
	list_add(&miss->node, &debuginfod__misses);
	mutex_unlock(&debuginfod__missed_lock);
}

/*
 * One fetch at a time.
 *
 * The terminal settings, the signal dispositions and the progress and
 * cancellation state below are process global, so two concurrent fetches,
 * which dso__debuginfo() makes possible by taking the fetch out of
 * dso__lock, would fight over them: the second one would take the first
 * one's raw mode as the state to restore and leave the terminal broken when
 * it is done, and resetting the cancellation state would drop the 's'/'d'
 * keypress that was meant for the fetch already in progress.  Serializing
 * also keeps the two from racing for the same keypresses and for the same
 * progress line, and a second fetch has nothing to gain from running in
 * parallel with a first one reading the same kind of file off the same
 * servers.
 *
 * What that costs is that a fetch for one build ID blocks a fetch for
 * another one, and it is what parallel downloads would fix: the terminal
 * in raw mode, the signal dispositions, the progress line and the 's'/'d'
 * keys would have to become per fetch and refcounted, so that N fetches
 * share one terminal session and one signal handler, with the first one in
 * setting them up and the last one out putting them back.  Worth doing
 * only if the wait turns out to be long, because it mostly is not: the
 * lookups below answer the second and later requests for a build ID from
 * memory, so after the first pass over the build IDs of a workload, which
 * is the only time anything is fetched at all, the serialization has
 * nothing left to serialize.  Start there if a profile with many DSOs to
 * fetch shows up in a profile of perf itself.
 */
static struct mutex debuginfod__fetch_lock;

static void debuginfod__fetch_lock_setup(void)
{
	mutex_init(&debuginfod__fetch_lock);
}

static void debuginfod__fetch_lock_init(void)
{
	static pthread_once_t once = PTHREAD_ONCE_INIT;

	pthread_once(&once, debuginfod__fetch_lock_setup);
}

/*
 * The lookups that are in progress, and the ones that brought a file back,
 * guarded by debuginfod__fetch_lock, which is also the mutex the waiters
 * below sleep on.
 *
 * A second thread that needs a build ID that is already being fetched waits
 * for that fetch instead of starting another one: while the first one is
 * still running, the file is not in the debuginfod client cache yet, so the
 * second one would be a second download of the same file, with a second
 * client, a second progress line and a second turn at putting the terminal
 * in raw mode, for the same answer.
 *
 * An entry that brought a file back stays, as the answer for whoever needs
 * the same build ID later, with no client at all: the DSOs in a profile get
 * asked for over and over, dso__debuginfo() is called per symbol annotated,
 * and with the path in hand the answer is a strdup().  The file stays in
 * the debuginfod client cache, so the path stays valid, and like
 * debuginfod__misses this grows with the number of build IDs in the
 * workload, one small entry each, and is not trimmed.
 *
 * An entry that didn't bring a file back is dropped as soon as whoever was
 * waiting for it is woken: there is nothing left to share, and the fetch
 * already recorded it in debuginfod__misses, either as a miss or as a user
 * cancellation, so the rest of the session doesn't ask for it again.
 */
struct debuginfo_lookup {
	struct list_head	 node;
	struct build_id		 bid;
	struct cond		 done;
	int			 err;		/* 0: 'path' is the file, -1: not available */
	char			*path;
	int			 nr_waiters;
	bool			 fetching;
};

static LIST_HEAD(debuginfo_lookups);

static bool build_id__equal(const struct build_id *a, const struct build_id *b)
{
	return a->size == b->size && memcmp(a->data, b->data, a->size) == 0;
}

static struct debuginfo_lookup *debuginfo_lookup__find(const struct build_id *bid)
{
	struct debuginfo_lookup *lookup;

	list_for_each_entry(lookup, &debuginfo_lookups, node) {
		if (build_id__equal(&lookup->bid, bid))
			return lookup;
	}

	return NULL;
}

static void debuginfo_lookup__delete(struct debuginfo_lookup *lookup)
{
	list_del(&lookup->node);
	cond_destroy(&lookup->done);
	zfree(&lookup->path);
	free(lookup);
}

/*
 * A lookup entry in progress, added to the shared list so that a second
 * request for the same build ID waits for this one instead of fetching it
 * again.  Called, and the result used, with debuginfod__fetch_lock held.
 * Out of memory just means not sharing this one, the fetch is the same
 * without the entry.
 */
static struct debuginfo_lookup *debuginfo_lookup__new(const struct build_id *bid)
{
	struct debuginfo_lookup *lookup = zalloc(sizeof(*lookup));

	if (lookup != NULL) {
		lookup->bid	 = *bid;
		lookup->err	 = -1;
		lookup->fetching = true;
		cond_init(&lookup->done);
		list_add(&lookup->node, &debuginfo_lookups);
	}

	return lookup;
}

/*
 * The fetch itself, the terminal in raw mode and the signal dispositions
 * swapped for the ones that restore it, so that the caller has to hold
 * debuginfod__fetch_lock for the whole of it, see the comment there.
 */
static int debuginfod__fetch(const struct build_id *bid, char **path)
{
	char sbuild_id[SBUILD_ID_SIZE];
	struct termios orig_termios;
	struct sigaction sa, orig_sigint, orig_sigterm;
	bool term_set = false, sigint_set = false, sigterm_set = false;
	debuginfod_client *c;
	int fd;

	debuginfod__setup_urls_env();

	c = debuginfod_begin();
	if (c == NULL)
		return -1;

	debuginfod_set_progressfn(c, debuginfod_progress_fn);

	debuginfod_fetch_cancelled = false;
	debuginfod_signal = 0;

	/*
	 * Make stdin deliver keypresses without waiting for a newline,
	 * the progress callback above polls it for the 's'/'d' keys,
	 * only in the stdio case with both stdin and stderr being a
	 * terminal, the TUI/pipe cases have no business being poked
	 * here.  Intercept SIGINT/SIGTERM so that the terminal is
	 * restored before the process dies, the handler only records
	 * the signal and the callback aborts the query.
	 */
	if (isatty(STDIN_FILENO) && isatty(STDERR_FILENO) && !use_browser) {
		set_term_quiet_input(&orig_termios);
		term_set = true;

		memset(&sa, 0, sizeof(sa));
		sa.sa_handler = debuginfod_signal_handler;
		sigemptyset(&sa.sa_mask);
		if (sigaction(SIGINT, &sa, &orig_sigint) == 0)
			sigint_set = true;
		if (sigaction(SIGTERM, &sa, &orig_sigterm) == 0)
			sigterm_set = true;
	}

	fd = debuginfod_find_debuginfo(c, bid->data, bid->size, path);

	if (term_set)
		tcsetattr(STDIN_FILENO, TCSANOW, &orig_termios);
	if (sigint_set)
		sigaction(SIGINT, &orig_sigint, NULL);
	if (sigterm_set)
		sigaction(SIGTERM, &orig_sigterm, NULL);

	debuginfod_end(c);
	if (debuginfod_progress_started) {
		fputc('\n', stderr);
		debuginfod_progress_started = false;
	}
	if (fd < 0) {
		build_id__snprintf(bid, sbuild_id, sizeof(sbuild_id));
		if (debuginfod_fetch_cancelled || debuginfod_signal) {
			pr_debug("debuginfod search for build ID %s cancelled by the user\n",
				 sbuild_id);
			/*
			 * Remember it so that the rest of the session doesn't
			 * ask for the same file again: the user may have
			 * skipped it for being too big.
			 */
			debuginfod__miss_add(bid, true);
			/*
			 * The terminal is restored, die as the user asked;
			 * the original dispositions are back in place.
			 */
			if (debuginfod_signal)
				raise(debuginfod_signal);
			return -1;
		}
		pr_debug("No debuginfo found for build ID %s in debuginfod\n",
			 sbuild_id);
		debuginfod__miss_add(bid, false);
		return -1;
	}

	close(fd);

	/*
	 * The interrupt can land after the file is already here, in which
	 * case there is no failure to report, but the user still asked for
	 * perf to stop, and the terminal and the signal dispositions are
	 * back to what they were, so honour it here as well instead of
	 * swallowing it and going on.
	 */
	if (debuginfod_signal) {
		build_id__snprintf(bid, sbuild_id, sizeof(sbuild_id));
		pr_debug("debuginfod found the debuginfo for build ID %s, but the search was interrupted, exiting\n",
			 sbuild_id);
		raise(debuginfod_signal);
	}

	return 0;
}

/*
 * Look the build ID up, sharing the fetch with whoever else needs it, see
 * the comment on struct debuginfo_lookup.  Called, and left, with
 * debuginfod__fetch_lock held.
 */
static int debuginfo_lookup__find_build_id(const struct build_id *bid, char **path)
{
	struct debuginfo_lookup *lookup = debuginfo_lookup__find(bid);
	bool waited = false;
	int err;

	if (lookup == NULL) {
		lookup = debuginfo_lookup__new(bid);
		if (lookup == NULL)
			return debuginfod__fetch(bid, path);

		err = debuginfod__fetch(bid, path);

		/*
		 * Publish it: whoever is waiting for this build ID gets the
		 * answer this fetch settled, and, when it brought a file
		 * back, so does whoever needs the same build ID later.
		 */
		lookup->fetching = false;
		lookup->err = err;
		if (err == 0)
			lookup->path = strdup(*path);

		cond_broadcast(&lookup->done);

		if (lookup->path == NULL && lookup->nr_waiters == 0)
			debuginfo_lookup__delete(lookup);

		return err;
	}

	/*
	 * Somebody else got here first: wait for the fetch that is in
	 * progress instead of starting another one, which, while that one
	 * is still running, would download the same file a second time.
	 */
	if (lookup->fetching) {
		lookup->nr_waiters++;
		waited = true;

		while (lookup->fetching)
			cond_wait(&lookup->done, &debuginfod__fetch_lock);
	}

	if (lookup->path != NULL) {
		/*
		 * The file stays in the debuginfod client cache, but that
		 * cache can be cleaned from under us, so check that it is
		 * still there before handing its path out.  If it isn't,
		 * forget the path and fetch it again, publishing the new
		 * answer the same way the first fetch does, so that the
		 * next lookup shares it instead of fetching it a third
		 * time.
		 */
		if (access(lookup->path, R_OK) == 0) {
			*path = strdup(lookup->path);
			err = *path != NULL ? 0 : -1;
		} else {
			zfree(&lookup->path);
			err = debuginfod__fetch(bid, path);
			lookup->err = err;
			if (err == 0)
				lookup->path = strdup(*path);
		}
	} else if (lookup->err == 0) {
		/*
		 * No path and no error: the path was fetched but could
		 * not be remembered, look for the file like a caller
		 * with no entry would, and publish the answer as above
		 * so that a success always comes with a path.
		 */
		err = debuginfod__fetch(bid, path);
		lookup->err = err;
		if (err == 0)
			lookup->path = strdup(*path);
	} else {
		/*
		 * Nothing came back and there is nothing to retry: the fetch
		 * was cancelled or interrupted by the user, or found nothing
		 * and said so on the misses list.
		 */
		err = lookup->err;
	}

	if (waited)
		lookup->nr_waiters--;

	/* The last one out drops an entry there is nothing to share. */
	if (lookup->nr_waiters == 0 && lookup->path == NULL)
		debuginfo_lookup__delete(lookup);

	return err;
}

/*
 * Find a debuginfo file keyed by the build ID, using the debuginfod
 * client, which checks its local cache first and then queries the
 * servers in DEBUGINFOD_URLS.  Used when the debuginfo is not available
 * locally under the name the DSO was opened with, for instance the
 * vmlinux for the kernel the profile was recorded on, when processing
 * the profile on another machine or after the kernel or its debuginfo
 * package got upgraded in between.
 *
 * Querying servers, possibly third party, sends the build IDs of the
 * binaries being analysed off the box, so this is opt-out: on by
 * default, switchable off with --no-debuginfod, with
 * core.debuginfod=false (what the 'd' key writes), with the per-tool
 * report.debuginfod/top.debuginfod, and it is off too when the user
 * disabled the local build-id/.debug cache, e.g. with
 * buildid.dir = /dev/null, as is the case for users that don't want
 * any of this stored locally.
 *
 * On success the path is stored in *@path and must be freed by the
 * caller, the file remains available in the debuginfod client cache.
 */
int debuginfo__find_build_id(const struct build_id *bid, char **path)
{
	int err = -1;

	*path = NULL;

	if (!build_id__is_defined(bid))
		return -1;

	/*
	 * The checks below have to be made with the lock held, as they look
	 * at the state the fetch changes: debuginfod can be turned off while
	 * a fetch is in progress, by the 'd' key in its progress line, and a
	 * build ID the fetch in progress just settled, as a miss or as a
	 * cancellation, is settled for whoever is waiting for the lock as
	 * well.  Deciding here and fetching there would repeat a fetch that
	 * was already made, and put the same build ID on the misses list
	 * twice.
	 */
	debuginfod__fetch_lock_init();
	mutex_lock(&debuginfod__fetch_lock);

	if (symbol_conf.debuginfod) {
		bool cancelled;

		if (debuginfod__cache_disabled()) {
			pr_debug("Build-id cache disabled (buildid dir is '%s'), not using debuginfod\n",
				 buildid_dir);
		} else if (debuginfod__missed(bid, &cancelled)) {
			char sbuild_id[SBUILD_ID_SIZE];

			build_id__snprintf(bid, sbuild_id, sizeof(sbuild_id));
			pr_debug("Not searching build ID %s in debuginfod again, %s\n",
				 sbuild_id,
				 cancelled ? "the user cancelled the search earlier" :
					     "it was a miss earlier");
		} else {
			err = debuginfo_lookup__find_build_id(bid, path);
		}
	}

	mutex_unlock(&debuginfod__fetch_lock);

	return err;
}

struct debuginfo *debuginfo__new_build_id(const struct build_id *bid)
{
	char sbuild_id[SBUILD_ID_SIZE];
	char *path = NULL;
	struct debuginfo *dbg;

	if (debuginfo__find_build_id(bid, &path))
		return NULL;

	dbg = __debuginfo__new(path);
	if (dbg == NULL) {
		build_id__snprintf(bid, sbuild_id, sizeof(sbuild_id));
		pr_debug("Failed to open DWARF in debuginfo fetched for build ID %s: %s\n",
			 sbuild_id, path);
	}
	free(path);
	return dbg;
}
#endif /* HAVE_DEBUGINFOD_SUPPORT */

void debuginfo__delete(struct debuginfo *dbg)
{
	if (dbg) {
		if (dbg->dwfl)
			dwfl_end(dbg->dwfl);
		free(dbg);
	}
}

/* For the kernel module, we need a special code to get a DIE */
int debuginfo__get_text_offset(struct debuginfo *dbg, Dwarf_Addr *offs,
				bool adjust_offset)
{
	int n, i;
	Elf32_Word shndx;
	Elf_Scn *scn;
	Elf *elf;
	GElf_Shdr mem, *shdr;
	const char *p;

	elf = dwfl_module_getelf(dbg->mod, &dbg->bias);
	if (!elf)
		return -EINVAL;

	/* Get the number of relocations */
	n = dwfl_module_relocations(dbg->mod);
	if (n < 0)
		return -ENOENT;
	/* Search the relocation related .text section */
	for (i = 0; i < n; i++) {
		p = dwfl_module_relocation_info(dbg->mod, i, &shndx);
		if (p && strcmp(p, ".text") == 0) {
			/* OK, get the section header */
			scn = elf_getscn(elf, shndx);
			if (!scn)
				return -ENOENT;
			shdr = gelf_getshdr(scn, &mem);
			if (!shdr)
				return -ENOENT;
			*offs = shdr->sh_addr;
			if (adjust_offset)
				*offs -= shdr->sh_offset;
		}
	}
	return 0;
}

#ifdef HAVE_DEBUGINFOD_SUPPORT
int get_source_from_debuginfod(const char *raw_path,
			       const char *sbuild_id, char **new_path)
{
	debuginfod_client *c = debuginfod_begin();
	const char *p = raw_path;
	int fd;

	if (!c)
		return -ENOMEM;

	fd = debuginfod_find_source(c, (const unsigned char *)sbuild_id,
				0, p, new_path);
	pr_debug("Search %s from debuginfod -> %d\n", p, fd);
	if (fd >= 0)
		close(fd);
	debuginfod_end(c);
	if (fd < 0) {
		pr_debug("Failed to find %s in debuginfod (%s)\n",
			raw_path, sbuild_id);
		return -ENOENT;
	}
	pr_debug("Got a source %s\n", *new_path);

	return 0;
}
#endif /* HAVE_DEBUGINFOD_SUPPORT */

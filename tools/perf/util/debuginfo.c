// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * DWARF debug information handling code.  Copied from probe-finder.c.
 *
 * Written by Masami Hiramatsu <mhiramat@redhat.com>
 */

#include <errno.h>
#include <fcntl.h>
#include <dirent.h>
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
 * 's': skip this fetch, the query is aborted by returning a non-zero
 * value from the progress callback, as the debuginfod client docs
 * prescribe.  'd': also disable debuginfod for the rest of the session,
 * telling how to make that permanent: rewriting the user's ~/.perfconfig
 * from here would drop its comments, so point at 'perf config' instead.
 */
static void debuginfod__poll_cancel_keys(void)
{
	char ch;

	while (read(STDIN_FILENO, &ch, 1) == 1) {
		if (ch == 's' || ch == 'S') {
			debuginfod_fetch_cancelled = true;
			fputs("\nSkipping this debuginfod fetch, press 'd' to also disable it for this session\n", stderr);
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
 * fetch, 'd' also disables debuginfod for the rest of the session.  The
 * client only invokes this once it committed to a server, so 'a' is the
 * number of bytes fetched so far, 'b' the total size when the server
 * tells it, -1 otherwise.
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
 */
static void debuginfod__setup_urls_env(void)
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
 * Build IDs already searched for on the debuginfod servers without
 * success, so that callers that see the same DSO over and over, such as
 * the data type profiler switching between DSOs on every hist entry,
 * don't pay a server round trip again for each miss.  The cache of
 * successes is the debuginfod client's own, in the local filesystem.
 */
struct debuginfod_miss {
	struct list_head	node;
	struct build_id		bid;
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

static bool debuginfod__missed(const struct build_id *bid)
{
	struct debuginfod_miss *miss;
	bool found = false;

	debuginfod__missed_lock_init();
	mutex_lock(&debuginfod__missed_lock);
	list_for_each_entry(miss, &debuginfod__misses, node) {
		if (miss->bid.size == bid->size &&
		    !memcmp(miss->bid.data, bid->data, bid->size)) {
			found = true;
			break;
		}
	}
	mutex_unlock(&debuginfod__missed_lock);

	return found;
}

static void debuginfod__miss_add(const struct build_id *bid)
{
	struct debuginfod_miss *miss = zalloc(sizeof(*miss));

	if (miss == NULL)
		return;

	miss->bid = *bid;

	debuginfod__missed_lock_init();
	mutex_lock(&debuginfod__missed_lock);
	list_add(&miss->node, &debuginfod__misses);
	mutex_unlock(&debuginfod__missed_lock);
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
 * core.debuginfod=false (what the 'd' key below writes), with the
 * per-tool report.debuginfod/top.debuginfod, and it is off too when
 * the user disabled the local build-id/.debug cache, e.g. with
 * buildid.dir = /dev/null, as is the case for users that don't want
 * any of this stored locally.
 *
 * On success the path is stored in *@path and must be freed by the
 * caller, the file remains available in the debuginfod client cache.
 */
int debuginfo__find_build_id(const struct build_id *bid, char **path)
{
	char sbuild_id[SBUILD_ID_SIZE];
	struct termios orig_termios;
	struct sigaction sa, orig_sigint, orig_sigterm;
	bool term_set = false, sigint_set = false, sigterm_set = false;
	debuginfod_client *c;
	int fd;

	*path = NULL;

	if (!build_id__is_defined(bid) || !symbol_conf.debuginfod)
		return -1;

	if (debuginfod__cache_disabled()) {
		pr_debug("Build-id cache disabled (buildid dir is '%s'), not using debuginfod\n",
			 buildid_dir);
		return -1;
	}

	if (debuginfod__missed(bid)) {
		build_id__snprintf(bid, sbuild_id, sizeof(sbuild_id));
		pr_debug("Not searching build ID %s in debuginfod again, it was a miss earlier\n",
			 sbuild_id);
		return -1;
	}

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
			 * The terminal is restored, die as the user asked;
			 * the original dispositions are back in place.
			 */
			if (debuginfod_signal)
				raise(debuginfod_signal);
			return -1;
		}
		pr_debug("No debuginfo found for build ID %s in debuginfod\n",
			 sbuild_id);
		debuginfod__miss_add(bid);
		return -1;
	}

	close(fd);
	return 0;
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

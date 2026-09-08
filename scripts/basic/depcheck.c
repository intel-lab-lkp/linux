// SPDX-License-Identifier: GPL-2.0-only
/*
 * depcheck - check the dependency timestamps of a directory's targets so that
 * make reads only what it needs from their .cmd files.
 *
 * fixdep writes a .cmd file as:
 *
 *	savedcmd_dir/foo.o := <command line>
 *
 *	source_dir/foo.o := dir/foo.c
 *
 *	deps_dir/foo.o := \
 *	  include/linux/bar.h \
 *	    $(wildcard include/config/BAZ) \
 *
 *	dir/foo.o: $(deps_dir/foo.o)
 *
 *	$(deps_dir/foo.o):
 *
 * and kbuild may append rules of its own after that, such as one making the
 * target depend on objtool.
 *
 * For a target that exists and is newer than every dependency listed, make can
 * have nothing to do with the list, so it is left out and only what precedes
 * and follows it is passed on; for anything else the .cmd file is passed on in
 * full.
 *
 * Usage: depcheck <output> <.cmd files...>
 *
 * The output is a makefile fragment to include in place of the .cmd files; a
 * non-zero exit status means the caller should include those instead.
 */
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <hash.h>
#include <hashtable.h>
#include <xalloc.h>

/* What fixdep writes, see above. */
#define DEPS_PREFIX		"deps_"
#define DEPS_RULE_PREFIX	"$(" DEPS_PREFIX
#define RULE_SUFFIX		":"
#define LINE_CONTINUATION	" \\"
#define WILDCARD_OPEN		"$(wildcard "
#define WILDCARD_CLOSE		")"
#define CMD_SUFFIX		".cmd"

/* A line of a .cmd file, without its newline. */
struct line {
	const char *text;
	size_t len;
};

static bool is_blank(char chr)
{
	return chr == ' ' || chr == '\t';
}

static bool str_ends_with(const char *str, const char *suffix)
{
	const size_t len = strlen(str), suffix_len = strlen(suffix);

	return len >= suffix_len && !strcmp(str + len - suffix_len, suffix);
}

static bool line_starts_with(const struct line *line, const char *prefix)
{
	const size_t len = strlen(prefix);

	return line->len >= len && !memcmp(line->text, prefix, len);
}

static bool line_ends_with(const struct line *line, const char *suffix)
{
	const size_t len = strlen(suffix);

	return line->len >= len &&
	       !memcmp(line->text + line->len - len, suffix, len);
}

static bool line_is_blank(const struct line *line)
{
	size_t i;

	for (i = 0; i < line->len; i++)
		if (!is_blank(line->text[i]))
			return false;

	return true;
}

/* Whether the line ends in " \", continuing the list on the next line. */
static bool line_is_continued(const struct line *line)
{
	return line_ends_with(line, LINE_CONTINUATION);
}

static void line_strip_continuation(struct line *line)
{
	if (line_is_continued(line))
		line->len -= strlen(LINE_CONTINUATION);
}

static void line_trim(struct line *line)
{
	while (line->len && is_blank(line->text[0])) {
		line->text++;
		line->len--;
	}
	while (line->len && is_blank(line->text[line->len - 1]))
		line->len--;
}

/* Take the next line out of [*pos, end); false once there are none left. */
static bool next_line(const char **pos, const char *end, struct line *line)
{
	const char *newline;

	if (*pos >= end)
		return false;

	newline = memchr(*pos, '\n', end - *pos);
	line->text = *pos;
	line->len = (newline ? newline : end) - *pos;
	*pos = newline ? newline + 1 : end;

	return true;
}

/* Describes a dependency file. */
struct dep {
	struct hlist_node hnode;
	struct timespec mtime;
	bool exists;
	char path[];
};

static HASHTABLE_DEFINE(dep_table, 1U << 16);

static const struct dep *lookup_dep(const char *path)
{
	const unsigned int key = hash_str(path);
	struct dep *dep;
	struct stat st;

	hash_for_each_possible(dep_table, dep, hnode, key) {
		if (!strcmp(dep->path, path))
			return dep;
	}

	dep = xmalloc(sizeof(*dep) + strlen(path) + 1);
	strcpy(dep->path, path);
	dep->exists = !stat(path, &st);
	if (dep->exists)
		dep->mtime = st.st_mtim;
	hash_add(dep_table, &dep->hnode, key);

	return dep;
}

/* Strictly newer, as make compares timestamps. */
static bool newer(const struct timespec *time_a, const struct timespec *time_b)
{
	if (time_a->tv_sec != time_b->tv_sec)
		return time_a->tv_sec > time_b->tv_sec;

	return time_a->tv_nsec > time_b->tv_nsec;
}

/*
 * $(wildcard include/config/FOO) is a prerequisite only while FOO is set:
 * unwrap it and say that it is optional.
 */
static bool line_unwrap_wildcard(struct line *line)
{
	if (!line_starts_with(line, WILDCARD_OPEN) ||
	    !line_ends_with(line, WILDCARD_CLOSE))
		return false;

	line->text += strlen(WILDCARD_OPEN);
	line->len -= strlen(WILDCARD_OPEN) + strlen(WILDCARD_CLOSE);

	return true;
}

/* fixdep doubles '$' and escapes '#' in a path: undo that into path[]. */
static bool unescape_path(const struct line *line, char *path, size_t size)
{
	size_t i, out_len = 0;

	if (line->len >= size)
		return false;

	for (i = 0; i < line->len; i++) {
		const char chr = line->text[i];
		const char next_chr = i + 1 < line->len ? line->text[i + 1] : '\0';

		if ((chr == '$' && next_chr == '$') || (chr == '\\' && next_chr == '#'))
			i++;
		path[out_len++] = line->text[i];
	}
	path[out_len] = '\0';

	return true;
}

/* Is the path contained in line older than the target? */
static bool dep_is_fresh(struct line line, const struct timespec *target)
{
	char path[PATH_MAX];
	const struct dep *dep;
	bool optional;

	line_strip_continuation(&line);
	line_trim(&line);
	if (!line.len)
		return false;

	optional = line_unwrap_wildcard(&line);
	if (!line.len || !unescape_path(&line, path, sizeof(path)))
		return false;

	dep = lookup_dep(path);
	if (!dep->exists)
		return optional;

	return !newer(&dep->mtime, target);
}

/*
 * Parse a dependency list which consists of one file per line and determine if
 * all dependencies are 'fresh', i.e. older than the target.
 */
static bool deps_are_fresh(const char *pos, const char *end,
			   const struct timespec *target)
{
	struct line line;

	while (next_line(&pos, end, &line)) {
		if (line_is_blank(&line))
			return true;
		if (!dep_is_fresh(line, target))
			return false;
		if (!line_is_continued(&line))
			return true;
	}

	return true;
}

/*
 * Find the "deps_" line in the input.
 *
 * On success returns true and *deps_off is set to its offset and *list is set
 * to the list's own lines start or NULL if the line is not continued.
 *
 * Otherwise returns false if the prefix cannot be found.
 */
static bool find_deps(const char *buf, size_t len, size_t *deps_off,
		      const char **list)
{
	const char *pos = buf, *end = buf + len;
	struct line line;

	while (next_line(&pos, end, &line)) {
		if (!line_starts_with(&line, DEPS_PREFIX))
			continue;

		*deps_off = line.text - buf;
		*list = line_is_continued(&line) ? pos : NULL;
		return true;
	}

	return false;
}

/*
 * Is this the target of a .cmd file, i.e. 'dir/.name.cmd names dir/name.'?
 */
static bool target_of(const char *cmd_path, char *target, size_t size)
{
	const char *slash = strrchr(cmd_path, '/');
	const char *base = slash ? slash + 1 : cmd_path;
	const size_t dir_len = base - cmd_path;
	size_t name_len;

	if (base[0] != '.' || !str_ends_with(base, CMD_SUFFIX))
		return false;

	name_len = strlen(base) - strlen(".") - strlen(CMD_SUFFIX);
	if (!name_len)
		return false;

	return snprintf(target, size, "%.*s%.*s", (int)dir_len, cmd_path,
			(int)name_len, base + 1) < (int)size;
}

/*
 * Does the .cmd file's target exists and is it newer than everything in its
 * dependency list?
 *
 * Returns true if so and sets *deps_off to the start of the list, otherwise
 * returns false.
 */
static bool target_is_fresh(const char *cmd_path, const char *buf, size_t len,
			    size_t *deps_off)
{
	char target[PATH_MAX];
	struct stat st;
	const char *list;

	if (!target_of(cmd_path, target, sizeof(target)) || stat(target, &st))
		return false;
	if (!find_deps(buf, len, deps_off, &list))
		return false;
	if (!list)
		return true;

	return deps_are_fresh(list, buf + len, &st.st_mtim);
}

static char *read_file(const char *path, size_t *len)
{
	FILE *file = fopen(path, "r");
	struct stat st;
	char *buf;
	size_t nr_read = 0;

	if (!file)
		return NULL;
	if (fstat(fileno(file), &st)) {
		fclose(file);
		return NULL;
	}

	buf = xmalloc(st.st_size + 1);
	while (nr_read < (size_t)st.st_size) {
		const size_t chunk = fread(buf + nr_read, 1, st.st_size - nr_read, file);

		if (!chunk)
			break;
		nr_read += chunk;
	}
	fclose(file);

	buf[nr_read] = '\0';
	*len = nr_read;
	return buf;
}

/*
 * Find the end of the dependency block: the offset just past its closing
 * "$(deps_x):" line. Anything after that was appended by kbuild and is not for
 * us to judge. Returns len if the line cannot be found.
 */
static size_t deps_block_end(const char *buf, size_t len, size_t deps_off)
{
	const char *pos = buf + deps_off, *end = buf + len;
	struct line line;

	while (next_line(&pos, end, &line)) {
		if (line_starts_with(&line, DEPS_RULE_PREFIX) &&
		    line_ends_with(&line, RULE_SUFFIX))
			return pos - buf;
	}

	return len;
}

/* The target is fresh: pass on everything but the dependency block. */
static void write_without_deps(FILE *out, const char *buf, size_t len,
			       size_t deps_off)
{
	const size_t tail_off = deps_block_end(buf, len, deps_off);

	fwrite(buf, 1, deps_off, out);
	fputc('\n', out);
	fwrite(buf + tail_off, 1, len - tail_off, out);
}

/*
 * Generate a fragment for make from one .cmd file - without the dependency
 * block if the target is fresh, otherwise all of it.
 */
static void process(FILE *out, const char *cmd_path)
{
	size_t len, deps_off;
	char *buf;

	buf = read_file(cmd_path, &len);
	if (!buf)
		return;

	if (target_is_fresh(cmd_path, buf, len, &deps_off))
		write_without_deps(out, buf, len, deps_off);
	else
		fwrite(buf, 1, len, out);

	free(buf);
}

int main(int argc, char **argv)
{
	char tmp_path[PATH_MAX];
	FILE *out;
	int i;

	if (argc < 2) {
		fprintf(stderr, "usage: %s <output> <.cmd files...>\n", argv[0]);
		return 1;
	}

	if (snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", argv[1]) >=
	    (int)sizeof(tmp_path)) {
		fprintf(stderr, "%s: path too long\n", argv[1]);
		return 1;
	}

	out = fopen(tmp_path, "w");
	if (!out) {
		perror(tmp_path);
		return 1;
	}

	for (i = 2; i < argc; i++)
		process(out, argv[i]);

	if (fclose(out) || rename(tmp_path, argv[1])) {
		perror(argv[1]);
		unlink(tmp_path);
		return 1;
	}

	return 0;
}

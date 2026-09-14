/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef _PERF_DEBUGINFO_H
#define _PERF_DEBUGINFO_H

#include <errno.h>
#include <linux/compiler.h>

struct build_id;

#ifdef HAVE_LIBDW_SUPPORT

#include "dwarf-aux.h"

/* debug information structure */
struct debuginfo {
	Dwarf		*dbg;
	Dwfl_Module	*mod;
	Dwfl		*dwfl;
	Dwarf_Addr	bias;
	const unsigned char	*build_id;
};

/* This also tries to open distro debuginfo */
struct debuginfo *debuginfo__new(const char *path);
void debuginfo__delete(struct debuginfo *dbg);

int debuginfo__get_text_offset(struct debuginfo *dbg, Dwarf_Addr *offs,
			       bool adjust_offset);

#else /* HAVE_LIBDW_SUPPORT */

/* dummy debug information structure */
struct debuginfo {
};

static inline struct debuginfo *debuginfo__new(const char *path __maybe_unused)
{
	return NULL;
}

static inline void debuginfo__delete(struct debuginfo *dbg __maybe_unused)
{
}

typedef void Dwarf_Addr;

static inline int debuginfo__get_text_offset(struct debuginfo *dbg __maybe_unused,
					     Dwarf_Addr *offs __maybe_unused,
					     bool adjust_offset __maybe_unused)
{
	return -EINVAL;
}

#endif /* HAVE_LIBDW_SUPPORT */

#ifdef HAVE_DEBUGINFOD_SUPPORT
int get_source_from_debuginfod(const char *raw_path, const char *sbuild_id,
			       char **new_path);

/*
 * Finding a debuginfo file keyed by build ID uses the debuginfod client,
 * but opening the DWARF in it needs libdw, i.e. these live in
 * debuginfo.o, which is only built with CONFIG_LIBDW.
 */
#ifdef HAVE_LIBDW_SUPPORT
int debuginfo__find_build_id(const struct build_id *bid, char **path);
struct debuginfo *debuginfo__new_build_id(const struct build_id *bid);
#else
static inline int debuginfo__find_build_id(const struct build_id *bid __maybe_unused,
					   char **path __maybe_unused)
{
	return -ENOTSUP;
}

static inline struct debuginfo *
debuginfo__new_build_id(const struct build_id *bid __maybe_unused)
{
	return NULL;
}
#endif /* HAVE_LIBDW_SUPPORT */
#else /* HAVE_DEBUGINFOD_SUPPORT */
static inline int get_source_from_debuginfod(const char *raw_path __maybe_unused,
					     const char *sbuild_id __maybe_unused,
					     char **new_path __maybe_unused)
{
	return -ENOTSUP;
}

static inline int debuginfo__find_build_id(const struct build_id *bid __maybe_unused,
					   char **path __maybe_unused)
{
	return -ENOTSUP;
}

static inline struct debuginfo *
debuginfo__new_build_id(const struct build_id *bid __maybe_unused)
{
	return NULL;
}
#endif /* HAVE_DEBUGINFOD_SUPPORT */

#endif /* _PERF_DEBUGINFO_H */

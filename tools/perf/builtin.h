/* SPDX-License-Identifier: GPL-2.0 */
#ifndef BUILTIN_H
#define BUILTIN_H

#include <stddef.h>
#include <linux/compiler.h>
#include <tools/config.h>

struct feature_support {
	const char *name;
	const char *macro;
	int is_builtin;
};

#define FEATURE_SUPPORT(name_, macro_) { \
	.name = name_,                       \
	.macro = #macro_,                    \
	.is_builtin = IS_BUILTIN(macro_) }

static struct feature_support supported_features[] __maybe_unused = {
	FEATURE_SUPPORT("dwarf", HAVE_DWARF_SUPPORT),
	FEATURE_SUPPORT("dwarf_getlocations", HAVE_DWARF_GETLOCATIONS_SUPPORT),
#ifndef HAVE_SYSCALL_TABLE_SUPPORT
	FEATURE_SUPPORT("libaudit", HAVE_LIBAUDIT_SUPPORT),
#endif
	FEATURE_SUPPORT("syscall_table", HAVE_SYSCALL_TABLE_SUPPORT),
	FEATURE_SUPPORT("libbfd", HAVE_LIBBFD_SUPPORT),
	FEATURE_SUPPORT("debuginfod", HAVE_DEBUGINFOD_SUPPORT),
	FEATURE_SUPPORT("libelf", HAVE_LIBELF_SUPPORT),
	FEATURE_SUPPORT("libnuma", HAVE_LIBNUMA_SUPPORT),
	FEATURE_SUPPORT("numa_num_possible_cpus", HAVE_LIBNUMA_SUPPORT),
	FEATURE_SUPPORT("libperl", HAVE_LIBPERL_SUPPORT),
	FEATURE_SUPPORT("libpython", HAVE_LIBPYTHON_SUPPORT),
	FEATURE_SUPPORT("libslang", HAVE_SLANG_SUPPORT),
	FEATURE_SUPPORT("libcrypto", HAVE_LIBCRYPTO_SUPPORT),
	FEATURE_SUPPORT("libunwind", HAVE_LIBUNWIND_SUPPORT),
	FEATURE_SUPPORT("libdw-dwarf-unwind", HAVE_DWARF_SUPPORT),
	FEATURE_SUPPORT("zlib", HAVE_ZLIB_SUPPORT),
	FEATURE_SUPPORT("lzma", HAVE_LZMA_SUPPORT),
	FEATURE_SUPPORT("get_cpuid", HAVE_AUXTRACE_SUPPORT),
	FEATURE_SUPPORT("bpf", HAVE_LIBBPF_SUPPORT),
	FEATURE_SUPPORT("aio", HAVE_AIO_SUPPORT),
	FEATURE_SUPPORT("zstd", HAVE_ZSTD_SUPPORT),
	FEATURE_SUPPORT("libpfm4", HAVE_LIBPFM),
	FEATURE_SUPPORT("libtraceevent", HAVE_LIBTRACEEVENT),

	// this should remain at end, to know the array end
	FEATURE_SUPPORT(NULL, _)
};

void list_common_cmds_help(void);
const char *help_unknown_cmd(const char *cmd);

int cmd_annotate(int argc, const char **argv);
int cmd_bench(int argc, const char **argv);
int cmd_build(int argc, const char **argv);
int cmd_buildid_cache(int argc, const char **argv);
int cmd_buildid_list(int argc, const char **argv);
int cmd_config(int argc, const char **argv);
int cmd_c2c(int argc, const char **argv);
int cmd_diff(int argc, const char **argv);
int cmd_evlist(int argc, const char **argv);
int cmd_help(int argc, const char **argv);
int cmd_sched(int argc, const char **argv);
int cmd_kallsyms(int argc, const char **argv);
int cmd_list(int argc, const char **argv);
int cmd_record(int argc, const char **argv);
int cmd_report(int argc, const char **argv);
int cmd_stat(int argc, const char **argv);
int cmd_timechart(int argc, const char **argv);
int cmd_top(int argc, const char **argv);
int cmd_script(int argc, const char **argv);
int cmd_version(int argc, const char **argv);
int cmd_probe(int argc, const char **argv);
int cmd_kmem(int argc, const char **argv);
int cmd_lock(int argc, const char **argv);
int cmd_kvm(int argc, const char **argv);
int cmd_test(int argc, const char **argv);
int cmd_trace(int argc, const char **argv);
int cmd_inject(int argc, const char **argv);
int cmd_mem(int argc, const char **argv);
int cmd_data(int argc, const char **argv);
int cmd_ftrace(int argc, const char **argv);
int cmd_daemon(int argc, const char **argv);
int cmd_kwork(int argc, const char **argv);

int find_scripts(char **scripts_array, char **scripts_path_array, int num,
		 int pathlen);
#endif

#include <bpf/libbpf.h>
#include <errno.h>
#include <internal/xyarray.h>
#include <string.h>

#include "bpf_skel/augmented_raw_syscalls.skel.h"
#include "debug.h"
#include "evlist.h"
#include "parse-events.h"
#include "trace_augment.h"

static struct augmented_raw_syscalls_bpf *skel;
static struct evsel *bpf_output;

/* Set by attach_prog() so the first failure is what gets reported. */
static int attach_err;

static int attach_prog(struct bpf_link **link, struct bpf_program *prog, const char *name)
{
	*link = bpf_program__attach(prog);
	if (*link)
		return 0;
	/*
	 * Save errno before pr_debug(), which formats and writes output and so
	 * can overwrite it.
	 */
	attach_err = -errno;
	pr_debug("Failed to attach %s BPF program\n", name);
	return attach_err;
}

int augmented_syscalls__prepare(bool inherit)
{
	struct bpf_program *prog;
	char buf[128];
	int err;

	skel = augmented_raw_syscalls_bpf__open();
	if (!skel) {
		pr_debug("Failed to open augmented syscalls BPF skeleton\n");
		return -errno;
	}

	skel->rodata->inherit = inherit;

	/*
	 * Disable attaching the BPF programs other than those attached
	 * explicitly by augmented_syscalls__attach(), the rest are reached by
	 * tail calls.
	 */
	bpf_object__for_each_program(prog, skel->obj) {
		if (prog != skel->progs.sys_enter && prog != skel->progs.sys_exit &&
		    prog != skel->progs.sched_process_fork &&
		    prog != skel->progs.sched_process_exit &&
		    prog != skel->progs.sched_process_exec)
			bpf_program__set_autoattach(prog, /*autoattach=*/false);
	}

	err = augmented_raw_syscalls_bpf__load(skel);
	if (err < 0) {
		libbpf_strerror(err, buf, sizeof(buf));
		pr_debug("Failed to load augmented syscalls BPF skeleton: %s\n", buf);
		/*
		 * Tear the skeleton down rather than leaving a half initialized
		 * one behind. The caller falls back to unaugmented tracing and
		 * still calls the setters below, which must then do nothing
		 * instead of failing against a skeleton with no maps.
		 */
		augmented_syscalls__cleanup();
		return err;
	}

	return 0;
}

int augmented_syscalls__attach(void)
{
	int err;

	if (skel == NULL)
		return 0;

	/*
	 * Attaching is deliberately separate from, and a lot later than,
	 * loading: these are system wide tracepoint programs, so from the
	 * moment they are attached they are the only thing deciding which
	 * tasks and syscalls are traced. Going live before the pid and
	 * syscall maps are populated would mean a target that forked in the
	 * meantime was never picked up by sched_process_fork() below.
	 *
	 * Attach explicitly, so that a failure part way through can undo what
	 * came before it: leaving sys_enter live on raw_syscalls:sys_enter
	 * would keep running a BPF program for every syscall on the system for
	 * a perf trace session that never starts.
	 *
	 * The scheduler programs maintain the pid maps, and are attached first
	 * so that no fork, exit or exec can be missed between sys_enter going
	 * live and the maps being maintained.
	 */
	if (attach_prog(&skel->links.sched_process_fork, skel->progs.sched_process_fork,
			"sched_process_fork"))
		goto out_cleanup;
	if (attach_prog(&skel->links.sched_process_exit, skel->progs.sched_process_exit,
			"sched_process_exit"))
		goto out_cleanup;
	if (attach_prog(&skel->links.sched_process_exec, skel->progs.sched_process_exec,
			"sched_process_exec"))
		goto out_cleanup;
	if (attach_prog(&skel->links.sys_enter, skel->progs.sys_enter, "sys_enter"))
		goto out_cleanup;
	if (attach_prog(&skel->links.sys_exit, skel->progs.sys_exit, "sys_exit"))
		goto out_cleanup;

	return 0;

out_cleanup:
	err = attach_err;
	/* Destroys every link attached above along with the skeleton. */
	augmented_syscalls__cleanup();
	/*
	 * Tearing the skeleton down closes file descriptors and frees memory,
	 * either of which may overwrite errno. Restore it so that a caller
	 * reporting this with "%m" describes the attach failure rather than
	 * whatever the teardown happened to do last.
	 */
	errno = -err;
	return err;
}

int augmented_syscalls__create_bpf_output(struct evlist *evlist)
{
	int err = parse_event(evlist, "bpf-output/no-inherit=1,name=__augmented_syscalls__/");

	if (err) {
		pr_err("ERROR: Setup BPF output event failed: %d\n", err);
		return err;
	}

	bpf_output = evlist__last(evlist);
	assert(evsel__name_is(bpf_output, "__augmented_syscalls__"));

	return 0;
}

void augmented_syscalls__setup_bpf_output(void)
{
	struct perf_cpu cpu;
	unsigned int i;

	if (bpf_output == NULL)
		return;

	/*
	 * Set up the __augmented_syscalls__ BPF map to hold for each
	 * CPU the bpf-output event's file descriptor.
	 */
	perf_cpu_map__for_each_cpu(cpu, i, bpf_output->core.cpus) {
		int mycpu = cpu.cpu;

		bpf_map__update_elem(skel->maps.__augmented_syscalls__,
				     &mycpu, sizeof(mycpu),
				     xyarray__entry(bpf_output->core.fd,
						    mycpu, 0),
				     sizeof(__u32), BPF_ANY);
	}
}

int augmented_syscalls__set_filter_pids(unsigned int nr, pid_t *pids)
{
	bool value = true;
	int err = 0;

	if (skel == NULL || nr == 0)
		return 0;

	/*
	 * Tell the BPF program that the pids_filtered map is in use. Without
	 * this it would have to look up every task in an empty map, on every
	 * syscall on the system, to find out that nothing is filtered.
	 */
	for (size_t i = 0; i < nr; ++i) {
		err = bpf_map__update_elem(skel->maps.pids_filtered, &pids[i],
					   sizeof(*pids), &value, sizeof(value),
					   BPF_ANY);
		if (err)
			return err;
	}
	/*
	 * Publish the filter only now that the map is fully populated.
	 * augmented_syscalls__attach() has not run yet, so nothing is reading
	 * either of them, but keeping the flag and the map consistent means
	 * the ordering stays correct however the callers are rearranged.
	 */
	skel->bss->has_pids_filtered = true;
	return 0;
}

/*
 * Populate target PIDs in the BPF pids_to_trace map (e.g., for -p <PID> or
 * when tracing a specified command workload).
 */
int augmented_syscalls__set_target_pids(unsigned int nr, pid_t *pids)
{
	bool value = true;
	int err = 0;

	if (skel == NULL || nr == 0)
		return 0;

	for (size_t i = 0; i < nr; ++i) {
		err = bpf_map__update_elem(skel->maps.pids_to_trace, &pids[i],
					   sizeof(*pids), &value, sizeof(value),
					   BPF_ANY);
		if (err)
			return err;
	}
	/*
	 * Set the flag only once every target is in the map, so that the two
	 * are never inconsistent. Publishing it first would, once the
	 * programs are attached, have them filter against a partially
	 * populated map and drop syscalls made by targets not yet added.
	 */
	skel->bss->has_pids_to_trace = true;
	return 0;
}

/*
 * Populate syscalls in the BPF syscalls_to_trace map:
 * - not_syscalls: true if '!' prefix was specified (blacklist mode: trace
 *   all syscalls EXCEPT these).
 *   false if whitelist mode (trace ONLY these syscalls).
 */
int augmented_syscalls__set_target_syscalls(unsigned int nr, int *syscall_ids, bool not_syscalls)
{
	bool value = true;
	int err = 0;

	if (skel == NULL || nr == 0)
		return 0;

	skel->bss->not_syscalls_to_trace = not_syscalls;
	for (size_t i = 0; i < nr; ++i) {
		err = bpf_map__update_elem(skel->maps.syscalls_to_trace, &syscall_ids[i],
					   sizeof(int), &value, sizeof(value),
					   BPF_ANY);
		if (err)
			return err;
	}
	/*
	 * As for the pid maps, publish the filter only once it is complete:
	 * in whitelist mode a half filled map would drop syscalls that were
	 * asked for but not added yet.
	 */
	skel->bss->has_syscalls_to_trace = true;
	return 0;
}

int augmented_syscalls__get_map_fds(int *enter_fd, int *exit_fd, int *beauty_fd)
{
	if (skel == NULL)
		return -1;

	*enter_fd = bpf_map__fd(skel->maps.syscalls_sys_enter);
	*exit_fd  = bpf_map__fd(skel->maps.syscalls_sys_exit);
	*beauty_fd = bpf_map__fd(skel->maps.beauty_map_enter);

	if (*enter_fd < 0 || *exit_fd < 0 || *beauty_fd < 0) {
		pr_err("Error: failed to get syscall or beauty map fd\n");
		return -1;
	}

	return 0;
}

struct bpf_program *augmented_syscalls__unaugmented(void)
{
	return skel->progs.syscall_unaugmented;
}

struct bpf_program *augmented_syscalls__find_by_title(const char *name)
{
	struct bpf_program *pos;
	const char *sec_name;

	if (skel->obj == NULL)
		return NULL;

	bpf_object__for_each_program(pos, skel->obj) {
		sec_name = bpf_program__section_name(pos);
		if (sec_name && !strcmp(sec_name, name))
			return pos;
	}

	return NULL;
}

void augmented_syscalls__cleanup(void)
{
	augmented_raw_syscalls_bpf__destroy(skel);
	skel = NULL;
}

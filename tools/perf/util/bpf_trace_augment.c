#include <assert.h>
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

int augmented_syscalls__prepare(void)
{
	struct bpf_program *prog;
	char buf[128];
	int err;

	skel = augmented_raw_syscalls_bpf__open();
	if (!skel) {
		pr_debug("Failed to open augmented syscalls BPF skeleton\n");
		return -errno;
	}

	/*
	 * Disable attaching the BPF programs except for sys_enter and
	 * sys_exit that tail call into this as necessary.
	 */
	bpf_object__for_each_program(prog, skel->obj) {
		if (prog != skel->progs.sys_enter && prog != skel->progs.sys_exit)
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

	/*
	 * Only sys_enter and sys_exit are attached, the remaining programs are
	 * reached by tail calls. Attach them explicitly and, on failure, undo
	 * any partial attachment: leaving sys_enter live on
	 * raw_syscalls:sys_enter would keep running a BPF program for every
	 * syscall on the system for a perf trace session that never starts.
	 */
	if (attach_prog(&skel->links.sys_enter, skel->progs.sys_enter, "sys_enter"))
		goto out_cleanup;
	if (attach_prog(&skel->links.sys_exit, skel->progs.sys_exit, "sys_exit"))
		goto out_cleanup;

	return 0;

out_cleanup:
	err = attach_err;
	/* Destroys every link attached above along with the skeleton. */
	augmented_syscalls__cleanup();
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

/*
 * Add every pid to a pid keyed filter map.
 *
 * A map with no room left is not treated as a failure. bpf_map__update_elem()
 * answers -E2BIG once max_entries keys are present, and a task that did not
 * fit is then one the filter does not know about, which is exactly the
 * position it would be in had it been created after the map filled up.
 * Refusing to trace at all instead would make perf trace unusable against the
 * very large targets that are the only way to reach the limit, so say how many
 * missed out and carry on with those that did fit.
 */
static int add_pids_to_map(struct bpf_map *map, const char *missing_out_on,
			   unsigned int nr, pid_t *pids)
{
	unsigned int nr_no_room = 0;
	bool value = true;

	for (unsigned int i = 0; i < nr; ++i) {
		int err = bpf_map__update_elem(map, &pids[i], sizeof(*pids),
					       &value, sizeof(value), BPF_ANY);

		if (err == -E2BIG) {
			nr_no_room++;
			continue;
		}
		if (err)
			return err;
	}

	if (nr_no_room) {
		pr_warning("Only %u of %u tasks fit in the %s BPF map, %u will not be %s.\n",
			   nr - nr_no_room, nr, bpf_map__name(map), nr_no_room,
			   missing_out_on);
	}

	return 0;
}

int augmented_syscalls__set_filter_pids(unsigned int nr, pid_t *pids)
{
	if (skel == NULL)
		return 0;

	return add_pids_to_map(skel->maps.pids_filtered, "filtered out", nr, pids);
}

/*
 * Populate target PIDs in the BPF pids_to_trace map (e.g., for -p <PID> or
 * when tracing a specified command workload).
 */
int augmented_syscalls__set_target_pids(unsigned int nr, pid_t *pids)
{
	int err;

	if (skel == NULL || nr == 0)
		return 0;

	err = add_pids_to_map(skel->maps.pids_to_trace, "traced", nr, pids);
	if (err)
		return err;

	/*
	 * Set the flag only once every target is in the map. The BPF programs
	 * are attached by this point, so flipping it first would have them
	 * filter against a partially populated map and drop syscalls made by
	 * the targets that had not been added yet.
	 */
	skel->bss->has_pids_to_trace = true;
	return 0;
}

int augmented_syscalls__add_target_pid(pid_t pid)
{
	bool value = true;

	if (skel == NULL || !skel->bss->has_pids_to_trace || skel->maps.pids_to_trace == NULL)
		return 0;

	return bpf_map__update_elem(skel->maps.pids_to_trace, &pid, sizeof(pid),
				    &value, sizeof(value), BPF_ANY);
}

int augmented_syscalls__del_target_pid(pid_t pid)
{
	if (skel == NULL || !skel->bss->has_pids_to_trace || skel->maps.pids_to_trace == NULL)
		return 0;

	return bpf_map__delete_elem(skel->maps.pids_to_trace, &pid, sizeof(pid), 0);
}

bool augmented_syscalls__has_target_pid(pid_t pid)
{
	bool value;

	if (skel == NULL || !skel->bss->has_pids_to_trace || skel->maps.pids_to_trace == NULL)
		return false;

	return bpf_map__lookup_elem(skel->maps.pids_to_trace, &pid, sizeof(pid),
				    &value, sizeof(value), 0) == 0;
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

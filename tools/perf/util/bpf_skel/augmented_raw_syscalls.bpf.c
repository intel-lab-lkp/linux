// SPDX-License-Identifier: GPL-2.0
/*
 * Augment the raw_syscalls tracepoints with the contents of the pointer arguments.
 *
 * This exactly matches what is marshalled into the raw_syscall:sys_enter
 * payload expected by the 'perf trace' beautifiers.
 */

#include "vmlinux.h"

#include <bpf/bpf_helpers.h>
#include <bpf/bpf_tracing.h>
#include <linux/limits.h>

#define PERF_ALIGN(x, a)        __PERF_ALIGN_MASK(x, (typeof(x))(a)-1)
#define __PERF_ALIGN_MASK(x, mask)      (((x)+(mask))&~(mask))

/**
 * is_power_of_2() - check if a value is a power of two
 * @n: the value to check
 *
 * Determine whether some value is a power of two, where zero is *not*
 * considered a power of two.  Return: true if @n is a power of 2, otherwise
 * false.
 */
#define is_power_of_2(n) (n != 0 && ((n & (n - 1)) == 0))

#define MAX_CPUS  4096

#define TRACE_AUG_MAX_BUF 32 /* for buffer augmentation in perf trace */

/* bpf-output associated map */
struct __augmented_syscalls__ {
	__uint(type, BPF_MAP_TYPE_PERF_EVENT_ARRAY);
	__type(key, int);
	__type(value, __u32);
	__uint(max_entries, MAX_CPUS);
} __augmented_syscalls__ SEC(".maps");

/*
 * What to augment at entry?
 *
 * Pointer arg payloads (filenames, etc) passed from userspace to the kernel
 */
struct syscalls_sys_enter {
	__uint(type, BPF_MAP_TYPE_PROG_ARRAY);
	__type(key, __u32);
	__type(value, __u32);
	__uint(max_entries, 1024);
} syscalls_sys_enter SEC(".maps");

/*
 * What to augment at exit?
 *
 * Pointer arg payloads returned from the kernel (struct stat, etc) to userspace.
 */
struct syscalls_sys_exit {
	__uint(type, BPF_MAP_TYPE_PROG_ARRAY);
	__type(key, __u32);
	__type(value, __u32);
	__uint(max_entries, 1024);
} syscalls_sys_exit SEC(".maps");

struct syscall_enter_args {
	unsigned long long common_tp_fields;
	long		   syscall_nr;
	unsigned long	   args[6];
};

struct syscall_exit_args {
	unsigned long long common_tp_fields;
	long		   syscall_nr;
	long		   ret;
};

/*
 * Desired design of maximum size and alignment (see RFC2553)
 */
#define SS_MAXSIZE   128     /* Implementation specific max size */

typedef unsigned short sa_family_t;

/*
 * FIXME: Should come from system headers
 *
 * The definition uses anonymous union and struct in order to control the
 * default alignment.
 */
struct sockaddr_storage {
	union {
		struct {
			sa_family_t    ss_family; /* address family */
			/* Following field(s) are implementation specific */
			char __data[SS_MAXSIZE - sizeof(unsigned short)];
				/* space to achieve desired size, */
				/* _SS_MAXSIZE value minus size of ss_family */
		};
		void *__align; /* implementation specific desired alignment */
	};
};

struct augmented_arg {
	unsigned int	size;
	int		err;
	union {
		char   value[PATH_MAX];
		struct sockaddr_storage saddr;
	};
};

/*
 * Hash map of PIDs/TGIDs whose events must be discarded, e.g. perf trace's own
 * pid, so that tracing doesn't feed back on itself.
 *
 * has_pids_filtered: set to true only when the map is populated. Checking a
 *                    boolean is much cheaper than a map lookup, and sys_enter
 *                    runs for every syscall on the system, so the common
 *                    "no pids filtered" case must stay on a fast path.
 *
 * max_entries matches pids_to_trace: userspace only ever names a handful of
 * pids here, but sched_process_fork() below adds every descendant of those,
 * so a --filter-pids target that forks or is heavily threaded needs the same
 * headroom as a traced one.
 */
struct pids_filtered {
	__uint(type, BPF_MAP_TYPE_HASH);
	__type(key, pid_t);
	__type(value, bool);
	__uint(max_entries, 16384);
} pids_filtered SEC(".maps");

bool has_pids_filtered;

/*
 * Optional hash map containing specific PIDs/TGIDs to trace (e.g., when
 * attached to a process with -p or tracing a specific command workload).
 *
 * has_pids_to_trace: Set to true if target PID filtering is active.
 *                    When false, all processes are eligible for tracing.
 *
 * max_entries bounds how many tasks can be tracked at once. sched_process_exit
 * below evicts a task as it dies, whatever it died of, so the map holds live
 * tasks rather than growing without bound. It is sized well
 * above the thread count of realistic traced workloads; should a workload
 * still exceed it, bpf_map_update_elem() fails with -E2BIG and the extra
 * tasks are simply not traced rather than anything being corrupted.
 */
struct pids_to_trace {
	__uint(type, BPF_MAP_TYPE_HASH);
	__type(key, pid_t);
	__type(value, bool);
	__uint(max_entries, 16384);
} pids_to_trace SEC(".maps");

bool has_pids_to_trace;

/*
 * Hash map storing syscall IDs for filtering (via 'perf trace -e ...').
 *
 * has_syscalls_to_trace: Set to true if any syscall filter is active.
 * not_syscalls_to_trace: Inverts matching when '!' prefix is used in -e
 *                        (e.g., -e !open,close means trace everything EXCEPT
 *                        open and close; an exclusion blacklist rather than
 *                        an inclusion whitelist).
 */
struct syscalls_to_trace {
	__uint(type, BPF_MAP_TYPE_HASH);
	__type(key, int);
	__type(value, bool);
	__uint(max_entries, 1024);
} syscalls_to_trace SEC(".maps");

bool has_syscalls_to_trace;
bool not_syscalls_to_trace;

/* Inherit tracing for child tasks (set to false if --no-inherit is specified) */
const volatile bool inherit = true;

struct augmented_args_payload {
	struct syscall_enter_args args;
	struct augmented_arg arg, arg2; // We have to reserve space for two arguments (rename, etc)
};

// We need more tmp space than the BPF stack can give us
struct augmented_args_tmp {
	__uint(type, BPF_MAP_TYPE_PERCPU_ARRAY);
	__type(key, int);
	__type(value, struct augmented_args_payload);
	__uint(max_entries, 1);
} augmented_args_tmp SEC(".maps");

struct beauty_map_enter {
	__uint(type, BPF_MAP_TYPE_HASH);
	__type(key, int);
	__type(value, __u32[6]);
	__uint(max_entries, 512);
} beauty_map_enter SEC(".maps");

struct beauty_payload_enter {
	struct syscall_enter_args args;
	struct augmented_arg aug_args[6];
};

struct beauty_payload_enter_map {
	__uint(type, BPF_MAP_TYPE_PERCPU_ARRAY);
	__type(key, int);
	__type(value, struct beauty_payload_enter);
	__uint(max_entries, 1);
} beauty_payload_enter_map SEC(".maps");

static inline struct augmented_args_payload *augmented_args_payload(void)
{
	int key = 0;
	return bpf_map_lookup_elem(&augmented_args_tmp, &key);
}

static inline int augmented__output(void *ctx, struct augmented_args_payload *args, int len)
{
	bpf_perf_event_output(ctx, &__augmented_syscalls__, BPF_F_CURRENT_CPU, args, len);
	return 1;
}

static inline int augmented__beauty_output(void *ctx, void *data, int len)
{
	return bpf_perf_event_output(ctx, &__augmented_syscalls__, BPF_F_CURRENT_CPU, data, len);
}

static inline
unsigned int augmented_arg__read_str(struct augmented_arg *augmented_arg, const void *arg, unsigned int arg_len)
{
	unsigned int augmented_len = sizeof(*augmented_arg);
	int string_len = bpf_probe_read_user_str(&augmented_arg->value, arg_len, arg);

	augmented_arg->size = augmented_arg->err = 0;
	/*
	 * probe_read_str may return < 0, e.g. -EFAULT
	 * So we leave that in the augmented_arg->size that userspace will
	 */
	if (string_len > 0) {
		augmented_len -= sizeof(augmented_arg->value) - string_len;
		_Static_assert(is_power_of_2(sizeof(augmented_arg->value)), "sizeof(augmented_arg->value) needs to be a power of two");
		augmented_len &= sizeof(augmented_arg->value) - 1;
		augmented_arg->size = string_len;
	} else {
		/*
		 * So that username notice the error while still being able
		 * to skip this augmented arg record
		 */
		augmented_arg->err = string_len;
		augmented_len = offsetof(struct augmented_arg, value);
	}

	return augmented_len;
}

/*
 * Default sys_enter program for syscalls without pointer argument augmentation.
 * Writes the raw struct syscall_enter_args payload into __augmented_syscalls__
 * and returns 1 so the tracepoint is never vetoed in the kernel.
 */
SEC("tp/syscalls/sys_enter_unaugmented")
int syscall_unaugmented(struct syscall_enter_args *args)
{
	struct augmented_args_payload *augmented_args = augmented_args_payload();

	if (augmented_args == NULL)
		return 1;

	bpf_probe_read_kernel(&augmented_args->args, sizeof(augmented_args->args), args);
	return augmented__output(args, augmented_args, sizeof(augmented_args->args));
}

/*
 * These will be tail_called from SEC("raw_syscalls:sys_enter"), so will find in
 * augmented_args_tmp what was read by that raw_syscalls:sys_enter and go
 * on from there, reading the first syscall arg as a string, i.e. open's
 * filename.
 */
SEC("tp/syscalls/sys_enter_connect")
int sys_enter_connect(struct syscall_enter_args *args)
{
	struct augmented_args_payload *augmented_args = augmented_args_payload();
	const void *sockaddr_arg = (const void *)args->args[1];
	unsigned int socklen = args->args[2];
	unsigned int len = sizeof(u64) + sizeof(augmented_args->args); // the size + err in all 'augmented_arg' structs
	int err;

        if (augmented_args == NULL)
                return 1; /* Failure: don't filter */

	_Static_assert(is_power_of_2(sizeof(augmented_args->arg.saddr)), "sizeof(augmented_args->arg.saddr) needs to be a power of two");
	socklen &= sizeof(augmented_args->arg.saddr) - 1;

	err = bpf_probe_read_user(&augmented_args->arg.saddr, socklen, sockaddr_arg);
	/*
	 * A failed read leaves the scratch holding whatever the previous
	 * syscall on this CPU put there, so say there is no payload and why,
	 * rather than describing another task's data as this task's sockaddr.
	 */
	if (err < 0)
		socklen = 0;
	augmented_args->arg.size = socklen;
	augmented_args->arg.err = err < 0 ? err : 0;

	return augmented__output(args, augmented_args, len + socklen);
}

SEC("tp/syscalls/sys_enter_sendto")
int sys_enter_sendto(struct syscall_enter_args *args)
{
	struct augmented_args_payload *augmented_args = augmented_args_payload();
	const void *sockaddr_arg = (const void *)args->args[4];
	unsigned int socklen = args->args[5];
	unsigned int len = sizeof(u64) + sizeof(augmented_args->args); // the size + err in all 'augmented_arg' structs
	int err;

        if (augmented_args == NULL)
                return 1; /* Failure: don't filter */

	socklen &= sizeof(augmented_args->arg.saddr) - 1;

	err = bpf_probe_read_user(&augmented_args->arg.saddr, socklen, sockaddr_arg);
	/* As in sys_enter_connect(), do not describe scratch as a sockaddr. */
	if (err < 0)
		socklen = 0;
	augmented_args->arg.size = socklen;
	augmented_args->arg.err = err < 0 ? err : 0;

	return augmented__output(args, augmented_args, len + socklen);
}

SEC("tp/syscalls/sys_enter_open")
int sys_enter_open(struct syscall_enter_args *args)
{
	struct augmented_args_payload *augmented_args = augmented_args_payload();
	const void *filename_arg = (const void *)args->args[0];
	unsigned int len = sizeof(augmented_args->args);

        if (augmented_args == NULL)
                return 1; /* Failure: don't filter */

	len += augmented_arg__read_str(&augmented_args->arg, filename_arg, sizeof(augmented_args->arg.value));

	return augmented__output(args, augmented_args, len);
}

SEC("tp/syscalls/sys_enter_openat")
int sys_enter_openat(struct syscall_enter_args *args)
{
	struct augmented_args_payload *augmented_args = augmented_args_payload();
	const void *filename_arg = (const void *)args->args[1];
	unsigned int len = sizeof(augmented_args->args);

        if (augmented_args == NULL)
                return 1; /* Failure: don't filter */

	len += augmented_arg__read_str(&augmented_args->arg, filename_arg, sizeof(augmented_args->arg.value));

	return augmented__output(args, augmented_args, len);
}

SEC("tp/syscalls/sys_enter_rename")
int sys_enter_rename(struct syscall_enter_args *args)
{
	struct augmented_args_payload *augmented_args = augmented_args_payload();
	const void *oldpath_arg = (const void *)args->args[0],
		   *newpath_arg = (const void *)args->args[1];
	unsigned int len = sizeof(augmented_args->args), oldpath_len, newpath_len;

        if (augmented_args == NULL)
                return 1; /* Failure: don't filter */

	len += 2 * sizeof(u64); // The overhead of size and err, just before the payload...

	oldpath_len = augmented_arg__read_str(&augmented_args->arg, oldpath_arg, sizeof(augmented_args->arg.value));
	augmented_args->arg.size = PERF_ALIGN(oldpath_len + 1, sizeof(u64));
	len += augmented_args->arg.size;

	/* Every read from userspace is limited to value size */
	if (augmented_args->arg.size > sizeof(augmented_args->arg.value))
		return 1; /* Failure: don't filter */

	struct augmented_arg *arg2 = (void *)&augmented_args->arg.value + augmented_args->arg.size;

	newpath_len = augmented_arg__read_str(arg2, newpath_arg, sizeof(augmented_args->arg.value));
	arg2->size = newpath_len;

	len += newpath_len;

	return augmented__output(args, augmented_args, len);
}

SEC("tp/syscalls/sys_enter_renameat2")
int sys_enter_renameat2(struct syscall_enter_args *args)
{
	struct augmented_args_payload *augmented_args = augmented_args_payload();
	const void *oldpath_arg = (const void *)args->args[1],
		   *newpath_arg = (const void *)args->args[3];
	unsigned int len = sizeof(augmented_args->args), oldpath_len, newpath_len;

        if (augmented_args == NULL)
                return 1; /* Failure: don't filter */

	len += 2 * sizeof(u64); // The overhead of size and err, just before the payload...

	oldpath_len = augmented_arg__read_str(&augmented_args->arg, oldpath_arg, sizeof(augmented_args->arg.value));
	augmented_args->arg.size = PERF_ALIGN(oldpath_len + 1, sizeof(u64));
	len += augmented_args->arg.size;

	/* Every read from userspace is limited to value size */
	if (augmented_args->arg.size > sizeof(augmented_args->arg.value))
		return 1; /* Failure: don't filter */

	struct augmented_arg *arg2 = (void *)&augmented_args->arg.value + augmented_args->arg.size;

	newpath_len = augmented_arg__read_str(arg2, newpath_arg, sizeof(augmented_args->arg.value));
	arg2->size = newpath_len;

	len += newpath_len;

	return augmented__output(args, augmented_args, len);
}

#define PERF_ATTR_SIZE_VER0     64      /* sizeof first published struct */

// we need just the start, get the size to then copy it
struct perf_event_attr_size {
        __u32                   type;
        /*
         * Size of the attr structure, for fwd/bwd compat.
         */
        __u32                   size;
};

SEC("tp/syscalls/sys_enter_perf_event_open")
int sys_enter_perf_event_open(struct syscall_enter_args *args)
{
	struct augmented_args_payload *augmented_args = augmented_args_payload();
	const struct perf_event_attr_size *attr = (const struct perf_event_attr_size *)args->args[0], *attr_read;
	unsigned int len = sizeof(u64) + sizeof(augmented_args->args); // the size + err in all 'augmented_arg' structs

        if (augmented_args == NULL)
		goto failure;

	if (bpf_probe_read_user(&augmented_args->arg.value, sizeof(*attr), attr) < 0)
		goto failure;

	attr_read = (const struct perf_event_attr_size *)augmented_args->arg.value;

	__u32 size = attr_read->size;

	if (!size)
		size = PERF_ATTR_SIZE_VER0;

	if (size > sizeof(augmented_args->arg.value))
                goto failure;

	// Now that we read attr->size and tested it against the size limits, read it completely
	if (bpf_probe_read_user(&augmented_args->arg.value, size, attr) < 0)
		goto failure;

	augmented_args->arg.size = size;
	augmented_args->arg.err = 0;

	return augmented__output(args, augmented_args, len + size);
failure:
	return 1; /* Failure: don't filter */
}

SEC("tp/syscalls/sys_enter_clock_nanosleep")
int sys_enter_clock_nanosleep(struct syscall_enter_args *args)
{
	struct augmented_args_payload *augmented_args = augmented_args_payload();
	const void *rqtp_arg = (const void *)args->args[2];
	unsigned int len = sizeof(u64) + sizeof(augmented_args->args); // the size + err in all 'augmented_arg' structs
	__u32 size = sizeof(struct timespec64);
	int err;

        if (augmented_args == NULL)
		goto failure;

	if (size > sizeof(augmented_args->arg.value))
                goto failure;

	err = bpf_probe_read_user(&augmented_args->arg.value, size, rqtp_arg);
	/* As in sys_enter_connect(), do not describe scratch as a timespec. */
	if (err < 0)
		size = 0;
	augmented_args->arg.size = size;
	augmented_args->arg.err = err < 0 ? err : 0;

	return augmented__output(args, augmented_args, len + size);
failure:
	return 1; /* Failure: don't filter */
}

SEC("tp/syscalls/sys_enter_nanosleep")
int sys_enter_nanosleep(struct syscall_enter_args *args)
{
	struct augmented_args_payload *augmented_args = augmented_args_payload();
	const void *req_arg = (const void *)args->args[0];
	// the size + err in all 'augmented_arg' structs
	unsigned int len = sizeof(u64) + sizeof(augmented_args->args);
	__u32 size = sizeof(struct timespec64);
	int err;

        if (augmented_args == NULL)
		goto failure;

	if (size > sizeof(augmented_args->arg.value))
                goto failure;

	err = bpf_probe_read_user(&augmented_args->arg.value, size, req_arg);
	/* As in sys_enter_connect(), do not describe scratch as a timespec. */
	if (err < 0)
		size = 0;
	augmented_args->arg.size = size;
	augmented_args->arg.err = err < 0 ? err : 0;

	return augmented__output(args, augmented_args, len + size);
failure:
	return 1; /* Failure: don't filter */
}

static pid_t getpid(void)
{
	return bpf_get_current_pid_tgid();
}

/*
 * Checks if a PID is explicitly excluded/filtered out (e.g., via --filter-pids).
 *
 * Children of a filtered task are added to the map by sched_process_fork()
 * below, so a plain lookup is all that is needed here.
 */
static bool pid_filter__has(struct pids_filtered *pids, pid_t pid)
{
	/*
	 * Fast path: this runs for every syscall on the system, so when no pid
	 * is filtered do no work at all rather than failing a lookup.
	 */
	if (!has_pids_filtered)
		return false;

	return bpf_map_lookup_elem(pids, &pid) != NULL;
}

/*
 * Checks if the current task is targeted for tracing.
 *
 * Every thread that existed when tracing started was named by the target and
 * inserted from userspace, and every task created since was inserted by
 * sched_process_fork() below, before it was able to run. So there is nothing
 * to derive here, and in particular no need to consult the tgid or walk to the
 * parent: a task is traced if and only if it is in the map.
 */
static inline bool pid_to_trace__has(pid_t pid)
{
	return bpf_map_lookup_elem(&pids_to_trace, &pid) != NULL;
}

/*
 * Determines if a syscall should be traced based on the filter map:
 * - When not_syscalls_to_trace is true: blacklist mode (trace if NOT in map).
 * - When not_syscalls_to_trace is false: whitelist mode (trace ONLY if IN map).
 */
static inline bool syscall_to_trace__enabled(int id)
{
	bool in_map = bpf_map_lookup_elem(&syscalls_to_trace, &id) != NULL;

	if (not_syscalls_to_trace)
		return !in_map;
	return in_map;
}

u64 ZERO = 0;

/*
 * Determine what type of argument and how many bytes to read from user space, using the
 * value in the beauty_map. This is the relation of parameter type and its corresponding
 * value in the beauty map, and how many bytes we read eventually:
 *
 * string: 1			      -> size of string
 * struct: size of struct	      -> size of struct
 * buffer: -1 * (index of paired len) -> value of paired len (maximum: TRACE_AUG_MAX_BUF)
 */
static inline int augment_arg(struct syscall_enter_args *args, int i,
			      unsigned int *beauty_map,
			      struct beauty_payload_enter *payload, u64 offset)
{
	int index, value_size = sizeof(struct augmented_arg) - offsetof(struct augmented_arg, value);
	int read_err = 0;
	struct augmented_arg *payload_offset;
	s64 aug_size, size;
	bool augmented;
	void *arg;

	arg = (void *)args->args[i];
	augmented = false;
	size = beauty_map[i];
	aug_size = size; /* size of the augmented data read from user space */

	if (size == 0 || arg == NULL)
		return 0;

	/* bounds check for the verifier */
	if (offset > sizeof(payload->aug_args) - sizeof(payload->aug_args[0]))
		return -1;
	barrier_var(offset);
	payload_offset = (struct augmented_arg *)((void *)&payload->aug_args + offset);

	if (size == 1) { /* string */
		aug_size = bpf_probe_read_user_str(payload_offset->value, value_size, arg);
		/* minimum of 0 to pass the verifier */
		if (aug_size < 0) {
			/*
			 * Record why nothing was read. The header sits in
			 * scratch that the next syscall on this CPU reuses,
			 * so an error left unwritten is the previous one's.
			 * No beautifier reads it yet, beauty.h still calls
			 * the field int_arg, so the string is still shown
			 * as an empty one.
			 */
			read_err = aug_size;
			aug_size = 0;
		}

		augmented = true;
	} else if (size > 0 && size <= value_size) { /* struct */
		if (!bpf_probe_read_user(payload_offset->value, size, arg))
			augmented = true;
	} else if ((int)size < 0 && size >= -6) { /* buffer */
		index = -(size + 1);
		barrier_var(index); // Prevent clang (noticed with v18) from removing the &= 7 trick.
		index &= 7;	    // Satisfy the bounds checking with the verifier in some kernels.
		aug_size = args->args[index] > TRACE_AUG_MAX_BUF ? TRACE_AUG_MAX_BUF : args->args[index];

		if (aug_size > 0) {
			if (!bpf_probe_read_user(payload_offset->value, aug_size, arg))
				augmented = true;
		}
	}

	/* Augmented data size is limited to sizeof(augmented_arg->unnamed union with value field) */
	if (aug_size > value_size)
		aug_size = value_size;

	/* write data to payload */
	if (augmented) {
		int written = offsetof(struct augmented_arg, value) + aug_size;

		if (written < 0 || written > sizeof(struct augmented_arg))
			return -1;

		payload_offset->size = aug_size;
		payload_offset->err = read_err;
		return written;
	}

	return 0;
}

static int augment_sys_enter(void *ctx, struct syscall_enter_args *args)
{
	bool do_output = false;
	int i, zero = 0, written;
	u64 output = 0; /* has to be u64, otherwise it won't pass the verifier */
	unsigned int nr, *beauty_map;
	struct beauty_payload_enter *payload;

	/* fall back to do predefined tail call */
	if (args == NULL)
		return 1;

	/* use syscall number to get beauty_map entry */
	nr             = (__u32)args->syscall_nr;
	beauty_map     = bpf_map_lookup_elem(&beauty_map_enter, &nr);

	/* set up payload for output */
	payload        = bpf_map_lookup_elem(&beauty_payload_enter_map, &zero);

	if (beauty_map == NULL || payload == NULL)
		return 1;

	/* copy the sys_enter header, which has the syscall_nr */
	__builtin_memcpy(&payload->args, args, sizeof(struct syscall_enter_args));

	if (bpf_ksym_exists(bpf_iter_num_new)) {
		bpf_for(i, 0, 6) {
			written = augment_arg(args, i, beauty_map, payload, output);
			if (written < 0)
				return 1;
			if (written > 0) {
				output += written;
				/*
				 * guide the verifier to forget range of `output`, which
				 * helps to prove convergence of the loop
				 */
				output += ZERO;
				do_output = true;
			}
		}
	} else {
		for (i = 0; i < 6; i++) {
			written = augment_arg(args, i, beauty_map, payload, output);
			if (written < 0)
				return 1;
			if (written > 0) {
				output += written;
				do_output = true;
			}
		}
	}

	if (!do_output || (sizeof(struct syscall_enter_args) + output) > sizeof(struct beauty_payload_enter))
		return 1;

	return augmented__beauty_output(ctx, payload, sizeof(struct syscall_enter_args) + output);
}

/*
 * Main raw_syscalls:sys_enter tracepoint handler.
 * Always returns 1 so the tracepoint is never vetoed in the kernel for
 * other concurrent listeners. Filtered events simply do not output to the ring buffer.
 */
SEC("tp/raw_syscalls/sys_enter")
int sys_enter(struct syscall_enter_args *args)
{
	struct augmented_args_payload *augmented_args;
	/*
	 * We start len, the amount of data that will be in the perf ring
	 * buffer, if this is not filtered out by one of pid_filter__has(),
	 * syscall->enabled, etc, with the non-augmented raw syscall payload,
	 * i.e. sizeof(augmented_args->args).
	 *
	 * We'll add to this as we add augmented syscalls right after that
	 * initial, non-augmented raw_syscalls:sys_enter payload.
	 */

	if (has_pids_to_trace && !pid_to_trace__has(getpid()))
		return 1;

	if (pid_filter__has(&pids_filtered, getpid()))
		return 1;

	augmented_args = augmented_args_payload();
	if (augmented_args == NULL)
		return 1;

	bpf_probe_read_kernel(&augmented_args->args, sizeof(augmented_args->args), args);

	if (has_syscalls_to_trace && !syscall_to_trace__enabled(augmented_args->args.syscall_nr))
		return 1;

	/*
	 * Jump to syscall specific augmenter. If augmented, augment_sys_enter()
	 * outputs the payload to __augmented_syscalls__ and returns 0.
	 * Return 1 so we never veto the kernel tracepoint for other listeners.
	 */
	if (augment_sys_enter(args, &augmented_args->args) == 0)
		return 1;

	bpf_tail_call(args, &syscalls_sys_enter, augmented_args->args.syscall_nr);

	/*
	 * If not found on the PROG_ARRAY syscalls map, return 1 so we
	 * don't veto the tracepoint event system-wide for other concurrent
	 * listeners.
	 */
	return 1;
}

/*
 * Main raw_syscalls:sys_exit tracepoint handler.
 * Always returns 1 so the tracepoint is never vetoed in the kernel.
 */
SEC("tp/raw_syscalls/sys_exit")
int sys_exit(struct syscall_exit_args *args)
{
	struct syscall_exit_args exit_args;

	if (has_pids_to_trace && !pid_to_trace__has(getpid()))
		return 1;

	/*
	 * Returning 1 leaves the record in place rather than vetoing the
	 * tracepoint for every other listener. The pids named by
	 * --filter-pids are also excluded by the raw_syscalls:sys_exit
	 * evsel's own tracepoint filter, so they are dropped there.
	 * Descendants added to pids_filtered by sched_process_fork() below
	 * are not in that static filter, so their sys_exit is still
	 * delivered and printed without a matching enter. A tracepoint
	 * program can only suppress an event by vetoing it, which is what
	 * this series removes; routing sys_exit through
	 * __augmented_syscalls__ the way sys_enter now is would close this.
	 */
	if (pid_filter__has(&pids_filtered, getpid()))
		return 1;

	bpf_probe_read_kernel(&exit_args, sizeof(exit_args), args);

	/*
	 * Jump to syscall specific return augmenter, even if the default one,
	 * "!raw_syscalls:unaugmented" that will just return 1 to return the
	 * unaugmented tracepoint payload.
	 */
	bpf_tail_call(args, &syscalls_sys_exit, exit_args.syscall_nr);
	/*
	 * If not found on the PROG_ARRAY syscalls map, return 1 so we
	 * don't veto the tracepoint event system-wide for other concurrent
	 * listeners. perf trace's own evsel filter will discard non-matching
	 * syscalls.
	 */
	return 1;
}

/*
 * Propagate tracing to a newly created task.
 *
 * tp_btf/sched_process_fork is raised by copy_process(), in the parent's
 * context and before the child is woken, so the child is in the maps before it
 * can issue its first syscall. That removes the need to inspect real_parent
 * when a syscall is seen from an unknown task, which could neither tell a
 * genuine descendant from a task merely reparented to a traced init, nor keep
 * following a descendant whose parent had already exited.
 *
 * Using tp_btf rather than tp/sched/sched_process_fork avoids depending on the
 * tracepoint ring-buffer record layout (TP_STRUCT__entry), which changed in
 * Linux 6.16 when parent_comm and child_comm were converted from fixed 16-byte
 * arrays to 4-byte __data_loc strings (shrinking the tracepoint context from
 * 48 to 24 bytes and causing BPF_PROG_TYPE_TRACEPOINT attachment to fail with
 * -EACCES when accessing higher offsets). Instead, tp_btf receives the stable
 * TP_PROTO arguments (struct task_struct *parent, struct task_struct *child)
 * directly.
 *
 * child->pid is task_struct.pid, i.e. the pid in the initial namespace, which
 * is what the maps are keyed by. A clone() return value, in contrast, is the
 * pid in the caller's namespace and would alias an unrelated host task when a
 * containerised workload is traced.
 *
 * CLONE_THREAD needs no special handling: a new thread arrives here like any
 * other task and is inserted under its own pid.
 */
SEC("tp_btf/sched_process_fork")
int BPF_PROG(sched_process_fork, struct task_struct *parent, struct task_struct *child)
{
	pid_t parent_pid, child_pid;
	bool val = true;

	if (!inherit)
		return 0;

	/*
	 * Inherit from the thread that called clone() and from nothing else.
	 * The maps name individual tasks: pid_to_trace__has() and
	 * pid_filter__has() look up a task's own pid and nothing more, and
	 * every thread of a -p target is enumerated from /proc/<pid>/task and
	 * inserted in its own right, so a traced thread is always here under
	 * its own key. Consulting the parent's tgid as well would let a child
	 * inherit from a thread that is not itself traced, which is precisely
	 * what 'perf trace -t <tid>' asked to leave out, and would re-admit
	 * descendants of a thread that sched_process_fork() skipped or that
	 * was missed while the programs were being attached, while still not
	 * tracing that thread itself.
	 */
	parent_pid = parent->pid;
	child_pid = child->pid;

	if (has_pids_to_trace &&
	    bpf_map_lookup_elem(&pids_to_trace, &parent_pid) != NULL)
		bpf_map_update_elem(&pids_to_trace, &child_pid, &val, BPF_ANY);

	if (has_pids_filtered &&
	    bpf_map_lookup_elem(&pids_filtered, &parent_pid) != NULL)
		bpf_map_update_elem(&pids_filtered, &child_pid, &val, BPF_ANY);

	return 0;
}

/*
 * Drop a dying task from the maps.
 *
 * tp_btf/sched_process_exit is raised by do_exit() for every task, in its own
 * context, so unlike hooking the exit and exit_group syscalls this also covers
 * tasks killed by a signal and threads torn down implicitly by exit_group.
 *
 * Doing it here rather than from the userspace PERF_RECORD_EXIT handler also
 * means there is no window between the task dying and the map being updated,
 * during which the kernel could recycle the pid and the late eviction silently
 * stop tracing whichever new task received it.
 *
 * Each thread is reported separately, including the group leader, whose pid is
 * the thread group's tgid, so one delete per map covers both uses of the key.
 */
SEC("tp_btf/sched_process_exit")
int BPF_PROG(sched_process_exit, struct task_struct *p)
{
	pid_t pid = p->pid;

	bpf_map_delete_elem(&pids_to_trace, &pid);
	bpf_map_delete_elem(&pids_filtered, &pid);

	return 0;
}

/*
 * Follow a task whose pid changed under it.
 *
 * When a thread that is not the thread group leader execs, de_thread() kills
 * the rest of the group and then hands the leader's pid, which is the tgid, to
 * the exec'ing thread. The leader dies first, so sched_process_exit() above
 * has already dropped that pid from the maps, and the survivor is now keyed by
 * a pid nothing knows about while its original entry is left behind for good.
 *
 * Move the entry across so the task stays tracked and nothing is leaked.
 * old_pid is sampled in bprm_execve() before de_thread() runs, so for the
 * common case of the group leader exec'ing it simply equals p->pid and there
 * is nothing to do.
 */
SEC("tp_btf/sched_process_exec")
int BPF_PROG(sched_process_exec, struct task_struct *p, pid_t old_pid)
{
	pid_t pid = p->pid;
	bool val = true;

	if (pid == old_pid)
		return 0;

	/*
	 * Drop the old key before adding the new one. The maps are bounded and
	 * the move is only ever a rename, but inserting first needs a spare
	 * slot for as long as both keys are present: on a full map that insert
	 * fails with -E2BIG while the delete still succeeds, which would lose
	 * the task rather than move it. Deleting first frees the slot the
	 * insert goes on to use. The task is mid exec and issues no syscalls in
	 * between, so the gap is not observable.
	 *
	 * There is no atomic rename for a hash map, so on a map that is exactly
	 * full this narrows the window rather than closing it: a fork on
	 * another CPU can still take the freed slot before the insert below
	 * runs, and the task is then dropped just as any other task is once the
	 * map is full.
	 *
	 * Nothing else takes old_pid out of the maps. sched_process_exit() ran
	 * for the group leader under the leader's own pid, before de_thread()
	 * swapped the two, so skipping the delete would leave old_pid behind
	 * as an entry for a task that no longer holds it.
	 *
	 * old_pid may already belong to somebody else by the time this runs. A
	 * non leader thread getting here has been through de_thread(), which
	 * hands it the leader's pid with exchange_tids() and then
	 * release_task()s the leader, freeing the pid the thread started with.
	 * That is inside the binfmt loader, whereas the tracepoint only fires
	 * once the whole of exec_binprm() is done, so the pid is up for reuse
	 * for the length of an exec. Should a task created in that window be
	 * given it, and be one that is traced or filtered itself, it is
	 * inserted under this same key and the move below carries its entry
	 * across rather than this task's: it stops being reported and this task
	 * takes its place.
	 *
	 * That is left as it is. An entry records only that some task with that
	 * pid was of interest, and the tracepoint offers no other handle on the
	 * task old_pid was taken from, so the two cannot be told apart here.
	 * Reuse needs the cyclic pid allocator to come the whole way round to
	 * old_pid inside that window, and a task cannot simply ask for the pid
	 * it wants: clone3()'s set_tid is gated on CAP_CHECKPOINT_RESTORE.
	 * Whichever task wins, both are ones the session was already following,
	 * so what is at stake is which of them gets reported rather than
	 * whether tracing reaches beyond what was asked for.
	 */
	if (bpf_map_lookup_elem(&pids_to_trace, &old_pid) != NULL) {
		bpf_map_delete_elem(&pids_to_trace, &old_pid);
		bpf_map_update_elem(&pids_to_trace, &pid, &val, BPF_ANY);
	}

	if (bpf_map_lookup_elem(&pids_filtered, &old_pid) != NULL) {
		bpf_map_delete_elem(&pids_filtered, &old_pid);
		bpf_map_update_elem(&pids_filtered, &pid, &val, BPF_ANY);
	}

	return 0;
}

char _license[] SEC("license") = "GPL";

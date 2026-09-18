#ifndef TRACE_AUGMENT_H
#define TRACE_AUGMENT_H

#include <linux/compiler.h>
#include <stdbool.h>
#include <sys/types.h>

struct bpf_program;
struct evlist;

#ifdef HAVE_BPF_SKEL

int augmented_syscalls__prepare(bool inherit);
int augmented_syscalls__attach(void);
int augmented_syscalls__create_bpf_output(struct evlist *evlist);
void augmented_syscalls__setup_bpf_output(void);
int augmented_syscalls__set_filter_pids(unsigned int nr, pid_t *pids);
int augmented_syscalls__set_target_pids(unsigned int nr, pid_t *pids);
int augmented_syscalls__prune_dead_pids(bool (*is_alive)(pid_t pid));
int augmented_syscalls__set_target_syscalls(unsigned int nr, int *syscall_ids, bool not_syscalls);
int augmented_syscalls__get_map_fds(int *enter_fd, int *exit_fd, int *beauty_fd);
struct bpf_program *augmented_syscalls__find_by_title(const char *name);
struct bpf_program *augmented_syscalls__unaugmented(void);
void augmented_syscalls__cleanup(void);

#else /* !HAVE_BPF_SKEL */

static inline int augmented_syscalls__prepare(bool inherit __maybe_unused)
{
	return -1;
}

static inline int augmented_syscalls__attach(void)
{
	return 0;
}

static inline int augmented_syscalls__create_bpf_output(struct evlist *evlist __maybe_unused)
{
	return -1;
}

static inline void augmented_syscalls__setup_bpf_output(void)
{
}

static inline int augmented_syscalls__set_filter_pids(unsigned int nr __maybe_unused,
						      pid_t *pids __maybe_unused)
{
	return 0;
}

static inline int augmented_syscalls__set_target_pids(unsigned int nr __maybe_unused,
						      pid_t *pids __maybe_unused)
{
	return 0;
}

static inline int augmented_syscalls__prune_dead_pids(bool (*is_alive)(pid_t pid) __maybe_unused)
{
	return 0;
}

static inline int augmented_syscalls__set_target_syscalls(unsigned int nr __maybe_unused,
							  int *syscall_ids __maybe_unused,
							  bool not_syscalls __maybe_unused)
{
	return 0;
}

static inline int augmented_syscalls__get_map_fds(int *enter_fd __maybe_unused,
						  int *exit_fd __maybe_unused,
						  int *beauty_fd __maybe_unused)
{
	return -1;
}

static inline struct bpf_program *
augmented_syscalls__find_by_title(const char *name __maybe_unused)
{
	return NULL;
}

static inline struct bpf_program *augmented_syscalls__unaugmented(void)
{
	return NULL;
}

static inline void augmented_syscalls__cleanup(void)
{
}

#endif /* HAVE_BPF_SKEL */

#endif

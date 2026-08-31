// SPDX-License-Identifier: GPL-2.0
#ifndef _BPF_MONITOR_H
#define _BPF_MONITOR_H

#ifdef HAVE_LIBBPF
int bpf_run_monitor(char *monitor_name, int argc, char **argv);
#else
static inline int bpf_run_monitor(char *monitor_name, int argc, char **argv)
{
	return 0;
}
#endif /* HAVE_LIBBPF */

#endif

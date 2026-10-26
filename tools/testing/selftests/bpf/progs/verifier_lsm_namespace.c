// SPDX-License-Identifier: GPL-2.0

#include <vmlinux.h>
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_tracing.h>

#include "bpf_misc.h"
#include "../test_kmods/bpf_testmod_kfunc.h"

/* The namespace hasn't been set up yet. */
SEC("lsm/namespace_init")
__failure __msg("R1 must be referenced or trusted")
int BPF_PROG(namespace_init_untrusted, struct ns_common *ns)
{
	bpf_kfunc_trusted_ns_test(ns);
	return 0;
}

/* The namespace is being torn down. */
SEC("lsm/namespace_free")
__failure __msg("R1 must be referenced or trusted")
int BPF_PROG(namespace_free_untrusted, struct ns_common *ns)
{
	bpf_kfunc_trusted_ns_test(ns);
	return 0;
}

/* setns() holds a reference to a fully set up namespace. */
SEC("lsm/namespace_install")
__success
int BPF_PROG(namespace_install_trusted, const struct nsset *nsset,
	     struct ns_common *ns)
{
	bpf_kfunc_trusted_ns_test(ns);
	return 0;
}

char _license[] SEC("license") = "GPL";

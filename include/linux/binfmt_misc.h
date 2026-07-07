/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_BINFMT_MISC_H
#define _LINUX_BINFMT_MISC_H

#include <linux/types.h>

struct bpf_prog;
struct linux_binprm;
struct user_namespace;

#define BINFMT_MISC_OPS_NAME_MAX 16

/**
 * struct binfmt_misc_ops - bpf-backed binary type handler
 * @load: match @bprm and select an interpreter via bpf_binprm_set_interp();
 *        returns > 0 if the binary was handled, 0 to fall through to the
 *        handlers registered after this one, a negative errno to fail the
 *        exec; -ENOEXEC does not fail the exec but moves on to the
 *        remaining binary formats
 * @name: name that 'B' entries reference the handler by
 */
struct binfmt_misc_ops {
	int (*load)(struct linux_binprm *bprm);
	char name[BINFMT_MISC_OPS_NAME_MAX];
};

#ifdef CONFIG_BINFMT_MISC_BPF
const struct binfmt_misc_ops *binfmt_misc_get_ops(struct user_namespace *user_ns,
						  const char *name);
void binfmt_misc_put_ops(const struct binfmt_misc_ops *ops);
bool bpf_prog_is_binfmt_misc_ops(const struct bpf_prog *prog);
#else
static inline const struct binfmt_misc_ops *
binfmt_misc_get_ops(struct user_namespace *user_ns, const char *name)
{
	return NULL;
}

static inline void binfmt_misc_put_ops(const struct binfmt_misc_ops *ops)
{
}

static inline bool bpf_prog_is_binfmt_misc_ops(const struct bpf_prog *prog)
{
	return false;
}
#endif /* CONFIG_BINFMT_MISC_BPF */

#endif /* _LINUX_BINFMT_MISC_H */

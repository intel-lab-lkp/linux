/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2023-2024 Huawei Technologies Duesseldorf GmbH
 *
 * Author: Petr Tesarik <petr.tesarik1@huawei-partners.com>
 *
 * SandBox Mode (SBM) public API declarations.
 */
#ifndef __LINUX_SBM_H
#define __LINUX_SBM_H

/**
 * struct sbm - SandBox Mode instance.
 * @error:    Error code. Initialized to zero by sbm_init() and updated when
 *            a SBM operation fails.
 * @private:  Arch-specific private data.
 */
struct sbm {
#ifdef CONFIG_SANDBOX_MODE
	int error;
	void *private;
#endif
};

/**
 * typedef sbm_func - Sandbox mode function pointer.
 * @data:  Arbitrary data passed via sbm_exec().
 *
 * Return: Zero on success, negative on error.
 */
typedef int (*sbm_func)(void *data);

#ifdef CONFIG_SANDBOX_MODE

/**
 * sbm_init() - Initialize a SandBox Mode instance.
 * @sbm:     SBM instance.
 *
 * Initialize a SBM instance structure.
 *
 * Return: Zero on success, negative on error.
 */
int sbm_init(struct sbm *sbm);

/**
 * sbm_destroy() - Clean up a SandBox Mode instance.
 * @sbm:    SBM instance to be cleaned up.
 */
void sbm_destroy(struct sbm *sbm);

/**
 * sbm_error() - Get SBM error status.
 * @sbm:  SBM instance.
 *
 * Get the SBM error code. This can be used to distinguish between
 * errors returned by the target function and errors from setting
 * up the sandbox environment.
 */
static inline int sbm_error(const struct sbm *sbm)
{
	return sbm->error;
}

/**
 * sbm_exec() - Execute function in a sandbox.
 * @sbm:   SBM instance.
 * @func:  Function to be called.
 * @data:  Argument for @func.
 *
 * Execute @func in a fully prepared SBM instance.
 *
 * Return: Return value of @func on success, or a negative error code.
 */
int sbm_exec(struct sbm *sbm, sbm_func func, void *data);

#ifdef CONFIG_HAVE_ARCH_SBM

/**
 * arch_sbm_init() - Arch hook to initialize a SBM instance.
 * @sbm:  Instance to be initialized.
 *
 * Perform any arch-specific initialization. This hook is called by sbm_init()
 * immediately after zeroing out @sbm.
 *
 * Return: Zero on success, negative error code on failure.
 */
int arch_sbm_init(struct sbm *sbm);

/**
 * arch_sbm_destroy() - Arch hook to clean up a SBM instance.
 * @sbm:  Instance to be cleaned up.
 *
 * Perform any arch-specific cleanup. This hook is called by sbm_destroy() as
 * the very last operation on @sbm.
 */
void arch_sbm_destroy(struct sbm *sbm);

/**
 * arch_sbm_exec() - Arch hook to execute code in a sandbox.
 * @sbm:   SBM instance.
 * @func:  Function to be executed in a sandbox.
 * @data:  Argument passed to @func.
 *
 * Execute @func in a fully prepared SBM instance. If sandbox mode
 * cannot be set up or is aborted, set &sbm->error to a negative error
 * value. This error is then returned by sbm_exec(), overriding the
 * return value of arch_sbm_exec().
 *
 * Return: Return value of @func.
 */
int arch_sbm_exec(struct sbm *sbm, sbm_func func, void *data);

#else /* !CONFIG_HAVE_ARCH_SBM */

static inline int arch_sbm_init(struct sbm *sbm)
{
	return 0;
}

static inline void arch_sbm_destroy(struct sbm *sbm)
{
}

static inline int arch_sbm_exec(struct sbm *sbm, sbm_func func, void *data)
{
	return func(data);
}

#endif /* CONFIG_HAVE_ARCH_SBM */

#else /* !CONFIG_SANDBOX_MODE */

static inline int sbm_init(struct sbm *sbm)
{
	return 0;
}

static inline void sbm_destroy(struct sbm *sbm)
{
}

static inline int sbm_error(const struct sbm *sbm)
{
	return 0;
}

static inline int sbm_exec(struct sbm *sbm, sbm_func func, void *data)
{
	return func(data);
}

#endif /* CONFIG_SANDBOX_MODE */

#endif /* __LINUX_SBM_H */

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

struct sbm_buf;

/**
 * struct sbm - SandBox Mode instance.
 * @error:    Error code. Initialized to zero by sbm_init() and updated when
 *            a SBM operation fails.
 * @private:  Arch-specific private data.
 * @input:    Input data. Copied to a temporary buffer before starting sandbox
 *            mode.
 * @output:   Output data. Copied from a temporary buffer after return from
 *            sandbox mode.
 * @io:       Input and output data. Copied to a temporary buffer before
 *            starting sandbox mode and copied back after return.
 */
struct sbm {
#ifdef CONFIG_SANDBOX_MODE
	int error;
	void *private;
	struct sbm_buf *input;
	struct sbm_buf *output;
	struct sbm_buf *io;
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

/**
 * struct sbm_buf - Description of an input/output buffer.
 * @next:      Pointer to the next buffer in the list.
 * @kern_ptr:  Buffer address in kernel mode.
 * @sbm_ptr:   Buffer address in sandbox mode.
 * @size:      Size of the buffer.
 */
struct sbm_buf {
	struct sbm_buf *next;
	void *kern_ptr;
	void *sbm_ptr;
	size_t size;
};

/**
 * sbm_alloc_buf() - Allocate a new input/output buffer.
 * @sbm:   SBM instance.
 * @size:  Size of the buffer.
 *
 * Allocate a new &struct sbm_buf and the corresponding sandbox mode
 * input/output buffer. If either allocation fails, update &sbm->error.
 *
 * Return: New buffer descriptor, or %NULL on allocation failure.
 */
struct sbm_buf *sbm_alloc_buf(struct sbm *sbm, size_t size);

/**
 * sbm_add_buf() - Add a new I/O buffer to the SBM instance.
 * @sbm:   SBM instance.
 * @list:  Target argument buffer list.
 * @buf:   Buffer virtual address.
 * @size:  Size of the buffer.
 *
 * Add a new buffer to @list.
 *
 * Return: SBM address of the buffer, or %NULL on error.
 */
static inline void *sbm_add_buf(struct sbm *sbm, struct sbm_buf **list,
				const void *buf, size_t size)
{
	struct sbm_buf *io;

	io = sbm_alloc_buf(sbm, size);
	if (!io)
		return NULL;

	io->kern_ptr = (void *)buf;
	io->next = *list;
	*list = io;
	return io->sbm_ptr;
}

/**
 * SBM_COPY_IN() - Mark an input buffer for copying into SBM.
 * @sbm:   SBM instance.
 * @buf:   Buffer virtual address.
 * @size:  Size of the buffer.
 *
 * Add a buffer to the input buffer list for @sbm. The content of the
 * buffer is copied to sandbox mode before calling the target function.
 *
 * It is OK to modify the input buffer after invoking this macro.
 *
 * Return: Buffer address in sandbox mode.
 */
#define SBM_COPY_IN(sbm, buf, size) \
	((typeof(({buf; })))sbm_add_buf((sbm), &(sbm)->input, (buf), (size)))

/**
 * SBM_COPY_OUT() - Mark an output buffer for copying out of SBM.
 * @sbm:   SBM instance.
 * @buf:   Buffer virtual address.
 * @size:  Size of the buffer.
 *
 * Add a buffer to the output buffer list for @sbm. The content of the
 * buffer is copied to kernel mode after calling the target function.
 *
 * Return: Buffer address in sandbox mode.
 */
#define SBM_COPY_OUT(sbm, buf, size) \
	((typeof(({buf; })))sbm_add_buf((sbm), &(sbm)->output, (buf), (size)))

/**
 * SBM_COPY_INOUT() - Mark an input buffer for copying into SBM and out of SBM.
 * @sbm:   SBM instance.
 * @buf:   Buffer virtual address.
 * @size:  Size of the buffer.
 *
 * Add a buffer to the input and output buffer list for @sbm. The content
 * of the buffer is copied to sandbox mode before calling the target function
 * and copied back to kernel mode after the call.
 *
 * Return: Buffer address in sandbox mode.
 */
#define SBM_COPY_INOUT(sbm, buf, size) \
	((typeof(({buf; })))sbm_add_buf((sbm), &(sbm)->io, (buf), (size)))

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
 * arch_sbm_map_readonly() - Arch hook to map a buffer for reading.
 * @sbm:  SBM instance.
 * @buf:  Buffer to be mapped.
 *
 * Make the specified buffer readable by sandbox code. See also
 * arch_sbm_map_writable().
 *
 * Return: Zero on success, negative on error.
 */
int arch_sbm_map_readonly(struct sbm *sbm, const struct sbm_buf *buf);

/**
 * arch_sbm_map_writable() - Arch hook to map a buffer for reading and writing.
 * @sbm:  SBM instance.
 * @buf:  Buffer to be mapped.
 *
 * Make the specified buffer readable and writable by sandbox code.
 * See also arch_sbm_map_readonly().
 *
 * Return: Zero on success, negative on error.
 */
int arch_sbm_map_writable(struct sbm *sbm, const struct sbm_buf *buf);

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

static inline int arch_sbm_map_readonly(struct sbm *sbm,
					const struct sbm_buf *buf)
{
	return 0;
}

static inline int arch_sbm_map_writable(struct sbm *sbm,
					const struct sbm_buf *buf)
{
	return 0;
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

/* Evaluate expression exactly once, avoiding warnings about a "statement
 * with no effect". GCC doesn't issue this warning for the return value
 * of a statement expression.
 */
#define __SBM_EVAL(x) ({ typeof(({x; })) __tmp = (x); __tmp; })

#define SBM_COPY_IN(sbm, buf, size)  __SBM_EVAL(buf)
#define SBM_COPY_OUT(sbm, buf, size) __SBM_EVAL(buf)
#define SBM_COPY_INOUT(sbm, buf, size) __SBM_EVAL(buf)

#endif /* CONFIG_SANDBOX_MODE */

#endif /* __LINUX_SBM_H */

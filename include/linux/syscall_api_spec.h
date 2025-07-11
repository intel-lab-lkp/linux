/* SPDX-License-Identifier: GPL-2.0 */
/*
 * syscall_api_spec.h - System Call API Specification Integration
 *
 * This header extends the SYSCALL_DEFINEX macros to support inline API specifications,
 * allowing syscall documentation to be written alongside the implementation in a
 * human-readable and machine-parseable format.
 */

#ifndef _LINUX_SYSCALL_API_SPEC_H
#define _LINUX_SYSCALL_API_SPEC_H

#include <linux/kernel_api_spec.h>



/* Automatic syscall validation infrastructure */
/* 
 * The validation is now integrated directly into the SYSCALL_DEFINEx macros
 * in syscalls.h when CONFIG_KAPI_RUNTIME_CHECKS is enabled.
 * 
 * The validation happens in the __do_kapi_sys##name wrapper function which:
 * 1. Validates all parameters before calling the actual syscall
 * 2. Calls the real syscall implementation
 * 3. Validates the return value
 * 4. Returns the result
 */


/*
 * Helper macros for common syscall patterns
 */

/* For syscalls that can sleep */
#define KAPI_SYSCALL_SLEEPABLE \
	KAPI_CONTEXT(KAPI_CTX_PROCESS | KAPI_CTX_SLEEPABLE)

/* For syscalls that must be atomic */
#define KAPI_SYSCALL_ATOMIC \
	KAPI_CONTEXT(KAPI_CTX_PROCESS | KAPI_CTX_ATOMIC)

/* Common parameter specifications */
#define KAPI_PARAM_FD(idx, desc) \
	KAPI_PARAM(idx, "fd", "int", desc) \
		KAPI_PARAM_FLAGS(KAPI_PARAM_IN) \
		.type = KAPI_TYPE_FD, \
		.constraint_type = KAPI_CONSTRAINT_NONE, \
	KAPI_PARAM_END

#define KAPI_PARAM_USER_BUF(idx, name, desc) \
	KAPI_PARAM(idx, name, "void __user *", desc) \
		KAPI_PARAM_FLAGS(KAPI_PARAM_USER_PTR | KAPI_PARAM_IN) \
	KAPI_PARAM_END

#define KAPI_PARAM_USER_STRUCT(idx, name, struct_type, desc) \
	KAPI_PARAM(idx, name, #struct_type " __user *", desc) \
		KAPI_PARAM_FLAGS(KAPI_PARAM_USER | KAPI_PARAM_IN) \
		.type = KAPI_TYPE_USER_PTR, \
		.size = sizeof(struct_type), \
		.constraint_type = KAPI_CONSTRAINT_NONE, \
	KAPI_PARAM_END

#define KAPI_PARAM_SIZE_T(idx, name, desc) \
	KAPI_PARAM(idx, name, "size_t", desc) \
		KAPI_PARAM_FLAGS(KAPI_PARAM_IN) \
		KAPI_PARAM_RANGE(0, SIZE_MAX) \
	KAPI_PARAM_END

/* Common error specifications */
#define KAPI_ERROR_EBADF(idx) \
	KAPI_ERROR(idx, -EBADF, "EBADF", "Invalid file descriptor", \
		   "The file descriptor is not valid or has been closed")

#define KAPI_ERROR_EINVAL(idx, condition) \
	KAPI_ERROR(idx, -EINVAL, "EINVAL", condition, \
		   "Invalid argument provided")

#define KAPI_ERROR_ENOMEM(idx) \
	KAPI_ERROR(idx, -ENOMEM, "ENOMEM", "Insufficient memory", \
		   "Cannot allocate memory for the operation")

#define KAPI_ERROR_EPERM(idx) \
	KAPI_ERROR(idx, -EPERM, "EPERM", "Operation not permitted", \
		   "The calling process does not have the required permissions")

#define KAPI_ERROR_EFAULT(idx) \
	KAPI_ERROR(idx, -EFAULT, "EFAULT", "Bad address", \
		   "Invalid user space address provided")

/* Standard return value specifications */
#define KAPI_RETURN_SUCCESS_ZERO \
	KAPI_RETURN("long", "0 on success, negative error code on failure") \
		KAPI_RETURN_SUCCESS(0, "== 0") \
	KAPI_RETURN_END

#define KAPI_RETURN_FD_SPEC \
	KAPI_RETURN("long", "File descriptor on success, negative error code on failure") \
		.check_type = KAPI_RETURN_FD, \
	KAPI_RETURN_END

#define KAPI_RETURN_COUNT \
	KAPI_RETURN("long", "Number of bytes processed on success, negative error code on failure") \
		KAPI_RETURN_SUCCESS(0, ">= 0") \
	KAPI_RETURN_END

/**
 * KAPI_ERROR_COUNT - Set the error count
 * @count: Number of errors defined
 */
#define KAPI_ERROR_COUNT(count) \
	.error_count = count,

/**
 * KAPI_PARAM_COUNT - Set the parameter count
 * @count: Number of parameters defined
 */
#define KAPI_PARAM_COUNT(count) \
	.param_count = count,

/**
 * KAPI_SINCE_VERSION - Set the since version
 * @version: Version string when the API was introduced
 */
#define KAPI_SINCE_VERSION(version) \
	.since_version = version,


/**
 * KAPI_SIGNAL_MASK_COUNT - Set the signal mask count
 * @count: Number of signal masks defined
 */
#define KAPI_SIGNAL_MASK_COUNT(count) \
	.signal_mask_count = count,



#endif /* _LINUX_SYSCALL_API_SPEC_H */
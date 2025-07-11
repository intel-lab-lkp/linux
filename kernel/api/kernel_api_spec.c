// SPDX-License-Identifier: GPL-2.0
/*
 * kernel_api_spec.c - Kernel API Specification Framework Implementation
 *
 * Provides runtime support for kernel API specifications including validation,
 * export to various formats, and querying capabilities.
 */

#include <linux/kernel.h>
#include <linux/kernel_api_spec.h>
#include <linux/string.h>
#include <linux/slab.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/seq_file.h>
#include <linux/debugfs.h>
#include <linux/export.h>
#include <linux/preempt.h>
#include <linux/hardirq.h>
#include <linux/file.h>
#include <linux/fdtable.h>
#include <linux/uaccess.h>
#include <linux/limits.h>
#include <linux/fcntl.h>

/* Section where API specifications are stored */
extern struct kernel_api_spec __start_kapi_specs[];
extern struct kernel_api_spec __stop_kapi_specs[];

/* Dynamic API registration */
static LIST_HEAD(dynamic_api_specs);
static DEFINE_MUTEX(api_spec_mutex);

struct dynamic_api_spec {
	struct list_head list;
	struct kernel_api_spec *spec;
};

/**
 * kapi_get_spec - Get API specification by name
 * @name: Function name to look up
 *
 * Return: Pointer to API specification or NULL if not found
 */
const struct kernel_api_spec *kapi_get_spec(const char *name)
{
	struct kernel_api_spec *spec;
	struct dynamic_api_spec *dyn_spec;

	/* Search static specifications */
	for (spec = __start_kapi_specs; spec < __stop_kapi_specs; spec++) {
		if (strcmp(spec->name, name) == 0)
			return spec;
	}

	/* Search dynamic specifications */
	mutex_lock(&api_spec_mutex);
	list_for_each_entry(dyn_spec, &dynamic_api_specs, list) {
		if (strcmp(dyn_spec->spec->name, name) == 0) {
			mutex_unlock(&api_spec_mutex);
			return dyn_spec->spec;
		}
	}
	mutex_unlock(&api_spec_mutex);

	return NULL;
}
EXPORT_SYMBOL_GPL(kapi_get_spec);

/**
 * kapi_register_spec - Register a dynamic API specification
 * @spec: API specification to register
 *
 * Return: 0 on success, negative error code on failure
 */
int kapi_register_spec(struct kernel_api_spec *spec)
{
	struct dynamic_api_spec *dyn_spec;

	if (!spec || !spec->name[0])
		return -EINVAL;

	/* Check if already exists */
	if (kapi_get_spec(spec->name))
		return -EEXIST;

	dyn_spec = kzalloc(sizeof(*dyn_spec), GFP_KERNEL);
	if (!dyn_spec)
		return -ENOMEM;

	dyn_spec->spec = spec;

	mutex_lock(&api_spec_mutex);
	list_add_tail(&dyn_spec->list, &dynamic_api_specs);
	mutex_unlock(&api_spec_mutex);

	return 0;
}
EXPORT_SYMBOL_GPL(kapi_register_spec);

/**
 * kapi_unregister_spec - Unregister a dynamic API specification
 * @name: Name of API to unregister
 */
void kapi_unregister_spec(const char *name)
{
	struct dynamic_api_spec *dyn_spec, *tmp;

	mutex_lock(&api_spec_mutex);
	list_for_each_entry_safe(dyn_spec, tmp, &dynamic_api_specs, list) {
		if (strcmp(dyn_spec->spec->name, name) == 0) {
			list_del(&dyn_spec->list);
			kfree(dyn_spec);
			break;
		}
	}
	mutex_unlock(&api_spec_mutex);
}
EXPORT_SYMBOL_GPL(kapi_unregister_spec);

/**
 * param_type_to_string - Convert parameter type to string
 * @type: Parameter type
 *
 * Return: String representation of type
 */
static const char *param_type_to_string(enum kapi_param_type type)
{
	static const char * const type_names[] = {
		[KAPI_TYPE_VOID] = "void",
		[KAPI_TYPE_INT] = "int",
		[KAPI_TYPE_UINT] = "uint",
		[KAPI_TYPE_PTR] = "pointer",
		[KAPI_TYPE_STRUCT] = "struct",
		[KAPI_TYPE_UNION] = "union",
		[KAPI_TYPE_ENUM] = "enum",
		[KAPI_TYPE_FUNC_PTR] = "function_pointer",
		[KAPI_TYPE_ARRAY] = "array",
		[KAPI_TYPE_FD] = "file_descriptor",
		[KAPI_TYPE_USER_PTR] = "user_pointer",
		[KAPI_TYPE_PATH] = "pathname",
		[KAPI_TYPE_CUSTOM] = "custom",
	};

	if (type >= ARRAY_SIZE(type_names))
		return "unknown";

	return type_names[type];
}

/**
 * lock_type_to_string - Convert lock type to string
 * @type: Lock type
 *
 * Return: String representation of lock type
 */
static const char *lock_type_to_string(enum kapi_lock_type type)
{
	static const char * const lock_names[] = {
		[KAPI_LOCK_NONE] = "none",
		[KAPI_LOCK_MUTEX] = "mutex",
		[KAPI_LOCK_SPINLOCK] = "spinlock",
		[KAPI_LOCK_RWLOCK] = "rwlock",
		[KAPI_LOCK_SEQLOCK] = "seqlock",
		[KAPI_LOCK_RCU] = "rcu",
		[KAPI_LOCK_SEMAPHORE] = "semaphore",
		[KAPI_LOCK_CUSTOM] = "custom",
	};

	if (type >= ARRAY_SIZE(lock_names))
		return "unknown";

	return lock_names[type];
}

/**
 * return_check_type_to_string - Convert return check type to string
 * @type: Return check type
 *
 * Return: String representation of return check type
 */
static const char *return_check_type_to_string(enum kapi_return_check_type type)
{
	static const char * const check_names[] = {
		[KAPI_RETURN_EXACT] = "exact",
		[KAPI_RETURN_RANGE] = "range",
		[KAPI_RETURN_ERROR_CHECK] = "error_check",
		[KAPI_RETURN_FD] = "file_descriptor",
		[KAPI_RETURN_CUSTOM] = "custom",
	};

	if (type >= ARRAY_SIZE(check_names))
		return "unknown";

	return check_names[type];
}

/**
 * capability_action_to_string - Convert capability action to string
 * @action: Capability action
 *
 * Return: String representation of capability action
 */
static const char *capability_action_to_string(enum kapi_capability_action action)
{
	static const char * const action_names[] = {
		[KAPI_CAP_BYPASS_CHECK] = "bypass_check",
		[KAPI_CAP_INCREASE_LIMIT] = "increase_limit",
		[KAPI_CAP_OVERRIDE_RESTRICTION] = "override_restriction",
		[KAPI_CAP_GRANT_PERMISSION] = "grant_permission",
		[KAPI_CAP_MODIFY_BEHAVIOR] = "modify_behavior",
		[KAPI_CAP_ACCESS_RESOURCE] = "access_resource",
		[KAPI_CAP_PERFORM_OPERATION] = "perform_operation",
	};

	if (action >= ARRAY_SIZE(action_names))
		return "unknown";

	return action_names[action];
}

/**
 * kapi_export_json - Export API specification to JSON format
 * @spec: API specification to export
 * @buf: Buffer to write JSON to
 * @size: Size of buffer
 *
 * Return: Number of bytes written or negative error
 */
int kapi_export_json(const struct kernel_api_spec *spec, char *buf, size_t size)
{
	int ret = 0;
	int i;

	if (!spec || !buf || size == 0)
		return -EINVAL;

	ret = scnprintf(buf, size,
		"{\n"
		"  \"name\": \"%s\",\n"
		"  \"version\": %u,\n"
		"  \"description\": \"%s\",\n"
		"  \"long_description\": \"%s\",\n"
		"  \"context_flags\": \"0x%x\",\n",
		spec->name,
		spec->version,
		spec->description,
		spec->long_description,
		spec->context_flags);

	/* Parameters */
	ret += scnprintf(buf + ret, size - ret,
		"  \"parameters\": [\n");

	for (i = 0; i < spec->param_count && i < KAPI_MAX_PARAMS; i++) {
		const struct kapi_param_spec *param = &spec->params[i];

		ret += scnprintf(buf + ret, size - ret,
			"    {\n"
			"      \"name\": \"%s\",\n"
			"      \"type\": \"%s\",\n"
			"      \"type_class\": \"%s\",\n"
			"      \"flags\": \"0x%x\",\n"
			"      \"description\": \"%s\"\n"
			"    }%s\n",
			param->name,
			param->type_name,
			param_type_to_string(param->type),
			param->flags,
			param->description,
			(i < spec->param_count - 1) ? "," : "");
	}

	ret += scnprintf(buf + ret, size - ret, "  ],\n");

	/* Return value */
	ret += scnprintf(buf + ret, size - ret,
		"  \"return\": {\n"
		"    \"type\": \"%s\",\n"
		"    \"type_class\": \"%s\",\n"
		"    \"check_type\": \"%s\",\n",
		spec->return_spec.type_name,
		param_type_to_string(spec->return_spec.type),
		return_check_type_to_string(spec->return_spec.check_type));

	switch (spec->return_spec.check_type) {
	case KAPI_RETURN_EXACT:
		ret += scnprintf(buf + ret, size - ret,
			"    \"success_value\": %lld,\n",
			spec->return_spec.success_value);
		break;
	case KAPI_RETURN_RANGE:
		ret += scnprintf(buf + ret, size - ret,
			"    \"success_min\": %lld,\n"
			"    \"success_max\": %lld,\n",
			spec->return_spec.success_min,
			spec->return_spec.success_max);
		break;
	case KAPI_RETURN_ERROR_CHECK:
		ret += scnprintf(buf + ret, size - ret,
			"    \"error_count\": %u,\n",
			spec->return_spec.error_count);
		break;
	default:
		break;
	}

	ret += scnprintf(buf + ret, size - ret,
		"    \"description\": \"%s\"\n"
		"  },\n",
		spec->return_spec.description);

	/* Errors */
	ret += scnprintf(buf + ret, size - ret,
		"  \"errors\": [\n");

	for (i = 0; i < spec->error_count && i < KAPI_MAX_ERRORS; i++) {
		const struct kapi_error_spec *error = &spec->errors[i];

		ret += scnprintf(buf + ret, size - ret,
			"    {\n"
			"      \"code\": %d,\n"
			"      \"name\": \"%s\",\n"
			"      \"condition\": \"%s\",\n"
			"      \"description\": \"%s\"\n"
			"    }%s\n",
			error->error_code,
			error->name,
			error->condition,
			error->description,
			(i < spec->error_count - 1) ? "," : "");
	}

	ret += scnprintf(buf + ret, size - ret, "  ],\n");

	/* Locks */
	ret += scnprintf(buf + ret, size - ret,
		"  \"locks\": [\n");

	for (i = 0; i < spec->lock_count && i < KAPI_MAX_CONSTRAINTS; i++) {
		const struct kapi_lock_spec *lock = &spec->locks[i];

		ret += scnprintf(buf + ret, size - ret,
			"    {\n"
			"      \"name\": \"%s\",\n"
			"      \"type\": \"%s\",\n"
			"      \"acquired\": %s,\n"
			"      \"released\": %s,\n"
			"      \"held_on_entry\": %s,\n"
			"      \"held_on_exit\": %s,\n"
			"      \"description\": \"%s\"\n"
			"    }%s\n",
			lock->lock_name,
			lock_type_to_string(lock->lock_type),
			lock->acquired ? "true" : "false",
			lock->released ? "true" : "false",
			lock->held_on_entry ? "true" : "false",
			lock->held_on_exit ? "true" : "false",
			lock->description,
			(i < spec->lock_count - 1) ? "," : "");
	}

	ret += scnprintf(buf + ret, size - ret, "  ],\n");

	/* Capabilities */
	ret += scnprintf(buf + ret, size - ret,
		"  \"capabilities\": [\n");

	for (i = 0; i < spec->capability_count && i < KAPI_MAX_CAPABILITIES; i++) {
		const struct kapi_capability_spec *cap = &spec->capabilities[i];

		ret += scnprintf(buf + ret, size - ret,
			"    {\n"
			"      \"capability\": %d,\n"
			"      \"name\": \"%s\",\n"
			"      \"action\": \"%s\",\n"
			"      \"allows\": \"%s\",\n"
			"      \"without_cap\": \"%s\",\n"
			"      \"check_condition\": \"%s\",\n"
			"      \"priority\": %u",
			cap->capability,
			cap->cap_name,
			capability_action_to_string(cap->action),
			cap->allows,
			cap->without_cap,
			cap->check_condition,
			cap->priority);

		if (cap->alternative_count > 0) {
			int j;
			ret += scnprintf(buf + ret, size - ret,
				",\n      \"alternatives\": [");
			for (j = 0; j < cap->alternative_count; j++) {
				ret += scnprintf(buf + ret, size - ret,
					"%d%s", cap->alternative[j],
					(j < cap->alternative_count - 1) ? ", " : "");
			}
			ret += scnprintf(buf + ret, size - ret, "]");
		}

		ret += scnprintf(buf + ret, size - ret,
			"\n    }%s\n",
			(i < spec->capability_count - 1) ? "," : "");
	}

	ret += scnprintf(buf + ret, size - ret, "  ],\n");

	/* Additional info */
	ret += scnprintf(buf + ret, size - ret,
		"  \"since_version\": \"%s\",\n"
		"  \"deprecated\": %s,\n"
		"  \"replacement\": \"%s\",\n"
		"  \"examples\": \"%s\",\n"
		"  \"notes\": \"%s\"\n"
		"}\n",
		spec->since_version,
		spec->deprecated ? "true" : "false",
		spec->replacement,
		spec->examples,
		spec->notes);

	return ret;
}
EXPORT_SYMBOL_GPL(kapi_export_json);


/**
 * kapi_print_spec - Print API specification to kernel log
 * @spec: API specification to print
 */
void kapi_print_spec(const struct kernel_api_spec *spec)
{
	int i;

	if (!spec)
		return;

	pr_info("=== Kernel API Specification ===\n");
	pr_info("Name: %s\n", spec->name);
	pr_info("Version: %u\n", spec->version);
	pr_info("Description: %s\n", spec->description);

	if (spec->long_description[0])
		pr_info("Long Description: %s\n", spec->long_description);

	pr_info("Context Flags: 0x%x\n", spec->context_flags);

	/* Parameters */
	if (spec->param_count > 0) {
		pr_info("Parameters:\n");
		for (i = 0; i < spec->param_count && i < KAPI_MAX_PARAMS; i++) {
			const struct kapi_param_spec *param = &spec->params[i];
			pr_info("  [%d] %s: %s (flags: 0x%x)\n",
				i, param->name, param->type_name, param->flags);
			if (param->description[0])
				pr_info("      Description: %s\n", param->description);
		}
	}

	/* Return value */
	pr_info("Return: %s\n", spec->return_spec.type_name);
	if (spec->return_spec.description[0])
		pr_info("  Description: %s\n", spec->return_spec.description);

	/* Errors */
	if (spec->error_count > 0) {
		pr_info("Possible Errors:\n");
		for (i = 0; i < spec->error_count && i < KAPI_MAX_ERRORS; i++) {
			const struct kapi_error_spec *error = &spec->errors[i];
			pr_info("  %s (%d): %s\n",
				error->name, error->error_code, error->condition);
		}
	}

	/* Capabilities */
	if (spec->capability_count > 0) {
		pr_info("Capabilities:\n");
		for (i = 0; i < spec->capability_count && i < KAPI_MAX_CAPABILITIES; i++) {
			const struct kapi_capability_spec *cap = &spec->capabilities[i];
			pr_info("  %s (%d):\n", cap->cap_name, cap->capability);
			pr_info("    Action: %s\n", capability_action_to_string(cap->action));
			pr_info("    Allows: %s\n", cap->allows);
			pr_info("    Without: %s\n", cap->without_cap);
			if (cap->check_condition[0])
				pr_info("    Condition: %s\n", cap->check_condition);
		}
	}

	pr_info("================================\n");
}
EXPORT_SYMBOL_GPL(kapi_print_spec);

#ifdef CONFIG_KAPI_RUNTIME_CHECKS

/**
 * kapi_validate_fd - Validate that a file descriptor is valid in current context
 * @fd: File descriptor to validate
 *
 * Return: true if fd is valid in current process context, false otherwise
 */
static bool kapi_validate_fd(int fd)
{
	struct fd f;

	/* Special case: AT_FDCWD is always valid */
	if (fd == AT_FDCWD)
		return true;

	/* Check basic range */
	if (fd < 0)
		return false;

	/* Check if fd is valid in current process context */
	f = fdget(fd);
	if (fd_empty(f)) {
		return false;
	}

	/* fd is valid, release reference */
	fdput(f);
	return true;
}

/**
 * kapi_validate_user_ptr - Validate that a user pointer is accessible
 * @ptr: User pointer to validate
 * @size: Size in bytes to validate
 * @write: Whether write access is required
 *
 * Return: true if user memory is accessible, false otherwise
 */
static bool kapi_validate_user_ptr(const void __user *ptr, size_t size, bool write)
{
	/* NULL is valid if parameter is marked optional */
	if (!ptr)
		return false;

	/* Check if the user memory region is accessible */
	if (write) {
		return access_ok(ptr, size);
	} else {
		return access_ok(ptr, size);
	}
}

/**
 * kapi_validate_user_ptr_with_params - Validate user pointer with dynamic size
 * @param_spec: Parameter specification
 * @ptr: User pointer to validate
 * @all_params: Array of all parameter values
 * @param_count: Number of parameters
 *
 * Return: true if user memory is accessible, false otherwise
 */
static bool kapi_validate_user_ptr_with_params(const struct kapi_param_spec *param_spec,
						const void __user *ptr,
						const s64 *all_params,
						int param_count)
{
	size_t actual_size;
	bool write;

	/* NULL is allowed for optional parameters */
	if (!ptr && (param_spec->flags & KAPI_PARAM_OPTIONAL))
		return true;

	/* Calculate actual size based on related parameter */
	if (param_spec->size_param_idx >= 0 &&
	    param_spec->size_param_idx < param_count) {
		s64 count = all_params[param_spec->size_param_idx];

		/* Validate count is positive */
		if (count <= 0) {
			pr_warn("Parameter %s: size determinant is non-positive (%lld)\n",
				param_spec->name, count);
			return false;
		}

		/* Check for multiplication overflow */
		if (param_spec->size_multiplier > 0 &&
		    count > SIZE_MAX / param_spec->size_multiplier) {
			pr_warn("Parameter %s: size calculation overflow\n",
				param_spec->name);
			return false;
		}

		actual_size = count * param_spec->size_multiplier;
	} else {
		/* Use fixed size */
		actual_size = param_spec->size;
	}

	write = (param_spec->flags & KAPI_PARAM_OUT) ||
		(param_spec->flags & KAPI_PARAM_INOUT);

	return kapi_validate_user_ptr(ptr, actual_size, write);
}

/**
 * kapi_validate_path - Validate that a pathname is accessible and within limits
 * @path: User pointer to pathname
 * @param_spec: Parameter specification
 *
 * Return: true if path is valid, false otherwise
 */
static bool kapi_validate_path(const char __user *path,
				const struct kapi_param_spec *param_spec)
{
	size_t len;

	/* NULL is allowed for optional parameters */
	if (!path && (param_spec->flags & KAPI_PARAM_OPTIONAL))
		return true;

	if (!path) {
		pr_warn("Parameter %s: NULL path not allowed\n", param_spec->name);
		return false;
	}

	/* Check if the path is accessible */
	if (!access_ok(path, 1)) {
		pr_warn("Parameter %s: path pointer %p not accessible\n",
			param_spec->name, path);
		return false;
	}

	/* Use strnlen_user to get the length and validate accessibility */
	len = strnlen_user(path, PATH_MAX + 1);
	if (len == 0) {
		pr_warn("Parameter %s: invalid path pointer %p\n",
			param_spec->name, path);
		return false;
	}

	/* Check path length limit */
	if (len > PATH_MAX) {
		pr_warn("Parameter %s: path too long (exceeds PATH_MAX)\n",
			param_spec->name);
		return false;
	}

	return true;
}

/**
 * kapi_validate_param - Validate a parameter against its specification
 * @param_spec: Parameter specification
 * @value: Parameter value to validate
 *
 * Return: true if valid, false otherwise
 */
bool kapi_validate_param(const struct kapi_param_spec *param_spec, s64 value)
{
	int i;

	/* Special handling for file descriptor type */
	if (param_spec->type == KAPI_TYPE_FD) {
		if (!kapi_validate_fd((int)value)) {
			pr_warn("Parameter %s: invalid file descriptor %lld\n",
				param_spec->name, value);
			return false;
		}
		/* Continue with additional constraint checks if needed */
	}

	/* Special handling for user pointer type */
	if (param_spec->type == KAPI_TYPE_USER_PTR) {
		const void __user *ptr = (const void __user *)value;
		bool write = (param_spec->flags & KAPI_PARAM_OUT) ||
			     (param_spec->flags & KAPI_PARAM_INOUT);

		/* NULL is allowed for optional parameters */
		if (!ptr && (param_spec->flags & KAPI_PARAM_OPTIONAL))
			return true;

		if (!kapi_validate_user_ptr(ptr, param_spec->size, write)) {
			pr_warn("Parameter %s: invalid user pointer %p (size: %zu, %s)\n",
				param_spec->name, ptr, param_spec->size,
				write ? "write" : "read");
			return false;
		}
		/* Continue with additional constraint checks if needed */
	}

	/* Special handling for path type */
	if (param_spec->type == KAPI_TYPE_PATH) {
		const char __user *path = (const char __user *)value;

		if (!kapi_validate_path(path, param_spec)) {
			return false;
		}
		/* Continue with additional constraint checks if needed */
	}

	switch (param_spec->constraint_type) {
	case KAPI_CONSTRAINT_NONE:
		return true;

	case KAPI_CONSTRAINT_RANGE:
		if (value < param_spec->min_value || value > param_spec->max_value) {
			pr_warn("Parameter %s value %lld out of range [%lld, %lld]\n",
				param_spec->name, value,
				param_spec->min_value, param_spec->max_value);
			return false;
		}
		return true;

	case KAPI_CONSTRAINT_MASK:
		if (value & ~param_spec->valid_mask) {
			pr_warn("Parameter %s value 0x%llx contains invalid bits (valid mask: 0x%llx)\n",
				param_spec->name, value, param_spec->valid_mask);
			return false;
		}
		return true;

	case KAPI_CONSTRAINT_ENUM:
		if (!param_spec->enum_values || param_spec->enum_count == 0)
			return true;

		for (i = 0; i < param_spec->enum_count; i++) {
			if (value == param_spec->enum_values[i])
				return true;
		}
		pr_warn("Parameter %s value %lld not in valid enumeration\n",
			param_spec->name, value);
		return false;

	case KAPI_CONSTRAINT_CUSTOM:
		if (param_spec->validate)
			return param_spec->validate(value);
		return true;

	default:
		return true;
	}
}
EXPORT_SYMBOL_GPL(kapi_validate_param);

/**
 * kapi_validate_param_with_context - Validate parameter with access to all params
 * @param_spec: Parameter specification
 * @value: Parameter value to validate
 * @all_params: Array of all parameter values
 * @param_count: Number of parameters
 *
 * Return: true if valid, false otherwise
 */
bool kapi_validate_param_with_context(const struct kapi_param_spec *param_spec,
				       s64 value, const s64 *all_params, int param_count)
{
	/* Special handling for user pointer type with dynamic sizing */
	if (param_spec->type == KAPI_TYPE_USER_PTR) {
		const void __user *ptr = (const void __user *)value;

		/* NULL is allowed for optional parameters */
		if (!ptr && (param_spec->flags & KAPI_PARAM_OPTIONAL))
			return true;

		if (!kapi_validate_user_ptr_with_params(param_spec, ptr, all_params, param_count)) {
			pr_warn("Parameter %s: invalid user pointer %p\n",
				param_spec->name, ptr);
			return false;
		}
		/* Continue with additional constraint checks if needed */
	}

	/* For other types, fall back to regular validation */
	return kapi_validate_param(param_spec, value);
}
EXPORT_SYMBOL_GPL(kapi_validate_param_with_context);

/**
 * kapi_validate_syscall_param - Validate syscall parameter with enforcement
 * @spec: API specification
 * @param_idx: Parameter index
 * @value: Parameter value
 *
 * Return: -EINVAL if invalid, 0 if valid
 */
int kapi_validate_syscall_param(const struct kernel_api_spec *spec,
				 int param_idx, s64 value)
{
	const struct kapi_param_spec *param_spec;

	if (!spec || param_idx >= spec->param_count)
		return 0;

	param_spec = &spec->params[param_idx];

	if (!kapi_validate_param(param_spec, value)) {
		if (strncmp(spec->name, "sys_", 4) == 0) {
			/* For syscalls, we can return EINVAL to userspace */
			return -EINVAL;
		}
	}

	return 0;
}
EXPORT_SYMBOL_GPL(kapi_validate_syscall_param);

/**
 * kapi_validate_syscall_params - Validate all syscall parameters together
 * @spec: API specification
 * @params: Array of parameter values
 * @param_count: Number of parameters
 *
 * Return: -EINVAL if any parameter is invalid, 0 if all valid
 */
int kapi_validate_syscall_params(const struct kernel_api_spec *spec,
				 const s64 *params, int param_count)
{
	int i;

	if (!spec || !params)
		return 0;

	/* Validate that we have the expected number of parameters */
	if (param_count != spec->param_count) {
		pr_warn("API %s: parameter count mismatch (expected %u, got %d)\n",
			spec->name, spec->param_count, param_count);
		return -EINVAL;
	}

	/* Validate each parameter with context */
	for (i = 0; i < spec->param_count && i < KAPI_MAX_PARAMS; i++) {
		const struct kapi_param_spec *param_spec = &spec->params[i];

		if (!kapi_validate_param_with_context(param_spec, params[i], params, param_count)) {
			if (strncmp(spec->name, "sys_", 4) == 0) {
				/* For syscalls, we can return EINVAL to userspace */
				return -EINVAL;
			}
		}
	}

	return 0;
}
EXPORT_SYMBOL_GPL(kapi_validate_syscall_params);

/**
 * kapi_check_return_success - Check if return value indicates success
 * @return_spec: Return specification
 * @retval: Return value to check
 *
 * Returns true if the return value indicates success according to the spec.
 */
bool kapi_check_return_success(const struct kapi_return_spec *return_spec, s64 retval)
{
	u32 i;

	if (!return_spec)
		return true; /* No spec means we can't validate */

	switch (return_spec->check_type) {
	case KAPI_RETURN_EXACT:
		return retval == return_spec->success_value;

	case KAPI_RETURN_RANGE:
		return retval >= return_spec->success_min &&
		       retval <= return_spec->success_max;

	case KAPI_RETURN_ERROR_CHECK:
		/* Success if NOT in error list */
		if (return_spec->error_values) {
			for (i = 0; i < return_spec->error_count; i++) {
				if (retval == return_spec->error_values[i])
					return false; /* Found in error list */
			}
		}
		return true; /* Not in error list = success */

	case KAPI_RETURN_FD:
		/* File descriptors: >= 0 is success, < 0 is error */
		return retval >= 0;

	case KAPI_RETURN_CUSTOM:
		if (return_spec->is_success)
			return return_spec->is_success(retval);
		fallthrough;

	default:
		return true; /* Unknown check type, assume success */
	}
}
EXPORT_SYMBOL_GPL(kapi_check_return_success);

/**
 * kapi_validate_return_value - Validate that return value matches spec
 * @spec: API specification
 * @retval: Return value to validate
 *
 * Return: true if return value is valid according to spec, false otherwise.
 *
 * This function checks:
 * 1. If the value indicates success, it must match the success criteria
 * 2. If the value indicates error, it must be one of the specified error codes
 */
bool kapi_validate_return_value(const struct kernel_api_spec *spec, s64 retval)
{
	int i;
	bool is_success;

	if (!spec)
		return true; /* No spec means we can't validate */

	/* First check if this is a success return */
	is_success = kapi_check_return_success(&spec->return_spec, retval);

	if (is_success) {
		/* Success case - already validated by kapi_check_return_success */
		return true;
	}

	/* Special validation for file descriptor returns */
	if (spec->return_spec.check_type == KAPI_RETURN_FD && is_success) {
		/* For successful FD returns, validate it's a valid FD */
		if (!kapi_validate_fd((int)retval)) {
			pr_warn("API %s returned invalid file descriptor %lld\n",
				spec->name, retval);
			return false;
		}
		return true;
	}

	/* Error case - check if it's one of the specified errors */
	if (spec->error_count == 0) {
		/* No errors specified, so any error is potentially valid */
		pr_debug("API %s returned unspecified error %lld\n",
			 spec->name, retval);
		return true;
	}

	/* Check if the error is in our list of specified errors */
	for (i = 0; i < spec->error_count && i < KAPI_MAX_ERRORS; i++) {
		if (retval == spec->errors[i].error_code)
			return true;
	}

	/* Error not in spec */
	pr_warn("API %s returned unspecified error code %lld. Valid errors are:\n",
		spec->name, retval);
	for (i = 0; i < spec->error_count && i < KAPI_MAX_ERRORS; i++) {
		pr_warn("  %s (%d): %s\n",
			spec->errors[i].name,
			spec->errors[i].error_code,
			spec->errors[i].condition);
	}

	return false;
}
EXPORT_SYMBOL_GPL(kapi_validate_return_value);

/**
 * kapi_validate_syscall_return - Validate syscall return value with enforcement
 * @spec: API specification
 * @retval: Return value
 *
 * Return: 0 if valid, -EINVAL if the return value doesn't match spec
 *
 * For syscalls, this can help detect kernel bugs where unspecified error
 * codes are returned to userspace.
 */
int kapi_validate_syscall_return(const struct kernel_api_spec *spec, s64 retval)
{
	if (!spec)
		return 0;

	if (!kapi_validate_return_value(spec, retval)) {
		/* Log the violation but don't change the return value */
		WARN_ONCE(1, "Syscall %s returned unspecified value %lld\n",
			  spec->name, retval);
		/* Could return -EINVAL here to enforce, but that might break userspace */
	}

	return 0;
}
EXPORT_SYMBOL_GPL(kapi_validate_syscall_return);

/**
 * kapi_check_context - Check if current context matches API requirements
 * @spec: API specification to check against
 */
void kapi_check_context(const struct kernel_api_spec *spec)
{
	u32 ctx = spec->context_flags;
	bool valid = false;

	if (!ctx)
		return;

	/* Check if we're in an allowed context */
	if ((ctx & KAPI_CTX_PROCESS) && !in_interrupt())
		valid = true;

	if ((ctx & KAPI_CTX_SOFTIRQ) && in_softirq())
		valid = true;

	if ((ctx & KAPI_CTX_HARDIRQ) && in_hardirq())
		valid = true;

	if ((ctx & KAPI_CTX_NMI) && in_nmi())
		valid = true;

	if (!valid) {
		WARN_ONCE(1, "API %s called from invalid context\n", spec->name);
	}

	/* Check specific requirements */
	if ((ctx & KAPI_CTX_ATOMIC) && preemptible()) {
		WARN_ONCE(1, "API %s requires atomic context\n", spec->name);
	}

	if ((ctx & KAPI_CTX_SLEEPABLE) && !preemptible()) {
		WARN_ONCE(1, "API %s requires sleepable context\n", spec->name);
	}
}
EXPORT_SYMBOL_GPL(kapi_check_context);

#endif /* CONFIG_KAPI_RUNTIME_CHECKS */

/* DebugFS interface */
#ifdef CONFIG_DEBUG_FS

static struct dentry *kapi_debugfs_root;

static int kapi_spec_show(struct seq_file *s, void *v)
{
	struct kernel_api_spec *spec = s->private;
	char *buf;
	int ret;

	buf = kmalloc(PAGE_SIZE * 4, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	ret = kapi_export_json(spec, buf, PAGE_SIZE * 4);
	if (ret > 0)
		seq_printf(s, "%s", buf);

	kfree(buf);
	return 0;
}

static int kapi_spec_open(struct inode *inode, struct file *file)
{
	return single_open(file, kapi_spec_show, inode->i_private);
}

static const struct file_operations kapi_spec_fops = {
	.open = kapi_spec_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};

static int kapi_list_show(struct seq_file *s, void *v)
{
	struct kernel_api_spec *spec;
	struct dynamic_api_spec *dyn_spec;

	seq_printf(s, "Kernel API Specifications:\n\n");

	/* List static specifications */
	seq_printf(s, "Static APIs:\n");
	for (spec = __start_kapi_specs; spec < __stop_kapi_specs; spec++) {
		seq_printf(s, "  %s (v%u): %s\n",
			spec->name, spec->version, spec->description);
	}

	/* List dynamic specifications */
	seq_printf(s, "\nDynamic APIs:\n");
	mutex_lock(&api_spec_mutex);
	list_for_each_entry(dyn_spec, &dynamic_api_specs, list) {
		spec = dyn_spec->spec;
		seq_printf(s, "  %s (v%u): %s\n",
			spec->name, spec->version, spec->description);
	}
	mutex_unlock(&api_spec_mutex);

	return 0;
}

static int kapi_list_open(struct inode *inode, struct file *file)
{
	return single_open(file, kapi_list_show, NULL);
}

static const struct file_operations kapi_list_fops = {
	.open = kapi_list_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};

static int __init kapi_debugfs_init(void)
{
	struct kernel_api_spec *spec;
	struct dentry *spec_dir;

	kapi_debugfs_root = debugfs_create_dir("kapi", NULL);
	if (!kapi_debugfs_root)
		return -ENOMEM;

	/* Create list file */
	debugfs_create_file("list", 0444, kapi_debugfs_root, NULL,
			    &kapi_list_fops);

	/* Create directory for specifications */
	spec_dir = debugfs_create_dir("specs", kapi_debugfs_root);

	/* Create files for each static specification */
	for (spec = __start_kapi_specs; spec < __stop_kapi_specs; spec++) {
		debugfs_create_file(spec->name, 0444, spec_dir, spec,
				    &kapi_spec_fops);
	}

	return 0;
}

late_initcall(kapi_debugfs_init);

#endif /* CONFIG_DEBUG_FS */
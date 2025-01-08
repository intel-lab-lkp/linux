// SPDX-License-Identifier: GPL-2.0-only
/*
 * Landlock LSM - Domain management
 *
 * Copyright © 2016-2020 Mickaël Salaün <mic@digikod.net>
 * Copyright © 2018-2020 ANSSI
 * Copyright © 2024-2025 Microsoft Corporation
 */

#include <linux/cred.h>
#include <linux/file.h>
#include <linux/landlock.h>
#include <linux/mm.h>
#include <linux/path.h>
#include <linux/pid.h>
#include <linux/sched.h>
#include <linux/timekeeping.h>

#include "domain.h"
#include "fs.h"
#include "id.h"

void landlock_get_hierarchy(struct landlock_hierarchy *const hierarchy)
{
	if (hierarchy)
		refcount_inc(&hierarchy->usage);
}

void landlock_put_hierarchy(struct landlock_hierarchy *hierarchy)
{
	while (hierarchy && refcount_dec_and_test(&hierarchy->usage)) {
		const struct landlock_hierarchy *const freeme = hierarchy;

#ifdef CONFIG_AUDIT
		put_cred(hierarchy->details->cred);
		put_pid(hierarchy->details->pid);
		kfree(hierarchy->details);
#endif /* CONFIG_AUDIT */

		hierarchy = hierarchy->parent;
		kfree(freeme);
	}
}

#ifdef CONFIG_AUDIT

/**
 * get_current_exe - Get the current's executable path, if any
 *
 * @path_str: Returned pointer to a path string with a lifetime tied to the
 *            returned buffer, if any.
 * @path_size: Returned size of the @path string (including the trailing null
 *             character), if any.
 *
 * Returns: A pointer to an allocated buffer where @path point to, %NULL if
 * there is no executable path, or an error otherwise.
 */
static const void *get_current_exe(const char **path_str, size_t *path_size)
{
	struct mm_struct *mm = current->mm;
	struct file *file __free(fput) = NULL;
	char *buffer __free(kfree) = NULL;
	const char *path;
	size_t size;

	/* Adds 11 extra characters for the potential " (deleted)" suffix. */
	const size_t buffer_size = PATH_MAX + 11;

	if (!mm)
		return NULL;

	file = get_mm_exe_file(mm);
	if (!file)
		return NULL;

	buffer = kmalloc(buffer_size, GFP_KERNEL);
	if (!buffer)
		return ERR_PTR(-ENOMEM);

	path = d_path(&file->f_path, buffer, buffer_size);
	if (WARN_ON_ONCE(IS_ERR(path)))
		/* Should never happen according to buffer_size. */
		return ERR_CAST(path);

	size = buffer + buffer_size - path;
	if (WARN_ON_ONCE(size <= 0))
		return ERR_PTR(-ENAMETOOLONG);

	*path_size = size;
	*path_str = path;
	return no_free_ptr(buffer);
}

/*
 * Returns: A newly allocated object describing a domain, or an error
 * otherwise.
 */
static struct landlock_details *get_current_details(void)
{
	/* Cf. audit_log_d_path_exe() */
	static const char null_path[] = "(null)";
	const char *path_str = null_path;
	size_t path_size = sizeof(null_path);
	struct landlock_details *details;
	const void *buffer __free(kfree) = NULL;

	buffer = get_current_exe(&path_str, &path_size);
	if (IS_ERR(buffer))
		return ERR_CAST(buffer);

	/*
	 * Create the new details according to the path's length.  Do not
	 * allocate with GFP_KERNEL_ACCOUNT because it is independent from the
	 * caller.
	 */
	details =
		kzalloc(struct_size(details, exe_path, path_size), GFP_KERNEL);
	if (!details)
		return ERR_PTR(-ENOMEM);

	memcpy(details->exe_path, path_str, path_size);
	ktime_get_coarse_real_ts64(&details->creation);

	WARN_ON_ONCE(current_cred() != current_real_cred());
	details->cred = get_current_cred();
	details->pid = get_pid(task_pid(current));
	get_task_comm(details->comm, current);
	return details;
}

/**
 * landlock_init_current_hierarchy - Partially initialize landlock_hierarchy
 *
 * @hierarchy: The hierarchy to initialize.
 *
 * The current task is referenced as the domain restrictor.  The subjective
 * credentials must not be in an overridden state.
 *
 * @hierarchy->parent and @hierarchy->usage should already be set.
 */
int landlock_init_current_hierarchy(struct landlock_hierarchy *const hierarchy)
{
	struct landlock_details *details;

	details = get_current_details();
	if (IS_ERR(details))
		return PTR_ERR(details);

	hierarchy->details = details;
	hierarchy->id = landlock_get_id_range(1);
	hierarchy->log_status = LANDLOCK_LOG_PENDING;
	atomic64_set(&hierarchy->num_denials, 0);
	return 0;
}

#endif /* CONFIG_AUDIT */

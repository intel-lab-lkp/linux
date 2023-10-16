// SPDX-License-Identifier: GPL-2.0
/*
 * Persistent-Across-Kexec memory (prmem) - Persistent instances.
 *
 * Copyright (C) 2023 Microsoft Corporation
 * Author: Madhavan T. Venkataraman (madvenka@linux.microsoft.com)
 */
#include <linux/prmem.h>

static struct prmem_instance *prmem_find(char *subsystem, char *name)
{
	struct prmem_instance	*instance;

	list_for_each_entry(instance, &prmem->instances, node) {
		if (!strcmp(instance->subsystem, subsystem) &&
		    !strcmp(instance->name, name)) {
			return instance;
		}
	}
	return NULL;
}

void *prmem_get(char *subsystem, char *name, bool create)
{
	int			subsystem_len = strlen(subsystem);
	int			name_len = strlen(name);
	struct prmem_instance	*instance;

	/*
	 * In early boot, you are allowed to get an existing instance. But
	 * you are not allowed to create one until prmem is fully initialized.
	 */
	if (!prmem || (!prmem_inited && create))
		return NULL;

	if (!subsystem_len || subsystem_len >= PRMEM_MAX_NAME ||
	    !name_len || name_len >= PRMEM_MAX_NAME) {
		return NULL;
	}

	spin_lock(&prmem_lock);

	/* Check if it already exists. */
	instance = prmem_find(subsystem, name);
	if (instance || !create)
		goto unlock;

	instance = prmem_alloc_locked(sizeof(*instance));
	if (!instance)
		goto unlock;

	strcpy(instance->subsystem, subsystem);
	strcpy(instance->name, name);
	instance->data = NULL;
	instance->size = 0;

	list_add_tail(&instance->node, &prmem->instances);
unlock:
	spin_unlock(&prmem_lock);
	return instance;
}
EXPORT_SYMBOL_GPL(prmem_get);

void prmem_set_data(struct prmem_instance *instance, void *data, size_t size)
{
	if (!prmem_inited)
		return;

	spin_lock(&prmem_lock);
	instance->data = data;
	instance->size = size;
	spin_unlock(&prmem_lock);
}
EXPORT_SYMBOL_GPL(prmem_set_data);

void prmem_get_data(struct prmem_instance *instance, void **data, size_t *size)
{
	if (!prmem)
		return;

	spin_lock(&prmem_lock);
	*data = instance->data;
	*size = instance->size;
	spin_unlock(&prmem_lock);
}
EXPORT_SYMBOL_GPL(prmem_get_data);

bool prmem_put(struct prmem_instance *instance)
{
	if (!prmem_inited)
		return true;

	spin_lock(&prmem_lock);

	if (instance->data) {
		/*
		 * Caller is responsible for freeing instance data and setting
		 * it to NULL.
		 */
		spin_unlock(&prmem_lock);
		return false;
	}

	/* Free instance. */
	list_del(&instance->node);
	prmem_free_locked(instance, sizeof(*instance));

	spin_unlock(&prmem_lock);
	return true;
}
EXPORT_SYMBOL_GPL(prmem_put);

int prmem_list(char *subsystem, prmem_list_func_t func, void *arg)
{
	int			subsystem_len = strlen(subsystem);
	struct prmem_instance	*instance;
	int			ret;

	if (!prmem)
		return 0;

	if (!subsystem_len || subsystem_len >= PRMEM_MAX_NAME)
		return -EINVAL;

	spin_lock(&prmem_lock);

	list_for_each_entry(instance, &prmem->instances, node) {
		if (strcmp(instance->subsystem, subsystem))
			continue;

		ret = func(instance, arg);
		if (ret)
			break;
	}

	spin_unlock(&prmem_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(prmem_list);

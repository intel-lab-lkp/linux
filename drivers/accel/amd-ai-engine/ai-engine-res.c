// SPDX-License-Identifier: GPL-2.0
/*
 * AMD AI Engine resource implementation
 *
 * Copyright(C) 2025 Advanced Micro Devices, Inc. All rights reserved.
 */

#include <linux/bitmap.h>

#include "ai-engine-internal.h"

/**
 * aie_resource_initialize() - initialize AI engine resource
 * @res: pointer to AI engine resource
 * @count: total number of element of this resource
 *
 * Return: 0 for success, negative value for failure.
 *
 * This function will initialize the data structure for the
 * resource.
 */
int aie_resource_initialize(struct aie_resource *res, int count)
{
	res->bitmap = bitmap_zalloc(count, GFP_KERNEL);
	if (!res->bitmap)
		return -ENOMEM;
	res->total = count;

	return 0;
}

/**
 * aie_resource_uninitialize() - uninitialize AI engine resource
 * @res: pointer to AI engine resource
 *
 * This function will release the AI engine resource data members.
 */
void aie_resource_uninitialize(struct aie_resource *res)
{
	res->total = 0;
	if (res->bitmap)
		bitmap_free(res->bitmap);
}

/**
 * aie_resource_check_region() - check availability of requested resource
 * @res: pointer to AI engine resource to check
 * @start: start index of the required resource, it will only be used if
 *	   @continuous is 1. It will check the available resource starting from
 *	   @start
 * @count: number of requested element
 *
 * Return: start resource id if the requested number of resources are available
 *	   It will return negative value of errors.
 *
 * This function will check the availability. It will return start resource id
 * if the requested number of resources are available.
 */
int aie_resource_check_region(struct aie_resource *res,
			      u32 start, u32 count)
{
	unsigned long id;

	if (!res || !res->bitmap || !count)
		return -EINVAL;
	id = bitmap_find_next_zero_area(res->bitmap, res->total, start,
					count, 0);
	if (id >= res->total)
		return -ERANGE;

	return (int)id;
}

/**
 * aie_resource_get_region() - get requested AI engine resource
 * @res: pointer to AI engine resource to check
 * @count: number of requested element
 * @start: start index of the required resource
 *
 * Return: start resource id for success, and negative value for failure.
 *
 * This function check if the requested AI engine resource is available.
 * If it is available, mark it used and return the start resource id.
 */
int aie_resource_get_region(struct aie_resource *res, u32 start, u32 count)
{
	unsigned long off;

	if (!res || !res->bitmap || !count)
		return -EINVAL;
	off = bitmap_find_next_zero_area(res->bitmap, res->total, start,
					 count, 0);
	if (off >= res->total)
		return -ERANGE;

	bitmap_set(res->bitmap, off, count);

	return (int)off;
}

/**
 * aie_resource_put_region() - release requested AI engine resource
 * @res: pointer to AI engine resource to check
 * @start: start index of the resource to release
 * @count: number of elements to release
 *
 * This function release the requested AI engine resource.
 */
void aie_resource_put_region(struct aie_resource *res, int start, u32 count)
{
	if (!res || !count)
		return;
	bitmap_clear(res->bitmap, start, count);
}

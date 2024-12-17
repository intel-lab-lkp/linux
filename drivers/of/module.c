// SPDX-License-Identifier: GPL-2.0
/*
 * Linux kernel module helpers.
 */

#include <linux/of.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/string.h>

#include "of_private.h"

/*
 * of_modalias - get MODALIAS string value for a OF device node
 * @np: the OF device node
 * @lenp: MODALIAS string length returned if set, exclude '\0'
 *
 * This function gets MODALIAS value for a device node.
 *
 * Returns MODALIAS string on success, or ERR_PTR() on error.
 *
 * Note: please kfree successful return value afer using it.
 */
char *of_modalias(const struct device_node *np, ssize_t *lenp)
{
	const char *compat;
	char *c;
	struct property *p;
	ssize_t csize;
	ssize_t tsize;
	char *str = NULL;
	ssize_t len = 0;
	ssize_t pos = 0;
	int counting = 1;

	if (lenp)
		*lenp = 0;

	/*
	 * Two cycles controlled by @counting, the fist cycle counts
	 * chars, the second saves chars.
	 */
	do {
		/* Name & Type */
		/* %p eats all alphanum characters, so %c must be used here */
		csize = snprintf(str + pos, len - pos, "of:N%pOFn%c%s", np, 'T',
				 of_node_get_device_type(np));
		if (counting)
			tsize = csize;
		else
			pos += csize;

		of_property_for_each_string(np, "compatible", p, compat) {
			csize = snprintf(str + pos, len - pos, "C%s", compat);
			if (counting) {
				tsize += csize;
				continue;
			}

			for (c = str + pos; c; ) {
				c = strchr(c, ' ');
				if (c)
					*c++ = '_';
			}
			pos += csize;
		}

		if (counting) {
			/* Include '\0' of MODALIAS string. */
			len = tsize + 1;
			/* MODALIAS value is too long */
			if (unlikely(len > 2048))
				return ERR_PTR(-EINVAL);

			str = kmalloc(len, GFP_KERNEL);
			if (!str)
				return ERR_PTR(-ENOMEM);
		}

	}	while (counting--);

	if (lenp)
		*lenp = tsize;
	return str;
}

int of_request_module(const struct device_node *np)
{
	char *str;
	int ret;

	if (!np)
		return -ENODEV;

	str = of_modalias(np, NULL);
	if (IS_ERR(str))
		return PTR_ERR(str);

	ret = request_module(str);
	kfree(str);

	return ret;
}
EXPORT_SYMBOL_GPL(of_request_module);

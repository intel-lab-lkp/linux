// SPDX-License-Identifier: GPL-2.0
#include "fbtft.h"
#include "internal.h"

static int get_next_ulong(char **str_p, unsigned long *val, char *sep, int base)
{
	char *p_val;

	if (!str_p || !(*str_p))
		return -EINVAL;

	p_val = strsep(str_p, sep);

	if (!p_val)
		return -EINVAL;

	return kstrtoul(p_val, base, val);
}

int fbtft_gamma_parse_str(struct fbtft_par *par, u32 *curves,
			  const char *str, int size)
{
	char *str_p, *curve_p = NULL;
	char *tmp;
	unsigned long val = 0;
	int ret = 0;
	int curve_counter, value_counter;
	int _count;

	if (!str || !curves)
		return -EINVAL;

	tmp = kmemdup(str, size + 1, GFP_KERNEL);
	if (!tmp)
		return -ENOMEM;

	/* replace optional separators */
	str_p = tmp;
	while (*str_p) {
		if (*str_p == ',')
			*str_p = ' ';
		if (*str_p == ';')
			*str_p = '\n';
		str_p++;
	}

	str_p = strim(tmp);

	curve_counter = 0;
	while (str_p) {
		if (curve_counter == par->gamma.num_curves) {
			dev_err(par->info->device, "Gamma: Too many curves\n");
			ret = -EINVAL;
			goto out;
		}
		curve_p = strsep(&str_p, "\n");
		value_counter = 0;
		while (curve_p) {
			if (value_counter == par->gamma.num_values) {
				dev_err(par->info->device,
					"Gamma: Too many values\n");
				ret = -EINVAL;
				goto out;
			}
			ret = get_next_ulong(&curve_p, &val, " ", 16);
			if (ret)
				goto out;

			_count = curve_counter * par->gamma.num_values +
				 value_counter;
			curves[_count] = val;
			value_counter++;
		}
		if (value_counter != par->gamma.num_values) {
			dev_err(par->info->device, "Gamma: Too few values\n");
			ret = -EINVAL;
			goto out;
		}
		curve_counter++;
	}
	if (curve_counter != par->gamma.num_curves) {
		dev_err(par->info->device, "Gamma: Too few curves\n");
		ret = -EINVAL;
		goto out;
	}

out:
	kfree(tmp);
	return ret;
}

void fbtft_expand_debug_value(unsigned long *debug)
{
	switch (*debug & 0x7) {
	case 1:
		*debug |= DEBUG_LEVEL_1;
		break;
	case 2:
		*debug |= DEBUG_LEVEL_2;
		break;
	case 3:
		*debug |= DEBUG_LEVEL_3;
		break;
	case 4:
		*debug |= DEBUG_LEVEL_4;
		break;
	case 5:
		*debug |= DEBUG_LEVEL_5;
		break;
	case 6:
		*debug |= DEBUG_LEVEL_6;
		break;
	case 7:
		*debug = 0xFFFFFFFF;
		break;
	}
}

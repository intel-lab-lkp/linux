// SPDX-License-Identifier: GPL-2.0-only

/*
 *  Linux logo to be displayed on boot
 *
 *  Copyright (C) 1996 Larry Ewing (lewing@isc.tamu.edu)
 *  Copyright (C) 1996,1998 Jakub Jelinek (jj@sunsite.mff.cuni.cz)
 *  Copyright (C) 2001 Greg Banks <gnb@alphalink.com.au>
 *  Copyright (C) 2001 Jan-Benedict Glaw <jbglaw@lug-owl.de>
 *  Copyright (C) 2003 Geert Uytterhoeven <geert@linux-m68k.org>
 */

#include <linux/linux_logo.h>
#include <linux/of.h>
#include <linux/sizes.h>
#include <linux/slab.h>
#include <linux/stddef.h>
#include <linux/module.h>

#ifdef CONFIG_M68K
#include <asm/setup.h>
#endif

static bool nologo;
module_param(nologo, bool, 0);
MODULE_PARM_DESC(nologo, "Disables startup logo");

#ifdef CONFIG_LOGO_DT_CLUT224

#define LOGO_DT_COMPATIBLE	"linux,boot-logo-clut224"
#define LOGO_DT_MAX_CLUT	224
/*
 * The first 32 palette entries are reserved for the console, so the logo
 * colours start at index 32. That is an implementation detail of the frame
 * buffer layer rather than a property of the image, so the device tree stores
 * plain indices and the offset is applied here.
 */
#define LOGO_DT_CLUT_OFFSET	32
/* Sanity limit on the image size, a device tree is not a good place for more */
#define LOGO_DT_MAX_PIXELS	SZ_32M

static struct linux_logo logo_dt_clut224 = {
	.type		= LINUX_LOGO_CLUT224,
};

static unsigned char *logo_dt_clut;
static unsigned char *logo_dt_data;

static int logo_dt_parse(struct device_node *np)
{
	unsigned int clutsize, npixels, i;
	unsigned char *clut, *data;
	u32 width, height;
	int len, ret;

	ret = of_property_read_u32(np, "width", &width);
	if (ret)
		return ret;

	ret = of_property_read_u32(np, "height", &height);
	if (ret)
		return ret;

	if (!width || !height || (u64)width * height > LOGO_DT_MAX_PIXELS)
		return -EINVAL;

	npixels = width * height;

	len = of_property_count_u8_elems(np, "clut");
	if (len < 3 || len % 3)
		return -EINVAL;

	clutsize = len / 3;
	if (clutsize > LOGO_DT_MAX_CLUT)
		return -EINVAL;

	ret = of_property_count_u8_elems(np, "data");
	if (ret < 0)
		return ret;
	if ((unsigned int)ret != npixels)
		return -EINVAL;

	clut = kmalloc(len, GFP_KERNEL);
	if (!clut)
		return -ENOMEM;

	data = kmalloc(npixels, GFP_KERNEL);
	if (!data) {
		ret = -ENOMEM;
		goto err_free_clut;
	}

	ret = of_property_read_u8_array(np, "clut", clut, len);
	if (ret)
		goto err_free_data;

	ret = of_property_read_u8_array(np, "data", data, npixels);
	if (ret)
		goto err_free_data;

	for (i = 0; i < npixels; i++) {
		if (data[i] >= clutsize) {
			ret = -ERANGE;
			goto err_free_data;
		}
		data[i] += LOGO_DT_CLUT_OFFSET;
	}

	logo_dt_clut = clut;
	logo_dt_data = data;

	logo_dt_clut224.width = width;
	logo_dt_clut224.height = height;
	logo_dt_clut224.clutsize = clutsize;
	logo_dt_clut224.clut = clut;
	logo_dt_clut224.data = data;

	return 0;

err_free_data:
	kfree(data);
err_free_clut:
	kfree(clut);
	return ret;
}

static const struct linux_logo *logo_dt_find(void)
{
	static bool probed;
	struct device_node *np;
	int ret;

	if (probed)
		return logo_dt_data ? &logo_dt_clut224 : NULL;

	probed = true;

	np = of_get_compatible_child(of_chosen, LOGO_DT_COMPATIBLE);
	if (!np)
		return NULL;

	if (of_device_is_available(np)) {
		ret = logo_dt_parse(np);
		if (ret)
			pr_warn("logo: ignoring malformed %pOF node (%d)\n",
				np, ret);
	}

	of_node_put(np);

	return logo_dt_data ? &logo_dt_clut224 : NULL;
}

static void logo_dt_free(void)
{
	logo_dt_clut224.clut = NULL;
	logo_dt_clut224.data = NULL;

	kfree(logo_dt_clut);
	logo_dt_clut = NULL;

	kfree(logo_dt_data);
	logo_dt_data = NULL;
}

#else /* !CONFIG_LOGO_DT_CLUT224 */

static inline const struct linux_logo *logo_dt_find(void)
{
	return NULL;
}

static inline void logo_dt_free(void) { }

#endif /* CONFIG_LOGO_DT_CLUT224 */

/*
 * Logos are located in the initdata, and will be freed in kernel_init.
 * Use late_init to mark the logos as freed to prevent any further use.
 */

static bool logos_freed;

static int __init fb_logo_late_init(void)
{
	logos_freed = true;
	logo_dt_free();
	return 0;
}

late_initcall_sync(fb_logo_late_init);

/* logo's are marked __initdata. Use __ref to tell
 * modpost that it is intended that this function uses data
 * marked __initdata.
 */
const struct linux_logo * __ref fb_find_logo(int depth)
{
	const struct linux_logo *logo = NULL;

	if (nologo || logos_freed)
		return NULL;

	/* A logo supplied by the device tree wins over the built-in ones */
	if (depth >= 8) {
		logo = logo_dt_find();
		if (logo)
			return logo;
	}

#ifdef CONFIG_LOGO_LINUX_MONO
	if (depth >= 1)
		logo = &logo_linux_mono;
#endif
	
#ifdef CONFIG_LOGO_LINUX_VGA16
	if (depth >= 4)
		logo = &logo_linux_vga16;
#endif
	
#ifdef CONFIG_LOGO_LINUX_CLUT224
	if (depth >= 8)
		logo = &logo_linux_clut224;
#endif

	return logo;
}
EXPORT_SYMBOL_GPL(fb_find_logo);

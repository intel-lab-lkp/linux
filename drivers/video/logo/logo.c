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

/* Boot logo supplied by the device tree */

#define LOGO_DT_MAX_CLUT	224
/*
 * The first 32 palette entries are reserved for the console, so the logo
 * colours start at index 32. That is an implementation detail of the frame
 * buffer layer rather than a property of the image, so the device tree stores
 * plain indices and the offset is applied here.
 */
#define LOGO_DT_CLUT_OFFSET	32
/*
 * Sanity limit on the image size, a device tree is not a good place for more:
 * a 4K screen is 8.3M pixels, and the copy is kept for the life of the kernel.
 * One byte per pixel, so this is a byte count as well.
 */
#define LOGO_DT_MAX_PIXELS	SZ_16M

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

	/* The palette is at most 672 bytes, the pixels can be megabytes */
	data = kvmalloc(npixels, GFP_KERNEL);
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
	kvfree(data);
err_free_clut:
	kfree(clut);
	return ret;
}

static const struct linux_logo *logo_dt_find(void)
{
	static bool probed;
	struct device_node *np;
	int ret;

	if (!IS_ENABLED(CONFIG_LOGO_DT_CLUT224))
		return NULL;

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

static int logo_dt_parse_rotation(const char *rotation)
{
	if (!strcmp(rotation, "none"))
		return LOGO_ROTATE_NONE;
	if (!strcmp(rotation, "cw"))
		return LOGO_ROTATE_CW;
	if (!strcmp(rotation, "ud"))
		return LOGO_ROTATE_UD;
	if (!strcmp(rotation, "ccw"))
		return LOGO_ROTATE_CCW;

	return -EINVAL;
}

/**
 * logo_dt_placement - where a logo supplied by the device tree asks to go
 * @pos: filled in with the requested position and offset
 * @rotation: filled in with one of LOGO_ROTATE_*, or -1 when the node does
 *            not ask for any
 *
 * Only the placement is read here, the image itself is fb_find_logo()'s
 * business. It lives next to the image parser rather than in any one user so
 * that everything that draws the logo reads the same properties the same way.
 *
 * Return: 0 if the device tree supplies a logo node, -ENODEV otherwise, in
 * which case @pos and @rotation are left untouched.
 */
int logo_dt_placement(struct logo_placement *pos, int *rotation)
{
	struct device_node *np;
	const char *str;
	u32 val[2];
	int rot;

	if (!IS_ENABLED(CONFIG_LOGO_DT_CLUT224))
		return -ENODEV;

	/*
	 * Same node the image itself comes from, looked up the same way: a
	 * child of /chosen, not just anything compatible anywhere in the tree.
	 */
	np = of_get_compatible_child(of_chosen, LOGO_DT_COMPATIBLE);
	if (!np)
		return -ENODEV;

	if (!of_device_is_available(np)) {
		of_node_put(np);
		return -ENODEV;
	}

	*pos = (struct logo_placement){ };
	*rotation = -1;

	if (!of_property_read_u32_array(np, "logo-position", val, 2)) {
		pos->x = (s32)val[0];
		pos->y = (s32)val[1];
	}

	if (!of_property_read_u32_array(np, "logo-offset", val, 2)) {
		pos->offset_x = (s32)val[0];
		pos->offset_y = (s32)val[1];
	}

	if (!of_property_read_string(np, "logo-rotation", &str)) {
		rot = logo_dt_parse_rotation(str);
		if (rot < 0)
			pr_warn("logo: %pOF: unknown logo-rotation \"%s\"\n",
				np, str);
		else
			*rotation = rot;
	}

	of_node_put(np);

	return 0;
}
EXPORT_SYMBOL_GPL(logo_dt_placement);

/*
 * Logos are located in the initdata, and will be freed in kernel_init.
 * Use late_init to mark the logos as freed to prevent any further use.
 */

static bool logos_freed;

static int __init fb_logo_late_init(void)
{
	logos_freed = true;
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

	if (nologo)
		return NULL;

	/*
	 * A logo supplied by the device tree wins over the built-in ones. It
	 * is an ordinary allocation rather than initdata, so unlike them it
	 * stays valid for the life of the kernel, and is still there for a
	 * display driver whose probe finishes after the built-in logos have
	 * gone.
	 */
	if (depth >= 8) {
		logo = logo_dt_find();
		if (logo)
			return logo;
	}

	if (logos_freed)
		return NULL;

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

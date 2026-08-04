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

#include <linux/io.h>
#include <linux/linux_logo.h>
#include <linux/of.h>
#include <linux/of_reserved_mem.h>
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
/* "LOGO", little endian, at the start of a handed over memory region */
#define LOGO_DT_MAGIC		0x4f474f4c

struct logo_dt_header {
	__le32 magic;
	__le32 width;
	__le32 height;
	__le32 clutsize;
};

static struct linux_logo logo_dt_clut224 = {
	.type		= LINUX_LOGO_CLUT224,
};

static unsigned char *logo_dt_clut;
static unsigned char *logo_dt_data;

/* Reject geometries that cannot describe a sane image before using them */
static int logo_dt_check_geometry(u32 width, u32 height, u32 clutsize)
{
	if (!width || !height || (u64)width * height > LOGO_DT_MAX_PIXELS)
		return -EINVAL;

	if (!clutsize || clutsize > LOGO_DT_MAX_CLUT)
		return -EINVAL;

	return 0;
}

/*
 * Take a private copy of an image that has already been range checked, so
 * that nothing else can change it under us, and shift the pixels into the
 * palette slots the frame buffer layer leaves to the logo.
 */
static int logo_dt_store(u32 width, u32 height, u32 clutsize,
			 const u8 *clut_src, const u8 *data_src)
{
	unsigned int npixels = width * height;
	unsigned char *clut, *data;
	unsigned int i;
	int ret;

	clut = kmemdup(clut_src, clutsize * 3, GFP_KERNEL);
	if (!clut)
		return -ENOMEM;

	data = kmemdup(data_src, npixels, GFP_KERNEL);
	if (!data) {
		ret = -ENOMEM;
		goto err_free_clut;
	}

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

static int logo_dt_parse_properties(struct device_node *np)
{
	const u8 *clut, *data;
	u32 width, height;
	int len, ret;

	ret = of_property_read_u32(np, "width", &width);
	if (ret)
		return ret;

	ret = of_property_read_u32(np, "height", &height);
	if (ret)
		return ret;

	len = of_property_count_u8_elems(np, "clut");
	if (len < 3 || len % 3)
		return -EINVAL;

	ret = logo_dt_check_geometry(width, height, len / 3);
	if (ret)
		return ret;

	if (of_property_count_u8_elems(np, "data") != width * height)
		return -EINVAL;

	clut = of_get_property(np, "clut", NULL);
	data = of_get_property(np, "data", NULL);
	if (!clut || !data)
		return -EINVAL;

	return logo_dt_store(width, height, len / 3, clut, data);
}

/*
 * Image handed over by the bootloader in a reserved memory region. Only a
 * region the device tree declared is accepted, never a bare address, so the
 * kernel can never be pointed at memory it is using for something else, and
 * so that a size is known and every access can be bounds checked.
 */
static int logo_dt_parse_memory_region(struct device_node *np)
{
	u32 width, height, clutsize;
	const struct logo_dt_header *hdr;
	struct device_node *mem_np;
	struct reserved_mem *rmem;
	size_t clutlen, datalen;
	const u8 *payload;
	void *mem;
	int ret;

	mem_np = of_parse_phandle(np, "memory-region", 0);
	if (!mem_np)
		return -ENOENT;

	rmem = of_reserved_mem_lookup(mem_np);
	of_node_put(mem_np);
	if (!rmem)
		return -EINVAL;

	if (rmem->size < sizeof(*hdr))
		return -EINVAL;

	mem = memremap(rmem->base, rmem->size, MEMREMAP_WB);
	if (!mem)
		return -ENOMEM;

	hdr = mem;
	if (le32_to_cpu(hdr->magic) != LOGO_DT_MAGIC) {
		ret = -EINVAL;
		goto out_unmap;
	}

	width = le32_to_cpu(hdr->width);
	height = le32_to_cpu(hdr->height);
	clutsize = le32_to_cpu(hdr->clutsize);

	ret = logo_dt_check_geometry(width, height, clutsize);
	if (ret)
		goto out_unmap;

	clutlen = (size_t)clutsize * 3;
	datalen = (size_t)width * height;

	/* Everything the header promises has to fit inside the region */
	if (sizeof(*hdr) + clutlen + datalen > rmem->size) {
		ret = -EINVAL;
		goto out_unmap;
	}

	payload = (const u8 *)(hdr + 1);
	ret = logo_dt_store(width, height, clutsize, payload, payload + clutlen);

out_unmap:
	memunmap(mem);
	return ret;
}

static int logo_dt_parse(struct device_node *np)
{
	if (of_property_present(np, "memory-region"))
		return logo_dt_parse_memory_region(np);

	return logo_dt_parse_properties(np);
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

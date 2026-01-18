// SPDX-License-Identifier: GPL-2.0-only
/*
 *	Copyright (C)  2025 Zsolt Kajtar (soci@c64.rulez.org)
 */

#include <linux/export.h>
#include <linux/module.h>
#include <linux/fb.h>
#include <linux/bitrev.h>
#include <linux/string.h>
#include <asm/types.h>

#ifdef CONFIG_FB_SYS_REV_PIXELS_IN_BYTE
#define FB_REV_PIXELS_IN_BYTE
#endif

#include "sysmem.h"
#include "fb_fillrect.h"

void sys_fillrect(struct fb_info *p, const struct fb_fillrect *rect)
{
	struct fb_fillrect modded;
	int vxres, vyres;

	if (!(p->flags & FBINFO_VIRTFB))
		fb_warn_once(p, "%s: framebuffer is not in virtual address space.\n", __func__);

	vxres = p->var.xres_virtual;
	vyres = p->var.yres_virtual;

	/* Validate and clip rectangle to virtual resolution */
	if (!rect->width || !rect->height ||
	    rect->dx >= vxres || rect->dy >= vyres)
		return;

	memcpy(&modded, rect, sizeof(struct fb_fillrect));

	if (modded.dx + modded.width > vxres)
		modded.width = vxres - modded.dx;
	if (modded.dy + modded.height > vyres)
		modded.height = vyres - modded.dy;

	fb_fillrect(p, &modded);
}
EXPORT_SYMBOL(sys_fillrect);

MODULE_AUTHOR("Zsolt Kajtar <soci@c64.rulez.org>");
MODULE_DESCRIPTION("Virtual memory packed pixel framebuffer area fill");
MODULE_LICENSE("GPL");

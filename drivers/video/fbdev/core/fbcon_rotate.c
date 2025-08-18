/*
 *  linux/drivers/video/console/fbcon_rotate.c -- Software Rotation
 *
 *      Copyright (C) 2005 Antonino Daplas <adaplas @pol.net>
 *
 *  This file is subject to the terms and conditions of the GNU General Public
 *  License.  See the file COPYING in the main directory of this archive for
 *  more details.
 */

#include <linux/module.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/fb.h>
#include <linux/vt_kern.h>
#include <linux/console.h>
#include <asm/types.h>
#include "fbcon.h"
#include "fbcon_rotate.h"

int fbcon_rotate_font(struct fb_info *info, struct vc_data *vc)
{
	struct fbcon *confb = info->fbcon_par;
	int len, err = 0;
	int s_cellsize, d_cellsize, i;
	const u8 *src;
	u8 *dst;

	if (vc->vc_font.data == confb->fontdata &&
	    confb->p->con_rotate == confb->cur_rotate)
		goto finished;

	src = confb->fontdata = vc->vc_font.data;
	confb->cur_rotate = confb->p->con_rotate;
	len = vc->vc_font.charcount;
	s_cellsize = ((vc->vc_font.width + 7)/8) *
		vc->vc_font.height;
	d_cellsize = s_cellsize;

	if (confb->rotate == FB_ROTATE_CW ||
	    confb->rotate == FB_ROTATE_CCW)
		d_cellsize = ((vc->vc_font.height + 7)/8) *
			vc->vc_font.width;

	if (info->fbops->fb_sync)
		info->fbops->fb_sync(info);

	if (confb->fd_size < d_cellsize * len) {
		dst = kmalloc_array(len, d_cellsize, GFP_KERNEL);

		if (dst == NULL) {
			err = -ENOMEM;
			goto finished;
		}

		confb->fd_size = d_cellsize * len;
		kfree(confb->fontbuffer);
		confb->fontbuffer = dst;
	}

	dst = confb->fontbuffer;
	memset(dst, 0, confb->fd_size);

	switch (confb->rotate) {
	case FB_ROTATE_UD:
		for (i = len; i--; ) {
			rotate_ud(src, dst, vc->vc_font.width,
				  vc->vc_font.height);

			src += s_cellsize;
			dst += d_cellsize;
		}
		break;
	case FB_ROTATE_CW:
		for (i = len; i--; ) {
			rotate_cw(src, dst, vc->vc_font.width,
				  vc->vc_font.height);
			src += s_cellsize;
			dst += d_cellsize;
		}
		break;
	case FB_ROTATE_CCW:
		for (i = len; i--; ) {
			rotate_ccw(src, dst, vc->vc_font.width,
				   vc->vc_font.height);
			src += s_cellsize;
			dst += d_cellsize;
		}
		break;
	}

finished:
	return err;
}

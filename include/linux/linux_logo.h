/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_LINUX_LOGO_H
#define _LINUX_LINUX_LOGO_H

/*
 *  Linux logo to be displayed on boot
 *
 *  Copyright (C) 1996 Larry Ewing (lewing@isc.tamu.edu)
 *  Copyright (C) 1996,1998 Jakub Jelinek (jj@sunsite.mff.cuni.cz)
 *  Copyright (C) 2001 Greg Banks <gnb@alphalink.com.au>
 *  Copyright (C) 2001 Jan-Benedict Glaw <jbglaw@lug-owl.de>
 *  Copyright (C) 2003 Geert Uytterhoeven <geert@linux-m68k.org>
 */

#include <linux/init.h>
#include <linux/minmax.h>
#include <linux/types.h>


#define LINUX_LOGO_MONO		1	/* monochrome black/white */
#define LINUX_LOGO_VGA16	2	/* 16 colors VGA text palette */
#define LINUX_LOGO_CLUT224	3	/* 224 colors */
#define LINUX_LOGO_GRAY256	4	/* 256 levels grayscale */

/* Compatible of the /chosen child describing a device tree supplied logo */
#define LOGO_DT_COMPATIBLE	"boot-logo-clut224"


struct linux_logo {
	int type;			/* one of LINUX_LOGO_* */
	unsigned int width;
	unsigned int height;
	unsigned int clutsize;		/* LINUX_LOGO_CLUT224 only */
	const unsigned char *clut;	/* LINUX_LOGO_CLUT224 only */
	const unsigned char *data;
};

extern const struct linux_logo logo_linux_mono;
extern const struct linux_logo logo_linux_vga16;
extern const struct linux_logo logo_linux_clut224;
extern const struct linux_logo logo_spe_clut224;

extern const struct linux_logo *fb_find_logo(int depth);

/*
 * Where a boot logo goes. A coordinate of -1 centres the logo on that axis.
 * Whatever draws the logo describes its placement this way and computes it
 * with logo_place_axis(), so that no two places can end up disagreeing about
 * where the logo is.
 */
struct logo_placement {
	s32 x, y;
};

/*
 * Place a logo along one axis. @pos is the coordinate asked for, or -1 to
 * centre on that axis. The result is clamped so that the logo always lies
 * entirely within the screen: the drawing code does not clip, so asking for
 * more than that would otherwise scribble past the end of the frame buffer.
 */
static inline int logo_place_axis(s32 pos, unsigned int span, unsigned int size)
{
	int last = (int)span - (int)size;

	if (size > span)
		return 0;

	return pos == -1 ? last / 2 : clamp(pos, 0, last);
}

#ifdef CONFIG_FB_LOGO_EXTRA
extern void fb_append_extra_logo(const struct linux_logo *logo,
				 unsigned int n);
#else
static inline void fb_append_extra_logo(const struct linux_logo *logo,
					unsigned int n)
{}
#endif

#endif /* _LINUX_LINUX_LOGO_H */

/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * link caps internal header, for link modes <-> capabilities <-> interfaces
 * conversions.
 */

#ifndef __PHY_CAPS_H
#define __PHY_CAPS_H

void linkmode_from_caps(unsigned long *linkmode, unsigned long caps);

#endif /* __PHY_CAPS_H */

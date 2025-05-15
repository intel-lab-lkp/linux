/* SPDX-License-Identifier: GPL-2.0 */
/*
 * mdio-boardinfo.h - board info interface internal to the mdio_bus
 * component
 */

#ifndef __MDIO_BOARD_INFO_H
#define __MDIO_BOARD_INFO_H

#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/phy.h>

struct mdio_board_entry {
	struct list_head	list;
	struct mdio_board_info	board_info;
};

extern struct list_head mdio_board_list;
extern struct mutex mdio_board_lock;

#endif /* __MDIO_BOARD_INFO_H */

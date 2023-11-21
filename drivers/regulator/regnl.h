/* SPDX-License-Identifier: GPL-2.0-or-later */

#ifndef __REGULATOR_EVENT_H
#define __REGULATOR_EVENT_H

int reg_generate_netlink_event(const char *reg_name, u64 event);

#endif

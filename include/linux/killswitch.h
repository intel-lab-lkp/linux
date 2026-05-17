/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2026 Sasha Levin <sashal@kernel.org>
 */
#ifndef _LINUX_KILLSWITCH_H
#define _LINUX_KILLSWITCH_H

#ifdef CONFIG_KILLSWITCH
int killswitch_engage(const char *symbol, long retval);
int killswitch_disengage(const char *symbol);
bool killswitch_is_engaged(const char *symbol);
#else
static inline int killswitch_engage(const char *symbol, long retval)
{ return -EOPNOTSUPP; }
static inline int killswitch_disengage(const char *symbol) { return -EOPNOTSUPP; }
static inline bool killswitch_is_engaged(const char *symbol) { return false; }
#endif

#endif /* _LINUX_KILLSWITCH_H */

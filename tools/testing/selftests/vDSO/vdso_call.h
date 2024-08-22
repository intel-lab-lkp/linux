/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Macro to call vDSO functions
 *
 * Copyright (C) 2024 Christophe Leroy <christophe.leroy@csgroup.eu>, CS GROUP France
 */
#ifndef __VDSO_CALL_H__
#define __VDSO_CALL_H__

#define VDSO_CALL(fn, nr, args...)	fn(args)

#endif

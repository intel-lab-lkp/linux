/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Support for Intel Camera Imaging ISP subsystem.
 * Copyright (c) 2010-2015, Intel Corporation.
 */

#ifndef __VMEM_LOCAL_H_INCLUDED__
#define __VMEM_LOCAL_H_INCLUDED__

#include "type_support.h"
#include "vmem_global.h"

typedef u16 t_vmem_elem;
typedef s16 t_svmem_elem;

#define VMEM_ARRAY(x, s)	t_vmem_elem x[(s) / ISP_NWAY][ISP_NWAY]
#define SVMEM_ARRAY(x, s)	t_svmem_elem x[(s) / ISP_NWAY][ISP_NWAY]

#endif /* __VMEM_LOCAL_H_INCLUDED__ */

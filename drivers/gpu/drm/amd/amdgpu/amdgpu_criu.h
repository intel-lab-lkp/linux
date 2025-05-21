/* SPDX-License-Identifier: MIT */
/*
* Copyright 2025 Advanced Micro Devices, Inc.
*
* Permission is hereby granted, free of charge, to any person obtaining a
* copy of this software and associated documentation files (the "Software"),
* to deal in the Software without restriction, including without limitation
* the rights to use, copy, modify, merge, publish, distribute, sublicense,
* and/or sell copies of the Software, and to permit persons to whom the
* Software is furnished to do so, subject to the following conditions:
*
* The above copyright notice and this permission notice shall be included in
* all copies or substantial portions of the Software.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
* IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
* FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
* THE COPYRIGHT HOLDER(S) OR AUTHOR(S) BE LIABLE FOR ANY CLAIM, DAMAGES OR
* OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
* ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
* OTHER DEALINGS IN THE SOFTWARE.
*/

#ifndef __AMDGPU_CRIU_H__
#define __AMDGPU_CRIU_H__

#include <drm/amdgpu_drm.h>

int amdgpu_criu_op_ioctl(struct drm_device *dev, void *data,
			    struct drm_file *filp);

struct amdgpu_bo *bo_from_criu_global_handle(uint8_t *handle);
int insert_bo_at_criu_global_handle(struct amdgpu_bo *bo, uint8_t *handle);

#endif

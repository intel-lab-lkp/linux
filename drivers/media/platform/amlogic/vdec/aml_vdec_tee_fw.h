/* SPDX-License-Identifier: (GPL-2.0-only OR MIT) */
/*
 * Copyright (C) 2025 Amlogic, Inc. All rights reserved
 */

#ifndef AML_VDEC_TEE_FW_H_
#define AML_VDEC_TEE_FW_H_

#include "aml_vdec_hw.h"

/**
 * struct aml_tee_fw - specify the firmware format for each dec type
 * @fw_format: Specify firmware format for current decoder.
 * @core: Specify which hardware core is needed.
 * @is_swap: Specify if the swap memory is needed.
 */
struct aml_tee_fw {
	u32 fw_format;
	u32 core;
	u32 is_swap;
};

int aml_tee_fw_preload(struct aml_vdec_hw *hw);
int load_firmware(struct aml_vdec_hw *hw, u32 type);

#endif


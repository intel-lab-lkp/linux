/* SPDX-License-Identifier: MIT */
/*
 * Copyright 2024 Advanced Micro Devices, Inc.
 */

#ifndef __DAL_HW_FACTORY_DCN_DDC_H__
#define __DAL_HW_FACTORY_DCN_DDC_H__

static inline void dcn_define_ddc_registers_common(
		struct hw_gpio_pin *pin,
		uint32_t en,
		const struct ddc_registers *data_regs,
		const struct ddc_registers *clk_regs,
		const struct ddc_shift *shift,
		const struct ddc_mask *mask)
{
	struct hw_ddc *ddc = HW_DDC_FROM_BASE(pin);

	switch (pin->id) {
	case GPIO_ID_DDC_DATA:
		ddc->regs = &data_regs[en];
		ddc->base.regs = &data_regs[en].gpio;
		break;

	case GPIO_ID_DDC_CLOCK:
		ddc->regs = &clk_regs[en];
		ddc->base.regs = &clk_regs[en].gpio;
		break;

	default:
		ASSERT_CRITICAL(false);
		return;
	}

	ddc->shifts = &shift[en];
	ddc->masks = &mask[en];
}

#endif /* __DAL_HW_FACTORY_DCN_DDC_H__ */

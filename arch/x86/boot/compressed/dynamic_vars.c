// SPDX-License-Identifier: GPL-2.0
#include <linux/const.h>
#include "dynamic_vars.h"
#include "../voffset.h"

const unsigned long vo__text = VO__text;
const unsigned long vo___bss_start = VO___bss_start;
const unsigned long vo__end = VO__end;
const unsigned long kernel_total_size = VO__end - VO__text;

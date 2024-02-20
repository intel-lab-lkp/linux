// SPDX-License-Identifier: GPL-2.0
#include <linux/const.h>
#include "dynamic_vars.h"
#include <generated/compile.h>
#include <generated/utsrelease.h>
#include <generated/utsversion.h>
#include "../voffset.h"

const unsigned long vo__text = VO__text;
const unsigned long vo___bss_start = VO___bss_start;
const unsigned long vo__end = VO__end;
const unsigned long kernel_total_size = VO__end - VO__text;

/* Simplified build-specific string for starting entropy. */
const char build_str[] = UTS_RELEASE " (" LINUX_COMPILE_BY "@"
		LINUX_COMPILE_HOST ") (" LINUX_COMPILER ") " UTS_VERSION;
unsigned long build_str_len = sizeof(build_str)-1;

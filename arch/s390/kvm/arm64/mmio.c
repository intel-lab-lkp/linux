// SPDX-License-Identifier: GPL-2.0

#include <linux/kvm_host.h>
#include <linux/bitfield.h>

#include <arm64/kvm_emulate.h>
#include <arm64/kvm_mmu.h>
#include <arm64/sysreg.h>

#define __INCL_GEN_ARM_FILE
#include "generated/mmio.inc"
#undef __INCL_GEN_ARM_FILE

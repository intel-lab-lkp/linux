# SPDX-License-Identifier: GPL-2.0-only

obj-$(CONFIG_MBEDTEE) += mbedtee.o

mbedtee-y += core.o
mbedtee-y += supp.o
mbedtee-y += shm_pool.o
mbedtee-y += rpc_caller.o rpc_callee.o
mbedtee-$(CONFIG_ARM) += rpc_caller_arm.o rpc_callee_arm.o
mbedtee-$(CONFIG_ARM64) += rpc_caller_arm.o rpc_callee_arm.o
mbedtee-$(CONFIG_RISCV) += rpc_caller_riscv.o rpc_callee_riscv.o

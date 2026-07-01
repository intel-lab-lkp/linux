# SPDX-License-Identifier: GPL-2.0-only
#
# Steal time monitor to alter preferred CPU state.
#
# Arch can implement strong function definitions and override the
# default by adding them in arch specific file. It must ensure
# that preferred is always subset of active.
#
# It is always compiled as module if CONFIG_PREFERRED_CPU=y
# One has to enable the module.
#
obj-$(subst y,m,$(CONFIG_PREFERRED_CPU)) += steal_monitor.o

steal_monitor-y := sm_core.o defaults.o

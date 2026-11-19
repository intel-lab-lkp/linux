# SPDX-License-Identifier: GPL-2.0
#
# Makefile for linux/drivers/platform/lenovo
# Lenovo Platform Specific Drivers (architecture independent)
#

lenovo-target-$(CONFIG_LENOVO_WMI_HELPERS)	+= wmi-helpers.o
lenovo-target-$(CONFIG_LENOVO_WMI_EVENTS)	+= wmi-events.o
lenovo-target-$(CONFIG_LENOVO_WMI_CAPDATA)	+= wmi-capdata.o
lenovo-target-$(CONFIG_LENOVO_WMI_HOTKEY_UTILITIES)	+= wmi-hotkey-utilities.o
lenovo-target-$(CONFIG_LENOVO_WMI_HOTKEYS)	+= wmi-hotkeys.o
lenovo-target-$(CONFIG_LENOVO_WMI_CAMERA)	+= wmi-camera.o
lenovo-target-$(CONFIG_LENOVO_WMI_GAMEZONE)	+= wmi-gamezone.o
lenovo-target-$(CONFIG_LENOVO_WMI_TUNING)	+= wmi-other.o

# Add 'lenovo' prefix to each module listed in lenovo-target-*
define LENOVO_OBJ_TARGET
lenovo-$(1)-y := $(1).o
obj-$(2) += lenovo-$(1).o
endef

$(foreach target, $(basename $(lenovo-target-y)), $(eval $(call LENOVO_OBJ_TARGET,$(target),y)))
$(foreach target, $(basename $(lenovo-target-m)), $(eval $(call LENOVO_OBJ_TARGET,$(target),m)))

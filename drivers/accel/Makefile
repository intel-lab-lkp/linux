# SPDX-License-Identifier: GPL-2.0-only

# several accel modules use drm*dbg, so they *are* DRM modules.
# When built with only CONFIG_DYNAMIC_DEBUG_CORE
# (and not CONFIG_DYNAMIC_DEBUG), they need this flag
subdir-ccflags-$(CONFIG_DRM_USE_DYNAMIC_DEBUG) += -DDYNAMIC_DEBUG_MODULE

obj-$(CONFIG_DRM_ACCEL_AMDXDNA)		+= amdxdna/
obj-$(CONFIG_DRM_ACCEL_ARM_ETHOSU)	+= ethosu/
obj-$(CONFIG_DRM_ACCEL_HABANALABS)	+= habanalabs/
obj-$(CONFIG_DRM_ACCEL_IVPU)		+= ivpu/
obj-$(CONFIG_DRM_ACCEL_QAIC)		+= qaic/
obj-$(CONFIG_DRM_ACCEL_ROCKET)		+= rocket/

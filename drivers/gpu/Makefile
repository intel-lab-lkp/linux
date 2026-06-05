# SPDX-License-Identifier: GPL-2.0-only
# drm/tegra depends on host1x, so if both drivers are built-in care must be
# taken to initialize them in the correct order. Link order is the only way
# to ensure this currently.
# Similarly, buddy must come first since it is used by other drivers.
obj-$(CONFIG_GPU_BUDDY)	+= buddy.o
obj-y			+= host1x/ drm/ vga/ tests/
obj-$(CONFIG_IMX_IPUV3_CORE)	+= ipu-v3/
obj-$(CONFIG_TRACE_GPU_MEM)		+= trace/

# nova-core and nova-drm are built from this Makefile so nova-drm's dependency
# on nova-core can be expressed as a plain Make prerequisite rather than a
# recursive sub-make. This is a temporary workaround until the Rust build
# system supports cross-crate dependencies natively.

obj-$(CONFIG_NOVA_CORE) += nova-core.o
nova-core-y := nova-core/nova_core.o

obj-$(CONFIG_DRM_NOVA) += nova-drm.o
nova-drm-y := drm/nova/nova.o

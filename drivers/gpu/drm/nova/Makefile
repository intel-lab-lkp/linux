# SPDX-License-Identifier: GPL-2.0

obj-$(CONFIG_DRM_NOVA) += nova-drm.o
nova-drm-y := nova.o

nova_core_rmeta := drivers/gpu/nova-core/libnova_core.rmeta

rustflags-y += --extern nova_core=$(objtree)/$(nova_core_rmeta)

# `nova-drm` imports `nova-core` at compile time, so request `nova-core`'s
# explicit metadata target before compiling nova.o.
$(obj)/nova.o: | $(objtree)/$(nova_core_rmeta)

# Build `nova-core`'s metadata by invoking make.
#
# This is ugly but only temporary until the build system natively supports
# cross-crate dependencies.
$(objtree)/$(nova_core_rmeta): FORCE
	$(Q)$(MAKE) $(build)=drivers/gpu/nova-core $(nova_core_rmeta)

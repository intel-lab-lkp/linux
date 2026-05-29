# SPDX-License-Identifier: GPL-2.0

obj-$(CONFIG_NOVA_CORE) += nova-core.o
nova-core-y := nova_core.o nova_core_exports.o

# Export Rust symbols so dependent modules can use them at runtime.
#
# This is a workaround until the build system supports Rust cross-module
# dependencies natively.
# Kbuild uses Rust v0 mangling, whose symbols start with "_R".
rust_exports = \
	$(NM) -p --defined-only $(1) | \
	awk '$$2 == "T" && $$3 ~ /^_R/ { \
		printf "extern void %s(void); EXPORT_SYMBOL_GPL(%s);\n", $$3, $$3 \
	}'

define filechk_nova_core_exports
	$(call rust_exports,$(obj)/nova_core.o)
endef

$(obj)/exports_nova_core_generated.h: $(obj)/nova_core.o FORCE
	$(call filechk,nova_core_exports)

targets += exports_nova_core_generated.h

$(obj)/nova_core_exports.o: $(obj)/exports_nova_core_generated.h

# Dependent Rust drivers need `nova-core`'s crate metadata.
# Expose it as an explicit target so they can request it directly.
nova_core_rmeta := libnova_core.rmeta
nova_core_rmeta_private := .nova_core_rmeta/$(nova_core_rmeta)
targets += $(nova_core_rmeta) $(nova_core_rmeta_private)

# Build in a private directory to avoid racing with `nova_core.o` on rustc's
# intermediate object names during parallel builds.
quiet_cmd_rustc_rmeta_rs = RUSTC RMETA $@
      cmd_rustc_rmeta_rs = mkdir -p $(dir $@); \
		$(rust_common_cmd) --emit=metadata=$@ \
		--emit=obj=$(basename $@).o $<; \
		rm -f $(basename $@).o

$(obj)/$(nova_core_rmeta_private): private __modname := nova-core
$(obj)/$(nova_core_rmeta_private): private part-of-builtin := $(if $(filter y,$(CONFIG_NOVA_CORE)),y)
$(obj)/$(nova_core_rmeta_private): private part-of-module := $(if $(filter m,$(CONFIG_NOVA_CORE)),y)
$(obj)/$(nova_core_rmeta_private): $(obj)/nova_core.rs FORCE
	+$(call if_changed_dep,rustc_rmeta_rs)

# Keep the metadata available at a stable path for consumers.
$(obj)/$(nova_core_rmeta): $(obj)/$(nova_core_rmeta_private) FORCE
	$(call if_changed,copy)

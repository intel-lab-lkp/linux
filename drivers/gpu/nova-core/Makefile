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

# SPDX-License-Identifier: GPL-2.0
# Greybus core
greybus-y :=	core.o		\
		debugfs.o	\
		hd.o		\
		manifest.o	\
		module.o	\
		interface.o	\
		bundle.o	\
		connection.o	\
		control.o	\
		svc.o		\
		svc_watchdog.o	\
		operation.o

obj-$(CONFIG_GREYBUS)		+= greybus.o

# needed for trace events
ccflags-y += -I$(src)

obj-$(CONFIG_GREYBUS_BEAGLEPLAY)	+= gb-beagleplay.o

# Greybus Host controller drivers
gb-es2-y := es2.o

obj-$(CONFIG_GREYBUS_ES2)	+= gb-es2.o

obj-$(CONFIG_GREYBUS_SOFTSVC)	+= gb-softsvc.o
gb-softsvc-y			+= gb_softsvc.o gb_softsvc_exports.o

obj-$(CONFIG_GREYBUS_UART_NODE)	+= gb-uart-node.o
gb-uart-node-y			+= gb_uart_node.o

# Export Rust symbols from gb-softsvc only if gb-uart-node actually references them.
gb-softsvc-export-deps := $(if $(CONFIG_GREYBUS_UART_NODE),$(obj)/gb_uart_node.o)

rust_needed_exports = \
	{ $(if $(strip $(2)),$(NM) -u $(2);,) echo "__DEFINED_RUST_SYMBOLS__"; \
	  $(NM) -p --defined-only $(1); } | \
	awk -v fmt='$(3)' ' \
		/^__DEFINED_RUST_SYMBOLS__$$/ { defs = 1; next } \
		!defs { if ($$NF ~ /^_R/) needed[$$NF] = 1; next } \
		defs && $$2 ~ /(T|R|D|B)/ && $$3 ~ /^_R/ && \
			$$3 !~ /_(init|cleanup)_module$$/ && \
			$$3 !~ /__(pfx|cfi|odr_asan)/ && \
			$$3 in needed { printf fmt, $$3 } \
	'

quiet_cmd_exports = EXPORTS $@
      cmd_exports = \
	$(call rust_needed_exports,$<,$(gb-softsvc-export-deps),EXPORT_SYMBOL_RUST_GPL(%s);\n) > $@

$(obj)/exports_gb_softsvc_generated.h: $(obj)/gb_softsvc.o $(gb-softsvc-export-deps) FORCE
	$(call if_changed,exports)

targets += exports_gb_softsvc_generated.h

$(obj)/gb_softsvc_exports.o: $(obj)/exports_gb_softsvc_generated.h
CFLAGS_gb_softsvc_exports.o := -I $(objtree)/$(obj)

ifdef CONFIG_MODVERSIONS
# The C export shim declares Rust symbols as `extern int`, so reuse its export
# list but generate symbol CRCs from the Rust object instead of the shim's DWARF.
$(obj)/gb_softsvc_exports.o: private cmd_gensymtypes_c = \
	$(call getexportsymbols,\1) | \
	$(objtree)/scripts/gendwarfksyms/gendwarfksyms \
		$(if $(KBUILD_GENDWARFKSYMS_STABLE), --stable) \
		$(if $(KBUILD_SYMTYPES), --symtypes $(@:.o=.symtypes),) \
		$(obj)/gb_softsvc.o
endif

# Output nova-core's crate metadata for use by nova-drm at compile time.
RUSTFLAGS_gb_softsvc.o += \
	--emit=metadata=$(objtree)/$(obj)/libgb_softsvc.rmeta

# Allow nova-drm to import nova-core's types.
$(obj)/gb_uart_node.o: $(obj)/gb_softsvc.o
RUSTFLAGS_gb_uart_node.o := -L $(objtree)/$(obj) --extern gb_softsvc

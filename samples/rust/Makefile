# SPDX-License-Identifier: GPL-2.0
ccflags-y += -I$(src)				# needed for trace events

obj-$(CONFIG_SAMPLE_RUST_MINIMAL)		+= rust_minimal.o
obj-$(CONFIG_SAMPLE_RUST_MISC_DEVICE)		+= rust_misc_device.o
obj-$(CONFIG_SAMPLE_RUST_PRINT)			+= rust_print.o
obj-$(CONFIG_SAMPLE_RUST_DEBUGFS)		+= rust_debugfs.o
obj-$(CONFIG_SAMPLE_RUST_DEBUGFS_SCOPED)	+= rust_debugfs_scoped.o
obj-$(CONFIG_SAMPLE_RUST_DMA)			+= rust_dma.o
obj-$(CONFIG_SAMPLE_RUST_DRIVER_I2C)		+= rust_driver_i2c.o
obj-$(CONFIG_SAMPLE_RUST_I2C_CLIENT)		+= rust_i2c_client.o
obj-$(CONFIG_SAMPLE_RUST_DRIVER_PCI)		+= rust_driver_pci.o
obj-$(CONFIG_SAMPLE_RUST_DRIVER_PLATFORM)	+= rust_driver_platform.o
obj-$(CONFIG_SAMPLE_RUST_DRIVER_SRIOV)		+= rust_driver_sriov_pf.o rust_driver_sriov_vf.o
obj-$(CONFIG_SAMPLE_RUST_DRIVER_USB)		+= rust_driver_usb.o
obj-$(CONFIG_SAMPLE_RUST_DRIVER_FAUX)		+= rust_driver_faux.o
obj-$(CONFIG_SAMPLE_RUST_DRIVER_AUXILIARY)	+= rust_driver_auxiliary.o
obj-$(CONFIG_SAMPLE_RUST_DRIVER_SERDEV)		+= rust_driver_serdev.o
obj-$(CONFIG_SAMPLE_RUST_CONFIGFS)		+= rust_configfs.o
obj-$(CONFIG_SAMPLE_RUST_SOC)			+= rust_soc.o

rust_print-y := rust_print_main.o rust_print_events.o

rust_driver_sriov_pf-y := rust_driver_sriov_pf_main.o rust_driver_sriov_pf_exports.o

# Follow the nova-core/nova-drm cross-crate rules in drivers/gpu/Makefile.
# Export Rust symbols from the PF only if the VF actually references them.
sriov-pf-export-deps := $(if $(CONFIG_SAMPLE_RUST_DRIVER_SRIOV),$(obj)/rust_driver_sriov_vf.o)

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
	$(call rust_needed_exports,$<,$(sriov-pf-export-deps),EXPORT_SYMBOL_RUST_GPL(%s);\n) > $@

$(obj)/exports_rust_driver_sriov_pf_generated.h: $(obj)/rust_driver_sriov_pf_main.o $(sriov-pf-export-deps) FORCE
	$(call if_changed,exports)

targets += exports_rust_driver_sriov_pf_generated.h

$(obj)/rust_driver_sriov_pf_exports.o: $(obj)/exports_rust_driver_sriov_pf_generated.h
CFLAGS_rust_driver_sriov_pf_exports.o := -I $(objtree)/$(obj)

ifdef CONFIG_MODVERSIONS
# The C export shim declares Rust symbols as `extern int`, so reuse its export
# list but generate symbol CRCs from the Rust object instead of the shim's DWARF.
$(obj)/rust_driver_sriov_pf_exports.o: private cmd_gensymtypes_c = \
	$(call getexportsymbols,\1) | \
	$(objtree)/scripts/gendwarfksyms/gendwarfksyms \
		$(if $(KBUILD_GENDWARFKSYMS_STABLE), --stable) \
		$(if $(KBUILD_SYMTYPES), --symtypes $(@:.o=.symtypes),) \
		$(obj)/rust_driver_sriov_pf_main.o
endif

# Output the PF's crate metadata for use by the VF at compile time.
RUSTFLAGS_rust_driver_sriov_pf_main.o += --crate-name rust_driver_sriov_pf \
	--emit=metadata=$(objtree)/$(obj)/librust_driver_sriov_pf.rmeta

# Allow the VF to import the PF's types.
$(obj)/rust_driver_sriov_vf.o: $(obj)/rust_driver_sriov_pf_main.o
RUSTFLAGS_rust_driver_sriov_vf.o := -L $(objtree)/$(obj) --extern rust_driver_sriov_pf

subdir-$(CONFIG_SAMPLE_RUST_HOSTPROGS)		+= hostprogs

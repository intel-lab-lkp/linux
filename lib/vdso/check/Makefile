# SPDX-License-Identifier: GPL-2.0-only

quiet_cmd_host_bindgen = HOSTBINDGEN $@
      cmd_host_bindgen = $(BINDGEN) $< -o $@ --depfile $(depfile) -- $(HOSTBINDGEN_FLAGS)

quiet_cmd_host_rustlib = HOSTRUSTC L $@
      cmd_host_rustlib = \
	$(HOSTRUSTC) $(hostrust_flags) \
		--emit=dep-info=$(depfile) --emit=link=$@ \
		--crate-type rlib \
		--crate-name $(patsubst %.rlib,%,$(notdir $@)) $<

uapi-directory := $(srctree)/tools/include/uapi

# bindings.rs will be generated in the build directory, but rustc can not use
# source code from both the source and build directory together.
# Compile the binding code into a library first.
$(obj)/bindings.rs: HOSTBINDGEN_FLAGS := -I$(uapi-directory)
$(obj)/bindings.rs: $(uapi-directory)/linux/elf.h FORCE
	$(call if_changed_dep,host_bindgen)

HOSTRUSTFLAGS_libbindings := -Amissing_docs -Anon-camel-case-types
$(obj)/libbindings.rlib: $(obj)/bindings.rs FORCE
	$(call if_changed_dep,host_rustlib)

targets += bindings.rs libbindings.rlib

hostprogs += vdsocheck
vdsocheck-rust := y
HOSTRUSTFLAGS_vdsocheck := --extern=bindings=$(obj)/libbindings.rlib
$(obj)/vdsocheck: $(obj)/libbindings.rlib

PHONY += vdsocheck
vdsocheck: $(obj)/vdsocheck

# SPDX-License-Identifier: GPL-2.0
#
# Makefile for the Kernel API Specification Framework
#

# Core API specification framework
obj-$(CONFIG_KAPI_SPEC)		+= kernel_api_spec.o

# Auto-generated API specifications collector
ifeq ($(CONFIG_KAPI_SPEC),y)
obj-$(CONFIG_KAPI_SPEC)		+= generated_api_specs.o

# Find all potential apispec files (this is evaluated at make time)
apispec-files := $(shell find $(objtree) -name "*.apispec.h" -type f 2>/dev/null)

# Generate the collector file
# Note: FORCE ensures this is always regenerated to pick up new apispec files
$(obj)/generated_api_specs.c: $(srctree)/scripts/generate_api_specs.sh FORCE
	$(Q)$(CONFIG_SHELL) $< $(srctree) $(objtree) > $@

targets += generated_api_specs.c
clean-files += generated_api_specs.c

# Add explicit dependency on the generator script
$(obj)/generated_api_specs.o: $(obj)/generated_api_specs.c
endif
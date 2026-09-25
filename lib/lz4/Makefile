# SPDX-License-Identifier: GPL-2.0-only
ccflags-y += -O3

# upstream/ holds verbatim copies of the upstream LZ4 sources; do not edit
# them.  The three entry point files include them.  freestanding/ supplies
# the ISO C headers upstream includes that -nostdinc does not provide.
ccflags-y += -I $(src)/freestanding

obj-$(CONFIG_LZ4_COMPRESS)	+= lz4_compress.o
obj-$(CONFIG_LZ4HC_COMPRESS)	+= lz4hc_compress.o
obj-$(CONFIG_LZ4_DECOMPRESS)	+= lz4_decompress.o

# SPDX-License-Identifier: GPL-2.0-only
include ../scripts/Makefile.include
include ../scripts/Makefile.arch

# Makefile to build the coresight configuration table file reader and generator tools

this-makefile := $(lastword $(MAKEFILE_LIST))
tools-src := $(realpath $(dir $(this-makefile)))
srctree := $(realpath $(dir $(tools-src)/../../.))

# ensure we use all as the default - skip anything in included Makefile
.DEFAULT_GOAL = all
# MAKECMDGOALS isn't set if there's no explicit goal in the
# command line, so set the default.
MAKECMDGOALS ?= $(.DEFAULT_GOAL)

# cs tools includes
CS_TOOLS_INCLUDE = -I$(srctree)/drivers/hwtracing/coresight -I$(srctree)/tools/include/ \
		-I$(srctree)/tools/include/uapi -I$(srctree)/tools/coresight

# compile flags
CFLAGS += $(CPPFLAGS) -c -Wall -DLINUX -Wno-switch -Wlogical-op -fPIC $(CS_TOOLS_INCLUDE)

# object files
coresight-cfg-file-gen-objs := coresight-cfg-file-gen.o coresight-cfg-bufw.o \
				coresight-cfg-example1.o coresight-cfg-example2.o
coresight-cfg-file-read-objs := coresight-cfg-file-read.o coresight-config-table.o

# debug variant
ifdef DEBUG
CFLAGS += -g -O0 -DDEBUG
else
CFLAGS += -O2 -DNDEBUG
endif

.PHONY: all
all:  coresight-cfg-file-gen coresight-cfg-file-read

coresight-config-table.o: src_copy
	$(CC) $(CFLAGS) coresight-config-table.c -o coresight-config-table.o

.PHONY: src_copy
src_copy:
	@cp $(srctree)/drivers/hwtracing/coresight/coresight-config-table.c $(srctree)/tools/coresight/.

coresight-cfg-file-gen: $(coresight-cfg-file-gen-objs)
	$(CC) $(LDFLAGS) $(coresight-cfg-file-gen-objs) -o coresight-cfg-file-gen

coresight-cfg-file-read: $(coresight-cfg-file-read-objs)
	$(CC) $(LDFLAGS) $(coresight-cfg-file-read-objs) -o coresight-cfg-file-read

clean:
	rm -f coresight-cfg-file-gen coresight-cfg-file-read
	rm -f *.o
	rm -f coresight-config-table.c
	rm -f *.cscfg

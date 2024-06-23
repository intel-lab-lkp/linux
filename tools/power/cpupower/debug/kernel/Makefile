# SPDX-License-Identifier: GPL-2.0
obj-m	:=

KDIR	:= /lib/modules/$(shell uname -r)/build
KMISC   := /lib/modules/$(shell uname -r)/cpufrequtils/

ifeq ("$(CONFIG_X86_TSC)", "y")
  obj-m	 += cpufreq-test_tsc.o
endif

default:
	$(MAKE) -C $(KDIR) M=$(CURDIR)

clean:
	- rm -rf *.o *.ko .*.cmd .*.mod.* *.mod.c
	- rm -rf Module.symvers modules.order

install: default
	install -d $(KMISC)
	install -m 644 -c *.ko $(KMISC)
	/sbin/depmod -a

all:	default

help:
	@echo  'Build targets:'
	@echo  '  all		  - Default target. Could be omitted. Builds "cpufreq-test_tsc"'
	@echo  '                    kernel module against the running kernel if the "CONFIG_X86_TSC"'
	@echo  '                    kernel config parameter is enabled. Puts the built module'
	@echo  '                    to the current dir'
	@echo  '  install	  - Install previously built "cpufreq-test_tsc.ko" module'
	@echo  '                    to the running kernel "modules" dir'
	@echo  ''
	@echo  'Clean targets:'
	@echo  '  clean		  - Clean "cpufreq-test_tsc" module build artifacts'

# SPDX-License-Identifier: GPL-2.0

obj-m := panic_trigger_test.o

KDIR := $(abspath ../../../../)
PWD  := $(shell pwd)

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
	rm -f *.mod.c *.o *.ko *.order *.symvers

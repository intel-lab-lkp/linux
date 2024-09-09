PAGE_POOL_TEST_DIR := $(realpath $(dir $(abspath $(lastword $(MAKEFILE_LIST)))))
KDIR ?= $(abspath $(PAGE_POOL_TEST_DIR)/../../../../..)

ifeq ($(V),1)
Q =
else
Q = @
endif

MODULES = page_pool_test.ko

obj-m += page_pool_test.o

all:
	+$(Q)make -C $(KDIR) M=$(PAGE_POOL_TEST_DIR) modules

clean:
	+$(Q)make -C $(KDIR) M=$(PAGE_POOL_TEST_DIR) clean

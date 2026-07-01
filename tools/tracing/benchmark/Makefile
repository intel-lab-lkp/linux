ifeq ($(O),)
KDIR ?= /lib/modules/$(shell uname -r)/build
else
KDIR := $(O)
endif
MDIR := $(CURDIR)

all:
	$(MAKE) -C $(KDIR) M=$(MDIR) modules

clean:
	$(MAKE) -C $(KDIR) M=$(MDIR) clean

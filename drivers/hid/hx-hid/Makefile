ifneq ($(filter m, $(CONFIG_HX_HID)),)
  ccflags-y += -D__HIMAX_MOD__
endif

ifneq ($(CONFIG_HX_HID_HEATMAP),)
	ccflags-y += -DHX_HEATMAP_EN
endif

ifneq ($(KERNELRELEASE),)
# kbuild part of makefile
ifneq ($(CONFIG_HX_HID_IC_83102J),)
	hx_spi_hid_hx83102j-objs := hx_ic_83102j.o
	hx_spi_hid_hx83102j-objs += hx_plat.o \
		hx_ic_core.o \
		hx_core.o \
		hx_hid.o \
		hx_inspect.o
ifneq ($(CONFIG_OF),)
	hx_spi_hid_hx83102j-objs += hx_of.o
else ifneq ($(CONFIG_ACPI),)
	hx_spi_hid_hx83102j-objs += hx_acpi.o
endif

	obj-$(CONFIG_HX_HID) += hx_spi_hid_hx83102j.o
endif

else
# normal makefile
KDIR ?= /lib/modules/`uname -r`/build

default:
	$(MAKE) -C $(KDIR) M=$$PWD
clean:
	@rm *.o *.mod *.mod.c .*.cmd Module.symvers *.a *.ko modules.order
endif

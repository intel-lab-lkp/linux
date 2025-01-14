# SPDX-License-Identifier: GPL-2.0
#
# arch/microblaze/boot/Makefile
#

targets := linux.bin linux.bin.gz linux.bin.ub simpleImage.*

OBJCOPYFLAGS := -R .note -R .comment -R .note.gnu.build-id -O binary

$(obj)/linux.bin.gz: $(obj)/linux.bin FORCE
	$(call if_changed,gzip)

quiet_cmd_strip = STRIP   $< $@$2
	cmd_strip = $(STRIP) -K microblaze_start -K _end -K __log_buf \
				-K _fdt_start $< -o $@$2

UIMAGE_LOADADDR = $(CONFIG_KERNEL_BASE_ADDR)

$(obj)/linux.bin $(obj)/simpleImage.$(DTB): vmlinux FORCE
	$(call if_changed,objcopy)

$(obj)/linux.bin.ub $(obj)/simpleImage.$(DTB).ub: %.ub: % FORCE
	$(call if_changed,uimage)

$(obj)/simpleImage.$(DTB).unstrip: vmlinux FORCE
	$(call if_changed,copy)

$(obj)/simpleImage.$(DTB).strip: vmlinux FORCE
	$(call if_changed,strip)

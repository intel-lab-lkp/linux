# SPDX-License-Identifier: GPL-2.0-only

obj-$(CONFIG_CRYPTO_CHACHA_RISCV64) += chacha-riscv64.o
chacha-riscv64-y := chacha-riscv64-glue.o chacha-riscv64-zvkb.o

obj-$(CONFIG_CRYPTO_POLY1305_RISCV) += poly1305-riscv.o
poly1305-riscv-y := poly1305-core.o poly1305-glue.o
AFLAGS_poly1305-core.o += -Dpoly1305_init=poly1305_block_init_arch
AFLAGS_poly1305-core.o += -Dpoly1305_blocks=poly1305_blocks_arch
AFLAGS_poly1305-core.o += -Dpoly1305_emit=poly1305_emit_arch

obj-$(CONFIG_CRYPTO_SHA256_RISCV64) += sha256-riscv64.o
sha256-riscv64-y := sha256.o sha256-riscv64-zvknha_or_zvknhb-zvkb.o

ifeq ($(CONFIG_64BIT),y)
PERLASM_ARCH := 64
else
PERLASM_ARCH := void
endif

quiet_cmd_perlasm = PERLASM $@
      cmd_perlasm = $(PERL) $(<) $(PERLASM_ARCH) $(@)

$(obj)/%-core.S: $(src)/%-riscv.pl
	$(call cmd,perlasm)

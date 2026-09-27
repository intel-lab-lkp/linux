# SPDX-License-Identifier: GPL-2.0

# KCSAN uses udelay for introducing watchpoint delay; avoid recursion.
KCSAN_SANITIZE_delay.o := n

lib-y		:= clear_user.o delay.o copy_from_user.o		\
		   copy_to_user.o copy_page.o				\
		   clear_page.o csum.o csum-copy.o csum-copy-neon.o	\
		   insn.o memchr.o memcpy.o				\
		   memset.o memcmp.o strcmp.o strncmp.o strlen.o	\
		   strnlen.o strchr.o strrchr.o tishift.o

# NEON intrinsics; only ever called between kernel_neon_begin()/end().
CFLAGS_csum-copy-neon.o		+= $(CC_FLAGS_FPU)
CFLAGS_REMOVE_csum-copy-neon.o	+= $(CC_FLAGS_NO_FPU)

lib-$(CONFIG_ARCH_HAS_UACCESS_FLUSHCACHE) += uaccess_flushcache.o

obj-$(CONFIG_FUNCTION_ERROR_INJECTION) += error-inject.o

obj-$(CONFIG_ARM64_MTE) += mte.o

obj-$(CONFIG_KASAN_SW_TAGS) += kasan_sw_tags.o

obj-$(CONFIG_KSTACK_WATCH)	+= kstackwatch.o
kstackwatch-y := kernel.o stack.o watch.o

obj-$(CONFIG_KSTACK_WATCH_TEST)	+= kstackwatch_test.o
CFLAGS_kstackwatch_test.o := -fno-ipa-sra -fno-inline \
                             -fno-optimize-sibling-calls \
                             -fno-section-anchors \
                             -fno-pic -fno-pie -O0 -Og

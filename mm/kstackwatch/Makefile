obj-$(CONFIG_KSTACKWATCH)	+= kstackwatch.o
kstackwatch-y := kernel.o stack.o watch.o

obj-$(CONFIG_KSTACKWATCH_TEST)	+= kstackwatch_test.o
kstackwatch_test-y := test.o
CFLAGS_test.o := -fno-inline \
		-fno-optimize-sibling-calls \
		-fno-pic -fno-pie -O0 -Og

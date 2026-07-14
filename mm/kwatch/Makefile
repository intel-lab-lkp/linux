obj-$(CONFIG_KWATCH) += kwatch.o

kwatch-y := core.o deref.o task_ctx.o hwbp.o probe.o anchor.o
kwatch-$(CONFIG_KWATCH_KUNIT_TEST) += deref_test.o

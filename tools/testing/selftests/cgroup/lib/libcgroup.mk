CGROUP_DIR := $(selfdir)/cgroup

LIBCGROUP_C := lib/cgroup_util.c

LIBCGROUP_O := $(patsubst %.c, $(OUTPUT)/%.o, $(LIBCGROUP_C))

CFLAGS += -I$(CGROUP_DIR)/lib/include

EXTRA_HDRS := $(selfdir)/clone3/clone3_selftests.h

$(LIBCGROUP_O): $(OUTPUT)/%.o : $(CGROUP_DIR)/%.c $(EXTRA_HDRS)
	$(CC) $(CFLAGS) $(CPPFLAGS) $(TARGET_ARCH) -c $< -o $@

EXTRA_CLEAN += $(LIBCGROUP_O)

CGROUP_DIR := $(selfdir)/cgroup

LIBCGROUP_C := lib/cgroup_util.c

LIBCGROUP_O := $(patsubst %.c, $(OUTPUT)/%.o, $(LIBCGROUP_C))

CFLAGS += -I$(CGROUP_DIR)/lib/include

$(LIBCGROUP_O): $(OUTPUT)/%.o : $(CGROUP_DIR)/%.c
	$(CC) $(CFLAGS) $(CPPFLAGS) $(TARGET_ARCH) -c $< -o $@

EXTRA_CLEAN += $(LIBCGROUP_O)

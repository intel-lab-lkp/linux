// SPDX-License-Identifier: GPL-2.0-only
/*
 * nvmemctl - Userspace tool to list and access NVMEM devices
 *
 * Copyright (C) 2026 Kuan-Wei Chiu <visitorckw@gmail.com>
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <dirent.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <errno.h>
#include <limits.h>

#define NVMEM_SYSPATH "/sys/bus/nvmem/devices"

struct nvmem_cell {
	char name[256];
	unsigned int byte_offset;
	unsigned int bit_offset;
	size_t size;
	struct nvmem_cell *next;
};

struct nvmem_device {
	char name[256];
	char type[32];
	bool force_ro;
	size_t size;
	struct nvmem_cell *cells;
	struct nvmem_device *next;
};

static int read_sysfs_string(const char *dev_name, const char *attr, char *buf, size_t buf_size)
{
	char path[PATH_MAX];
	size_t len;
	FILE *f;

	snprintf(path, sizeof(path), "%s/%s/%s", NVMEM_SYSPATH, dev_name, attr);
	f = fopen(path, "r");
	if (!f)
		return -1;

	if (fgets(buf, buf_size, f)) {
		len = strlen(buf);
		if (len > 0 && buf[len - 1] == '\n')
			buf[len - 1] = '\0';
	} else {
		buf[0] = '\0';
	}

	fclose(f);
	return 0;
}

static int write_sysfs_string(const char *dev_name, const char *attr, const char *val)
{
	char path[PATH_MAX];
	int fd, ret;

	snprintf(path, sizeof(path), "%s/%s/%s", NVMEM_SYSPATH, dev_name, attr);
	fd = open(path, O_WRONLY);
	if (fd < 0) {
		fprintf(stderr, "Failed to open %s for writing: %s\n", path, strerror(errno));
		return -1;
	}

	ret = write(fd, val, strlen(val));
	close(fd);

	if (ret < 0) {
		fprintf(stderr, "Failed to write to %s: %s\n", path, strerror(errno));
		return -1;
	}
	return 0;
}

static struct nvmem_cell *scan_nvmem_cells(const char *dev_name)
{
	char cells_path[PATH_MAX];
	DIR *dp;
	struct dirent *entry;
	struct nvmem_cell *head = NULL, *tail = NULL;

	snprintf(cells_path, sizeof(cells_path), "%s/%s/cells", NVMEM_SYSPATH, dev_name);
	dp = opendir(cells_path);
	if (!dp)
		return NULL;

	while ((entry = readdir(dp)) != NULL) {
		struct nvmem_cell *cell;
		char cell_file_path[PATH_MAX];
		struct stat st;
		int parsed;

		if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
			continue;

		cell = calloc(1, sizeof(*cell));
		if (!cell)
			break;

		parsed = sscanf(entry->d_name, "%[^@]@%x,%x", cell->name,
				&cell->byte_offset, &cell->bit_offset);
		if (parsed < 2) {
			snprintf(cell->name, sizeof(cell->name), "%s", entry->d_name);
			cell->byte_offset = 0;
			cell->bit_offset = 0;
		} else if (parsed == 2) {
			cell->bit_offset = 0;
		}

		snprintf(cell_file_path, sizeof(cell_file_path), "%s/%s/cells/%s",
			 NVMEM_SYSPATH, dev_name, entry->d_name);
		if (stat(cell_file_path, &st) == 0)
			cell->size = st.st_size;

		if (!head) {
			head = cell;
			tail = cell;
		} else {
			tail->next = cell;
			tail = cell;
		}
	}
	closedir(dp);
	return head;
}

static struct nvmem_device *scan_nvmem_devices(void)
{
	DIR *dp;
	struct dirent *entry;
	struct nvmem_device *head = NULL, *tail = NULL;

	dp = opendir(NVMEM_SYSPATH);
	if (!dp) {
		perror("Failed to open NVMEM sysfs directory");
		return NULL;
	}

	while ((entry = readdir(dp)) != NULL) {
		struct nvmem_device *dev;
		char ro_buf[8] = {0};
		char nvmem_file_path[PATH_MAX];
		struct stat st;

		if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
			continue;

		dev = calloc(1, sizeof(*dev));
		if (!dev)
			break;

		snprintf(dev->name, sizeof(dev->name), "%s", entry->d_name);

		if (read_sysfs_string(dev->name, "type", dev->type, sizeof(dev->type)) < 0)
			strncpy(dev->type, "Unknown", sizeof(dev->type));

		if (read_sysfs_string(dev->name, "force_ro", ro_buf, sizeof(ro_buf)) == 0)
			dev->force_ro = (ro_buf[0] == '1');

		snprintf(nvmem_file_path, sizeof(nvmem_file_path), "%s/%s/nvmem",
			 NVMEM_SYSPATH, dev->name);
		if (stat(nvmem_file_path, &st) == 0)
			dev->size = st.st_size;

		dev->cells = scan_nvmem_cells(dev->name);

		if (!head) {
			head = dev;
			tail = dev;
		} else {
			tail->next = dev;
			tail = dev;
		}
	}
	closedir(dp);
	return head;
}

static void free_nvmem_devices(struct nvmem_device *devices)
{
	struct nvmem_device *tmp_dev;
	struct nvmem_cell *c, *tmp_c;

	while (devices) {
		tmp_dev = devices;
		c = devices->cells;

		while (c) {
			tmp_c = c;
			c = c->next;
			free(tmp_c);
		}

		devices = devices->next;
		free(tmp_dev);
	}
}

static void dump_binary_data(const char *path, const char *title)
{
	int fd, i;
	unsigned char buf[16];
	ssize_t bytes_read;
	size_t offset = 0;

	fd = open(path, O_RDONLY);
	if (fd < 0) {
		fprintf(stderr, "Failed to open %s: %s\n", path, strerror(errno));
		return;
	}

	printf("Dumping %s:\n", title);
	printf("Offset    00 01 02 03 04 05 06 07 08 09 0A 0B 0C 0D 0E 0F | ASCII\n");
	printf("------------------------------------------------------------------\n");

	while ((bytes_read = read(fd, buf, sizeof(buf))) > 0) {
		printf("%08zX  ", offset);

		for (i = 0; i < 16; i++) {
			if (i < bytes_read)
				printf("%02X ", buf[i]);
			else
				printf("   ");
		}
		printf(" | ");
		for (i = 0; i < bytes_read; i++) {
			if (buf[i] >= 32 && buf[i] <= 126)
				printf("%c", buf[i]);
			else
				printf(".");
		}
		printf("\n");
		offset += bytes_read;
	}

	if (bytes_read < 0)
		perror("Error reading file");

	printf("------------------------------------------------------------------\n");
	close(fd);
}

static void cmd_list(void)
{
	struct nvmem_device *devices, *current;
	struct nvmem_cell *c;

	printf("%-15s | %-15s | %-5s | %-8s\n", "Device", "Type", "R/O", "Size(B)");
	printf("------------------------------------------------------------------\n");

	devices = scan_nvmem_devices();
	if (!devices) {
		printf("No NVMEM devices found.\n");
		return;
	}

	for (current = devices; current; current = current->next) {
		printf("%-15s | %-15s | %-5s | %-8zu\n",
		       current->name, current->type,
		       current->force_ro ? "Yes" : "No", current->size);

		for (c = current->cells; c; c = c->next) {
			printf("  |- Cell: %-15s (Offset: 0x%04X, Bit: %d, Size: %zu B)\n",
			       c->name, c->byte_offset, c->bit_offset, c->size);
		}
	}
	printf("------------------------------------------------------------------\n");
	free_nvmem_devices(devices);
}

static void print_usage(const char *progname)
{
	printf("Usage: %s <command> [args]\n\n", progname);
	printf("Commands:\n");
	printf("  list                          List all NVMEM devices and cells\n");
	printf("  dump   <device>               Hexdump the entire NVMEM device\n");
	printf("  read   <device> <cell_name>   Hexdump a specific cell within a device\n");
	printf("  lock   <device>               Set device read-only (force_ro=1)\n");
	printf("  unlock <device>               Set device read-write (force_ro=0)\n");
}

int main(int argc, char *argv[])
{
	char path[PATH_MAX];
	DIR *dp;
	struct dirent *entry;
	int found = 0;

	if (argc < 2) {
		print_usage(argv[0]);
		return 1;
	}

	if (!strcmp(argv[1], "list")) {
		cmd_list();
	} else if (!strcmp(argv[1], "dump") && argc == 3) {
		snprintf(path, sizeof(path), "%s/%s/nvmem", NVMEM_SYSPATH, argv[2]);
		dump_binary_data(path, argv[2]);
	} else if (!strcmp(argv[1], "read") && argc == 4) {
		snprintf(path, sizeof(path), "%s/%s/cells", NVMEM_SYSPATH, argv[2]);
		dp = opendir(path);
		if (!dp) {
			fprintf(stderr, "Could not open cells directory for %s\n", argv[2]);
			return 1;
		}

		while ((entry = readdir(dp)) != NULL) {
			if (strncmp(entry->d_name, argv[3], strlen(argv[3])) == 0) {
				snprintf(path, sizeof(path), "%s/%s/cells/%s",
					 NVMEM_SYSPATH, argv[2], entry->d_name);
				dump_binary_data(path, argv[3]);
				found = 1;
				break;
			}
		}
		closedir(dp);
		if (!found)
			fprintf(stderr, "Cell '%s' not found in device '%s'\n", argv[3], argv[2]);

	} else if (!strcmp(argv[1], "lock") && argc == 3) {
		if (write_sysfs_string(argv[2], "force_ro", "1") == 0)
			printf("Successfully locked %s (Read-Only)\n", argv[2]);
	} else if (!strcmp(argv[1], "unlock") && argc == 3) {
		if (write_sysfs_string(argv[2], "force_ro", "0") == 0)
			printf("Successfully unlocked %s (Read-Write)\n", argv[2]);
	} else {
		print_usage(argv[0]);
		return 1;
	}

	return 0;
}

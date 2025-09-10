// SPDX-License-Identifier: GPL-2.0+
/* devmem test ram_map.c
 *
 * Copyright (C) 2025 Red Hat, Inc. All Rights Reserved.
 * Written by Alessandro Carminati (acarmina@redhat.com)
 */

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include "ram_map.h"
#include "utils.h"
#include "debug.h"

static int calculate_bits(uint64_t max_addr)
{
	uint64_t value = max_addr + 1;
	int bits = 0;

	while (value > 0) {
		value >>= 1;
		bits++;
	}
	return bits;
}

uint64_t get_highest_ram_addr(const struct ram_map *map)
{
	if (!map || map->count == 0)
		return 0;
	return map->regions[map->count - 1].end;
}

static int fill_iomem_regions(FILE *fp, struct ram_map *map)
{
	char line[512];
	uint64_t start, end;
	char name[256];
	size_t idx = 0;

	while (fgets(line, sizeof(line), fp)) {
		if (sscanf(line, "%" SCNx64 "-%" SCNx64 " : %255[^\n]",
		    &start, &end, name) == 3) {
			map->regions[idx].start = start;
			map->regions[idx].end = end;
			map->regions[idx].name = strdup(name);
			if (!map->regions[idx].name) {
				perror("strdup");
				return -1;
			}
			idx++;
		}
	}
	return 0;
}

static size_t count_iomem_regions(FILE *fp)
{
	char line[512];
	size_t count = 0;
	uint64_t start, end;
	char name[256];

	rewind(fp);
	while (fgets(line, sizeof(line), fp)) {
		if (sscanf(line, "%" SCNx64 "-%" SCNx64 " : %255[^\n]",
		    &start, &end, name) == 3) {
			count++;
		}
	}
	rewind(fp);
	return count;
}

struct ram_map *parse_iomem(void)
{
	FILE *fp = fopen("/proc/iomem", "r");

	if (!fp) {
		perror("fopen /proc/iomem");
		return NULL;
	}

	size_t count = count_iomem_regions(fp);

	if (count == 0) {
		fprintf(stderr, "No parsable regions found in /proc/iomem.\n");
		fclose(fp);
		return NULL;
	}

	struct ram_map *map = calloc(1, sizeof(*map));

	if (!map) {
		perror("calloc map");
		fclose(fp);
		return NULL;
	}

	map->regions = calloc(count, sizeof(*map->regions));
	if (!map->regions) {
		perror("calloc regions");
		free(map);
		fclose(fp);
		return NULL;
	}
	map->count = count;

	if (fill_iomem_regions(fp, map) < 0) {
		fclose(fp);
		return NULL;
	}

	fclose(fp);
	return map;
}

void free_ram_map(struct ram_map *map)
{
	if (!map)
		return;

	for (size_t i = 0; i < map->count; i++)
		free(map->regions[i].name);

	free(map->regions);
	free(map);
}

uint64_t find_last_linear_byte(int fd, uint64_t low_start, uint64_t max_addr)
{
	uint64_t low = low_start + SAFE_OFFSET;
	uint64_t high = max_addr;
	uint64_t last_good = 0;

	while (low <= high) {
		uint64_t mid = low + (high - low) / 2;
		int ret = try_read_dev_mem(fd, mid, 0, NULL);

		if (ret > 0) {
			last_good = mid;
			low = mid + 1;
		} else if (ret == -EFAULT) {
			if (mid == 0)
				break;
			high = mid - 1;
		} else {
			deb_printf("Unexpected error at 0x%llx: %d\n",
					(unsigned long long)mid, -ret);
			break;
		}
	}
	return last_good;
}

void dump_ram_map(const struct ram_map *map)
{
	printf("Parsed RAM map (%zu regions):\n", map->count);

	for (size_t i = 0; i < map->count; i++) {
		printf("  %016" SCNx64 "-%016" SCNx64 " : %s\n",
			   map->regions[i].start,
			   map->regions[i].end,
			   map->regions[i].name);
	}
}

void report_physical_memory(const struct ram_map *map)
{
	uint64_t highest_addr = get_highest_ram_addr(map);

	if (highest_addr == 0) {
		printf("No System RAM regions detected!\n");
		return;
	}

	int bits = calculate_bits(highest_addr);

	printf("Highest physical RAM address: 0x%llx\n",
		   (unsigned long long)highest_addr);
	printf("Physical address width (installed RAM): %d bits\n", bits);
}

uint64_t find_high_system_ram_addr(const struct ram_map *map)
{
	for (size_t i = 0; i < map->count; i++) {
		if (strstr(map->regions[i].name, "System RAM") &&
			map->regions[i].start >= LOW_MEM_LIMIT) {
			return map->regions[i].start;
		}
	}
	return 0;
}

uint64_t pick_restricted_address(const struct ram_map *map)
{
	if (!map || !map->regions || map->count == 0)
		return 0;

	for (size_t i = 0; i < map->count; i++) {
		if ((!strcmp("System RAM", map->regions[i].name)) &&
		    (map->regions[i].start < LEGACY_MEM_START)) {
			uint64_t start = map->regions[i].start;
			uint64_t end   = map->regions[i].end;

			if (end > start)
				return start + (end - start) / 2;
		}
	}

	return 0;
}

uint64_t pick_outside_address(const struct ram_map *map)
{
	uint64_t max_addr = 0;

	if (!map || !map->regions || map->count == 0)
		return 0;

	for (size_t i = 0; i < map->count; i++) {
		if (max_addr < map->regions[i].end)
			max_addr = map->regions[i].end;
	}

	return max_addr + 0x1000;
}

uint64_t pick_valid_ram_address(const struct ram_map *map)
{
	uint64_t best_low = 0, best_size = 0;

	if (!map || !map->regions || map->count == 0)
		return 0;

	for (size_t i = 0; i < map->count; i++) {
		if (!strcmp("System RAM", map->regions[i].name)) {
			if (best_size < map->regions[i].end -
					      map->regions[i].start) {
				best_low = map->regions[i].end;
				best_size = map->regions[i].end -
				   map->regions[i].start;
			}
		}
	}
	return best_low + (best_size / 2);
}

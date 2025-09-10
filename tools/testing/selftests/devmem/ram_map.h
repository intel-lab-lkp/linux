/* SPDX-License-Identifier: GPL-2.0+ */
/* devmem test ram_map.h
 *
 * Copyright (C) 2025 Red Hat, Inc. All Rights Reserved.
 * Written by Alessandro Carminati (acarmina@redhat.com)
 */

#ifndef RAM_MAP_H
#define RAM_MAP_H

#define _GNU_SOURCE
#define SAFE_OFFSET (512ULL * 1024ULL)
#define LOW_MEM_LIMIT 0x100000ULL
#define LEGACY_MEM_START 0x10000

struct ram_region {
	uint64_t start;
	uint64_t end;
	char *name;
};

struct ram_map {
	struct ram_region *regions;
	size_t count;
};

uint64_t get_highest_ram_addr(const struct ram_map *map);
struct ram_map *parse_iomem(void);
void free_ram_map(struct ram_map *map);
uint64_t find_last_linear_byte(int fd, uint64_t low_start, uint64_t max_addr);
void dump_ram_map(const struct ram_map *map);
void report_physical_memory(const struct ram_map *map);
uint64_t find_high_system_ram_addr(const struct ram_map *map);
uint64_t pick_restricted_address(const struct ram_map *map);
uint64_t pick_outside_address(const struct ram_map *map);
uint64_t pick_valid_ram_address(const struct ram_map *map);

#endif

/* SPDX-License-Identifier: GPL-2.0-only */

#ifndef _ASM_MIPS_DPS_ROOT_H
#define _ASM_MIPS_DPS_ROOT_H

#ifdef CONFIG_CPU_LITTLE_ENDIAN
#ifdef CONFIG_64BIT
#define DPS_ROOT_PARTITION_TYPE_UUID "700bda43-7a34-4507-b179-eeb93d7a7ca3"
#else
#define DPS_ROOT_PARTITION_TYPE_UUID "37c58c8a-d913-4156-a25f-48b1b64e07f0"
#endif
#else
#ifdef CONFIG_64BIT
#define DPS_ROOT_PARTITION_TYPE_UUID "d113af76-80ef-41b4-bdb6-0cff4d3d4a25"
#else
#define DPS_ROOT_PARTITION_TYPE_UUID "e9434544-6e2c-47cc-bae2-12d6deafb44c"
#endif
#endif

#endif /* _ASM_MIPS_DPS_ROOT_H */

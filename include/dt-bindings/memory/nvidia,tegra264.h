/* SPDX-License-Identifier: (GPL-2.0-only OR BSD-2-Clause) */
/* Copyright (c) 2025, NVIDIA CORPORATION. All rights reserved. */

#ifndef DT_BINDINGS_MEMORY_NVIDIA_TEGRA264_H
#define DT_BINDINGS_MEMORY_NVIDIA_TEGRA264_H

/*
 * memory client IDs
 */

/* HOST1X read client */
#define TEGRA264_MEMORY_CLIENT_HOST1XR		0x16
/* VIC read client */
#define TEGRA264_MEMORY_CLIENT_VICR		0x6c
/* VIC Write client */
#define TEGRA264_MEMORY_CLIENT_VICW		0x6d
/* VI R5 Write client */
#define TEGRA264_MEMORY_CLIENT_VIW		0x72
#define TEGRA264_MEMORY_CLIENT_NVDECSRD2MC	0x78
#define TEGRA264_MEMORY_CLIENT_NVDECSWR2MC	0x79
/* Audio processor(APE) Read client */
#define TEGRA264_MEMORY_CLIENT_APER		0x7a
/* Audio processor(APE) Write client */
#define TEGRA264_MEMORY_CLIENT_APEW		0x7b
/* Audio DMA Read client */
#define TEGRA264_MEMORY_CLIENT_APEDMAR		0x9f
/* Audio DMA Write client */
#define TEGRA264_MEMORY_CLIENT_APEDMAW		0xa0
#define TEGRA264_MEMORY_CLIENT_GPUR02MC		0xb6
#define TEGRA264_MEMORY_CLIENT_GPUW02MC		0xb7
/* VI Falcon Read client */
#define TEGRA264_MEMORY_CLIENT_VIFALCONR	0xbc
/* VI Falcon Write client */
#define TEGRA264_MEMORY_CLIENT_VIFALCONW	0xbd
/* Read Client of RCE */
#define TEGRA264_MEMORY_CLIENT_RCER		0xd2
/* Write client of RCE */
#define TEGRA264_MEMORY_CLIENT_RCEW		0xd3
/* PCIE0/MSI Write clients */
#define TEGRA264_MEMORY_CLIENT_PCIE0W		0xd9
/* PCIE1/RPX4 Read clients */
#define TEGRA264_MEMORY_CLIENT_PCIE1R		0xda
/* PCIE1/RPX4 Write clients */
#define TEGRA264_MEMORY_CLIENT_PCIE1W		0xdb
/* PCIE2/DMX4 Read clients */
#define TEGRA264_MEMORY_CLIENT_PCIE2AR		0xdc
/* PCIE2/DMX4 Write clients */
#define TEGRA264_MEMORY_CLIENT_PCIE2AW		0xdd
/* PCIE3/RPX4 Read clients */
#define TEGRA264_MEMORY_CLIENT_PCIE3R		0xde
/* PCIE3/RPX4 Write clients */
#define TEGRA264_MEMORY_CLIENT_PCIE3W		0xdf
/* PCIE4/DMX8 Read clients */
#define TEGRA264_MEMORY_CLIENT_PCIE4R		0xe0
/* PCIE4/DMX8 Write clients */
#define TEGRA264_MEMORY_CLIENT_PCIE4W		0xe1
/* PCIE5/DMX4 Read clients */
#define TEGRA264_MEMORY_CLIENT_PCIE5R		0xe2
/* PCIE5/DMX4 Write clients */
#define TEGRA264_MEMORY_CLIENT_PCIE5W		0xe3
/* UFS Read client */
#define TEGRA264_MEMORY_CLIENT_UFSR		0x15c
/* UFS write client */
#define TEGRA264_MEMORY_CLIENT_UFSW		0x15d
/* HDA Read client */
#define TEGRA264_MEMORY_CLIENT_HDAR		0x17c
/* HDA Write client */
#define TEGRA264_MEMORY_CLIENT_HDAW		0x17d
/* Disp ISO Read Client */
#define TEGRA264_MEMORY_CLIENT_DISPR		0x182
/* MGBE0 Read mccif */
#define TEGRA264_MEMORY_CLIENT_MGBE0R		0x1a2
/* MGBE0 Write mccif */
#define TEGRA264_MEMORY_CLIENT_MGBE0W		0x1a3
/* MGBE1 Read mccif */
#define TEGRA264_MEMORY_CLIENT_MGBE1R		0x1a4
/* MGBE1 Write mccif */
#define TEGRA264_MEMORY_CLIENT_MGBE1W		0x1a5
/* VI1 R5 Write client */
#define TEGRA264_MEMORY_CLIENT_VI1W		0x1a6
/* SDMMC0 Read mccif */
#define TEGRA264_MEMORY_CLIENT_SDMMC0R		0x1c2
/* SDMMC0 Write mccif */
#define TEGRA264_MEMORY_CLIENT_SDMMC0W		0x1c3

#endif /* DT_BINDINGS_MEMORY_NVIDIA_TEGRA264_H */

/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Support for Intel Camera Imaging ISP subsystem.
 * Copyright (c) 2015, Intel Corporation.
 */

#ifndef __IA_CSS_MIPI_H
#define __IA_CSS_MIPI_H

/* @file
 * This file contains MIPI support functionality
 */

#include <type_support.h>
#include "ia_css_err.h"
#include "ia_css_stream_format.h"
#include "ia_css_input_port.h"

/**
 * ia_css_mipi_frame_calculate_size() - Calculate the size of a mipi frame.
 * @width: The width (in pixels) of the frame.
 * @height: The height (in lines) of the frame.
 * @format: The frame (MIPI) format.
 * @hasSOLandEOL: Whether frame (MIPI) contains (optional) SOL and EOF packets.
 * @embedded_data_size_words: Embedded data size in memory words.
 * @size_mem_words: The mipi frame size in memory words (32B).
 *
 * Calculate the size of a mipi frame, based on the resolution and format.
 *
 * Return: The error code.
 */
int
ia_css_mipi_frame_calculate_size(const unsigned int width,
				 const unsigned int height,
				 const enum atomisp_input_format format,
				 const bool hasSOLandEOL,
				 const unsigned int embedded_data_size_words,
				 unsigned int *size_mem_words);

#endif /* __IA_CSS_MIPI_H */

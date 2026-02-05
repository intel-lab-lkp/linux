/*
 * Copyright © 2026 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 *
 */

#ifndef _INTEL_OPROM_REGS_H_
#define _INTEL_OPROM_REGS_H_

#define PRIMARY_SPI_TRIGGER			_MMIO(0x102040)
#define PRIMARY_SPI_ADDRESS			_MMIO(0x102080)
#define PRIMARY_SPI_REGIONID			_MMIO(0x102084)
#define SPI_STATIC_REGIONS			_MMIO(0x102090)
#define   OPTIONROM_SPI_REGIONID_MASK		REG_GENMASK(7, 0)
#define OROM_OFFSET				_MMIO(0x1020c0)
#define   OROM_OFFSET_MASK			REG_GENMASK(20, 16)

#endif

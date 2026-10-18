#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
#
# Copyright (c) Mauro Carvalho Chehab <mchehab@kernel.org>
#
# Extract xc3028 firmware from vendor Windows drivers.
#
# 1) Download one of:
#    wget http://www.twinhan.com/files/AW/BDA T/20080303_V1.0.6.7.zip
#    wget http://www.stefanringel.de/pub/20080303_V1.0.6.7.zip
#    wget http://www.steventoth.net/linux/xc5000/HVR-12x0-14x0-17x0_1_25_25271_WHQL.zip
# 2) unzip -j 20080303_V1.0.6.7.zip 20080303_v1.0.6.7/UDXTTM6000.sys
#    unzip -j HVR-12x0-14x0-17x0_1_25_25271_WHQL.zip Driver85/hcw85bda.sys
# 3) ./extract_xc3028.py
# 4) cp xc3028-v24.fw xc3028-v27.fw /lib/firmware

import hashlib
import shutil
import struct
import sys

# Entry layout:
#   regular:  (offset, length, type, id_hi, id_lo)
#   scode:    (offset, length, type, id_hi, id_lo, if_mhz)   # 5th=None => regular

# Firmware 24 (from UDXTTM6000.sys, hash cb9deb5508a5e150af2880f5b0066d78)
FW24 = [
    (257752, 6635, 0x00000003, 0, 0),
    (264392, 6635, 0x00000007, 0, 0),
    (271040, 6525, 0x00000401, 0, 0),
    (277568, 6539, 0x00000c01, 0, 0),
    (284120, 6633, 0x00000001, 0, 0),
    (290760, 6617, 0x00000005, 0, 0),
    (297384, 161, 0x00000000, 0x1, 0x7),
    (297552, 169, 0x00000004, 0x1, 0x7),
    (297728, 161, 0x00000000, 0x2, 0x7),
    (297896, 169, 0x00000004, 0x2, 0x7),
    (298072, 161, 0x00000000, 0x4, 0x7),
    (298240, 169, 0x00000004, 0x4, 0x7),
    (298416, 161, 0x00000000, 0x8, 0x7),
    (298584, 169, 0x00000004, 0x8, 0x7),
    (298760, 161, 0x00000000, 0x3, 0xe0),
    (298928, 169, 0x00000004, 0x3, 0xe0),
    (299104, 161, 0x00000000, 0xc, 0xe0),
    (299272, 169, 0x00000004, 0xc, 0xe0),
    (299448, 161, 0x00000000, 0x0, 0x200000),
    (299616, 169, 0x00000004, 0x0, 0x200000),
    (299792, 161, 0x00000000, 0x0, 0x4000000),
    (299960, 169, 0x00000004, 0x0, 0x4000000),
    (300136, 149, 0x00010030, 0, 0),
    (300296, 149, 0x00000068, 0, 0),
    (300448, 149, 0x00000070, 0, 0),
    (300608, 149, 0x00000088, 0, 0),
    (300760, 149, 0x00000090, 0, 0),
    (300920, 149, 0x00000108, 0, 0),
    (301072, 149, 0x00000110, 0, 0),
    (301232, 149, 0x00000208, 0, 0),
    (301384, 149, 0x00000210, 0, 0),
    (301554, 135, 0x00000400, 0, 0),
    (301688, 161, 0x00000000, 0x0, 0x10),
    (301856, 169, 0x00000004, 0x0, 0x10),
    # 34 commented out in Perl
    (302032, 161, 0x00000000, 0xc, 0x400000),
    (302200, 161, 0x00000000, 0x0, 0x800000),
    (302368, 161, 0x00000000, 0x0, 0x8000),
    (302536, 161, 0x00001000, 0x0, 0x8000),
    (302704, 161, 0x00003000, 0x0, 0x8000),
    (302872, 169, 0x00000004, 0x0, 0x8000),
    (303048, 161, 0x00000000, 0x0, 0xb700),
    (303216, 161, 0x00001000, 0x0, 0xb700),
    (303384, 161, 0x00003000, 0x0, 0xb700),
    (303552, 161, 0x00000000, 0x0, 0x2000),
    (303720, 169, 0x00000004, 0x0, 0xb700),
    (303896, 169, 0x00001004, 0x0, 0xb700),
    (304072, 169, 0x00003004, 0x0, 0xb700),
    # SCODE: (offset, length, type, id_hi, id_lo, if_mhz)
    (309048, 192, 0x60000000, 0x0, 0x0, 3280),
    # 49 commented out
    (309432, 192, 0x60000000, 0x0, 0x0, 3440),
    (309624, 192, 0x60000000, 0x0, 0x0, 3460),
    (306936, 192, 0x60210020, 0x0, 0x0, 3800),
    (309240, 192, 0x60000000, 0x0, 0x0, 4000),
    (307128, 192, 0x60410020, 0x0, 0x0, 4080),
    (308856, 192, 0x60000000, 0x0, 0x0, 4200),
    (305208, 192, 0x60008000, 0x0, 0x8000, 4320),
    (309816, 192, 0x60000000, 0x0, 0x0, 4450),
    (304824, 192, 0x6002b004, 0x0, 0xb700, 4500),
    (305016, 192, 0x60023000, 0x0, 0x8000, 4600),
    (304440, 192, 0x620003e0, 0x0, 0x0, 4760),
    (308664, 192, 0x60000000, 0x0, 0x0, 4940),
    (307704, 192, 0x60000000, 0x0, 0x0, 5260),
    (307896, 192, 0x60008000, 0xf, 0x7, 5320),
    (304248, 192, 0x65000380, 0x0, 0x0, 5400),
    (306744, 192, 0x60110020, 0x0, 0x0, 5580),
    (305592, 192, 0x60000000, 0x3, 0x7, 5640),
    (305784, 192, 0x60000000, 0xc, 0x7, 5740),
    (307512, 192, 0x60000000, 0x0, 0x0, 5900),
    (305576, 192, 0x60008000, 0xc, 0x4c000f0, 6000),
    (306552, 192, 0x68050060, 0x0, 0x0, 6200),
    (305400, 192, 0x60000000, 0x0, 0x10, 6240),
    (308472, 192, 0x60008000, 0x0, 0x200000, 6320),
    (306360, 192, 0x60000000, 0x0, 0x200000, 6340),
    (308280, 192, 0x60008000, 0xc, 0x44000e0, 6500),
    (304632, 192, 0x60090020, 0x0, 0x0, 6580),
    (306168, 192, 0x60000000, 0x3, 0xe0, 6600),
    (308088, 192, 0x60008000, 0x3, 0xe0, 6680),
    (307320, 192, 0x60810020, 0x0, 0x0, 8140),
    # 79 commented out
]

# Firmware 27 (from hcw85bda.sys, hash 0e44dbf63bb0169d57446aec21881ff2)
FW27 = [
    (813432, 8718, 0x00000003, 0, 0),
    (822152, 8712, 0x00000007, 0, 0),
    (830872, 8562, 0x00000401, 0, 0),
    (839440, 8576, 0x00000c01, 0, 0),
    (848024, 8706, 0x00000001, 0, 0),
    (856736, 8682, 0x00000005, 0, 0),
    (865424, 161, 0x00000000, 0x1, 0x7),
    (865592, 169, 0x00000004, 0x1, 0x7),
    (865424, 161, 0x00000000, 0x2, 0x7),
    (865592, 169, 0x00000004, 0x2, 0x7),
    (866112, 161, 0x00000000, 0x4, 0x7),
    (866280, 169, 0x00000004, 0x4, 0x7),
    (866112, 161, 0x00000000, 0x8, 0x7),
    (866280, 169, 0x00000004, 0x8, 0x7),
    (866800, 161, 0x00000000, 0x3, 0xe0),
    (866968, 169, 0x00000004, 0x3, 0xe0),
    (867144, 161, 0x00000000, 0xc, 0xe0),
    (867312, 169, 0x00000004, 0xc, 0xe0),
    (867488, 161, 0x00000000, 0x0, 0x200000),
    (867656, 169, 0x00000004, 0x0, 0x200000),
    (867832, 161, 0x00000000, 0x0, 0x4000000),
    (868000, 169, 0x00000004, 0x0, 0x4000000),
    (868176, 149, 0x00010030, 0, 0),
    (868336, 149, 0x00000068, 0, 0),
    (868488, 149, 0x00000070, 0, 0),
    (868648, 149, 0x00000088, 0, 0),
    (868800, 149, 0x00000090, 0, 0),
    (868960, 149, 0x00000108, 0, 0),
    (869112, 149, 0x00000110, 0, 0),
    (868648, 149, 0x00000208, 0, 0),
    (868800, 149, 0x00000210, 0, 0),
    (869584, 135, 0x00000400, 0, 0),
    (869728, 161, 0x00000000, 0x0, 0x10),
    (869896, 169, 0x00000004, 0x0, 0x10),
    (870072, 169, 0x00000000, 0x10, 0x400000),
    (870248, 161, 0x00000000, 0xc, 0x400000),
    (870416, 161, 0x00000000, 0x0, 0x800000),
    (870584, 161, 0x00000000, 0x0, 0x8000),
    (870752, 161, 0x00001000, 0x0, 0x8000),
    (870920, 161, 0x00003000, 0x0, 0x8000),
    (871088, 169, 0x00000004, 0x0, 0x8000),
    (871264, 161, 0x00000000, 0x0, 0xb700),
    (871432, 161, 0x00001000, 0x0, 0xb700),
    (871600, 161, 0x00003000, 0x0, 0xb700),
    (871264, 161, 0x00000000, 0x0, 0x2000),
    (871936, 169, 0x00000004, 0x0, 0xb700),
    (872112, 169, 0x00001004, 0x0, 0xb700),
    (872288, 169, 0x00003004, 0x0, 0xb700),
    (811896, 192, 0x60000000, 0x0, 0x0, 3280),
    (813048, 192, 0x60000000, 0x0, 0x0, 3300),
    (812280, 192, 0x60000000, 0x0, 0x0, 3440),
    (812472, 192, 0x60000000, 0x0, 0x0, 3460),
    (809784, 192, 0x60210020, 0x0, 0x0, 3800),
    (812088, 192, 0x60000000, 0x0, 0x0, 4000),
    (809976, 192, 0x60410020, 0x0, 0x0, 4080),
    (811704, 192, 0x60000000, 0x0, 0x0, 4200),
    (808056, 192, 0x60008000, 0x0, 0x8000, 4320),
    (812664, 192, 0x60000000, 0x0, 0x0, 4450),
    (807672, 192, 0x6002b004, 0x0, 0xb700, 4500),
    (807864, 192, 0x60023000, 0x0, 0x8000, 4600),
    (807288, 192, 0x620003e0, 0x0, 0x0, 4760),
    (811512, 192, 0x60000000, 0x0, 0x0, 4940),
    (810552, 192, 0x60000000, 0x0, 0x0, 5260),
    (810744, 192, 0x60008000, 0xf, 0x7, 5320),
    (807096, 192, 0x65000380, 0x0, 0x0, 5400),
    (809592, 192, 0x60110020, 0x0, 0x0, 5580),
    (808440, 192, 0x60000000, 0x3, 0x7, 5640),
    (808632, 192, 0x60000000, 0xc, 0x7, 5740),
    (810360, 192, 0x60000000, 0x0, 0x0, 5900),
    (808824, 192, 0x60008000, 0xc, 0x4c000f0, 6000),
    (809400, 192, 0x68050060, 0x0, 0x0, 6200),
    (808248, 192, 0x60000000, 0x0, 0x10, 6240),
    (811320, 192, 0x60008000, 0x0, 0x200000, 6320),
    (809208, 192, 0x60000000, 0x0, 0x200000, 6340),
    (811128, 192, 0x60008000, 0xc, 0x44000e0, 6500),
    (807480, 192, 0x60090020, 0x0, 0x0, 6580),
    (809016, 192, 0x60000000, 0x3, 0xe0, 6600),
    (810936, 192, 0x60008000, 0x3, 0xe0, 6680),
    (810168, 192, 0x60810020, 0x0, 0x0, 8140),
    (812856, 192, 0x60000000, 0x0, 0x0, 8200),
]


def verify(filename, expected_hash):
    """Verify the md5 of the downloaded .sys file."""
    try:
        h = hashlib.md5()
        with open(filename, 'rb') as f:
            for chunk in iter(lambda: f.read(8192), b''):
                h.update(chunk)
    except OSError as e:
        sys.exit(f"Cannot open {filename}: {e}")
    if h.hexdigest() != expected_hash:
        sys.exit(f"Hash of extracted file does not match "
                 f"(found {h.hexdigest()}, expected {expected_hash}!)")


def get_hunk(infile, offset, length):
    infile.seek(offset)
    data = infile.read(length)
    if len(data) != length:
        sys.exit("Ran out of data")
    return data


def write_hunk(outfile, infile, offset, length):
    outfile.write(get_hunk(infile, offset, length))


def write_hunk_fix_endian(outfile, infile, offset, length):
    """Read a length-prefixed sequence of endian-swapped records."""
    data = get_hunk(infile, offset, length)
    out = bytearray()
    i = 0
    while i < length:
        size = data[i] * 256 + data[i + 1]
        out += data[i + 1:i + 2] + data[i:i + 1]
        i += 2
        if 0 < size < 0x8000:
            out += data[i:i + size]
            i += size
    outfile.write(out)


def main_firmware(outfile_path, name, version, nr_desc, entries, infile):
    """Write one complete firmware file."""
    name_bytes = name.encode('ascii').ljust(32, b'\x00')
    with open(outfile_path, 'wb') as out:
        out.write(name_bytes)
        out.write(struct.pack('<H', version))
        out.write(struct.pack('<H', nr_desc))

        for entry in entries:
            offset, length, typ, id_hi, id_lo = entry[:5]
            is_scode = len(entry) > 5
            if_mhz = entry[5] if is_scode else None

            out.write(struct.pack('<I', typ))
            out.write(struct.pack('<Q', (id_hi << 32) | id_lo))
            if is_scode:
                out.write(struct.pack('<H', if_mhz))
            out.write(struct.pack('<I', length))
            if is_scode:
                write_hunk(out, infile, offset, length)
            else:
                write_hunk_fix_endian(out, infile, offset, length)


def extract_firmware():
    targets = [
        ('UDXTTM6000.sys',
         'cb9deb5508a5e150af2880f5b0066d78',
         'xc3028-v24.fw', 'xc2028 firmware', 516, 77, FW24),
        ('hcw85bda.sys',
         '0e44dbf63bb0169d57446aec21881ff2',
         'xc3028-v27.fw', 'xc2028 firmware', 519, 80, FW27),
    ]
    for sysfile, md5, outfile, name, version, nr_desc, entries in targets:
        try:
            with open(sysfile, 'rb') as infile:
                verify(sysfile, md5)
                # Count active (non-SCODE) entries for the header;
                # the Perl passes 77/80 regardless, so use that.
                main_firmware(outfile, name, version, nr_desc, entries,
                              infile)
        except FileNotFoundError:
            continue


if __name__ == '__main__':
    extract_firmware()
    print("Firmwares generated.")
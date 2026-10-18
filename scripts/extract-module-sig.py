#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
#
# extract-module-sig.py <part> <module-file>
#   -0: unsigned module  -a: all sig data  -d: descriptor
#   -n: signer name      -k: key ID        -s: signature

import struct
import sys

MAGIC = b'~Module signature appended~\n'


def main(argv):
    if len(argv) != 2:
        sys.exit(f'Format: {sys.argv[0]} -[0adnks] module-file >out')

    part, modfile = argv

    try:
        with open(modfile, 'rb') as f:
            buf = f.read()
    except OSError as e:
        sys.exit(f'{modfile}: {e}')

    print(f'Read {len(buf)} bytes from module file', file=sys.stderr)

    if len(buf) < 12 + len(MAGIC):
        sys.exit('The file is too short to have a sig magic number and descriptor')

    p = len(buf) - len(MAGIC)
    if buf[p:] != MAGIC:
        sys.exit(f'Magic number not found at {len(buf)}')
    print(f'Found magic number at {len(buf)}', file=sys.stderr)

    p -= 12
    info_raw = buf[p:p + 12]
    algo = info_raw[0]
    hash_ = info_raw[1]
    id_type = info_raw[2]
    name_len = info_raw[3]
    kid_len = info_raw[4]
    sig_len = struct.unpack('>I', info_raw[8:12])[0]
    info = [algo, hash_, id_type, name_len, kid_len, sig_len]

    type_msg = {
        0: 'Found PGP key identifier',
        1: 'Found X.509 cert identifier',
        2: 'Found PKCS#7/CMS encapsulation',
    }.get(id_type, f'Found unsupported identifier type {id_type}')
    print(type_msg, file=sys.stderr)

    if p < name_len + kid_len + sig_len:
        sys.exit('Insufficient name+kid+sig data in file')

    p -= sig_len
    raw_sig = buf[p:p + sig_len]
    p -= kid_len
    raw_kid = buf[p:p + kid_len]
    p -= name_len
    raw_name = buf[p:p + name_len]
    module_len = p

    if sig_len > 0:
        n = min(16, sig_len)
        hs = ''.join(f'{b:02x}' for b in raw_sig[:n])
        print(f'Found {sig_len} bytes of signature [{hs}]', file=sys.stderr)

    if kid_len > 0:
        n = min(16, kid_len)
        hs = ''.join(f'{b:02x}' for b in raw_kid[:n])
        print(f'Found {kid_len} bytes of key identifier [{hs}]',
              file=sys.stderr)

    if name_len > 0:
        sys.stderr.buffer.write(
            f"Found {name_len} bytes of signer's name [".encode() +
            raw_name + b']\n')

    if part == '-0':
        sys.stdout.buffer.write(buf[:module_len])
    elif part == '-a':
        sys.stdout.buffer.write(buf[module_len:])
    elif part == '-d':
        print(' '.join(str(x) for x in info))
    elif part == '-n':
        if id_type == 2:
            print("No signer's name for PKCS#7 message type sig",
                  file=sys.stderr)
        sys.stdout.buffer.write(raw_name)
    elif part == '-k':
        if id_type == 2:
            print('No key ID for PKCS#7 message type sig', file=sys.stderr)
        sys.stdout.buffer.write(raw_kid)
    elif part == '-s':
        sys.stdout.buffer.write(raw_sig)
    else:
        sys.exit(f'Unknown part: {part}')
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
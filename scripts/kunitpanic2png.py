#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
#
# Copyright (c) 2025 Red Hat.
# Author: Jocelyn Falempe <jfalempe@redhat.com>

from argparse import ArgumentParser
from PIL import Image
import base64
import zlib

def get_dim(s):
    (w, h) = s.split('x')
    return (int(w), int(h))

def draw_image(img_data, width, height, n_img):

    decoded = base64.b64decode(img_data)
    unzipped = zlib.decompress(decoded)

    img = Image.frombytes("1", (width, height), unzipped)
    fname = f"panic_screen_{n_img}.png"
    img.save(fname)
    print(f"Image {width}x{height} saved to {fname}")

def main():
    parser = ArgumentParser(
        prog="kunitpanic2png",
        description="Read drm_panic kunit logs and translate that to png files")

    parser.add_argument("filename", help="log file from kunit, usually test.log")

    parsing_img = False
    img_data = ""
    n_img = 0

    args = parser.parse_args()
    with open(args.filename, "r") as f:
        for line in f.readlines():
            if line.startswith("KUNIT PANIC IMAGE DUMP START"):
                parsing_img = True
                width, height = get_dim(line.split()[-1])
                continue
            if line.startswith("KUNIT PANIC IMAGE DUMP END"):
                draw_image(img_data, width, height, n_img)
                parsing_img = False
                img_data = ""
                n_img += 1
                continue
            if parsing_img:
                img_data += line.strip()

main()

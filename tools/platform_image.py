#!/usr/bin/env python3
"""The platform's picture, pkg/pocket/Platforms/_images/n64.bin, to and from an image file.

    tools/platform_image.py <picture> <out.bin>         a 521 x 165 picture (any format Pillow reads) -> the Pocket's file
    tools/platform_image.py -d <in.bin> <out.png>       the Pocket's file -> a picture, to look at it

The Pocket's file is the picture in grey, turned a quarter turn counter-clockwise (165 pixels a row, 521 rows), two
bytes a pixel: its darkness (255 less its brightness: the Pocket shows 0 as white) and a zero.
"""
import sys

from PIL import Image

W, H = 521, 165


def main():
    a = sys.argv[1:]
    if len(a) == 3 and a[0] == "-d":
        data = open(a[1], "rb").read()
        if len(data) != W * H * 2:
            sys.exit(f"{a[1]}: {len(data)} bytes, not {W * H * 2}")
        dark = Image.frombytes("L", (H, W), data[0::2]).transpose(Image.ROTATE_270)
        dark.point(lambda v: 255 - v).save(a[2])
    elif len(a) == 2:
        im = Image.open(a[0])
        if im.size != (W, H):
            sys.exit(f"{a[0]}: {im.size[0]} x {im.size[1]}, not {W} x {H}")
        grey = im.convert("L").point(lambda v: 255 - v).transpose(Image.ROTATE_90).tobytes()
        out = bytearray(W * H * 2)
        out[0::2] = grey
        open(a[1], "wb").write(out)
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""Generate a small original wallpaper without optional imaging packages."""
import struct
import sys
import zlib


def chunk(kind, data):
    return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data))


def main(path):
    width, height = 375, 812
    rows = bytearray()
    for y in range(height):
        rows.append(0)
        for x in range(width):
            rows.extend((8 + 22 * y // height, 22 + 35 * x // width, 50 + 70 * y // height))
    header = struct.pack('>IIBBBBB', width, height, 8, 2, 0, 0, 0)
    with open(path, 'wb') as output:
        output.write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', header) +
                     chunk(b'IDAT', zlib.compress(rows, 9)) + chunk(b'IEND', b''))


if __name__ == '__main__':
    main(sys.argv[1])

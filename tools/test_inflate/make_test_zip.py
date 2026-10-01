"""Builds test data for test_inflate.cpp: a 300MB image (random data mixed
with long runs of zeros, like a disc image), the same image deflated into a
zip the way Redump's are, and that zip cut into the 32MB pieces the console
downloads it as. Everything goes in the folder given, default ./data.

Run:  python make_test_zip.py [folder]
"""

import os, random, struct, sys, zipfile, zlib

out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(os.path.abspath(__file__)), 'data')
os.makedirs(out, exist_ok=True)

IMAGE = os.path.join(out, 'image.iso')
ZIP = os.path.join(out, 'image.zip')
PIECE = 32 * 1024 * 1024
SIZE = 300 * 1024 * 1024

rng = random.Random(360)
with open(IMAGE, 'wb') as f:
    written = 0
    while written < SIZE:
        n = min(SIZE - written, rng.choice([64, 512, 4096]) * 1024)
        f.write(bytes(n) if rng.random() < 0.45 else rng.randbytes(n))
        written += n

with zipfile.ZipFile(ZIP, 'w', compression=zipfile.ZIP_DEFLATED, compresslevel=6) as z:
    z.write(IMAGE, 'image.iso')

data = open(ZIP, 'rb').read()
# Where the member's compressed data starts: after its local header.
name_len, extra_len = struct.unpack('<HH', data[26:30])
data_start = 30 + name_len + extra_len
info = zipfile.ZipFile(ZIP).infolist()[0]

for i in range(0, len(data), PIECE):
    open(os.path.join(out, 'P%05d.bin' % (i // PIECE)), 'wb').write(data[i:i + PIECE])

open(os.path.join(out, 'info.txt'), 'w').write('%d %d %d %d %d\n' % (
    data_start, info.compress_size, info.file_size, info.CRC, PIECE))
print('image %d bytes, zip %d bytes, data at %d, packed %d, crc %08X, %d pieces' % (
    info.file_size, len(data), data_start, info.compress_size, info.CRC, (len(data) + PIECE - 1) // PIECE))

// PC test for InflateSource.cpp: indexes a zip's deflate stream from 32MB
// pieces, checks the CRC, then reads the image back at random offsets through
// InflateSource and compares every byte with the original.
//
// Build and run with build_and_run.bat beside this file, after
// make_test_zip.py.

#include "../../InflateSource.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : "data";
    char path[512];

    _snprintf(path, sizeof(path), "%s\\info.txt", dir);
    FILE *f = fopen(path, "r");
    if (f == NULL) { printf("no %s - run make_test_zip.py first\n", path); return 1; }
    unsigned long long dataStart, packed, unpacked, piece;
    unsigned long crc;
    if (fscanf(f, "%I64u %I64u %I64u %lu %I64u", &dataStart, &packed, &unpacked, &crc, &piece) != 5) return 1;
    fclose(f);

    ZipPieces pieces(dir, piece);

    // Index, a megabyte at a time, as the console does while pieces land.
    InflateIndexer indexer(unpacked);
    unsigned char *buf = (unsigned char *)malloc(1024 * 1024);
    clock_t t0 = clock();
    for (unsigned long long at = dataStart; at < dataStart + packed && !indexer.Finished();)
    {
        unsigned long take = (unsigned long)((dataStart + packed - at) < 1024 * 1024 ? (dataStart + packed - at) : 1024 * 1024);
        if (!pieces.Read(at, buf, take) || !indexer.Feed(buf, take)) { printf("FAIL: indexing at %I64u\n", at); return 1; }
        at += take;
    }
    int count = 0;
    InflatePoint *points = indexer.TakePoints(&count);
    printf("indexed: finished %d, out %I64u (want %I64u), crc %08lX (want %08lX), %d points, %.1fs\n",
           indexer.Finished() ? 1 : 0, indexer.Out(), unpacked, indexer.Crc(), crc, count,
           (double)(clock() - t0) / CLOCKS_PER_SEC);
    if (!indexer.Finished() || indexer.Out() != unpacked || indexer.Crc() != crc) { printf("FAIL: index\n"); return 1; }

    // The original, to compare against.
    _snprintf(path, sizeof(path), "%s\\image.iso", dir);
    f = fopen(path, "rb");
    unsigned char *original = (unsigned char *)malloc((size_t)unpacked);
    if (f == NULL || original == NULL || fread(original, 1, (size_t)unpacked, f) != unpacked) { printf("no image\n"); return 1; }
    fclose(f);

    InflateSource source(&pieces, dataStart, packed, unpacked, points, count);
    unsigned char *got = (unsigned char *)malloc(4 * 1024 * 1024);

    // Random reads: small and large, forward and back, across piece and
    // point boundaries.
    srand(360);
    t0 = clock();
    int reads = 0;
    for (; reads < 3000; ++reads)
    {
        unsigned long len = (rand() % 4 == 0) ? (unsigned long)(rand() % (4 * 1024 * 1024)) + 1 : (unsigned long)(rand() % 65536) + 1;
        unsigned long long off = ((unsigned long long)rand() * RAND_MAX + rand()) % (unpacked - len);
        if (!source.ReadAt(off, got, len)) { printf("FAIL: read %lu at %I64u\n", len, off); return 1; }
        if (memcmp(got, original + off, len) != 0) { printf("FAIL: wrong bytes reading %lu at %I64u\n", len, off); return 1; }
    }
    printf("random reads: %d OK, %lu restarts, %.1fs\n", reads, source.Restarts(), (double)(clock() - t0) / CLOCKS_PER_SEC);

    // One sequential pass of the whole image in 1MB reads, as the copy does.
    t0 = clock();
    for (unsigned long long off = 0; off < unpacked; off += 1024 * 1024)
    {
        unsigned long len = (unsigned long)((unpacked - off) < 1024 * 1024 ? (unpacked - off) : 1024 * 1024);
        if (!source.ReadAt(off, got, len) || memcmp(got, original + off, len) != 0) { printf("FAIL: sequential at %I64u\n", off); return 1; }
    }
    printf("sequential pass OK, %.1fs, %lu restarts in all\n", (double)(clock() - t0) / CLOCKS_PER_SEC, source.Restarts());
    printf("PASS\n");
    return 0;
}

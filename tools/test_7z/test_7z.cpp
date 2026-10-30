// PC test for SevenZip.cpp: unpacks one .7z into a folder, as a homebrew
// install does, and says how it went. run_test.py makes the archives with
// 7-Zip and checks what comes out against what went in.
//
//   test_7z.exe archive.7z destFolder [folderToStrip]

#include "../../src/SevenZip.h"

#include <stdarg.h>
#include <stdio.h>

// OutputConsole.h's, on the console.
extern "C" void __cdecl dprintf(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    vprintf(format, args);
    va_end(args);
}

static int g_reports = 0;

static bool Progress(unsigned long long done, unsigned long long total)
{
    if (done > total)
        printf("progress past the end: %I64u of %I64u\n", done, total);
    g_reports++;
    return true;
}

int main(int argc, char **argv)
{
    if (argc < 3)
    {
        printf("test_7z.exe archive.7z destFolder [folderToStrip]\n");
        return 2;
    }
    int skipped = 0;
    const SevenZipResult result = ExtractSevenZip(argv[1], argv[2], argc > 3 ? argv[3] : NULL, Progress, &skipped);
    printf("%s: %s (%d progress reports, %d left out)\n", argv[1], SevenZipResultText(result), g_reports, skipped);
    return result == SEVEN_ZIP_OK ? 0 : 1;
}

/*
FILE : ImageDecode.cpp
PROJECT : Omni360
DESCRIPTION : JPEGs and PNGs to ARGB pixels through stb_image, and the scaling
              the covers need. See ImageDecode.h.
*/

#include "ImageDecode.h"

#include <xtl.h>
#include <stdlib.h>
#include <string.h>

// Only the two formats the Store serves, from memory, in plain C - there's no
// SSE here, and no thread-local storage wanted: the failure reason is only a
// hint for the log.
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#define STBI_NO_SIMD
#define STBI_NO_THREAD_LOCALS
#define STBI_MAX_DIMENSIONS 2048
#pragma warning(push, 0)
#include "stb_image.h"
#pragma warning(pop)

unsigned long *DecodeImageToArgb(const unsigned char *data, unsigned long size, int *outW, int *outH)
{
    int w = 0, h = 0, channels = 0;
    unsigned char *rgba = stbi_load_from_memory(data, (int)size, &w, &h, &channels, 4);
    if (rgba == NULL)
        return NULL;
    if (w < 1 || h < 1)
    {
        stbi_image_free(rgba);
        return NULL;
    }

    // R, G, B, A bytes to 0xAARRGGBB words, in place: each word is read
    // whole before it's written.
    unsigned long *pixels = (unsigned long *)rgba;
    const int count = w * h;
    for (int i = 0; i < count; ++i)
    {
        const unsigned char *p = rgba + i * 4;
        pixels[i] = ((unsigned long)p[3] << 24) | ((unsigned long)p[0] << 16) | ((unsigned long)p[1] << 8) | p[2];
    }

    *outW = w;
    *outH = h;
    return pixels; // stb_image allocates with malloc, so free() frees it
}

const char *ImageDecodeError()
{
    const char *reason = stbi_failure_reason();
    return reason != NULL ? reason : "?";
}

unsigned long *HalveArgb(const unsigned long *src, int w, int h)
{
    const int halfW = w / 2, halfH = h / 2;
    if (halfW < 1 || halfH < 1)
        return NULL;
    unsigned long *out = (unsigned long *)malloc((size_t)halfW * halfH * 4);
    if (out == NULL)
        return NULL;

    // All four channels at once: the top six bits of each channel summed,
    // then the bottom two with the rounding in a word of their own, so nothing
    // carries from one channel into the next. The same result as averaging
    // channel by channel, which took four times as long.
    for (int y = 0; y < halfH; ++y)
    {
        const unsigned long *r0 = src + (y * 2) * w;
        const unsigned long *r1 = r0 + w;
        unsigned long *o = out + y * halfW;
        for (int x = 0; x < halfW; ++x)
        {
            const unsigned long a = r0[x * 2], b = r0[x * 2 + 1], c = r1[x * 2], d = r1[x * 2 + 1];
            const unsigned long high = ((a >> 2) & 0x3F3F3F3F) + ((b >> 2) & 0x3F3F3F3F) +
                                       ((c >> 2) & 0x3F3F3F3F) + ((d >> 2) & 0x3F3F3F3F);
            const unsigned long low = (((a & 0x03030303) + (b & 0x03030303) + (c & 0x03030303) +
                                        (d & 0x03030303) + 0x02020202) >> 2) & 0x03030303;
            o[x] = high + low;
        }
    }
    return out;
}

// Between two pixels, f/256 of the way from a to b, rounded: red and blue
// together in one multiply, green in another - no floating point. Alpha is
// left to the caller.
static unsigned long LerpRgb(unsigned long a, unsigned long b, unsigned long f)
{
    const unsigned long g = 256 - f;
    const unsigned long rb = (((a & 0x00FF00FF) * g + (b & 0x00FF00FF) * f + 0x00800080) >> 8) & 0x00FF00FF;
    const unsigned long gg = (((a & 0x0000FF00) * g + (b & 0x0000FF00) * f + 0x00008000) >> 8) & 0x0000FF00;
    return rb | gg;
}

bool ScaleArgb(const unsigned long *src, int w, int h, float srcX, float srcY, float srcW, float srcH,
               int outW, int outH, unsigned long *out)
{
    if (outW < 1 || outH < 1 || outW > 2048 || outH > 2048)
        return false;

    // The 360's CPU pays dozens of cycles for every conversion between float
    // and integer - each goes through memory - so where each column and row
    // samples from is worked out once, and every pixel blended in integers:
    // a float sample per pixel took 124ms for a cover's front.
    const int n = outW + outH;
    int *table = (int *)malloc((size_t)n * 3 * sizeof(int));
    if (table == NULL)
        return false;
    int *first = table, *second = table + n;
    unsigned long *weight = (unsigned long *)(table + 2 * n);

    for (int i = 0; i < n; ++i)
    {
        const bool isCol = (i < outW);
        const int d = isCol ? i : i - outW;
        const int count = isCol ? outW : outH;
        const int limit = isCol ? w : h;
        float s = (isCol ? srcX : srcY) + ((float)d + 0.5f) * (isCol ? srcW : srcH) / (float)count - 0.5f;
        if (s < 0.0f) s = 0.0f;
        if (s > (float)(limit - 1)) s = (float)(limit - 1);
        const int a = (int)s;
        first[i] = a;
        second[i] = (a + 1 < limit) ? a + 1 : a;
        weight[i] = (unsigned long)((s - (float)a) * 256.0f + 0.5f);
    }

    const int *colA = first, *colB = second, *rowA = first + outW, *rowB = second + outW;
    const unsigned long *colF = weight, *rowF = weight + outW;
    for (int dy = 0; dy < outH; ++dy)
    {
        const unsigned long *row0 = src + rowA[dy] * w;
        const unsigned long *row1 = src + rowB[dy] * w;
        const unsigned long fy = rowF[dy];
        unsigned long *o = out + dy * outW;
        for (int dx = 0; dx < outW; ++dx)
        {
            const int a = colA[dx], b = colB[dx];
            const unsigned long fx = colF[dx];
            o[dx] = 0xFF000000 | LerpRgb(LerpRgb(row0[a], row0[b], fx), LerpRgb(row1[a], row1[b], fx), fy);
        }
    }

    free(table);
    return true;
}

double ImageTimerMs()
{
    static double msPerTick = 0.0;
    if (msPerTick == 0.0)
    {
        LARGE_INTEGER frequency;
        QueryPerformanceFrequency(&frequency);
        msPerTick = 1000.0 / (double)frequency.QuadPart;
    }
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return (double)now.QuadPart * msPerTick;
}

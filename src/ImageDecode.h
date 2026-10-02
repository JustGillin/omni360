#ifndef IMAGE_DECODE_H
#define IMAGE_DECODE_H

// ---------------------------------------------------------------------------
// JPEGs and PNGs to pixels, on any thread
// ---------------------------------------------------------------------------
//
// The covers, wallpapers and screenshots used to be decoded by D3DX on the UI
// thread, as it needs the device: about 20ms a cover and 200ms a wallpaper,
// each one a visible stutter. stb_image (src/stb_image.h, public domain)
// needs nothing but memory, so the workers that download the images decode
// them too, and the UI thread only copies finished pixels into a texture.
//
// Pixels are 0xAARRGGBB, row after row with no padding, malloc'd - free them
// with free().

// NULL if it isn't a JPEG or PNG it can read, or is over 2048 either way.
unsigned long *DecodeImageToArgb(const unsigned char *data, unsigned long size, int *outW, int *outH);

// Why the last decode failed, for the log. Shared between threads, so only
// a hint.
const char *ImageDecodeError();

// Half the size each way (an odd last row or column is dropped), each pixel
// the rounded average of four. NULL if out of memory.
unsigned long *HalveArgb(const unsigned long *src, int w, int h);

// The rectangle (srcX, srcY, srcW, srcH) of a w x h image, scaled bilinearly
// to outW x outH into out. False if out of memory, or outW or outH is over
// 2048.
bool ScaleArgb(const unsigned long *src, int w, int h, float srcX, float srcY, float srcW, float srcH,
               int outW, int outH, unsigned long *out);

// Milliseconds, for timing lines.
double ImageTimerMs();

#endif

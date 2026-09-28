#ifndef PNG_DECODE_H
#define PNG_DECODE_H

// A small, self-contained PNG decoder, used only for the cover thumbnails
// D3DX refuses.
//
// Why this exists: the Xbox 360 build of D3DX9 does not decode palette-indexed
// PNGs (colour type 3), and neither does XUI - xuierror.h says outright that
// XuiCreateTextureFromPNG takes "only 24bpp and 32bpp PNG files". Publishers'
// PNG optimisers routinely convert small icons to palette form because it
// roughly halves their size, so a real library turns up a few of them (007
// Legends, Fallout: New Vegas and MGS Peace Walker HD on the test console).
// The dashboard draws them because the system has its own decoder; that one
// is not exposed to titles.
//
// Decoding PNG means inflating its image data, and nothing in the XDK or this
// project does deflate (XMemDecompress is LZX, a different algorithm). So the
// inflater is here too, written for clarity rather than speed: it only ever
// runs on a handful of 64x64 thumbnails at startup.
//
// Deliberately free of any Xbox header, so it compiles unchanged on a PC and
// can be checked against a reference decoder there rather than one hardware
// build cycle at a time. Everything is read byte by byte - never through a
// wider pointer cast - so it gives the same answer on the big-endian console
// as on a little-endian PC.
//
// Supports the whole of the format a thumbnail could plausibly use: every
// colour type, every bit depth, tRNS transparency (palette alpha and colour
// keys), and Adam7 interlacing. Ancillary chunks it has no use for (gamma,
// colour profiles, text) are skipped.
//
// The input is untrusted - it comes out of content packages on disk - so every
// read is bounds-checked, the output size is fixed from the header before
// inflating a byte, and image dimensions are capped (PNG_MAX_DIMENSION) so a
// hostile header cannot demand an enormous allocation.

#define PNG_MAX_DIMENSION 512

// Decodes data[0..size) into a malloc'd array of width*height pixels, row-major
// from the top, each one 0xAARRGGBB as an integer value (so it is correct to
// store straight into a D3DFMT_LIN_A8R8G8B8 texel on either byte order).
//
// On success returns true and sets *outPixels (caller frees with PngFree),
// *outWidth and *outHeight. On failure returns false, leaves *outPixels NULL,
// and sets *outError to a short static description suitable for a log line.
bool PngDecodeToArgb(const unsigned char *data, unsigned long size,
                     unsigned long **outPixels,
                     unsigned long *outWidth, unsigned long *outHeight,
                     const char **outError);

void PngFree(unsigned long *pixels);

#endif

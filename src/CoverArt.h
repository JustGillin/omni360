#ifndef COVER_ART_H
#define COVER_ART_H

#include <xtl.h> // DWORD

// ---------------------------------------------------------------------------
// Box art for the library and the Store, from Xbox Live or xboxunity.net,
// cached on the hard drive
// ---------------------------------------------------------------------------
//
// Each game's tile is the front of its case art beside one fixed banner - the
// green XBOX 360 strip down the left edge, from Media/BoxArtBanner.png - which
// makes a square. An Xbox Live Arcade game has its own, from
// Media/BoxArtBannerXbla.png. The banner is the same on every tile of a kind;
// each cover's own header strip changed design over the console's life, so it
// isn't used.
//
// First choice is Xbox Live's box art, over plain HTTP:
//
//   download.xbox.com/content/images/66acd000-77fe-1000-9115-d802XXXXXXXX/1033/boxartlg.jpg
//       the front alone, 219x300 under a header strip, about 60KB.
//
// It used to be the fallback; it's first because it's a fraction of the work
// to decode and cut - xboxunity's insert is four times the pixels. At the
// size tiles are drawn it's about as sharp; only a game page's big box is a
// touch softer. For a game Xbox Live has no box art for - often a Japan-only
// disc - xboxunity, which needs no account:
//
//   Resources/Lib/CoverInfo.php?titleid=XXXXXXXX
//       a game's covers, as JSON. Each one's CoverID is what the next URL
//       wants; the first one marked Official is used, else the first listed.
//   Resources/Lib/Cover.php?size=large&cid=N
//       the cover itself: the whole case insert - back, spine, front - as a
//       900x600 JPEG of about half a megabyte.
//
// Both are cut to the front the same way. Covers already cached stay as they
// were fetched.
//
// A worker thread looks covers up, downloads them, decodes and cuts them
// (ImageDecode, not D3DX, so it needs no device), and reads the ones already
// cached - then puts the banner beside each and hands over the finished
// tile, so the UI thread only copies it into a texture:
//
//   game:\Covers\XXXXXXXX.bin   the front of the case, 418x512 ARGB pixels
//   game:\Covers\XXXXXXXX.none2 neither had anything; asked again after a week
//
// Only the front is cached, and the banner put beside it as each cover
// loads, so a new banner never means downloading the covers again.
//
// So the first launch downloads about 60KB a game (half a megabyte where it
// takes xboxunity), and every launch after reads only the cache. A game with no cover, or one that
// couldn't be fetched, keeps its icon tile.

#define COVER_SIZE     512                           // the tile's square, in pixels
#define COVER_BANNER_W 94                            // the banner down its left
#define COVER_FRONT_W  (COVER_SIZE - COVER_BANNER_W) // the front of the case, beside it

// False if the thread couldn't be created; covers then never arrive.
bool StartCoverArt();

// Waits up to timeoutMs for a download in progress. See StopDownloadQueue.
bool StopCoverArt(DWORD timeoutMs);

// The games to find covers for, in the order to get them - the library's
// order, so the first screenful arrives first. Replaces any earlier list;
// covers already handed over aren't asked for again.
void RequestCoverArt(const unsigned long *titleIds, int count);

// The Store's covers, for the tiles on screen: fetched ahead of the
// library's. Replaces any earlier Store list. Unlike the library's, a cover
// asked for again is read again - the Store keeps only the covers it's
// showing, so it may have let one go.
void RequestStoreCoverArt(const unsigned long *titleIds, int count);

// The Store's tiles are half size: a long letter can't then fill memory.
#define COVER_STORE_SIZE (COVER_SIZE / 2)

struct CoverData
{
    unsigned long titleId;
    unsigned long *pixels; // the tile, side x side ARGB pixels, malloc'd; the caller frees it
    int side;              // COVER_SIZE, or COVER_STORE_SIZE for the Store
    bool forStore;         // asked for by RequestStoreCoverArt
};

// The next tile ready to draw, if there is one. The UI thread calls this.
bool TakeCoverData(CoverData *out);

#endif

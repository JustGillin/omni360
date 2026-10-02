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
// makes a square. The banner is the same on every tile; each cover's own
// header strip changed design over the console's life, so it isn't used.
//
// First choice is Xbox Live's box art, over plain HTTP:
//
//   download.xbox.com/content/images/66acd000-77fe-1000-9115-d802XXXXXXXX/1033/boxartlg.jpg
//       the front alone, 219x300 under a header strip, about 60KB.
//
// It used to be the fallback. It's first because the cover is decoded and cut
// on the UI thread: a new cover from xboxunity cost an estimated 25-60ms there
// - a visible stutter while browsing - and this a fraction of it. At the size
// tiles are drawn it's about as sharp; only a game page's big box is a touch
// softer. For a game Xbox Live has no box art for - often a Japan-only disc -
// xboxunity, which needs no account:
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
// A worker thread looks covers up, downloads them, and reads the ones already
// cached. Cutting the front out of a download happens on the UI thread
// (GameListUI's PumpCoverArt), because it decodes through D3DX, which needs
// the device; the front comes back here to be written to the cache:
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

enum CoverDataKind
{
    COVER_DATA_PIXELS, // from the cache: the front, COVER_FRONT_W * COVER_SIZE ARGB pixels
    COVER_DATA_JPEG    // just downloaded: the whole case insert, to be cut down
};

struct CoverData
{
    unsigned long titleId;
    CoverDataKind kind;
    unsigned char *bytes; // malloc'd; the caller frees it
    unsigned long size;
    bool forStore;        // asked for by RequestStoreCoverArt
};

// The next cover ready to draw, if there is one. The UI thread calls this.
bool TakeCoverData(CoverData *out);

// Hands back the front cut from a COVER_DATA_JPEG, to be written to the
// cache. Takes ownership of pixels (COVER_FRONT_W * COVER_SIZE, malloc'd).
void SaveCoverPixels(unsigned long titleId, unsigned long *pixels);

// A download that wouldn't decode: noted like a game with no cover, so it
// isn't downloaded again on every launch.
void MarkCoverUnusable(unsigned long titleId);

#endif

#ifndef FEATURED_H
#define FEATURED_H

#include <stddef.h> // size_t

// ---------------------------------------------------------------------------
// The Store's featured games
// ---------------------------------------------------------------------------
//
// Chosen by featured.json in the repository, so they change without a new
// release - and mostly without anyone changing that either:
//
//   {
//     "events": [
//       { "from": "2026-10-05", "to": "2026-10-12", "label": "Gears of War: E-Day is out",
//         "games": ["4D5307D5", "4D53082D", "4D5308AB"] }
//     ],
//     "rotation": [
//       ["4D5307E6", "545407D8", "4D530AA4"],
//       ...
//     ]
//   }
//
// An event's games - one to five - are featured from its first day to its last,
// inclusive; otherwise the week picks the next set from the rotation, so it
// changes by itself every Monday. Only games the Store has count - a set
// with a title ID it doesn't know is passed over - and with nothing usable,
// the built-in three stand.
//
// Today is GitHub's date, from its reply, so a console clock that's wrong
// can't start an event early. The file is kept in game:\Store, so offline
// the last one fetched is used, with the console's clock.

// The same file can carry a notice, shown on the Queue page while it runs:
//
//   "notice": { "from": "2026-11-06", "to": "2026-11-13",
//               "text": "archive.org is slow for everyone today." }
//
// For what isn't any one download's fault and isn't worth a release -
// archive.org having a bad day, most of all, which otherwise reads as the
// app being broken. from and to are optional and inclusive; without them
// the notice stands until it's taken out of the file. A dated one says
// nothing while the day isn't known, rather than guess.
#define NOTICE_MAX 200

#define FEATURED_MAX_GAMES 5

struct FeaturedSet
{
    unsigned long titleIds[FEATURED_MAX_GAMES];
    int count;      // 1 to FEATURED_MAX_GAMES
    char label[64]; // over the tiles; "Featured" unless an event names itself
};

// Reads the kept file, then asks GitHub for the latest on a thread of its own.
// Returns at once.
void StartFeatured();

// Today's set. changeCount goes up each time it changes - the fetch finishing
// - so the Store can ask for the new games' wallpapers.
void GetFeatured(FeaturedSet *out, unsigned long *changeCount);

// Today's notice, or "" if there is none. Safe before StartFeatured.
void GetServiceNotice(char *out, size_t outSize);

#endif

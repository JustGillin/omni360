#ifndef FEATURED_H
#define FEATURED_H

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
//         "games": ["4D5307D5", "4D53082D", "4D53085B"] }
//     ],
//     "rotation": [
//       ["4D5307E6", "545407D8", "4D530AA4"],
//       ...
//     ]
//   }
//
// An event's three games are featured from its first day to its last,
// inclusive; otherwise the week picks the next set from the rotation, so it
// changes by itself every Monday. Only games the Store has count - a set
// with a title ID it doesn't know is passed over - and with nothing usable,
// the built-in three stand.
//
// Today is GitHub's date, from its reply, so a console clock that's wrong
// can't start an event early. The file is kept in game:\Store, so offline
// the last one fetched is used, with the console's clock.

#define FEATURED_GAMES 3

struct FeaturedSet
{
    unsigned long titleIds[FEATURED_GAMES];
    char label[64]; // over the tiles; "Featured" unless an event names itself
};

// Reads the kept file, then asks GitHub for the latest on a thread of its own.
// Returns at once.
void StartFeatured();

// Today's set. changeCount goes up each time it changes - the fetch finishing
// - so the Store can ask for the new games' wallpapers.
void GetFeatured(FeaturedSet *out, unsigned long *changeCount);

#endif

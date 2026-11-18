"""Writes Languages/en-ca.txt: Omni360 in English (Canada). An easter egg.

    python tools/make_en_ca.py

It's the English, plus:
  - OK and Confirm say "Give 'er", Cancel says "Yeah, no"
  - error headings apologise first ("Sorry, couldn't install")
  - some sentences pick up an "eh" or a "bud"/"buddy" on the end
  - a few hand-written lines

Rerun it after Strings.def changes (after --template), so new lines come
along. The picks are fixed - the same lines every run - so the file only
changes where the English did.
"""

import hashlib
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TEMPLATE = os.path.join(ROOT, 'Languages', 'TEMPLATE.txt')
OUTPUT = os.path.join(ROOT, 'Languages', 'en-ca.txt')

# Hand-written. Keys missing from the template are reported, not added.
LINES = {
    'BUTTON_OK': "Give 'er",
    'BUTTON_CONFIRM': "Give 'er",
    'BUTTON_CANCEL': 'Yeah, no',
    'TRY_AGAIN': "Try 'er again",
    'LOADING': 'Loading, eh...',
    'HOME_NO_GAMES_HINT': "Find something in the Store and give 'er, bud.",
    'HOME_NOTHING_PLAYED_HINT': 'Games you start show up here, buddy.',
    'LIB_NO_GAMES': 'No games here, eh',
    'NOTHING_IS_DOWNLOADING': "Nothing's downloading, bud",
    'SOMETHING_WENT_WRONG': 'Sorry, something went wrong, eh.',
    'OUT_OF_MEMORY': 'Sorry bud, out of memory',
    'UNKNOWN_ERROR': 'Sorry, no idea what happened there',
    'NOT_ENOUGH_SPACE': 'Sorry, not enough space, bud',
    'QUEUE_RECENTLY_INSTALLED': 'Recently installed, eh',
    'STORE_MORE_LIKE_THIS': 'More like this, bud',
    'SPEED_CONSOLE_LIMIT': "Your console's the slowest part, buddy: it can't keep up with any more than this.",
    'NIGHT_WAITING': "The console'll turn off when the queued downloads are done. Sleep tight, bud.",
    'COLLECTION_ONLY_ON_360': 'Only on Xbox 360, eh',
    # Canadian spelling, which the US English base doesn't have.
    'NAV_FAVOURITES': 'Favourites',
    'HOME_FAVOURITES': 'FAVOURITES',
    'NO_FAVOURITES_YET': 'No favourites yet, eh',
    'PRESS_Y_ON_A_GAME_TO': 'Press Y on a game in Games and choose Add to favourites.',
    'ADD_TO_FAVOURITES': 'Add to favourites',
    'REMOVE_FROM_FAVOURITES': 'Remove from favourites',
    'ADDED_TO_FAVOURITES': 'Added to favourites',
    'REMOVED_FROM_FAVOURITES': 'Removed from favourites',
}

# Short headings that are bad news: "Couldn't install" -> "Sorry, couldn't install".
ERROR_HEADING = re.compile(r"^(Couldn't|Could not|Can't|Download failed|Download was damaged|Keys not|"
                           r"Not enough space|Folder not found|Not saved|Not paired|Refused by|Failed$|"
                           r"Saved keys unusable|Emulator missing)")


def pick(key, salt, one_in):
    """A fixed pseudo-random choice per key, so reruns don't reshuffle."""
    h = int(hashlib.md5((salt + key).encode('utf-8')).hexdigest(), 16)
    return h % one_in == 0


def canadian(key, text):
    if key in LINES:
        return LINES[key]
    if ERROR_HEADING.match(text) and len(text) < 45 and not text.endswith('.'):
        return 'Sorry, ' + text[0].lower() + text[1:]
    # Whole sentences only - never a label, pill or name - ending in a
    # plain full stop, not a placeholder or an ellipsis.
    if text.endswith('.') and not text.endswith('..') and ' ' in text and len(text) < 140:
        body = text[:-1]
        if pick(key, 'eh', 5):
            return body + ', eh.'
        if pick(key, 'bud', 7):
            return body + (', buddy.' if pick(key, 'buddy', 2) else ', bud.')
    return text


def main():
    if hasattr(sys.stdout, 'reconfigure'):
        sys.stdout.reconfigure(encoding='utf-8', errors='replace')
    out = ['# Omni360 - English (Canada). An easter egg, made by tools/make_en_ca.py - edit that, not this.',
           '@name = English (Canada)',
           '@flag = ca',
           '']
    seen, changed = set(), 0
    for line in open(TEMPLATE, encoding='utf-8'):
        line = line.rstrip('\n')
        if line.startswith('@') or line.startswith('# Omni360') or line.startswith('# de.txt') or \
                line.startswith('# README.md'):
            continue
        m = re.match(r'^([A-Z0-9_]+) = (.*)$', line)
        if not m:
            out.append(line)
            continue
        key, text = m.group(1), m.group(2)
        seen.add(key)
        new = canadian(key, text)
        changed += (new != text)
        out.append('%s = %s' % (key, new))
    for key in LINES:
        if key not in seen:
            print('not in the template (renamed?): %s' % key)
    with open(OUTPUT, 'w', encoding='utf-8', newline='\n') as f:
        f.write('\n'.join(out).strip() + '\n')
    print('wrote %s - %d of %d lines Canadian' % (os.path.relpath(OUTPUT, ROOT), changed, len(seen)))


if __name__ == '__main__':
    main()

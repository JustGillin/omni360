"""Looks up games for featured.json, and checks the file.

  python tools/featured.py search "gears of war"   title IDs for a name
  python tools/featured.py check                   check featured.json
  python tools/featured.py check --art             ...and that each game has a wallpaper (asks Xbox Live)
  python tools/featured.py show 2026-11-02         what the Store features that day

Reads the Store's own lists (src/StoreTitles.h, src/XboxTitles.h), so a title
ID it finds is one the Store can open. Original Xbox games are listed but
can't be featured: Xbox Live has no wallpaper for any of them.
"""
import datetime
import json
import os
import re
import sys
import urllib.request

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..')
FEATURED = os.path.join(ROOT, 'featured.json')
MAX_GAMES = 5
LABEL_BYTES = 63  # FeaturedSet::label, less its terminator
ART_URL = 'http://download.xbox.com/content/images/66acd000-77fe-1000-9115-d802%s/1033/background.jpg'

ROW = re.compile(r'^\s*\{ "((?:[^"\\]|\\.)*)", 0x([0-9A-F]{8}), 0x[0-9A-F]+, \d+, \d+, \'.\'(, STORE_SYSTEM_XBOX)? \},')


def c_string(text):
    """A C string literal's contents - \\xNN escapes are UTF-8 bytes - as text."""
    raw = text.encode('latin-1').decode('unicode_escape').encode('latin-1')
    return raw.decode('utf-8', 'replace')


def load_games():
    """[(title ID, name, system)] in the Store's order: 360 discs, then Original Xbox."""
    games = []
    for header, system in (('StoreTitles.h', '360'), ('XboxTitles.h', 'Original Xbox')):
        with open(os.path.join(ROOT, 'src', header), encoding='latin-1') as f:
            for line in f:
                m = ROW.match(line)
                if m and m.group(2) != '00000000':
                    games.append((m.group(2), c_string(m.group(1)), system))
    return games


def by_id(games):
    """The game the Store shows for each title ID - the first with it, as StoreGameByTitleId."""
    out = {}
    for tid, name, system in games:
        out.setdefault(tid, (name, system))
    return out


def normalise(text):
    return re.sub(r'[^a-z0-9]+', ' ', text.lower().replace("'", '')).strip()


def search(words):
    games = load_games()
    shown = by_id(games)
    query = normalise(' '.join(words))
    found = 0
    for tid, name, system in games:
        if all(w in normalise(name) for w in query.split()):
            note = ''
            if system != '360':
                note = '   (Original Xbox - no wallpaper, can\'t be featured)'
            elif shown[tid][0] != name:
                note = '   (shows as "%s")' % shown[tid][0]
            print('%s  %s%s' % (tid, name, note))
            found += 1
    if found == 0:
        print('No Store game matches. Try fewer or shorter words.')


def parse_date(text, where, errors):
    try:
        return datetime.date.fromisoformat(text)
    except (TypeError, ValueError):
        errors.append('%s: "%s" isn\'t a date like 2026-11-02' % (where, text))
        return None


def check_games(games, where, shown, errors, warnings, ids):
    if not isinstance(games, list) or not 1 <= len(games) <= MAX_GAMES:
        errors.append('%s: needs 1 to %d games' % (where, MAX_GAMES))
        return
    if len(set(games)) != len(games):
        errors.append('%s: a game is in it twice' % where)
    for tid in games:
        if not isinstance(tid, str) or not re.fullmatch(r'[0-9A-F]{8}', tid):
            errors.append('%s: "%s" isn\'t a title ID like 4D5307E6 (8 hex digits, capitals)' % (where, tid))
        elif tid not in shown:
            errors.append('%s: %s isn\'t a game the Store has - the set would be skipped' % (where, tid))
        elif shown[tid][1] != '360':
            errors.append('%s: %s (%s) is Original Xbox - no wallpaper' % (where, tid, shown[tid][0]))
        else:
            ids.add(tid)


def check(art):
    shown = by_id(load_games())
    errors, warnings, ids = [], [], set()
    try:
        with open(FEATURED, encoding='utf-8') as f:
            data = json.load(f)
    except (OSError, ValueError) as e:
        print('ERROR: featured.json won\'t load: %s' % e)
        return 1

    events = data.get('events', [])
    spans = []
    for n, event in enumerate(events, 1):
        where = 'event %d (%s)' % (n, event.get('label', 'no label'))
        start = parse_date(event.get('from'), where, errors)
        end = parse_date(event.get('to'), where, errors)
        if start and end and end < start:
            errors.append('%s: ends before it starts' % where)
        label = event.get('label', '')
        if len(label.encode('utf-8')) > LABEL_BYTES:
            errors.append('%s: the label is longer than %d bytes - keep it short' % (where, LABEL_BYTES))
        check_games(event.get('games'), where, shown, errors, warnings, ids)
        if start and end:
            for other_start, other_end, other in spans:
                if start <= other_end and other_start <= end:
                    warnings.append('%s overlaps %s - the earlier one in the file wins those days' % (where, other))
            spans.append((start, end, where))

    rotation = data.get('rotation', [])
    if not rotation:
        errors.append('rotation: needs at least one set, for weeks with no event')
    for n, games in enumerate(rotation, 1):
        check_games(games, 'rotation set %d' % n, shown, errors, warnings, ids)

    # The weeks ahead with no event, so they're easy to spot.
    today = datetime.date.today()
    monday = today - datetime.timedelta(days=today.weekday())
    gaps = []
    for w in range(12):
        day = monday + datetime.timedelta(weeks=w)
        if not any(s <= day + datetime.timedelta(days=d) <= e for s, e, _ in spans for d in range(7)):
            gaps.append(day.isoformat())
    if gaps:
        warnings.append('weeks starting %s have no event - the rotation fills them' % ', '.join(gaps))

    if art:
        for tid in sorted(ids):
            try:
                with urllib.request.urlopen(ART_URL % tid.lower(), timeout=10) as r:
                    ok = r.status == 200
            except Exception:
                ok = False
            if not ok:
                errors.append('%s (%s) has no wallpaper on Xbox Live - pick another game' % (tid, shown[tid][0]))

    for w in warnings:
        print('warning: ' + w)
    for e in errors:
        print('ERROR: ' + e)
    print('%d error(s), %d warning(s); %d events, %d rotation sets' % (len(errors), len(warnings), len(events), len(rotation)))
    return 1 if errors else 0


def show(day_text):
    """What Featured.cpp picks for a day: an event running then, else the week's rotation set."""
    shown = by_id(load_games())
    day = datetime.date.fromisoformat(day_text)
    with open(FEATURED, encoding='utf-8') as f:
        data = json.load(f)
    for event in data.get('events', []):
        if datetime.date.fromisoformat(event['from']) <= day <= datetime.date.fromisoformat(event['to']):
            print('Event: %s' % event.get('label', 'Featured'))
            for tid in event['games']:
                print('  %s  %s' % (tid, shown.get(tid, ('NOT IN THE STORE',))[0]))
            return
    rotation = data.get('rotation', [])
    week = ((day - datetime.date(1970, 1, 5)).days) // 7
    n = week % len(rotation)
    print('Rotation set %d of %d (Featured)' % (n + 1, len(rotation)))
    for tid in rotation[n]:
        print('  %s  %s' % (tid, shown.get(tid, ('NOT IN THE STORE',))[0]))


if __name__ == '__main__':
    sys.stdout.reconfigure(encoding='utf-8')
    if len(sys.argv) >= 3 and sys.argv[1] == 'search':
        search(sys.argv[2:])
    elif len(sys.argv) >= 2 and sys.argv[1] == 'check':
        sys.exit(check('--art' in sys.argv))
    elif len(sys.argv) == 3 and sys.argv[1] == 'show':
        show(sys.argv[2])
    else:
        print(__doc__)

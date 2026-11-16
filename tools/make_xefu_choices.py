"""Builds src/XefuChoices.h: which emulator each Original Xbox game off
Microsoft's list plays best on, from ConsoleMods' compatibility list.

The 360 plays Original Xbox games through one of twelve emulators (xefu
files) on HddX. A game on Microsoft's list names its own; one off it gets
whatever the hacked xbox.xex falls back to, which often isn't the one that
works. ConsoleMods' community list rates every game on every emulator, so
for each game off the list this picks the best-rated one, and Omni360 puts
it in place before the game starts - see XboxCompat.h.

    python tools/make_xefu_choices.py           # from tools/data/xefu_compat.tsv
    python tools/make_xefu_choices.py --fetch   # refresh that from the wiki first

The wiki's table is kept as tools/data/xefu_compat.tsv - a name and twelve
ratings a line - so a build doesn't need the network. Hand choices, which
win over the list, are in tools/data/xefu_choices.txt. Title IDs come from
src/XboxTitles.h by name, so run make_store_titles.py --system xbox first.

Run from the repo root.
"""

import argparse
import os
import re
import sys
import urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TSV = os.path.join(ROOT, "tools", "data", "xefu_compat.tsv")
HAND = os.path.join(ROOT, "tools", "data", "xefu_choices.txt")
BACKCOMPAT = os.path.join(ROOT, "tools", "data", "xbox_backcompat.txt")
XBOX_TITLES = os.path.join(ROOT, "src", "XboxTitles.h")
OUT = os.path.join(ROOT, "src", "XefuChoices.h")
WIKI = ("https://consolemods.org/wiki/index.php?title="
        "Xbox_360:Original_Xbox_Games_Compatibility_List&action=raw")

# The wiki's columns, in its order - also XEFU_* in XboxCompat.h.
EMULATORS = ["xefu", "xefu2", "xefu3", "xefu5", "xefu1_1", "xefu6", "xefu7", "xefu7b",
             "xefu2019", "xefu2021a", "xefu2021b", "xefu2021c"]
# Ratings, worst to best; the digit is what XefuChoices.h keeps.
RATINGS = {"untested": 0, "unplayable": 1, "intro": 2, "menus": 3, "in-game": 4, "playable": 5, "supported": 5}
# Between equal ratings: xefu7 first - the one most of the list's notes
# settle on, and the one ConsoleMods' per-game configs are for - then
# newest to oldest.
PREFER = ["xefu7", "xefu7b", "xefu2021c", "xefu2021b", "xefu2021a", "xefu2019", "xefu6",
          "xefu5", "xefu1_1", "xefu3", "xefu2", "xefu"]


NO_PICK = 255


def c_string(text):
    """A C string literal for UTF-8 text: octal escapes, which can't run on."""
    out = []
    for b in text.encode("utf-8"):
        ch = chr(b)
        if ch in '"\\?':
            out.append("\\" + ch)
        elif 0x20 <= b < 0x7F:
            out.append(ch)
        else:
            out.append("\\%03o" % b)
    return '"' + "".join(out) + '"'


def key(name):
    name = re.sub(r"\\x([0-9A-Fa-f]{2})", lambda m: chr(int(m.group(1), 16)), name)
    name = name.lower().replace("&", "and")
    return re.sub(r"[^a-z0-9]", "", name)


# The wiki's emulator names as the Store's chooser shows them: "Emulator 7".
# Longest first, so xefu7b isn't read as xefu7 and a "b".
# A bare "xefu" in a note is the emulator in general far more often than
# the first one, so it's left to the generic rule.
EMULATOR_WORDS = sorted([(e, i + 1) for i, e in enumerate(EMULATORS) if e != "xefu"] + [("xefu1", 1)],
                        key=lambda p: -len(p[0]))
ISSUES_MAX = 700  # the notes screen's room, about


def clean_issues(text):
    """A Known Issues cell as a player reads it: wiki markup gone, and each
    emulator named as the Store names it, never xefu."""
    t = text.strip()
    t = re.sub(r"<br\s*/?>", " ", t, flags=re.I)
    t = re.sub(r"<[^>]*>", "", t)
    t = re.sub(r"\[\[(?:[^|\]]*\|)?([^\]]*)\]\]", r"\1", t)   # [[page|text]]
    t = re.sub(r"\[https?://\S+\s+([^\]]*)\]", r"\1", t)       # [url text]
    t = re.sub(r"\[https?://\S+\]", "", t)
    t = re.sub(r"\{\{[^}]*\}\}", "", t)
    t = t.replace("'''", "").replace("''", "")
    t = re.sub(r"(?i)config[ -]?loader\s+", "", t)              # its config is part of Emulator 7 here
    for word, n in EMULATOR_WORDS:
        spaced = word.replace("xefu", r"xefu[ _]?")
        t = re.sub(r"(?i)\b%s\b" % spaced, "Emulator %d" % n, t)
    t = re.sub(r"(?i)\b(?:older|newer|newest|oldest|other|all|later|earlier)\s+xefu'?s\b",
               lambda m: m.group(0).split()[0] + " emulators", t)
    t = re.sub(r"(?i)\bxefu'?s\b", "emulators", t)
    t = re.sub(r"(?i)\bxefu\b", "the emulator", t)
    t = re.sub(r"\s+", " ", t).strip()
    if t.lower().rstrip(".") in ("none", "n/a", "no issues", "-", ""):
        return ""
    if len(t) > ISSUES_MAX:
        t = t[:ISSUES_MAX].rsplit(" ", 1)[0] + "..."
    return t


def fetch_tsv():
    req = urllib.request.Request(WIKI, headers={"User-Agent": "Xbox Live Client"})
    text = urllib.request.urlopen(req, timeout=120).read().decode("utf-8")
    rows = []
    for block in text.split("\n|-")[1:]:
        cells = [c.strip() for c in block.strip().split("\n") if c.startswith("|") and not c.startswith("|}")]
        if len(cells) < 13:
            continue
        name = re.sub(r"\[\[(?:[^|\]]*\|)?([^\]]*)\]\]", r"\1", cells[0][1:].strip())
        name = re.sub(r"<[^>]*>|''+", "", name).strip()
        ratings = []
        for c in cells[1:13]:
            m = re.search(r"\{\{([A-Za-z-]+)(\|CFG)?", c)
            r = RATINGS.get(m.group(1).lower(), 0) if m else 0
            ratings.append("%d%s" % (r, "c" if m and m.group(2) else ""))
        issues = clean_issues(cells[14][1:]) if len(cells) > 14 else ""
        rows.append((name, ratings, issues))
    with open(TSV, "w", encoding="utf-8", newline="\n") as f:
        f.write("# ConsoleMods' Original Xbox compatibility list, the hacked emulators' table:\n")
        f.write("# %s\n" % WIKI)
        f.write("# A game's name, then its rating on each of %s.\n" % ", ".join(EMULATORS))
        f.write("# 0 untested, 1 doesn't work, 2 stops at the intro, 3 menus, 4 in-game, 5 playable;\n")
        f.write("# a 'c' after it: one of ConsoleMods' per-game configs, for xefu7's config loader.\n")
        f.write("# Last, the list's known issues, cleaned up for players (see clean_issues).\n")
        f.write("# Written by tools/make_xefu_choices.py --fetch - don't edit by hand.\n")
        for name, ratings, issues in rows:
            f.write("%s\t%s\t%s\n" % (name, "\t".join(ratings), issues))
    print("fetched %d games into %s" % (len(rows), TSV))


def load_tsv():
    rows = []
    for line in open(TSV, encoding="utf-8"):
        if line.startswith("#") or not line.strip():
            continue
        parts = line.rstrip("\n").split("\t")
        rows.append((parts[0], parts[1:13], parts[13] if len(parts) > 13 else ""))
    return rows


def load_titles():
    """Name key -> the title IDs of every disc by that name, from XboxTitles.h."""
    text = open(XBOX_TITLES, encoding="utf-8").read()
    ids = {}
    for tid, zipname in re.findall(r'\{ \d+, \d+, 0x[0-9A-F]+, 0x[0-9A-F]+, \d+, 0x([0-9A-F]{8}), \d+ULL, "((?:[^"\\]|\\.)*)" \}', text):
        ids.setdefault(key(zipname.split(" (")[0]), set()).add(int(tid, 16))
    for name, tid in re.findall(r'\{ "((?:[^"\\]|\\.)*)", 0x([0-9A-F]{8}), [^}]*STORE_SYSTEM_XBOX \}', text):
        ids.setdefault(key(name), set()).add(int(tid, 16))
    return ids


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--fetch", action="store_true", help="refresh the table from the wiki first")
    args = parser.parse_args()
    if args.fetch or not os.path.exists(TSV):
        fetch_tsv()

    listed = set()
    for line in open(BACKCOMPAT, encoding="utf-8"):
        if line.strip() and not line.startswith("#"):
            listed.add(int(line.split("\t")[0], 16))

    titles = load_titles()
    choices = {}   # title ID -> (emulator index or NO_PICK, config, ratings, name, issues)
    unmatched = 0
    for name, ratings, issues in load_tsv():
        tids = titles.get(key(name))
        if not tids:
            tids = titles.get(key(name.replace(":", " -")))
        if not tids:
            unmatched += 1
            continue
        scores = [int(r[0]) for r in ratings]
        best = max(scores)
        # Nothing past the logo: no pick, but the issues are still worth saying.
        i, config = NO_PICK, False
        if best >= 2:
            i = EMULATORS.index(next(e for e in PREFER if scores[EMULATORS.index(e)] == best))
            config = ratings[i].endswith("c")
        if i == NO_PICK and not issues:
            continue
        for tid in tids:
            if tid not in listed:
                choices[tid] = (i, config, "".join(str(s) for s in scores), name, issues)

    hand = 0
    if os.path.exists(HAND):
        for line in open(HAND, encoding="utf-8"):
            line = line.split("#")[0].strip()
            if not line:
                continue
            tid, emu = [p.strip() for p in line.split("=")]
            tid = int(tid, 16)
            old = choices.get(tid, (0, False, "0" * 12, "", ""))
            choices[tid] = (EMULATORS.index(emu), False, old[2], old[3], old[4])
            hand += 1

    L = []
    L.append("// GENERATED by tools/make_xefu_choices.py - do not edit by hand.")
    L.append("//")
    L.append("// Which emulator each Original Xbox game off Microsoft's list plays best")
    L.append("// on, from ConsoleMods' compatibility list (tools/data/xefu_compat.tsv)")
    L.append("// and tools/data/xefu_choices.txt. %d games; %d of the list's names not in" % (len(choices), unmatched))
    L.append("// XboxTitles.h; %d chosen by hand." % hand)
    L.append("")
    L.append("#ifndef XEFU_CHOICES_H")
    L.append("#define XEFU_CHOICES_H")
    L.append("")
    L.append("#define XEFU_NO_PICK %d" % NO_PICK)
    L.append("")
    L.append("struct XefuChoice")
    L.append("{")
    L.append("    unsigned long titleId;")
    L.append("    unsigned char emulator; // XEFU_*, the wiki's column order; XEFU_NO_PICK if none gets far")
    L.append("    unsigned char config;   // 1: xefu7 with its config loader and ConsoleMods' config")
    L.append("    char ratings[13];       // each emulator's, '0' untested to '5' playable")
    L.append("    const char *issues;     // the list's known issues, for players; \"\" if none")
    L.append("};")
    L.append("")
    L.append("#ifdef XEFU_CHOICES_DATA")
    L.append("static const XefuChoice kXefuChoices[] =")
    L.append("{")
    for tid in sorted(choices):
        i, config, scores, name, issues = choices[tid]
        L.append('    { 0x%08X, %d, %d, "%s", %s }, // %s' % (tid, i, 1 if config else 0, scores, c_string(issues),
                                                       name.encode("ascii", "replace").decode()))
    L.append("};")
    L.append("#endif")
    L.append("")
    L.append("#endif")
    with open(OUT, "w", encoding="utf-8", newline="\r\n") as f:
        f.write("\n".join(L) + "\n")
    print("%d games, %d unmatched, %d by hand -> %s" % (len(choices), unmatched, hand, OUT))


if __name__ == "__main__":
    main()

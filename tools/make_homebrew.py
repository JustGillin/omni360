"""
Builds src/HomebrewCatalog.h: the homebrew in the ConsoleMods repo - the
same .ini files Aurora's Xbox 360 Homebrew Store script reads - for the
Store's Homebrew page.

    python tools/make_homebrew.py

Reads tools/data/homebrew/: ConsoleModsRepo.ini, which names each category's
.ini and its type, and the category .ini files themselves, saved from
consolemods.org. Only the App, Homebrew and Emulator categories are used -
the types the Apps, Homebrew and Emulators pages show; Dashboards and Other
install to paths of their own. To update the list, save newer copies of the
category files over those there and run this again.

Each category file is a section an item:

    [retroarch]
    itemTitle=RetroArch 360
    itemVersion=1.0.0.2
    itemAuthor=LibRetro
    itemDescription=An official port of RetroArch 1.0.0.2.\\nMore on a second line.
    itemSize=12MB                   (sometimes)
    dataurl=https://consolemods.org/wiki/images/9/94/RetroArch-360.7z
    dataurl2=...                    (an item too big for one archive: each is unpacked in turn)
    path=RetroArch\\

An item without a dataurl that's a URL is left out, as is a second section
of the same name.
"""
import os, re, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DATA = os.path.join(ROOT, "tools", "data", "homebrew")
OUT = os.path.join(ROOT, "src", "HomebrewCatalog.h")

# The repo's types, as the app's kinds - in the order the page lists them.
KINDS = [("App", "HOMEBREW_KIND_APP"), ("Homebrew", "HOMEBREW_KIND_HOMEBREW"), ("Emulator", "HOMEBREW_KIND_EMULATOR")]


def read_ini(path):
    """Sections in file order, each a dict of its keys; the first of a name stands."""
    sections, order, current = {}, [], None
    with open(path, encoding="utf-8", errors="replace") as f:
        for raw in f:
            line = raw.strip()
            if not line or line[0] in ";#":
                continue
            m = re.match(r"^\[(.+)\]$", line)
            if m:
                name = m.group(1).strip()
                if name in sections:
                    print(f"  {os.path.basename(path)}: [{name}] again - the first is kept", file=sys.stderr)
                    current = None
                else:
                    current = sections[name] = {}
                    order.append(name)
                continue
            if current is not None and "=" in line:
                key, value = line.split("=", 1)
                current.setdefault(key.strip(), value.strip())
    return [(name, sections[name]) for name in order]


def c_string(text):
    """A C string literal: the repo writes line breaks as a literal \\n."""
    text = text.replace("\\n", "\n")
    out = []
    for ch in text:
        if ch == "\\":
            out.append("\\\\")
        elif ch == '"':
            out.append('\\"')
        elif ch == "\n":
            out.append("\\n")
        elif ord(ch) < 0x20:
            continue
        elif ord(ch) < 0x80:
            out.append(ch)
        else:
            # UTF-8, byte by byte - the UI draws UTF-8. Closed off so a hex
            # digit after it isn't read as part of the escape.
            out.append("".join(f'\\x{b:02X}""' for b in ch.encode("utf-8")))
    return '"' + "".join(out) + '"'


def main():
    repo = read_ini(os.path.join(DATA, "ConsoleModsRepo.ini"))
    types = {t for t, _ in KINDS}
    items, urls = [], []
    for _, section in repo:
        kind = section.get("type")
        iniurl = section.get("iniurl", "")
        if kind not in types or not iniurl:
            continue
        filename = iniurl.rsplit("/", 1)[-1]
        path = os.path.join(DATA, filename)
        if not os.path.exists(path):
            print(f"  {filename} isn't in tools/data/homebrew - its {kind} items are left out", file=sys.stderr)
            continue
        for name, item in read_ini(path):
            title = item.get("itemTitle", "")
            parts = [item[k] for k in sorted((k for k in item if re.fullmatch(r"dataurl\d*", k)),
                                             key=lambda k: int(k[7:] or 1))]
            parts = [u for u in parts if re.match(r"^https?://\S+$", u)]
            folder = item.get("path", "").strip().rstrip("\\")
            if not title or not parts or not folder or ":" in folder or "/" in folder:
                print(f"  {filename}: [{name}] has no usable download or folder - left out", file=sys.stderr)
                continue
            items.append({
                "kind": dict(KINDS)[kind],
                "title": title,
                "version": item.get("itemVersion", ""),
                "author": item.get("itemAuthor", ""),
                "description": item.get("itemDescription", ""),
                "size": item.get("itemSize", "").strip(),
                "folder": folder,
                "firstUrl": len(urls),
                "urlCount": len(parts),
            })
            urls.extend(parts)

    order = {k: i for i, (_, k) in enumerate(KINDS)}
    items.sort(key=lambda it: (order[it["kind"]], it["title"].lower()))
    counts = {k: sum(1 for it in items if it["kind"] == k) for _, k in KINDS}

    lines = [
        "// GENERATED by tools/make_homebrew.py - do not edit by hand.",
        "//",
        "// The homebrew in the ConsoleMods repo (consolemods.org), from the .ini files",
        "// in tools/data/homebrew - what Aurora's Xbox 360 Homebrew Store script lists.",
        f"// {len(items)} items: " + ", ".join(f"{counts[k]} {t.lower()}" for t, k in KINDS) + ".",
        "",
        "#ifndef HOMEBREW_CATALOG_H",
        "#define HOMEBREW_CATALOG_H",
        "",
        "// What an item is, which is the page it ends up on and the folder kind it",
        "// installs into.",
        "enum HomebrewKind",
        "{",
        "    HOMEBREW_KIND_APP,",
        "    HOMEBREW_KIND_HOMEBREW,",
        "    HOMEBREW_KIND_EMULATOR,",
        "    HOMEBREW_KIND_COUNT",
        "};",
        "",
        "struct HomebrewItem",
        "{",
        "    const char *title;",
        "    const char *version;     // may be empty",
        "    const char *author;      // may be empty",
        "    const char *description; // may be empty; may hold '\\n's",
        "    const char *size;        // as the repo gives it, \"12MB\"; usually empty",
        "    const char *folder;      // the folder it installs as, under its kind's folder",
        "    unsigned short firstUrl; // into kHomebrewUrls: its .7z archives, unpacked in turn",
        "    unsigned char urlCount;",
        "    unsigned char kind;      // HomebrewKind",
        "};",
        "",
        f"#define HOMEBREW_ITEM_COUNT {len(items)}",
        "",
        "#ifdef HOMEBREW_CATALOG_DATA",
        "",
        "static const char *const kHomebrewUrls[] = {",
    ]
    lines += [f"    {c_string(u)}," for u in urls]
    lines += ["};", "", "// By kind, then title.", "static const HomebrewItem kHomebrewItems[HOMEBREW_ITEM_COUNT] = {"]
    for it in items:
        lines.append("    { " + ", ".join([
            c_string(it["title"]), c_string(it["version"]), c_string(it["author"]),
            c_string(it["description"]), c_string(it["size"]), c_string(it["folder"]),
            str(it["firstUrl"]), str(it["urlCount"]), it["kind"],
        ]) + " },")
    lines += ["};", "", "#endif // HOMEBREW_CATALOG_DATA", "", "#endif", ""]

    with open(OUT, "w", encoding="utf-8", newline="\r\n") as f:
        f.write("\n".join(lines))
    print(f"Wrote {OUT}: {len(items)} items, {len(urls)} archives")


if __name__ == "__main__":
    main()

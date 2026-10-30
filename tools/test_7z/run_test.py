"""
PC test for SevenZip.cpp, the homebrew installs' .7z unpacker: builds
test_7z.exe (build.bat), makes archives of a made-up app with 7-Zip in the
ways homebrew is packed - LZMA2, LZMA with the BCJ filter, PPMd, solid and
not, inside a folder of the app's name or not - unpacks each, and checks
every file that comes out against the one that went in.

    python tools/test_7z/run_test.py [path\\to\\7z.exe [real-archive.7z]]

With a real archive too - RetroArch-360-V3.7z, whose 266MB block is past
the console's limit - it's unpacked as the console would and compared with
7-Zip's own unpacking of it.

Needs 7-Zip (C:\\Program Files\\7-Zip\\7z.exe by default) and Visual Studio
2010. On a PC the decoder runs little-endian; the console's big-endian build
is only checked on the console.
"""
import filecmp, os, random, shutil, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
WORK = os.path.join(HERE, "build", "work")
SEVEN = sys.argv[1] if len(sys.argv) > 1 else r"C:\Program Files\7-Zip\7z.exe"


def make_app(root):
    """A small app folder: an executable, text, a binary blob, nested folders, an empty folder."""
    rnd = random.Random(360)
    os.makedirs(os.path.join(root, "media", "skins", "default"))
    os.makedirs(os.path.join(root, "empty"))
    with open(os.path.join(root, "default.xex"), "wb") as f:
        f.write(b"XEX2" + bytes(rnd.getrandbits(8) for _ in range(200000)))
    with open(os.path.join(root, "readme.txt"), "w") as f:
        f.write("RetroArch-ish readme\n" * 3000)
    with open(os.path.join(root, "media", "skins", "default", "bg.png"), "wb") as f:
        f.write(bytes(rnd.getrandbits(8) for _ in range(50000)))
    with open(os.path.join(root, "media", "config.ini"), "w") as f:
        f.write("[video]\nwidth=1280\n")
    # Names FATX can't hold - left out of the unpacking, the rest unharmed.
    with open(os.path.join(root, "media", "a sample rom with a name far too long for FATX.zip"), "wb") as f:
        f.write(bytes(rnd.getrandbits(8) for _ in range(70000)))
    with open(os.path.join(root, "media", "Sonic, the demo.txt"), "w") as f:
        f.write("comma\n")


def fatx_ok(name):
    return 0 < len(name) <= 42 and not any(c in '"*+,/:;<=>?|' for c in name)


def drop_unwritable(root):
    """What the console can't write, removed from a tree 7-Zip unpacked, to compare with."""
    for dirpath, dirnames, filenames in os.walk(root, topdown=True):
        for name in [d for d in dirnames if not fatx_ok(d)]:
            shutil.rmtree(os.path.join(dirpath, name))
            dirnames.remove(name)
        for name in filenames:
            if not fatx_ok(name):
                os.remove(os.path.join(dirpath, name))


def same_tree(a, b):
    cmp = filecmp.dircmp(a, b)
    if cmp.left_only or cmp.right_only or cmp.funny_files:
        print("   differs:", cmp.left_only, cmp.right_only, cmp.funny_files)
        return False
    _, mismatch, errors = filecmp.cmpfiles(a, b, cmp.common_files, shallow=False)
    if mismatch or errors:
        print("   differs:", mismatch, errors)
        return False
    return all(same_tree(os.path.join(a, d), os.path.join(b, d)) for d in cmp.common_dirs)


def main():
    if subprocess.call(["cmd", "/c", os.path.join(HERE, "build.bat")]) != 0:
        return 1
    # The real limit, and a 64KB one that streams every block bigger than that.
    exes = [("whole", os.path.join(HERE, "build", "test_7z.exe")),
            ("streamed", os.path.join(HERE, "build", "test_7z_stream.exe"))]

    shutil.rmtree(WORK, ignore_errors=True)
    src = os.path.join(WORK, "src", "MyApp")
    make_app(src)
    # What should come out: the app less the names the drive can't hold.
    expected = os.path.join(WORK, "expected", "MyApp")
    shutil.copytree(src, expected)
    drop_unwritable(expected)

    # (name, 7z switches, archive the folder itself, strip it, streamable).
    # A big block with a filter in front, or PPMd, can't be streamed: the
    # 64KB build refuses those, as the console would one over 192MB.
    cases = [
        ("lzma2-solid",        ["-m0=LZMA2", "-ms=on"],  False, None,            True),
        ("lzma2-not-solid",    ["-m0=LZMA2", "-ms=off"], False, None,            True),
        ("lzma-solid",         ["-m0=LZMA", "-ms=on"],   False, None,            True),
        ("lzma-bcj",           ["-m0=BCJ", "-m1=LZMA"],  False, None,            False),
        ("ppmd",               ["-m0=PPMd"],             False, None,            False),
        ("in-folder-stripped", ["-m0=LZMA2"],            True,  "MyApp",         True),
        ("in-folder-other",    ["-m0=LZMA2"],            True,  "SomethingElse", True),
    ]
    failures = 0
    for name, switches, inFolder, strip, streamable in cases:
        archive = os.path.join(WORK, name + ".7z")
        cwd = os.path.dirname(src) if inFolder else src
        subprocess.check_call([SEVEN, "a", "-bd", "-y", archive] + switches + ["MyApp" if inFolder else "*"],
                              cwd=cwd, stdout=subprocess.DEVNULL)
        for mode, exe in exes:
            dest = os.path.join(WORK, "out", mode, name)
            args = [exe, archive, dest] + ([strip] if strip else [])
            unpacked = subprocess.call(args) == 0
            if mode == "streamed" and not streamable:
                ok = not unpacked and not os.path.exists(os.path.join(dest, "readme.txt"))
                print(f"{'ok  ' if ok else 'FAIL'} {name} ({mode}: refused, nothing written)")
            else:
                # Stripped: the app's files straight in dest. Not stripped,
                # but in a folder: under dest\MyApp. Not in one: straight in dest.
                expect = os.path.join(dest, "MyApp") if inFolder and strip != "MyApp" else dest
                ok = unpacked and same_tree(expected, expect)
                print(f"{'ok  ' if ok else 'FAIL'} {name} ({mode})")
            failures += 0 if ok else 1

    # Not a 7z at all: refused, nothing written.
    junk = os.path.join(WORK, "junk.7z")
    with open(junk, "wb") as f:
        f.write(b"not an archive" * 100)
    dest = os.path.join(WORK, "out", "junk")
    refused = subprocess.call([exes[0][1], junk, dest]) != 0 and not os.path.exists(os.path.join(dest, "readme.txt"))
    print(f"{'ok  ' if refused else 'FAIL'} junk is refused")
    failures += 0 if refused else 1

    # A real archive, when one's given: its biggest block streamed at the
    # console's limit, and every file checked against what 7-Zip unpacks.
    if len(sys.argv) > 2:
        real = sys.argv[2]
        mine = os.path.join(WORK, "out", "real-mine")
        theirs = os.path.join(WORK, "out", "real-7zip")
        subprocess.check_call([SEVEN, "x", "-bd", "-y", "-o" + theirs, real], stdout=subprocess.DEVNULL)
        drop_unwritable(theirs)
        ok = subprocess.call([exes[0][1], real, mine]) == 0 and same_tree(theirs, mine)
        print(f"{'ok  ' if ok else 'FAIL'} {os.path.basename(real)} matches 7-Zip's unpacking")
        failures += 0 if ok else 1

    print("all passed" if failures == 0 else f"{failures} failed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())

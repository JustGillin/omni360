# Omni360 Setup Guide

Start-to-finish setup for a console you already have modded. Adapted from X-Store's original setup guide by 951261 — the transfer and Aurora advice is still theirs in substance; the rest describes a different application and has been rewritten.

## Prerequisites

**Required:**
- A soft-modded (BadUpdate, ABadAvatar) or hard-modded (RGH, JTAG) Xbox 360.
- A working internet connection on the console. Ethernet recommended.
- An archive.org account. The `msx360gcdlc` collection is marked private in its own metadata, so an anonymous request gets a hard 401 — authentication is genuinely required, not optional.

**Not required:**
- Xbox Live
- Stealth server
- A PC, after the initial file transfer. That is the whole point of the app.

## 1. Build or obtain the XEX

There is no release download. Build `XboxTLS2.vcxproj` from `Omni360.sln` in Visual Studio 2010 with the official Microsoft Xbox 360 XDK; the Release configuration produces `Omni360.xex`. See `COMPILING.md`.

## 2. Get your archive.org API keys

Log in to archive.org, then visit [archive.org/account/s3.php](https://archive.org/account/s3.php) and copy your **IAS3 access key and secret key**. These are archive.org's official script-friendly credentials — the same pair their own `ia` command-line tool uses. They are *not* your account password.

Create a plain text file with the access key on line 1 and the secret key on line 2. You will transfer it as `ArchiveOrgKeys.txt` alongside the XEX.

Doing this on a PC is strongly recommended over the alternative, which is typing a roughly 40-character secret key on an on-screen keyboard with a controller. The app will prompt for the keys on first run if the file is absent, and save them to the same location afterwards.

**This file is plain text either way.** That is fine on a console only you use. Delete it if anyone else has physical or FTP access.

## 3. Optional: settings.txt

The app works with no settings file. Two keys override its defaults, and they are separate because your game library and your content folder are often on different drives:

- `xbla-path:` — where downloaded content is written. Defaults to `Hdd1:\Content\0000000000000000`. The name is inherited from X-Store, which used the same key.
- `games-path:` — where your installed library is scanned for the picker. Defaults to `Hdd1:\Games`.

If Aurora keeps your GOD/disc games in a dedicated folder — `Hdd1:\Games\{TitleID}\00007000\{ContentID}` is what testing found — point `games-path:` there. `Content\0000000000000000` on its own holds DLC and title updates, not your actual games, so scanning it will produce an empty or near-empty list.

## 4. Transfer files to the console

Copy `Omni360.xex`, `ArchiveOrgKeys.txt`, and `settings.txt` if you made one, into a single folder on the console — something like `Hdd1:\Apps\Omni360\` or `Usb0:\Apps\Omni360\`. Keep them together, and do not put any other `.xex` in that folder.

Three ways to get them across:

- **USB drive** — easiest. Copy to a FAT32 drive from your PC or phone, then either run from the USB or use a file manager (Aurora, XEX Menu) to move the files to the internal HDD.
- **FTP** — copy straight to the HDD or a USB over the network.
- **Xbox 360 Neighborhood** — works, but is prone to crashing and failed transfers. Not recommended.

## 5. Point Aurora at the right folders

If you use Aurora, add the paths you configured to **Settings → Content → Path**, and set scan depth to at least 4. Without this, downloaded content will not show up in Aurora even though it installed correctly.

While you are there, make sure Aurora can see `Omni360.xex` so you can launch it from the home screen.

## 6. Run it

Launch `Omni360.xex` from a file manager or your dashboard's home screen.

1. The app mounts drive aliases, then scans your library and reads each title's name and cover art out of its STFS package header. This is entirely offline and takes a moment on a large library.
2. Your games appear as an icon list. Move with the **D-pad or left stick**.
3. Press **A** for a title's DLC, or **Y** for its title updates.
4. It authenticates to archive.org and searches by fuzzy name match, then shows you what it found, ranked. **Nothing downloads automatically** — you confirm the pack yourself. Anything already installed is marked as such.
5. Pick one and it downloads with a live progress bar, installing straight to the correct folder. Press **B** to back out at any point; B returns you to the previous screen rather than quitting.

Because matching is deliberately loose — it has to cope with release-group naming like `Halo.1.Combat.Evolved.Anniversary` — read the filename before confirming. That check is the reason auto-selection was removed.

## Troubleshooting

`game:\DebugInfo.txt` is the log, and it keeps recording even once the drawn UI takes over the screen. Check it first.

- **Empty game list** — `games-path:` is pointing somewhere without games. See step 3.
- **401 from archive.org** — key file missing, malformed, or on the wrong line order. Access key line 1, secret key line 2.
- **No results for a game you know exists** — the collection may genuinely not have it, or the display name may be too abbreviated to match. Not every title has DLC or updates archived.
- **Garbled game names** — STFS display-name encoding. `StfsParser.cpp` detects UTF-8 versus UTF-16BE per file; non-Latin characters decode to `?` since no full Unicode re-encoding is implemented.

There is no automatic resume. A failed download has to be retried by hand.

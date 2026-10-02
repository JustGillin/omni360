# Releasing

How to publish a new version of Omni360 so that people's consoles can update to it from **Settings → Updates**.

The app checks GitHub's releases for this repository, and will only install a release whose XEX carries a valid signature from the project's key. So every release is the XEX **and** its signature, `Omni360.xex.sig`, made by `tools/sign_release.py`.

## Once: the signing key

The key is made once, ever, and lives outside the repository:

```bash
python tools/sign_release.py --new-key
```

That writes the private key to `%USERPROFILE%\.omni360\release-signing-key.pem` and its public half to `UpdateKey.h`, which is compiled into the app. It refuses to replace a key that already exists.

**Back the key file up** somewhere safe and private — a password manager, or an encrypted USB stick. Never commit it, and never attach it to a release.

- **If it leaks,** someone could sign a build that consoles would accept, so move to a new key:
  1. Make it somewhere new: `python tools/sign_release.py --new-key --key <new key path>`. That rewrites `UpdateKey.h`.
  2. Build that, and sign it with the **old** key, so existing installs accept it. The script won't, as the old key no longer matches `UpdateKey.h`, so use `openssl` directly: `openssl dgst -sha256 -sign <old key path> -out Omni360.xex.sig Release/Omni360.xex`.
  3. Release it. Sign every release after it with the new key, and move the new key to the default path (or pass `--key`).
- **If it's lost,** builds signed with a new key won't install over versions that trust the old one. Everyone would have to replace `Omni360.xex` by hand once, to a build carrying the new `UpdateKey.h`; from then on, updates work again.

`openssl` does the signing. Git for Windows includes one, and the script finds it on the `PATH` or in Git's folder.

## Each release

### 1. Set the version

- `CURRENT_VERSION` in `settings.h`, e.g. `"0.2.4-beta"`.
- The **Status** line in `README.md`.

Versions are compared as `MAJOR.MINOR.PATCH`, then an optional suffix after a `-`: `0.3.0` is newer than `0.3.0-beta`, which is newer than `0.2.9`, and `beta.10` is newer than `beta.2`. The tag is the version with a `v` in front: `v0.2.4-beta`.

### 2. Build and sign

Build the Release configuration (see `COMPILING.md`), then:

```bash
python tools/sign_release.py Release/Omni360.xex
```

It checks the file is an XEX and that your key matches `UpdateKey.h`, writes `Release/Omni360.xex.sig`, verifies it with the public key alone, and prints the XEX's SHA-256.

Then **set the signed pair aside straight away**, before anything else rebuilds:

```bash
mkdir -p Release/dist/v0.2.4-beta
```

```bash
mv Release/Omni360.xex.sig Release/dist/v0.2.4-beta/ && cp Release/Omni360.xex Release/dist/v0.2.4-beta/
```

A rebuild changes the XEX's bytes even when the source hasn't changed, and the signature would no longer match — the app would refuse the release ("Its signature doesn't match"). `Release/` is ignored by git, so none of this can be committed by accident.

### 3. Commit, tag and push

```bash
git commit -am "Version 0.2.4-beta"
```

```bash
git tag -a v0.2.4-beta -m "Omni360 0.2.4-beta"
```

```bash
git push && git push origin v0.2.4-beta
```

Don't move a tag that's been published, or replace a release's files with a different build: two different builds would then both claim the same version. Fix a bad release with a new version instead.

### 4. Publish it on GitHub

1. **Releases → Draft a new release**, and choose the tag.
2. Attach **both** files from `Release/dist/vX.Y.Z/`: `Omni360.xex` and `Omni360.xex.sig`.
3. Upload the XEX to [VirusTotal](https://www.virustotal.com) and put the link to its results in the notes, with the SHA-256 the script printed. (r/360hacks requires a scan for every new file.)
4. Leave **Set as a pre-release** unticked, and tick **Set as the latest release**, so it shows on the repository's front page. A pre-release is only offered to people already running a pre-release (a version with a `-` suffix), and never shows as the latest.
5. Publish.

### Writing the notes

The app shows a release's notes in **Settings → Updates**, with the Markdown taken out: headings, bold, links (as their text) and `-` lists come through, as plain paragraphs and bullets. So keep them to:

- a heading and a line or two of what's new;
- short paragraphs and `-` bullet points — no tables, images or code blocks;
- what someone on an older version needs to do, if anything;
- the SHA-256 and the VirusTotal link.

About sixteen lines fit on the console's screen; the rest is cut off there, though not on GitHub.

## Checking it worked

Running the previous version, **Settings → Updates** should offer the new one within a few seconds of starting, and **Update** should download it, restart, and then say *up to date*. The previous XEX is kept beside the new one as `Omni360.xex.old`.

To test before telling anyone, build the same code with `CURRENT_VERSION` set to the previous version, put that on a console, and update from it. Put `settings.h` back afterwards.

If it goes wrong, the screen says which step failed, and `DebugInfo.txt` has an `[update]` line:

| On screen | Usually means |
|---|---|
| *This release isn't signed* | `Omni360.xex.sig` isn't attached. The app still announces the release, but points people to GitHub instead of offering Update. |
| *Its signature doesn't match* | The XEX attached isn't the one that was signed — usually a rebuild. Attach the pair from `Release/dist/`. |
| *It couldn't be downloaded from GitHub* | Either file failed to download, or the XEX's size didn't match what the release lists. |
| *It couldn't replace the current version* | The rename on the console failed; the old version is untouched. |

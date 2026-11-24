#!/bin/sh
# Attaches Omni360 Helper to a release - Windows, Linux and macOS, each with
# its signature, and the helper's version - building it only when it has
# changed.
#
#     sh tools/release_helpers.sh v0.21.0-beta
#
# Running helpers update themselves from the newest release: they read
# Omni360Helper-version.txt, and install their platform's file only if its
# .sig checks out against the release key (helper/update.go). So every
# release carries all seven files.
#
# The helper changes far less often than the app. If nothing it's built from
# (helper/, this script, build_helper.sh, the macOS workflow) changed since
# the last published release, its files are copied from that release to this
# one: nothing is built or signed, and the macOS workflow isn't run.
# Otherwise:
#   - helper/version.go's version must be newer than the last release's, or
#     running helpers wouldn't see an update (the script stops if it isn't);
#   - Windows and Linux are built here (tools/build_helper.sh), signed, and
#     attached with the version;
#   - the macOS workflow builds and attaches its zip on a Mac, which this
#     waits for (a few minutes), then signs the zip as attached and attaches
#     the signature.
#
# The release must exist already - a draft is fine - and its tag must be
# pushed, as the comparison is between tags. Needs gh, Python and openssl
# for signing (tools/sign_release.py; SIGN_KEY=<path> for a key elsewhere),
# and Go when it builds.
set -e
tag="$1"
if [ -z "$tag" ]; then
    echo "usage: sh tools/release_helpers.sh <tag>, e.g. v0.21.0-beta" >&2
    exit 2
fi
cd "$(dirname "$0")/.."

builds="Omni360Helper.exe Omni360Helper-linux Omni360Helper-macos.zip"
files="$builds Omni360Helper.exe.sig Omni360Helper-linux.sig Omni360Helper-macos.zip.sig Omni360Helper-version.txt"
sources="helper tools/build_helper.sh tools/release_helpers.sh .github/workflows/helper-macos.yml"
out=Release/dist/helper

sign() {
    if [ -n "$SIGN_KEY" ]; then
        python tools/sign_release.py --key "$SIGN_KEY" "$1"
    else
        python tools/sign_release.py "$1"
    fi
}

# The newest published release that isn't this one - drafts don't count, as
# their files may not all be there yet.
prev=$(gh release list --exclude-drafts --limit 20 --json tagName --jq '.[].tagName' | grep -vx "$tag" | head -n 1)

carry=no
if [ -n "$prev" ] && git diff --quiet "$prev" "$tag" -- $sources; then
    have=$(gh release view "$prev" --json assets --jq '.assets[].name')
    carry=yes
    for f in $files; do
        echo "$have" | grep -qx "$f" || carry=no
    done
    [ "$carry" = yes ] || echo "$prev is missing a helper file - building instead"
fi

if [ "$carry" = yes ]; then
    echo "the helper hasn't changed since $prev - copying its files to $tag"
    dir=$(mktemp -d)
    for f in $files; do
        gh release download "$prev" --pattern "$f" --dir "$dir"
    done
    (cd "$dir" && gh release upload "$tag" $files --clobber)
    rm -rf "$dir"
    echo "done: all of the helper's files attached to $tag"
    exit 0
fi

version=$(sed -n 's/^const helperVersion = "\([^"]*\)".*/\1/p' helper/version.go)
if [ -z "$version" ]; then
    echo "couldn't read helperVersion from helper/version.go" >&2
    exit 1
fi
if [ -n "$prev" ]; then
    dir=$(mktemp -d)
    if gh release download "$prev" --pattern Omni360Helper-version.txt --dir "$dir" 2>/dev/null; then
        before=$(tr -d '\r\n ' < "$dir/Omni360Helper-version.txt")
        if [ "$before" = "$version" ]; then
            rm -rf "$dir"
            echo "the helper changed since $prev, but helper/version.go still says $version, as $prev's does -" >&2
            echo "bump it, or running helpers won't update to this build" >&2
            exit 1
        fi
    fi
    rm -rf "$dir"
fi

echo "the helper changed since ${prev:-the last release} - building $version"
sh tools/build_helper.sh
sign "$out/Omni360Helper.exe"
sign "$out/Omni360Helper-linux"
printf '%s\n' "$version" > "$out/Omni360Helper-version.txt"

# Started first, so the Mac builds while Windows and Linux upload.
last=$(gh run list --workflow helper-macos.yml --event workflow_dispatch --limit 1 --json databaseId --jq '.[0].databaseId')
gh workflow run helper-macos.yml -f release_tag="$tag"

(cd "$out" && gh release upload "$tag" Omni360Helper.exe Omni360Helper.exe.sig Omni360Helper-linux \
    Omni360Helper-linux.sig Omni360Helper-version.txt --clobber)
echo "Windows and Linux attached to $tag; waiting for the macOS workflow to attach its zip"

run=""
for i in $(seq 1 30); do
    run=$(gh run list --workflow helper-macos.yml --event workflow_dispatch --limit 1 --json databaseId --jq '.[0].databaseId')
    [ -n "$run" ] && [ "$run" != "$last" ] && break
    run=""
    sleep 4
done
if [ -z "$run" ]; then
    echo "the macOS workflow didn't start - run it from Actions, then sign its zip:" >&2
    echo "  gh release download $tag --pattern Omni360Helper-macos.zip --dir $out --clobber" >&2
    echo "  python tools/sign_release.py $out/Omni360Helper-macos.zip" >&2
    echo "  gh release upload $tag $out/Omni360Helper-macos.zip.sig --clobber" >&2
    exit 1
fi
gh run watch "$run" --exit-status

# Signed as attached: the bytes people and helpers download.
rm -f "$out/Omni360Helper-macos.zip"
gh release download "$tag" --pattern Omni360Helper-macos.zip --dir "$out"
sign "$out/Omni360Helper-macos.zip"
gh release upload "$tag" "$out/Omni360Helper-macos.zip.sig" --clobber
echo "done: helper $version - all of its files attached to $tag"

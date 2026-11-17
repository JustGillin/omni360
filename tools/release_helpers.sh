#!/bin/sh
# Attaches Omni360 Helper to a release - Windows, Linux and macOS - building
# it only when it has changed.
#
#     sh tools/release_helpers.sh v0.21.0-beta
#
# The helper changes far less often than the app. If nothing it's built from
# (helper/, this script, build_helper.sh, the macOS workflow) changed since
# the last published release, its three files are copied from that release
# to this one: nothing is built, and the macOS workflow isn't run. Otherwise
# Windows and Linux are built here (tools/build_helper.sh) and attached, and
# the macOS workflow is started to build and attach its zip on a Mac.
#
# The release must exist already - a draft is fine - and its tag must be
# pushed, as the comparison is between tags. Needs gh, and Go when it builds.
set -e
tag="$1"
if [ -z "$tag" ]; then
    echo "usage: sh tools/release_helpers.sh <tag>, e.g. v0.21.0-beta" >&2
    exit 2
fi
cd "$(dirname "$0")/.."

files="Omni360Helper.exe Omni360Helper-linux Omni360Helper-macos.zip"
sources="helper tools/build_helper.sh tools/release_helpers.sh .github/workflows/helper-macos.yml"

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
    gh release upload "$tag" "$dir/Omni360Helper.exe" "$dir/Omni360Helper-linux" "$dir/Omni360Helper-macos.zip" --clobber
    rm -rf "$dir"
    echo "done: all three attached to $tag"
else
    echo "the helper changed since ${prev:-the last release} - building it"
    sh tools/build_helper.sh
    gh release upload "$tag" Release/dist/helper/Omni360Helper.exe Release/dist/helper/Omni360Helper-linux --clobber
    gh workflow run helper-macos.yml -f release_tag="$tag"
    echo "Windows and Linux attached to $tag; the macOS workflow is building its zip and attaches it when done:"
    echo "  gh run list --workflow helper-macos.yml --limit 1"
fi

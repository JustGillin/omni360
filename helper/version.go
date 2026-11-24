package main

// helperVersion is this build's version: shown in the tray, sent to the
// console in /hello, and compared with the newest release's for updates
// (update.go). Bump it whenever helper/ changes for a release -
// tools/release_helpers.sh reads it from here and attaches it to the release
// as Omni360Helper-version.txt, which is how a running helper learns there's
// a newer one. The Mac app's Info.plist gets it in the macOS workflow; the
// Windows file properties need winres/winres.json bumped to match and the
// .syso remade (see tools/build_helper.sh).
const helperVersion = "1.1.0"

// protocol is the version of what the helper and the console say to each
// other. Bump it only for a change an older console or helper can't work
// with - a new request the other side must make, a reply it would misread.
// Anything both sides can ignore (a new field at the end, a new endpoint the
// console only tries) doesn't need a bump.
//
// The console compares the number with its own and, when they differ, says
// which side needs updating instead of using the helper; the tray does the
// same with the number the console sends. So these must never change shape,
// whatever the protocol becomes:
//
//	UDP announcement  "OMNI360-HELPER <protocol> <http port> <id> <name>"
//	GET /hello        "omni360-helper <protocol> <id> <helper version>"
//	                  (?p=<console protocol>&v=<console version> from consoles
//	                  that send them - 0.22.0 on; older ones speak protocol 2)
const protocol = 2

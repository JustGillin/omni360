#ifndef SPEED_TEST_H
#define SPEED_TEST_H

// Settings' Speed test: where a download's speed goes. Three measurements,
// each on its own, so they can be told apart:
//
//   1. the network, with no encryption - a plain-HTTP file from a CDN, for
//      10 seconds. The most the console can receive at all; its Ethernet is
//      100 Mbit, so about 11 MB/s at best.
//   2. decrypting on one core - BearSSL's ChaCha20-Poly1305 and AES-128-GCM,
//      the implementations a download's TLS uses, over a buffer in memory.
//      The most one connection can take in, whatever the network.
//   3. archive.org over HTTPS, 10 seconds a run, each connection on a core of
//      its own as a game install's are: one connection with the cipher the
//      server picks (AES-GCM) and with ChaCha20 - whether decrypting holds a
//      connection back - then 2 and 3 connections as downloads connect. A
//      pause before each run, and a log line for every connection: what it
//      got, its cipher, and whether it dropped. This needs the archive.org
//      keys; without them it's left out.
//
// The results are shown, with a line on what they say, and written to the
// log as [speedtest] lines.

// Runs it, drawing its own screens: progress, which B cancels, then the
// results. authHeader may be NULL - no keys - to leave out archive.org.
// scratchPath is anywhere on the drive the downloads go to: the archive.org
// test writes its data under that drive's staging folder, and deletes it.
void RunSpeedTestUI(const char *authHeader, const char *scratchPath);

#endif

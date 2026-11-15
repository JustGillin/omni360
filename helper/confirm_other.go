//go:build !windows && !darwin

package main

// confirmPairing has no dialog on Linux: the tray's "Allow" item is the
// way to answer there.
func confirmPairing(from, code string) (allow, asked bool) { return false, false }

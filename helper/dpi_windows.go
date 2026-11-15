//go:build windows

package main

import "syscall"

// beDpiAware tells Windows the helper draws at the screen's real scale.
// Without it, at 125% or 150% Windows draws the tray menu and the pairing
// dialog at 100% and stretches them, which is why they came out blurry. It has
// to run before any window is made. Per-monitor v2 is Windows 10 1703 and
// later; on older versions the call isn't there and nothing changes.
func beDpiAware() {
	user32 := syscall.NewLazyDLL("user32.dll")
	if proc := user32.NewProc("SetProcessDpiAwarenessContext"); proc.Find() == nil {
		const perMonitorV2 = ^uintptr(3) // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2, (HANDLE)-4
		if r, _, _ := proc.Call(perMonitorV2); r != 0 {
			return
		}
	}
	if proc := user32.NewProc("SetProcessDPIAware"); proc.Find() == nil {
		proc.Call() // system-aware, at least: sharp on the main screen
	}
}

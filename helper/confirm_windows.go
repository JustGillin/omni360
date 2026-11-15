//go:build windows

package main

import (
	"fmt"
	"syscall"
	"unsafe"
)

var messageBox = syscall.NewLazyDLL("user32.dll").NewProc("MessageBoxW")

// confirmPairing asks on the PC, in a dialog that comes to the front.
// asked is false if no dialog could be shown (the tray item still works).
func confirmPairing(from, code string) (allow, asked bool) {
	const (
		mbYesNo        = 0x00000004
		mbIconQuestion = 0x00000020
		mbSystemModal  = 0x00001000
		mbForeground   = 0x00010000
		mbTopmost      = 0x00040000
		idYes          = 6
	)
	text := fmt.Sprintf("An Xbox at %s wants to download through this PC.\n\n"+
		"Check that your TV shows the code %s, then choose Yes to allow it.", from, code)
	t, _ := syscall.UTF16PtrFromString(text)
	c, _ := syscall.UTF16PtrFromString("Omni360 Helper - pair with your Xbox?")
	r, _, err := messageBox.Call(0, uintptr(unsafe.Pointer(t)), uintptr(unsafe.Pointer(c)),
		mbYesNo|mbIconQuestion|mbSystemModal|mbForeground|mbTopmost)
	if r == 0 {
		_ = err
		return false, false
	}
	return r == idYes, true
}

//go:build windows

package main

import (
	"fmt"
	"runtime"
	"syscall"
	"unsafe"
)

var (
	user32         = syscall.NewLazyDLL("user32.dll")
	messageBox     = user32.NewProc("MessageBoxW")
	sendMessage    = user32.NewProc("SendMessageW")
	shell32        = syscall.NewLazyDLL("shell32.dll")
	browseFolder   = shell32.NewProc("SHBrowseForFolderW")
	pathFromIDList = shell32.NewProc("SHGetPathFromIDListW")
	ole32          = syscall.NewLazyDLL("ole32.dll")
	coInitialize   = ole32.NewProc("CoInitializeEx")
	coUninitialize = ole32.NewProc("CoUninitialize")
	coTaskMemFree  = ole32.NewProc("CoTaskMemFree")
)

const caseInsensitiveFS = true

const (
	mbOK           = 0x00000000
	mbYesNo        = 0x00000004
	mbIconError    = 0x00000010
	mbIconQuestion = 0x00000020
	mbSystemModal  = 0x00001000
	mbForeground   = 0x00010000
	mbTopmost      = 0x00040000
	idYes          = 6
)

func box(title, text string, flags uintptr) uintptr {
	t, _ := syscall.UTF16PtrFromString(text)
	c, _ := syscall.UTF16PtrFromString(title)
	r, _, _ := messageBox.Call(0, uintptr(unsafe.Pointer(t)), uintptr(unsafe.Pointer(c)),
		flags|mbSystemModal|mbForeground|mbTopmost)
	return r
}

// confirmPairing asks on the PC, in a dialog that comes to the front.
// asked is false if no dialog could be shown (the tray item still works).
func confirmPairing(from, code string) (allow, asked bool) {
	r := box("Omni360 Helper - pair with your Xbox?",
		fmt.Sprintf("An Xbox at %s wants to download through this PC.\n\n"+
			"Check that your TV shows the code %s, then choose Yes to allow it.", from, code),
		mbYesNo|mbIconQuestion)
	if r == 0 {
		return false, false
	}
	return r == idYes, true
}

// askYesNo asks a question; no dialog counts as yes.
func askYesNo(title, text string) bool {
	r := box(title, text, mbYesNo|mbIconQuestion)
	return r == 0 || r == idYes
}

// showError says something went wrong.
func showError(title, text string) { box(title, text, mbOK|mbIconError) }

// browseInfo is BROWSEINFOW.
type browseInfo struct {
	owner       uintptr
	root        uintptr
	displayName *uint16
	title       *uint16
	flags       uint32
	callback    uintptr
	lParam      uintptr
	image       int32
}

// The dialog starts in the folder lParam names.
var browseCallback = syscall.NewCallback(func(hwnd, msg, lParam, data uintptr) uintptr {
	const bffmInitialized, bffmSetSelectionW = 1, 0x467
	if msg == bffmInitialized && data != 0 {
		sendMessage.Call(hwnd, bffmSetSelectionW, 1, data)
	}
	return 0
})

// chooseFolder asks for a folder, starting at start. ok is false if the
// user cancelled.
func chooseFolder(prompt, start string) (folder string, ok bool, err error) {
	// The dialog's new style (with "Make New Folder") needs COM, on a
	// thread of its own.
	runtime.LockOSThread()
	defer runtime.UnlockOSThread()
	const coinitApartmentThreaded = 0x2
	coInitialize.Call(0, coinitApartmentThreaded)
	defer coUninitialize.Call()

	const (
		bifReturnOnlyFSDirs = 0x0001
		bifEditBox          = 0x0010
		bifNewDialogStyle   = 0x0040
	)
	name := make([]uint16, 260)
	title, _ := syscall.UTF16PtrFromString(prompt)
	startPtr, _ := syscall.UTF16PtrFromString(start)
	bi := browseInfo{
		displayName: &name[0],
		title:       title,
		flags:       bifReturnOnlyFSDirs | bifEditBox | bifNewDialogStyle,
		callback:    browseCallback,
		lParam:      uintptr(unsafe.Pointer(startPtr)),
	}
	pidl, _, _ := browseFolder.Call(uintptr(unsafe.Pointer(&bi)))
	runtime.KeepAlive(startPtr)
	if pidl == 0 {
		return "", false, nil
	}
	defer coTaskMemFree.Call(pidl)
	path := make([]uint16, 32768)
	if r, _, _ := pathFromIDList.Call(pidl, uintptr(unsafe.Pointer(&path[0]))); r == 0 {
		return "", false, fmt.Errorf("that isn't a folder on a drive")
	}
	return syscall.UTF16ToString(path), true, nil
}

//go:build darwin

package main

import (
	"fmt"
	"os/exec"
	"strings"
)

// confirmPairing asks with a native dialog, through osascript (part of every
// Mac). asked is false if it couldn't be shown; the menu bar item still works.
func confirmPairing(from, code string) (allow, asked bool) {
	script := fmt.Sprintf(`display dialog "An Xbox at %s wants to download through this Mac.\n\nCheck that your TV shows the code %s, then choose Allow." `+
		`with title "Omni360 Helper" buttons {"Don't Allow", "Allow"} default button "Allow" giving up after 110`,
		from, code)
	out, err := exec.Command("osascript", "-e", script).Output()
	if err != nil {
		// "Don't Allow" is reported as a cancel, which osascript exits on.
		if exitErr, ok := err.(*exec.ExitError); ok && exitErr.ExitCode() == 1 {
			return false, true
		}
		return false, false
	}
	result := string(out)
	if strings.Contains(result, "gave up:true") {
		return false, false // nobody answered; the request times out by itself
	}
	return strings.Contains(result, "button returned:Allow"), true
}

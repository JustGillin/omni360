//go:build darwin

package main

import (
	"fmt"
	"os/exec"
	"strings"
)

const caseInsensitiveFS = true // APFS's default

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

// appleScriptString quotes s for AppleScript.
func appleScriptString(s string) string {
	return `"` + strings.NewReplacer(`\`, `\\`, `"`, `\"`).Replace(s) + `"`
}

// askYesNo asks a question; no dialog counts as yes.
func askYesNo(title, text string) bool {
	script := fmt.Sprintf(`display dialog %s with title %s buttons {"Cancel", "OK"} default button "OK"`,
		appleScriptString(text), appleScriptString(title))
	err := exec.Command("osascript", "-e", script).Run()
	if exitErr, ok := err.(*exec.ExitError); ok && exitErr.ExitCode() == 1 {
		return false
	}
	return true
}

// showError says something went wrong.
func showError(title, text string) {
	script := fmt.Sprintf(`display dialog %s with title %s buttons {"OK"} default button "OK" with icon caution`,
		appleScriptString(text), appleScriptString(title))
	exec.Command("osascript", "-e", script).Run()
}

// chooseFolder asks for a folder, starting at start. ok is false if the
// user cancelled.
func chooseFolder(prompt, start string) (folder string, ok bool, err error) {
	script := fmt.Sprintf(`POSIX path of (choose folder with prompt %s default location (POSIX file %s))`,
		appleScriptString(prompt), appleScriptString(start))
	out, err := exec.Command("osascript", "-e", script).Output()
	if err != nil {
		if exitErr, ok := err.(*exec.ExitError); ok && exitErr.ExitCode() == 1 {
			return "", false, nil // cancelled
		}
		return "", false, err
	}
	return strings.TrimRight(strings.TrimSpace(string(out)), "/"), true, nil
}

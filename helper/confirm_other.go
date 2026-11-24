//go:build !windows && !darwin

package main

import (
	"errors"
	"os/exec"
	"strings"
)

const caseInsensitiveFS = false

// confirmPairing has no dialog on Linux: the tray's "Allow" item is the
// way to answer there.
func confirmPairing(from, code string) (allow, asked bool) { return false, false }

// askYesNo asks with zenity if it's there; otherwise the answer is yes.
func askYesNo(title, text string) bool {
	if _, err := exec.LookPath("zenity"); err != nil {
		return true
	}
	return exec.Command("zenity", "--question", "--title", title, "--text", text).Run() == nil
}

// showError says something went wrong, with zenity if it's there. The log
// has it either way.
func showError(title, text string) {
	if _, err := exec.LookPath("zenity"); err == nil {
		exec.Command("zenity", "--error", "--title", title, "--text", text).Run()
	}
}

// chooseFolder asks with zenity or kdialog, whichever the desktop has.
func chooseFolder(prompt, start string) (folder string, ok bool, err error) {
	var cmd *exec.Cmd
	if _, e := exec.LookPath("zenity"); e == nil {
		cmd = exec.Command("zenity", "--file-selection", "--directory", "--title", prompt, "--filename", start+"/")
	} else if _, e := exec.LookPath("kdialog"); e == nil {
		cmd = exec.Command("kdialog", "--getexistingdirectory", start, "--title", prompt)
	} else {
		return "", false, errors.New("no folder picker here (zenity or kdialog) - start the helper with -cache <folder> instead")
	}
	out, err := cmd.Output()
	if err != nil {
		if _, isExit := err.(*exec.ExitError); isExit {
			return "", false, nil // cancelled
		}
		return "", false, err
	}
	return strings.TrimSpace(string(out)), true, nil
}

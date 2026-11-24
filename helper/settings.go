package main

import (
	"encoding/hex"
	"encoding/json"
	"log"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"time"
)

// settings are the choices made in the tray, kept in settings.json in the
// data folder (beside pairing.json and the log).
type settings struct {
	mu   sync.Mutex
	path string

	// Downloads is the folder downloads are kept in; empty for the data
	// folder, where they always were before it could be changed.
	Downloads string `json:"downloads,omitempty"`
	// CleanUp is a downloads folder that was swapped for another: the next
	// start empties it of the helper's files, once nothing has them open.
	CleanUp string `json:"clean_up,omitempty"`
	// ManualUpdates turns off installing updates by itself while idle.
	ManualUpdates bool `json:"manual_updates,omitempty"`
}

func loadSettings(dataDir string) *settings {
	s := &settings{path: filepath.Join(dataDir, "settings.json")}
	if b, err := os.ReadFile(s.path); err == nil {
		if err := json.Unmarshal(b, s); err != nil {
			log.Printf("couldn't read %s: %v", s.path, err)
		}
	}
	return s
}

// save writes the settings; call with s.mu held.
func (s *settings) save() error {
	b, _ := json.MarshalIndent(s, "", "  ")
	return os.WriteFile(s.path, b, 0o644)
}

// downloadsSubfolder is what the helper makes inside the folder the user
// picks, so its files never sit loose among theirs: picking D:\ uses
// D:\Omni360Helper.
const downloadsSubfolder = "Omni360Helper"

// downloadsFolderFor is where downloads go for a folder the user picked.
func downloadsFolderFor(picked string) string {
	if strings.EqualFold(filepath.Base(picked), downloadsSubfolder) {
		return picked
	}
	return filepath.Join(picked, downloadsSubfolder)
}

// isDownloadFile is whether name is one of the helper's own files in a
// downloads folder: a download (16 hex digits, see cachePath) or its
// .progress. Nothing else there is ever removed - the folder may be one the
// user keeps other things in.
func isDownloadFile(name string) bool {
	name = strings.TrimSuffix(name, ".progress")
	if len(name) != 16 {
		return false
	}
	_, err := hex.DecodeString(name)
	return err == nil
}

// emptyDownloads removes the helper's files from an old downloads folder,
// and the folder too if that leaves it empty. Tries for a minute: the old
// helper process may still have them open while it exits.
func emptyDownloads(dir string, done func()) {
	for i := 0; i < 60; i++ {
		entries, err := os.ReadDir(dir)
		if os.IsNotExist(err) {
			break
		}
		left := 0
		for _, e := range entries {
			if e.IsDir() || !isDownloadFile(e.Name()) {
				continue
			}
			if err := os.Remove(filepath.Join(dir, e.Name())); err != nil && !os.IsNotExist(err) {
				left++
			}
		}
		if left == 0 {
			if filepath.Base(dir) == downloadsSubfolder {
				os.Remove(dir) // only if empty
			}
			log.Printf("emptied the old downloads folder %s", dir)
			break
		}
		time.Sleep(time.Second)
	}
	done()
}

// sameFolder compares two folder paths as the file system would: without
// case on Windows and macOS.
func sameFolder(a, b string) bool {
	a, b = filepath.Clean(a), filepath.Clean(b)
	if a == b {
		return true
	}
	return caseInsensitiveFS && strings.EqualFold(a, b)
}

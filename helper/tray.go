package main

import (
	"fmt"
	"omni360helper/icon"
	"os/exec"
	"runtime"
	"time"

	"fyne.io/systray"
)

// The tray icon is the helper's whole interface: allowing a console to pair,
// what it's doing, and a way to quit. A release build has no console window
// (-ldflags -H=windowsgui), so this is where the code is read from.
func runTray(s *store, p *pairing, cacheDir, logPath string) {
	systray.Run(func() {
		systray.SetIcon(stillIcon())
		systray.SetTitle("Omni360 Helper")
		systray.SetTooltip("Omni360 Helper")

		title := systray.AddMenuItem("Omni360 Helper", "")
		title.Disable()
		allow := systray.AddMenuItem("", "Your Xbox is asking to download through this PC")
		allow.Hide()
		status := systray.AddMenuItem("Waiting for your console", "")
		status.Disable()
		systray.AddSeparator()
		openCache := systray.AddMenuItem("Open downloads folder", "")
		openLog := systray.AddMenuItem("Open log", "")
		systray.AddSeparator()
		quit := systray.AddMenuItem("Quit", "")

		go func() {
			for {
				if req := p.waiting(); req != nil {
					allow.SetTitle(fmt.Sprintf("Allow the Xbox at %s (code %s)", req.from, req.code))
					allow.Show()
				} else {
					allow.Hide()
				}
				line := s.summary()
				status.SetTitle(line)
				systray.SetTooltip("Omni360 Helper - " + line)
				time.Sleep(2 * time.Second)
			}
		}()
		go spinWhileBusy(s)
		go func() {
			for {
				select {
				case <-allow.ClickedCh:
					if req := p.waiting(); req != nil {
						p.decide(req, true)
					}
				case <-openCache.ClickedCh:
					openPath(cacheDir)
				case <-openLog.ClickedCh:
					openPath(logPath)
				case <-quit.ClickedCh:
					systray.Quit()
					return
				}
			}
		}()
	}, func() {})
}

func openPath(path string) {
	switch runtime.GOOS {
	case "windows":
		exec.Command("explorer", path).Start()
	case "darwin":
		exec.Command("open", path).Start()
	default:
		exec.Command("xdg-open", path).Start()
	}
}

// summary is one line for the tray: what's downloading, or that it's idle.
func (s *store) summary() string {
	s.mu.Lock()
	all := make([]*download, 0, len(s.all))
	for _, d := range s.all {
		all = append(all, d)
	}
	s.mu.Unlock()

	for _, d := range all {
		if d.busy() {
			size, have := d.progress()
			if size > 0 {
				return fmt.Sprintf("Downloading %s - %d%%", shorten(d.file, 40), have*100/size)
			}
		}
	}
	if len(all) > 0 {
		return "Ready - everything asked for is downloaded"
	}
	return "Waiting for your console"
}

func shorten(s string, n int) string {
	r := []rune(s)
	if len(r) <= n {
		return s
	}
	return string(r[:n-1]) + "…"
}

// The icon's frames: the disc turned 15 degrees at a time. The two wedges are
// half a turn apart, so twelve frames make a full cycle.
const spinFrames = 12

var frames [spinFrames][]byte

func stillIcon() []byte {
	if frames[0] == nil {
		frames[0] = trayICO(0)
	}
	return frames[0]
}

func trayICO(turn float64) []byte {
	if runtime.GOOS == "windows" {
		return icon.ICO(turn)
	}
	return icon.PNG(64, turn)
}

// spinWhileBusy turns the disc while anything is downloading, and puts it back
// still when nothing is.
func spinWhileBusy(s *store) {
	frame, spinning := 0, false
	for {
		busy := s.anyBusy()
		switch {
		case busy:
			frame = (frame + 1) % spinFrames
			if frames[frame] == nil {
				frames[frame] = trayICO(float64(frame) * 180 / spinFrames)
			}
			systray.SetIcon(frames[frame])
			spinning = true
			time.Sleep(90 * time.Millisecond)
		case spinning:
			frame, spinning = 0, false
			systray.SetIcon(stillIcon())
			time.Sleep(500 * time.Millisecond)
		default:
			time.Sleep(500 * time.Millisecond)
		}
	}
}

// anyBusy is whether any download is fetching or waiting to.
func (s *store) anyBusy() bool {
	s.mu.Lock()
	all := make([]*download, 0, len(s.all))
	for _, d := range s.all {
		all = append(all, d)
	}
	s.mu.Unlock()
	for _, d := range all {
		if d.busy() {
			return true
		}
	}
	return false
}

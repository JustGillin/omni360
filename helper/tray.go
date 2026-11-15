package main

import (
	"fmt"
	"omni360helper/icon"
	"os/exec"
	"runtime"
	"sync"
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

	speed := s.speed()
	for _, d := range all {
		if d.busy() {
			size, have := d.progress()
			if size > 0 {
				line := fmt.Sprintf("Downloading %s - %d%%", shorten(d.file, 40), have*100/size)
				if speed > 0 {
					line += fmt.Sprintf(" \u00b7 %.1f MB/s", speed)
				}
				return line
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

// The speed from archive.org, over the last few seconds: the tray asks every
// two seconds, and each answer is the bytes since the last over the time
// since. Smoothed a little, so it doesn't jump with each 8 MB chunk's bursts.
var (
	speedMu    sync.Mutex
	speedAt    time.Time
	speedBytes int64
	speedMBps  float64
)

func (s *store) speed() float64 {
	speedMu.Lock()
	defer speedMu.Unlock()
	now := time.Now()
	total := s.received.Load()
	if !speedAt.IsZero() {
		if secs := now.Sub(speedAt).Seconds(); secs > 0.5 {
			current := float64(total-speedBytes) / secs / (1 << 20)
			if speedMBps == 0 {
				speedMBps = current
			} else {
				speedMBps = speedMBps*0.6 + current*0.4
			}
			if speedMBps < 0.05 {
				speedMBps = 0
			}
		}
	}
	speedAt, speedBytes = now, total
	return speedMBps
}

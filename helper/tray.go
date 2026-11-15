package main

import (
	"bytes"
	"encoding/binary"
	"fmt"
	"image"
	"image/color"
	"image/png"
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
		systray.SetIcon(trayIcon())
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

// trayIcon draws the icon: a green disc with a white ring, as an ICO on
// Windows (a PNG inside an ICO header) and a plain PNG elsewhere.
func trayIcon() []byte {
	const size = 32
	img := image.NewRGBA(image.Rect(0, 0, size, size))
	green := color.RGBA{0x10, 0x7C, 0x10, 0xFF}
	white := color.RGBA{0xFF, 0xFF, 0xFF, 0xFF}
	c := float64(size-1) / 2
	for y := 0; y < size; y++ {
		for x := 0; x < size; x++ {
			dx, dy := float64(x)-c, float64(y)-c
			d := dx*dx + dy*dy
			switch {
			case d <= 15.5*15.5 && d >= 11*11 && d <= 13.5*13.5:
				img.Set(x, y, white)
			case d <= 15.5*15.5:
				img.Set(x, y, green)
			}
		}
	}
	var pngBytes bytes.Buffer
	png.Encode(&pngBytes, img)
	if runtime.GOOS != "windows" {
		return pngBytes.Bytes()
	}

	var ico bytes.Buffer
	binary.Write(&ico, binary.LittleEndian, []uint16{0, 1, 1}) // reserved, type icon, one image
	ico.Write([]byte{size, size, 0, 0})                        // width, height, colours, reserved
	binary.Write(&ico, binary.LittleEndian, []uint16{1, 32})   // planes, bits per pixel
	binary.Write(&ico, binary.LittleEndian, []uint32{uint32(pngBytes.Len()), 22})
	ico.Write(pngBytes.Bytes())
	return ico.Bytes()
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

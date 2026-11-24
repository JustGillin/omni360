package main

import (
	"fmt"
	"log"
	"omni360helper/icon"
	"os"
	"os/exec"
	"runtime"
	"sync"
	"sync/atomic"
	"time"

	"fyne.io/systray"
)

// The tray icon is the helper's whole interface: allowing a console to pair,
// what it's doing, where downloads go, updates, and a way to quit. A release
// build has no console window (-ldflags -H=windowsgui), so this is where the
// code is read from.
func runTray(a *app) {
	s, p := a.store, a.pair
	systray.Run(func() {
		systray.SetIcon(stillIcon())
		systray.SetTitle("Omni360 Helper")
		systray.SetTooltip("Omni360 Helper")

		title := systray.AddMenuItem("Omni360 Helper "+helperVersion, "")
		title.Disable()
		xbox := systray.AddMenuItem("No Xbox connected", "Whether a paired Xbox has checked in")
		xbox.Disable()
		allow := systray.AddMenuItem("", "Your Xbox is asking to download through this PC")
		allow.Hide()
		status := systray.AddMenuItem("Waiting for your console", "")
		status.Disable()
		update := systray.AddMenuItem("", "")
		update.Hide()
		systray.AddSeparator()
		openCache := systray.AddMenuItem("Open downloads folder", a.downloadsDir)
		changeCache := systray.AddMenuItem("Change downloads folder…", "Where this PC keeps games while your Xbox copies them")
		if a.pinned {
			changeCache.SetTooltip("Set by -cache when the helper was started")
			changeCache.Disable()
		}
		openLog := systray.AddMenuItem("Open log", "")
		a.settings.mu.Lock()
		autoOn := !a.settings.ManualUpdates
		a.settings.mu.Unlock()
		autoUpdate := systray.AddMenuItemCheckbox("Install updates automatically", "When nothing has downloaded for a while", autoOn)
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
				from, connected := p.xbox()
				xboxProtocol, _ := p.xboxProtocol()
				switch {
				case !connected:
					xbox.SetTitle("No Xbox connected")
				case xboxProtocol < protocol:
					xbox.SetTitle("Your Xbox needs a newer Omni360 - update it in Settings")
				case xboxProtocol > protocol:
					xbox.SetTitle("Your Xbox needs a newer helper - update this one")
				default:
					xbox.SetTitle("Connected to your Xbox (" + from + ")")
				}
				line := s.summary(connected)
				status.SetTitle(line)
				systray.SetTooltip("Omni360 Helper - " + line)

				available, busy, failed := a.updates.state()
				switch {
				case available == "":
					update.Hide()
				case busy:
					update.SetTitle("Updating to " + available + "…")
					update.SetTooltip("")
					update.Disable()
					update.Show()
				case failed != "":
					update.SetTitle("Update to " + available + " failed - try again")
					update.SetTooltip(failed)
					update.Enable()
					update.Show()
				default:
					update.SetTitle("Update to " + available)
					update.SetTooltip("Downloads it, then restarts the helper")
					update.Enable()
					update.Show()
				}
				time.Sleep(2 * time.Second)
			}
		}()
		go spinWhileBusy(s, p)
		go func() {
			for {
				select {
				case <-allow.ClickedCh:
					if req := p.waiting(); req != nil {
						p.decide(req, true)
					}
				case <-update.ClickedCh:
					go a.updates.install(a)
				case <-openCache.ClickedCh:
					openPath(a.downloadsDir)
				case <-changeCache.ClickedCh:
					go a.changeDownloads()
				case <-openLog.ClickedCh:
					openPath(a.logPath)
				case <-autoUpdate.ClickedCh:
					a.settings.mu.Lock()
					a.settings.ManualUpdates = !a.settings.ManualUpdates
					on := !a.settings.ManualUpdates
					a.settings.save()
					a.settings.mu.Unlock()
					if on {
						autoUpdate.Check()
					} else {
						autoUpdate.Uncheck()
					}
				case <-quit.ClickedCh:
					systray.Quit()
					return
				}
			}
		}()
	}, func() {})
}

var choosingFolder atomic.Bool

// changeDownloads asks for a new downloads folder and restarts the helper
// to use it. The old folder's downloads are removed by the new start - they
// can't be while this one has them open, and moving many GB between drives
// isn't worth it for a cache: the console has its own copy of what it's read.
func (a *app) changeDownloads() {
	if !choosingFolder.CompareAndSwap(false, true) {
		return // a dialog is open already
	}
	defer choosingFolder.Store(false)
	const title = "Omni360 Helper"

	picked, ok, err := chooseFolder("Choose where Omni360 Helper keeps downloads. It makes an "+
		downloadsSubfolder+" folder inside the one you pick.", a.downloadsDir)
	if err != nil {
		showError(title, "Couldn't ask for a folder: "+err.Error())
		return
	}
	if !ok {
		return
	}
	target := downloadsFolderFor(picked)
	if sameFolder(target, a.downloadsDir) {
		return
	}
	if err := checkWritable(target); err != nil {
		showError(title, "Omni360 Helper can't save downloads in "+target+":\n\n"+err.Error())
		return
	}
	if a.store.anyBusy() && !askYesNo(title, "A download is running. The helper restarts to change the folder, "+
		"and your Xbox carries on from archive.org for those few seconds.\n\nChange it now?") {
		return
	}

	a.settings.mu.Lock()
	if sameFolder(target, a.dataDir) {
		a.settings.Downloads = ""
	} else {
		a.settings.Downloads = target
	}
	a.settings.CleanUp = a.downloadsDir
	err = a.settings.save()
	a.settings.mu.Unlock()
	if err != nil {
		showError(title, "Couldn't save the setting: "+err.Error())
		return
	}
	log.Printf("downloads move to %s - restarting", target)
	if err := a.restartSelf(); err != nil {
		showError(title, "Couldn't restart the helper - quit it and start it again to use the new folder.\n\n"+err.Error())
	}
}

// checkWritable makes dir if it isn't there and checks a file can be
// written in it.
func checkWritable(dir string) error {
	if err := os.MkdirAll(dir, 0o755); err != nil {
		return err
	}
	f, err := os.CreateTemp(dir, ".write-test-")
	if err != nil {
		return err
	}
	name := f.Name()
	f.Close()
	return os.Remove(name)
}

// restartSelf starts this program again, with the same arguments, and quits
// this one.
func (a *app) restartSelf() error {
	exe, err := os.Executable()
	if err != nil {
		return err
	}
	if runtime.GOOS == "darwin" {
		if bundle := appBundle(exe); bundle != "" {
			return a.restart(exec.Command("open", append([]string{"-n", bundle, "--args"}, os.Args[1:]...)...))
		}
	}
	return a.restart(exec.Command(exe, os.Args[1:]...))
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
func (s *store) summary(connected bool) string {
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
	if !connected {
		return "Open Omni360 on your Xbox to use this PC"
	}
	return "Ready for downloads"
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
var dimIcon []byte

// stillIcon is the disc at rest: green with an Xbox connected, grey without.
func stillIcon() []byte { return stillFor(false) }

func stillFor(connected bool) []byte {
	if !connected {
		if dimIcon == nil {
			dimIcon = trayICO(0, true)
		}
		return dimIcon
	}
	if frames[0] == nil {
		frames[0] = trayICO(0, false)
	}
	return frames[0]
}

func trayICO(turn float64, dim bool) []byte {
	if runtime.GOOS == "windows" {
		return icon.ICOStyle(turn, dim)
	}
	return icon.PNGStyle(64, turn, dim)
}

// spinWhileBusy turns the disc while anything is downloading, and otherwise
// keeps it still - green with a paired Xbox connected, grey without.
func spinWhileBusy(s *store, p *pairing) {
	frame, spinning := 0, false
	shown := -1 // the still icon on show: 0 grey, 1 green, -1 neither yet
	for {
		_, connected := p.xbox()
		if s.anyBusy() {
			frame = (frame + 1) % spinFrames
			if frames[frame] == nil {
				frames[frame] = trayICO(float64(frame)*180/spinFrames, false)
			}
			systray.SetIcon(frames[frame])
			spinning, shown = true, -1
			time.Sleep(90 * time.Millisecond)
			continue
		}
		want := 0
		if connected {
			want = 1
		}
		if spinning || shown != want {
			frame, spinning, shown = 0, false, want
			systray.SetIcon(stillFor(connected))
		}
		time.Sleep(500 * time.Millisecond)
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

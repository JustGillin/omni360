// Omni360 Helper: downloads Store games on a PC for the console.
//
// The console stays in charge. It asks the helper to fetch an archive.org
// file (sending the user's archive.org key header with the request), and
// then reads that file from the helper in byte ranges over plain HTTP. These
// are the same ranges it would otherwise ask archive.org for. The helper
// fetches with many connections into a cache file and answers each range as
// soon as those bytes are on disk, so the console's copy runs alongside the
// PC's download instead of after it. Unzipping, checking and installing all
// stay on the console.
//
// Finding the helper: every few seconds it broadcasts
// "OMNI360-HELPER 2 <http port> <id> <name>" on UDP port 36060, so Omni360
// can find it without anyone typing an address. Everything but /hello and
// /pair needs a paired console's token (?t=) - see pairing.go. The 2 is the
// protocol's version; version.go has what the console does with it.
//
// HTTP, on port 36061:
//
//	GET  /hello?p=P&v=V                -> "omni360-helper 2 <id> <version>"
//	GET  /pair/request                  -> "<request id> <code>"; asks on the PC
//	GET  /pair/wait?r=ID                -> "<token> <id>" once allowed
//	POST /fetch?item=I&file=F           start (or keep) a download; the
//	                                    request's Authorization header is
//	                                    passed to archive.org, held in memory
//	GET  /file/<item>/<file>           the file; Range supported; waits for
//	                                    bytes not downloaded yet
//	GET  /status?item=I&file=F          "<size> <bytes done>"
//	POST /forget?item=I&file=F          drop the cached copy
//	GET  /speed?bytes=N                 N bytes from memory, for the
//	                                    console's speed test
package main

import (
	"flag"
	"fmt"
	"log"
	"net"
	"net/http"
	"os"
	"os/exec"
	"path/filepath"
	"time"

	"fyne.io/systray"
)

const announcePort = 36060

var httpPort = 36061

// app is the running helper: what the tray and the updater work with.
type app struct {
	store        *store
	pair         *pairing
	settings     *settings
	updates      *updater
	srv          *http.Server
	dataDir      string // pairing.json, settings.json, the log
	downloadsDir string // the downloads themselves
	logPath      string
	pinned       bool // -cache was given: the tray can't change the folder
}

func main() {
	beDpiAware() // before the tray makes any window
	cacheDir := flag.String("cache", "", "where downloads are kept (default: the folder chosen in the tray, or the data folder)")
	dataDir := flag.String("data", defaultDataDir(), "where pairing, settings and the log are kept")
	conns := flag.Int("connections", 8, "connections to Internet Archive, shared by every download")
	cacheGB := flag.Int64("cache-gb", 50, "how much the downloads folder may hold, in GB")
	flag.IntVar(&httpPort, "port", httpPort, "HTTP port the console reads from")
	showVersion := flag.Bool("version", false, "print the version and quit")
	flag.Parse()
	if *showVersion {
		fmt.Printf("Omni360 Helper %s (protocol %d)\n", helperVersion, protocol)
		return
	}

	if err := os.MkdirAll(*dataDir, 0o755); err != nil {
		log.Fatalf("can't make the data folder %s: %v", *dataDir, err)
	}
	// The log goes to a file in the data folder as well, for "Open log" when
	// there's no console window.
	logPath := filepath.Join(*dataDir, "helper.log")
	if f, err := os.OpenFile(logPath, os.O_CREATE|os.O_WRONLY|os.O_TRUNC, 0o644); err == nil {
		log.SetOutput(fileFirst{f})
	}

	a := &app{dataDir: *dataDir, logPath: logPath, settings: loadSettings(*dataDir), updates: newUpdater()}
	a.downloadsDir = *dataDir
	switch {
	case *cacheDir != "":
		a.downloadsDir, a.pinned = *cacheDir, true
	case a.settings.Downloads != "":
		a.downloadsDir = a.settings.Downloads
	}
	if err := os.MkdirAll(a.downloadsDir, 0o755); err != nil {
		// A drive that's gone - unplugged, or a letter that moved. Better to
		// keep working from the data folder than not start; the tray shows
		// where downloads are.
		log.Printf("can't use the downloads folder %s (%v) - using %s for now", a.downloadsDir, err, *dataDir)
		a.downloadsDir = *dataDir
	}
	if clean := a.settings.CleanUp; clean != "" && !sameFolder(clean, a.downloadsDir) {
		go emptyDownloads(clean, func() {
			a.settings.mu.Lock()
			if a.settings.CleanUp == clean {
				a.settings.CleanUp = ""
				a.settings.save()
			}
			a.settings.mu.Unlock()
		})
	}
	go removeOldProgram()

	a.store = newStore(a.downloadsDir, *conns, *cacheGB<<30)
	a.pair = loadPairing(*dataDir)

	name, _ := os.Hostname()
	go announce(name, a.pair.ID)

	mux := http.NewServeMux()
	registerHandlers(mux, a.store, a.pair)
	a.srv = &http.Server{
		Handler:           logRequests(mux),
		ReadHeaderTimeout: 30 * time.Second,
	}

	log.Printf("Omni360 Helper %s on port %d, downloads in %s", helperVersion, httpPort, a.downloadsDir)
	ln, err := listen(fmt.Sprintf(":%d", httpPort))
	if err != nil {
		log.Fatal(err)
	}
	go func() {
		if err := a.srv.Serve(ln); err != http.ErrServerClosed {
			log.Fatal(err)
		}
	}()
	go a.updates.checkLoop(a)
	runTray(a)
}

// listen opens the HTTP port, waiting a little for it: after an update or a
// change of folder, the helper that started this one may not have let go of
// it yet.
func listen(addr string) (net.Listener, error) {
	var err error
	for i := 0; i < 30; i++ {
		var ln net.Listener
		if ln, err = net.Listen("tcp", addr); err == nil {
			return ln, nil
		}
		time.Sleep(500 * time.Millisecond)
	}
	return nil, fmt.Errorf("can't listen on %s - is another Omni360 Helper running? %w", addr, err)
}

// restart starts cmd - this program again, or its update - and quits this
// one once it has.
func (a *app) restart(cmd *exec.Cmd) error {
	if err := cmd.Start(); err != nil {
		return err
	}
	log.Printf("restarting")
	a.srv.Close() // the port is the new one's
	systray.Quit()
	return nil
}

// fileFirst writes the log to the file, and to the console too when there is
// one. io.MultiWriter stopped at the first error, and a release build has no
// console, so nothing reached the file at all.
type fileFirst struct{ f *os.File }

func (w fileFirst) Write(p []byte) (int, error) {
	n, err := w.f.Write(p)
	os.Stderr.Write(p) // no console: fails, and that's fine
	return n, err
}

// defaultDataDir is where the helper has always kept its files, and where
// downloads go until another folder is chosen.
func defaultDataDir() string {
	if dir, err := os.UserCacheDir(); err == nil {
		return filepath.Join(dir, "Omni360Helper")
	}
	return "Omni360Helper"
}

// announce broadcasts the helper's port on the LAN every 3 seconds.
//
// 255.255.255.255 alone isn't enough: Windows sends it out of one network
// adapter only, and a PC with more than one - Wi-Fi and Ethernet, Hyper-V's
// or WSL's virtual switch, a VPN - can send it everywhere but where the
// Xbox is. So it also goes to each adapter's own subnet broadcast
// (192.168.68.255 and so on), from that adapter's address, which always
// leaves through that adapter. Re-read each round, so an adapter that comes
// up later - Wi-Fi joining after start - is covered.
func announce(name, id string) {
	msg := []byte(fmt.Sprintf("OMNI360-HELPER %d %d %s %s", protocol, httpPort, id, name))
	send := func(from net.IP, to net.IP) {
		var local *net.UDPAddr
		if from != nil {
			local = &net.UDPAddr{IP: from}
		}
		if conn, err := net.DialUDP("udp4", local, &net.UDPAddr{IP: to, Port: announcePort}); err == nil {
			conn.Write(msg)
			conn.Close()
		}
	}
	for {
		send(nil, net.IPv4bcast)
		for _, b := range subnetBroadcasts() {
			send(b.from, b.to)
		}
		time.Sleep(3 * time.Second)
	}
}

type broadcastTarget struct{ from, to net.IP }

// subnetBroadcasts lists each up IPv4 adapter's address and its subnet's
// broadcast address, leaving out loopback and link-local (169.254.x.x) ones,
// which no Xbox is reachable on.
func subnetBroadcasts() []broadcastTarget {
	var targets []broadcastTarget
	ifaces, err := net.Interfaces()
	if err != nil {
		return nil
	}
	for _, iface := range ifaces {
		if iface.Flags&net.FlagUp == 0 || iface.Flags&net.FlagLoopback != 0 || iface.Flags&net.FlagBroadcast == 0 {
			continue
		}
		addrs, err := iface.Addrs()
		if err != nil {
			continue
		}
		for _, a := range addrs {
			ipnet, ok := a.(*net.IPNet)
			if !ok {
				continue
			}
			ip := ipnet.IP.To4()
			if ip == nil || ip.IsLinkLocalUnicast() || len(ipnet.Mask) != net.IPv4len {
				continue
			}
			bcast := make(net.IP, net.IPv4len)
			for i := range ip {
				bcast[i] = ip[i] | ^ipnet.Mask[i]
			}
			targets = append(targets, broadcastTarget{from: ip, to: bcast})
		}
	}
	return targets
}

func logRequests(h http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		log.Printf("%s %s %s %s", r.RemoteAddr, r.Method, r.URL.Path, r.Header.Get("Range"))
		h.ServeHTTP(w, r)
	})
}

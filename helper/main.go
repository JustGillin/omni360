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
// /pair needs a paired console's token (?t=) - see pairing.go.
//
// HTTP, on port 36061:
//
//	GET  /hello                        -> "omni360-helper 2 <id>"
//	GET  /pair/request                  -> "<request id> <code>"; asks on the PC
//	GET  /pair/wait?r=ID                -> "<token> <id>" once allowed
//	POST /fetch?item=I&file=F           start (or keep) a download; the
//	                                    request's Authorization header is
//	                                    passed to archive.org, held in memory
//	GET  /file/<item>/<file>           the file; Range supported; waits for
//	                                    bytes not downloaded yet
//	GET  /status?item=I&file=F          "<size> <bytes done>"
//	POST /forget?item=I&file=F          drop the cached copy
package main

import (
	"flag"
	"fmt"
	"log"
	"net"
	"net/http"
	"os"
	"path/filepath"
	"time"
)

const announcePort = 36060

var httpPort = 36061

func main() {
	beDpiAware() // before the tray makes any window
	cacheDir := flag.String("cache", defaultCacheDir(), "where downloads are kept")
	conns := flag.Int("connections", 8, "connections to archive.org, shared by every download")
	cacheGB := flag.Int64("cache-gb", 50, "how much the downloads folder may hold, in GB")
	flag.IntVar(&httpPort, "port", httpPort, "HTTP port the console reads from")
	flag.Parse()

	if err := os.MkdirAll(*cacheDir, 0o755); err != nil {
		log.Fatalf("can't make the cache folder %s: %v", *cacheDir, err)
	}
	store := newStore(*cacheDir, *conns, *cacheGB<<30)
	pair := loadPairing(*cacheDir)

	name, _ := os.Hostname()
	go announce(name, pair.ID)

	mux := http.NewServeMux()
	registerHandlers(mux, store, pair)
	srv := &http.Server{
		Addr:              fmt.Sprintf(":%d", httpPort),
		Handler:           logRequests(mux),
		ReadHeaderTimeout: 30 * time.Second,
	}
	// The log goes to a file beside the cache as well, for "Open log" when
	// there's no console window.
	logPath := filepath.Join(*cacheDir, "helper.log")
	if f, err := os.OpenFile(logPath, os.O_CREATE|os.O_WRONLY|os.O_TRUNC, 0o644); err == nil {
		log.SetOutput(fileFirst{f})
	}

	log.Printf("Omni360 Helper on port %d, cache in %s", httpPort, *cacheDir)
	go func() { log.Fatal(srv.ListenAndServe()) }()
	runTray(store, pair, *cacheDir, logPath)
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

func defaultCacheDir() string {
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
	msg := []byte(fmt.Sprintf("OMNI360-HELPER 2 %d %s %s", httpPort, id, name))
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

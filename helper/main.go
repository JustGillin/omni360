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
// "OMNI360-HELPER 1 <http port> <name>" on UDP port 36060, so Omni360 can
// list it without anyone typing an address.
//
// HTTP, on port 36061:
//   GET  /hello                        -> "omni360-helper 1"
//   POST /fetch?item=I&file=F           start (or keep) a download; the
//                                       request's Authorization header is
//                                       passed to archive.org, held in memory
//   GET  /file/<item>/<file>           the file; Range supported; waits for
//                                       bytes not downloaded yet
//   GET  /status?item=I&file=F          "<size> <bytes done>"
//   POST /forget?item=I&file=F          drop the cached copy
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

const (
	announcePort = 36060
	httpPort     = 36061
)

func main() {
	cacheDir := flag.String("cache", defaultCacheDir(), "where downloads are kept")
	conns := flag.Int("connections", 16, "connections per download")
	flag.Parse()

	if err := os.MkdirAll(*cacheDir, 0o755); err != nil {
		log.Fatalf("can't make the cache folder %s: %v", *cacheDir, err)
	}
	store := newStore(*cacheDir, *conns)

	name, _ := os.Hostname()
	go announce(name)

	mux := http.NewServeMux()
	registerHandlers(mux, store)
	srv := &http.Server{
		Addr:              fmt.Sprintf(":%d", httpPort),
		Handler:           logRequests(mux),
		ReadHeaderTimeout: 30 * time.Second,
	}
	log.Printf("Omni360 Helper on port %d, cache in %s", httpPort, *cacheDir)
	log.Fatal(srv.ListenAndServe())
}

func defaultCacheDir() string {
	if dir, err := os.UserCacheDir(); err == nil {
		return filepath.Join(dir, "Omni360Helper")
	}
	return "Omni360Helper"
}

// announce broadcasts the helper's port on the LAN every 3 seconds.
func announce(name string) {
	addr := &net.UDPAddr{IP: net.IPv4bcast, Port: announcePort}
	msg := []byte(fmt.Sprintf("OMNI360-HELPER 1 %d %s", httpPort, name))
	for {
		if conn, err := net.DialUDP("udp4", nil, addr); err == nil {
			conn.Write(msg)
			conn.Close()
		}
		time.Sleep(3 * time.Second)
	}
}

func logRequests(h http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		log.Printf("%s %s %s %s", r.RemoteAddr, r.Method, r.URL.Path, r.Header.Get("Range"))
		h.ServeHTTP(w, r)
	})
}

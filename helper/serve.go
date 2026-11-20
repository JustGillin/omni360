package main

import (
	"crypto/rand"
	"fmt"
	"io"
	"log"
	"net/http"
	"os"
	"strconv"
	"strings"
)

func registerHandlers(mux *http.ServeMux, s *store, p *pairing) {
	mux.HandleFunc("/pair/request", p.handleRequest)
	mux.HandleFunc("/pair/wait", p.handleWait)

	mux.HandleFunc("/hello", func(w http.ResponseWriter, r *http.Request) {
		p.allowed(r) // a paired console's check-in: the tray says it's connected
		fmt.Fprintf(w, "omni360-helper 2 %s", p.ID)
	})

	mux.HandleFunc("/fetch", p.requireToken(func(w http.ResponseWriter, r *http.Request) {
		item, file := r.URL.Query().Get("item"), r.URL.Query().Get("file")
		if item == "" || file == "" {
			http.Error(w, "item and file are needed", http.StatusBadRequest)
			return
		}
		d, err := s.get(item, file, r.Header.Get("Authorization"))
		if err != nil {
			http.Error(w, err.Error(), http.StatusBadGateway)
			return
		}
		d.wantAll()
		size, have := d.progress()
		fmt.Fprintf(w, "%d %d", size, have)
	}))

	mux.HandleFunc("/status", p.requireToken(func(w http.ResponseWriter, r *http.Request) {
		s.mu.Lock()
		d := s.all[key(r.URL.Query().Get("item"), r.URL.Query().Get("file"))]
		s.mu.Unlock()
		if d == nil {
			http.NotFound(w, r)
			return
		}
		size, have := d.progress()
		fmt.Fprintf(w, "%d %d", size, have)
	}))

	mux.HandleFunc("/forget", p.requireToken(func(w http.ResponseWriter, r *http.Request) {
		item, file := r.URL.Query().Get("item"), r.URL.Query().Get("file")
		s.mu.Lock()
		d := s.all[key(item, file)]
		delete(s.all, key(item, file))
		s.mu.Unlock()
		if d != nil {
			d.stop() // its workers finish the chunk they're on, then stop
		}
		s.removeSoon(s.cachePath(item, file))
		log.Printf("dropped %s/%s - the console cancelled it", item, file)
	}))

	// /speed?bytes=N: N bytes from memory, for the console's speed test - how
	// fast the helper can hand a download over the local network, with
	// archive.org and the disk left out of it.
	mux.HandleFunc("/speed", p.requireToken(func(w http.ResponseWriter, r *http.Request) {
		n, err := strconv.ParseInt(r.URL.Query().Get("bytes"), 10, 64)
		if err != nil || n <= 0 || n > 4<<30 {
			n = 1 << 30
		}
		w.Header().Set("Content-Type", "application/octet-stream")
		w.Header().Set("Content-Length", strconv.FormatInt(n, 10))
		for n > 0 {
			k := int64(len(speedData))
			if k > n {
				k = n
			}
			if _, err := w.Write(speedData[:k]); err != nil {
				return // the console stopped reading: its test is over
			}
			n -= k
		}
	}))

	// /file/<item>/<file>: what the console reads in place of archive.org.
	mux.HandleFunc("/file/", p.requireToken(func(w http.ResponseWriter, r *http.Request) {
		rest := strings.TrimPrefix(r.URL.Path, "/file/")
		slash := strings.IndexByte(rest, '/')
		if slash <= 0 {
			http.NotFound(w, r)
			return
		}
		d, err := s.get(rest[:slash], rest[slash+1:], r.Header.Get("Authorization"))
		if err != nil {
			http.Error(w, err.Error(), http.StatusBadGateway)
			return
		}
		serveRange(w, r, s, d)
	}))
}

// speedData is what /speed sends, over and over: random, so nothing on the
// way can shrink it.
var speedData = func() []byte {
	b := make([]byte, 1<<20)
	rand.Read(b)
	return b
}()

// smallRead is the most a read can be to skip the chunk queue.
const smallRead = 1 << 20

func serveRange(w http.ResponseWriter, r *http.Request, s *store, d *download) {
	d.noteServed()
	from, to := int64(0), d.size // [from, to)
	status := http.StatusOK
	if h := r.Header.Get("Range"); h != "" {
		var a, b int64
		n, _ := fmt.Sscanf(h, "bytes=%d-%d", &a, &b)
		switch {
		case n == 2:
			from, to = a, b+1
		case n == 1:
			from = a
		default:
			http.Error(w, "bad range", http.StatusRequestedRangeNotSatisfiable)
			return
		}
		if to > d.size {
			to = d.size
		}
		if from >= to {
			w.Header().Set("Content-Range", fmt.Sprintf("bytes */%d", d.size))
			w.WriteHeader(http.StatusRequestedRangeNotSatisfiable)
			return
		}
		status = http.StatusPartialContent
		w.Header().Set("Content-Range", fmt.Sprintf("bytes %d-%d/%d", from, to-1, d.size))
	}
	w.Header().Set("Content-Length", fmt.Sprint(to-from))
	w.Header().Set("Accept-Ranges", "bytes")

	// A small read - a zip's header or directory, a RAR's headers - that
	// isn't here yet goes straight to archive.org for just those bytes. In
	// the chunk queue it waited behind whole 8 MB chunks, which on a slow
	// line took longer than the console waits.
	if status == http.StatusPartialContent && to-from <= smallRead && !d.have(from, to) {
		if body, err := s.fetchRange(d, from, to); err == nil {
			w.WriteHeader(status)
			w.Write(body)
			return
		} else {
			log.Printf("small read of %s straight from archive.org failed: %v - waiting for its chunk", d.file, err)
		}
	}

	f, err := os.Open(d.path)
	if err != nil {
		http.Error(w, err.Error(), http.StatusInternalServerError)
		return
	}
	defer f.Close()
	w.WriteHeader(status)

	// Send it a chunk at a time, waiting for each to arrive from archive.org.
	for at := from; at < to; {
		end := (at/chunkSize + 1) * chunkSize
		if end > to {
			end = to
		}
		if err := d.waitFor(at, end); err != nil {
			return // the console sees a short body and retries
		}
		if _, err := io.Copy(w, io.NewSectionReader(f, at, end-at)); err != nil {
			return
		}
		at = end
	}
}

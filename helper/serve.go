package main

import (
	"fmt"
	"io"
	"net/http"
	"os"
	"strings"
)

func registerHandlers(mux *http.ServeMux, s *store, p *pairing) {
	mux.HandleFunc("/pair/request", p.handleRequest)
	mux.HandleFunc("/pair/wait", p.handleWait)

	mux.HandleFunc("/hello", func(w http.ResponseWriter, r *http.Request) {
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
		delete(s.all, key(item, file))
		s.mu.Unlock()
		p := s.cachePath(item, file)
		os.Remove(p)
		os.Remove(p + ".progress")
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
		serveRange(w, r, d)
	}))
}

func serveRange(w http.ResponseWriter, r *http.Request, d *download) {
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

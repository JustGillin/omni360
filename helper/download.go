package main

import (
	"crypto/sha1"
	"encoding/hex"
	"errors"
	"fmt"
	"io"
	"log"
	"net/http"
	"net/url"
	"os"
	"path/filepath"
	"sort"
	"sync"
	"time"
)

// chunkSize is the unit the PC downloads in. It's smaller than the console's
// 32 MB pieces so the first bytes the console wants show up fast.
const chunkSize = 8 << 20

// download is one archive.org file being fetched into the cache.
type download struct {
	item, file string
	path       string
	size       int64
	auth       string

	directMu sync.Mutex
	direct   string // the datanode archive.org redirected to, used from then on

	mu      sync.Mutex
	cond    *sync.Cond
	done    []bool // per chunk
	pending []int  // chunks not started, front of the file first
	failed  error
	started bool
}

type store struct {
	dir      string
	conns    int
	maxBytes int64 // the cache's limit; the oldest downloads go to make room
	mu       sync.Mutex
	all      map[string]*download
	http     *http.Client
}

func newStore(dir string, conns int, maxBytes int64) *store {
	return &store{dir: dir, conns: conns, maxBytes: maxBytes, all: map[string]*download{},
		http: &http.Client{Timeout: 0}}
}

func key(item, file string) string { return item + "\x00" + file }

func (s *store) cachePath(item, file string) string {
	h := sha1.Sum([]byte(key(item, file)))
	return filepath.Join(s.dir, hex.EncodeToString(h[:8]))
}

func archiveURL(item, file string) string {
	return "https://archive.org/download/" + url.PathEscape(item) + "/" + url.PathEscape(file)
}

// get returns the download for item/file, starting it if it isn't running.
// auth may be empty for public items.
func (s *store) get(item, file, auth string) (*download, error) {
	s.mu.Lock()
	d := s.all[key(item, file)]
	isNew := d == nil
	if isNew {
		d = &download{item: item, file: file, path: s.cachePath(item, file)}
		d.cond = sync.NewCond(&d.mu)
		s.all[key(item, file)] = d
	}
	s.mu.Unlock()
	if isNew {
		s.makeRoom(d.path)
	}

	d.mu.Lock()
	defer d.mu.Unlock()
	if auth != "" {
		d.auth = auth
	}
	if d.started && d.failed == nil {
		return d, nil
	}
	d.failed = nil
	if err := s.start(d); err != nil {
		d.failed = err
		return nil, err
	}
	return d, nil
}

// start finds the size, picks up what an earlier run left, and launches the
// connections. Called with d.mu held.
func (s *store) start(d *download) error {
	size, err := s.remoteSize(d)
	if err != nil {
		return err
	}
	d.size = size
	n := int((size + chunkSize - 1) / chunkSize)
	d.done = loadProgress(d.path, n)
	d.pending = d.pending[:0]
	for i := 0; i < n; i++ {
		if !d.done[i] {
			d.pending = append(d.pending, i)
		}
	}
	f, err := os.OpenFile(d.path, os.O_RDWR|os.O_CREATE, 0o644)
	if err != nil {
		return err
	}
	if err := f.Truncate(size); err != nil {
		f.Close()
		return err
	}
	d.started = true
	log.Printf("fetching %s/%s: %d MB, %d of %d chunks already here",
		d.item, d.file, size>>20, n-len(d.pending), n)
	for i := 0; i < s.conns; i++ {
		go s.worker(d, f)
	}
	return nil
}

func (s *store) request(d *download, method string, rangeHeader string) (*http.Response, error) {
	d.directMu.Lock()
	target := d.direct
	d.directMu.Unlock()
	if target == "" {
		target = archiveURL(d.item, d.file)
	}
	req, err := http.NewRequest(method, target, nil)
	if err != nil {
		return nil, err
	}
	if d.auth != "" {
		req.Header.Set("Authorization", d.auth)
	}
	if rangeHeader != "" {
		req.Header.Set("Range", rangeHeader)
	}
	req.Header.Set("User-Agent", "Omni360Helper/1")
	resp, err := s.http.Do(req)
	if err == nil && resp.Request.URL.String() != target {
		d.directMu.Lock()
		d.direct = resp.Request.URL.String()
		d.directMu.Unlock()
		log.Printf("%s comes from %s", d.file, resp.Request.URL.Host)
	}
	return resp, err
}

func (s *store) remoteSize(d *download) (int64, error) {
	resp, err := s.request(d, "GET", "bytes=0-0")
	if err != nil {
		return 0, err
	}
	resp.Body.Close()
	if resp.StatusCode != http.StatusPartialContent {
		return 0, fmt.Errorf("archive.org answered %s", resp.Status)
	}
	var a, b, total int64
	if _, err := fmt.Sscanf(resp.Header.Get("Content-Range"), "bytes %d-%d/%d", &a, &b, &total); err != nil {
		return 0, errors.New("no size from archive.org")
	}
	return total, nil
}

func (s *store) worker(d *download, f *os.File) {
	for {
		d.mu.Lock()
		if len(d.pending) == 0 || d.failed != nil {
			d.mu.Unlock()
			return
		}
		c := d.pending[0]
		d.pending = d.pending[1:]
		d.mu.Unlock()

		var err error
		for attempt := 0; attempt < 8; attempt++ {
			if err = s.fetchChunk(d, f, c); err == nil {
				break
			}
			log.Printf("chunk %d of %s: %v (try %d)", c, d.file, err, attempt+1)
			time.Sleep(time.Duration(attempt+1) * 2 * time.Second)
		}

		d.mu.Lock()
		if err != nil {
			d.failed = err
		} else {
			d.done[c] = true
			saveProgress(d.path, d.done)
		}
		d.cond.Broadcast()
		d.mu.Unlock()
	}
}

func (s *store) fetchChunk(d *download, f *os.File, c int) error {
	from := int64(c) * chunkSize
	to := from + chunkSize
	if to > d.size {
		to = d.size
	}
	resp, err := s.request(d, "GET", fmt.Sprintf("bytes=%d-%d", from, to-1))
	if err != nil {
		return err
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusPartialContent {
		return fmt.Errorf("archive.org answered %s", resp.Status)
	}
	_, err = io.Copy(io.NewOffsetWriter(f, from), io.LimitReader(resp.Body, to-from))
	return err
}

// waitFor blocks until bytes [from, to) are on disk or the download fails.
func (d *download) waitFor(from, to int64) error {
	d.mu.Lock()
	defer d.mu.Unlock()
	for {
		if d.failed != nil {
			return d.failed
		}
		ready := true
		for c := from / chunkSize; c*chunkSize < to; c++ {
			if !d.done[c] {
				ready = false
				d.bump(int(c))
				break
			}
		}
		if ready {
			return nil
		}
		d.cond.Wait()
	}
}

// bump moves chunk c to the front of the queue: the console is waiting on it.
func (d *download) bump(c int) {
	for i, p := range d.pending {
		if p == c {
			copy(d.pending[1:i+1], d.pending[:i])
			d.pending[0] = c
			return
		}
	}
}

func (d *download) progress() (int64, int64) {
	d.mu.Lock()
	defer d.mu.Unlock()
	var have int64
	for c, ok := range d.done {
		if ok {
			n := int64(chunkSize)
			if int64(c+1)*chunkSize > d.size {
				n = d.size - int64(c)*chunkSize
			}
			have += n
		}
	}
	return d.size, have
}

// Progress is kept next to the cache file, one byte per chunk.
func loadProgress(path string, n int) []bool {
	done := make([]bool, n)
	if b, err := os.ReadFile(path + ".progress"); err == nil && len(b) == n {
		for i := range b {
			done[i] = b[i] == 1
		}
	}
	return done
}

func saveProgress(path string, done []bool) {
	b := make([]byte, len(done))
	for i, ok := range done {
		if ok {
			b[i] = 1
		}
	}
	os.WriteFile(path+".progress", b, 0o644)
}

// makeRoom removes the least recently used downloads until the cache is under
// its limit, leaving keep and anything still downloading alone. A new
// download's own size isn't known yet, so the limit is a little soft.
func (s *store) makeRoom(keep string) {
	entries, err := os.ReadDir(s.dir)
	if err != nil {
		return
	}
	type cached struct {
		path string
		size int64
		used time.Time
	}
	var files []cached
	var total int64
	for _, e := range entries {
		name := e.Name()
		if e.IsDir() || filepath.Ext(name) != "" { // downloads have no extension; .progress, .json, .log do
			continue
		}
		info, err := e.Info()
		if err != nil {
			continue
		}
		files = append(files, cached{filepath.Join(s.dir, name), info.Size(), info.ModTime()})
		total += info.Size()
	}
	if total <= s.maxBytes {
		return
	}
	sort.Slice(files, func(i, j int) bool { return files[i].used.Before(files[j].used) })

	busy := map[string]bool{keep: true}
	s.mu.Lock()
	for _, d := range s.all {
		if size, have := d.progress(); size == 0 || have < size {
			busy[d.path] = true
		}
	}
	s.mu.Unlock()

	for _, f := range files {
		if total <= s.maxBytes {
			break
		}
		if busy[f.path] {
			continue
		}
		os.Remove(f.path)
		os.Remove(f.path + ".progress")
		total -= f.size
		s.mu.Lock()
		for k, d := range s.all {
			if d.path == f.path {
				delete(s.all, k)
			}
		}
		s.mu.Unlock()
		log.Printf("made room: removed an old download (%d MB)", f.size>>20)
	}
}

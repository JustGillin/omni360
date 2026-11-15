package main

import (
	"crypto/rand"
	"crypto/subtle"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"log"
	"math/big"
	"net/http"
	"os"
	"path/filepath"
	"sync"
	"time"
)

// pairing keeps strangers on the LAN out. The console sends the user's
// archive.org keys with each request, so only a console the user paired may
// use the helper.
//
// Each run shows a 4-digit code. The console sends it to /pair once and gets a
// token back, which it puts on every request after (?t=). Tokens are kept in
// pairing.json beside the cache, so a console stays paired across restarts.
// The helper's id is kept there too: the console uses only the helper it
// paired with, not any helper it hears.
type pairing struct {
	mu     sync.Mutex
	path   string
	ID     string            `json:"id"`
	Tokens map[string]string `json:"tokens"` // token -> when and from where it was made
	code   string
	misses int
}

func loadPairing(dir string) *pairing {
	p := &pairing{path: filepath.Join(dir, "pairing.json"), Tokens: map[string]string{}}
	if b, err := os.ReadFile(p.path); err == nil {
		json.Unmarshal(b, p)
	}
	if p.Tokens == nil {
		p.Tokens = map[string]string{}
	}
	if p.ID == "" {
		p.ID = randomHex(4)
		p.save()
	}
	p.newCode()
	return p
}

func (p *pairing) save() {
	b, _ := json.MarshalIndent(p, "", "  ")
	if err := os.WriteFile(p.path, b, 0o600); err != nil {
		log.Printf("couldn't save %s: %v", p.path, err)
	}
}

func (p *pairing) newCode() {
	n, _ := rand.Int(rand.Reader, big.NewInt(10000))
	p.code = fmt.Sprintf("%04d", n.Int64())
}

func (p *pairing) showCode() {
	log.Printf("To pair a console: Omni360 > Settings > General > PC helper > Pair, and enter %s", p.code)
}

func randomHex(n int) string {
	b := make([]byte, n)
	rand.Read(b)
	return hex.EncodeToString(b)
}

// handlePair answers /pair?code=NNNN with "<token> <id>".
func (p *pairing) handlePair(w http.ResponseWriter, r *http.Request) {
	p.mu.Lock()
	defer p.mu.Unlock()
	if subtle.ConstantTimeCompare([]byte(r.URL.Query().Get("code")), []byte(p.code)) != 1 {
		// Guessing 10,000 codes takes a while at a second a try, and three
		// misses make a new code anyway.
		p.misses++
		if p.misses >= 3 {
			p.misses = 0
			p.newCode()
			log.Printf("three wrong codes - here's a new one")
			p.showCode()
		}
		time.Sleep(time.Second)
		http.Error(w, "wrong code", http.StatusForbidden)
		return
	}
	token := randomHex(16)
	p.Tokens[token] = time.Now().Format(time.RFC3339) + " " + r.RemoteAddr
	p.save()
	p.misses = 0
	p.newCode() // one use each
	log.Printf("paired with %s", r.RemoteAddr)
	p.showCode()
	fmt.Fprintf(w, "%s %s", token, p.ID)
}

func (p *pairing) allowed(r *http.Request) bool {
	t := r.URL.Query().Get("t")
	if t == "" {
		return false
	}
	p.mu.Lock()
	defer p.mu.Unlock()
	_, ok := p.Tokens[t]
	return ok
}

// requireToken turns away requests without a paired console's token.
func (p *pairing) requireToken(h http.HandlerFunc) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		if !p.allowed(r) {
			http.Error(w, "not paired", http.StatusUnauthorized)
			return
		}
		h(w, r)
	}
}

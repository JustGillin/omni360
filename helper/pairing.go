package main

import (
	"crypto/rand"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"log"
	"math/big"
	"net"
	"net/http"
	"os"
	"path/filepath"
	"strconv"
	"sync"
	"time"
)

// pairing keeps strangers on the LAN out. The console sends the user's
// archive.org keys with each request, so only a console the user approved
// may use the helper.
//
// Pairing is a request the user allows on the PC. The console asks
// (/pair/request) and gets a 4-digit code to show; the PC asks "Allow this
// Xbox?" with the same code; the console waits (/pair/wait) and, once
// allowed, gets a token it puts on every request after (?t=). Matching codes
// on both screens are what show the user it's their console asking. Tokens
// are kept in pairing.json beside the cache, so a console stays paired across
// restarts. The helper's id is kept there too: the console uses only the
// helper it paired with, not any helper it hears.
type pairing struct {
	mu      sync.Mutex
	path    string
	ID      string            `json:"id"`
	Tokens  map[string]string `json:"tokens"` // token -> when and from where it was made
	pending *pairRequest

	// The last request from a paired console - a download, or its check-in
	// every 10 seconds - for the tray to say whether one is connected.
	seenAt   time.Time
	seenFrom string
	// What it said it speaks on its last /hello: the protocol, and its
	// Omni360 version ("" from one too old to say - those speak 2).
	seenProtocol int
	seenVersion  string
}

// xbox is the paired console heard in the last 30 seconds, if there is one.
func (p *pairing) xbox() (string, bool) {
	p.mu.Lock()
	defer p.mu.Unlock()
	return p.seenFrom, !p.seenAt.IsZero() && time.Since(p.seenAt) < 30*time.Second
}

// xboxProtocol is the protocol and Omni360 version the paired console last
// said it has.
func (p *pairing) xboxProtocol() (int, string) {
	p.mu.Lock()
	defer p.mu.Unlock()
	return p.seenProtocol, p.seenVersion
}

type pairRequest struct {
	id       string
	code     string
	from     string // the console's address
	decided  chan struct{}
	approved bool
	token    string
	done     bool
	expired  bool // nobody answered on the PC before it gave up
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
	return p
}

func (p *pairing) save() {
	b, _ := json.MarshalIndent(p, "", "  ")
	if err := os.WriteFile(p.path, b, 0o600); err != nil {
		log.Printf("couldn't save %s: %v", p.path, err)
	}
}

func randomHex(n int) string {
	b := make([]byte, n)
	rand.Read(b)
	return hex.EncodeToString(b)
}

func randomCode() string {
	n, _ := rand.Int(rand.Reader, big.NewInt(10000))
	return fmt.Sprintf("%04d", n.Int64())
}

// waiting is the request the tray offers to allow, if there is one.
func (p *pairing) waiting() *pairRequest {
	p.mu.Lock()
	defer p.mu.Unlock()
	if p.pending != nil && !p.pending.done {
		return p.pending
	}
	return nil
}

// decide allows or refuses req. Only the first answer counts.
func (p *pairing) decide(req *pairRequest, allow bool) {
	p.mu.Lock()
	defer p.mu.Unlock()
	if req.done {
		return
	}
	req.done = true
	req.approved = allow
	if allow {
		req.token = randomHex(16)
		p.Tokens[req.token] = time.Now().Format(time.RFC3339) + " " + req.from
		p.save()
		log.Printf("paired with the Xbox at %s", req.from)
	} else {
		log.Printf("refused pairing with %s", req.from)
	}
	close(req.decided)
}

// handleRequest answers /pair/request with "<request id> <code>" and asks the
// user on the PC.
func (p *pairing) handleRequest(w http.ResponseWriter, r *http.Request) {
	host, _, _ := net.SplitHostPort(r.RemoteAddr)
	req := &pairRequest{id: randomHex(8), code: randomCode(), from: host, decided: make(chan struct{})}

	p.mu.Lock()
	if old := p.pending; old != nil && !old.done {
		old.done = true // a newer request replaces it
		close(old.decided)
	}
	p.pending = req
	p.mu.Unlock()

	log.Printf("the Xbox at %s wants to pair - code %s", host, req.code)
	go func() {
		// Nobody answering is a no, in time; the tray item stays until then.
		if allow, asked := confirmPairing(host, req.code); asked {
			p.decide(req, allow)
		}
	}()
	go func() {
		time.Sleep(2 * time.Minute)
		p.mu.Lock()
		if !req.done {
			req.expired = true
		}
		p.mu.Unlock()
		p.decide(req, false)
	}()
	fmt.Fprintf(w, "%s %s", req.id, req.code)
}

// handleWait answers /pair/wait?r=ID: "<token> <helper id>" once allowed, 403
// if refused, 410 if nobody answered on the PC before the request expired,
// 408 if nobody has answered in 20 seconds (the console asks again).
func (p *pairing) handleWait(w http.ResponseWriter, r *http.Request) {
	p.mu.Lock()
	req := p.pending
	p.mu.Unlock()
	if req == nil || req.id != r.URL.Query().Get("r") {
		http.Error(w, "no such request", http.StatusNotFound)
		return
	}
	select {
	case <-req.decided:
	case <-time.After(20 * time.Second):
		http.Error(w, "still waiting", http.StatusRequestTimeout)
		return
	}
	if req.expired {
		http.Error(w, "nobody answered", http.StatusGone)
		return
	}
	if !req.approved || req.token == "" {
		http.Error(w, "refused", http.StatusForbidden)
		return
	}
	fmt.Fprintf(w, "%s %s", req.token, p.ID)
}

func (p *pairing) allowed(r *http.Request) bool {
	t := r.URL.Query().Get("t")
	if t == "" {
		return false
	}
	p.mu.Lock()
	defer p.mu.Unlock()
	_, ok := p.Tokens[t]
	if ok {
		p.seenAt = time.Now()
		p.seenFrom, _, _ = net.SplitHostPort(r.RemoteAddr)
		// Only /hello says; other requests leave what it said alone.
		if r.URL.Path == "/hello" {
			p.seenProtocol, p.seenVersion = 2, ""
			if n, err := strconv.Atoi(r.URL.Query().Get("p")); err == nil {
				p.seenProtocol = n
				p.seenVersion = r.URL.Query().Get("v")
			}
		}
	}
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

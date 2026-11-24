package main

import (
	"crypto/ecdsa"
	"crypto/elliptic"
	"crypto/rand"
	"crypto/sha256"
	"os"
	"path/filepath"
	"testing"
)

func TestNewerVersion(t *testing.T) {
	cases := []struct {
		a, b string
		want bool
	}{
		{"1.1.0", "1.0.0", true},
		{"1.0.0", "1.1.0", false},
		{"1.1.0", "1.1.0", false},
		{"1.10.0", "1.9.0", true},
		{"2.0", "1.9.9", true},
		{"1.1.1", "1.1", true},
		{"", "1.1.0", false},
	}
	for _, c := range cases {
		if got := newerVersion(c.a, c.b); got != c.want {
			t.Errorf("newerVersion(%q, %q) = %v, want %v", c.a, c.b, got, c.want)
		}
	}
}

func TestIsDownloadFile(t *testing.T) {
	for name, want := range map[string]bool{
		"0123456789abcdef":          true,
		"0123456789abcdef.progress": true,
		"pairing.json":              false,
		"helper.log":                false,
		"README":                    false,
		"0123456789abcdeg":          false,
		"0123456789abcdef0":         false,
	} {
		if got := isDownloadFile(name); got != want {
			t.Errorf("isDownloadFile(%q) = %v, want %v", name, got, want)
		}
	}
}

func TestDownloadsFolderFor(t *testing.T) {
	d := filepath.Join("D:", "Games")
	if got := downloadsFolderFor(d); got != filepath.Join(d, downloadsSubfolder) {
		t.Errorf("got %s", got)
	}
	if got := downloadsFolderFor(filepath.Join(d, downloadsSubfolder)); got != filepath.Join(d, downloadsSubfolder) {
		t.Errorf("a folder already called %s got another inside: %s", downloadsSubfolder, got)
	}
}

func TestVerifySignature(t *testing.T) {
	key, err := ecdsa.GenerateKey(elliptic.P256(), rand.Reader)
	if err != nil {
		t.Fatal(err)
	}
	saved := updatePublicKey
	defer func() { updatePublicKey = saved }()
	updatePublicKey[0] = 4
	key.PublicKey.X.FillBytes(updatePublicKey[1:33])
	key.PublicKey.Y.FillBytes(updatePublicKey[33:65])

	path := filepath.Join(t.TempDir(), "update")
	os.WriteFile(path, []byte("a new helper"), 0o644)
	sum := sha256.Sum256([]byte("a new helper"))
	sig, err := ecdsa.SignASN1(rand.Reader, key, sum[:])
	if err != nil {
		t.Fatal(err)
	}
	if err := verifySignature(path, sig); err != nil {
		t.Errorf("a good signature failed: %v", err)
	}
	os.WriteFile(path, []byte("a changed helper"), 0o644)
	if verifySignature(path, sig) == nil {
		t.Error("a changed file passed")
	}
	updatePublicKey = saved
	os.WriteFile(path, []byte("a new helper"), 0o644)
	if verifySignature(path, sig) == nil {
		t.Error("another key's signature passed")
	}
}

func TestEmptyDownloadsKeepsOtherFiles(t *testing.T) {
	dir := filepath.Join(t.TempDir(), downloadsSubfolder)
	os.MkdirAll(dir, 0o755)
	for _, n := range []string{"0123456789abcdef", "0123456789abcdef.progress", "notes.txt", "README"} {
		os.WriteFile(filepath.Join(dir, n), []byte("x"), 0o644)
	}
	emptyDownloads(dir, func() {})
	left, _ := os.ReadDir(dir)
	if len(left) != 2 {
		t.Errorf("want notes.txt and README left, got %d files", len(left))
	}
}

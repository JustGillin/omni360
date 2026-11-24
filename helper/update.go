package main

import (
	"crypto/ecdsa"
	"crypto/elliptic"
	"crypto/sha256"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"log"
	"math/big"
	"net/http"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"strconv"
	"strings"
	"sync"
	"time"
)

// The helper updates itself from the newest GitHub release - the same one
// the console updates from. The release carries the helper's version
// (Omni360Helper-version.txt, from version.go), each platform's build, and a
// .sig beside each made with the project's release key (tools/sign_release.py).
// An update is installed only if its signature checks out against the public
// key compiled in here (updatekey.go), so someone who got into the GitHub
// account still couldn't push a program onto anyone's PC.
//
// Installing swaps the program's file for the new one and starts it again:
// Windows can't overwrite a running .exe but can rename it, so the old one is
// moved aside to .old and removed by the new one's start. On a Mac the whole
// app bundle is swapped. A download in progress carries on after the restart
// from what's on disk; the console fetches from archive.org for the few
// seconds the helper is away.
const (
	latestReleaseURL = "https://api.github.com/repos/JustGillin/omni360/releases/latest"
	versionAsset     = "Omni360Helper-version.txt"
	checkEvery       = 12 * time.Hour
	// Installing by itself waits for this long with no downloads.
	idleBeforeUpdate = 10 * time.Minute
)

// updateAsset is this platform's download on the release page; empty where
// there's no build to update to.
func updateAsset() string {
	switch {
	case runtime.GOOS == "windows" && runtime.GOARCH == "amd64":
		return "Omni360Helper.exe"
	case runtime.GOOS == "linux" && runtime.GOARCH == "amd64":
		return "Omni360Helper-linux"
	case runtime.GOOS == "darwin":
		return "Omni360Helper-macos.zip" // one app for Apple Silicon and Intel
	}
	return ""
}

type updater struct {
	mu        sync.Mutex
	available string            // a newer version on the release page, or ""
	urls      map[string]string // its assets: name -> download URL
	busy      bool              // installing now
	failed    string            // why the last install failed, for the tray
	http      *http.Client
}

func newUpdater() *updater {
	return &updater{http: &http.Client{Timeout: 10 * time.Minute}}
}

// state is what the tray shows.
func (u *updater) state() (available string, busy bool, failed string) {
	u.mu.Lock()
	defer u.mu.Unlock()
	return u.available, u.busy, u.failed
}

func (u *updater) get(url string, accept string) (*http.Response, error) {
	req, err := http.NewRequest("GET", url, nil)
	if err != nil {
		return nil, err
	}
	req.Header.Set("User-Agent", "Omni360Helper/"+helperVersion)
	if accept != "" {
		req.Header.Set("Accept", accept)
	}
	resp, err := u.http.Do(req)
	if err != nil {
		return nil, err
	}
	if resp.StatusCode != http.StatusOK {
		resp.Body.Close()
		return nil, fmt.Errorf("%s answered %s", req.URL.Host, resp.Status)
	}
	return resp, nil
}

// check looks at the newest release for a newer helper.
func (u *updater) check() error {
	asset := updateAsset()
	if asset == "" {
		return nil
	}
	resp, err := u.get(latestReleaseURL, "application/vnd.github+json")
	if err != nil {
		return err
	}
	var release struct {
		Tag    string `json:"tag_name"`
		Assets []struct {
			Name string `json:"name"`
			URL  string `json:"browser_download_url"`
		} `json:"assets"`
	}
	err = json.NewDecoder(resp.Body).Decode(&release)
	resp.Body.Close()
	if err != nil {
		return err
	}
	urls := map[string]string{}
	for _, a := range release.Assets {
		urls[a.Name] = a.URL
	}
	// A release from before the helper had versions, or one still having
	// its files attached: nothing to go on.
	if urls[versionAsset] == "" || urls[asset] == "" || urls[asset+".sig"] == "" {
		return nil
	}
	resp, err = u.get(urls[versionAsset], "")
	if err != nil {
		return err
	}
	b, err := io.ReadAll(io.LimitReader(resp.Body, 64))
	resp.Body.Close()
	if err != nil {
		return err
	}
	latest := strings.TrimSpace(string(b))

	u.mu.Lock()
	defer u.mu.Unlock()
	if newerVersion(latest, helperVersion) {
		if u.available != latest {
			log.Printf("Omni360 Helper %s is out (%s) - this is %s", latest, release.Tag, helperVersion)
		}
		u.available, u.urls = latest, urls
	} else {
		u.available, u.urls = "", nil
	}
	return nil
}

// newerVersion is whether a is newer than b, both "MAJOR.MINOR.PATCH".
func newerVersion(a, b string) bool {
	pa, pb := strings.Split(a, "."), strings.Split(b, ".")
	for i := 0; i < len(pa) || i < len(pb); i++ {
		var x, y int
		if i < len(pa) {
			x, _ = strconv.Atoi(pa[i])
		}
		if i < len(pb) {
			y, _ = strconv.Atoi(pb[i])
		}
		if x != y {
			return x > y
		}
	}
	return false
}

// checkLoop checks soon after start and twice a day, and installs by itself
// once the helper has been idle a while, unless that's turned off.
func (u *updater) checkLoop(a *app) {
	time.Sleep(20 * time.Second)
	var lastCheck time.Time
	idleSince := time.Now()
	for {
		if time.Since(lastCheck) >= checkEvery {
			lastCheck = time.Now()
			if err := u.check(); err != nil {
				log.Printf("couldn't check for a helper update: %v", err)
			}
		}
		if a.store.anyBusy() {
			idleSince = time.Now()
		}
		available, busy, failed := u.state()
		a.settings.mu.Lock()
		auto := !a.settings.ManualUpdates
		a.settings.mu.Unlock()
		// A failed install isn't retried by itself: the tray says why, and
		// the user can try again from there.
		if auto && available != "" && !busy && failed == "" && time.Since(idleSince) >= idleBeforeUpdate {
			log.Printf("installing %s by itself - nothing has downloaded for %v", available, idleBeforeUpdate)
			u.install(a)
		}
		time.Sleep(time.Minute)
	}
}

// install downloads the update, checks its signature, puts it in place and
// restarts into it. It returns only if something went wrong, which the tray
// then shows.
func (u *updater) install(a *app) {
	u.mu.Lock()
	if u.busy || u.available == "" {
		u.mu.Unlock()
		return
	}
	u.busy, u.failed = true, ""
	version, urls := u.available, u.urls
	u.mu.Unlock()

	err := u.installFrom(a, version, urls)
	u.mu.Lock()
	u.busy = false
	if err != nil {
		u.failed = err.Error()
		log.Printf("couldn't update to %s: %v", version, err)
	}
	u.mu.Unlock()
}

func (u *updater) installFrom(a *app, version string, urls map[string]string) error {
	asset := updateAsset()
	exe, err := os.Executable()
	if err != nil {
		return err
	}
	if exe, err = filepath.EvalSymlinks(exe); err != nil {
		return err
	}
	log.Printf("updating to %s from %s", version, urls[asset])

	// The new file goes beside the old, so putting it in place is a rename.
	newPath := exe + ".new"
	if runtime.GOOS == "darwin" {
		newPath = filepath.Join(os.TempDir(), "Omni360Helper-update.zip")
	}
	defer os.Remove(newPath)
	if err := u.download(urls[asset], newPath); err != nil {
		return fmt.Errorf("couldn't download it: %w", err)
	}
	resp, err := u.get(urls[asset+".sig"], "")
	if err != nil {
		return fmt.Errorf("couldn't download its signature: %w", err)
	}
	sig, err := io.ReadAll(io.LimitReader(resp.Body, 1024))
	resp.Body.Close()
	if err != nil {
		return fmt.Errorf("couldn't download its signature: %w", err)
	}
	if err := verifySignature(newPath, sig); err != nil {
		return err
	}
	log.Printf("the update's signature checks out")

	if runtime.GOOS == "darwin" {
		return replaceApp(a, exe, newPath)
	}
	return replaceProgram(a, exe, newPath)
}

func (u *updater) download(url, path string) error {
	resp, err := u.get(url, "")
	if err != nil {
		return err
	}
	defer resp.Body.Close()
	f, err := os.OpenFile(path, os.O_CREATE|os.O_WRONLY|os.O_TRUNC, 0o755)
	if err != nil {
		return err
	}
	if _, err := io.Copy(f, resp.Body); err != nil {
		f.Close()
		return err
	}
	return f.Close()
}

// verifySignature checks sig - ECDSA P-256, DER - over the file's SHA-256,
// against the release key.
func verifySignature(path string, sig []byte) error {
	f, err := os.Open(path)
	if err != nil {
		return err
	}
	h := sha256.New()
	_, err = io.Copy(h, f)
	f.Close()
	if err != nil {
		return err
	}
	key := &ecdsa.PublicKey{
		Curve: elliptic.P256(),
		X:     new(big.Int).SetBytes(updatePublicKey[1:33]),
		Y:     new(big.Int).SetBytes(updatePublicKey[33:65]),
	}
	if !ecdsa.VerifyASN1(key, h.Sum(nil), sig) {
		return errors.New("its signature doesn't match - not installing it")
	}
	return nil
}

// replaceProgram puts the new program file where the running one is - the
// running one moved aside first, as Windows won't overwrite it - and
// restarts into it.
func replaceProgram(a *app, exe, newPath string) error {
	old := exe + ".old"
	os.Remove(old)
	if err := os.Rename(exe, old); err != nil {
		return fmt.Errorf("couldn't move the old program aside: %w", err)
	}
	if err := os.Rename(newPath, exe); err != nil {
		os.Rename(old, exe)
		return fmt.Errorf("couldn't put the new program in place: %w", err)
	}
	os.Chmod(exe, 0o755)
	if err := a.restart(exec.Command(exe, os.Args[1:]...)); err != nil {
		os.Remove(exe)
		os.Rename(old, exe)
		return fmt.Errorf("couldn't start the new version: %w", err)
	}
	return nil
}

// replaceApp swaps the Mac's whole "Omni360 Helper.app" for the one in the
// update's zip, and restarts into it.
func replaceApp(a *app, exe, zipPath string) error {
	bundle := appBundle(exe)
	if bundle == "" {
		return errors.New("this isn't running from Omni360 Helper.app - download the new version from the release page")
	}
	tmp, err := os.MkdirTemp(filepath.Dir(bundle), ".Omni360Helper-update-")
	if err != nil {
		return fmt.Errorf("can't write beside the app: %w", err)
	}
	defer os.RemoveAll(tmp)
	// ditto keeps what the zip was made with (by ditto, in the workflow):
	// permissions and the code signature.
	if out, err := exec.Command("ditto", "-x", "-k", zipPath, tmp).CombinedOutput(); err != nil {
		return fmt.Errorf("couldn't unzip it: %v %s", err, out)
	}
	fresh := filepath.Join(tmp, "Omni360 Helper.app")
	if _, err := os.Stat(filepath.Join(fresh, "Contents", "MacOS", "Omni360Helper")); err != nil {
		return errors.New("the update's zip doesn't hold the app")
	}
	old := bundle + ".old"
	os.RemoveAll(old)
	if err := os.Rename(bundle, old); err != nil {
		return fmt.Errorf("couldn't move the old app aside: %w", err)
	}
	if err := os.Rename(fresh, bundle); err != nil {
		os.Rename(old, bundle)
		return fmt.Errorf("couldn't put the new app in place: %w", err)
	}
	// -n: a new instance, not this one brought to the front.
	args := append([]string{"-n", bundle, "--args"}, os.Args[1:]...)
	if err := a.restart(exec.Command("open", args...)); err != nil {
		os.RemoveAll(bundle)
		os.Rename(old, bundle)
		return fmt.Errorf("couldn't start the new version: %w", err)
	}
	os.RemoveAll(old) // this process keeps running from memory until it quits
	return nil
}

// appBundle is the .app the program runs from (…/X.app/Contents/MacOS/exe),
// or "".
func appBundle(exe string) string {
	macos := filepath.Dir(exe)
	contents := filepath.Dir(macos)
	bundle := filepath.Dir(contents)
	if filepath.Base(macos) == "MacOS" && filepath.Base(contents) == "Contents" && strings.HasSuffix(bundle, ".app") {
		return bundle
	}
	return ""
}

// removeOldProgram removes what an update moved aside, once the old process
// that was running it has quit.
func removeOldProgram() {
	exe, err := os.Executable()
	if err != nil {
		return
	}
	if exe, err = filepath.EvalSymlinks(exe); err != nil {
		return
	}
	old := exe + ".old"
	if _, err := os.Stat(old); err != nil {
		return
	}
	for i := 0; i < 30; i++ {
		if err := os.Remove(old); err == nil || os.IsNotExist(err) {
			return
		}
		time.Sleep(time.Second)
	}
}

package main

import (
	"bytes"
	"context"
	"crypto/aes"
	"crypto/rand"
	"encoding/base64"
	"encoding/json"
	"fmt"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strings"
	"sync/atomic"
	"testing"
	"time"
)

// fakeService behaves like the real one — AES-256 in ECB mode, zero padding,
// trailing zeros removed on the way back, base64 without padding — under a key
// made for the test. The real key is not public and is not used here.
type fakeService struct {
	key       []byte
	decrypts  atomic.Int32
	encrypts  atomic.Int32
	failNext  atomic.Bool
	garbage   atomic.Bool // answer encrypt with bytes that are not the encryption
	wrongBack atomic.Bool // answer decrypt with different JSON
}

func newFake() *fakeService {
	k := make([]byte, 32)
	rand.Read(k)
	return &fakeService{key: k}
}

func (f *fakeService) ecb(in []byte, enc bool) []byte {
	c, _ := aes.NewCipher(f.key)
	if enc {
		for len(in)%16 != 0 {
			in = append(in, 0)
		}
	}
	out := make([]byte, len(in))
	for i := 0; i+16 <= len(in); i += 16 {
		if enc {
			c.Encrypt(out[i:], in[i:i+16])
		} else {
			c.Decrypt(out[i:], in[i:i+16])
		}
	}
	return out
}

func (f *fakeService) Decrypt(ctx context.Context, enc []byte) ([]byte, error) {
	f.decrypts.Add(1)
	if f.failNext.Load() {
		return nil, os.ErrDeadlineExceeded
	}
	out := bytes.TrimRight(f.ecb(enc, false), "\x00")
	if f.wrongBack.Load() {
		return []byte(`{"playerId":"tampered"}`), nil
	}
	return out, nil
}

func (f *fakeService) Encrypt(ctx context.Context, plain []byte) ([]byte, error) {
	f.encrypts.Add(1)
	if f.failNext.Load() {
		return nil, os.ErrDeadlineExceeded
	}
	if f.garbage.Load() {
		return bytes.Repeat([]byte{7}, 32), nil
	}
	return f.ecb(append([]byte{}, plain...), true), nil
}

// ------------------------------------------------------------------ fixtures

const guid = "FFDDE729442D495C4FC0F0A94E63CDDE"

func charJSON(xp int) []byte {
	b, _ := json.Marshal(map[string]any{"playerId": "p-1", "uniqueSaveId": "u-1", "version": 3, "xp": xp,
		"items": []any{map[string]any{"type": "Sword", "power": 1.5}}})
	return b
}

type env struct {
	t       *testing.T
	win, sw string
	svc     *fakeService
	s       *syncer
	clock   time.Time
	log     []string
}

func newEnv(t *testing.T) *env {
	t.Helper()
	e := &env{t: t, win: t.TempDir(), sw: t.TempDir(), svc: newFake(), clock: time.Now().Add(time.Hour)}
	e.s = &syncer{winDir: e.win, swDir: e.sw, svc: e.svc, settle: 3 * time.Second,
		now:  func() time.Time { return e.clock },
		logf: func(f string, a ...any) { e.log = append(e.log, strings.TrimSpace(sprintf(f, a...))) }}
	return e
}

func (e *env) pass() {
	e.t.Helper()
	if err := e.s.runOnce(context.Background()); err != nil {
		e.t.Fatal(err)
	}
}

func (e *env) writeWin(plain []byte) {
	e.t.Helper()
	enc, _ := e.svc.Encrypt(context.Background(), plain)
	e.svc.encrypts.Add(-1)
	if err := os.WriteFile(filepath.Join(e.win, guid+".dat"), append(append([]byte{}, magic...), enc...), 0o666); err != nil {
		e.t.Fatal(err)
	}
}
func (e *env) writeSw(plain []byte) {
	e.t.Helper()
	if err := os.WriteFile(filepath.Join(e.sw, "Character"+guid), plain, 0o666); err != nil {
		e.t.Fatal(err)
	}
}
func (e *env) readSw() []byte {
	b, _ := os.ReadFile(filepath.Join(e.sw, "Character"+guid))
	return b
}
func (e *env) readWinPlain() []byte {
	raw, err := os.ReadFile(filepath.Join(e.win, guid+".dat"))
	if err != nil {
		return nil
	}
	if !bytes.HasPrefix(raw, magic) {
		e.t.Fatalf("a Windows file was written without the D001 header")
	}
	p, _ := e.svc.Decrypt(context.Background(), raw[len(magic):])
	e.svc.decrypts.Add(-1)
	return p
}
func (e *env) logged(sub string) bool {
	for _, l := range e.log {
		if strings.Contains(l, sub) {
			return true
		}
	}
	return false
}
func (e *env) ago() { e.clock = e.clock.Add(time.Hour) } // time passes: files settle

func sprintf(f string, a ...any) string { return strings.ReplaceAll(fmt.Sprintf(f, a...), "\n", " ") }

// ------------------------------------------------------------------- tests

func TestWindowsToSwitchThenBackAgain(t *testing.T) {
	e := newEnv(t)
	e.writeWin(charJSON(100))
	e.pass()
	if !sameJSON(e.readSw(), charJSON(100)) {
		t.Fatalf("the Switch file is %q", e.readSw())
	}
	// Nothing changed: nothing is asked of the service.
	d, en := e.svc.decrypts.Load(), e.svc.encrypts.Load()
	e.pass()
	if e.svc.decrypts.Load() != d || e.svc.encrypts.Load() != en {
		t.Error("an unchanged pair still called the service")
	}
	// Progress on the Switch comes back into the Windows file, which is backed up.
	e.writeSw(charJSON(250))
	e.pass()
	if !sameJSON(e.readWinPlain(), charJSON(250)) {
		t.Fatalf("Windows file did not get the Switch's progress: %q", e.readWinPlain())
	}
	backups, _ := filepath.Glob(filepath.Join(e.win, backupName, guid+".*.dat"))
	if len(backups) != 1 {
		t.Fatalf("expected one backup of the old Windows save, got %v", backups)
	}
	// And a further change on Windows goes the other way again.
	e.writeWin(charJSON(900))
	e.pass()
	if !sameJSON(e.readSw(), charJSON(900)) {
		t.Fatalf("second Windows change did not reach the Switch: %q", e.readSw())
	}
}

func TestNewSwitchCharacterIsEncryptedIntoWindows(t *testing.T) {
	e := newEnv(t)
	e.writeSw(charJSON(5))
	e.pass()
	if !sameJSON(e.readWinPlain(), charJSON(5)) {
		t.Fatal("a Switch-only character was not written to Windows")
	}
}

func TestConflictChangesNothing(t *testing.T) {
	e := newEnv(t)
	e.writeWin(charJSON(1))
	e.pass()
	e.writeWin(charJSON(2))
	e.writeSw(charJSON(3)) // both moved on
	winBefore, _ := os.ReadFile(filepath.Join(e.win, guid+".dat"))
	swBefore := e.readSw()
	e.ago()
	e.pass()
	winAfter, _ := os.ReadFile(filepath.Join(e.win, guid+".dat"))
	if !bytes.Equal(winBefore, winAfter) || !bytes.Equal(swBefore, e.readSw()) {
		t.Fatal("a conflict overwrote something")
	}
	if !e.logged("CONFLICT") {
		t.Errorf("the conflict was not reported: %v", e.log)
	}
}

func TestFirstSightOfTwoDifferentCopiesIsAConflict(t *testing.T) {
	e := newEnv(t)
	e.writeWin(charJSON(10))
	e.writeSw(charJSON(20))
	e.pass()
	if !sameJSON(e.readSw(), charJSON(20)) || !sameJSON(e.readWinPlain(), charJSON(10)) {
		t.Fatal("two unrelated copies were merged without asking")
	}
	if !e.logged("CONFLICT") {
		t.Error("not reported")
	}
	// Identical copies are simply adopted.
	e2 := newEnv(t)
	e2.writeWin(charJSON(7))
	e2.writeSw(append(charJSON(7), '\n')) // formatting differs, content does not
	e2.pass()
	if !e2.logged("already the same") {
		t.Errorf("identical content was not recognised: %v", e2.log)
	}
}

func TestNothingIsEverDeleted(t *testing.T) {
	e := newEnv(t)
	e.writeWin(charJSON(1))
	e.pass()
	os.Remove(filepath.Join(e.sw, "Character"+guid)) // deleted on the Switch
	e.ago()
	e.pass()
	if e.readSw() != nil {
		t.Error("a deleted Switch file was brought back")
	}
	if _, err := os.Stat(filepath.Join(e.win, guid+".dat")); err != nil {
		t.Error("the Windows file was removed")
	}
}

func TestAServiceFailureWritesNothing(t *testing.T) {
	e := newEnv(t)
	e.writeWin(charJSON(1))
	e.svc.failNext.Store(true)
	e.pass()
	if e.readSw() != nil {
		t.Fatal("a file was written although the service failed")
	}
	e.svc.failNext.Store(false)
	e.pass() // and it recovers by itself
	if e.readSw() == nil {
		t.Fatal("did not recover once the service was back")
	}
}

func TestABadEncryptionIsNotWrittenToWindows(t *testing.T) {
	e := newEnv(t)
	e.writeWin(charJSON(1))
	e.pass()
	winBefore, _ := os.ReadFile(filepath.Join(e.win, guid+".dat"))

	e.writeSw(charJSON(2))
	e.svc.garbage.Store(true) // the service hands back something that is not the encryption
	e.ago()
	e.pass()
	winAfter, _ := os.ReadFile(filepath.Join(e.win, guid+".dat"))
	if !bytes.Equal(winBefore, winAfter) {
		t.Fatal("garbage was written into the game's save folder")
	}
	e.svc.garbage.Store(false)
	e.svc.wrongBack.Store(true) // encrypts fine, but decrypts back to something else
	e.pass()
	winAfter, _ = os.ReadFile(filepath.Join(e.win, guid+".dat"))
	if !bytes.Equal(winBefore, winAfter) {
		t.Fatal("an unverified encryption was written")
	}
	if !e.logged("did not decrypt back") && !e.logged("skipped") {
		t.Errorf("the refusal was not reported: %v", e.log)
	}
}

func TestOtherFilesAreLeftAlone(t *testing.T) {
	e := newEnv(t)
	os.WriteFile(filepath.Join(e.win, "notes.dat"), []byte("plain text, no header"), 0o666)
	// An encrypted file that is not a character (a profile, say).
	other, _ := e.svc.Encrypt(context.Background(), []byte(`{"settings":{"volume":3}}`))
	os.WriteFile(filepath.Join(e.win, "PROFILE.dat"), append(append([]byte{}, magic...), other...), 0o666)
	e.pass()
	e.pass()
	n := 0
	for _, l := range e.log {
		if strings.Contains(l, "not a character save") {
			n++
		}
	}
	if n != 1 {
		t.Errorf("a skipped file should be reported once, not %d times: %v", n, e.log)
	}
	entries, _ := os.ReadDir(e.sw)
	for _, en := range entries {
		if !strings.HasPrefix(en.Name(), ".") {
			t.Errorf("a file that is not a character reached the Switch folder: %s", en.Name())
		}
	}
}

func TestAFileBeingWrittenIsLeftForLater(t *testing.T) {
	e := newEnv(t)
	e.clock = time.Now() // files just written are fresh
	e.writeWin(charJSON(1))
	e.pass()
	if e.readSw() != nil {
		t.Fatal("a file still being written was copied")
	}
	e.ago()
	e.pass()
	if e.readSw() == nil {
		t.Fatal("it was never picked up")
	}
}

func TestStateSurvivesARestartAndIgnoresDamage(t *testing.T) {
	e := newEnv(t)
	e.writeWin(charJSON(1))
	e.pass()
	// A new process: same folders, fresh syncer.
	s2 := &syncer{winDir: e.win, swDir: e.sw, svc: e.svc, settle: e.s.settle, now: e.s.now, logf: e.s.logf}
	d := e.svc.decrypts.Load()
	if err := s2.runOnce(context.Background()); err != nil {
		t.Fatal(err)
	}
	if e.svc.decrypts.Load() != d {
		t.Error("the restarted tool re-did work it had already done")
	}
	// A damaged state file must not lose data: both copies exist and match, so it
	// simply re-learns that.
	os.WriteFile(filepath.Join(e.sw, stateName), []byte("{not json"), 0o666)
	swBefore := e.readSw()
	e.pass()
	if !bytes.Equal(swBefore, e.readSw()) {
		t.Fatal("damaged state changed a save")
	}
}

func TestBackupsAreKeptToAFew(t *testing.T) {
	e := newEnv(t)
	e.writeWin(charJSON(0))
	e.pass()
	for i := 1; i <= 8; i++ {
		e.ago()
		e.writeSw(charJSON(i))
		e.pass()
	}
	backups, _ := filepath.Glob(filepath.Join(e.win, backupName, guid+".*.dat"))
	if len(backups) != keepBackups {
		t.Fatalf("%d backups kept, want %d", len(backups), keepBackups)
	}
}

// ---------------------------------------------------- the HTTP protocol itself

func TestHTTPServiceSpeaksTheServicesProtocol(t *testing.T) {
	f := newFake()
	var gotDecrypt, gotEncrypt atomic.Bool
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		var in map[string]string
		json.NewDecoder(r.Body).Decode(&in)
		unpad := func(s string) []byte { b, _ := decodeB64(s); return b }
		switch r.URL.Path {
		case "/api/encryption/decrypt":
			gotDecrypt.Store(true)
			if in["Encrypted"] == "" {
				w.WriteHeader(422)
				return
			}
			p, _ := f.Decrypt(r.Context(), unpad(in["Encrypted"]))
			// The real service leaves the padding off its base64.
			json.NewEncoder(w).Encode(map[string]string{"Decrypted": strings.TrimRight(base64.StdEncoding.EncodeToString(p), "="), "Encrypted": ""})
		case "/api/encryption/encrypt":
			gotEncrypt.Store(true)
			if in["Decrypted"] == "" {
				w.WriteHeader(422)
				return
			}
			c, _ := f.Encrypt(r.Context(), unpad(in["Decrypted"]))
			json.NewEncoder(w).Encode(map[string]string{"Encrypted": strings.TrimRight(base64.StdEncoding.EncodeToString(c), "=")})
		default:
			w.WriteHeader(404)
		}
	}))
	defer srv.Close()

	svc := newHTTPService(srv.URL)
	plain := charJSON(42)
	enc, err := svc.Encrypt(context.Background(), plain)
	if err != nil || len(enc)%16 != 0 {
		t.Fatalf("encrypt: %v (%d bytes)", err, len(enc))
	}
	back, err := svc.Decrypt(context.Background(), enc)
	if err != nil || !bytes.Equal(back, plain) {
		t.Fatalf("round trip: %v %q", err, back)
	}
	if !gotDecrypt.Load() || !gotEncrypt.Load() {
		t.Error("an endpoint was not used")
	}
	if _, err := svc.Decrypt(context.Background(), nil); err == nil {
		t.Error("an empty save was sent")
	}
	if _, err := svc.Decrypt(context.Background(), make([]byte, maxBody+1)); err == nil {
		t.Error("an oversize save was sent")
	}
	// A server that errors, and one that sends rubbish.
	bad := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) { http.Error(w, "no", 500) }))
	defer bad.Close()
	if _, err := newHTTPService(bad.URL).Decrypt(context.Background(), []byte("0123456789abcdef")); err == nil {
		t.Error("a server error was not reported")
	}
	junk := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) { w.Write([]byte("<html>")) }))
	defer junk.Close()
	if _, err := newHTTPService(junk.URL).Decrypt(context.Background(), []byte("0123456789abcdef")); err == nil {
		t.Error("an unreadable reply was accepted")
	}
}

func TestFindWindowsFolder(t *testing.T) {
	root := t.TempDir()
	if _, err := findWindowsFolder(filepath.Join(root, "missing")); err == nil {
		t.Error("a missing folder was not reported")
	}
	if _, err := findWindowsFolder(root); err == nil {
		t.Error("an empty tree should find nothing")
	}
	chars := filepath.Join(root, "Saves", "abc", "Characters")
	os.MkdirAll(chars, 0o777)
	os.WriteFile(filepath.Join(chars, "plain.dat"), []byte("not encrypted"), 0o666) // no header: ignored
	if _, err := findWindowsFolder(root); err == nil {
		t.Error("a .dat without the header should not count")
	}
	os.WriteFile(filepath.Join(chars, guid+".dat"), append(append([]byte{}, magic...), make([]byte, 32)...), 0o666)
	got, err := findWindowsFolder(root)
	if err != nil || got != chars {
		t.Fatalf("got %q, %v; want %q", got, err, chars)
	}
	// Two places: refuse to guess which is meant.
	other := filepath.Join(root, "Saves", "def", "Characters")
	os.MkdirAll(other, 0o777)
	os.WriteFile(filepath.Join(other, "AAAA.dat"), append(append([]byte{}, magic...), make([]byte, 32)...), 0o666)
	if _, err := findWindowsFolder(root); err == nil || !strings.Contains(err.Error(), "several") {
		t.Errorf("two candidate folders must not be guessed between: %v", err)
	}
	// Our own backup folder is never mistaken for the saves.
	os.RemoveAll(other)
	os.MkdirAll(filepath.Join(root, backupName), 0o777)
	os.WriteFile(filepath.Join(root, backupName, "x.dat"), append(append([]byte{}, magic...), make([]byte, 32)...), 0o666)
	if got, err := findWindowsFolder(root); err != nil || got != chars {
		t.Errorf("the backup folder confused detection: %q %v", got, err)
	}
}

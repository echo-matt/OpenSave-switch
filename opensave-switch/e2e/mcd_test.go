package switche2e

import (
	"bytes"
	"crypto/aes"
	"crypto/rand"
	"encoding/base64"
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strings"
	"sync/atomic"
	"testing"
	"time"

	"github.com/opensave/opensave/testutil"
)

const mcdTitle = "01006C100EC08000"

var magicDat = []byte("D001\x00\x00\x00\x00")

// convService stands in for dungeons.tools: the same protocol (POST
// api/encryption/{decrypt,encrypt}, camelCase fields, base64 with no padding,
// AES-256-ECB with zero padding, zeros stripped on decrypt) under a key made for
// the test. The real key is not public and is not used.
type convService struct {
	srv      *httptest.Server
	key      []byte
	down     atomic.Bool
	garbage  atomic.Bool
	requests atomic.Int32
}

func newConvService(t *testing.T) *convService {
	t.Helper()
	c := &convService{key: make([]byte, 32)}
	rand.Read(c.key)
	c.srv = httptest.NewServer(http.HandlerFunc(c.handle))
	t.Cleanup(c.srv.Close)
	return c
}

func (c *convService) ecb(in []byte, enc bool) []byte {
	b, _ := aes.NewCipher(c.key)
	if enc {
		for len(in)%16 != 0 {
			in = append(in, 0)
		}
	}
	out := make([]byte, len(in))
	for i := 0; i+16 <= len(in); i += 16 {
		if enc {
			b.Encrypt(out[i:], in[i:i+16])
		} else {
			b.Decrypt(out[i:], in[i:i+16])
		}
	}
	return out
}

func b64(b []byte) string { return strings.TrimRight(base64.StdEncoding.EncodeToString(b), "=") }
func unb64(s string) []byte {
	b, _ := base64.RawStdEncoding.DecodeString(strings.TrimRight(s, "="))
	return b
}

func (c *convService) handle(w http.ResponseWriter, r *http.Request) {
	c.requests.Add(1)
	if c.down.Load() {
		http.Error(w, "down", 503)
		return
	}
	var in map[string]string
	json.NewDecoder(r.Body).Decode(&in)
	switch r.URL.Path {
	case "/api/encryption/decrypt":
		plain := bytes.TrimRight(c.ecb(unb64(in["Encrypted"]), false), "\x00")
		json.NewEncoder(w).Encode(map[string]any{"encrypted": nil, "decrypted": b64(plain)})
	case "/api/encryption/encrypt":
		enc := c.ecb(append([]byte{}, unb64(in["Decrypted"])...), true)
		if c.garbage.Load() {
			enc = bytes.Repeat([]byte{9}, 32)
		}
		json.NewEncoder(w).Encode(map[string]any{"encrypted": b64(enc), "decrypted": nil})
	default:
		http.NotFound(w, r)
	}
}

func (c *convService) dat(plain []byte) []byte {
	return append(append([]byte{}, magicDat...), c.ecb(append([]byte{}, plain...), true)...)
}
func (c *convService) undat(raw []byte) []byte {
	return bytes.TrimRight(c.ecb(raw[len(magicDat):], false), "\x00")
}

func character(id string, xp int) []byte {
	b, _ := json.Marshal(map[string]any{"playerId": "p-" + id, "uniqueSaveId": id, "version": 3, "xp": xp,
		"items": []any{map[string]any{"type": "Sword", "power": 1.25}}})
	return b
}

const (
	guidA = "FFDDE729442D495C4FC0F0A94E63CDDE"
	guidB = "AABBCCDDEEFF00112233445566778899"
	guidN = "0123456789ABCDEF0123456789ABCDEF"
)

type mcdEnv struct {
	t     *testing.T
	svc   *convService
	pc    *testutil.TestDaemon
	pcID  string
	sw    *switchDev
	chars string // the Windows folder, below the tracked one
}

func (e *mcdEnv) swPath(name string) string {
	return filepath.Join(e.sw.saves, mcdTitle, name)
}
func (e *mcdEnv) readSw(name string) []byte {
	b, _ := os.ReadFile(e.swPath(name))
	return b
}
func (e *mcdEnv) run(timeout time.Duration, args ...string) string {
	return e.sw.run(timeout, append([]string{"--service", e.svc.srv.URL + "/"}, args...)...)
}

// newMcdEnv: a PC tracking a Windows-style Minecraft Dungeons folder (nested,
// with a profile and a settings file beside the characters), a Switch paired
// with it, and the PC's game linked to the Switch title.
func newMcdEnv(t *testing.T) *mcdEnv {
	t.Helper()
	e := &mcdEnv{t: t, svc: newConvService(t)}
	e.pc = testutil.NewTestDaemon(t, "My PC")
	e.chars = "Saves/prof1/Characters"
	e.pc.WriteSave(e.chars+"/"+guidA+".dat", string(e.svc.dat(character(guidA, 100))))
	e.pc.WriteSave(e.chars+"/"+guidB+".dat", string(e.svc.dat(character(guidB, 200))))
	e.pc.WriteSave("Saves/prof1/ProfileSettings.dat", string(e.svc.dat([]byte(`{"settings":{"volume":3}}`)))) // encrypted, not a character
	e.pc.WriteSave("settings.txt", "plain settings")
	e.pcID = e.pc.TrackGame("Minecraft Dungeons (Saved)")

	e.sw = newSwitch(t)
	if err := os.MkdirAll(filepath.Join(e.sw.saves, mcdTitle), 0o777); err != nil {
		t.Fatal(err)
	}
	pair(t, e.sw, &pcSetup{pc: e.pc, gameID: e.pcID})

	out := e.sw.run(30*time.Second, "games")
	if !strings.Contains(out, "GAME id="+e.pcID) {
		t.Fatalf("the PC's game list does not show %q:\n%s", e.pcID, out)
	}
	if out := e.sw.run(30*time.Second, "link", mcdTitle, e.pcID, "Minecraft Dungeons (Saved)"); !strings.Contains(out, "LINKED") {
		t.Fatalf("link: %s", out)
	}
	return e
}

func sameJ(a, b []byte) bool {
	var ca, cb bytes.Buffer
	return json.Compact(&ca, a) == nil && json.Compact(&cb, b) == nil && bytes.Equal(ca.Bytes(), cb.Bytes())
}

func (e *mcdEnv) pcFile(rel string) []byte {
	b, _ := os.ReadFile(filepath.Join(e.pc.SaveDir, filepath.FromSlash(rel)))
	return b
}

func TestMcdReceiveConvertsWindowsSavesForTheSwitch(t *testing.T) {
	e := newMcdEnv(t)

	out := e.run(30*time.Second, "mcd-compare", mcdTitle)
	if !strings.Contains(out, "MCD-COMPARE state=1 only_remote=4") {
		t.Fatalf("before receiving, everything on the PC is new:\n%s", out)
	}
	// Looking must not have created anything on the PC.
	if o, _ := e.pc.Daemon.Store.ListOfferedGames(); len(o) != 0 {
		t.Fatalf("a compare listed an offered game on the PC: %v", o)
	}

	out = e.run(90*time.Second, "mcd-pull", mcdTitle, "5")
	if !strings.Contains(out, "MCD-PULL OK same=0 converted=2 skipped=1 files=4") {
		t.Fatalf("receive: %s", out)
	}
	// The Switch has the characters, as plain JSON, and nothing else.
	if !sameJ(e.readSw("Character"+guidA), character(guidA, 100)) || !sameJ(e.readSw("Character"+guidB), character(guidB, 200)) {
		t.Fatalf("the Switch characters are wrong: %q", e.readSw("Character"+guidA))
	}
	ents, _ := os.ReadDir(filepath.Join(e.sw.saves, mcdTitle))
	if len(ents) != 2 {
		t.Errorf("the Switch save should hold only the two characters, has %d entries", len(ents))
	}
	// The PC recorded the Switch as in step with it: the mirror it served matches.
	if !testutil.WaitFor(20*time.Second, func() bool {
		return e.pc.Daemon.Store.GetAgreedHash(e.pcID, switchID(t, &pcSetup{pc: e.pc})) != ""
	}) {
		t.Error("the PC never recorded the Switch as in sync")
	}
	// Nothing left to do.
	if out := e.run(30*time.Second, "mcd-compare", mcdTitle); !strings.Contains(out, "state=0") {
		t.Fatalf("after receiving, the two should match:\n%s", out)
	}
	if out := e.run(30*time.Second, "mcd-pull", mcdTitle, "1"); !strings.Contains(out, "same=1") {
		t.Fatalf("a second receive should have nothing to do: %s", out)
	}
	// The PC's files were not touched by any of this.
	if !sameJ(e.svc.undat(e.pcFile(e.chars+"/"+guidA+".dat")), character(guidA, 100)) {
		t.Error("the PC's own save changed")
	}
}

func TestMcdSendEncryptsSwitchProgressIntoTheWindowsFolder(t *testing.T) {
	e := newMcdEnv(t)
	if out := e.run(90*time.Second, "mcd-pull", mcdTitle, "3"); !strings.Contains(out, "converted=2") {
		t.Fatalf("receive: %s", out)
	}
	untouchedB := e.pcFile(e.chars + "/" + guidB + ".dat")
	profile := e.pcFile("Saves/prof1/ProfileSettings.dat")

	// Progress on a character, and a character created on the Switch.
	time.Sleep(1100 * time.Millisecond)
	if err := os.WriteFile(e.swPath("Character"+guidA), character(guidA, 5000), 0o666); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(e.swPath("Character"+guidN), character(guidN, 7), 0o666); err != nil {
		t.Fatal(err)
	}
	if out := e.run(30*time.Second, "mcd-compare", mcdTitle); !strings.Contains(out, "only_local=1") || !strings.Contains(out, "differ=1") {
		t.Fatalf("the Switch's edits should show before sending:\n%s", out)
	}

	p := e.sw.start("--service", e.svc.srv.URL+"/", "mcd-send", mcdTitle, "25")
	line := p.waitFor("MCD-SEND TRIGGERED", 60*time.Second)
	if !strings.Contains(line, "prepared=2") {
		t.Fatalf("expected two characters prepared: %s", line)
	}
	if !testutil.WaitFor(40*time.Second, func() bool {
		raw := e.pcFile(e.chars + "/" + guidA + ".dat")
		return raw != nil && bytes.HasPrefix(raw, magicDat) && sameJ(e.svc.undat(raw), character(guidA, 5000))
	}) {
		t.Fatalf("the PC never received the Switch's progress\nswitch output:\n%s", p.output())
	}
	// A new character lands beside the others, encrypted and with the header.
	newRaw := e.pcFile(e.chars + "/" + guidN + ".dat")
	if newRaw == nil || !bytes.HasPrefix(newRaw, magicDat) || !sameJ(e.svc.undat(newRaw), character(guidN, 7)) {
		t.Fatalf("the new character did not reach the PC's character folder")
	}
	// Everything else on the PC is exactly as it was.
	if !bytes.Equal(e.pcFile(e.chars+"/"+guidB+".dat"), untouchedB) {
		t.Error("an unchanged character was rewritten")
	}
	if !bytes.Equal(e.pcFile("Saves/prof1/ProfileSettings.dat"), profile) || string(e.pcFile("settings.txt")) != "plain settings" {
		t.Error("other files on the PC were disturbed")
	}
}

func TestMcdFailuresLeaveEverythingAlone(t *testing.T) {
	e := newMcdEnv(t)
	e.sw.write("keep-me", "x") // (written under the Pokémon-style title dir; irrelevant to this one)
	if err := os.WriteFile(e.swPath("CharacterOLD0000000000000"), character("OLD", 1), 0o666); err != nil {
		t.Fatal(err)
	}
	before := snapshot(t, filepath.Join(e.sw.saves, mcdTitle))

	// The conversion service is down: nothing on the Switch changes.
	e.svc.down.Store(true)
	out := e.run(60*time.Second, "mcd-pull", mcdTitle, "1")
	if !strings.Contains(out, "MCD-PULL FAIL") || !strings.Contains(out, "conversion service") {
		t.Fatalf("a service outage should fail the receive and say why: %s", out)
	}
	if !sameSnapshot(before, snapshot(t, filepath.Join(e.sw.saves, mcdTitle))) {
		t.Fatal("the Switch save changed although the service was down")
	}
	e.svc.down.Store(false)

	// Receive for real, then break sending: garbage from the service must not
	// reach the PC or the mirror.
	if out := e.run(90*time.Second, "mcd-pull", mcdTitle, "3"); !strings.Contains(out, "MCD-PULL OK") {
		t.Fatalf("receive: %s", out)
	}
	pcBefore := e.pcFile(e.chars + "/" + guidA + ".dat")
	os.WriteFile(e.swPath("Character"+guidA), character(guidA, 999), 0o666)
	e.svc.garbage.Store(true)
	out = e.run(60*time.Second, "mcd-send", mcdTitle, "2")
	if !strings.Contains(out, "MCD-SEND FAIL") {
		t.Fatalf("an unverifiable encryption must fail the send: %s", out)
	}
	if !bytes.Equal(e.pcFile(e.chars+"/"+guidA+".dat"), pcBefore) {
		t.Fatal("garbage was written into the PC's save")
	}
	e.svc.garbage.Store(false)

	// Sending before anything was received is refused with an instruction.
	e2 := newMcdEnv(t)
	if out := e2.run(30*time.Second, "mcd-send", mcdTitle, "1"); !strings.Contains(out, "Receive from the PC first") {
		t.Fatalf("send before receive: %s", out)
	}
}

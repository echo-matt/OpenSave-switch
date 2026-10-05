// Package switche2e runs the Switch client's portable C core (as the hostcli
// test driver) against a real, in-process OpenSave daemon over loopback HTTP.
//
// It is the check that matters most: every other test of the C code compares it
// with a reading of the Go code, and this one makes the Go daemon itself accept
// or refuse what the C client does — pairing, signed requests, manifests, block
// transfers in both directions — and then breaks the transfer in the ways a real
// network does, to show a save is never left half-written.
package switche2e

import (
	"bufio"
	"bytes"
	"encoding/base64"
	"encoding/json"
	"fmt"
	"io"
	"net"
	"net/http"
	"net/http/httptest"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"strconv"
	"strings"
	"sync"
	"testing"
	"time"

	"github.com/opensave/opensave/internal/e2ee"
	"github.com/opensave/opensave/testutil"
)

const title = "0100F2C0115B6000"

var hostcli string

func TestMain(m *testing.M) {
	// The driver speaks POSIX sockets and reads /dev/urandom: it is built and
	// run on Linux and macOS. The Windows CI job has neither the toolchain nor
	// the need — the Switch client is not built there.
	if runtime.GOOS == "windows" {
		fmt.Println("skipping: the Switch client's test driver is not built on Windows")
		os.Exit(0)
	}
	if _, err := exec.LookPath("make"); err != nil {
		fmt.Println("skipping: make not installed")
		os.Exit(0)
	}
	if _, err := exec.LookPath("gcc"); err != nil {
		fmt.Println("skipping: gcc not installed")
		os.Exit(0)
	}
	dir, err := os.MkdirTemp("", "hostcli-*")
	if err != nil {
		panic(err)
	}
	cmd := exec.Command("make", "-f", "Makefile.host", "BUILD="+dir, "hostcli")
	cmd.Dir = ".."
	if out, err := cmd.CombinedOutput(); err != nil {
		fmt.Printf("building hostcli failed: %v\n%s\n", err, out)
		os.Exit(1)
	}
	hostcli = filepath.Join(dir, "hostcli")
	code := m.Run()
	os.RemoveAll(dir)
	os.Exit(code)
}

// ---------------------------------------------------------------- the client

// switchDev is one Switch: its settings file, its saves, and a fixed port (the
// PC remembers where to call it back, so it must not change between runs).
type switchDev struct {
	t     *testing.T
	state string
	saves string
	work  string
	port  int
}

func newSwitch(t *testing.T) *switchDev {
	t.Helper()
	root := testutil.TempDir(t)
	l, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	port := l.Addr().(*net.TCPAddr).Port
	l.Close()
	d := &switchDev{t: t, state: filepath.Join(root, "cfg", "state.json"), saves: filepath.Join(root, "saves"),
		work: filepath.Join(root, "work"), port: port}
	if err := os.MkdirAll(d.titleDir(), 0o777); err != nil {
		t.Fatal(err)
	}
	return d
}

func (d *switchDev) titleDir() string { return filepath.Join(d.saves, title) }

func (d *switchDev) args(extra ...string) []string {
	return append([]string{"--state", d.state, "--saves", d.saves, "--work", d.work,
		"--port", strconv.Itoa(d.port), "--name", "Test Switch"}, extra...)
}

func (d *switchDev) write(rel, content string) {
	d.t.Helper()
	full := filepath.Join(d.titleDir(), filepath.FromSlash(rel))
	if err := os.MkdirAll(filepath.Dir(full), 0o777); err != nil {
		d.t.Fatal(err)
	}
	if err := os.WriteFile(full, []byte(content), 0o666); err != nil {
		d.t.Fatal(err)
	}
}

func (d *switchDev) read(rel string) (string, bool) {
	raw, err := os.ReadFile(filepath.Join(d.titleDir(), filepath.FromSlash(rel)))
	return string(raw), err == nil
}

// run executes one command to completion and returns what it printed.
func (d *switchDev) run(timeout time.Duration, args ...string) string {
	d.t.Helper()
	cmd := exec.Command(hostcli, d.args(args...)...)
	var out bytes.Buffer
	cmd.Stdout = &out
	var stderr bytes.Buffer
	cmd.Stderr = &stderr
	if err := cmd.Start(); err != nil {
		d.t.Fatal(err)
	}
	done := make(chan error, 1)
	go func() { done <- cmd.Wait() }()
	select {
	case <-done:
	case <-time.After(timeout):
		cmd.Process.Kill()
		d.t.Fatalf("hostcli %v timed out\nstdout:\n%s\nstderr:\n%s", args, out.String(), stderr.String())
	}
	// A sanitizer report on stderr is a failure even if the command "worked".
	if strings.Contains(stderr.String(), "ERROR: AddressSanitizer") || strings.Contains(stderr.String(), "runtime error") ||
		strings.Contains(stderr.String(), "LeakSanitizer") {
		d.t.Fatalf("hostcli %v: sanitizer report:\n%s", args, stderr.String())
	}
	return out.String()
}

// proc is a long-running hostcli whose output is read as it arrives.
type proc struct {
	t     *testing.T
	cmd   *exec.Cmd
	mu    sync.Mutex
	lines []string
	done  chan struct{}
	err   bytes.Buffer
}

func (d *switchDev) start(args ...string) *proc {
	d.t.Helper()
	p := &proc{t: d.t, done: make(chan struct{})}
	p.cmd = exec.Command(hostcli, d.args(args...)...)
	p.cmd.Stderr = &p.err
	stdout, err := p.cmd.StdoutPipe()
	if err != nil {
		d.t.Fatal(err)
	}
	if err := p.cmd.Start(); err != nil {
		d.t.Fatal(err)
	}
	go func() {
		sc := bufio.NewScanner(stdout)
		for sc.Scan() {
			p.mu.Lock()
			p.lines = append(p.lines, sc.Text())
			p.mu.Unlock()
		}
		p.cmd.Wait()
		close(p.done)
	}()
	d.t.Cleanup(func() { p.cmd.Process.Kill() })
	return p
}

func (p *proc) output() string {
	p.mu.Lock()
	defer p.mu.Unlock()
	return strings.Join(p.lines, "\n")
}

func (p *proc) waitFor(prefix string, d time.Duration) string {
	p.t.Helper()
	var found string
	if !testutil.WaitFor(d, func() bool {
		p.mu.Lock()
		defer p.mu.Unlock()
		for _, l := range p.lines {
			if strings.HasPrefix(l, prefix) {
				found = l
				return true
			}
		}
		return false
	}) {
		p.t.Fatalf("never saw %q from hostcli; output:\n%s\nstderr:\n%s", prefix, p.output(), p.err.String())
	}
	return found
}

func (p *proc) wait(d time.Duration) {
	p.t.Helper()
	select {
	case <-p.done:
	case <-time.After(d):
		p.t.Fatalf("hostcli did not finish; output:\n%s", p.output())
	}
	if s := p.err.String(); strings.Contains(s, "ERROR: AddressSanitizer") || strings.Contains(s, "runtime error") ||
		strings.Contains(s, "LeakSanitizer") {
		p.t.Fatalf("sanitizer report:\n%s", s)
	}
}

func field(line, key string) string {
	i := strings.Index(line, key+"=")
	if i < 0 {
		return ""
	}
	rest := line[i+len(key)+1:]
	if strings.HasPrefix(rest, "\"") {
		j := strings.Index(rest[1:], "\"")
		return rest[1 : 1+j]
	}
	if j := strings.IndexAny(rest, " \n"); j >= 0 {
		return rest[:j]
	}
	return rest
}

// ------------------------------------------------------------------- the PC

type pcSetup struct {
	pc      *testutil.TestDaemon
	gameID  string
	saveDir string
}

// newPC boots a daemon that tracks a Switch game the way a scan of an
// emulator's save folder would: at .../save/<account>/<profile>/<title id>.
func newPC(t *testing.T) *pcSetup {
	t.Helper()
	pc := testutil.NewTestDaemon(t, "My PC")
	dir := filepath.Join(pc.SaveDir, "save", "0000000000000000", "00000000000000000000000000000001", title)
	if err := os.MkdirAll(dir, 0o777); err != nil {
		t.Fatal(err)
	}
	pc.SaveDir = dir
	pc.WriteSave("main", "PC main save, chapter 5")
	pc.WriteSave("slot/1.dat", "slot one from the PC")
	pc.WriteSave("slot/2.dat", strings.Repeat("0123456789abcdef", 8000)) // 128 KB: three 64 KB blocks
	// A name-based id on purpose: the Switch must use the PC's own id for the
	// game, not assume "switch-<title id>".
	var resp struct {
		ID string `json:"id"`
	}
	pc.API(http.MethodPost, "/api/games", map[string]string{"name": "Test Game", "savePath": dir}, &resp)
	if resp.ID == "" {
		t.Fatal("no game id")
	}
	return &pcSetup{pc: pc, gameID: resp.ID, saveDir: dir}
}

// pair performs the Switch-initiated pairing flow end to end.
func pair(t *testing.T, sw *switchDev, s *pcSetup) (cLine string) {
	t.Helper()
	p := sw.start("pair", "127.0.0.1", strconv.Itoa(s.pc.Port), "60")
	p.waitFor("HANDSHAKE-SENT", 15*time.Second)
	var swID string
	if !testutil.WaitFor(15*time.Second, func() bool {
		reqs := s.pc.Daemon.P2P.Pairing.PendingRequests()
		if len(reqs) == 0 {
			return false
		}
		swID = reqs[0].PeerID
		return true
	}) {
		t.Fatalf("the PC never saw the Switch's pairing request; output:\n%s", p.output())
	}
	s.pc.API(http.MethodPost, "/api/peers/approve", map[string]any{"peerId": swID}, nil)
	line := p.waitFor("PAIRED", 20*time.Second)
	p.wait(20 * time.Second)

	// Both devices must show the same fingerprint, which is what a person
	// compares to rule out a man in the middle. The C side computes its own;
	// the PC's comes from Go's e2ee.Fingerprint over the keys it pinned.
	pcFP, err := s.pc.Daemon.Store.PairingFingerprint(swID)
	if err != nil || pcFP == "" {
		t.Fatalf("PC has no fingerprint for the Switch: %v", err)
	}
	if got := field(line, "fingerprint"); got != pcFP {
		t.Fatalf("fingerprints differ: Switch shows %q, PC shows %q", got, pcFP)
	}
	return line
}

func switchID(t *testing.T, s *pcSetup) string {
	t.Helper()
	peers, err := s.pc.Daemon.Store.ListPeers()
	if err != nil || len(peers) == 0 {
		t.Fatalf("PC has no peers: %v", err)
	}
	return peers[0].ID
}

// ------------------------------------------------------------------- tests

func TestPingAnswersLikeADevice(t *testing.T) {
	s := newPC(t)
	sw := newSwitch(t)
	out := sw.run(30*time.Second, "ping", "127.0.0.1", strconv.Itoa(s.pc.Port))
	if !strings.Contains(out, `PING OK name="My PC"`) {
		t.Fatalf("ping: %s", out)
	}
	// And the Switch answers a PC's ping, which is how the PC knows it is online.
	p := sw.start("serve", "6")
	line := p.waitFor("LISTENING", 10*time.Second)
	resp, err := http.Get("http://127.0.0.1:" + field(line, "port") + "/api/p2p/ping?from=nobody")
	if err != nil {
		t.Fatal(err)
	}
	defer resp.Body.Close()
	var body map[string]any
	json.NewDecoder(resp.Body).Decode(&body)
	if body["status"] != "ok" || body["paired"] != false || body["deviceName"] != "Test Switch" || body["deviceType"] != "handheld" {
		t.Fatalf("the Switch's ping answer is %v", body)
	}
}

// TestPairPullAndPush is the whole journey: pair, take the PC's save onto the
// Switch (with a backup), then make progress on the Switch and send it back.
func TestPairPullAndPush(t *testing.T) {
	s := newPC(t)
	sw := newSwitch(t)
	pair(t, sw, s)

	// The Switch holds an older, different save: one file to overwrite, one the
	// PC lacks, and a hidden file the sync never carries.
	sw.write("main", "Switch main save, chapter 2")
	sw.write("slot/1.dat", "old slot one")
	sw.write("obsolete.dat", "only on the Switch")
	sw.write(".hidden", "never synced")

	// The PC is not waiting for a sync; it needs to see the Switch online.
	if !testutil.WaitFor(20*time.Second, func() bool {
		p, err := s.pc.Daemon.Store.GetPeer(switchID(t, s))
		return err == nil && p.Status == "online"
	}) {
		t.Log("PC does not yet show the Switch online; carrying on")
	}

	cmp := sw.run(30*time.Second, "compare", title)
	if !strings.Contains(cmp, "RESOLVE found=1 game="+s.gameID) {
		t.Fatalf("the Switch did not find the PC's own id (%q) for the title:\n%s", s.gameID, cmp)
	}
	if !strings.Contains(cmp, "COMPARE state=1 only_remote=1 only_local=1 differ=2") {
		t.Fatalf("compare: %s", cmp)
	}

	// ---- pull
	out := sw.run(60*time.Second, "pull", title, "5")
	if !strings.Contains(out, "PULL OK same=0 downloaded=3 deleted=1") {
		t.Fatalf("pull: %s", out)
	}
	for rel, want := range map[string]string{
		"main":       "PC main save, chapter 5",
		"slot/1.dat": "slot one from the PC",
		"slot/2.dat": strings.Repeat("0123456789abcdef", 8000),
	} {
		if got, _ := sw.read(rel); got != want {
			t.Errorf("after pull %s = %q…", rel, got[:min(len(got), 40)])
		}
	}
	if _, ok := sw.read("obsolete.dat"); ok {
		t.Error("a file the PC does not have should have been removed")
	}
	if got, _ := sw.read(".hidden"); got != "never synced" {
		t.Error("a hidden file was touched")
	}
	// The backup holds exactly what was there before — hidden file included.
	backup := filepath.Join(sw.work, "backup", title)
	for rel, want := range map[string]string{"main": "Switch main save, chapter 2", "obsolete.dat": "only on the Switch", ".hidden": "never synced"} {
		raw, err := os.ReadFile(filepath.Join(backup, rel))
		if err != nil || string(raw) != want {
			t.Errorf("backup %s = %q, %v", rel, raw, err)
		}
	}
	if _, err := os.Stat(filepath.Join(sw.work, "staging", title)); err == nil {
		t.Error("staging was not cleaned up")
	}

	// The in-sync report must have reached the PC and been checked by it: it
	// calls back for the Switch's file list and records the agreed state.
	if !testutil.WaitFor(20*time.Second, func() bool {
		return s.pc.Daemon.Store.GetAgreedHash(s.gameID, switchID(t, s)) != ""
	}) {
		t.Fatal("the PC never recorded the Switch as in sync after the pull")
	}

	// Pulling again is a no-op.
	if again := sw.run(30*time.Second, "pull", title, "1"); !strings.Contains(again, "PULL OK same=1 downloaded=0") {
		t.Fatalf("second pull: %s", again)
	}

	// ---- push: the Switch makes progress and asks the PC to take it.
	time.Sleep(1100 * time.Millisecond)
	sw.write("main", "Switch main save, chapter 6 — progress made on the train")
	sw.write("slot/3.dat", "a brand new slot")
	p := sw.start("push", title, "20")
	p.waitFor("PUSH TRIGGERED", 20*time.Second)
	if !testutil.WaitFor(30*time.Second, func() bool {
		return s.pc.ReadSave("main") == "Switch main save, chapter 6 — progress made on the train" &&
			s.pc.ReadSave("slot/3.dat") == "a brand new slot"
	}) {
		t.Fatalf("the PC never took the Switch's progress; main=%q\nswitch output:\n%s", s.pc.ReadSave("main"), p.output())
	}
	if s.pc.ReadSave("slot/1.dat") != "slot one from the PC" {
		t.Error("an unchanged file was disturbed by the push")
	}
}

// TestNothingAcceptedUnsigned drives the Switch's server with the requests an
// attacker on the network could make.
func TestNothingAcceptedUnsigned(t *testing.T) {
	s := newPC(t)
	sw := newSwitch(t)
	pair(t, sw, s)
	sw.write("main", "the Switch's secret save")

	p := sw.start("serve", "40")
	base := "http://127.0.0.1:" + field(p.waitFor("LISTENING", 10*time.Second), "port")
	swID := ""
	peers, _ := s.pc.Daemon.Store.ListPeers()
	swID = peers[0].ID
	gameID := "switch-" + strings.ToLower(title)

	do := func(method, path string, body []byte, hdr map[string]string) (int, string) {
		req, _ := http.NewRequest(method, base+path, bytes.NewReader(body))
		for k, v := range hdr {
			req.Header.Set(k, v)
		}
		resp, err := http.DefaultClient.Do(req)
		if err != nil {
			t.Fatal(err)
		}
		defer resp.Body.Close()
		raw, _ := io.ReadAll(resp.Body)
		return resp.StatusCode, string(raw)
	}

	// 1. No signature at all: refused, and nothing about the save is revealed.
	for _, path := range []string{"/api/p2p/manifest/" + gameID, "/api/p2p/games", "/api/sync/trigger/" + gameID} {
		if code, body := do("GET", path, nil, nil); code != 401 || strings.Contains(body, "secret") {
			t.Errorf("unsigned GET %s -> %d %s", path, code, body)
		}
	}
	if code, _ := do("POST", "/api/p2p/blocks/"+gameID, []byte(`{"relPath":"main","blockIndices":[0],"blockSize":65536}`), nil); code != 401 {
		t.Errorf("unsigned blocks request -> %d", code)
	}
	// 2. The right peer id with a made-up MAC.
	forged := map[string]string{"X-Opensave-Peer": swID, "X-Opensave-Nonce": "abcd", "X-Opensave-Auth-Ms": strconv.FormatInt(time.Now().UnixMilli(), 10), "X-Opensave-Auth": "AAAA"}
	if code, _ := do("GET", "/api/p2p/manifest/"+gameID, nil, forged); code != 401 {
		t.Errorf("forged MAC -> %d", code)
	}
	// 3. A properly signed request is served — signed as the PC would, by Go's
	//    own e2ee code and the keys the PC pinned.
	pcKey, _ := s.pc.Daemon.Store.DeviceIdentity()
	swPeer, _ := s.pc.Daemon.Store.GetPeer(swID)
	theirPub, _ := e2ee.DecodeKey(swPeer.PublicKey)
	key, err := e2ee.AuthKey(pcKey.Private, theirPub)
	if err != nil {
		t.Fatal(err)
	}
	pcSettings, _ := s.pc.Daemon.Store.GetSettings()
	sign := func(method, route string, body []byte, at int64, nonce string) map[string]string {
		return map[string]string{
			"X-Opensave-Peer": pcSettings.NodeID, "X-Opensave-Nonce": nonce,
			"X-Opensave-Auth-Ms": strconv.FormatInt(at, 10),
			"X-Opensave-Auth":    e2ee.RequestMAC(key, pcSettings.NodeID, swID, route, method, body, nonce, at),
		}
	}
	route := "/api/p2p/manifest/" + gameID + "?name=x&savePath=y&isFile=false"
	now := time.Now().UnixMilli()
	h := sign("GET", route, nil, now, "nonce-ok-1")
	code, body := do("GET", route, nil, h)
	if code != 200 || !strings.Contains(body, `"main"`) {
		t.Fatalf("a properly signed manifest request was refused: %d %s", code, body)
	}
	// 4. The identical request again is a replay.
	if code, _ := do("GET", route, nil, h); code != 401 {
		t.Errorf("replayed request -> %d, want 401", code)
	}
	// 5. A signature for one route is no good on another, or with another body.
	if code, _ := do("GET", "/api/p2p/manifest/"+gameID+"?name=other", nil, sign("GET", route, nil, now, "nonce-2")); code != 401 {
		t.Errorf("signature reused on another route -> %d", code)
	}
	blocksBody := []byte(`{"relPath":"main","root":"","blockIndices":[0],"blockSize":65536}`)
	bh := sign("POST", "/api/p2p/blocks/"+gameID, blocksBody, now, "nonce-3")
	if code, _ := do("POST", "/api/p2p/blocks/"+gameID, []byte(`{"relPath":"main","root":"","blockIndices":[0,1],"blockSize":65536}`), bh); code != 401 {
		t.Errorf("signature reused with another body -> %d", code)
	}
	// 6. A stale timestamp is refused, with a hint about the clock.
	stale := now - 10*60*1000
	if code, body := do("GET", route, nil, sign("GET", route, nil, stale, "nonce-4")); code != 401 || !strings.Contains(body, "date and time") {
		t.Errorf("stale request -> %d %s", code, body)
	}
	// 7. Blocks come back correct, and paths that escape the save are refused.
	code, body = do("POST", "/api/p2p/blocks/"+gameID, blocksBody, sign("POST", "/api/p2p/blocks/"+gameID, blocksBody, now, "nonce-5"))
	if code != 200 {
		t.Fatalf("blocks: %d %s", code, body)
	}
	var br struct {
		Blocks []struct {
			Index  int    `json:"index"`
			Data   string `json:"data"`
			Length int    `json:"length"`
		} `json:"blocks"`
	}
	json.Unmarshal([]byte(body), &br)
	if len(br.Blocks) != 1 {
		t.Fatalf("blocks reply: %s", body)
	}
	if raw, _ := base64.StdEncoding.DecodeString(br.Blocks[0].Data); string(raw) != "the Switch's secret save" {
		t.Errorf("served block = %q", raw)
	}
	for i, bad := range []string{"../../etc/passwd", "/etc/passwd", "a/../../x", ".hidden", "slot/../../../x"} {
		b := []byte(fmt.Sprintf(`{"relPath":%q,"root":"","blockIndices":[0],"blockSize":65536}`, bad))
		nonce := fmt.Sprintf("nonce-bad-%d", i)
		if code, _ := do("POST", "/api/p2p/blocks/"+gameID, b, sign("POST", "/api/p2p/blocks/"+gameID, b, now, nonce)); code == 200 {
			t.Errorf("a request for %q was served", bad)
		}
	}
	// 8. Another device may not delete files here.
	del := []byte(`{"relPath":"main"}`)
	if code, _ := do("POST", "/api/p2p/delete-file/"+gameID, del, sign("POST", "/api/p2p/delete-file/"+gameID, del, now, "nonce-del")); code != 403 {
		t.Errorf("remote delete -> %d, want 403", code)
	}
	if got, _ := sw.read("main"); got != "the Switch's secret save" {
		t.Errorf("the save changed: %q", got)
	}
	// 9. Pairing messages from nobody who was asked for are not accepted.
	spoof := []byte(`{"peerId":"node_evil","deviceName":"Evil","port":9,"publicKey":"` + e2ee.EncodeKey(make([]byte, 32)) + `"}`)
	if code, _ := do("POST", "/api/p2p/approve-confirm", spoof, nil); code != 400 {
		t.Errorf("unsolicited approve-confirm -> %d, want 400", code)
	}
}

// proxy sits between the Switch and the PC so the network can be made to fail.
type proxy struct {
	srv  *httptest.Server
	mu   sync.Mutex
	mode string // "", "truncate", "tamper"
	hits int
}

func newProxy(t *testing.T, pcAddr string) *proxy {
	t.Helper()
	p := &proxy{}
	client := &http.Client{Timeout: 30 * time.Second}
	p.srv = httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		body, _ := io.ReadAll(r.Body)
		req, _ := http.NewRequest(r.Method, "http://"+pcAddr+r.URL.RequestURI(), bytes.NewReader(body))
		req.Header = r.Header.Clone()
		resp, err := client.Do(req)
		if err != nil {
			http.Error(w, err.Error(), 502)
			return
		}
		defer resp.Body.Close()
		raw, _ := io.ReadAll(resp.Body)
		p.mu.Lock()
		mode := p.mode
		p.mu.Unlock()
		isBlocks := strings.Contains(r.URL.Path, "/blocks/")
		switch {
		case isBlocks && mode == "truncate":
			p.mu.Lock()
			p.hits++
			p.mu.Unlock()
			// Promise the whole body, deliver part of it, hang up.
			hj := w.(http.Hijacker)
			conn, buf, _ := hj.Hijack()
			fmt.Fprintf(buf, "HTTP/1.1 200 OK\r\nContent-Length: %d\r\n\r\n", len(raw))
			buf.Write(raw[:len(raw)/2])
			buf.Flush()
			conn.Close()
			return
		case isBlocks && mode == "tamper":
			p.mu.Lock()
			p.hits++
			p.mu.Unlock()
			var doc struct {
				Blocks []map[string]any `json:"blocks"`
			}
			if json.Unmarshal(raw, &doc) == nil && len(doc.Blocks) > 0 {
				data, _ := base64.StdEncoding.DecodeString(doc.Blocks[0]["data"].(string))
				if len(data) > 0 {
					data[0] ^= 0xFF // one flipped byte, as a bad cable or an attacker might
				}
				doc.Blocks[0]["data"] = base64.StdEncoding.EncodeToString(data)
				raw, _ = json.Marshal(doc)
			}
		}
		for k, vs := range resp.Header {
			if strings.EqualFold(k, "Content-Length") || strings.EqualFold(k, "Transfer-Encoding") {
				continue
			}
			w.Header()[k] = vs
		}
		w.WriteHeader(resp.StatusCode)
		w.Write(raw)
	}))
	t.Cleanup(p.srv.Close)
	return p
}

func (p *proxy) set(mode string) {
	p.mu.Lock()
	p.mode = mode
	p.mu.Unlock()
}

// snapshot reads every file under dir, so "the save did not change" is checked
// against everything, not one file.
func snapshot(t *testing.T, dir string) map[string]string {
	t.Helper()
	out := map[string]string{}
	filepath.Walk(dir, func(p string, info os.FileInfo, err error) error {
		if err == nil && !info.IsDir() {
			raw, _ := os.ReadFile(p)
			rel, _ := filepath.Rel(dir, p)
			out[filepath.ToSlash(rel)] = string(raw)
		}
		return nil
	})
	return out
}

func sameSnapshot(a, b map[string]string) bool {
	if len(a) != len(b) {
		return false
	}
	for k, v := range a {
		if b[k] != v {
			return false
		}
	}
	return true
}

// TestFailedTransfersLeaveTheSaveAlone is the safety property: whatever goes
// wrong on the way, the save on the Switch is exactly what it was.
func TestFailedTransfersLeaveTheSaveAlone(t *testing.T) {
	s := newPC(t)
	sw := newSwitch(t)
	px := newProxy(t, s.pc.Addr)

	// Pair normally, then route the Switch's calls to the PC through the
	// proxy: the Switch keeps the address and port the PC gave when it
	// confirmed, so the path to it is changed where the Switch stores it.
	pair(t, sw, s)
	raw, err := os.ReadFile(sw.state)
	if err != nil {
		t.Fatal(err)
	}
	var st map[string]any
	if err := json.Unmarshal(raw, &st); err != nil {
		t.Fatal(err)
	}
	peers := st["peers"].([]any)
	peers[0].(map[string]any)["port"] = px.srv.Listener.Addr().(*net.TCPAddr).Port
	raw, _ = json.Marshal(st)
	if err := os.WriteFile(sw.state, raw, 0o666); err != nil {
		t.Fatal(err)
	}

	sw.write("main", "Switch main save, chapter 2")
	sw.write("slot/1.dat", "old slot one")
	sw.write("obsolete.dat", "only on the Switch")
	before := snapshot(t, sw.titleDir())

	for _, mode := range []string{"truncate", "tamper"} {
		px.set(mode)
		out := sw.run(90*time.Second, "pull", title, "1")
		if !strings.Contains(out, "PULL FAIL") {
			t.Fatalf("%s: the pull should have failed, got:\n%s", mode, out)
		}
		switch mode {
		case "truncate":
			if !strings.Contains(out, "mid-reply") && !strings.Contains(out, "closed") {
				t.Errorf("truncated reply was not reported as such: %s", out)
			}
		case "tamper":
			if !strings.Contains(out, "does not match the hash") {
				t.Errorf("a tampered block was not caught by its hash: %s", out)
			}
		}
		if px.hits == 0 {
			t.Fatalf("%s: the proxy never saw a blocks request, so nothing was tested", mode)
		}
		if after := snapshot(t, sw.titleDir()); !sameSnapshot(before, after) {
			t.Fatalf("%s: the save changed although the pull failed:\nbefore %v\nafter  %v", mode, before, after)
		}
		if _, err := os.Stat(filepath.Join(sw.work, "staging", title)); err == nil {
			t.Errorf("%s: staging was left behind", mode)
		}
	}

	// And with the network back, the same pull succeeds.
	px.set("")
	out := sw.run(90*time.Second, "pull", title, "1")
	if !strings.Contains(out, "PULL OK same=0") {
		t.Fatalf("pull after the network recovered: %s", out)
	}
	if got, _ := sw.read("main"); got != "PC main save, chapter 5" {
		t.Errorf("main = %q", got)
	}
}

// TestRestoreBackup: the backup a pull makes can be put back, exactly.
func TestRestoreBackup(t *testing.T) {
	s := newPC(t)
	sw := newSwitch(t)
	pair(t, sw, s)
	sw.write("main", "my own progress")
	sw.write("slot/1.dat", "my slot")
	sw.write("mine-only.dat", "only here")
	sw.write(".hidden", "kept too")
	before := snapshot(t, sw.titleDir())

	if out := sw.run(60*time.Second, "pull", title, "1"); !strings.Contains(out, "PULL OK same=0") {
		t.Fatalf("pull: %s", out)
	}
	if sameSnapshot(before, snapshot(t, sw.titleDir())) {
		t.Fatal("the pull changed nothing")
	}
	if out := sw.run(30*time.Second, "restore", title); !strings.Contains(out, "RESTORE OK") {
		t.Fatalf("restore: %s", out)
	}
	if after := snapshot(t, sw.titleDir()); !sameSnapshot(before, after) {
		t.Fatalf("the restored save is not what was backed up:\nbefore %v\nafter  %v", before, after)
	}
}

// TestPullRefusesToEmptyTheSave: an empty PC folder is not an instruction to
// wipe the Switch.
func TestPullRefusesToEmptyTheSave(t *testing.T) {
	s := newPC(t)
	sw := newSwitch(t)
	pair(t, sw, s)
	sw.write("main", "precious progress")
	for _, rel := range []string{"main", "slot/1.dat", "slot/2.dat"} {
		s.pc.RemoveSave(rel)
	}
	out := sw.run(60*time.Second, "pull", title, "1")
	// Either side may stop it: the PC holds an emptied game back, or the
	// Switch declines. What must not happen is the save being replaced.
	if !strings.Contains(out, "PULL FAIL") {
		t.Fatalf("an empty PC save was accepted:\n%s", out)
	}
	t.Logf("refused with: %s", strings.TrimSpace(out))
	if got, _ := sw.read("main"); got != "precious progress" {
		t.Fatalf("the Switch's save was damaged: %q", got)
	}
}

// TestUnpairFromThePC: when the PC unpairs, the Switch drops it.
func TestUnpairFromThePC(t *testing.T) {
	s := newPC(t)
	sw := newSwitch(t)
	pair(t, sw, s)
	p := sw.start("serve", "20")
	p.waitFor("LISTENING", 10*time.Second)
	s.pc.API(http.MethodDelete, "/api/peers/"+switchID(t, s), nil, nil)
	p.waitFor("UNPAIRED", 20*time.Second)
	// Free the Switch's fixed port before the next command wants it.
	p.cmd.Process.Kill()
	<-p.done
	out := sw.run(15*time.Second, "compare", title)
	if !strings.Contains(out, "not paired") {
		t.Fatalf("the Switch still thinks it is paired: %s", out)
	}
}

// TestPCInitiatedPairing: the PC adds the Switch by address; the person at the
// console approves.
func TestPCInitiatedPairing(t *testing.T) {
	s := newPC(t)
	sw := newSwitch(t)
	p := sw.start("--auto-approve", "serve", "40")
	p.waitFor("LISTENING", 10*time.Second)
	s.pc.API(http.MethodPost, "/api/peers/pair", map[string]any{"address": "127.0.0.1", "port": sw.port}, nil)
	line := p.waitFor("PAIRED", 30*time.Second)
	if !testutil.WaitFor(20*time.Second, func() bool {
		peers, _ := s.pc.Daemon.Store.ListPeers()
		return len(peers) == 1 && peers[0].PublicKey != ""
	}) {
		t.Fatal("the PC did not record the Switch as paired")
	}
	peers, _ := s.pc.Daemon.Store.ListPeers()
	fp, _ := s.pc.Daemon.Store.PairingFingerprint(peers[0].ID)
	if field(line, "fingerprint") != fp {
		t.Errorf("fingerprints differ: %q vs %q", field(line, "fingerprint"), fp)
	}
}

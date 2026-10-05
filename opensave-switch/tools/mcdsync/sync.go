package main

import (
	"bytes"
	"context"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"
	"sort"
	"strings"
	"time"
)

const (
	switchPrefix = "Character"
	stateName    = ".mcdsync-state.json" // dot-files are never synced by OpenSave
	backupName   = ".mcdsync-backup"
	keepBackups  = 5
)

// pairState is what both files held when they were last known to agree. It is
// what tells "the Windows copy changed" from "the Switch copy changed", and so
// what makes a conflict detectable instead of a coin toss.
type pairState struct {
	Win string `json:"win"` // sha256 of the Windows file as it is on disk
	Sw  string `json:"sw"`  // sha256 of the Switch file
}

type syncer struct {
	winDir, swDir string
	svc           service
	settle        time.Duration // a file touched more recently than this is left alone
	now           func() time.Time
	logf          func(format string, args ...any)
	noted         map[string]bool // files already reported as not being characters
}

func sum(b []byte) string {
	h := sha256.Sum256(b)
	return hex.EncodeToString(h[:])
}

func (s *syncer) statePath() string { return filepath.Join(s.swDir, stateName) }

func (s *syncer) loadState() map[string]pairState {
	st := map[string]pairState{}
	if raw, err := os.ReadFile(s.statePath()); err == nil {
		_ = json.Unmarshal(raw, &st) // damaged state only costs a re-check, never a write
	}
	return st
}

func (s *syncer) saveState(st map[string]pairState) error {
	raw, _ := json.MarshalIndent(st, "", "  ")
	return writeAtomic(s.statePath(), raw)
}

// writeAtomic writes beside the target and moves into place, so a crash leaves
// the old file or the new one, never half of either.
func writeAtomic(path string, data []byte) error {
	tmp := path + ".mcdsync.tmp"
	if err := os.WriteFile(tmp, data, 0o666); err != nil {
		return err
	}
	if err := os.Rename(tmp, path); err != nil {
		os.Remove(tmp)
		return err
	}
	return nil
}

// stable reports whether a file has been left alone long enough to be read: a
// game or a sync in the middle of writing it is not something to copy.
func (s *syncer) stable(path string) bool {
	info, err := os.Stat(path)
	return err == nil && s.now().Sub(info.ModTime()) >= s.settle
}

// sameJSON compares two documents ignoring formatting.
func sameJSON(a, b []byte) bool {
	var ca, cb bytes.Buffer
	if json.Compact(&ca, a) == nil && json.Compact(&cb, b) == nil {
		return bytes.Equal(ca.Bytes(), cb.Bytes())
	}
	return bytes.Equal(a, b)
}

// isCharacter reports whether decrypted data is a character save, as opposed
// to some other encrypted file living beside it.
func isCharacter(plain []byte) bool {
	var m map[string]json.RawMessage
	if json.Unmarshal(plain, &m) != nil {
		return false
	}
	_, a := m["uniqueSaveId"]
	_, b := m["playerId"]
	return a || b
}

func isWindowsSave(raw []byte) bool { return bytes.HasPrefix(raw, magic) && len(raw) > len(magic) }

// backup copies a Windows file aside before it is replaced, keeping the newest few.
func (s *syncer) backup(path string, raw []byte) error {
	dir := filepath.Join(s.winDir, backupName)
	if err := os.MkdirAll(dir, 0o777); err != nil {
		return err
	}
	base := strings.TrimSuffix(filepath.Base(path), filepath.Ext(path))
	if err := os.WriteFile(filepath.Join(dir, fmt.Sprintf("%s.%d.dat", base, s.now().Unix())), raw, 0o666); err != nil {
		return err
	}
	old, _ := filepath.Glob(filepath.Join(dir, base+".*.dat"))
	sort.Strings(old)
	for len(old) > keepBackups {
		os.Remove(old[0])
		old = old[1:]
	}
	return nil
}

// toSwitch decrypts a Windows save and writes the Switch file.
func (s *syncer) toSwitch(ctx context.Context, guid string, winRaw []byte, swPath string) ([]byte, error) {
	plain, err := s.svc.Decrypt(ctx, winRaw[len(magic):])
	if err != nil {
		return nil, fmt.Errorf("decrypt: %w", err)
	}
	if !isCharacter(plain) {
		return nil, errNotCharacter
	}
	if err := writeAtomic(swPath, plain); err != nil {
		return nil, err
	}
	return plain, nil
}

var errNotCharacter = fmt.Errorf("not a character save")

// toWindows encrypts a Switch save and writes the Windows file — but only after
// proving the result: the encrypted bytes are decrypted again and must give
// back exactly what was put in. A file the game could not read is worse than no
// file, and this is the one write that lands in the game's own folder.
func (s *syncer) toWindows(ctx context.Context, swRaw []byte, winPath string, oldWin []byte) ([]byte, error) {
	if !isCharacter(swRaw) {
		return nil, errNotCharacter
	}
	enc, err := s.svc.Encrypt(ctx, swRaw)
	if err != nil {
		return nil, fmt.Errorf("encrypt: %w", err)
	}
	if len(enc) == 0 || len(enc)%16 != 0 {
		return nil, fmt.Errorf("the service returned data that is not an encrypted save")
	}
	back, err := s.svc.Decrypt(ctx, enc)
	if err != nil {
		return nil, fmt.Errorf("verify: %w", err)
	}
	if !bytes.Equal(back, swRaw) {
		return nil, fmt.Errorf("the encrypted save did not decrypt back to the original, so it was not written")
	}
	out := append(append([]byte{}, magic...), enc...)
	if oldWin != nil {
		if err := s.backup(winPath, oldWin); err != nil {
			return nil, fmt.Errorf("could not back up the Windows save, so it was not replaced: %w", err)
		}
	}
	if err := writeAtomic(winPath, out); err != nil {
		return nil, err
	}
	return out, nil
}

// runOnce looks at every character on either side and brings the two together
// where exactly one has changed. It never deletes, and never overwrites when
// both changed.
func (s *syncer) runOnce(ctx context.Context) error {
	if err := os.MkdirAll(s.swDir, 0o777); err != nil {
		return err
	}
	st := s.loadState()
	dirty := false

	guids := map[string]bool{}
	if ents, err := os.ReadDir(s.winDir); err == nil {
		for _, e := range ents {
			if !e.IsDir() && strings.EqualFold(filepath.Ext(e.Name()), ".dat") {
				guids[strings.TrimSuffix(e.Name(), filepath.Ext(e.Name()))] = true
			}
		}
	} else {
		return fmt.Errorf("cannot read the Windows folder: %w", err)
	}
	if ents, err := os.ReadDir(s.swDir); err == nil {
		for _, e := range ents {
			if !e.IsDir() && strings.HasPrefix(e.Name(), switchPrefix) && !strings.Contains(e.Name(), ".") {
				guids[strings.TrimPrefix(e.Name(), switchPrefix)] = true
			}
		}
	}
	ordered := make([]string, 0, len(guids))
	for g := range guids {
		ordered = append(ordered, g)
	}
	sort.Strings(ordered)

	for _, guid := range ordered {
		if ctx.Err() != nil {
			break
		}
		winPath := filepath.Join(s.winDir, guid+".dat")
		swPath := filepath.Join(s.swDir, switchPrefix+guid)
		winRaw, winErr := os.ReadFile(winPath)
		swRaw, swErr := os.ReadFile(swPath)
		hasW, hasS := winErr == nil, swErr == nil
		if hasW && !isWindowsSave(winRaw) {
			continue // some other .dat: not ours to touch
		}
		if (hasW && !s.stable(winPath)) || (hasS && !s.stable(swPath)) {
			continue // being written; look again next time
		}
		prev, known := st[guid]
		var newW, newS []byte
		var err error

		switch {
		case hasW && !hasS:
			if known {
				s.logf("%s: deleted on the Switch side; not recreating it", guid)
				continue
			}
			if newS, err = s.toSwitch(ctx, guid, winRaw, swPath); err == nil {
				newW = winRaw
				s.logf("%s: Windows -> Switch (new)", guid)
			}
		case hasS && !hasW:
			if known {
				s.logf("%s: deleted on the Windows side; not recreating it", guid)
				continue
			}
			if newW, err = s.toWindows(ctx, swRaw, winPath, nil); err == nil {
				newS = swRaw
				s.logf("%s: Switch -> Windows (new)", guid)
			}
		case hasW && hasS:
			winChanged := !known || sum(winRaw) != prev.Win
			swChanged := !known || sum(swRaw) != prev.Sw
			switch {
			case !winChanged && !swChanged:
				continue
			case !known:
				// First time both are seen: only agree if they already match.
				plain, derr := s.svc.Decrypt(ctx, winRaw[len(magic):])
				if derr != nil {
					err = fmt.Errorf("decrypt: %w", derr)
				} else if sameJSON(plain, swRaw) {
					newW, newS = winRaw, swRaw
					s.logf("%s: already the same on both sides", guid)
				} else {
					s.logf("%s: CONFLICT — both sides have this character and they differ. Nothing was changed. "+
						"Delete the copy you do not want, and it will be replaced from the other.", guid)
					continue
				}
			case winChanged && swChanged:
				s.logf("%s: CONFLICT — changed on both sides since they last matched. Nothing was changed. "+
					"Delete the copy you do not want, and it will be replaced from the other.", guid)
				continue
			case winChanged:
				if newS, err = s.toSwitch(ctx, guid, winRaw, swPath); err == nil {
					newW = winRaw
					s.logf("%s: Windows -> Switch", guid)
				}
			default:
				if newW, err = s.toWindows(ctx, swRaw, winPath, winRaw); err == nil {
					newS = swRaw
					s.logf("%s: Switch -> Windows (previous Windows save backed up)", guid)
				}
			}
		}
		if err != nil {
			if err == errNotCharacter {
				// A different kind of file (a profile, say): left alone, and said
				// once so that "nothing happens" is not a mystery.
				if s.noted == nil {
					s.noted = map[string]bool{}
				}
				if !s.noted[guid] {
					s.noted[guid] = true
					s.logf("%s: not a character save (no playerId or uniqueSaveId), so it was left alone", guid)
				}
				continue
			}
			s.logf("%s: skipped — %v", guid, err)
			continue
		}
		if newW != nil && newS != nil {
			// The state records what is on disk now, so the next pass sees no change.
			if cur, e := os.ReadFile(winPath); e == nil {
				newW = cur
			}
			if cur, e := os.ReadFile(swPath); e == nil {
				newS = cur
			}
			st[guid] = pairState{Win: sum(newW), Sw: sum(newS)}
			dirty = true
		}
	}
	if dirty {
		return s.saveState(st)
	}
	return nil
}

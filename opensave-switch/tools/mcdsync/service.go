package main

import (
	"bytes"
	"context"
	"encoding/base64"
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"strings"
	"time"
)

// A save is encrypted for Windows and plain JSON for the Switch. The Windows
// file is "D001", four zero bytes, then the encrypted JSON; the Switch file is
// the JSON itself with no extension. The key is not published, so encrypting
// and decrypting is done by the service MCDSaveEdit uses (see service below).
var magic = []byte("D001\x00\x00\x00\x00")

// maxBody is the service's own request limit (5,000,000 characters of base64).
const maxBody = 3_000_000

// service turns the encrypted part of a Windows save into JSON and back. The
// encrypted bytes passed in and returned never include the D001 header.
type service interface {
	Decrypt(ctx context.Context, encrypted []byte) ([]byte, error)
	Encrypt(ctx context.Context, plain []byte) ([]byte, error)
}

// httpService is the dungeons.tools protocol, as DungeonTools'
// RemoteEncryptionProvider speaks it: POST {"Encrypted": base64} to
// api/encryption/decrypt and read {"Decrypted": base64}, and the reverse for
// api/encryption/encrypt.
type httpService struct {
	base   string
	client *http.Client
}

func newHTTPService(base string) *httpService {
	if !strings.HasSuffix(base, "/") {
		base += "/"
	}
	return &httpService{base: base, client: &http.Client{Timeout: 30 * time.Second}}
}

type payload struct {
	Encrypted string `json:"encrypted,omitempty"`
	Decrypted string `json:"decrypted,omitempty"`
}

func (s *httpService) call(ctx context.Context, endpoint string, in payload) (payload, error) {
	var out payload
	body, _ := json.Marshal(map[string]string{"Encrypted": in.Encrypted, "Decrypted": in.Decrypted})
	req, err := http.NewRequestWithContext(ctx, http.MethodPost, s.base+"api/encryption/"+endpoint, bytes.NewReader(body))
	if err != nil {
		return out, err
	}
	req.Header.Set("Content-Type", "application/json")
	resp, err := s.client.Do(req)
	if err != nil {
		return out, err
	}
	defer resp.Body.Close()
	raw, err := io.ReadAll(io.LimitReader(resp.Body, 8_000_000))
	if err != nil {
		return out, err
	}
	if resp.StatusCode != http.StatusOK {
		return out, fmt.Errorf("the service answered HTTP %d", resp.StatusCode)
	}
	if err := json.Unmarshal(raw, &out); err != nil {
		return out, fmt.Errorf("the service sent an unreadable reply: %w", err)
	}
	return out, nil
}

// decodeB64 reads base64 with or without its padding, which the service omits.
func decodeB64(s string) ([]byte, error) {
	return base64.RawStdEncoding.DecodeString(strings.TrimRight(strings.TrimSpace(s), "="))
}

func (s *httpService) Decrypt(ctx context.Context, encrypted []byte) ([]byte, error) {
	if len(encrypted) == 0 || len(encrypted) > maxBody {
		return nil, fmt.Errorf("a save of %d bytes cannot be sent", len(encrypted))
	}
	out, err := s.call(ctx, "decrypt", payload{Encrypted: base64.StdEncoding.EncodeToString(encrypted)})
	if err != nil {
		return nil, err
	}
	if out.Decrypted == "" {
		return nil, fmt.Errorf("the service returned nothing")
	}
	return decodeB64(out.Decrypted)
}

func (s *httpService) Encrypt(ctx context.Context, plain []byte) ([]byte, error) {
	if len(plain) == 0 || len(plain) > maxBody {
		return nil, fmt.Errorf("a save of %d bytes cannot be sent", len(plain))
	}
	out, err := s.call(ctx, "encrypt", payload{Decrypted: base64.StdEncoding.EncodeToString(plain)})
	if err != nil {
		return nil, err
	}
	if out.Encrypted == "" {
		return nil, fmt.Errorf("the service returned nothing")
	}
	return decodeB64(out.Encrypted)
}

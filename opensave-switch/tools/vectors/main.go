// Command vectors prints known-answer test vectors for the Switch client's C
// crypto, computed by the real Go implementation in internal/e2ee, so the two
// cannot drift apart unnoticed. Output is one "kind field field ..." line per
// vector, all binary fields hex-encoded.
package main

import (
	"crypto/hmac"
	"crypto/rand"
	"crypto/sha256"
	"encoding/base64"
	"encoding/hex"
	"fmt"
	"strings"

	"github.com/opensave/opensave/internal/e2ee"
	"golang.org/x/crypto/curve25519"
)

// hx hex-encodes b, writing "-" for no bytes so a field is never empty and
// the line still splits cleanly on spaces.
func hx(b []byte) string {
	if len(b) == 0 {
		return "-"
	}
	return hex.EncodeToString(b)
}

func rnd(n int) []byte {
	b := make([]byte, n)
	if _, err := rand.Read(b); err != nil {
		panic(err)
	}
	return b
}

func main() {
	for i := 0; i < 200; i++ {
		n := i * 7 % 300
		msg := rnd(n)
		sum := sha256.Sum256(msg)
		fmt.Printf("sha256 %s %s\n", hx(msg), hex.EncodeToString(sum[:]))
		enc := base64.StdEncoding.EncodeToString(msg)
		if enc == "" {
			enc = "-"
		}
		fmt.Printf("b64 %s %s\n", hx(msg), enc)
	}
	for i := 0; i < 100; i++ {
		key := rnd(1 + i*3%150) // exercises keys shorter and longer than a block
		msg := rnd(i * 11 % 400)
		m := hmac.New(sha256.New, key)
		m.Write(msg)
		fmt.Printf("hmac %s %s %s\n", hex.EncodeToString(key), hx(msg), hex.EncodeToString(m.Sum(nil)))
	}
	for i := 0; i < 100; i++ {
		a, _ := e2ee.GenerateIdentity()
		b, _ := e2ee.GenerateIdentity()
		k, err := e2ee.AuthKey(a.Private, b.Public)
		if err != nil {
			panic(err)
		}
		k2, _ := e2ee.AuthKey(b.Private, a.Public)
		if hex.EncodeToString(k) != hex.EncodeToString(k2) {
			panic("auth keys disagree")
		}
		pub, _ := curve25519.X25519(a.Private, curve25519.Basepoint)
		fmt.Printf("authkey %s %s %s %s\n", hex.EncodeToString(a.Private), hex.EncodeToString(pub),
			hex.EncodeToString(b.Public), hex.EncodeToString(k))
	}
	for i := 0; i < 50; i++ {
		a, _ := e2ee.GenerateIdentity()
		b, _ := e2ee.GenerateIdentity()
		if i == 0 {
			b = a // identical keys
		}
		fmt.Printf("fingerprint %s %s %s\n", hex.EncodeToString(a.Public), hex.EncodeToString(b.Public),
			strings.ReplaceAll(e2ee.Fingerprint(a.Public, b.Public), " ", "_"))
	}
	// Request/response MACs with awkward field splits and non-ASCII.
	for i := 0; i < 60; i++ {
		key := rnd(32)
		body := rnd(i * 13 % 500)
		from, to := "node-"+fmt.Sprint(i), "ünï-peer-"+fmt.Sprint(i*3)
		route := fmt.Sprintf("/api/p2p/manifest/switch-0100f2c0115b6000?name=a%%20b&n=%d", i)
		nonce := hex.EncodeToString(rnd(16))
		ms := int64(1_700_000_000_000 + i*977)
		fmt.Printf("reqmac %s %s %s %s %s %s %s %d %s\n", hex.EncodeToString(key), from, to, route, "GET", hx(body), nonce, ms,
			e2ee.RequestMAC(key, from, to, route, "GET", body, nonce, ms))
		fmt.Printf("respmac %s %s %s %s %d %s %s %d %s\n", hex.EncodeToString(key), from, to, "msg-"+fmt.Sprint(i), 200, hx(body), nonce, ms,
			e2ee.ResponseMAC(key, from, to, "msg-"+fmt.Sprint(i), 200, body, nonce, ms))
	}
	// RFC 7748 section 5.2 vectors.
	for _, v := range [][3]string{
		{"a546e36bf0527c9d3b16154b82465edd62144c0ac1fc5a18506a2244ba449ac4", "e6db6867583030db3594c1a424b15f7c726624ec26b3353b10a903a6d0ab1c4c", "c3da55379de9c6908e94ea4df28d084f32eccf03491c71f754b4075577a28552"},
		{"4b66e9d4d1b4673c5ad22691957d6af5c11b6421e0ea01d42ca4169e7918ba0d", "e5210f12786811d3f4b7959d0538ae2c31dbe7106fc03c3efc4cd549c715a493", "95cbde9476e8907d7aade45cb4b873f88b595a68799fa152e6f8f7647aac7957"},
	} {
		fmt.Printf("x25519 %s %s %s\n", v[0], v[1], v[2])
	}
	// Low-order point: must be refused.
	fmt.Printf("x25519zero %s %s\n", hex.EncodeToString(rnd(32)), "0000000000000000000000000000000000000000000000000000000000000000")
}

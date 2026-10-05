// Command gomanifest exposes the Go manifest code to the Switch client's tests,
// so the C implementation is checked against the real thing rather than
// against a second reading of it.
//
//	gomanifest build <dir>    prints {"manifest": <delta.Manifest>, "hash": "<ManifestHash>"}
//	gomanifest hash           reads a delta.Manifest as JSON on stdin and prints its ManifestHash
package main

import (
	"encoding/json"
	"fmt"
	"io"
	"os"

	"github.com/opensave/opensave/internal/delta"
)

func main() {
	if len(os.Args) < 2 {
		fmt.Fprintln(os.Stderr, "usage: gomanifest build <dir> | hash")
		os.Exit(2)
	}
	switch os.Args[1] {
	case "build":
		m, err := delta.BuildManifest(os.Args[2])
		if err != nil {
			fmt.Fprintln(os.Stderr, err)
			os.Exit(1)
		}
		out, _ := json.Marshal(map[string]any{"manifest": m, "hash": m.ManifestHash()})
		fmt.Println(string(out))
	case "hash":
		raw, _ := io.ReadAll(os.Stdin)
		var m delta.Manifest
		if err := json.Unmarshal(raw, &m); err != nil {
			fmt.Fprintln(os.Stderr, "Go could not read the manifest:", err)
			os.Exit(1)
		}
		fmt.Println(m.ManifestHash())
	}
}

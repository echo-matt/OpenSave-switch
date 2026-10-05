// Command mcdsync keeps Minecraft Dungeons character saves in step between the
// Windows game (encrypted ".dat" files) and the Nintendo Switch format (plain
// JSON files named "Character<id>"), so OpenSave can carry them to a Switch.
//
//	mcdsync -yes -windows "%LOCALAPPDATA%\Dungeons\Saved\<profile>\Characters" -switch <switch save folder>
//
// Point -switch at the folder OpenSave syncs to the console (a Switch save laid
// out as an emulator keeps it). Encryption is done by the dungeons.tools
// service, the same one MCDSaveEdit uses: the save's JSON is sent to it, over
// HTTPS, each time a file changes.
package main

import (
	"context"
	"flag"
	"fmt"
	"log"
	"os"
	"os/signal"
	"path/filepath"
	"time"
)

const notice = `mcdsync sends your character saves (the game progress JSON, nothing else) to
%s to encrypt and decrypt them, because the encryption key is not public. Only
the save files are sent, and only when one changes. Pass -yes to agree.`

func main() {
	win := flag.String("windows", "auto", `folder holding the Windows character .dat files, or "auto" to find it under %LOCALAPPDATA%\Dungeons\Saved`)
	sw := flag.String("switch", "", "folder holding (or to hold) the Switch-format Character<id> files")
	every := flag.Duration("interval", 5*time.Second, "how often to look for changes (0: once and exit)")
	svcURL := flag.String("service", "https://dungeons.tools/", "encryption service")
	yes := flag.Bool("yes", false, "agree to send saves to the service")
	settle := flag.Duration("settle", 3*time.Second, "leave a file alone until it has been still this long")
	flag.Parse()

	if *sw == "" {
		flag.Usage()
		os.Exit(2)
	}
	if *win == "auto" || *win == "" {
		root := filepath.Join(os.Getenv("LOCALAPPDATA"), "Dungeons", "Saved")
		dir, err := findWindowsFolder(root)
		if err != nil {
			fmt.Fprintf(os.Stderr, "could not find the Windows saves: %v\nPass -windows with the folder that holds the .dat files.\n", err)
			os.Exit(2)
		}
		log.Printf("using the Windows saves in %s", dir)
		*win = dir
	}
	if !*yes {
		fmt.Fprintf(os.Stderr, notice+"\n", *svcURL)
		os.Exit(2)
	}
	s := &syncer{winDir: *win, swDir: *sw, svc: newHTTPService(*svcURL), settle: *settle, now: time.Now,
		logf: func(f string, a ...any) { log.Printf(f, a...) }}

	ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt)
	defer stop()
	for {
		if err := s.runOnce(ctx); err != nil {
			log.Printf("sync pass failed: %v", err)
		}
		if *every <= 0 {
			return
		}
		select {
		case <-ctx.Done():
			return
		case <-time.After(*every):
		}
	}
}

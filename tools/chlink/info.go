package main

import (
	"encoding/json"
	"flag"
	"fmt"
	"os"
)

func cmdInfo(args []string) error {
	fs := flag.NewFlagSet("info", flag.ExitOnError)
	c := addCommon(fs)
	to := fs.String("to", "", "receiver address ip[:port]")
	fs.Usage = func() {
		fmt.Fprintln(os.Stderr, "usage: chlink info --to <ip[:port]> [flags]")
		fs.PrintDefaults()
	}
	fs.Parse(args)

	target, err := resolveTarget(*to, c.port)
	if err != nil {
		return err
	}
	ri, err := fetchInfo(newHTTPClient(c.timeout), target)
	if err != nil {
		return err
	}
	if c.jsonOut {
		return json.NewEncoder(os.Stdout).Encode(ri)
	}
	fmt.Printf("device:          %s\n", ri.Device)
	fmt.Printf("version:         %s\n", ri.Version)
	if ri.MaxUploadBytes == 0 {
		fmt.Printf("maxUploadBytes:  0 (unlimited or unknown; consoles currently cap uploads at 32 MiB)\n")
	} else {
		fmt.Printf("maxUploadBytes:  %d (%s)\n", ri.MaxUploadBytes, humanBytes(ri.MaxUploadBytes))
	}
	fmt.Printf("freeSpaceBytes:  %d\n", ri.FreeSpaceBytes)
	return nil
}

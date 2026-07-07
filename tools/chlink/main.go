// chlink — companion CLI for Checkpoint's wireless save transfer.
//
// Speaks the exact HTTP/multipart/store-zip protocol implemented by the
// console targets (see 3ds/source/transfer.cpp), acting as sender or
// receiver, plus offline zip/unzip helpers.
package main

import (
	"fmt"
	"os"
)

// version is injected at build time via -ldflags "-X main.version=...".
var version = "dev"

const defaultPort = 8000

func usage() {
	fmt.Fprintf(os.Stderr, `chlink %s — Checkpoint wireless transfer companion

Usage:
  chlink send <path> --to <ip[:port]> --pin <PIN> [flags]
  chlink receive [flags]
  chlink info --to <ip[:port]> [flags]
  chlink zip <folder> [-o out.zip] [--store|--deflate]
  chlink unzip <archive.zip> [-o dir]
  chlink version

Run 'chlink <command> -h' for command flags.
`, version)
}

func main() {
	if len(os.Args) < 2 {
		usage()
		os.Exit(2)
	}
	cmd, args := os.Args[1], os.Args[2:]
	var err error
	switch cmd {
	case "send":
		err = cmdSend(args)
	case "receive":
		err = cmdReceive(args)
	case "info":
		err = cmdInfo(args)
	case "zip":
		err = cmdZip(args)
	case "unzip":
		err = cmdUnzip(args)
	case "version", "--version", "-v":
		fmt.Printf("chlink %s\n", version)
	case "help", "-h", "--help":
		usage()
	default:
		fmt.Fprintf(os.Stderr, "unknown command %q\n\n", cmd)
		usage()
		os.Exit(2)
	}
	if err != nil {
		fmt.Fprintln(os.Stderr, "error:", err)
		os.Exit(1)
	}
}

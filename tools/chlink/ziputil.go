package main

// Offline zip/unzip helpers using the same store-mode writer/extractor the
// network paths use, so archives can be prepared/inspected without a peer.

import (
	"archive/zip"
	"flag"
	"fmt"
	"io"
	"io/fs"
	"os"
	"path/filepath"
	"strings"
)

func cmdZip(args []string) error {
	fs := flag.NewFlagSet("zip", flag.ExitOnError)
	out := fs.String("o", "", "output archive (default <folder>.zip)")
	store := fs.Bool("store", true, "store-mode, console-compatible (default)")
	deflate := fs.Bool("deflate", false, "deflate compression (PC-only archives)")
	fs.Usage = func() {
		fmt.Fprintln(os.Stderr, "usage: chlink zip <folder> [-o out.zip] [--store|--deflate]")
		fs.PrintDefaults()
	}
	folder, err := parseOnePositional(fs, args, "<folder>")
	if err != nil {
		return err
	}
	info, err := os.Stat(folder)
	if err != nil {
		return err
	}
	if !info.IsDir() {
		return fmt.Errorf("%s is not a folder", folder)
	}

	dest := *out
	if dest == "" {
		dest = filepath.Base(filepath.Clean(folder)) + ".zip"
	}

	f, err := os.Create(dest)
	if err != nil {
		return err
	}
	success := false
	defer func() {
		f.Close()
		if !success {
			os.Remove(dest)
		}
	}()

	var size int64
	if *deflate {
		size, err = writeDeflateZip(f, folder)
	} else {
		_ = *store // store is the default; --deflate overrides it
		size, err = WriteStoreZip(f, folder, nil)
	}
	if err != nil {
		return err
	}
	if err := f.Close(); err != nil {
		return err
	}
	success = true
	mode := "store"
	if *deflate {
		mode = "deflate"
	}
	fmt.Printf("wrote %s (%s, %s)\n", dest, humanBytes(size), mode)
	return nil
}

func writeDeflateZip(w io.Writer, root string) (int64, error) {
	cw := &countingWriter{w: w}
	zw := zip.NewWriter(cw)
	err := filepath.WalkDir(root, func(path string, d fs.DirEntry, err error) error {
		if err != nil || path == root {
			return err
		}
		rel, err := filepath.Rel(root, path)
		if err != nil {
			return err
		}
		rel = filepath.ToSlash(rel)
		if d.IsDir() {
			_, err := zw.CreateHeader(&zip.FileHeader{Name: rel + "/", Method: zip.Store})
			return err
		}
		dst, err := zw.CreateHeader(&zip.FileHeader{Name: rel, Method: zip.Deflate})
		if err != nil {
			return err
		}
		src, err := os.Open(path)
		if err != nil {
			return err
		}
		defer src.Close()
		_, err = io.Copy(dst, src)
		return err
	})
	if err != nil {
		zw.Close()
		return 0, err
	}
	if err := zw.Close(); err != nil {
		return 0, err
	}
	return cw.n, nil
}

type countingWriter struct {
	w io.Writer
	n int64
}

func (c *countingWriter) Write(p []byte) (int, error) {
	n, err := c.w.Write(p)
	c.n += int64(n)
	return n, err
}

func cmdUnzip(args []string) error {
	fs := flag.NewFlagSet("unzip", flag.ExitOnError)
	out := fs.String("o", "", "output directory (default archive name without .zip)")
	verbose := fs.Bool("verbose", false, "verbose output")
	fs.Usage = func() {
		fmt.Fprintln(os.Stderr, "usage: chlink unzip <archive.zip> [-o dir]")
		fs.PrintDefaults()
	}
	archive, err := parseOnePositional(fs, args, "<archive.zip>")
	if err != nil {
		return err
	}

	dest := *out
	if dest == "" {
		base := filepath.Base(archive)
		dest = strings.TrimSuffix(base, filepath.Ext(base))
		if dest == "" || dest == base {
			dest = base + ".extracted"
		}
	}
	if err := os.MkdirAll(dest, 0o755); err != nil {
		return err
	}
	if err := ExtractZip(archive, dest, *verbose); err != nil {
		return err
	}
	fmt.Printf("extracted %s -> %s\n", archive, dest)
	return nil
}

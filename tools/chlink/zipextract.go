package main

import (
	"archive/zip"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"strings"
)

// isSafeZipRelativePath ports the console's traversal check verbatim:
// non-empty, no leading '/' or '\', no '\', no ':', no '..' component.
func isSafeZipRelativePath(rel string) bool {
	if rel == "" {
		return false
	}
	if rel[0] == '/' || rel[0] == '\\' {
		return false
	}
	if strings.ContainsAny(rel, "\\:") {
		return false
	}
	for _, part := range strings.Split(rel, "/") {
		if part == ".." {
			return false
		}
	}
	return true
}

// ExtractZip extracts archive into destDir, rejecting unsafe entry paths.
// Reading uses the stdlib (which verifies each entry's CRC-32 on read), so
// it accepts a superset of what the consoles emit.
func ExtractZip(archive, destDir string, verbose bool) error {
	r, err := zip.OpenReader(archive)
	if err != nil {
		return fmt.Errorf("open zip: %w", err)
	}
	defer r.Close()

	for _, f := range r.File {
		name := f.Name
		if !isSafeZipRelativePath(name) {
			return fmt.Errorf("invalid zip entry path: %q", name)
		}
		target := filepath.Join(destDir, filepath.FromSlash(name))
		if strings.HasSuffix(name, "/") {
			if err := os.MkdirAll(target, 0o755); err != nil {
				return err
			}
			continue
		}
		if err := os.MkdirAll(filepath.Dir(target), 0o755); err != nil {
			return err
		}
		if verbose {
			fmt.Fprintf(os.Stderr, "  extracting %s (%d bytes)\n", name, f.UncompressedSize64)
		}
		rc, err := f.Open()
		if err != nil {
			return fmt.Errorf("read %q: %w", name, err)
		}
		out, err := os.Create(target)
		if err != nil {
			rc.Close()
			return err
		}
		_, cerr := io.Copy(out, rc)
		rc.Close()
		if werr := out.Close(); cerr == nil {
			cerr = werr
		}
		if cerr != nil {
			return fmt.Errorf("extract %q: %w", name, cerr)
		}
	}
	return nil
}

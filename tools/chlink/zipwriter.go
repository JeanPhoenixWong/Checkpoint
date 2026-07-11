package main

// Console-compatible store-mode ZIP writer.
//
// The console extractor (TransferProto::extractZip in common/transferprotocol.cpp)
// requires: method 0 (store), no zip64, directory entries ending in '/', and real
// sizes present in the local header. It now tolerates data descriptors (flag bit 3)
// and skips over extra fields rather than rejecting them. This writer still emits
// the strict minimum — no data descriptors, no extra fields, sizes/CRC backfilled
// into the local header after streaming — so its output stays valid for any
// consumer, not only the current extractor. It is hand-rolled (mirroring the
// console sender) because Go's archive/zip emits data descriptors on some
// CreateHeader paths: local header, then central directory, then EOCD.

import (
	"encoding/binary"
	"fmt"
	"hash/crc32"
	"io"
	"io/fs"
	"os"
	"path/filepath"
	"sort"
	"strings"
)

type zipCentralEntry struct {
	name   string
	crc    uint32
	size   uint32
	offset uint32
	isDir  bool
}

const (
	zipLocalHeaderSig   = 0x04034b50
	zipCentralHeaderSig = 0x02014b50
	zipEOCDSig          = 0x06054b50
	zipVersion          = 20
	zipMaxEntrySize     = 0xFFFFFFFF
)

type zipWriter struct {
	w   io.WriteSeeker
	pos int64
}

func (z *zipWriter) write(p []byte) error {
	n, err := z.w.Write(p)
	z.pos += int64(n)
	return err
}

func (z *zipWriter) le16(v uint16) error {
	var b [2]byte
	binary.LittleEndian.PutUint16(b[:], v)
	return z.write(b[:])
}

func (z *zipWriter) le32(v uint32) error {
	var b [4]byte
	binary.LittleEndian.PutUint32(b[:], v)
	return z.write(b[:])
}

func (z *zipWriter) localHeader(name string, crc, size uint32) error {
	z.le32(zipLocalHeaderSig)
	z.le16(zipVersion)
	z.le16(0) // flags: bit 3 (data descriptor) must stay clear
	z.le16(0) // method: store
	z.le16(0) // mod time
	z.le16(0) // mod date
	z.le32(crc)
	z.le32(size) // compressed
	z.le32(size) // uncompressed
	z.le16(uint16(len(name)))
	z.le16(0) // extra length
	return z.write([]byte(name))
}

// WriteStoreZip packages the contents of root (files and directories,
// paths relative to root, forward slashes) into a console-compatible
// store-mode zip written to w. progress, if non-nil, receives the number of
// payload bytes written so far. Returns the total zip size.
func WriteStoreZip(w io.WriteSeeker, root string, progress func(int64)) (int64, error) {
	type fileItem struct {
		abs   string
		rel   string
		size  uint32
		isDir bool
	}
	var items []fileItem

	err := filepath.WalkDir(root, func(path string, d fs.DirEntry, err error) error {
		if err != nil {
			return err
		}
		if path == root {
			return nil
		}
		rel, err := filepath.Rel(root, path)
		if err != nil {
			return err
		}
		rel = filepath.ToSlash(rel)
		if len(rel) > 0xFFFF {
			return fmt.Errorf("entry name too long: %s", rel)
		}
		if d.IsDir() {
			items = append(items, fileItem{abs: path, rel: rel + "/", isDir: true})
			return nil
		}
		if !d.Type().IsRegular() {
			return fmt.Errorf("unsupported file type: %s", rel)
		}
		info, err := d.Info()
		if err != nil {
			return err
		}
		if info.Size() > zipMaxEntrySize {
			return fmt.Errorf("file too large for zip (no zip64): %s", rel)
		}
		items = append(items, fileItem{abs: path, rel: rel, size: uint32(info.Size())})
		return nil
	})
	if err != nil {
		return 0, err
	}
	if len(items) == 0 {
		return 0, fmt.Errorf("no files or folders found to package")
	}
	// WalkDir is already deterministic (lexical); keep directories before
	// their content, matching what extraction expects at most loosely — the
	// console extractor creates parent dirs on demand either way.
	sort.SliceStable(items, func(i, j int) bool { return items[i].rel < items[j].rel })

	z := &zipWriter{w: w}
	central := make([]zipCentralEntry, 0, len(items))
	buf := make([]byte, 64*1024)
	var payloadDone int64

	for _, it := range items {
		if z.pos > zipMaxEntrySize {
			return 0, fmt.Errorf("archive exceeds 4 GiB (no zip64)")
		}
		entry := zipCentralEntry{name: it.rel, size: it.size, offset: uint32(z.pos), isDir: it.isDir}
		if it.isDir {
			if err := z.localHeader(it.rel, 0, 0); err != nil {
				return 0, err
			}
			central = append(central, entry)
			continue
		}

		crcFieldOffset := z.pos + 14
		if err := z.localHeader(it.rel, 0, it.size); err != nil {
			return 0, err
		}

		in, err := os.Open(it.abs)
		if err != nil {
			return 0, err
		}
		crc := crc32.NewIEEE()
		written := int64(0)
		for {
			n, rerr := in.Read(buf)
			if n > 0 {
				crc.Write(buf[:n])
				if werr := z.write(buf[:n]); werr != nil {
					in.Close()
					return 0, werr
				}
				written += int64(n)
				payloadDone += int64(n)
				if progress != nil {
					progress(payloadDone)
				}
			}
			if rerr == io.EOF {
				break
			}
			if rerr != nil {
				in.Close()
				return 0, rerr
			}
		}
		in.Close()
		if written != int64(it.size) {
			return 0, fmt.Errorf("file changed while packaging: %s", it.rel)
		}

		// Backfill the real CRC into the local header, then resume appending.
		entry.crc = crc.Sum32()
		end := z.pos
		if _, err := z.w.Seek(crcFieldOffset, io.SeekStart); err != nil {
			return 0, err
		}
		var b [4]byte
		binary.LittleEndian.PutUint32(b[:], entry.crc)
		if _, err := z.w.Write(b[:]); err != nil {
			return 0, err
		}
		if _, err := z.w.Seek(end, io.SeekStart); err != nil {
			return 0, err
		}
		central = append(central, entry)
	}

	centralOffset := z.pos
	for _, e := range central {
		z.le32(zipCentralHeaderSig)
		z.le16(zipVersion) // version made by
		z.le16(zipVersion) // version needed
		z.le16(0)          // flags
		z.le16(0)          // method
		z.le16(0)          // mod time
		z.le16(0)          // mod date
		z.le32(e.crc)
		z.le32(e.size)
		z.le32(e.size)
		z.le16(uint16(len(e.name)))
		z.le16(0) // extra
		z.le16(0) // comment
		z.le16(0) // disk number
		z.le16(0) // internal attrs
		if e.isDir {
			z.le32(0x10) // MS-DOS directory attribute
		} else {
			z.le32(0)
		}
		z.le32(e.offset)
		if err := z.write([]byte(e.name)); err != nil {
			return 0, err
		}
	}

	centralSize := z.pos - centralOffset
	z.le32(zipEOCDSig)
	z.le16(0)
	z.le16(0)
	z.le16(uint16(len(central)))
	z.le16(uint16(len(central)))
	z.le32(uint32(centralSize))
	z.le32(uint32(centralOffset))
	if err := z.le16(0); err != nil {
		return 0, err
	}
	return z.pos, nil
}

// countPayload walks a folder and reports the number of regular files, the
// number of subdirectories, the total file bytes, and the relative path of
// the single file when there is exactly one (used for the raw-send path).
func countPayload(root string) (files int, dirs int, totalBytes int64, singleRel string, err error) {
	err = filepath.WalkDir(root, func(path string, d fs.DirEntry, werr error) error {
		if werr != nil {
			return werr
		}
		if path == root {
			return nil
		}
		if d.IsDir() {
			dirs++
			return nil
		}
		info, ierr := d.Info()
		if ierr != nil {
			return ierr
		}
		files++
		totalBytes += info.Size()
		rel, rerr := filepath.Rel(root, path)
		if rerr != nil {
			return rerr
		}
		singleRel = filepath.ToSlash(rel)
		return nil
	})
	if files != 1 {
		singleRel = ""
	}
	return
}

// isZipFile reports whether path looks like an existing zip archive.
func isZipFile(path string) bool {
	if !strings.EqualFold(filepath.Ext(path), ".zip") {
		return false
	}
	f, err := os.Open(path)
	if err != nil {
		return false
	}
	defer f.Close()
	var sig [4]byte
	if _, err := io.ReadFull(f, sig[:]); err != nil {
		return false
	}
	return binary.LittleEndian.Uint32(sig[:]) == zipLocalHeaderSig ||
		binary.LittleEndian.Uint32(sig[:]) == zipEOCDSig // empty archive
}

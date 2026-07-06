package main

import (
	"archive/zip"
	"bytes"
	"encoding/binary"
	"hash/crc32"
	"os"
	"path/filepath"
	"testing"
)

func buildTree(t *testing.T, files map[string]string) string {
	t.Helper()
	root := t.TempDir()
	for rel, content := range files {
		p := filepath.Join(root, filepath.FromSlash(rel))
		if err := os.MkdirAll(filepath.Dir(p), 0o755); err != nil {
			t.Fatal(err)
		}
		if err := os.WriteFile(p, []byte(content), 0o644); err != nil {
			t.Fatal(err)
		}
	}
	return root
}

func writeZipToFile(t *testing.T, root string) string {
	t.Helper()
	out := filepath.Join(t.TempDir(), "out.zip")
	f, err := os.Create(out)
	if err != nil {
		t.Fatal(err)
	}
	defer f.Close()
	if _, err := WriteStoreZip(f, root, nil); err != nil {
		t.Fatal(err)
	}
	return out
}

// TestStoreZipConsoleConstraints checks the raw bytes against every rule the
// console extractor enforces: store method, flag bit 3 clear, version 20,
// no extra fields, correct CRC, no zip64.
func TestStoreZipConsoleConstraints(t *testing.T) {
	root := buildTree(t, map[string]string{
		"file1.bin":     "hello world",
		"sub/file2.bin": "nested content here",
		"sub/empty.bin": "",
	})
	raw, err := os.ReadFile(writeZipToFile(t, root))
	if err != nil {
		t.Fatal(err)
	}

	le16 := func(off int) uint16 { return binary.LittleEndian.Uint16(raw[off:]) }
	le32 := func(off int) uint32 { return binary.LittleEndian.Uint32(raw[off:]) }

	// Walk local headers sequentially, exactly like the console extractor.
	pos := 0
	entries := 0
	for pos+30 <= len(raw) && le32(pos) == zipLocalHeaderSig {
		version := le16(pos + 4)
		flags := le16(pos + 6)
		method := le16(pos + 8)
		crc := le32(pos + 14)
		compSize := le32(pos + 18)
		uncompSize := le32(pos + 22)
		nameLen := int(le16(pos + 26))
		extraLen := int(le16(pos + 28))

		if version != 20 {
			t.Errorf("entry %d: version %d, want 20", entries, version)
		}
		if flags != 0 {
			t.Errorf("entry %d: flags %#x, want 0 (no data descriptor)", entries, flags)
		}
		if method != 0 {
			t.Errorf("entry %d: method %d, want 0 (store)", entries, method)
		}
		if extraLen != 0 {
			t.Errorf("entry %d: extra field length %d, want 0", entries, extraLen)
		}
		if compSize != uncompSize {
			t.Errorf("entry %d: comp %d != uncomp %d in store mode", entries, compSize, uncompSize)
		}
		name := string(raw[pos+30 : pos+30+nameLen])
		data := raw[pos+30+nameLen : pos+30+nameLen+int(compSize)]
		if got := crc32.ChecksumIEEE(data); compSize > 0 && got != crc {
			t.Errorf("entry %q: CRC %#x in header, computed %#x", name, crc, got)
		}
		if name[len(name)-1] == '/' && uncompSize != 0 {
			t.Errorf("directory entry %q has nonzero size", name)
		}
		pos += 30 + nameLen + int(compSize)
		entries++
	}
	if entries != 4 { // sub/ dir + 3 files
		t.Errorf("walked %d local entries, want 4", entries)
	}

	// Central directory must start right after and match EOCD bookkeeping.
	if le32(pos) != zipCentralHeaderSig {
		t.Fatalf("expected central directory at %#x", pos)
	}
	eocd := len(raw) - 22
	if le32(eocd) != zipEOCDSig {
		t.Fatalf("no EOCD at end of archive")
	}
	if got := int(le16(eocd + 10)); got != entries {
		t.Errorf("EOCD entry count %d, want %d", got, entries)
	}
	if got := int(le32(eocd + 16)); got != pos {
		t.Errorf("EOCD central directory offset %d, want %d", got, pos)
	}
	if bytes.Contains(raw, []byte{0x50, 0x4b, 0x06, 0x06}) {
		t.Error("archive contains a zip64 EOCD record")
	}
}

// TestStoreZipRoundTrip verifies the writer's output through the stdlib
// reader (which also re-verifies CRCs on read).
func TestStoreZipRoundTrip(t *testing.T) {
	files := map[string]string{
		"a.txt":         "alpha",
		"dir/b.txt":     "bravo bravo",
		"dir/sub/c.bin": string(bytes.Repeat([]byte{0xAB, 0x00, 0x7F}, 5000)),
	}
	root := buildTree(t, files)
	archive := writeZipToFile(t, root)

	zr, err := zip.OpenReader(archive)
	if err != nil {
		t.Fatal(err)
	}
	defer zr.Close()

	got := map[string]string{}
	for _, f := range zr.File {
		if f.Method != zip.Store {
			t.Errorf("%s: method %d, want store", f.Name, f.Method)
		}
		if f.Name[len(f.Name)-1] == '/' {
			continue
		}
		rc, err := f.Open()
		if err != nil {
			t.Fatalf("%s: %v", f.Name, err)
		}
		var buf bytes.Buffer
		if _, err := buf.ReadFrom(rc); err != nil {
			t.Fatalf("%s: %v", f.Name, err)
		}
		rc.Close()
		got[f.Name] = buf.String()
	}
	for rel, want := range files {
		if got[rel] != want {
			t.Errorf("%s: content mismatch", rel)
		}
	}
}

func TestStoreZipEmptyFolder(t *testing.T) {
	root := t.TempDir()
	f, err := os.Create(filepath.Join(t.TempDir(), "out.zip"))
	if err != nil {
		t.Fatal(err)
	}
	defer f.Close()
	if _, err := WriteStoreZip(f, root, nil); err == nil {
		t.Error("expected error for empty folder")
	}
}

func TestExtractZipRejectsTraversal(t *testing.T) {
	// Build a malicious zip with the stdlib writer.
	evil := filepath.Join(t.TempDir(), "evil.zip")
	f, err := os.Create(evil)
	if err != nil {
		t.Fatal(err)
	}
	zw := zip.NewWriter(f)
	w, _ := zw.CreateHeader(&zip.FileHeader{Name: "../escape.txt", Method: zip.Store})
	w.Write([]byte("pwned"))
	zw.Close()
	f.Close()

	if err := ExtractZip(evil, t.TempDir(), false); err == nil {
		t.Error("expected traversal rejection")
	}
}

func TestIsSafeZipRelativePath(t *testing.T) {
	cases := []struct {
		path string
		ok   bool
	}{
		{"file.txt", true},
		{"dir/file.txt", true},
		{"dir/", true},
		{"", false},
		{"/abs", false},
		{`\abs`, false},
		{`dir\file`, false},
		{"c:evil", false},
		{"../up", false},
		{"dir/../up", false},
		{"dir/..", false},
		{"..", false},
		{"dir/..hidden", true}, // ".." only as a full component is rejected
	}
	for _, c := range cases {
		if got := isSafeZipRelativePath(c.path); got != c.ok {
			t.Errorf("isSafeZipRelativePath(%q) = %v, want %v", c.path, got, c.ok)
		}
	}
}

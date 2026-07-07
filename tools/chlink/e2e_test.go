package main

import (
	"net/http"
	"net/http/httptest"
	"net/url"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"
)

// startReceiver spins up the CLI's own receive handler on an httptest server
// and returns its host:port plus the output directory.
func startReceiver(t *testing.T, opts receiveOpts) (string, string) {
	t.Helper()
	if opts.outDir == "" {
		opts.outDir = t.TempDir()
	}
	if opts.pin == "" {
		opts.pin = "1234"
	}
	rv := &receiver{opts: opts}
	srv := httptest.NewServer(rv.handler())
	t.Cleanup(srv.Close)
	u, err := url.Parse(srv.URL)
	if err != nil {
		t.Fatal(err)
	}
	return u.Host, opts.outDir
}

func mustReadTree(t *testing.T, root string) map[string]string {
	t.Helper()
	out := map[string]string{}
	err := filepath.Walk(root, func(path string, info os.FileInfo, err error) error {
		if err != nil || info.IsDir() {
			return err
		}
		rel, _ := filepath.Rel(root, path)
		data, err := os.ReadFile(path)
		if err != nil {
			return err
		}
		out[filepath.ToSlash(rel)] = string(data)
		return nil
	})
	if err != nil {
		t.Fatal(err)
	}
	return out
}

func testClient() *http.Client { return newHTTPClient(5 * time.Second) }

// TestSendReceiveZipRoundTrip sends a multi-file folder through the real
// multipart/zip pipeline into the CLI receiver and byte-compares the result.
func TestSendReceiveZipRoundTrip(t *testing.T) {
	files := map[string]string{
		"main.sav":     "primary save data",
		"sub/extra.%1": "nested & weird",
	}
	src := buildTree(t, files)

	target, outDir := startReceiver(t, receiveOpts{})

	// Zip like cmdSend does for a folder with >1 file.
	tmp, err := os.CreateTemp(t.TempDir(), "send_*.zip")
	if err != nil {
		t.Fatal(err)
	}
	if _, err := WriteStoreZip(tmp, src, nil); err != nil {
		t.Fatal(err)
	}
	tmp.Close()
	st, _ := os.Stat(tmp.Name())

	meta := Meta{
		TitleID:        "0004000000055D00",
		TitleName:      "Test Game",
		DataType:       "save",
		BackupName:     "backup one",
		IsZip:          true,
		FileBytesTotal: st.Size(),
		FileName:       "backup.zip",
		Timestamp:      timestamp(),
	}
	savedPath, err := doSend(testClient(), target, "1234", meta, tmp.Name(), nil)
	if err != nil {
		t.Fatal(err)
	}

	wantRoot := filepath.Join(outDir, "saves", "0004000000055D00 Test Game", "backup one")
	if savedPath != wantRoot {
		t.Errorf("savedPath = %q, want %q", savedPath, wantRoot)
	}
	got := mustReadTree(t, wantRoot)
	for rel, want := range files {
		if got[rel] != want {
			t.Errorf("%s: content mismatch (got %d files: %v)", rel, len(got), got)
		}
	}
	// No spool leftovers.
	if leftovers, _ := filepath.Glob(filepath.Join(outDir, "chlink_recv_*")); len(leftovers) != 0 {
		t.Errorf("spool files left behind: %v", leftovers)
	}
}

func TestSendReceiveRawFile(t *testing.T) {
	src := buildTree(t, map[string]string{"save.bin": "raw single file"})
	target, outDir := startReceiver(t, receiveOpts{})

	meta := Meta{
		TitleName:      "Raw Game",
		DataType:       "extdata",
		BackupName:     "rawbk",
		IsZip:          false,
		FileBytesTotal: int64(len("raw single file")),
		FileName:       "save.bin",
		Timestamp:      timestamp(),
	}
	if _, err := doSend(testClient(), target, "1234", meta, filepath.Join(src, "save.bin"), nil); err != nil {
		t.Fatal(err)
	}
	dst := filepath.Join(outDir, "extdata", "Raw Game", "rawbk", "save.bin")
	data, err := os.ReadFile(dst)
	if err != nil {
		t.Fatal(err)
	}
	if string(data) != "raw single file" {
		t.Errorf("content mismatch: %q", data)
	}
}

func TestSendWrongPin(t *testing.T) {
	src := buildTree(t, map[string]string{"save.bin": "x"})
	target, _ := startReceiver(t, receiveOpts{})

	meta := Meta{TitleName: "G", DataType: "save", BackupName: "b", FileName: "save.bin", FileBytesTotal: 1, Timestamp: timestamp()}
	_, err := doSend(testClient(), target, "9999", meta, filepath.Join(src, "save.bin"), nil)
	if err == nil {
		t.Fatal("expected 403 error")
	}
	if got := err.Error(); !strings.Contains(got, "403") {
		t.Errorf("error should surface the 403: %v", got)
	}
}

func TestReceiveReplacesExistingBackup(t *testing.T) {
	target, outDir := startReceiver(t, receiveOpts{flat: true})

	stale := filepath.Join(outDir, "bk", "stale.bin")
	if err := os.MkdirAll(filepath.Dir(stale), 0o755); err != nil {
		t.Fatal(err)
	}
	os.WriteFile(stale, []byte("old"), 0o644)

	src := buildTree(t, map[string]string{"new.bin": "new data"})
	meta := Meta{TitleName: "G", DataType: "save", BackupName: "bk", FileName: "new.bin", FileBytesTotal: 8, Timestamp: timestamp()}
	if _, err := doSend(testClient(), target, "1234", meta, filepath.Join(src, "new.bin"), nil); err != nil {
		t.Fatal(err)
	}
	if _, err := os.Stat(stale); !os.IsNotExist(err) {
		t.Error("stale file survived the same-name replacement")
	}
	if _, err := os.Stat(filepath.Join(outDir, "bk", "new.bin")); err != nil {
		t.Errorf("new backup missing: %v", err)
	}
}

func TestReceiveInfoEndpoint(t *testing.T) {
	target, _ := startReceiver(t, receiveOpts{})
	ri, err := fetchInfo(testClient(), target)
	if err != nil {
		t.Fatal(err)
	}
	if ri.Device != "PC" {
		t.Errorf("device = %q, want PC", ri.Device)
	}
	if ri.MaxUploadBytes != 0 {
		t.Errorf("maxUploadBytes = %d, want 0 (unlimited)", ri.MaxUploadBytes)
	}
}

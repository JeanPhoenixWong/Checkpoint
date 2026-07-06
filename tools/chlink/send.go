package main

import (
	"bufio"
	"crypto/rand"
	"encoding/hex"
	"encoding/json"
	"flag"
	"fmt"
	"io"
	"net"
	"net/http"
	"os"
	"os/signal"
	"path/filepath"
	"regexp"
	"strings"
	"time"
)

const assumedCap = 32 * 1024 * 1024 // console MAX_REQUEST_SIZE when info reports 0

var fullTitleID = regexp.MustCompile(`^[0-9A-Fa-f]{16}$`)

func cmdSend(args []string) error {
	fs := flag.NewFlagSet("send", flag.ExitOnError)
	c := addCommon(fs)
	to := fs.String("to", "", "receiver address ip[:port]")
	pin := fs.String("pin", "", "receiver PIN (4 digits; prompted when omitted)")
	titleID := fs.String("title-id", "", "title id (16 hex)")
	titleName := fs.String("title-name", "", "title name")
	dataType := fs.String("type", "", "data type: save|extdata (default save)")
	backupName := fs.String("backup-name", "", "destination backup folder name")
	yes := fs.Bool("yes", false, "skip the confirmation prompt")
	force := fs.Bool("force", false, "send even when over the receiver's size cap")
	fs.Usage = func() {
		fmt.Fprintln(os.Stderr, "usage: chlink send <path> --to <ip[:port]> --pin <PIN> [flags]")
		fs.PrintDefaults()
	}
	path, err := parseOnePositional(fs, args, "<path>")
	if err != nil {
		return err
	}

	target, err := resolveTarget(*to, c.port)
	if err != nil {
		return err
	}

	info, err := os.Stat(path)
	if err != nil {
		return err
	}

	// Resolve payload: folder → zip (or raw when it holds exactly one file
	// and no subfolders, mirroring the console), file → raw, .zip → as-is.
	meta := Meta{DataType: "save", Timestamp: timestamp()}
	if inf, ok := inferFromPath(path, info.IsDir()); ok {
		meta.TitleID = inf.TitleID
		meta.TitleName = inf.TitleName
		meta.DataType = inf.DataType
		meta.BackupName = inf.BackupName
		if c.verbose {
			fmt.Fprintf(os.Stderr, "inferred metadata from Checkpoint SD layout under %s\n", path)
		}
	}

	var payloadPath string // what gets streamed
	var payloadName string // multipart filename / meta fileName
	var tempZip string     // non-empty when we created it and must delete it

	cleanup := func() {
		if tempZip != "" {
			os.Remove(tempZip)
		}
	}
	defer cleanup()
	sigc := make(chan os.Signal, 1)
	signal.Notify(sigc, os.Interrupt)
	go func() {
		<-sigc
		cleanup()
		os.Exit(1)
	}()

	switch {
	case !info.IsDir() && isZipFile(path):
		meta.IsZip = true
		payloadPath = path
		payloadName = filepath.Base(path)
		if meta.BackupName == "" {
			meta.BackupName = strings.TrimSuffix(filepath.Base(path), filepath.Ext(path))
		}
	case !info.IsDir():
		meta.IsZip = false
		payloadPath = path
		payloadName = filepath.Base(path)
		if meta.BackupName == "" {
			meta.BackupName = "Received_" + fsTimestamp()
		}
	default:
		nFiles, nDirs, totalBytes, singleRel, err := countPayload(path)
		if err != nil {
			return err
		}
		if nFiles == 0 && nDirs == 0 {
			return fmt.Errorf("no files or folders found to package in %s", path)
		}
		if meta.BackupName == "" {
			meta.BackupName = filepath.Base(filepath.Clean(path))
		}
		if nFiles == 1 && nDirs == 0 {
			meta.IsZip = false
			payloadPath = filepath.Join(path, filepath.FromSlash(singleRel))
			payloadName = singleRel
		} else {
			meta.IsZip = true
			payloadName = "backup.zip"
			tmp, err := os.CreateTemp("", "chlink_send_*.zip")
			if err != nil {
				return err
			}
			tempZip = tmp.Name()
			if !c.jsonOut {
				fmt.Fprintf(os.Stderr, "packaging %s (%d files, %s)...\n", path, nFiles, humanBytes(totalBytes))
			}
			var progress func(int64)
			if !c.jsonOut {
				progress = progressPrinter("packaging", totalBytes)
			}
			if _, err := WriteStoreZip(tmp, path, progress); err != nil {
				tmp.Close()
				return fmt.Errorf("packaging failed: %w", err)
			}
			if err := tmp.Close(); err != nil {
				return err
			}
			if !c.jsonOut {
				fmt.Fprintln(os.Stderr)
			}
			payloadPath = tempZip
		}
	}

	// Explicit flags always override inference.
	if *titleID != "" {
		if !fullTitleID.MatchString(*titleID) {
			return fmt.Errorf("--title-id must be 16 hex characters")
		}
		meta.TitleID = strings.ToUpper(*titleID)
	}
	if *titleName != "" {
		meta.TitleName = *titleName
	}
	if *dataType != "" {
		if *dataType != "save" && *dataType != "extdata" {
			return fmt.Errorf("--type must be save or extdata")
		}
		meta.DataType = *dataType
	}
	if *backupName != "" {
		meta.BackupName = *backupName
	}
	if meta.TitleName == "" {
		meta.TitleName = "Unknown"
	}

	st, err := os.Stat(payloadPath)
	if err != nil {
		return err
	}
	meta.FileBytesTotal = st.Size()
	meta.FileName = payloadName

	// Query the receiver's cap before shipping bytes at it.
	client := newHTTPClient(c.timeout)
	if ri, err := fetchInfo(client, target); err != nil {
		fmt.Fprintf(os.Stderr, "warning: GET /transfer/info failed (%v), sending blind\n", err)
	} else {
		cap := ri.MaxUploadBytes
		capNote := ""
		if cap == 0 {
			cap = assumedCap
			capNote = " (reported 0: assuming the console's 32 MiB limit)"
		}
		if c.verbose {
			fmt.Fprintf(os.Stderr, "receiver: %s %s, upload cap %s%s\n", ri.Device, ri.Version, humanBytes(cap), capNote)
		}
		if meta.FileBytesTotal > cap {
			if !*force {
				return fmt.Errorf("payload is %s but the receiver accepts at most %s%s; use --force to try anyway",
					humanBytes(meta.FileBytesTotal), humanBytes(cap), capNote)
			}
			fmt.Fprintf(os.Stderr, "warning: payload %s exceeds the receiver cap %s%s, sending anyway (--force)\n",
				humanBytes(meta.FileBytesTotal), humanBytes(cap), capNote)
		}
	}

	if !c.jsonOut {
		fmt.Printf("target:      %s\n", target)
		fmt.Printf("titleId:     %s\n", orDash(meta.TitleID))
		fmt.Printf("titleName:   %s\n", meta.TitleName)
		fmt.Printf("dataType:    %s\n", meta.DataType)
		fmt.Printf("backupName:  %s\n", meta.BackupName)
		fmt.Printf("payload:     %s (%s, isZip=%v)\n", payloadName, humanBytes(meta.FileBytesTotal), meta.IsZip)
	}
	if !*yes && !c.jsonOut {
		fmt.Print("proceed? [y/N] ")
		line, _ := bufio.NewReader(os.Stdin).ReadString('\n')
		line = strings.ToLower(strings.TrimSpace(line))
		if line != "y" && line != "yes" {
			return fmt.Errorf("aborted")
		}
	}

	pinStr := *pin
	if pinStr == "" {
		if c.jsonOut {
			return fmt.Errorf("--pin is required with --json")
		}
		fmt.Print("PIN (shown on the receiver's screen): ")
		line, _ := bufio.NewReader(os.Stdin).ReadString('\n')
		pinStr = strings.TrimSpace(line)
	}
	if !validPin(pinStr) {
		return fmt.Errorf("PIN must be exactly 4 digits")
	}

	var progress func(int64)
	if !c.jsonOut {
		progress = progressPrinter("sending", meta.FileBytesTotal)
	}
	savedPath, err := doSend(client, target, pinStr, meta, payloadPath, progress)
	if !c.jsonOut && progress != nil {
		fmt.Fprintln(os.Stderr)
	}
	if err != nil {
		if c.jsonOut {
			json.NewEncoder(os.Stdout).Encode(map[string]any{"ok": false, "error": err.Error()})
		}
		return err
	}
	if c.jsonOut {
		json.NewEncoder(os.Stdout).Encode(map[string]any{"ok": true, "savedPath": savedPath, "meta": meta})
	} else {
		fmt.Printf("done. receiver stored the backup at: %s\n", savedPath)
	}
	return nil
}

// doSend performs the POST /transfer/upload request, hand-rolling the
// multipart body exactly like the console sender so Content-Length is known
// upfront (the console server requires it and cannot parse chunked bodies).
func doSend(client *http.Client, target, pin string, meta Meta, payloadPath string, progress func(int64)) (string, error) {
	metaJSON, err := json.Marshal(meta)
	if err != nil {
		return "", err
	}

	var rnd [8]byte
	rand.Read(rnd[:])
	boundary := "----chlink-boundary-" + hex.EncodeToString(rnd[:])

	fileName := meta.FileName
	if i := strings.LastIndexByte(fileName, '/'); i >= 0 {
		fileName = fileName[i+1:]
	}
	if fileName == "" {
		fileName = "backup.bin"
	}

	partMeta := "--" + boundary + "\r\n" +
		"Content-Disposition: form-data; name=\"meta\"\r\n" +
		"Content-Type: application/json\r\n\r\n" +
		string(metaJSON) + "\r\n"
	partFileHeader := "--" + boundary + "\r\n" +
		"Content-Disposition: form-data; name=\"file\"; filename=\"" + fileName + "\"\r\n" +
		"Content-Type: application/octet-stream\r\n\r\n"
	partEnd := "\r\n--" + boundary + "--\r\n"

	f, err := os.Open(payloadPath)
	if err != nil {
		return "", err
	}
	defer f.Close()
	st, err := f.Stat()
	if err != nil {
		return "", err
	}

	var fileReader io.Reader = f
	if progress != nil {
		fileReader = &progressReader{r: f, fn: progress}
	}
	body := io.MultiReader(strings.NewReader(partMeta), strings.NewReader(partFileHeader), fileReader, strings.NewReader(partEnd))

	req, err := http.NewRequest(http.MethodPost, "http://"+target+"/transfer/upload", body)
	if err != nil {
		return "", err
	}
	req.ContentLength = int64(len(partMeta)+len(partFileHeader)+len(partEnd)) + st.Size()
	req.Header.Set("X-CP-Token", pin)
	req.Header.Set("Content-Type", "multipart/form-data; boundary="+boundary)
	req.Close = true

	resp, err := client.Do(req)
	if err != nil {
		return "", fmt.Errorf("upload failed (the receiver may have cancelled or closed): %w", err)
	}
	defer resp.Body.Close()
	raw, _ := io.ReadAll(io.LimitReader(resp.Body, 64*1024))

	var ur UploadResponse
	json.Unmarshal(raw, &ur)
	if resp.StatusCode != http.StatusOK || !ur.OK {
		detail := ur.Error
		if detail == "" {
			detail = strings.TrimSpace(string(raw))
		}
		if detail == "" {
			detail = resp.Status
		}
		if resp.StatusCode == http.StatusForbidden {
			return "", fmt.Errorf("receiver rejected the PIN (403): %s", detail)
		}
		return "", fmt.Errorf("receiver returned %d: %s", resp.StatusCode, detail)
	}
	return ur.SavedPath, nil
}

func fetchInfo(client *http.Client, target string) (InfoResponse, error) {
	var ri InfoResponse
	resp, err := client.Get("http://" + target + "/transfer/info")
	if err != nil {
		return ri, err
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusOK {
		return ri, fmt.Errorf("HTTP %s", resp.Status)
	}
	if err := json.NewDecoder(io.LimitReader(resp.Body, 64*1024)).Decode(&ri); err != nil {
		return ri, err
	}
	return ri, nil
}

// newHTTPClient bounds connect and response-header time but not the body
// transfer itself, so large uploads aren't killed by a global deadline.
func newHTTPClient(timeout time.Duration) *http.Client {
	return &http.Client{
		Transport: &http.Transport{
			DialContext:           (&net.Dialer{Timeout: timeout}).DialContext,
			ResponseHeaderTimeout: timeout,
			DisableCompression:    true,
		},
	}
}

type progressReader struct {
	r    io.Reader
	done int64
	fn   func(int64)
}

func (p *progressReader) Read(b []byte) (int, error) {
	n, err := p.r.Read(b)
	p.done += int64(n)
	p.fn(p.done)
	return n, err
}

// progressPrinter returns a rate-limited \r progress line writer for stderr.
func progressPrinter(verb string, total int64) func(int64) {
	last := time.Time{}
	return func(done int64) {
		now := time.Now()
		if now.Sub(last) < 100*time.Millisecond && done < total {
			return
		}
		last = now
		if total > 0 {
			fmt.Fprintf(os.Stderr, "\r%s %s / %s (%d%%)", verb, humanBytes(done), humanBytes(total), done*100/total)
		} else {
			fmt.Fprintf(os.Stderr, "\r%s %s", verb, humanBytes(done))
		}
	}
}

func humanBytes(n int64) string {
	const unit = 1024
	if n < unit {
		return fmt.Sprintf("%d B", n)
	}
	div, exp := int64(unit), 0
	for m := n / unit; m >= unit; m /= unit {
		div *= unit
		exp++
	}
	return fmt.Sprintf("%.1f %ciB", float64(n)/float64(div), "KMGT"[exp])
}

func orDash(s string) string {
	if s == "" {
		return "-"
	}
	return s
}

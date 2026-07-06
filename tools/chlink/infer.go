package main

import (
	"path/filepath"
	"regexp"
	"strings"
)

// InferredMeta is the metadata deduced from a path inside a Checkpoint SD
// layout: .../Checkpoint/{saves,extdata}/<title folder>/<backup folder>[/file].
type InferredMeta struct {
	TitleID    string // full 16-hex id, "" when only a 3DS unique-ID prefix is present
	TitleName  string
	DataType   string // "save" | "extdata"
	BackupName string
}

var (
	// Switch title folders: "0x%016llX <name>" (full 16-hex title id with a 0x
	// prefix — switch/source/titleprobe.cpp). Checked before prefix3DS so the
	// longer id wins.
	prefixFullID = regexp.MustCompile(`^0x([0-9A-Fa-f]{16}) `)
	// 3DS backup folders: "0x%05X <shortDescription>" (unique-ID prefix,
	// not the full title id — titleprobe.cpp:64). 5–8 hex, never 16.
	prefix3DS = regexp.MustCompile(`^0x[0-9A-Fa-f]{5,8} `)
)

// inferFromPath inspects path (pathIsDir tells whether it names the backup
// folder itself or a file inside one) and infers transfer metadata when it
// sits inside a Checkpoint SD layout. Returns ok=false when the layout does
// not match.
func inferFromPath(path string, pathIsDir bool) (InferredMeta, bool) {
	clean := filepath.ToSlash(filepath.Clean(path))
	parts := strings.Split(clean, "/")

	// The backup folder is the last component for a directory, the parent
	// for a file.
	backupIdx := len(parts) - 1
	if !pathIsDir {
		backupIdx--
	}
	titleIdx := backupIdx - 1
	typeIdx := titleIdx - 1
	rootIdx := typeIdx - 1
	if rootIdx < 0 {
		return InferredMeta{}, false
	}
	if !strings.EqualFold(parts[rootIdx], "Checkpoint") {
		return InferredMeta{}, false
	}
	var dataType string
	switch strings.ToLower(parts[typeIdx]) {
	case "saves":
		dataType = "save"
	case "extdata":
		dataType = "extdata"
	default:
		return InferredMeta{}, false
	}

	title := parts[titleIdx]
	out := InferredMeta{DataType: dataType, BackupName: parts[backupIdx]}
	if m := prefixFullID.FindStringSubmatch(title); m != nil {
		out.TitleID = strings.ToUpper(m[1])
		out.TitleName = title[len(m[0]):]
	} else if m := prefix3DS.FindString(title); m != "" {
		out.TitleName = title[len(m):]
	} else {
		out.TitleName = title
	}
	if out.TitleName == "" || out.BackupName == "" {
		return InferredMeta{}, false
	}
	return out, true
}

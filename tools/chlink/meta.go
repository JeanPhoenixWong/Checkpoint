package main

// Meta mirrors the "meta" multipart part produced by the console sender
// (see sendBackup() in 3ds/source/transfer.cpp).
type Meta struct {
	TitleID        string `json:"titleId"`   // 16 uppercase hex; "" if unknown
	TitleName      string `json:"titleName"` // short description
	DataType       string `json:"dataType"`  // "save" | "extdata"
	BackupName     string `json:"backupName"`
	IsZip          bool   `json:"isZip"`
	FileBytesTotal int64  `json:"fileBytesTotal"`
	FileName       string `json:"fileName"`
	Timestamp      string `json:"timestamp"`
}

// InfoResponse mirrors GET /transfer/info.
type InfoResponse struct {
	Device         string `json:"device"`
	Version        string `json:"version"`
	MaxUploadBytes int64  `json:"maxUploadBytes"`
	FreeSpaceBytes int64  `json:"freeSpaceBytes"`
}

// UploadResponse mirrors the POST /transfer/upload response body.
type UploadResponse struct {
	OK        bool   `json:"ok"`
	SavedPath string `json:"savedPath,omitempty"`
	Error     string `json:"error,omitempty"`
}

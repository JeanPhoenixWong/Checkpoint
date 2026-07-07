package main

import "testing"

func TestInferFromPath(t *testing.T) {
	cases := []struct {
		name  string
		path  string
		isDir bool
		ok    bool
		want  InferredMeta
	}{
		{
			name: "3ds save backup folder", isDir: true, ok: true,
			path: "/mnt/sd/3ds/Checkpoint/saves/0x0055D Super Game/2026-07-06 backup",
			want: InferredMeta{TitleID: "", TitleName: "Super Game", DataType: "save", BackupName: "2026-07-06 backup"},
		},
		{
			name: "3ds extdata backup folder", isDir: true, ok: true,
			path: "sd/3ds/Checkpoint/extdata/0x0055D Super Game/backup1",
			want: InferredMeta{TitleID: "", TitleName: "Super Game", DataType: "extdata", BackupName: "backup1"},
		},
		{
			name: "switch layout full title id", isDir: true, ok: true,
			path: "/sd/switch/Checkpoint/saves/0x0100000000010000 Odyssey/my backup",
			want: InferredMeta{TitleID: "0100000000010000", TitleName: "Odyssey", DataType: "save", BackupName: "my backup"},
		},
		{
			name: "file inside a backup folder", isDir: false, ok: true,
			path: "/sd/3ds/Checkpoint/saves/0x0055D Super Game/backup1/save.bin",
			want: InferredMeta{TitleID: "", TitleName: "Super Game", DataType: "save", BackupName: "backup1"},
		},
		{
			name: "no prefix on title folder", isDir: true, ok: true,
			path: "/sd/3ds/Checkpoint/saves/Plain Title/backup1",
			want: InferredMeta{TitleID: "", TitleName: "Plain Title", DataType: "save", BackupName: "backup1"},
		},
		{name: "not a checkpoint layout", path: "/home/user/backups/save1", isDir: true, ok: false},
		{name: "wrong branch name", path: "/sd/3ds/Checkpoint/cheats/0x0055D Game/backup1", isDir: true, ok: false},
		{name: "too shallow", path: "Checkpoint/saves/backup1", isDir: true, ok: false},
	}
	for _, c := range cases {
		t.Run(c.name, func(t *testing.T) {
			got, ok := inferFromPath(c.path, c.isDir)
			if ok != c.ok {
				t.Fatalf("ok = %v, want %v", ok, c.ok)
			}
			if ok && got != c.want {
				t.Errorf("got %+v, want %+v", got, c.want)
			}
		})
	}
}

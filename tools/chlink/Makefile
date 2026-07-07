BIN     := chlink
VERSION := $(shell git describe --tags --always --dirty 2>/dev/null || echo dev)
LDFLAGS := -s -w -X main.version=$(VERSION)
GOFLAGS := -trimpath -ldflags "$(LDFLAGS)"

PLATFORMS := linux/amd64 linux/arm64 darwin/amd64 darwin/arm64 windows/amd64

.PHONY: all build test vet fmt release clean

all: build

build:
	CGO_ENABLED=0 go build $(GOFLAGS) -o $(BIN) .

test:
	go test ./...

vet:
	go vet ./...

fmt:
	gofmt -w .

release: test vet
	@mkdir -p dist
	@for p in $(PLATFORMS); do \
		os=$${p%/*}; arch=$${p#*/}; \
		out=dist/$(BIN)-$$os-$$arch; \
		[ "$$os" = "windows" ] && out=$$out.exe; \
		echo "  $$out"; \
		CGO_ENABLED=0 GOOS=$$os GOARCH=$$arch go build $(GOFLAGS) -o $$out . || exit 1; \
	done

clean:
	rm -rf $(BIN) dist

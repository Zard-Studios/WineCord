PREFIX ?= /usr/local
BUILD_DIR ?= build
DIST_DIR ?= dist
VERSION ?= 0.1.4

CC ?= clang
CFLAGS ?= -std=c11 -Wall -Wextra -Wpedantic -O2
LDFLAGS ?=

WINDOWS_CC ?= x86_64-w64-mingw32-gcc
WINDOWS_CFLAGS ?= -std=c11 -Wall -Wextra -O2
WINDOWS_LDFLAGS ?= -lws2_32 -ladvapi32

NATIVE := $(BUILD_DIR)/winecord
UNIVERSAL := $(BUILD_DIR)/winecord-universal
WINDOWS_HELPER := $(BUILD_DIR)/winecord-bridge.exe
WINDOWS_SMOKE := $(BUILD_DIR)/pipe-smoke.exe
RELEASE_TARBALL := $(DIST_DIR)/winecord-$(VERSION)-macos-universal.tar.gz

.PHONY: all clean check install windows-helper windows-smoke universal package release

all: $(NATIVE)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(NATIVE): src/winecord.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) $< -o $@ $(LDFLAGS)

$(BUILD_DIR)/arm64/winecord: src/winecord.c | $(BUILD_DIR)
	mkdir -p $(BUILD_DIR)/arm64
	$(CC) $(CFLAGS) -arch arm64 $< -o $@ $(LDFLAGS)

$(BUILD_DIR)/x86_64/winecord: src/winecord.c | $(BUILD_DIR)
	mkdir -p $(BUILD_DIR)/x86_64
	$(CC) $(CFLAGS) -arch x86_64 $< -o $@ $(LDFLAGS)

universal: $(UNIVERSAL)

$(UNIVERSAL): $(BUILD_DIR)/arm64/winecord $(BUILD_DIR)/x86_64/winecord
	lipo -create -output $@ $^

windows-helper: $(WINDOWS_HELPER)

$(WINDOWS_HELPER): src/winecord-bridge.c | $(BUILD_DIR)
	@command -v $(WINDOWS_CC) >/dev/null || { \
		echo "$(WINDOWS_CC) not found. Install mingw-w64 to build the Wine helper."; \
		exit 1; \
	}
	$(WINDOWS_CC) $(WINDOWS_CFLAGS) $< -o $@ $(WINDOWS_LDFLAGS)

windows-smoke: $(WINDOWS_SMOKE)

$(WINDOWS_SMOKE): tools/pipe-smoke.c | $(BUILD_DIR)
	@command -v $(WINDOWS_CC) >/dev/null || { \
		echo "$(WINDOWS_CC) not found. Install mingw-w64 to build the smoke test."; \
		exit 1; \
	}
	$(WINDOWS_CC) $(WINDOWS_CFLAGS) $< -o $@

check: $(NATIVE)
	$(NATIVE) --version
	$(NATIVE) doctor

install: $(NATIVE)
	install -d "$(DESTDIR)$(PREFIX)/bin"
	install -d "$(DESTDIR)$(PREFIX)/libexec/winecord"
	install -m 0755 $(NATIVE) "$(DESTDIR)$(PREFIX)/bin/winecord"
	@if [ -f "$(WINDOWS_HELPER)" ]; then \
		install -m 0755 "$(WINDOWS_HELPER)" "$(DESTDIR)$(PREFIX)/libexec/winecord/winecord-bridge.exe"; \
	fi

package: $(RELEASE_TARBALL)

release: package

$(RELEASE_TARBALL): $(UNIVERSAL) $(WINDOWS_HELPER) README.md LICENSE NOTICE | $(BUILD_DIR)
	rm -rf "$(BUILD_DIR)/package-root"
	mkdir -p "$(BUILD_DIR)/package-root/bin" "$(BUILD_DIR)/package-root/libexec/winecord"
	mkdir -p "$(DIST_DIR)"
	install -m 0755 "$(UNIVERSAL)" "$(BUILD_DIR)/package-root/bin/winecord"
	install -m 0755 "$(WINDOWS_HELPER)" "$(BUILD_DIR)/package-root/libexec/winecord/winecord-bridge.exe"
	install -m 0644 README.md LICENSE NOTICE "$(BUILD_DIR)/package-root/"
	tar -C "$(BUILD_DIR)/package-root" -czf "$@" .
	shasum -a 256 "$@"

clean:
	rm -rf $(BUILD_DIR)

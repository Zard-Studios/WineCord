PREFIX ?= /usr/local
BUILD_DIR ?= build

CC ?= clang
CFLAGS ?= -std=c11 -Wall -Wextra -Wpedantic -O2
LDFLAGS ?=

WINDOWS_CC ?= x86_64-w64-mingw32-gcc
WINDOWS_CFLAGS ?= -std=c11 -Wall -Wextra -O2
WINDOWS_LDFLAGS ?= -lws2_32 -ladvapi32

NATIVE := $(BUILD_DIR)/winecord
WINDOWS_HELPER := $(BUILD_DIR)/winecord-bridge.exe
WINDOWS_SMOKE := $(BUILD_DIR)/pipe-smoke.exe

.PHONY: all clean check install windows-helper windows-smoke

all: $(NATIVE)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(NATIVE): src/winecord.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) $< -o $@ $(LDFLAGS)

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
	install -m 0755 $(NATIVE) "$(DESTDIR)$(PREFIX)/bin/winecord"

clean:
	rm -rf $(BUILD_DIR)

CC ?= clang
VERSION := $(shell sed -n 's/^VERSION *:= *//p' SameBoy/version.mk 2>/dev/null || echo 1.0.3)
BUILD := build
OBJ := $(BUILD)/obj

CORE_SRC := $(wildcard SameBoy/Core/*.c)
CLI_SRC := $(wildcard cli/src/*.c)

SRC := $(CORE_SRC) $(CLI_SRC)
OBJS := $(patsubst %.c,$(OBJ)/%.o,$(SRC))

CFLAGS := -std=gnu11 -O2 -MMD -MP -D_GNU_SOURCE -DGB_INTERNAL -DGB_VERSION='"$(VERSION)"' -DGB_COPYRIGHT_YEAR='"2025"' \
          -ISameBoy -ISameBoy/Core -Wall -Wno-unused-function -Wno-unused-variable -Wno-multichar
LDFLAGS := -lm -lz

BIN := $(BUILD)/gbemu
DIST_NAME := gbemu-cli-$(VERSION)

all: $(BIN)

$(BIN): $(OBJS)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(OBJ)/%.o: %.c Makefile
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -c -o $@ $<

bootroms:
	./tools/fetch_bootroms.sh

test: $(BIN)
	python3 cli/tests/make_test_rom.py $(BUILD)/test_rom.gb $(BUILD)/test_rom.json --sym $(BUILD)/test_rom.sym
	python3 cli/tests/smoke.py $(BIN) $(BUILD)/test_rom.gb $(BUILD)/test_rom.json
	python3 cli/tests/sanitize_test.py $(BIN)

clean:
	rm -rf $(BUILD)

# Source distribution tarball (written next to the repo).
# Excludes build artifacts, VCS state and OS cruft. Boot ROMs are included if present;
# recipients can provision them with `make bootroms` otherwise.
dist:
	@rm -f ../$(DIST_NAME).tar.gz
	tar --exclude=build --exclude=.git --exclude=.gitignore --exclude=.DS_Store \
	    --exclude='__pycache__' --exclude='*.pyc' --exclude='*.o' --exclude='*.d' \
	    -czf ../$(DIST_NAME).tar.gz -C .. $(notdir $(CURDIR))
	@echo "Created ../$(DIST_NAME).tar.gz"
	@tar -tzf ../$(DIST_NAME).tar.gz | grep -cE 'bootroms/.*\.bin$$' | xargs -I{} sh -c \
	  '[ "{}" -gt 0 ] && echo "  includes {} boot ROM binaries" || echo "  WARNING: no boot ROMs included (run make bootroms first)"'

-include $(OBJS:.o=.d)

.PHONY: all bootroms test dist clean
